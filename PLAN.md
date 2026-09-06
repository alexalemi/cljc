# cljc — single-file Clojure in C

Goal: a babashka-coverage-ish Clojure interpreter in one embeddable C file
(`cljc.c`), inspired by Janet. Build: `make`. Tests: `make test`
(suite + conformance); `make test-full` adds the `CLJC_GC_STRESS=1` pass.

## Status (as of 2026-09-05, v0.4.1)

~15,400 lines, zero warnings, ~1470 assertions in `tests.clj` + 412-expression
conformance corpus diffed against a JVM golden (`make conformance`), ASan/UBSan clean
(ASan needs `ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0` —
required by conservative stack scanning, same as Boehm GC). CI: Linux (+ASan),
macOS arm64 + x86_64/Rosetta, Windows (MSYS2 mingw64) — suite runs normal +
GC-stress on every push; tagged releases publish portable binaries.

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
    Cross-compilation ✅ done 2026-06-21 — bundle.clj grew flag parsing:
    --static, --windows (mingw-w64 shorthand), --cc=/--libs=/--cflags=
    and the CLJC_CC env var steer any cross toolchain (zig cc, ARM gcc).
    cljc.c is now Windows-portable behind `#ifdef _WIN32`: winsock2 for
    the tcp prims, Win32 loader for dlopen, QueryPerformanceCounter for
    monotonic time, tmpfile spooling for with-out-str, st_mtime for
    mtime. Degraded on Windows: REPL → plain line input (no termios),
    nREPL server unavailable (sockets aren't fdopen-able). Verified:
    full cljc.exe and a bundle both build clean under
    x86_64-w64-mingw32-gcc (0 warnings); Linux suite unchanged.
    Couldn't run the .exe here (no wine) — runtime behavior unverified.
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
25. ~~Meta propagation + survey round 4~~ ✅ done 2026-06-11 — metadata
    now propagates through conj/assoc/dissoc on all collections, list
    conj, vector index-assoc, and the transient/persistent! roundtrip
    (into preserves meta). reify implemented over the type/multimethod
    machinery (anon type keyword + defmethods + tagged map). ns now
    PROCESSES (:require ...) clauses (transitive library deps load);
    namespace dashes map to file underscores. Added: subvec, \formfeed
    \backspace \oNNN char literals. camel-snake-kebab: loads all 5
    files transitively, still one arity error from green — PARTIAL
    (next session: chase the 2-arg arity miss in its split path).
    clojure.data: upstream URL 404'd, untried.
26. ~~KNOWN BUG: lazy macro expansions~~ ✅ CLOSED 2026-07-09 (verified
    fixed, regression-pinned). Hunt findings: rebuilt the 0ac1817 binary
    and reproduced both historical symptoms — (cons '+ (concat lazy ...))
    as an expansion SILENTLY returned 0 (lazy cell in form position
    dropped its args → (+)), and the lazy reify splice threw "no matching
    arity for 2 args". On today's binary every shape passes, normal +
    GC-stress + ASan: lazy-seq-as-form, ~@ splices across chunk
    boundaries (100 args), mapcat splices in VM-compiled fns evaluated
    repeatedly (splice-once semantics: side effects realize exactly once),
    nested lazy splices, 40-method defmethod generation, macro-defining-
    macros, lazy param-vector splices (35 params), macro-time throws.
    Fixed en route by item 14's eval-of-lazy-as-form + the item-36 VM
    bughunt (macro resolution). All shapes now pinned in tests.clj (the
    i26-* block). NOT item 26 but found while hunting: (a) the multimethod-
    based reify EMULATION pattern (defmethod per clause) has last-closure-
    wins semantics per call site — the CURRENT builtin reify is immune
    (per-instance :cljc/impls closures); (b) tests.clj's "refer shadows
    reduce" test was marked must-stay-LAST but ~500 lines had been
    appended after it — defused by re-referring clojure.core/reduce
    (re-refer restores, now asserted).
27. ~~Namespaces-lite (require isolation)~~ ✅ done 2026-06-11 — the
    flat-global collision fix. Design: symbol cells carry a home_ns
    stamped by the reader during library loads (cljc/in-ns* set/
    restored around load-file, nesting-safe); defs during a load land
    under "ns/name"; resolution is locals -> home-ns/name -> bare ->
    alias (m/foo -> <full-ns>/foo -> bare), all cached in the symbol
    cell so steady-state cost is zero. require parses :as (alias ->
    full ns) and :refer (copies bindings to bare names). ACCEPTANCE:
    zip+medley+set+walk coexist, core next intact. KEY INSIGHT: the
    context must live on symbol CELLS, not an eval-time global —
    library code evaluates at call time, long after loading.
28. ~~DX + bb-gap batch~~ ✅ done 2026-06-11 — (a) ERROR TRACES:
    reader stamps {:line} meta on list forms (rd_line counter, strings
    counted too); eval wrapper maintains a form stack (ErrFrames and
    top-level handlers restore eval_sp); raise snapshots the top 8
    frames into err_trace, printed by print_error. (b) TRANSDUCERS:
    map-xf/filter-xf/take-xf/drop-xf/keep-xf/mapcat-xf/distinct-xf +
    transduce/sequence*2/eduction; reduce is now a LAZY CURSOR (was
    realizing via to_seq — reduced over infinite seqs hung); (conj)=>[]
    0-arity; mapcat-xf preserves the reduced wrapper (inner unwrap =
    infinite loop, found by suite hang). NAMING DIVERGENCE: xf
    constructors are separate names (map-xf), not 1-arity overloads of
    map — our multi-arity map already uses arity 2 for two colls.
    (c) cheap tier: dedupe partition-by split-with tree-seq lazy-cat
    run! not-any? not-every? edn/read-string pprint. (d) fs.clj
    (babashka.fs-flavored; list-dir via FFI opendir/readdir/dirent
    defstruct!) + process.clj (shell-escaped sh/shell/out).
29. ~~Install story~~ ✅ done 2026-06-11 — make install/uninstall with
    PREFIX/DESTDIR conventions; batteries + vendor libs land in
    $PREFIX/share/cljc (path compiled in via -DCLJC_SHAREDIR);
    *load-path* = [. vendor CLJC_PATH-dirs sharedir sharedir/vendor];
    load-file searches *load-path* after the literal path; --version
    flag; install.sh (defaults to ~/.local, tests before installing).
    Verified end-to-end from a scratch prefix + unrelated cwd.
30. ~~Elm-style errors~~ ✅ done 2026-06-11 — print_error renders:
    red header bar with the source file, plain-language message
    (unresolved symbols: "I don't know what `x` refers to."), the
    offending source line (retained script text + err_line from the
    innermost located frame) with a caret under the token (found by
    substring search in the line), "Did you mean `y`?" via capped
    levenshtein over live root bindings (so user defs suggest too),
    and the dimmed trace. ANSI color only on ttys. load-file'd code
    gets traces but not source excerpts (its text isn't retained) —
    noted for later.
31. ~~Enhanced REPL~~ ✅ done 2026-06-11 — linenoise-style editor, zero
    deps: raw termios; insert/backspace/delete, arrows, ctrl-a/e/k/u/w/
    l/c, ctrl-d exit; persistent history (~/.cljc_history, 512 entries,
    up/down with in-progress-line save); tab completion against LIVE
    root bindings + special forms (longest-common-prefix extension,
    candidate listing); live syntax highlighting per keypress incl. MATCHING-BRACKET reverse-video (closer behind cursor or opener at cursor, string/comment-aware pairing) (strings
    green, keywords cyan, numbers yellow, delimiters+comments dim);
    paren-balance multiline with ... continuation; *1 *2 *3 result
    history (ipython-style); !cmd shell mode ({:exit :out} into *1+Out, red exit notice); MISMATCH detection: pairs type-check — ( closed by ] renders both in red reverse, orphan closers (no opener) red alone. ABSOLUTE history: prompt is cljc[N]>, results print as dim [N], and *results* is a literal vector so (*results* 3) retrieves and (count *results*)/(last *results*) just work — shell results numbered too. Errors in the REPL get the full Elm
    treatment minus source excerpts. Also column-precise carets landed
    this session (form {:line :col} meta; token search anchored at
    col; fallback caret at the form's paren). UNICODE glyphs: when the
    locale is UTF-8 (term_utf8 checks LC_ALL→LC_CTYPE→LANG), errors
    render with ─ header rule, │ gutter, ▔ underline; plain -, |, ^
    otherwise — pure cosmetics, zero behavior change.
32. ~~Clerk clone~~ ✅ done 2026-06-11 — `cljc notebook file.clj [port]`
    (alias `clerk`; `-o out.html` for static builds). clerk.clj battery
    (~370 lines, deliberately NO FFI deps so it works without cc):
    line-scanner cell parser (depth/string tracking; top-level `;;` →
    markdown prose, each top-level form → code cell, single-`;` invisible,
    `;; @clerk:hide-code` / `hide-result` directives); per-cell eval via
    `(do …)` wrap in the shared root env with error isolation +
    with-out-str capture; hand-rolled markdown subset (h1-h3, lists,
    bold/italic/code/links; `$…$` passes through to client-side KaTeX);
    server-side syntax highlighting to span classes; viewer registry
    (map → kv table, seq-of-maps → 2D table, "<"-string → raw html,
    `clerk/register-viewer!` to extend; fn/nil values hidden);
    single-threaded HTTP+SSE server — `tcp/accept` 250ms timeout doubles
    as the mtime watch tick, browser reloads on `data: reload`. Design
    follows their s7/janet/racket clerk clones (studied all three).
    New C support: tcp/listen accept recv send close (MSG_NOSIGNAL),
    cljc/mtime* (ms resolution), cljc/with-out-str* (ErrFrame-safe
    stream swap) + with-out-str macro, str/index-of, sequential?.
33. ~~Subcommand CLI~~ ✅ done 2026-06-11 — `cljc help|version|run|eval
    (-e)|repl|nrepl|notebook|test|lint|bundle`. Exact subcommand match
    wins; `cljc run <file>` is the escape hatch; bare `cljc <file.clj>`
    still works; --version/--nrepl/-h kept as aliases. `test` loads the
    test.clj battery + files and exits 1 on failures; `lint` is a
    reader-level syntax check with the Elm rendering; `bundle` now finds
    cljc.c in the share dir (installed by make install) so it works
    outside the checkout. WISHED-FOR LATER (user): `doc`, `fmt`,
    `docgen`, `lsp` subcommands.
34. AOC CORPUS as the compat suite (user request, in progress
    2026-06-11) — run all 167 of their advent solutions
    (~/projects/advent/*/clojure/p*.clj, self-asserting). Sweep script:
    /tmp/aoc-sweep.sh (60s timeout, TSV per file). Baseline 16/167 →
    fix in batches. Landed so far: transducer arities; case list-branch
    + no-match-throws fix (was silently nil — broke grid parsers);
    cons-onto-lazy-seq truncation fixed in print/=/nth (seq1 stepping);
    read-string saves line tracking (nested requires poisoned caret
    lines); regex-tagged #\"...\" (meta {:regex true}, strings can carry
    meta now) with regex-aware str/split; ns vector clauses; clojure.test
    + clojure.string vendor shims (are/use-fixtures added to battery);
    bit ops; ##Inf/##-Inf/##NaN; (def ^:dynamic ...); assert-with-msg;
    time macro (+ cljc/now-ms*, cljc/epoch*; libc.clj must NOT bind C
    time() — it shadowed the macro and SEGV'd); == distinct? char class
    re-pattern reductions every-pred some-fn memoize take-last drop-last
    take-nth update-keys update-vals boolean? nat-int? isa? pmap(serial)
    str/replace-first str/index-of sequential?; peek/pop on maps =
    priority-map semantics (O(n)) so clojure.data.priority-map is a
    hash-map shim — their util.clj a-star runs; deftype tolerated
    (constructor → field map, methods ignored);
    clojure.lang.PersistentQueue/EMPTY = [] (LIFO divergence); vendor
    clojure.data.json + clojure.math.combinatorics + nextjournal.clerk.
    TALLY after odd-tail + perf round 1 (2026-06-12): **97 pass /
    13 fail / 41 timeout** of 151 — from 16 at campaign start. By year:
    2015 15/22, 2016 12/22, 2017 16/26, 2018 7/11, 2021 15/25,
    2022 18/25, 2023 9/13, 2024 5/6, 2025 0/1 (JS).
    The 13 fails: 6 unfixable-ish (jpeg/match custom lib, defproject,
    upstream-broken OPS, 2 missing inputs, cherry-JS), int-array/
    byte-array (2, mutable arrays), 2 huge-apply vstack overflows,
    Integer/toString and 2023 d18 {n,m} quantifiers ✅ fixed
    2026-06-12 (X{n}/X{n,}/X{n,m} desugar to atom copies, n,m ≤ 64;
    Integer/toString with radix + toBinaryString).
    SWEEP with the 3.2x binary (2026-06-12): **99 pass / 12 fail /
    40 timeout** — measured, matching the estimate. The 40 remaining
    timeouts are brute-force minute+ puzzles needing >>3x (bytecode VM
    territory). The 2015 d20 / 2017 d22 "OOM" investigation
    (2026-06-12) found a REAL BUG, not GC policy: vector-pattern
    DESTRUCTURING called to_seq, fully realizing the value — a
    self-recursive sieve over (range) ran away to 17GB before ever
    returning. destructure now steps with the seq1 cursor; lazy/
    infinite seqs destructure one element per binding. 2015 d20 passes
    both parts at 2.2GB. Also: gc_extra_bytes counter (string buffers,
    HAMT kid arrays, vector tails) with a 256MB cap backs up the
    cell-count threshold; CLJC_GC_LOG=1 prints live/freed + tag
    histogram per collection. MUTABLE ARRAYS: int-array/byte-array/
    long-array/object-array are transient vectors (assoc! mutates in
    place), aget/aset/alength on top; TVECs are seqable. 2017 d5 and
    2021 d16 pass. → 102 passing.
    (Previous tally after batch 7, 2026-06-11: 83/28/40.)
    Batch 5 added MD5 native + interop shims (MessageDigest idiom runs
    verbatim, .indexOf, Integer/parseInt radix, Character/digit). Batch 6:
    regex lookahead (?=)(?!), (ns foo) ENTERS foo (defs land foo/, like
    require'd libs), syntax-quote qualifies home-ns-defined symbols
    (matches JVM-qualified case constants), n-coll map. Batch 7: conj on
    lazy seqs; binding resolves ^:dynamic defs under their home ns
    (regression from ns-entering); alias resolution prefers the
    ns-qualified def over a bare global (one-pass || bug); bootstrap
    set/intersection+difference made variadic; mapv n-coll; sort-by
    comparator arity; RX_MAX_GROUPS 10→32.
    KNOWN REMAINING (~10 fails + 38 timeouts): perf timeouts are the big
    bucket — the corpus is the benchmark set for any future perf work
    (PLAN item 36). Unfixable-ish: BufferedImage/jpeg visualization
    files, defproject, 2025 cherry-compiled JS, 2 missing input files,
    2018 p16 references undefined OPS (broken upstream). Odd tail to
    diagnose someday: 2017 p03 case-clause \"I\", 2021 p13/2015 p22
    expected-number, 2022 p17 case nil, 2022 p21/2023 p22 not-seqable,
    2023 p18 keyword-coercion, 2016 p19 pop-empty-vector, 2017 p16
    dumped-core under timeout, 2 huge-apply value-stack overflows
    (apply str on multi-million-char seqs).
35. ~~Judge clone~~ ✅ done 2026-06-11 — `cljc judge [-a|-i] <files...>`,
    judge.clj battery (~290 lines). Macros test / test-error /
    test-stdout / trust are NO-OPS in normal runs (files need
    (require '[judge :refer [test ...]]) and carry zero runtime cost);
    the runner reads the source itself, tracks each top-level form's
    char extents (string/comment-aware scanner) plus depth-1 element
    extents inside judge forms, evals everything in order, and splices
    corrected snapshots into the ORIGINAL text — formatting never
    reflows, only the expected value is replaced (upstream's minimal-
    rewrite rule). Seq snapshots are quoted ('(1 2 3)) so they read
    back as data. Default mode writes file.clj.tested + a red/green
    diff; -a applies in place; -i prompts y/n/q per correction (y
    default). trust never re-evals once filled. Exit 0 green / 1
    corrections / 2 load error (editor-distinguishable, like upstream).
    New natives: read-line, flush, cljc/isatty*.
    LATER MAYBE: deftest grouping, test-macro with gensym
    stabilization, FILE:LINE targets, --name filters.
    (c) numerics/dates/fork-pmap menu from the bb-gap analysis.
36. PERFORMANCE (in progress 2026-06-12; benchmark set = the AoC corpus
    timeouts). ROUND 1 ✅ ~2.7x: gprof showed GC at 40-64% of runtime —
    (a) gc threshold 2x→4x live and floor 64k→1M allocs (tight loops
    were collection-bound), (b) O(1) global-bounds reject in the
    conservative scan (the per-stack-word block walk was ~13%),
    (c) MACRO EXPANSION SPLICING: call sites mutate into their expansion
    on first eval — re-expansion (qq_expand) ran millions of times in
    hot loops; divergence: redefining a macro doesn't reach
    already-evaluated sites (same as JVM compiled code).
    2015 d9 12.8→4.7s, d13 12.6→4.3s, 2017 d10 18.5→7.1s.
    FAILED EXPERIMENT (do not retry naively): classify-once
    special-form byte on symbol cells was 5-8% SLOWER than the plain
    23-pointer-compare chain — the branch predictor already eats it.
    ROUND 2 candidates (post-splice profile: eval_inner 31%,
    binding_alloc 18% at 68M calls, apply 7%, env_alloc+destructure 6%):
    the args-as-array calling convention / inline env slots is now the
    clear next cut — every call allocates env + one Binding node per
    param. Then NaN-boxing / bytecode VM if still needed.
    ROUND 2 ✅ (2026-06-12, another ~14%, cumulative ~3.2x): (a) child
    envs bind into 4 inline slots before Binding overflow (root keeps
    pure Binding chains — root_cache requires stable Binding*; lookup
    checks chain-then-slots-backward for shadowing); 6 slots was WORSE
    (bigger envs made gc sweep traffic visibly slower) — 4 is the spot.
    (b) sym_amp() now caches the interned "&" — arity dispatch was
    re-interning (a hash lookup) per param per call: 93M times in one
    benchmark run. (c) destructuring loop desugar (gensym let rewrite)
    splices into the form — was rebuilt per loop ENTRY.
    Benchmarks now: 2015 d9 3.9s, d13 3.7s, 2017 d10 6.2s (from
    12.8/12.6/18.5 two days ago).
    ROUND 3 — BYTECODE VM ✅ (branch vm, 2026-06-12): hybrid stack VM,
    ~600 lines. Fn arity bodies compile to CLJC_CHUNK cells on first
    call (cached in arity meta); compiler covers if/do/let/loop/recur/
    and/or/when/cond/calls/literals/vectors/closures/lazy-seq, VOP_EVAL
    falls back to the tree-walker per sub-form for the rest (def, try,
    quasiquote, ns, computed map/set literals, named fns) — correct
    without being complete. Macros expand at compile time + splice.
    Operand stack = the GC-rooted vstack; locals stay in slot-envs;
    VOP_SYM uses resolve_symbol (extracted from eval_inner, root_cache
    intact). CRITICAL FIX en route: arities + chunks are SHARED across
    closures of the same source forms (**arities** marker on the forms
    spine meta) — per-iteration closures were recompiling every call
    (5M+ vm_compiles; VM was SLOWER than the tree-walker until this).
    Numbers (post-bughunt, honest): 2015 d9 1.80s (7.1x vs
    pre-campaign), d13 1.85s (6.8x), 2017 d10 3.7s vs 6.3s tree-walk
    (the earlier 1.58s figure was a miscompile that skipped work — see
    bughunt below). VM corpus sweep (run PRE-bughunt, exit-code based —
    deftest failures invisible, see KNOWN GAP): 102 pass / 9 fail /
    40 timeout, no exit-code regressions vs main; 2017 d16 (the
    billion-dance file that once SEGV'd the GC) PASSES inside 60s.
    Error-trace granularity inside compiled bodies is coarser (no
    per-subform frames) — documented divergence.
    BUGHUNT PASS (2026-06-12, two adversarial review agents + manual):
    six real bugs fixed before merge — (1) loop-recur escaping through a
    VOP_EVAL'd try/binding rebound the FN params (silent wrong loop or
    infinite loop; compiler now tree-walks loops containing such forms
    via vmc_contains_recur); (2) vm_resolve_maybe missed home_ns/alias
    macro resolution AND closure-captured locals shadowing macros — the
    misfired expansion was even spliced into shared source (now full
    resolution + cenv local scan); (3) late-resolved macros (forward
    refs, fn→macro redefs) were applied as plain fns — VOP_CALL carries
    the call-site form const and deopts to eval when the callee is a
    macro; (4) compile-error retry leak (meta=TRUE pinned before
    compiling); (5) locals shadowing special forms diverged from eval
    (compiler now matches eval: specials win); (6) malformed if/when/
    cond compiled to silent nil (now bail to eval's errors). Plus
    chunk_keep volatile (chunk's only root could be optimized away) and
    conj no longer propagates the **arities** marker. The bughunt also
    exposed that the PRE-fix VM mis-compiled 2017 d10's knot-hash
    (unresolved symbol, test silently failed) — its "1.6s" was broken-
    fast; the honest VM time is 3.7s vs 6.3s tree-walk.
    KNOWN GAP: deftest failures don't fail script exit codes, so sweep
    PASS counts can hide deftest regressions — sweep should also grep
    "0 failed". Non-tail recur in compiled loops leaks a vstack slot
    per iteration (degenerate programs only; eval treats it as an inert
    value — divergence).
    Next perf ideas if ever needed: compile-time local slot indices
    (skip env scan), direct-threaded dispatch, apply fast path.
37. ~~Example suite + FFI structs/floats + GUI/DB bindings~~ ✅ done
    2026-06-21 — (a) examples/ gallery: 15 self-contained programs
    (mandelbrot, life, primes/lazy, nqueens, sudoku, calc parser,
    brainfuck, macros, shapes/polymorphism, dijkstra, wordfreq, bank/
    atoms, ffi-demo, sqlite, fractal-svg) + examples/serve.clj, a gallery
    HTTP server written IN cljc (runs each example, captures output,
    highlight.js page). (b) FFI gained :float (marshals via as_double/
    mk_double with a (float) cast) and STRUCT-BY-VALUE: a :structs
    registry {"Color" [[:int "r"]...]} lets sigs reference C struct types;
    struct args cross as cljc vectors (destructured with new vtable
    nth_elem), struct returns come back as vectors (built with new vtable
    mk_vec). Both appended to CljcFfiApi (append-only preserved); cache
    prefix ffi3->ffi4. The generated wrapper #includes the real headers so
    the C compiler handles the actual by-value ABI — no libffi. (c)
    raylib.clj: full binding built on the above (Color/Vector2/Rectangle
    structs, :float args, GetMousePosition struct return) — verified to
    LOAD + init raylib through the FFI; window/GL-context creation needs a
    real display (use xvfb-run headless). (d) sqlite.clj: libsqlite3 via a
    shim header (static inline wrappers flatten sqlite3** out-params to
    handle-returning calls) — open/exec/prepare/step/column, runs against
    real libsqlite3. (e) tcp/listen gained an optional host arg (default
    loopback unchanged; "0.0.0.0" for LAN/tailscale) so serve.clj is
    remote-viewable. Regression tests in tests.clj cover :float + struct
    arg/return. NOTE: a stray raylib.h in the repo root is a saved GitHub
    HTML page (bad download), not the header — unused by the build.
38. ~~Coroutines + core.async~~ ✅ done 2026-06-22 — the suspend/resume
    substrate the language was missing (cf. PLAN comparison w/ Janet fibers
    / s7 call/cc). (a) C PRIMITIVE: CLJC_CORO, a stackful Lua-style coroutine
    (coro/new / resume / yield / status / alive?) over ucontext, Linux-only
    (CLJC_HAVE_CORO; Windows errors cleanly, CreateFiber a future option).
    Each coro owns a 1 MiB C stack; the interpreter recurses on it normally
    and yield swapcontexts back to the resumer. The fiddly bits, all solved:
    the SHARED vstack is saved/restored as a per-coro segment on yield/resume
    (vbase recaptured each resume — a coro can be resumed from any depth);
    err_top/cur_exc/eval_sp saved per switch; each coro installs its own base
    ErrFrame so a throw never longjmps across stacks (it propagates to the
    resumer via coro/resume instead). GC: a CLJC_CORO mark case conservatively
    scans each reachable SUSPENDED coro's live stack range + its ucontext
    register blob + its saved vstack segment; gc_collect scans the ACTIVE
    coro's stack with the real SP (not gc_stack_base) and the suspended-main
    range; unreferenced suspended coros are collectible (5000 created+dropped
    → 14 MB RSS, stacks freed). Verified: generators, value passing, nesting,
    operands-live-across-yield, exception propagation, GC-stress with
    suspended coros holding the only refs, ASan/UBSan clean (modulo the known
    benign makecontext warning). Added cljc/sleep-ms* native. (b) csp.clj:
    the ClojureScript model of core.async (single-threaded, cooperative) on
    the primitive — chan (buffered/unbuffered), <! / >! (park via yield), go /
    go-loop (each a coroutine; returns a result channel), close!, timeout,
    alts! (take-only, shared commit-flag handler), a cooperative scheduler,
    and <!! (blocking take — cljc's bonus over JS core.async, pumps the
    scheduler). examples/csp-sieve.clj: the concurrent prime sieve (one
    filter goroutine per prime). Regression tests in tests.clj (coro +
    csp), pass normal + GC-stress + ASan. (c) ASYNC I/O EVENT LOOP ✅ done
    2026-06-22 — the scheduler's idle phase now poll()s the fds that parked
    goroutines wait on (bounded by the nearest timer; blocks until ready when
    only I/O is pending), turning the cooperative scheduler into a real event
    loop (~Janet ev/). New C: cljc/poll-fds* (parallel fds/events vectors +
    timeout → readiness vector; encoding 1=read/closed, 2=write) and
    tcp/connect (completes the socket surface for clients). csp.clj gained
    park-io + accept!/recv!/send! (park on readiness, then the now-non-blocking
    tcp op, so one slow connection never stalls the others). examples/
    csp-http-server.clj: a concurrent async HTTP server, one go block per
    connection on a single thread. Regression test: loopback echo server+client
    through the event loop (normal + GC-stress + ASan clean). CAVEAT surfaced:
    deferred closures over a dotimes/loop binding see its FINAL value (cljc
    reuses loop bindings in place) — pass via a fn param for per-iteration
    capture. (d) BACKPRESSURE + COMBINATORS 2026-06-22 — send! is now
    backpressure-correct: tcp/send-some* (one non-blocking send via
    MSG_DONTWAIT, returns bytes/-1 would-block/-2 dead) + a POLLOUT-park loop,
    so a slow client can't stall the loop. csp.clj combinators (core.async-
    flavored, all go-block based): onto-chan!/to-chan!, pipe, into, merge
    (fan-in), mult/tap/untap/untap-all (broadcast fan-out), take-n. Capstone
    example: examples/csp-chat.clj — a broadcast chat server (mult/tap + event
    loop; many nc/telnet clients, one thread). Combinator + chat regression
    tests in tests.clj (normal + GC-stress + ASan clean).
39. ~~core.async completion + HTTP battery~~ ✅ done 2026-06-22 — (a)
    TRANSDUCER CHANNELS: (chan n xform) reduces values through the transducer
    into the buffer on put — one put can yield many (mapcat), none (filter),
    or a reduced that closes the channel; close! runs the 1-arity completion
    (partition-all flushes its remainder). Backpressure via a ::xform parked-
    putter marker. (b) pub/sub (per-topic mults, lazy topic creation) and
    pipeline-async (N concurrent async workers — the parallelism a single
    thread CAN exploit: overlapping waits). (c) http.clj battery on the csp
    event loop: request parsing (method/path/query/headers/Content-Length
    body), a router with ":param" path captures, response maps (status/
    headers/body, string→200 shorthand), an async server (one go per
    connection), and an async CLIENT that returns a channel (so it composes in
    go blocks — N concurrent fetches via pipeline-async). examples/http-app.clj
    (routed app + shared atom state). GOTCHA: defining http/get SHADOWS core
    get inside the http ns (flat-namespace home-ns resolution) — used map-as-fn
    internally instead. Regression tests (transducer chan, pub/sub, http
    round-trip server+client) pass normal + GC-stress. (d) CONCURRENT nREPL
    ✅ done 2026-06-22 — nrepl.clj: a multi-client bencode nREPL on csp (the
    built-in C --nrepl is single-client/serial; this one is additive, C
    untouched). bencode encode/decode (decode returns nil on a partial value →
    read more), one go block per client accumulating + dispatching framed
    messages, ops clone/describe/eval/load-file/close/ls-sessions, eval with
    with-out-str stdout capture + error status. All sessions share the one root
    env (defs visible across clients — verified two Python clients, B sees A's
    def). start (setup, returns srv) / serve (start + run! + .nrepl-port).
    Tests: bencode round-trip + in-process concurrent eval through the loop.
    NOTE: clerk was NOT moved to csp — its accept loop is entangled with file-
    watching and works cross-platform (csp would make it Linux-only), so a new
    additive nREPL was the better-value, zero-regression choice. REMAINING:
    clerk multi-client (needs a coro-availability fallback); mix; dynamic
    bindings still don't convey across a park.
40. ~~Bundle dependency embedding~~ ✅ done 2026-06-22 — bundling was NOT
    self-contained: it embedded only the script, so a bundled binary that
    require'd clojure.test (or any battery/vendored lib) failed at runtime away
    from vendor//the share dir (the AoC deftest report). Fix: (a) C embedded
    virtual-file registry (CljcEmbeddedFile {name,data}, cljc_set_embedded_files);
    prim_slurp checks it before the filesystem, matching by exact name or a
    '/'-bounded suffix so "clojure/test.clj" satisfies require's
    "./clojure/test.clj" / "vendor/clojure/test.clj" lookups. Empty in a normal
    cljc → no effect. (b) bundle.clj walks the require/ns(:require, list OR
    vector clause)/use/load-file graph transitively (read-string the source
    wrapped in parens, spec->ns), resolves each against *load-path*, and emits
    the dep sources as NUL-terminated byte arrays + a bundled[] table the
    generated main registers before eval. Verified: deftest/clojure.test,
    clojure.set, no-dep, --static, and a real AoC ns-vector file all bundle and
    run from a clean dir. Suite + ASan unaffected (registry empty without a
    bundle).
41. ~~AoC correctness pass~~ ✅ done 2026-06-23 — re-ran the corpus (CLJC_PATH=
    repo/vendor:repo for vendored libs; sweep categorizes PASS/ERROR/TESTFAIL/
    TIMEOUT, grepping "N failed" so deftest failures aren't invisible). Three
    real bugs fixed: (a) get on transient vectors returned nil for every index
    (prim_get missed CLJC_TVEC) — 2017 d5; (b) hex (0x) + radix (NrDDD) integer
    literals were parsed base-10 and truncated — 2021 d16; (c) huge apply
    ((apply str/concat/+ million-element-seq)) overflowed the value stack —
    heap-argv path when the splice won't fit + fast-path headroom so seq1's
    realization doesn't overflow near the cap + lazy concat variadic arity
    (was ~n-deep recursion) — 2016 d16 part-1, 2021 d20. TALLY (15s timeout):
    99 pass / 42 timeout / 7 error / 2 testfail (was 97/40/9/13 at session
    start of this pass). The 2 TESTFAILs (2017 d4 char-set anagram, 2021 d12
    cave-paths) are UPSTREAM-BROKEN — cljc gives byte-identical results to
    babashka; the authors' tests don't match their own code. The 7 ERRORs are
    all not-cljc: jpeg/match + OPS custom libs, 2 missing input files, a
    Leiningen project.clj the glob caught, 2025 cherry-JS, and 2023 d22 (bb
    produces no answer either). NET: cljc is now CORRECT on every corpus file
    it can run; remaining failures are pure performance (the 42 timeouts —
    e.g. 2021 d20 and 2016 d16 now COMPUTE the right answer, just >15s) or
    unfixable-by-design. The 42 timeouts are the benchmark set for any future
    perf work (the bytecode VM got us here; next levers per item 36).
42. ~~General tail-call optimization~~ ✅ done 2026-06-23 — proper tail calls
    for ANY fn in tail position (beyond Clojure, which has only recur +
    trampoline; both also added/present). The recur machinery already did 80%:
    VOP_RECURFN builds a CLJC_RECUR sentinel and apply's for(;;) loop re-binds
    argv to the SAME fn. Generalized: (a) the compiler threads a `tail` flag
    (vmc_form/vmc_body) — propagated through if/do/let/loop/when/cond result
    positions (and/or left non-tail, conservative) — and emits VOP_TAILCALL
    instead of VOP_CALL in tail position; (b) VOP_TAILCALL builds the same
    sentinel but stores the TARGET fn in the cell's meta slot (zero memory cost
    — no union growth; cell_alloc already NULLs meta, so meta==NULL reliably
    means self-recur); (c) apply's loop, on a sentinel with meta set, switches
    fn to the target (loops, replacing the frame) if it's a CLJC_FN, else leaf-
    dispatches (native/keyword/map). fastcall_init moved inside the loop (fn can
    change). Verified: 10M self-tail (non-recur), 5M mutual recursion (was
    stack-bound), multi-arity self-tailcall, native/keyword/map tails, forward-
    ref mutual; ADVERSARIAL: non-tail calls ((inc (h n)), (+ 1 (f 2) 3), fib,
    nested) give correct results (not mis-TCO'd), undefined symbol in tail
    position still errors. fib(32) unchanged (~0.5s — non-tail unaffected).
    Tree-walked fns (uncompiled: def/try bodies) still recurse — TCO is a VM
    feature. Suite + ASan/UBSan + AoC corpus regression all clean.
43. ~~Lazy-seq head-retention fix (seq1_slot)~~ ✅ done 2026-06-23 — profiling
    the AoC timeouts (perf triage: ~9 of 42 are cljc-specific gaps where bb is
    fast; the rest are algorithmic/missing-input) found `(first (filter p
    (iterate f x)))` and friends growing the live set LINEARLY (O(n²) GC): the
    consumer pins the lazy-seq head as its argv slot on the value stack for the
    whole walk, so `F0.cached → F1.cached → …` keeps the entire realized chain
    live (Clojure avoids this with compiler locals-clearing, which cljc lacks).
    Fix: `seq1_slot(Cljc **slot)` — like seq1 but advances the caller's GC root
    slot as it forces, dropping the head behind it. Applied to prim_first /
    prim_reduce / prim_nth (now O(1) live) and prim_count (which also stopped
    materializing via to_seq — 10× better, but a residual O(n) remains: a tight
    no-call loop leaves a stale head pointer in a register that the conservative
    scan finds; reduce avoids it because apply() clobbers the register). p11's
    live set: was growing to millions, now constant ~12K. NOTE: this did NOT get
    the 9 gap files under the 15s bar — they're compute/alloc-bound (interpreter-
    vs-JIT gap; p11 still ~60s vs bb 4.9s). It's a general scalability/correctness
    win for all lazy-seq consumption, not a timeout-buster. The 42 timeouts need
    raw interpreter throughput (env-alloc-per-call, dispatch, resolve_symbol
    volume — a larger project). Suite + ASan/UBSan + corpus regression clean.
    Follow-on: adaptive GC floor — compute-heavy code keeps a tiny live set but
    churns millions of cells, so the fixed per-collection cost (stack scan +
    root marking) dominated at the 1M floor. The floor now doubles when a
    collection reclaims >87.5% (mostly garbage) and halves when >50% survives
    (retentive), capped at GC_CHURN_CAP (4M cells, ~192MB). ~1.2x on alloc-heavy
    workloads; retentive programs stay tight (a 5M-vec build peaks at genuine
    live data, not ballooned garbage). Still not enough to cross 15s on the
    gap files — raw throughput remains the real lever.

44. ~~Loop-closure capture fix (VM)~~ ✅ done 2026-07-02 — closures created
    inside a VM-compiled loop saw POST-recur values of the loop vars:
    VOP_REBIND mutated the loop frame's slots in place while the closure
    held the frame by reference (the tree-walker loop already fresh-framed
    per iteration; apply's fn-recur loop already env_new'd per call — the
    VM was the one remaining mutator). Repro: filter-chain narrowing
    `[100 20 20]` vs Clojure's `[100 10 10]`. Fix: a `captured` flag on
    CljcEnv (fits struct padding, cleared in env_new) — make_fn marks the
    whole parent chain (early-exit at an already-marked frame: ancestors
    are guaranteed marked, parents never change); VOP_REBIND rebinds into
    a FRESH frame when the flag is set, mutates in place otherwise. A
    plain loop's frame holds exactly the loop names, all rewritten from
    the vstack, so the copy reads nothing from the old frame (GC-safe).
    Closure-free loops pay one branch (5M-loop benchmark unchanged);
    closure-per-iteration loops pay the env alloc correctness requires.
    Suite + GC-stress + ASan/UBSan (both modes) clean.
45. ~~Namespace-object layer + ^:private enforcement~~ ✅ done 2026-07-02 —
    the flat-global model gets its missing introspection/identity surface.
    A namespace IS its symbol (no Namespace type): *ns* (a var, tracked by
    set_cur_ns — the single choke point behind the ns special form,
    cljc/in-ns*, and eval-forms*'s save/restore), in-ns, find-ns, the-ns,
    create-ns, all-ns (C-side ns_registry; require/ns/in-ns register),
    ns-publics/ns-interns (maps of symbol→Var), ns-map, ns-aliases,
    ns-resolve, alias. ^:private (incl. defn-, which previously LOST the
    meta, and defmacro) records the qualified name in a side table;
    root_find(public_only) refuses a private reached from a foreign ns at
    the two qualified-match points — plain symbol resolution errors "not
    public" while #'a/x, resolve, and same-ns (home_ns) access stay open,
    matching Clojure's @#'a/x escape. ns-publics* now filters privates, so
    `use`/:refer :all stop leaking them. BONUS pre-existing bug fixed: the
    ns special form processed :require clauses BEFORE entering the ns, so
    a top-level script's own (ns x (:require [y :as a :refer [f]])) hung
    its alias/refer registrations on the PREVIOUS ns (invisible via
    require-one, which enters the ns around load-file; fatal for scripts —
    the refer was simply unresolvable). Clauses now run after set_cur_ns.
    Divergences kept: user-level defs stay bare (visible cross-ns);
    remove-ns/ns-unmap unimplemented (root Binding pointers must stay
    stable for the root_cache). Suite + vendored-namespace smoke (26/26) +
    AoC 16-file sample clean.

46. ~~Tier-1 concurrency: shared fiber scheduler + real future/promise~~
    ✅ done 2026-07-02 — the Janet ev/ model completed. New bootstrap
    Tier 3.5 (after deftype): a runtime-owned cooperative scheduler —
    fiber/ready (thunk queue), fiber/timers ([deadline wake-fn] wheel),
    fiber/io-waiters (fd park table), fiber/pump! (one step: ready →
    due timers → poll(2) bounded by nearest deadline → idle) — that
    clojure.core, csp.clj and vendor core.async ALL share; fiber/*self*
    is the running coroutine (bound around every resume). On it:
    (a) promise/deliver and future/future-call as deftypes (CljcPromise/
    CljcFuture) whose deref BLOCKS correctly — parks the fiber when
    inside one, drives the scheduler on the main thread, throws a
    deadlock ex-info when provably stuck (old shim returned nil from an
    undelivered promise!). A failed future stores the error and rethrows
    at deref. (b) (deref x ms tval) timeout arity: prim_deref dispatches
    a cljc/deref-timeout deftype method when nargs>=3 (@atom hot path
    untouched). (c) Thread/sleep is fiber-aware: parks on the timer
    wheel inside a fiber (two sleeping futures/go blocks overlap), pumps
    background fibers while waiting on the main thread. (d) realized?/
    future-done?/future-cancel (wins only before first run — cooperative)
    /future-cancelled?/future?/pcalls/pvalues. (e) csp.clj + vendored
    clojure.core.async rebased onto the shared layer (their private
    schedulers deleted; core.async's current-coro atom replaced by
    fiber/*self*, so Thread/sleep and @future inside its go blocks park
    instead of stalling the loop; a/thread is now a concurrent fiber,
    not inline execution). Cross-pump regression tests: <!! runs pending
    futures, main-thread @promise resumes parked go blocks. GOTCHA
    (correct but easy to trip): the bare-@(promise) deadlock error can
    only fire when io-waiters is empty — a live server parked on an
    accept fd means "something could still deliver", so deref blocks
    (real Clojure semantics); the tests.clj concurrency block therefore
    runs BEFORE any server test. pmap deliberately stays = map (fibers
    give no CPU parallelism; eager future-spawning would break lazy
    bigness). Tier 2 (if ever wanted): isolated worker OS threads with
    per-thread heaps + copying channels, Janet ev/thread-style.

47. ~~Windows + macOS portability & CI~~ ✅ done 2026-07-02 — cljc now
    builds warning-free for Linux (gcc), Windows (mingw-w64), and macOS
    x86_64 + arm64 (verified via zig cc cross-compile; real-runner
    verification in CI). macOS fixes: _XOPEN_SOURCE 600 + _DARWIN_C_SOURCE
    before all includes (Darwin's ucontext.h #errors without it; pragma
    push/pop silences the deprecation warnings around the coro engine —
    Darwin still ships and supports the family); st_mtim → st_mtimespec;
    MSG_NOSIGNAL absent → cljc_no_sigpipe() marks every socket/accepted fd
    SO_NOSIGPIPE (peer hangup = EPIPE, not SIGPIPE); _NSGetExecutablePath
    branch in exe-sharedir (declared by hand — mach-o/dyld.h's DYLD_BOOL
    enum collides with the TRUE/FALSE globals); libc.clj drops GNU-only
    get_current_dir_name for a getcwd shim header (macOS lacks it).
    Windows fixes (new code since the June port): nfds_t typedef for the
    WSAPoll shim; __USE_MINGW_ANSI_STDIO 1 + format(__MINGW_PRINTF_FORMAT)
    on cljc_error so %zu works AND warns correctly; stack-floor pointer
    arithmetic moved to uintptr_t (was UB, flagged by mingw -Warray-bounds).
    NEW: (cljc/os*) native → :linux/:macos/:windows/:unknown. tests.clj
    grew capability probes (cljc-test-unix?, cljc-test-coro? — a RUNTIME
    coro/new probe, so a broken-ucontext platform self-skips) gating the
    FFI/JIT/shell/notebook//tmp sections (unix-only) and coro/csp/futures/
    core.async sections; a coro-less Linux build passes 1199/1199 with
    SKIP markers, full build passes all 1263. .github/workflows/ci.yml:
    linux + linux-asan + macos-14 arm64 + x86_64-under-Rosetta + windows
    (MSYS2 mingw64), all -Werror, suite normal + GC stress. .gitattributes
    pins LF. Windows remains degraded by design (no coro/csp/nREPL, plain
    REPL); macOS is FULL-featured — ucontext coroutines verified working on
    arm64 (timing probes: two 50ms futures overlap correctly).
    THE BIG CATCH (found by this CI, reproduced locally with `ulimit -s
    8192`): lazy_force recursed one C frame per lazy-of-lazy chain link —
    (first (filter #(= % 3000000) (iterate inc 0))) (tests.clj's item-43
    head-retention test) builds a ~3M-link chain of thunks that yield
    another lazy, and the walk needed ~90B × chain C stack. Linux's 1 GiB
    rlimit masked it completely; macOS's fixed 8 MiB tripped the guard
    ("stack overflow" abort — which grep -E 'FAIL|error' MISSED, no
    lowercase match); Windows' 2 MiB PE default SEGV'd outright because
    stack_floor_init assumed 8 MiB when rlimit is absent. One bug, three
    disguises. Fixes: (a) lazy_force is now an ITERATIVE two-phase chain
    walk (thunks run down the chain with provisional cached links keeping
    it GC-reachable — gc_mark follows cached unconditionally — then every
    cell back-patched with the realized result; suite passes under a 2 MiB
    hard ulimit now); (b) Windows stack guard uses
    GetCurrentThreadStackLimits for the exact floor (clean catchable
    "stack overflow" instead of SEGV); (c) the two 40ms-sleep overlap
    asserts loosened to 200ms/<350ms (75ms bound flaked on loaded CI
    runners); (d) ffi-build honors CLJC_CC and hashes it into the module
    cache key (Rosetta needs cc -arch x86_64 or dlopen gets an arm64
    dylib); (e) CLJC_UNBUFFERED=1 env var disables stdio buffering (crash
    diagnostics; Windows CRT can't line-buffer). Also: the item-43 note
    "conservative scan finds a stale head pointer" now has a sibling —
    a deep C stack also made every GC collection O(depth) via the
    conservative scan (the arm64 suite burned 10 CPU-minutes before the
    fix; expect normal times after).

48. ~~Bundle entrypoint parity (`-main` + args)~~ ✅ done 2026-08-17 — filed
    and fixed same day, found by comparing against `jolt build` (see
    "Prior art" below). Three defects: (a) `cljc.c` gated `bundle` on
    `argc != 4`, so EVERY flag `bundle.clj` documents (`--static`,
    `--windows`, `--cc=`, `--libs=`, `--cflags=`) was rejected before
    reaching the Clojure side — the header's cross-compilation recipes
    were unrunnable; the gate is now `argc < 3` and bundle.clj owns the
    arg grammar. (b) The generated `main()` had script semantics only —
    a script that is just `(defn -main ...)` bundled into a binary that
    defined a fn and exited silently. Now: after the top-level forms, an
    embedded entry-glue expr auto-calls `-main` (resolve-maybe) with the
    argv strings; an integer return is the exit status; new
    `cljc bundle -m <ns> <out>` bundles a namespace from *load-path*
    (`add-dep!` pulls its transitive requires — verified clojure.string
    rides along) and REQUIRES a `-main`. (c) latent: a bundled script
    that threw still exited 0 — main() now checks `cljc_eval_errored`.
    Bonus: `cljc -m` itself gained the int-exit rule
    (run_subprogram_int_exit; judge keeps its own loop — it exits 2 on
    load errors by contract). e2e regression tests in tests.clj bundle
    with `--cflags=-O0`, which doubles as the flag-path regression test.

49. ~~Agents + STM refs (Tier 3.6)~~ ✅ done 2026-08-17 — the two API gaps
    the jolt comparison surfaced, closed the cheap way the fiber scheduler
    allows (jolt gets them from Chez OS threads; see "Prior art").
    Agents: CljcAgent deftype over a state atom; send enqueues, a drain
    fiber runs actions serially per agent; error modes match Clojure
    (default :fail — handler implies :continue, handler NOT called in
    :fail; failed agent holds its queue for restart-agent); await rides a
    marker action + promise so deref drives the pump. STM: MVCC-lite —
    CljcRef carries {:val :version}; a tx caches first-touch values +
    versions and commit verifies WRITTEN and ENSURED refs, retrying the
    body on mismatch (reads don't conflict = snapshot-ish, write skew and
    all, which is what ensure is for); commute re-applies at commit
    without conflicting; in-tx sends held until commit. THE BUG WORTH
    REMEMBERING: the current tx started as a ^:dynamic var — but binding
    is a GLOBAL stack, so a tx body that yields (sleep/deref/io) leaks
    its binding to whichever context runs next; the main thread's dosync
    silently JOINED the parked fiber's tx (caught by the retry test:
    attempts stayed 1). Fix: cljc/txs map keyed by fiber/*self* (nil =
    main), which item 46 already rebinds around every resume; coros hash
    by identity so they work as map keys. Divergences: no validators;
    sends inside agent ACTIONS dispatch immediately (Clojure holds them);
    watches real on agents/refs, still no-ops on atoms (swap! is a C
    native with no watch registry). Conflict-retry e2e tests use a
    future that sleeps mid-tx while the main thread commits under it —
    deterministic under the cooperative scheduler. ASan gotcha re-found:
    `cljc-asan < tests.clj` "fails" one dbg assert that checks the
    FILENAME in output — stdin mode prints <stdin>; run `cljc-asan
    tests.clj` like the Makefile does.
50. ~~Specter bring-up~~ ✅ done 2026-08-27 — com.rpl/specter 1.1.6 (+
    riddley) runs: 92/92 navigator battery (COMPATIBILITY.md). Library
    bring-up again paid off as a bug-finder: six GENERAL fixes, none
    specter-specific. (1) TOP-LEVEL `do` SPLITTING in eval_toplevel: the
    whole `#?(:clj (do (defmacro defmacroalias ..) (defmacroalias richnav
    ..)))` form was one VM chunk, so the alias call compiled before its
    macro existed — VOP_CALL's late-macro deopt only saves the HEAD; the
    args were already compiled as evaluations ("I don't know what
    `richnav` refers to"). JVM Clojure evaluates top-level do subforms
    one at a time; now so do we. (2) macro_fn_of(): a call head that is
    a CLJC_VAR chain deref'ing to a macro fn IS a macro (Var cells are
    fresh per (var x), so :macro meta can't stick — a Var to a macro fn
    is taken as an alias regardless; the JVM would arity-error otherwise).
    Six check sites unified (compiler, VOP_CALL/TAILCALL via
    IS_MACRO_HEAD, eval, macroexpand-1, macro?). (3) `:bb` READER
    CONDITIONAL feature, priority cljc > bb > default > clj — specter's
    MutableCell is a defrecord under :bb, an imported Java class under
    :clj. (4) EMPTY_VEC SINGLETON (rooted like EMPTY): specter's
    terminal* does (identical? vals []) — JVM `[]` is
    PersistentVector/EMPTY; mk_empty_vec returns it, vec_with_meta copies
    on non-NULL meta (persistent! was the one writer). (5) FN BODIES WITH
    LAZY SPINES returned nil — my reify rewrite built clauses as (cons
    params (drop 2 m)); make_fn's clause walker stepped raw cons tails,
    saw the LAZY cell, and read an EMPTY body. realize_spine() patches
    the spine in place (keeps the **arities** cache on the same cells).
    This one is general and nasty: any macro building a fn body with
    cons/list*/concat hit it silently. (6) REIFY same-name clauses (the
    2- and 3-arg IReduce `reduce`) collapsed to the last in a hash-map;
    now grouped into one multi-arity fn as deftype does; prim_reduce
    dispatches a deftype/reify `reduce` method and dispatch_deftype_method
    reads a reify's :cljc/impls (reify registers in multi-tables, not
    deftype-methods — the memory note about reg-method! applies here
    too). Plus host class mappings (java.util.List/Set, PersistentTree*,
    IReduce, ITransientVector, Cons, IRecord via defrecord derive) and
    (type (transient [])) = :transient-vector. NOT bugs (test-author
    errors to avoid re-chasing): (defnav x [] ..) yields the navigator
    OBJECT, use `x` not `(x)`; NONE-ELEM on a vector APPENDS;
    with-fresh-collected reverts collected vals at the terminal ([2]);
    replace-in's second value must be seqable; walker/MAP-VALS results on
    hash-maps come in hash order (documented divergence). meander
    m/rewrite still open. ALSO FOUND by building ASan at -O1 instead of
    the documented -O0: eval_inner's loop/recur read the sentinel's
    spilled arg array after env_new/env_define collected — `r` was not
    volatile-kept (apply's recur_keep was); -O0 kept `r` on the stack so
    the conservative scan saw it. Lesson: run the ASan sweep at -O1 too
    (`cc -g -O1 -fsanitize=address,undefined ...`); put a `cljc` copy
    next to the ASan binary or the exe-dir subprocess tests exit 127.
    MISDIAGNOSIS TO REMEMBER: meander's m/rewrite "non exhaustive pattern
    match" looked like an alias-resolution bug (an alias-vs-fully-
    qualified A/B even "confirmed" it) — it was THIS GC bug corrupting
    loop rebinding nondeterministically; allocation-pattern changes flip
    such symptoms. When an A/B flips on an innocuous edit, suspect the GC
    first. Separately real and fixed: macro_expand1 now expands under the
    call site's home ns (symc.home_ns) with ErrFrame save/restore of
    cur_reader_ns + the *ns* var — JVM expands at definition time, so
    `*ns*`/`(ns-aliases *ns*)` inside a macro must be the defining ns.
    m/rewrite's REMAINING blocker (not fixed, a deliberate divergence):
    meander registers its pattern parsers under syntax-quoted bare
    symbols (`with, `$) — JVM qualifies an UNRESOLVABLE bare symbol to
    the current ns (meander.syntax.epsilon/with); cljc leaves it bare,
    while lookups use the qualified symbol → `with` parsed as a plain seq
    → "Unbound reference %right". Flipping syntax-quote to qualify
    unresolvable bare symbols (at least inside library loads) is the fix;
    it is held back only because cljc's non-hygienic macro style (bare
    binding symbols in templates, illegal in JVM Clojure) is documented
    as load-bearing — needs a prelude/battery audit first.

## Prior art / inspiration

- **jolt** (<https://jolt-lang.net/>, <https://github.com/jolt-lang/jolt>,
  EPL-1.0 like cljc) — a self-hosted Clojure that compiles to Scheme
  (Chez natively; Gambit → JavaScript) instead of the JVM. Same "Clojure
  without a JVM" target as cljc, opposite strategy: cljc is a tree-walking
  interpreter in one C file that BUNDLES source next to the runtime, jolt
  is a real compiler emitting host-neutral IR → Scheme, AoT-linked into a
  standalone binary with C deps statically linked. Worth stealing from:
  - `jolt build -m <ns>` — entrypoint-based standalone binaries; the
    direct model for roadmap item 48 above.
  - `jolt run -m` with dependency resolution — cljc has `vendor` +
    `*load-path*`, no resolver.
  - Native C FFI and OS-threaded `future`/`agent`/`pmap` as headline
    goals (cljc's concurrency is fiber/coroutine-based — item 46 — so
    it is cooperative, not parallel; jolt is the pressure test for
    whether that ceiling matters).
  Non-goals for cljc: we are not becoming a compiler or growing a second
  host language. The single embeddable `cljc.c` file is the constraint.

## 2026-07 arc: perf campaign, usability, unicode, tooling (summary)

Compressed log of the July push (details in git history, commits 6b964f7..):

- **Perf campaign** (07-01): primitive int64 arrays (CLJC_IARRAY),
  real O(log n) priority-map + O(log n) `(first sorted)`, top-level
  form VM compilation (TAILCALL sentinel trampoline), chunked
  range/repeat/repeatedly. AoC corpus (262 days, local-only advent/)
  605s total. Measurement gotcha: ±5% wall-clock can be pure code
  alignment — A/B with -falign-functions=64 -falign-loops=32 on BOTH.
- **Error UX**: traces reach inside VM-compiled bodies (vm_trace shadow
  stack; NOT address-linked stack locals — publishing &local deopts the
  dispatch loop ~4%), tail calls REPLACE their frame (g_tc_form handoff),
  frames carry file:line (:file meta via rd_file_cell, ErrFrames
  save/restore reader state), caret suppressed for other-file frames,
  did-you-mean + "(require 'the.ns) first" hints, val_desc names scalar
  values in type errors, stack overflow hints loop/recur.
- **Discovery/REPL**: docs.clj battery (~240 entries incl. special forms,
  arglists for natives), (source f), (dir ns), (dbg expr)/#p (real &form
  binding at all 3 macro-expansion sites), *e/(pst), trace-vars, Ctrl-R
  search, paren-depth continuation indent, pretty-printed wide results
  (pp_to fill layout), *print-length* REPL default + seq1 lazy printing
  (to_seq in print paths was the infinite-print hang), Ctrl-C interrupts
  eval (g_interrupt in eval()/VOP_CALL), named #<fn f [x]> printing.
- **Tooling**: cljc watch (mtime rerun), cljc doctor (load-path triage),
  cljc vendor / (vendor! "src") — Clojars release jar or GitHub clone →
  ./vendor/ + try-require report; nREPL completions/lookup (+cider
  aliases); exe-dir on *load-path* (repo builds work from any cwd).
- **Interop shims**: epoch-ms java.time.Instant (+ISO-8601 via Hinnant
  civil_from_days), DateTimeFormatter/ISO_INSTANT, real v4 random-uuid,
  String./getChars/char-arrays, Long|Double/valueOf, class-name→type-
  keyword defs + derives so (extend Cls Proto ..) dispatches. Acceptance:
  vendored clojure.data.json round-trips unmodified.
- **Unicode**: strings index by CODEPOINT (UTF-8 storage; strx {nbytes,
  lazily cached ncp} union view; ASCII O(1) fast path; latin-1 fallback
  for invalid bytes) and the regex engine matches codepoints (int32
  literals, unicode class ranges, all scan loops step by codepoint).
  Divergence: astral = 1 char (JVM: 2 UTF-16 units). Differentially
  fuzzed vs babashka: 1200 BMP string/regex exprs, 0 divergences;
  self-consistency invariants hold under GC stress (fuzz/ in repo).
- **Windows parity**: the real line editor (SetConsoleMode raw + VT
  in/out driving the same loop; fgets fallback), console width, Ctrl-C
  via SetConsoleCtrlHandler. windows.h TRUE/FALSE are #undef'd — Win32
  callbacks return 1/0.
- **Battery bugs found by the tooling**: (require 'process) self-capture
  infinite tail-loop (bare `sh` inside ns process = process/sh itself —
  now clojure.core/sh), mkdir-p was single-level, bigint("123") returned
  the first char's codepoint, Double/isNaN could not work ((= x x) is
  identity-true; num_cmp treats NaN equal-to-all → cljc/nan?* native).

## 2026-08 arc: Jolt-inspired batch (summary)

Four items picked from comparing against Jolt (Clojure-on-Chez), adapted to
cljc's niche:

- **JIT type hints** (jit.clj + reader): the reader keeps `^Tag` hints as
  static `{:tag Tag}` metadata on SYMBOL cells (fresh per read, so invisible
  to eval/=; non-symbol hints still discarded — `^String (foo)` in expression
  position must not become a runtime with-meta). jit.clj gained a two-type
  system (:long/:double): param types from `^long`/`^double` hints, return
  type from a hint on the fn name or body inference (two-pass fixpoint for
  self-recursion — the lattice is monotone so one promotion pass converges).
  New in the subset: `/` (double-only), `Math/sqrt`, `(double x)`/`(long x)`,
  unary minus (was silently `dst = t1`). GOTCHA that shaped the design:
  defaulting an unhinted return to :long silently truncated double bodies —
  interpreted/compiled agreement is the contract, so inference, not default.
- **Conformance corpus** (fuzz/conformance.txt): 412 curated clojure.core
  exprs diffed against a JVM-verified golden (`make conformance`, in `make
  test`; re-derived live when bb is on PATH). Ground rules: deterministic,
  print unordered colls only through sorted views, skip documented
  divergences. Caught on first run: `(partition-all n step c)` and 3-coll
  `interleave` missing, `.toUpperCase`/`.toLowerCase` unshimmed,
  `rationalize` was identity (now exact ratio from the shortest decimal
  repr), and double `range` promoting its first element — JVM semantics are
  element type = start/step promotion only, a double end only bounds
  (`(range 2.5)` => `(0 1 2)`).
- **vendor deps.edn** (prelude): `cljc vendor` with no arg resolves ./deps.edn
  :deps transitively — pinned :mvn/version via Clojars/Maven Central then the
  jar's poms (compile/runtime scope; property-versioned/optional/clojure
  skipped; Java-only jars tolerated with a note), :git/url+:git/sha|tag via
  clone (io.github/com.github names infer the URL; :deps/root honored),
  :local/root by copy; git/local deps recurse into their own deps.edn.
  First coordinate wins. Maven repos live in the cljc/vendor-repos* atom so
  tests point it at a file:// layout — the tests.clj integration test builds
  a throwaway m2 repo + git repo + local project and runs a SUBPROCESS cljc
  (so the repo's own vendor/ stays clean).
- **bundle --library** (bundle.clj): `--library` emits a shared library plus
  a generated <out>.h — cljc_lib_init/cljc_lib_eval/cljc_lib_last_error over
  the embedded script, results as pr-str strings owned by the library.
  cljc_eval_string anchors the conservative GC at its own frame per call, so
  hosts call from any stack depth; errors are captured from cur_exc/err_msg
  and the interpreter survives. Also fixed: the C CLI's `argc != 4` guard
  had been rejecting ALL bundle flags (--static/--windows/--cc) — now >= 4.

## Known divergences from Clojure (deliberate)
- `catch` is untyped; hash maps/sets iterate in hash order; plain int64
  arithmetic wraps (see README's list — it supersedes this one)
- Namespaces: flat global + per-ns aliases behind a symbol-based namespace
  layer; top-level `user` defs are bare, hence visible from other
  namespaces; no remove-ns/ns-unmap (root Bindings are append-only)
- Quasiquote: auto-gensym `x#` works; nested quasiquote levels do not
- Conservative GC may retain garbage pointed at by stale stack slots

## Testing conventions
- `tests.clj` — every bug fix gets a regression assertion (assert= prints
  PASS/FAIL; script mode aborts on first `error:`)
- Verify sweeps: `make test`, then ASan build:
  `cc -g -fsanitize=address,undefined -o /tmp/cljc-asan cljc.c` with the
  ASAN_OPTIONS above, both normal and stress.
- Memory bound check: 2M-iteration churn loop should stay ~5 MB RSS.
