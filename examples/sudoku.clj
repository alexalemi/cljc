;; sudoku.clj — solve a Sudoku puzzle by backtracking search.
;;
;; Showcases: a vector as a flat 81-cell board, sets to compute the legal
;; candidates for a cell, the "most-constrained cell first" heuristic, and
;; recursion that returns a solved board or nil (failure unwinds naturally).

(require '[clojure.set])   ; cljc loads the real upstream clojure.set

;; The board is a vector of 81 ints, 0 = blank, row-major.
(def puzzle
  [5 3 0  0 7 0  0 0 0
   6 0 0  1 9 5  0 0 0
   0 9 8  0 0 0  0 6 0
   8 0 0  0 6 0  0 0 3
   4 0 0  8 0 3  0 0 1
   7 0 0  0 2 0  0 0 6
   0 6 0  0 0 0  2 8 0
   0 0 0  4 1 9  0 0 5
   0 0 0  0 8 0  0 7 9])

(defn row-vals [board r] (set (for [c (range 9)] (nth board (+ (* r 9) c)))))
(defn col-vals [board c] (set (for [r (range 9)] (nth board (+ (* r 9) c)))))
(defn box-vals [board r c]
  (let [r0 (* 3 (quot r 3)), c0 (* 3 (quot c 3))]
    (set (for [dr (range 3) dc (range 3)]
           (nth board (+ (* (+ r0 dr) 9) (+ c0 dc)))))))

;; The digits 1-9 not already used in this cell's row, column, or 3x3 box.
;; clojure.set is overkill here — plain set difference via `remove` reads fine.
(defn candidates [board r c]
  (let [used (clojure.set/union (row-vals board r) (col-vals board c) (box-vals board r c))]
    (remove used (range 1 10))))

;; Pick the empty cell with the FEWEST candidates — this prunes the search
;; tree dramatically versus scanning left-to-right.
(defn most-constrained [board]
  (->> (for [i (range 81) :when (zero? (nth board i))]
         [i (candidates board (quot i 9) (mod i 9))])
       (sort-by (fn [[_ cs]] (count cs)))
       first))

(defn solve [board]
  (if-let [[i cands] (most-constrained board)]
    (some (fn [v] (solve (assoc board i v))) cands)   ; try each; nil unwinds
    board))                                            ; no blanks left → solved

(defn show [board]
  (str/join "\n"
    (for [r (range 9)]
      (str/join " "
        (for [c (range 9)]
          (let [v (nth board (+ (* r 9) c))]
            (if (zero? v) "." (str v))))))))

(println "Puzzle:")
(println (show puzzle))
(println)
(let [solved (solve puzzle)]
  (if solved
    (do (println "Solution:") (println (show solved)))
    (println "No solution.")))
