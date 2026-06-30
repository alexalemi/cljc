(def nums (clojure.string/split-lines "00100\n11110\n10110\n10111\n10101\n01111\n00111\n11100\n10000\n11001\n00010\n01010"))
(defn col [i] (map (fn [s] (- (int (nth s i)) 48)) nums))
(def W (count (first nums)))
(defn gamma [] (Integer/parseInt (apply str (for [i (range W)] (if (> (apply + (col i)) (/ (count nums) 2)) 1 0))) 2))
(let [g (gamma) e (bit-xor g (dec (bit-shift-left 1 W)))]
  (println "power" (* g e)) (assert (= 198 (* g e))))
(println "2021d03 OK")
