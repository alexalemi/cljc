# Differential + invariant fuzzing, conformance corpus

## String/regex fuzzing

String/regex-focused fuzzer used to validate the codepoint-strings work.

```sh
python3 gen_str.py 42 400 bmp > bmp.txt     # BMP corpus: cljc must match bb exactly
python3 gen_str.py 43 400 inv > inv.txt     # invariants: every line must print true

cljc runner_cljc.clj bmp.txt > a.txt
bb -f runner.clj -- bmp.txt > b.txt
diff a.txt b.txt                             # empty = no divergences

cljc runner_cljc.clj inv.txt | grep -v '^true$'          # empty = all hold
CLJC_GC_STRESS=1 cljc runner_cljc.clj inv.txt | grep -v '^true$'
```

The BMP corpus stays below U+10000 because JVM strings index UTF-16 code
units — astral chars legitimately count as 2 there and 1 in cljc — so
astral inputs are exercised by the self-consistency invariants instead.

## Conformance corpus

`conformance.txt` is a curated corpus of clojure.core expressions whose
printed value must match JVM Clojure exactly — the same eval-and-`prn`
runners, but hand-written coverage of the core vocabulary (numbers incl.
ratios/bignums/double printing, strings/regex, collections, seq library,
destructuring, transducers, multimethods, error handling) rather than
generated strings. `conformance_expected.txt` is the golden output; every
line was verified against JVM Clojure semantics. `make conformance` (also
part of `make test`) diffs cljc against the golden, and re-derives the
golden live when `bb` is on PATH:

```sh
cljc runner_cljc.clj conformance.txt | diff - conformance_expected.txt
bb -f runner.clj -- conformance.txt > conformance_expected.txt   # regenerate golden
```

Ground rules for corpus lines: one deterministic expression per line
(`;` comments and blanks are skipped), print unordered collections only
through sorted views, and avoid the documented divergences (astral strings,
wrapping int64 arithmetic, typed catch, small-`N` literal demotion, hash
values, gensym). The corpus has already earned its keep: it caught
`(partition-all n step coll)`, 3-coll `interleave`, `.toUpperCase`,
`rationalize`, and double-`range` element promotion all diverging — each
now fixed with regression tests.
