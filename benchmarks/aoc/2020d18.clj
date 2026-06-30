(defn tokenize [s] (re-seq #"\d+|[-+*()]" s))
(defn evl [tokens]
  (let [toks (atom tokens)]
    (letfn [(value [] (let [t (first @toks)] (swap! toks rest)
                        (if (= t "(") (let [v (expr)] (swap! toks rest) v) (parse-long t))))
            (expr [] (loop [acc (value)] (let [op (first @toks)]
                       (if (#{"+" "*"} op) (do (swap! toks rest) (recur ((if (= op "+") + *) acc (value)))) acc))))]
      (expr))))
(defn solve [line] (evl (tokenize line)))
(println "r" (solve "2 * 3 + (4 * 5)"))
(assert (= 26 (solve "2 * 3 + (4 * 5)")))
(assert (= 437 (solve "5 + (8 * 3 + 9 + 3 * 4 * 3)")))
(println "2020d18 OK")
