# Changelog

## Unreleased

### Added
- **Specter runs** (`com.rpl/specter` + `riddley`): see COMPATIBILITY.md for the
  92-case battery. The bring-up fixed six general interpreter issues below.
- Reader conditionals honor the `:bb` feature (priority `:cljc` > `:bb` >
  `:default` > `:clj`): a library's babashka branch is its JVM-class-free
  path, which is what cljc can run.
- Host class names for protocol dispatch: `java.util.List`/`Set`,
  `clojure.lang.PersistentTreeMap`/`PersistentTreeSet`, `IReduce`,
  `ITransientVector`, `Cons`, `IRecord` (every `defrecord` type derives it),
  `Sorted`; sorted maps/sets derive the map/set interfaces.
  `(type (transient []))` is `:transient-vector` (was `:unknown`).

### Fixed
- A top-level `(do ...)` evaluates its subforms one at a time, as JVM Clojure
  does: `(do (defmacro m ..) (m ..))` — e.g. a `#?(:clj (do ...))` body — saw
  the macro use compiled before the `defmacro` ran (the head deopted at
  runtime but its args had been compiled as evaluations: "I don't know what
  `richnav` refers to").
- Macro aliases through a Var work: `(def alias (var m))` +
  `(alter-meta! #'alias merge {:macro true})` (specter's `defmacroalias`) now
  expands as a macro everywhere (tree-walker, VM, `macroexpand-1`, `macro?`);
  it used to be called as a plain fn returning the expansion unevaluated.
- The empty vector is a singleton like `PersistentVector/EMPTY`:
  `(identical? [] (vector))`, `(empty v)`, `(pop [x])`,
  `(persistent! (transient []))` all hold (specter's `terminal*` relies on
  it). Metadata on an empty vector takes a private copy.
- `fn` bodies whose list spine ends in a lazy seq — macro-built with
  `(cons params (drop 2 m))`, `list*`, `concat` — evaluated to **nil**: the
  clause/body walkers stepped raw cons tails and read the lazy tail as an
  empty body. Spines are realized in place (the arity cache survives).
- `reify` with several clauses of the same method name (IReduce's 2- and
  3-arg `reduce`) kept only the last; they now form one multi-arity fn as in
  `deftype`. `reduce`, `into` and `transduce` honor a reify/deftype's own
  `reduce`; the C-side deftype method dispatch (`count`/`nth`/`get`/…) also
  finds methods on reify instances.
- **GC**: the tree-walker's `loop` did not root its `recur` sentinel across
  the rebinding allocations; an optimizing build could keep only the pointer
  into the sentinel's heap-spilled argument array (invisible to the
  conservative scan) and the sweep freed it mid-iteration (ASan `-O1`
  heap-use-after-free; `-O0` hid it). Now `volatile`-kept like `apply`'s.
- Macros expand under the namespace their call site was *written* in. cljc
  expands lazily (first call of the enclosing fn), so a macro that reads
  `*ns*` or `(ns-aliases *ns*)` inside a library fn first called from `user`
  saw `user` — JVM Clojure expands at definition time. `*ns*` is restored
  afterwards, including when the expansion throws.

## v0.3.0 — 2026-08-27

### Added
- **Agents**: `agent`/`send`/`send-off`/`send-via`, `await`/`await-for`,
  `agent-error`/`restart-agent` (`:clear-actions`; a failed agent holds its
  queue), error modes (`:fail` default, `:continue` with `:error-handler`),
  `release-pending-sends`, `shutdown-agents`. Actions run serially per agent
  on a drain fiber (needs coroutines at runtime, like `future`).
- **STM**: real `ref`/`dosync`/`alter`/`ref-set`/`commute`/`ensure` replacing
  the atom-backed shims — MVCC-lite with per-ref versions: commit verifies
  written and `ensure`d refs and retries the body on conflict (possible when
  a transaction yields — sleep/deref/io — while another fiber commits);
  `commute` re-applies on the latest value without conflicting; sends inside
  a transaction are held until commit; `io!` throws inside a transaction;
  nested `dosync` joins the outer transaction. `alter`/`ref-set`/`commute`/
  `ensure` outside `dosync` now throw (they used to silently mutate).
  The current transaction is fiber-local (keyed by the fiber, not a dynamic
  var — a `binding` would leak across yields to other fibers).
- **Watches**: `add-watch`/`remove-watch` are real on agents and refs
  (fired per action / at commit); still no-ops on atoms.
- `cljc bundle` entrypoint parity with `jolt build -m` / `bb -m`: the bundled
  binary now calls the script's `-main` with the command-line args when one
  is defined (top-level forms still run first; scripts without `-main` are
  unchanged), and an integer return from `-main` becomes the exit status.
  New `cljc bundle -m <ns> <out>` bundles a namespace from `*load-path*`
  (plus its transitive requires) and entrypoints its `-main`. `cljc -m` now
  honors the integer-exit rule too.
- **`cljc bundle --library`**: build a script into a shared library
  (`.so`/`.dylib`/`.dll`) with a C ABI instead of an executable —
  `cljc_lib_init()` runs the embedded script, `cljc_lib_eval(src)` returns
  `pr-str` of the last form (`NULL` on error, interpreter survives),
  `cljc_lib_last_error()` reports; a matching `<out>.h` header is generated
  for C/C++/Rust/Zig hosts (see `examples/libhost.c`).

- **`cljc vendor` understands deps.edn**: `cljc vendor` with no argument (or
  a deps.edn path) resolves the `:deps` map transitively without Java or
  `~/.m2` — pinned `:mvn/version` jars from Clojars/Maven Central followed
  through their poms (compile/runtime scope, property-versioned/optional/
  `org.clojure/clojure` skipped, Java-only jars tolerated), `:git/url` +
  `:git/sha`/`:git/tag` clones (URL inferred for `io.github.*`/`com.github.*`
  names, `:deps/root` respected), `:local/root` copies; git/local deps
  recurse into their own deps.edn. First coordinate wins; everything lands in
  `./vendor/` and gets the usual try-each-namespace load report. Maven
  mirrors (incl. `file://`) via the `cljc/vendor-repos*` atom.

- **Conformance corpus**: `fuzz/conformance.txt` — 412 curated clojure.core
  expressions whose printed value must match JVM Clojure exactly, diffed
  against a golden file by `make conformance` (part of `make test`; verified
  live against babashka when `bb` is on PATH). Already caught and fixed five
  divergences: `(partition-all n step coll)`, 3-coll `interleave`,
  `.toUpperCase`/`.toLowerCase`, `rationalize` (was identity; now derives
  exact ratios from the shortest decimal repr), and `range` element types
  (start/step promotion decides element type, a double end only bounds —
  `(range 2.5)` is `(0 1 2)`, `(range 0 2 0.5)` starts with int `0`).

- **JIT type hints + doubles**: the reader now keeps `^Tag` hints as
  `{:tag Tag}` metadata on symbols (read-time, like Clojure; still discarded
  on non-symbol forms), and `jit.clj` honors JVM-style `^long`/`^double`
  hints on params and the fn name — hinted functions compile to unboxed
  `double`/`long long` machine ops, with the return type inferred from the
  body when unhinted. Also new in the JIT subset: `/` (double division),
  `Math/sqrt`, `(double x)`/`(long x)` casts, unary minus.

### Fixed
- `compare` on vectors orders by count first, then element-wise, like Clojure
  (`(compare [2] [1 10])` is -1; `(sort [[2] [1 10] [1 2]])` puts `[2]` first)
- **STM**: `commute` on a ref also `alter`ed/`ref-set` in the same transaction
  was applied twice at commit (`(dosync (alter r inc) (commute r inc))` on
  `(ref 0)` gave 3, not 2, and fired watches twice); commutes now skip refs in
  the write set, as in Clojure.
- `cljc bundle` rejected every flag `bundle.clj` documents (`--static`,
  `--windows`, `--cc=`, `--libs=`, `--cflags=`) — the C dispatcher demanded
  exactly two arguments, so cross-compilation was unreachable from the CLI;
  a bundled script that threw still exited 0 (now 1)
- **Memory safety**: int-array bounds checks truncated 64-bit indices to 32
  bits (`(aget a 4294967296)` and `(get a 4294967296)` read/wrote out of
  bounds — segfault or heap corruption); printing bigints over ~280 digits overflowed a heap buffer
  (`(str (apply *' (range 1 201)))`); a source file ending right after `^`
  crashed the reader (NULL deref)
- `binding` now installs in parallel like Clojure (inits can't see earlier
  new values) and no longer permanently leaks earlier bindings when a later
  init throws or a var fails to resolve
- Deftype method dispatch for `.length`/`.charAt`/`.subSequence`/`.close` was
  dead — a `cljc/dt-method` redefinition had swapped its parameters, so e.g.
  `with-open` never called a deftype's own `close`; unified into one
  definition reading `cljc/deftype-methods`
- `record?` no longer returns true for deftype instances (consults the
  `cljc/record-types` registry the rest of the file already maintains)
- Regex: `(?i)`/`(?m)`/`(?u)` now raise "unsupported flag" instead of being
  silently stripped (wrong matches); a nested regex inside a `str/replace`
  replacement fn no longer clobbers the outer pattern's `(?s)` flag;
  `str/split`/`replace`/`replace-first` only treat a string as a pattern when
  it carries the `:regex` meta tag, not any metadata
- `read-line` no longer silently splits lines longer than 4095 bytes
- Conformance corpus: `(- 1/2 1/2)` was in the golden as `0` while JVM Clojure
  gives `0N` (ratio→integer collapse yields a BigInt; cljc's small-N demotion
  is a documented divergence, so the case is replaced by `(- 3/2 1/4)`); the
  babashka re-verification in `make conformance` now fails on disagreement
  instead of being swallowed
- `(keyword nil "a")` returns `:a` (was an error), matching `symbol`
- `(int-array 4294967296)` errors instead of silently making a 0-length array
- Under-arity native calls (`(atom)`, `(mod 5)`, `(subseq ss)`, …) raise
  "wrong number of args" instead of reading stale stack slots
- `.contains` works on the `HashSet.`/`HashMap.` atom shims again (a later
  string-oriented redefinition had dropped the atom branch)
- `cljc watch`: `$`/`` ` `` in file names no longer shell-expand
- bundle: non-ASCII scripts embed as UTF-8 bytes, not codepoints (bundled
  binaries with unicode literals were corrupted); new `cljc/str-bytes*` /
  `cljc/str-nbytes*` natives expose byte-level string access
- http.clj: `Content-Length` is now a byte count (non-ASCII bodies were
  mis-framed/truncated on both server and client side)
- json.clj: `\uXXXX` escapes decode fully, including surrogate pairs (were
  always replaced with `?`)
- jit.clj: `(- x)` compiled to identity (dropped the negation); chained
  comparisons `(< a b c)` compiled to C's `t1 < t2 < t3` instead of
  pairwise-AND
- `csp/merge` with no input channels closes its output immediately
- Removed nine dead duplicate prelude definitions (later defn silently won);
  fixed `process/sh`, `process/shell`, `fs/list-dir`, `http/serve`,
  `http/run-server`, `json/parse` docstrings that described behavior the
  code doesn't have

## v0.2.0 — 2026-07-07

First tagged release. Everything below shipped since the version string was
minted; highlights only — `PLAN.md` carries the engineering log.

### Language & runtime
- Bytecode VM for hot functions and top-level forms, proper tail calls,
  primitive int64 arrays (`int-array`/`aget`/`aset`), chunked lazy
  `range`/`repeat`/`repeatedly`, real `clojure.data.priority-map`
- **Codepoint-indexed strings** (UTF-8 storage, O(1) ASCII fast path) and a
  **codepoint-aware regex engine** — unicode class ranges, astral chars are
  one char (documented divergence from JVM UTF-16). Differentially fuzzed
  against babashka: 1,200 string/regex expressions, zero divergences
- Fiber-backed concurrency: `future`/`promise`/`Thread/sleep` +
  `clojure.core.async` (`go`/`<!`/`>!`/`alts!`) on one cooperative scheduler
- JVM interop shims: epoch-backed `java.time.Instant` with real ISO-8601,
  `DateTimeFormatter/ISO_INSTANT`, v4 `random-uuid`, boxed-number methods,
  class keys for `(extend Cls …)` — vendored `clojure.data.json` runs
  unmodified

### Errors & debugging
- Uncaught errors show the source line with a caret, a `file:line` trace that
  reaches **inside** compiled fn bodies, typo suggestions, and — when an
  unresolved symbol's namespace exists on the load path — the actual fix:
  `try (require 'clojure.set) first`
- Type errors name the value (`expected a number, got a keyword: :kw`);
  stack overflows suggest `loop/recur`
- `(dbg expr)` / `#p expr` print `#dbg file.clj:12 (* x x) => 49` and pass the
  value through; `trace-vars`/`untrace-vars` give depth-indented call traces;
  `*e` holds the last REPL error and `(pst)` reprints it

### REPL
- `(doc x)` with arglists (natives and special forms included), `(source x)`,
  `(dir ns)`, `apropos`, tab completion, Ctrl-R history search, paren-depth
  continuation indent, pretty-printed wide results, numbered results
  (`*1 *2 *3`, `(*results* n)`)
- **Ctrl-C interrupts evaluation** instead of killing the session; printing an
  infinite seq shows `(0 1 … 99 ...)` instead of hanging
- Windows gets the full line editor (Windows 10+ VT console; plain-line
  fallback)

### Tooling
- `cljc vendor <coord>` / `(vendor! "user/repo")` — fetch a pure-Clojure
  library from Clojars (release jar) or GitHub into `./vendor/` and report
  which namespaces load
- `cljc watch file.clj` (rerun on save), `cljc doctor` (load-path triage),
  `cljc nrepl` with nREPL-0.8 `completions`/`lookup` (+ CIDER aliases),
  notebook mode, `cljc bundle` (script + runtime → one native binary)

### Platforms
- CI-tested on Linux (+ ASan/UBSan), macOS arm64 + x86_64 (Rosetta), and
  Windows (mingw-w64); the suite runs twice everywhere (normal + GC stress)
- Tagged releases ship portable archives per platform: unpack anywhere and
  run — batteries and vendored libraries resolve relative to the binary
