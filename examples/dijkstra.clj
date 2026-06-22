;; dijkstra.clj — shortest paths through a weighted graph.
;;
;; Showcases: maps as adjacency lists, sets for the visited frontier,
;; immutable state threaded through loop/recur, and reconstructing a path by
;; walking a predecessor map backwards.
;;
;; The graph: a small road network. Each node maps to its neighbours and the
;; edge cost to reach them.
(def graph
  {:a {:b 7 :c 9 :f 14}
   :b {:a 7 :c 10 :d 15}
   :c {:a 9 :b 10 :d 11 :f 2}
   :d {:b 15 :c 11 :e 6}
   :e {:d 6 :f 9}
   :f {:a 14 :c 2 :e 9}})

;; Returns {:dist {node cost}, :prev {node predecessor}}. We avoid a real
;; priority queue (cljc has none built in) by scanning the unvisited set for
;; the current minimum each round — fine for small graphs and clearer to read.
(defn dijkstra [graph source]
  (loop [dist    {source 0}
         prev    {}
         visited #{}
         frontier #{source}]
    (if (empty? frontier)
      {:dist dist :prev prev}
      (let [u (apply min-key #(get dist % 1e18) frontier)
            visited (conj visited u)
            ;; relax every edge out of u, collecting distance/prev updates
            updates (for [[v w] (get graph u)
                          :when (not (contains? visited v))
                          :let [alt (+ (get dist u) w)]
                          :when (< alt (get dist v 1e18))]
                      [v alt])
            dist (reduce (fn [d [v alt]] (assoc d v alt)) dist updates)
            prev (reduce (fn [p [v _]] (assoc p v u)) prev updates)
            frontier (into (disj frontier u) (map first updates))]
        (recur dist prev visited frontier)))))

;; Walk the predecessor chain from target back to source, then reverse it.
(defn path-to [prev source target]
  (loop [node target, acc '()]
    (cond
      (= node source) (cons source acc)
      (nil? node)     nil                  ; unreachable
      :else           (recur (get prev node) (cons node acc)))))

(let [{:keys [dist prev]} (dijkstra graph :a)]
  (println "Shortest distances from :a")
  (doseq [node (sort (keys dist))]
    (let [path (path-to prev :a node)]
      (println (format "  to %s: cost %2d  via %s"
                       (name node)
                       (get dist node)
                       (str/join " -> " (map name path)))))))
