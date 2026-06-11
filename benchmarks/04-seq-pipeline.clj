(println (reduce + (map inc (filter even? (range 1000000)))))
