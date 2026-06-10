# cljc — single-file Clojure in C

Goal: a babashka-coverage-ish Clojure interpreter in one embeddable C file
(`cljc.c`), inspired by Janet. Build: `make`. Tests: `make test` (runs the
suite twice — normal and `CLJC_GC_STRESS=1`).

## Status (as of 2026-06-10)

~3,250 lines, zero warnings, 262 assertions in `tests.clj`, ASan/UBSan clean
(ASan needs `ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0` —
required by conservative stack scanning, same as Boehm GC).

### Done
- **Reader**: lists/vectors/maps/strings (with escapes), keywords, comments,
  commas-as-whitespace, `'` `` ` `` `~` `~@`
- **Eval**: special forms `quote if do def defn defmacro let fn loop recur
  and or when cond quasiquote`; macro expansion (fn with `is_macro` flag,
  unevaluated args, expansion re-evaluated)
- **Calling**: variadic `& rest`, `apply` with splice, stack-safe `recur` in
  both `loop` and `fn` (CLJC_RECUR sentinel caught by the enclosing form),
  keywords/maps/vectors callable
- **Data**: int64/double arithmetic with Clojure unary semantics, value
  equality incl. cross-type sequential `(= [1 2] '(1 2))`, HAMT persistent
  maps + 32-way trie persistent vectors (see roadmap item 5 for details)
- **Prelude** (cljc source in `PRELUDE` string): `identity some? not= even?
  odd? abs max min complement constantly comp partial into mapv filterv
  repeat -> ->> if-not when-not`
- **GC**: mark-and-sweep over block pools (cells + envs), conservative
  C-stack + register scanning, adaptive threshold, `(gc)` native,
  `CLJC_GC_STRESS` env var. Key invariants documented below.
- **Errors as values**: `try`/`catch`/`finally` via a stack of handler frames
  (ErrFrame) threaded through the C stack; `throw` any value; `ex-info`
  exceptions are plain maps `{:message m :data d}` (`ex-message`/`ex-data`);
  interpreter errors caught as message strings; `cur_exc` is a GC root.
  finally runs on normal exit, body throw, and handler throw. Divergence:
  catch is untyped — `(catch Exception e ...)` accepts and ignores the class.
- **Atoms**: `atom deref reset! swap!` + `@` reader sugar; printed `#atom[v]`.
- **Modes**: interactive REPL (multi-line, paren-balance), `./cljc file.clj`,
  piped stdin (babashka-style: no result echo). Embed API: `cljc_new_env`,
  `cljc_eval_string`, `cljc_define_native`, `cljc_set_stack_base`,
  `-DCLJC_NO_MAIN`.

### GC invariants (read before touching allocation paths!)
1. Only `cell_alloc`/`env_alloc` can trigger collection (`xmalloc` cannot).
2. A cell's union is zeroed at allocation — half-built cells are always safe
   to mark/sweep.
3. Collections under construction must grow `len`/`n` incrementally when
   allocation can happen between slot fills (see eval VECTOR/MAP, qq_expand,
   SYM_RECUR). Values held only in C locals are safe (conservative scan).
4. Swept cells get tag `CLJC_FREE`; mark refuses to traverse them.
5. `gc_scan_range` is exempt from ASan instrumentation by design.

## Roadmap (agreed order)

1. ~~try/catch + atoms~~ ✅ done 2026-06-10
2. ~~Destructuring + multi-arity fn~~ ✅ done 2026-06-10 — fns hold an
   arities list dispatched by argc; destructure() is a recursive C binder
   shared by let/fn-params/& rest (vector patterns with & and :as, map
   patterns with {sym :key}, :keys, :or, :as). loop stays symbols-only.
3. ~~Babashka surface batch 1~~ ✅ done 2026-06-10 — natives: compare sort
   vec name keyword symbol quot subs slurp spit pr prn print +
   str/{upper-case lower-case trim split join starts-with? ends-with?
   includes? blank?} (split/join: plain-string separators, NOT regex).
   Prelude: next mapcat remove keep butlast every? some take-while
   drop-while interleave interpose partition distinct group-by frequencies
   juxt sort-by update get-in assoc-in update-in select-keys if-let
   when-let dotimes doseq while case for. case quotes its test constants;
   for supports multiple bindings, no :when/:let yet.
4. ~~Batch 5-lite~~ ✅ done 2026-06-10 — prelude: boolean true? false?
   map-indexed keep-indexed partition-all zipmap merge-with reduce-kv
   repeatedly doto letfn (works via late binding!) assert; natives int?
   double?. Also fixed: ~@ splicing inside vector templates ([~@(...)]).
5. ~~HAMT persistent maps + persistent vectors~~ ✅ done 2026-06-10 — bit-partitioned trie
   (5-bit chunks, bitmap+popcount nodes, collision nodes, path-copying).
   Nodes are ordinary GC cells (CLJC_HNODE) with kids interleaved
   [k1,v1,...], k==NULL → subnode. cljc_hash agrees with cljc_eq
   ((= 1 1.0), list/vector seq equality, order-independent map hash).
   Reader errors on duplicate literal keys; map iteration order is hash
   order (divergence: literal eval order + print order not source order).
   100k assoc+lookup ≈ 0.35s. Vectors: Clojure-style 32-way position
   tries + owned tail of ≤32 elems (amortized O(1) conj — 100k conjs in
   ~33ms), path-copying assoc, trie nodes reuse CLJC_HNODE; all access
   behind vec_len/vec_nth/vec_conj1/vec_assoc_idx. STILL TODO: sets
   (#{...} over the HAMT) — start the next session with these.
6. **More surface**: sets (#{...}) — fold into the HAMT milestone; regex
   (`re-find`/`re-matches`; needs a decision: POSIX ERE divergence vs tiny
   regex engine); `condp`, `for` :when/:let modifiers, `..`,
   `iterate` (eager n-limited?), `format`?
7. **Performance later, maybe**: NaN-boxing, symbol→binding caching,
   bytecode VM. Not before semantics are broader.

## Known divergences from Clojure (deliberate, v0)
- `()` ≡ `nil` (so `(= () [])` is false)
- No Ratio type: `(/ 7 2)` ⇒ `3.5`
- No namespaces, no vars (def installs directly into root env)
- No lazy seqs (everything eager; `range` is bounded-only)
- Quasiquote: no nesting levels, no auto-gensym `x#`, no ns-resolution
- Conservative GC may retain garbage pointed at by stale stack slots

## Testing conventions
- `tests.clj` — every bug fix gets a regression assertion (assert= prints
  PASS/FAIL; script mode aborts on first `error:`)
- Verify sweeps: `make test`, then ASan build:
  `cc -g -fsanitize=address,undefined -o /tmp/cljc-asan cljc.c` with the
  ASAN_OPTIONS above, both normal and stress.
- Memory bound check: 2M-iteration churn loop should stay ~5 MB RSS.
