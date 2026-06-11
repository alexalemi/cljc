(println (loop [i 0 acc 0] (if (< i 5000000) (recur (inc i) (+ acc i)) acc)))
