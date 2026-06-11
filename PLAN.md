# cljc — single-file Clojure in C

Goal: a babashka-coverage-ish Clojure interpreter in one embeddable C file
(`cljc.c`), inspired by Janet. Build: `make`. Tests: `make test` (runs the
suite twice — normal and `CLJC_GC_STRESS=1`).

## Status (as of 2026-06-10)

~4,300 lines, zero warnings, 408 assertions in `tests.clj`, ASan/UBSan clean
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
   behind vec_len/vec_nth/vec_conj1/vec_assoc_idx.
6. ~~Sets~~ ✅ done 2026-06-10 — CLJC_SET reuses the map union member and
   the whole HAMT engine (elements stored as their own values). #{...}
   reader literal (duplicate elements error), set/hash-set/disj/set?,
   sets callable as membership fns, conj/contains?/get/count/seq work,
   prelude set/union set/intersection set/difference. '#' stays a symbol
   char except when a form starts with "#{" (so t# macro symbols still read).
7. ~~Regex~~ ✅ done 2026-06-10 — tiny self-contained backtracking engine
   (~350 lines, no deps): literals . ^ $ [a-z] [^...] \d \D \w \W \s \S,
   capture groups, (?:...), |, * + ? with lazy variants. X+ desugars to
   X X* (atom re-parsed); X? to ALT; star has an empty-match cycle guard.
   #"..." reader literal = raw string (only \" escape). re-find/
   re-matches/re-seq; patterns are plain strings (no regex value type).
   NOT supported: {n,m}, backrefs, lookaround (braces are literals).
   Quality pass 2026-06-10: re-seq empty-match iteration fixed (was OOB
   past NUL), re-replace substitutes the trailing empty match, a 2M-step
   budget guards catastrophic backtracking ((a+)+b errors instead of
   hanging), format %s no longer truncates at 512 bytes, quasiquote
   descends into set templates, apply splices any seqable via to_seq.
   Note: re-split does not split on empty-width matches (#"" or #"x*").
8. ~~Scripting surface~~ ✅ done 2026-06-10 — format (printf subset:
   %s %d %x %o %f %e %g with flags/width/precision), str/replace
   (LITERAL match — regex variants are explicitly named), re-replace
   (all matches, $0-$9 group refs, $$ escape), re-split (trailing
   empties dropped), condp (with thrown no-match), for rewritten with
   :when/:let modifiers (classic recursive mapcat expansion),
   Math/{sqrt,pow,floor,ceil,round,abs}, rand, rand-int, rand-nth,
   max-key, min-key. Makefile links -lm. Skipped: `..` (no interop),
   iterate (no lazy seqs).
9. ~~Threading variants + reflection~~ ✅ done 2026-06-10 — some->
   some->> cond-> cond->> as-> (recursive expansions; some-> gensyms
   against double-eval), read-string + eval natives (eval runs in the
   root env, no lexical capture, like Clojure), peek/pop (vector pop
   has an O(1) tail fast path, O(n) rebuild every 32nd), empty,
   not-empty, doall/dorun (eager no-ops), flatten, fnil.
10. ~~Performance round 1~~ ✅ done 2026-06-10 — 5.7x on the eval
    benchmark (fib 27 + 3M loop + seq pipeline: 2.65s → 0.46s):
    (a) root def MUTATES the existing binding instead of shadowing,
    making root Binding* stable for the process lifetime; (b) each
    symbol cell carries a root_cache (as.symc aliases as.sym) that
    memoizes its resolved root binding — locals scan first, the root
    scan happens once per symbol cell ever; (c) bindings are pooled
    like cells/envs (a fn call allocates one per param), recycled at
    env sweep. CAVEAT: the cache assumes a single root env per
    process (already documented in the README embedding rules).
11. ~~Performance round 2~~ ✅ done 2026-06-10 — allocation reduction,
    driven by the AoC day-5 profile (25M vector assocs): (a) recur
    sentinels store up to 3 values inline in the cell union (covers
    real loops; wider recurs spill to a flagged heap array); (b) the
    32-byte union memset is skipped for INT/DOUBLE allocs (leaf tags,
    no owned pointers/children); (c) 256B chunk pool for 32-slot trie
    nodes + size-classed free lists for vector tails (1..32 ptrs).
    Always-32 tail chunks were tried and REVERTED — they inflated
    memory between GCs and regressed page-fault-bound vector builds.
    Results: day5 17.0s→12.5s (-26%), 5M loop 578→504ms (-13%),
    others neutral. Next bottleneck identified and deferred: argument
    CONS LISTS — every native call allocates its arg list (~12 conses
    per day5 iteration); fixing it means a calling-convention overhaul
    touching all ~150 natives.
12. ~~nREPL server~~ ✅ done 2026-06-10 — ./cljc --nrepl [port]: bencode
    protocol in C (~200 lines), ops clone/describe/eval/load-file/close/
    interrupt/ls-sessions, stdout/stderr captured per-eval via swappable
    COUT/CERR globals + open_memstream, .nrepl-port written for editor
    discovery. Single client, loopback, serial. Verified end-to-end with
    a Python bencode client (sessions, out/err routing, state
    persistence). Untested against real Conjure/CIDER yet — try it!
13. ~~Compat Tier 1~~ ✅ done 2026-06-10 — #(...) shorthand (%, %1-%9,
    %&; scan via worklist; % and %1 mutually exclusive), ns/require/
    use/import as no-op special forms (flat globals already match the
    :as str convention), defn/defmacro docstrings skipped, \a \space
    char literals (as 1-char strings), ^meta parse-and-discard,
    #?(:cljc/:default) reader conditionals (no #?@; no-match => nil),
    comment macro, loop destructuring (gensym rewrite + inner let, the
    same rewrite Clojure does). Remaining compat tiers: lazy seqs
    (Tier 2, big), vars/multimethods/protocols (Tier 3, usage-driven).
14. ~~Lazy sequences~~ ✅ done 2026-06-10 — as designed below, plus two
    discoveries: (1) lazy concat is required or cycle self-realizes into
    a stack overflow; (2) eval must treat CLJC_LAZY in form position as
    a call form (macros like ->> build expansions WITH concat — their
    expansions are now lazy seqs). Cost: 1M seq pipeline 321ms -> 2.9s
    (interpreted thunk-per-element); chunked seqs are the known remedy.
    Original design:
    Design (agreed, ready to execute):
    a. New tag CLJC_LAZY: union { Cljc *thunk; Cljc *cached; bool done; }
       — thunk is a zero-arg fn; forcing calls it once, caches the
       result (which must be a seq: nil, cons, or another lazy), marks
       done, and drops the thunk reference (GC reclaims the closure).
    b. (lazy-seq & body) special form => lazy cell wrapping (fn [] body).
    c. Force discipline: first/rest/seq/to_seq force ONE cell, never the
       chain — laziness survives only if cons tails can be lazy cells.
       to_seq must stop materializing: return the cons chain as-is and
       let consumers walk via first/rest (audit every to_seq caller —
       the iteration idiom `for (l = to_seq(x); l->tag == CLJC_LIST;)`
       must become a seq-cursor helper that forces as it walks).
    d. Move map/filter/take/drop/concat/iterate/repeat/cycle/range
       (infinite arity) to lazy prelude definitions via lazy-seq, e.g.
       (defn map [f c] (lazy-seq (when-let [s (seq c)]
         (cons (f (first s)) (map f (rest s)))))).
       Eager consumers (reduce/count/doseq/into/vec/sort) force
       progressively via the cursor.
    e. GC: mark thunk+cached; print realizes fully (infinite seq print
       hangs — same as Clojure); equality forces both sides.
    f. Tests: (take 5 (range)) infinite range, (take 3 (iterate inc 0)),
       cycle, map-over-infinite, ensure 1M-element eager pipelines do
       not regress (benchmarks/), GC-stress with half-realized chains.
    Risks: every `->as.cons.tail` walk in primitives is a potential
    eager-forcing bug; introduce `static Cljc *seq_first/seq_next` and
    convert callers mechanically.
15. ~~Compat Tier 3~~ ✅ done 2026-06-10 — binding/with-redefs as a
    special form (mutates root bindings in place — sound because root
    def mutates, single-threaded; restored on every exit via an
    ErrFrame), type native (keyword per tag; record maps carry
    :cljc/type), defmulti/defmethod (pure prelude: global registry
    atom, :default fallback), defprotocol/extend-type/satisfies?
    (methods are multimethods dispatching on (type (first args))),
    defrecord (->Ctor building :cljc/type-tagged maps, so records ARE
    maps and participate in protocols). Divergences: type returns
    keywords not classes; no defrecord positional equality semantics
    beyond map equality; extend-type takes type KEYWORDS.
16. ~~C FFI (s7/cload model)~~ ✅ done 2026-06-10 — inspired by
    ~/build/s7's cload.scm: (ffi/define [[:double cos [:double]] ...]
    {:headers [...] :libs "-lm"}) declares signatures as DATA; a
    prelude generator emits glue C marshalling through a CljcFfiApi
    VTABLE (append-only struct — modules need no cljc symbols/headers),
    (sh ...) compiles it -shared -fPIC, ffi-load* dlopens and calls
    cljc_module_init(env, api). Types: :int :double :string :void
    :pointer (as int64; NULL<->nil for strings). Modules are cached
    content-addressed in /tmp (hash of code+libs) — reload is a dlopen.
    libc.clj ships in-repo (load-file): process/env/fs/time surface +
    friendly wrappers (file-exists? cwd env mkdir-p now-epoch).
    Generator emits _GNU_SOURCE (get_current_dir_name needs it).
    Structs: (ffi/defstruct timeval [[:int tv_sec] ...] {:headers [...]})
    generates make-NAME (calloc), NAME-field getters, set-NAME-field!
    setters — struct must be declared by the headers. json.clj battery:
    pure-cljc parser/writer (parse-long/double, :keywords? opt, escape
    round-trip; \uXXXX => "?" placeholder). Reader gained \b \f \0
    string escapes (json.clj needed them — dogfooding works).
    Also: sh native (popen, {:exit :out}). Requires cc at runtime
    (the s7 trade). Future: :pointer type, struct support, a libc.clj
    batteries module, caching compiled modules by signature hash.
