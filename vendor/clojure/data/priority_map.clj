;; clojure.data.priority-map — real O(log n) implementation for cljc.
;; Two indexes, like the upstream library: a sorted-map priority -> #{keys}
;; (cljc's weight-balanced tree) and a hash-map key -> priority. Represented
;; as a cljc deftype-style tagged map; assoc/get/count/seq dispatch through
;; the deftype method machinery, and core peek/pop consult the methods first
;; (previously peek did an O(n) min-scan over a plain map — quadratic Dijkstra).

(ns clojure.data.priority-map)

(defn- pm-cell [p->ks k->p]
  {:cljc/type :PriorityMap :p->ks p->ks :k->p k->p})

(defn- pm-assoc [this k v]
  (let [k->p (:k->p this)
        p->ks (:p->ks this)
        old (get k->p k ::none)
        p->ks (if (= old ::none)
                p->ks
                (let [ks (disj (get p->ks old) k)]
                  (if (empty? ks) (dissoc p->ks old) (assoc p->ks old ks))))]
    (pm-cell (assoc p->ks v (conj (get p->ks v #{}) k))
             (assoc k->p k v))))

(defn- pm-peek [this]
  (when-let [[p ks] (first (:p->ks this))]
    [(first ks) p]))

(defn- pm-pop [this]
  (let [[p ks] (first (:p->ks this))
        k (first ks)
        ks (disj ks k)]
    (pm-cell (if (empty? ks) (dissoc (:p->ks this) p) (assoc (:p->ks this) p ks))
             (dissoc (:k->p this) k))))

(defn- pm-dissoc [this k]
  (let [old (get (:k->p this) k ::none)]
    (if (= old ::none)
      this
      (let [ks (disj (get (:p->ks this) old) k)]
        (pm-cell (if (empty? ks) (dissoc (:p->ks this) old) (assoc (:p->ks this) old ks))
                 (dissoc (:k->p this) k))))))

(defn- pm-seq [this]
  (for [[p ks] (seq (:p->ks this)) k ks] [k p]))

(cljc/reg-method! 'assoc  :PriorityMap pm-assoc)
(cljc/reg-method! 'valAt  :PriorityMap (fn ([this k] (get (:k->p this) k))
                                         ([this k nf] (get (:k->p this) k nf))))
(cljc/reg-method! 'count  :PriorityMap (fn [this] (count (:k->p this))))
(cljc/reg-method! 'seq    :PriorityMap (fn [this] (seq (pm-seq this))))
(cljc/reg-method! 'peek   :PriorityMap pm-peek)
(cljc/reg-method! 'pop    :PriorityMap pm-pop)
(cljc/reg-method! 'dissoc :PriorityMap pm-dissoc)
(cljc/reg-method! 'containsKey :PriorityMap (fn [this k] (contains? (:k->p this) k)))

(defn priority-map [& kvs]
  (reduce (fn [m [k v]] (pm-assoc m k v))
          (pm-cell (sorted-map) {})
          (partition 2 kvs)))

(defn priority-map-by
  "Comparator variant: orders priorities with cmp."
  [cmp & kvs]
  (reduce (fn [m [k v]] (pm-assoc m k v))
          (pm-cell (sorted-map-by cmp) {})
          (partition 2 kvs)))
