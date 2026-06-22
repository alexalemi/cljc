;; life.clj — Conway's Game of Life on an infinite grid.
;;
;; Showcases: persistent sets as the world state, `frequencies` for the
;; neighbour count trick, set/keep idioms, and `iterate` to make the
;; simulation itself a lazy sequence of generations.
;;
;; The classic functional encoding: the board is just the SET of live cells
;; (each an [x y] vector). No array, no bounds — the universe is unbounded.

(defn neighbours [[x y]]
  (for [dx [-1 0 1]
        dy [-1 0 1]
        :when (not (and (zero? dx) (zero? dy)))]
    [(+ x dx) (+ y dy)]))

;; The whole rule in five lines: tally how many live cells each coordinate
;; touches, then keep the ones that either (a) are alive with 2-3 neighbours
;; or (b) are dead with exactly 3. `frequencies` over all neighbour-of-live
;; coordinates is the elegant trick — it only ever considers cells adjacent
;; to something alive.
(defn step [live]
  (let [counts (frequencies (mapcat neighbours live))]
    (set
      (for [[cell n] counts
            :when (or (= n 3) (and (= n 2) (contains? live cell)))]
        cell))))

(defn render [live w h]
  (str/join "\n"
    (for [y (range h)]
      (apply str
        (for [x (range w)]
          (if (contains? live [x y]) "#" "."))))))

;; A "glider" — the famous pattern that walks diagonally across the grid.
(def glider #{[1 0] [2 1] [0 2] [1 2] [2 2]})

(let [generations (iterate step glider)]
  (doseq [g (range 5)]
    (println (str "Generation " g ":"))
    (println (render (nth generations g) 10 10))
    (println)))

;; A blinker oscillates with period 2 — prove it returns to itself.
(def blinker #{[1 0] [1 1] [1 2]})
(println "Blinker is periodic (period 2)?"
         (= blinker (step (step blinker))))
