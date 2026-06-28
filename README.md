# cljc

A small Clojure in a single C file. Inspired by [Janet](https://janet-lang.org/)'s
embeddability, aimed at [babashka](https://babashka.org/)-flavored scripting and
beyond: persistent collections, macros, destructuring, protocols, ratios,
namespaces, coroutines, regex — in ~12,000 lines of C99 with no dependencies.

```clojure
(defn top-words [text n]
  (->> (re-seq #"\w+" (str/lower-case text))
       frequencies
       (sort-by (fn [[_ c]] (- c)))
       (take n)))

(doseq [[word count] (top-words (slurp "moby-dick.txt") 5)]
  (println (format "%-12s %d" word count)))
```

It's grown well past a scripting toy: real libraries run on it unmodified —
[Emmy](https://github.com/mentat-collective/emmy) (a computer-algebra system),
[DataScript](https://github.com/tonsky/datascript) (Datalog),
[core.logic](https://github.com/clojure/core.logic) (miniKanren),
[core.async](https://github.com/clojure/core.async),
[hiccup](https://github.com/weavejester/hiccup),
[instaparse](https://github.com/Engelberg/instaparse), and ~30 of babashka's
built-in namespaces. See [Running real libraries](#running-real-libraries).

## Quick start

```sh
make            # builds ./cljc (cc, -lm -ldl, nothing else)
./cljc          # REPL: editing, history, tab-completion, highlighting, *1 *2 *3
./cljc file.clj # run a script
make test       # 1000+ assertions, run twice: normal + GC-stress mode

./install.sh                      # install to ~/.local (no sudo)
PREFIX=/usr/local ./install.sh    # system-wide
```

Installed, `cljc` works from anywhere: batteries (`json.clj`, `fs.clj`,
`process.clj`, …) and vendored libraries (`clojure.set`, `clojure.tools.logging`,
`nextjournal.markdown`, medley, …) resolve through `$PREFIX/share/cljc`, with
`CLJC_PATH` (colon-separated) for extra roots. `cljc --version` tells you what
you have.

## What's inside

| Area | Coverage |
|---|---|
| **Numbers** | int64, **bignums** (`123…890N` literals and the auto-promoting `+'`/`-'`/`*'`), doubles, **ratios** (`3/4`, `(/ 22 7)` ⇒ `22/7`, `numerator`/`denominator`) |
| **Lazy seqs** | `lazy-seq`, infinite `range`/`iterate`/`repeat`/`cycle`/`repeatedly`, lazy `map`/`filter`/`take`/`concat`, true call-by-need, 32-element chunking |
| **Data** | nil, bools, strings (escapes), symbols, keywords, lists, **persistent vectors** (32-way tries + tail, transients `transient`/`conj!`/`assoc!`/`persistent!`), **persistent maps** (HAMT), **persistent sets**, **sorted maps/sets** (weight-balanced trees: `subseq`/`rsubseq`/`sorted-set-by`), atoms |
| **Special forms** | `quote if do def defn defmacro let fn loop recur and or when cond try/catch/finally quasiquote var` |
| **Macros** | `defmacro` + quasiquote (`` ` `` `~` `~@`, splices into lists/vectors/maps/sets), **auto-gensym `x#`**; `->` `->>` `some->` `cond->` `as->` `case` `condp` `for` `doseq` `dotimes` `doto` `letfn` `with-out-str` `with-redefs` … most written in cljc itself (the prelude) |
| **Functions** | multi-arity `(fn ([x] …) ([x y] …))`, variadic `& rest`, full destructuring (`[a b & r :as v]`, `{:keys [x] :or {…} :as m}`), `:pre`/`:post` conditions |
| **Namespaces & vars** | per-namespace `:as`/`:refer` aliases, first-class **vars** (`#'x`, `var?`, `resolve`, `alter-var-root`, `with-redefs`), a real `user` namespace at the top level, `*ns*` |
| **Polymorphism** | `defmulti`/`defmethod`, `defprotocol`/`extend-type`/`extend-protocol`/`satisfies?`, **`deftype`/`defrecord`** with real method dispatch (this is what lets Emmy/DataScript/instaparse run), `binding` |
| **Errors** | `throw` any value, `try`/`catch`/`finally`, `ex-info`/`ex-message`/`ex-data`; interpreter errors are catchable. Friendly messages: arity errors name the fn + accepted arities, type errors name the offending value's type |
| **I/O streams** | `print`/`println`/`pr`/`prn` route through the **`*out*`** var; bind it to capture (`with-out-str`) or to `*err*` for stderr; `slurp`/`spit`, string readers/writers |
| **Regex** | self-contained backtracking engine: `\d \w \s`, classes, groups, `(?:…)`, lookahead `(?=)`, alternation, `{n,m}` quantifiers, lazy quantifiers, **backreferences `\1`…`\9`**; `#"…"` literals; `re-find` `re-matches` `re-seq` `re-replace` (`$1` refs) `re-split`; guarded against catastrophic backtracking |
| **Transducers** | `map`/`filter`/`take`/`mapcat`/… as transducers, `transduce`, `eduction`, `into` with an xform — compose over infinite seqs with `reduced` early-exit |
| **Concurrency** | stackful **coroutines** (`coro/new`/`resume`/`yield`) — the substrate for a genuine `clojure.core.async` (`go`/`<!`/`>!`/`alts!`, channels, timeouts) on a single thread |
| **Library** | hundreds of core fns: seq ops (`map filter reduce group-by frequencies partition …`), string ops, `format`, `sort`/`compare`, `read-string`/`eval`, `Math/*` |
| **Memory** | mark-and-sweep GC over pooled cells, conservative C-stack scanning (interpreter C code needs no root registration), structural sharing throughout, adaptive collection floor for high-churn workloads |
| **Execution** | tree-walking evaluator with a **bytecode VM** for hot functions, plus proper tail calls (general TCO, beyond Clojure) and `trampoline` |
| **Modes** | REPL, script file, piped stdin, embedded C library, **nREPL server** |

## Editor integration (nREPL)

```sh
./cljc --nrepl        # serves nREPL on 127.0.0.1:7888, writes .nrepl-port
./cljc --nrepl 7999   # custom port
```

Speaks bencode nREPL (`clone` `describe` `eval` `load-file` `close`
`interrupt` `ls-sessions`), so Conjure, CIDER, and Calva can connect:
evaluation results come back as `value`, `println` output as `out`
messages, errors as `err`. Definitions persist across evals. One client
at a time, loopback only.

## JIT: compile hot functions to native C

```clojure
(load-file "jit.clj")
(jit/defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(jit/compile! 'fib)    ; generate C → cc → dlopen → swap the binding, live
(fib 32)               ; 1.24 s interpreted → 6 ms native (faster than bb/JVM)
```

`jit.clj` (~150 lines of cljc) compiles a numeric subset — `if`, `let`,
`loop`/`recur`, self-recursion, integer arithmetic/comparisons — to unboxed
`long long` C through the same generate→`cc`→`dlopen`→rebind pipeline as the
FFI. Compiled modules are content-cached, so warm compiles are a `dlopen`.
Outside the subset, `jit/compile!` errors cleanly and the interpreted
version stays. fib(32): **6 ms** vs babashka's 540 ms and JVM Clojure's
630 ms — it's real machine code.

## FFI: call C from Clojure

```clojure
(ffi/define [[:double cos [:double]]] {:headers ["math.h"] :libs "-lm"})
(cos 0.0)   ;=> 1.0
```

s7/cload-style: declare C signatures as data, cljc generates glue, compiles a
`.so` at runtime, `dlopen`s it, and the C function becomes a cljc fn. Modules
cache by content hash; `ffi/defstruct` generates struct accessors. The battery
shelf builds on it: `libc.clj` (the libc surface — `getpid`, `cwd`, `env`…),
`fs.clj` (babashka.fs-flavored, `list-dir` via real `readdir`), `process.clj`
(`sh`/`shell`/`out`), `json.clj`, `http.clj`, plus `(sh "cmd")` ⇒ `{:exit :out}`.

## Standalone binaries

```sh
./cljc bundle.clj myscript.clj mybinary       # one ~180 KB executable, no deps
./cljc bundle.clj --static myscript.clj out   # static: no glibc dep, best to share
./cljc bundle.clj --windows myscript.clj out.exe   # cross-compile a Windows .exe
```

`bundle.clj` embeds your script — plus every `.clj` it transitively `require`s
or `load-file`s (batteries, vendored libs) — next to the runtime and compiles
the result: a genuinely self-contained binary that runs anywhere, no `vendor/`
or share dir needed. Janet-style deployment. Script arguments arrive as
`*args*`. This is bundling, not compilation: the script still runs on the
interpreter inside.

A bundle is native code for the platform that built it, so match your friend's
machine. Flags steer the compiler:

| target | command | needs |
|---|---|---|
| same Linux/arch | `bundle script out` | nothing extra |
| portable Linux | `bundle --static script out` | removes the `GLIBC_2.xx` failure mode |
| Linux ARM64 | `bundle --cc=aarch64-linux-gnu-gcc --static script out` | `gcc-aarch64-linux-gnu` |
| Windows | `bundle --windows script out.exe` | `mingw-w64` (`x86_64-w64-mingw32-gcc`) |

`--cc=`, `--libs=`, and `--cflags=` (plus the `CLJC_CC` env var) expose the
compiler directly for any other cross toolchain, e.g. `zig cc`. On Windows the
interactive REPL drops to plain line input and the nREPL server is unavailable —
script execution and bundles are unaffected.

## Running real libraries

`require` resolves namespaces against `*load-path*` (`.`, `vendor/`, the share
dir) and `CLJC_PATH`, loads `.clj`/`.cljc` files once, and registers per-namespace
`:as`/`:refer` aliases. Each namespace's defs resolve own-namespace-first, so
multi-file libraries with internal cross-references load in isolation — `deftype`,
`defrecord`, and protocols are real, which is what unlocks the heavy hitters.

**Bundled** — `require` works out of the box (vendored under the share dir):

| | |
|---|---|
| `clojure.set` `clojure.string` `clojure.walk` `clojure.zip` `clojure.edn` | core utilities, complete |
| `clojure.data` `clojure.datafy` `clojure.math` `clojure.pprint` `clojure.stacktrace` | |
| `clojure.test` | `deftest`/`is`/`testing`/`are`/`run-tests` |
| `clojure.core.async` | `go`/`<!`/`>!`/`alts!`, channels, timeouts (coroutine-backed) |
| `clojure.core.match` | pattern matching |
| `clojure.data.json` / `cheshire.core` | JSON read/write |
| `clojure.tools.logging` / `taoensso.timbre` | console loggers (to `*err*`) |
| `nextjournal.markdown` | markdown → hiccup (pure-Clojure CommonMark subset) |
| `babashka.fs` `babashka.process` `babashka.cli` `bencode.core` | scripting batteries |
| medley, edamame | |

**Runs with the library's own source on `CLJC_PATH`** (clone it, point cljc at
`src`) — these exercise deftype, multimethods, multi-file graphs, the lot:

| library | what runs |
|---|---|
| **Emmy** | symbolic + numerical calculus, `simplify`, `D` (autodiff), Lagrangian/Hamiltonian mechanics |
| **DataScript** | `q`/`pull`/`transact!` — Datalog over an in-memory db |
| **core.logic** | `run*`/`conde`/`membero`/`appendo` — relational/logic programming |
| **hiccup** / **hiccup2** | HTML generation, CSS sugar, escaping |
| **instaparse** | context-free grammars → parse trees |
| **SCI** | a Clojure interpreter, running *inside* cljc |
| **clojure.data.csv** / **clojure.tools.cli** | CSV, command-line parsing |
| **test.check** | generative property testing |

**Not yet** — these need a JVM library reimplemented from scratch (a real XML/YAML
parser, a host serializer): `clojure.data.xml` (StAX), `clj-yaml` (SnakeYAML),
`cognitect.transit`, `core.rrb-vector`. The line: pure-Clojure libraries — even
big, multi-file, deftype-heavy ones — are reachable; libraries that lean on a
specific Java library for their core work are not, until that library is ported.

## Tooling

- **Linting**: ships a `.clj-kondo/config.edn` tuned for the dialect.
  `clj-kondo --lint your.clj` works in this repo; name files `.cljc` if they use
  `#?(:cljc …)` reader conditionals. For runtime-defined vars (`ffi/define`), add
  a `(declare …)` like `libc.clj` does.
- **Tests**: `clojure.test` is bundled — `deftest`, `is` (with `thrown?`),
  `testing`, `run-tests`.
- **Editor/LSP**: `./cljc --nrepl` for eval (Conjure/CIDER/Calva); clojure-lsp's
  static features work on cljc files via the same clj-kondo analysis.

## Embedding

The model is stb-style: define `CLJC_NO_MAIN` and `#include "cljc.c"` into one
translation unit. Your native functions then have the interpreter's value
constructors in scope, which is what you need to exchange data:

```c
#define CLJC_NO_MAIN
#include "cljc.c"

static Cljc *native_add(CljcEnv *env, Cljc *args) {
    int64_t a = as_int(args->as.cons.head, "host-add");
    int64_t b = as_int(args->as.cons.tail->as.cons.head, "host-add");
    return mk_int(a + b);
}

int main(int argc, char **argv) {
    cljc_set_stack_base(&argc);          /* GC scans the C stack for roots */
    CljcEnv *env = cljc_new_env();
    cljc_define_native(env, "host-add", native_add);
    cljc_eval_string(env, "(println (host-add 2 40))");
}
```

See `examples/host.c` for a fuller demo (C-side state mutated from scripts,
maps built in C, results consumed back). Build it with `make example`.

Embedding rules:
- Call `cljc_set_stack_base` with an address near the top of the thread's
  stack **before** evaluating anything (the conservative GC scans from there).
- Don't hold `Cljc *` across `cljc_eval_string` calls unless the value is
  reachable from the root env (e.g. `def`'d) — the GC doesn't know about
  C globals.
- One interpreter per process (global state); single-threaded.

## Performance

Benchmarks in `benchmarks/` run unmodified on cljc, [babashka](https://babashka.org/),
and JVM Clojure, producing identical output (mean wall time, hyperfine, one machine —
treat as orders of magnitude):

| benchmark | cljc | babashka 1.12 | Clojure / JDK 21 |
|---|---|---|---|
| startup (hello world) | **2 ms** | 10 ms | 440 ms |
| fib(27), interpreted recursion | 88 ms | **56 ms** | 563 ms |
| 5M-iteration loop/recur | 439 ms | **239 ms** | 563 ms |
| seq pipeline (1M: filter→map→reduce) | 332 ms | **58 ms** | 488 ms |
| build+read 100k-entry map | 231 ms | **107 ms** | 699 ms |
| build 1M-element vector | 223 ms | **107 ms** | 542 ms |
| sort 200k | **126 ms** | 185 ms | 641 ms |
| regex word-frequency | **23 ms** | 23 ms | 632 ms |
| fib(32), ~1 s of compute | 1.00 s | **0.54 s** | 0.63 s |

Honest reading: babashka wins most compute because its `clojure.core` is
AOT-compiled native code — SCI only interprets your glue, while cljc interprets
*everything*. That cljc stays within ~1.5–4× of bb (and beats it on sort and
startup) is the trade we wanted. JVM Clojure pays ~440 ms of startup, which
dominates at script scale; for long-running compute the JIT inverts everything.
Use cljc where its 2 ms startup, ~300 KB binary, zero dependencies, and
embeddability matter; use bb/JVM where throughput does.

## Deliberate divergences from Clojure

- `catch` is untyped — the class in `(catch Exception e …)` is ignored; the
  first `catch` wins
- Plain integer arithmetic wraps on int64 overflow (Clojure's `*`/`+` throw);
  use the auto-promoting `*'`/`+'`/`-'` or `…N` literals for bignums
- Hash maps/sets iterate in hash order, not insertion order (use sorted
  collections for ordered traversal)
- `str/replace` is literal even with a `#"…"` pattern; regex replace is spelled
  `re-replace` (whose `$1` group refs are the substitution syntax)
- Quasiquote auto-gensym (`x#`) works; nested quasiquote levels do not
- `()` is the empty list (not `nil`), but seq fns treat an exhausted seq as
  `nil`, as in Clojure
- Namespaces are a flat global with per-namespace aliases (not separate var
  interning per ns); `Math/sqrt` resolves as a host-method symbol
- Metadata works (`with-meta`/`meta`/`^{}`) and propagates through collection
  ops (`conj`/`assoc`/`into`, incl. transient roundtrips)
- `#?(:cljc …)` reader conditionals; `#(…)`, `\a` char literals, `^meta` supported

The full list, the roadmap, and the GC invariants live in [PLAN.md](PLAN.md).

## Testing

```sh
make test                      # full suite, normal + CLJC_GC_STRESS=1
# sanitizers (flags required by conservative stack scanning, as with Boehm GC):
cc -g -fsanitize=address,undefined -o /tmp/cljc-asan cljc.c -lm
ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 /tmp/cljc-asan tests.clj
```

Every bug fix lands with a regression assertion in `tests.clj`.
