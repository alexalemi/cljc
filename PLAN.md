# cljc — single-file Clojure in C

Goal: a babashka-coverage-ish Clojure interpreter in one embeddable C file
(`cljc.c`), inspired by Janet. Build: `make`. Tests: `make test` (runs the
suite twice — normal and `CLJC_GC_STRESS=1`).

## Status (as of 2026-06-10)

~2,100 lines, zero warnings, 95+ assertions in `tests.clj`, ASan/UBSan clean
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
  equality incl. cross-type sequential `(= [1 2] '(1 2))`, copy-on-write
  assoc-array maps (O(n), interface-compatible with a future HAMT)
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
2. **Destructuring + multi-arity fn** — `(fn ([x] ...) ([x y] ...))`,
   `(let [[a b] pair, {:keys [x]} m] ...)`. Destructuring can be a macro-time
   rewrite once `let*` exists.
3. **HAMT persistent collections** — replace assoc-array maps and
   copying vectors; the public interface (get/assoc/conj/...) is already
   stable so this is engine-swap only. Also a real ISeq so `to_seq` stops
   copying.
4. **More babashka surface**: `sort sort-by group-by frequencies juxt
   interleave interpose partition every? some take-while drop-while
   update update-in assoc-in get-in`, `clojure.string` equivalents,
   `re-find`/`re-matches` (POSIX regex or tiny regex engine), `slurp/spit`,
   `doseq dotimes for` (macros), `letfn`, `case`.
5. **Performance later, maybe**: NaN-boxing, symbol→binding caching,
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
