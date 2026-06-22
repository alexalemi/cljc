# cljc examples

A suite of small, self-contained programs that each exercise a different
corner of the language. Run any of them from the repo root:

```sh
cljc examples/mandelbrot.clj
```

For a browsable gallery (every example run, output captured, source
syntax-highlighted) served over HTTP:

```sh
cljc examples/serve.clj        # → http://127.0.0.1:8088
cljc examples/serve.clj 9000   # custom port
```

The gallery server is itself a cljc program — it uses the built-in `tcp/*`
primitives and shells out to `cljc` to run each example.

## The programs

| File | What it is | Features it shows off |
|---|---|---|
| `mandelbrot.clj` | ASCII Mandelbrot set | double math, `loop`/`recur` hot loops |
| `life.clj` | Conway's Game of Life | persistent sets as state, `frequencies` |
| `primes.clj` | Infinite primes, Collatz, Fibonacci | `lazy-seq`, infinite `iterate`/`lazy-cat`, `take-while` |
| `nqueens.clj` | N-Queens solver | recursive backtracking, `mapcat` search |
| `sudoku.clj` | Sudoku solver | backtracking + the most-constrained heuristic, `clojure.set` |
| `calc.clj` | Arithmetic expression evaluator | recursive-descent parsing, regex tokenizing |
| `brainfuck.clj` | A complete Brainfuck interpreter | mutable arrays (`int-array`/`aset`), hosting a language |
| `macros.clj` | `unless`, `dbg`, `my-and`, `infix` | `defmacro`, quasiquote, `gensym` hygiene |
| `shapes.clj` | Areas & FizzBuzz | protocols, multimethods, records — all three dispatch styles |
| `dijkstra.clj` | Shortest paths in a road network | maps as graphs, immutable state through `loop` |
| `wordfreq.clj` | Word-frequency CLI | regex, `frequencies`, `*args*`, file *or* stdin |
| `bank.clj` | A tiny bank ledger | `atom`/`swap!`, `ex-info`/`try`/`catch` |
| `ffi-demo.clj` | Calling libm & libc | `ffi/define` — C signatures as data (needs `cc`) |
| `sqlite.clj` | A real SQLite database | binding libsqlite3 via a shim header; prepare/step/column |
| `raylib-bounce.clj` | Interactive bouncing balls | the raylib GUI binding — `:float`/struct-by-value FFI (needs raylib + a display) |
| `fractal-svg.clj` | A recursive fractal tree | recursion, trig, writing a real `.svg` file with `spit` |

Plus two that aren't scripts:

| File | What it is |
|---|---|
| `host.c` | Embedding cljc in a C program (`#define CLJC_NO_MAIN`); build with `make example` |
| `hello-bundle.clj` | A script meant for `cljc bundle.clj` → a standalone binary |

## Notes on the dialect

A few of these examples bumped into deliberate divergences from JVM Clojure,
worth knowing if you write your own:

- **`nth` doesn't index strings** — use `subs`/`get`, or `(vec s)` first.
- **`Math/PI` and `Math/E` are real constants**, but they're *values*, not
  functions — write `Math/PI`, never `(Math/PI)`.
- **No `double` cast fn** — force a double with `(* 1.0 x)` (and `/` already
  yields a double for inexact division, since there's no Ratio type).
- **`extend-type` takes a type keyword and method impls directly**, with no
  protocol-name argument; records get keywords like `:Circle` from `type`.
- **Quasiquote has no auto-gensym** (`x#`) — call `gensym` explicitly when a
  macro needs a fresh name.
