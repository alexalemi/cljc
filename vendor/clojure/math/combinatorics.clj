;; clojure.math.combinatorics — the commonly used subset, pure cljc.

(ns clojure.math.combinatorics)

(defn combinations
  "All k-element subsets of coll, in order."
  [coll k]
  (let [v (vec coll) n (count v)]
    (cond
      (zero? k) '(())
      (> k n) '()
      :else
      (loop [idx (vec (range k)) acc []]
        (let [acc (conj acc (mapv v idx))
              ;; rightmost index that can advance
              i (loop [i (dec k)]
                  (cond (neg? i) -1
                        (< (idx i) (+ n (- k) i)) i
                        :else (recur (dec i))))]
          (if (neg? i)
            (seq acc)
            (recur (let [b (inc (idx i))]
                     (reduce (fn [ix j] (assoc ix j (+ b (- j i))))
                             idx (range i k)))
                   acc)))))))

(defn permutations
  "All orderings of coll (duplicates appear multiply)."
  [coll]
  (let [v (vec coll)]
    (if (<= (count v) 1)
      (list (seq v))
      (mapcat (fn [i]
                (map (fn [p] (cons (v i) p))
                     (permutations (concat (subvec v 0 i) (subvec v (inc i))))))
              (range (count v))))))

(defn subsets
  "All subsets of coll, smallest first."
  [coll]
  (mapcat (fn [k] (combinations coll k)) (range (inc (count coll)))))

(defn cartesian-product [& seqs]
  (if (empty? seqs)
    '(())
    (for [x (first seqs)
          more (apply cartesian-product (rest seqs))]
      (cons x more))))

(defn selections
  "All ways to take n (possibly repeated) elements from coll."
  [coll n]
  (apply cartesian-product (repeat n coll)))