17. ~~Chunked seqs (perf round 3)~~ ✅ done 2026-06-10 — map/filter
    realize 32 elements per lazy cell, chunk walk in C natives
    (cljc/chunk-map*, cljc/chunk-filter*, cljc/onto prepends a strict
    list onto a lazy tail with no per-element lazy cells). Pipeline
    2.9s -> 443ms (eager baseline was 321ms). Chunked semantics are
    Clojure-faithful: side effects realize <=32 at a time (tests pin
    exactly-one-chunk behavior). 2-coll map stays unchunked.
18. ~~Args-as-array calling convention~~ ✅ merged 2026-06-11 — natives
    take (env, argv, nargs); args live on a GC-rooted value stack
    (vstack, vsp saved/restored through ErrFrames and top-level
    unwinds); & rest is the only remaining arg-list construction;
    fn recur swaps argv to the sentinel's array (with a VOLATILE
    keep-alive — the optimizer otherwise elides the cell's only root:
    crashes at -O1+, invisible at -O0/ASan). FFI ABI bumped (ffi3
    cache prefix, 3-arg wrappers with arity guards). Verdict: pipeline
    -25%, loop -13%, day5 0% — its real bottleneck is vec_assoc
    path-copy memcpy (~15GB/25M iters). NEXT PERF LEVER IF NEEDED:
    transients (mutate-in-place vectors/maps behind transient!/
    persistent!, Clojure's own answer for hot assoc loops).
19. ~~Transient vectors~~ ✅ done 2026-06-11 — CLJC_TVEC shares the
    persistent trie; node ownership by monotonic edit id (never reused,
    so pool-recycled cells can't forge ownership); a node is copied at
    most once per transient then mutated in place; tail is a private
    32-cap chunk mutated directly (conj!/assoc! in the tail allocate
    NOTHING). persistent! invalidates the transient and trims the tail.
    transient/persistent!/conj!/assoc! + nth/count on transients; into
    uses the fast path for vectors. Day5: 12.5s -> 9.46s (-24%); 1M
    build 206 -> 81ms (2.5x). Honest residual: day5 is now EVAL-bound
    (~12 interpreted ops/iter) — the remaining lever is a bytecode VM.
    Map transients (assoc!/dissoc! on HAMTs) not yet — same edit-id
    scheme applies when wanted.
20. ~~Static bundling~~ ✅ done 2026-06-11 — bundle.clj: a 30-line cljc
    script that embeds a script's bytes in a generated C file with the
    runtime and ccs a standalone ~130KB binary (Janet-style deploys).
    Enablers: *args* (script argv as a vector), int coercion (char
    byte / double truncation). NOT compilation — noted explicitly.
    FUTURE TIER (plausible, unbuilt): per-function Clojure->C JIT via
    the existing FFI pipeline (generate C → cc → dlopen → mutate root
    binding) — would attack the eval-dispatch wall day5 ended at.
21. ~~Per-function JIT (tier 2)~~ ✅ done 2026-06-11 — jit.clj: a
    ~150-line compiler IN CLJC for the numeric subset (if/let/loop/
    recur/self-recursion/int ops), emitting unboxed long long C;
    fn-level recur compiles to the enclosing for(;;); self-calls are
    direct C calls. Wrapper unboxes via the FFI vtable, arity- and
    type-checked. jit/defn records source; jit/compile! generates,
    builds through cljc/ffi-build (content-cached => warm compile is
    a dlopen), and rebinds the root name — callers switch live.
    fib(32) 1.24s -> 6ms; 100M-iter loop 26ms. Subset limits: ints
    only, no calls to other fns, no collections. Next tier if wanted:
    boxed Cljc* codegen through the api vtable for general functions
    (smaller win, broader coverage), or whole-program AOT.
22. ~~Library survey + loading require~~ ✅ done 2026-06-11 — require
    now LOADS: namespace -> path against *load-path* (["." "vendor"]),
    .clj/.cljc, once-only via cljc/loaded-namespaces. UPSTREAM
    clojure.set RUNS IN FULL (vendored, EPL notice retained, 9 fns
    suite-tested incl. join/project/index). Compat that unlocked it:
    defn attr-maps skipped, defn-, identical? (with interned KEYWORD
    CELLS — immortal, out-of-pool, like smallints — so (identical?
    :a :a) and zero alloc per keyword read), transient map/set SHIMS
    (persistent fallbacks — same results, no speedup; real HAMT
    transients still future), with-meta/meta no-ops. Survey: medley
    fails at read (#?@-class gaps); Clerk impossible (JVM-bound, not
    even bb runs it). Aliases (:as m -> m/foo) NOT implemented — flat
    globals; next compat step if multi-lib use emerges.
23. ~~Library survey round 2~~ ✅ done 2026-06-11 — medley runs
    substantially and upstream clojure.walk completely. Compat landed:
    #?@ splicing (reader-splice marker unpacked by read_list, also
    powers #_ discard), apostrophes inside symbols (coll'), named fns
    ((fn step [x] ...) — late binding into a wrapper env), reduced/
    reduced?/unreduced with native reduce early-exit, conj of [k v]
    entries and maps onto maps, :as aliases (registered by require,
    resolved on lookup miss by prefix-strip, cached in the symbol
    cell), instance? as always-false macro, volatile!/vreset!/vswap!
    as atom shims, coll?/map-entry?/list*/find/key/val/nnext family.
24. ~~Metadata~~ ✅ done 2026-06-11 — Cljc gains a meta slot (+8B/cell,
    one gc_mark line); with-meta copies the cell (vectors also copy
    their owned tail — double-free otherwise) and errors on non-
    carriers like Clojure; ^{} / ^:kw in the reader compile to
    runtime (with-meta form m), ^Tag hints still discarded; vary-meta
    real. NOT preserved through coll ops (deferred — zip threads meta
    explicitly so it doesn't need it). UPSTREAM CLOJURE.ZIP RUNS IN
    FULL (4th library). Previous finding, now resolved: clojure.zip was BLOCKED
    structurally — zippers carry their branch?/children/make-node fns
    in ^{:zip/...} metadata, so the metadata-discard divergence is
    load-bearing there. Unlock if wanted: real with-meta/meta via a
    cell->meta side table (with-meta copies the cell, records meta;
    sweep drops dead entries). ~a session. Other untried candidates:
    clojure.data (diff), camel-snake-kebab, hiccup core, tools.cli.
25. **Performance later, maybe**: args-as-array calling convention,
    NaN-boxing, bytecode VM. Only if a real workload demands it.

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
