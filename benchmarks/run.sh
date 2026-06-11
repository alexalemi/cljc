#!/bin/bash
# Compare cljc / babashka / clojure on identical scripts (best of 3, wall time).
# All benchmarks produce identical output on all three runtimes.
cd "$(dirname "$0")"
for f in 0*.clj; do
  echo "── $f"
  hyperfine --warmup 1 --min-runs 5 --style basic \
    -n cljc "../cljc $f" -n bb "bb $f" -n clojure "clojure $f" 2>/dev/null |
    grep -E '^Benchmark|Time |relative'
done
