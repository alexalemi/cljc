# Differential + invariant fuzzing

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
