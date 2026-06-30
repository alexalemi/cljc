(def fish (mapv parse-long (clojure.string/split "3,4,3,1,2" #",")))
(defn step [counts]
  (let [c (vec counts) spawn (nth c 0)]
    (-> (vec (concat (subvec c 1) [spawn])) (update 6 + spawn))))
(defn sim [days]
  (let [init (reduce (fn [v f] (update v f inc)) (vec (repeat 9 0)) fish)]
    (apply + (nth (iterate step init) days))))
(println "d80" (sim 80) "d256" (sim 256))
(assert (= 5934 (sim 80))) (assert (= 26984457539 (sim 256)))
(println "2021d06 OK")
