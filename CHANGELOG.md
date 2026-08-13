# Changelog

## Unreleased

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
