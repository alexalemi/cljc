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
| **Data** | nil, bools, int64, doubles, strings (with escapes), symbols, keywords, lists, **persistent vectors** (32-way tries + tail, amortized O(1) `conj`), **persistent maps** (HAMT), **persistent sets**, atoms |
| **Special forms** | `quote if do def defn defmacro let fn loop recur and or when cond try/catch/finally quasiquote` |
| **Macros** | `defmacro` + quasiquote (`` ` `` `~` `~@`, splices into lists/vectors/maps/sets); `->` `->>` `some->` `cond->` `as->` `case` `condp` `for` (with `:when`/`:let`) `doseq` `dotimes` `doto` `letfn` … all written in cljc itself (the prelude) |
| **Functions** | multi-arity `(fn ([x] …) ([x y] …))`, variadic `& rest`, full destructuring (`[a b & r :as v]`, `{:keys [x] :or {…} :as m}`) in `let`/`fn` params |
| **Errors** | `throw` any value, `try`/`catch`/`finally` (finally runs on every exit path), `ex-info`/`ex-message`/`ex-data`; interpreter errors are catchable |
| **Regex** | self-contained backtracking engine: `\d \w \s`, classes, groups, `(?:…)`, alternation, lazy quantifiers; `#"…"` literals; `re-find` `re-matches` `re-seq` `re-replace` (with `$1` refs) `re-split`; guarded against catastrophic backtracking |
| **Library** | ~150 core fns: seq ops (`map filter reduce group-by frequencies partition …`), string ops (`str/split str/join str/trim …`), `format`, `slurp`/`spit`, `Math/*`, `sort`/`compare`, `read-string`/`eval` |
| **Memory** | mark-and-sweep GC over pooled cells, conservative C-stack scanning (interpreter C code needs no root registration), structural sharing throughout |
| **Modes** | REPL, script file, piped stdin, embedded |

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

## Deliberate divergences from Clojure

- `()` ≡ `nil`; no Ratio type (`(/ 7 2)` ⇒ `3.5`); no lazy seqs (eager,
  `range` is bounded); no namespaces or vars (`Math/sqrt` is just a symbol)
- `catch` is untyped — the class in `(catch Exception e …)` is ignored
- Map/set iteration order is hash order, not insertion order
- Patterns are plain strings (`#"…"` is raw-string sugar); `{n,m}` braces are
  literals; `str/replace` is literal — regex replace is spelled `re-replace`
- Quasiquote: no auto-gensym `x#`, no nesting levels

The full list, the roadmap, and the GC invariants live in [PLAN.md](PLAN.md).

## Testing

```sh
make test                      # full suite, normal + CLJC_GC_STRESS=1
# sanitizers (flags required by conservative stack scanning, as with Boehm GC):
cc -g -fsanitize=address,undefined -o /tmp/cljc-asan cljc.c -lm
ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 /tmp/cljc-asan tests.clj
```

Every bug fix lands with a regression assertion in `tests.clj`.
