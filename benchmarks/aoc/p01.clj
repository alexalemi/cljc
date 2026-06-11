;; AoC 2017 day 1 — portable across cljc / babashka / clojure.
(try (require (quote [clojure.string :as str])) (catch Exception e nil)) ; no-op on cljc
(def data (str/trim (slurp "input/01.txt")))
(def ds (mapv (fn [c] (parse-long (str c))) (seq data)))

(def ans1
  (reduce + (map (fn [[a b]] (if (= a b) a 0))
                 (partition 2 1 (concat ds [(first ds)])))))

(def ans2
  (let [n (count ds)
        half (quot n 2)]
    (reduce + (map-indexed (fn [i a] (if (= a (nth ds (mod (+ i half) n))) a 0)) ds))))

(println "Answer1:" ans1)
(println "Answer2:" ans2)
