# Changelog

## Unreleased

### Fixed
- **Memory safety**: int-array bounds checks truncated 64-bit indices to 32
  bits (`(aget a 4294967296)` read/wrote out of bounds — segfault or heap
  corruption); printing bigints over ~280 digits overflowed a heap buffer
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
