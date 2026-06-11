# cljc

A small Clojure in a single C file. Inspired by [Janet](https://janet-lang.org/)'s
embeddability, aimed at [babashka](https://babashka.org/)-flavored scripting
coverage: persistent collections, macros, destructuring, exceptions, regex —
in ~4,000 lines of C99 with no dependencies.

```clojure
(defn top-words [text n]
  (->> (re-seq #"\w+" (str/lower-case text))
       frequencies
       (sort-by (fn [[_ c]] (- c)))
       (take n)))

(doseq [[word count] (top-words (slurp "moby-dick.txt") 5)]
  (println (format "%-12s %d" word count)))
```

## Quick start

```sh
make            # builds ./cljc (cc, -lm, nothing else)
./cljc          # interactive REPL (multi-line aware)
./cljc file.clj # run a script
make test       # 380+ assertions, run twice: normal + GC-stress mode
```

## What's inside

| Area | Coverage |
|---|---|
| **Lazy seqs** | `lazy-seq`, infinite `range`/`iterate`/`repeat`/`cycle`/`repeatedly`, lazy `map`/`filter`/`take`/`concat`, true call-by-need |
| **Data** | nil, bools, int64, doubles, strings (with escapes), symbols, keywords, lists, **persistent vectors** (32-way tries + tail, amortized O(1) `conj`), **persistent maps** (HAMT), **persistent sets**, atoms |
| **Special forms** | `quote if do def defn defmacro let fn loop recur and or when cond try/catch/finally quasiquote` |
| **Macros** | `defmacro` + quasiquote (`` ` `` `~` `~@`, splices into lists/vectors/maps/sets); `->` `->>` `some->` `cond->` `as->` `case` `condp` `for` (with `:when`/`:let`) `doseq` `dotimes` `doto` `letfn` … all written in cljc itself (the prelude) |
| **Functions** | multi-arity `(fn ([x] …) ([x y] …))`, variadic `& rest`, full destructuring (`[a b & r :as v]`, `{:keys [x] :or {…} :as m}`) in `let`/`fn` params |
| **Polymorphism** | `defmulti`/`defmethod`, `defprotocol`/`extend-type`/`satisfies?` (dispatch on `type` keywords), `defrecord` (tagged maps), `binding`/`with-redefs` |
| **Errors** | `throw` any value, `try`/`catch`/`finally` (finally runs on every exit path), `ex-info`/`ex-message`/`ex-data`; interpreter errors are catchable |
| **Regex** | self-contained backtracking engine: `\d \w \s`, classes, groups, `(?:…)`, alternation, lazy quantifiers; `#"…"` literals; `re-find` `re-matches` `re-seq` `re-replace` (with `$1` refs) `re-split`; guarded against catastrophic backtracking |
| **Library** | ~150 core fns: seq ops (`map filter reduce group-by frequencies partition …`), string ops (`str/split str/join str/trim …`), `format`, `slurp`/`spit`, `Math/*`, `sort`/`compare`, `read-string`/`eval` |
| **Memory** | mark-and-sweep GC over pooled cells, conservative C-stack scanning (interpreter C code needs no root registration), structural sharing throughout |
| **FFI** | s7/cload-style: `(ffi/define [[:double cos [:double]]] {:headers ["math.h"] :libs "-lm"})` declares C signatures as data, generates glue, compiles a .so at runtime, dlopens it — `cos` becomes a cljc fn. Modules cache by content hash; `ffi/defstruct` generates struct accessors; `libc.clj` ships the libc surface (`(load-file "libc.clj")` → `getpid`, `file-exists?`, `cwd`, `env`…); `json.clj` is a pure-cljc JSON parser/writer. Plus `(sh "cmd")` → `{:exit :out}` |
| **Modes** | REPL, script file, piped stdin, embedded, **nREPL server** |

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
| 5M-iteration loop/recur | 578 ms | **239 ms** | 563 ms |
| seq pipeline (1M: filter→map→reduce) | 2.9 s¹ | **58 ms** | 488 ms |
| build+read 100k-entry map | 231 ms | **107 ms** | 699 ms |
| build 1M-element vector | 223 ms | **107 ms** | 542 ms |
| sort 200k | **126 ms** | 185 ms | 641 ms |
| regex word-frequency | **23 ms** | 23 ms | 632 ms |
| fib(32), ~1 s of compute | 1.00 s | **0.54 s** | 0.63 s |

Real-program benchmarks — [Advent of Code](https://adventofcode.com/) solutions
in `benchmarks/aoc/`, identical sources and answers on all three runtimes:

| puzzle (AoC 2017) | cljc | babashka | Clojure |
|---|---|---|---|
| day 1: digit pairs | **5 ms** | 12 ms | 586 ms |
| day 4: passphrases (sets + regex) | **11 ms** | 16 ms | 608 ms |
| day 5: jump tape (25M vector assocs) | 17.0 s | 8.2 s | **2.9 s** |
| day 6: redistribution (vectors as hash keys) | 186 ms | **101 ms** | 640 ms |

¹ was 321 ms when map/filter were eager natives; real lazy sequences cost a
thunk+closure per element in an interpreter. Chunked seqs (Clojure's own fix)
are the planned remedy.

Honest reading: babashka wins most compute because its `clojure.core` is
AOT-compiled native code — SCI only interprets your glue, while cljc interprets
*everything*. That cljc stays within ~1.5–4× of bb with a 4,000-line tree-walker
(and beats it on sort and startup) is the trade we wanted. JVM Clojure pays
~440 ms of startup, which dominates at script scale; for long-running compute
the JIT inverts everything. Use cljc where its 2 ms startup, ~100 KB binary,
and zero dependencies matter; use bb/JVM where throughput does.

## Deliberate divergences from Clojure

- `()` ≡ `nil`; no Ratio type (`(/ 7 2)` ⇒ `3.5`); no namespaces or vars
  (`Math/sqrt` is just a symbol)
- `catch` is untyped — the class in `(catch Exception e …)` is ignored
- Map/set iteration order is hash order, not insertion order
- Patterns are plain strings (`#"…"` is raw-string sugar); `{n,m}` braces are
  literals; `str/replace` is literal — regex replace is spelled `re-replace`
- Quasiquote: no auto-gensym `x#`, no nesting levels
- `ns`/`require` are tolerated no-ops; `#?(:cljc …)` reader conditionals;
  `#(…)`, `\a` char literals (1-char strings), `^meta` (discarded) supported

The full list, the roadmap, and the GC invariants live in [PLAN.md](PLAN.md).

## Testing

```sh
make test                      # full suite, normal + CLJC_GC_STRESS=1
# sanitizers (flags required by conservative stack scanning, as with Boehm GC):
cc -g -fsanitize=address,undefined -o /tmp/cljc-asan cljc.c -lm
ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 /tmp/cljc-asan tests.clj
```

Every bug fix lands with a regression assertion in `tests.clj`.
