(def moves (clojure.string/split "R2, L3" #", "))
(defn walk [moves]
  (loop [m moves dir [0 1] pos [0 0]]
    (if (empty? m) (+ (Math/abs (first pos)) (Math/abs (second pos)))
      (let [[t & n] (first (clojure.string/split (first m) #"")) ; first char
            turn (first (first m)) steps (parse-long (subs (first m) 1))
            [dx dy] dir nd (if (= turn \R) [dy (- dx)] [(- dy) dx])]
        (recur (rest m) nd [(+ (first pos) (* steps (first nd))) (+ (second pos) (* steps (second nd)))])))))
(println "dist" (walk moves))
(assert (= 5 (walk moves)))
(println "2016d01 OK")
