;; AoC 2017 day 5 — portable across cljc / babashka / clojure.
(try (require (quote [clojure.string :as str])) (catch Exception e nil)) ; no-op on cljc
;; Persistent-vector stress: parts do ~350k / ~25M assoc+nth steps.
(def tape (mapv parse-long (str/split-lines (slurp "input/05.txt"))))

(defn exit [t bump]
  (let [n (count t)]
    (loop [t t loc 0 step 0]
      (if (or (< loc 0) (>= loc n))
        step
        (let [v (nth t loc)]
          (recur (assoc t loc (bump v)) (+ loc v) (inc step)))))))

(println "Answer1:" (exit tape inc))
(println "Answer2:" (exit tape (fn [v] (if (>= v 3) (dec v) (inc v)))))
