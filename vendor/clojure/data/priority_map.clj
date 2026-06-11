;; clojure.data.priority-map — pragmatic cljc stand-in.
;; A priority map is just a map: core peek/pop treat maps as priority
;; queues (entry with the smallest value; O(n) scan, fine for puzzles).
;; assoc/dissoc/get/contains?/empty? all work natively.

(ns clojure.data.priority-map)

(defn priority-map [& kvs] (apply hash-map kvs))

(defn priority-map-by
  "Comparator variant — comparator is ignored (min-by-value only)."
  [_ & kvs]
  (apply hash-map kvs))
