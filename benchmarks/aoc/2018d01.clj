(def changes [3 3 4 -2 -4])
(defn first-twice [changes]
  (reduce (fn [[seen sum] d] (let [s (+ sum d)] (if (seen s) (reduced s) [(conj seen s) s])))
          [#{0} 0] (cycle changes)))
(println "twice" (first-twice changes))
(assert (= 10 (first-twice changes)))
(println "2018d01 OK")
