(ns clojure.data
  "Recursive data diff (clojure.data/diff): [things-only-in-a only-in-b in-both]."
  (:require [clojure.set :as set]))

(declare diff)

(defn- atom-diff [a b]
  (if (= a b) [nil nil a] [a b nil]))

(defn- diff-associative-key [a b k]
  (let [va (get a k) vb (get b k)
        [a* b* ab] (diff va vb)
        in-a (contains? a k) in-b (contains? b k)
        same (and in-a in-b (or (not (nil? ab)) (and (nil? va) (nil? vb))))]
    [(when (and in-a (or (not (nil? a*)) (not same))) {k a*})
     (when (and in-b (or (not (nil? b*)) (not same))) {k b*})
     (when same {k ab})]))

(defn- diff-associative [a b ks]
  (reduce (fn [d1 d2] (doall (map merge d1 d2)))
          [nil nil nil]
          (map (partial diff-associative-key a b) ks)))

(defn- diff-set [a b]
  [(not-empty (set/difference a b))
   (not-empty (set/difference b a))
   (not-empty (set/intersection a b))])

(defn- vectorize [m]      ; {idx val} -> a vector with nils in the gaps
  (when (seq m)
    (reduce (fn [result [k v]] (assoc result k v))
            (vec (repeat (inc (apply max (keys m))) nil))
            m)))

(defn- diff-sequential [a b]
  (vec (map vectorize
            (diff-associative (vec a) (vec b)
                              (range (max (count a) (count b)))))))

(defn diff [a b]
  (if (= a b)
    [nil nil a]
    (cond
      (and (map? a) (map? b))
      (diff-associative a b (set/union (set (keys a)) (set (keys b))))
      (and (set? a) (set? b)) (diff-set a b)
      (and (sequential? a) (sequential? b)) (diff-sequential a b)
      :else (atom-diff a b))))
