(let [m (reduce (fn [m i] (assoc m i (* i i))) {} (range 100000))]
  (println (reduce (fn [acc i] (+ acc (get m i))) 0 (range 100000))))
