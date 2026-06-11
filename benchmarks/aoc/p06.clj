;; AoC 2017 day 6 — portable across cljc / babashka / clojure.
;; Vectors as hash keys: stresses collection hashing + set/map lookups.
(def data (mapv parse-long (re-seq #"\d+" (slurp "input/06.txt"))))

(defn max-index [banks]
  (reduce (fn [[bi bv] [i v]] (if (> v bv) [i v] [bi bv]))
          [0 -1]
          (map-indexed vector banks)))

(defn redistribute [banks]
  (let [[index blocks] (max-index banks)
        n (count banks)]
    (loop [b (assoc banks index 0)
           i (mod (inc index) n)
           k blocks]
      (if (zero? k)
        b
        (recur (assoc b i (inc (get b i))) (mod (inc i) n) (dec k))))))

(defn first-repeat [banks]
  (loop [b banks seen #{} c 0]
    (let [nb (redistribute b)]
      (if (contains? seen nb) (inc c) (recur nb (conj seen nb) (inc c))))))

(defn cycle-length [banks]
  (loop [b banks seen {} c 0]
    (let [nb (redistribute b)]
      (if (contains? seen nb)
        (- c (get seen nb))
        (recur nb (assoc seen nb c) (inc c))))))

(println "Answer1:" (first-repeat data))
(println "Answer2:" (cycle-length data))
