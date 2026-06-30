(def N 100)
(def grid (vec (for [r (range N)] (vec (for [c (range N)] (if (zero? (mod (+ (* r 31) (* c 17)) 5)) 1 0))))))
(defn nbr-sum [g r c] (apply + (for [dr [-1 0 1] dc [-1 0 1] :when (not (and (zero? dr) (zero? dc)))
                                     :let [nr (+ r dr) nc (+ c dc)] :when (and (>= nr 0) (< nr N) (>= nc 0) (< nc N))]
                                 (get-in g [nr nc]))))
(defn step [g] (vec (for [r (range N)] (vec (for [c (range N)]
  (let [n (nbr-sum g r c) on (get-in g [r c])] (if (or (= n 3) (and (= on 1) (= n 2))) 1 0)))))))
(def after (nth (iterate step grid) 5))
(println "on" (apply + (map (fn [row] (apply + row)) after)))
(println "2015d18 OK")
