;; nqueens.clj — the N-Queens puzzle by recursive backtracking.
;;
;; Showcases: recursion that grows partial solutions, `for` as a backtracking
;; search (each level filters candidate columns), and representing a board
;; as a vector where index = row, value = column.
;;
;; Run:  cljc examples/nqueens.clj          ; defaults to 8
;;       cljc examples/nqueens.clj 6

;; A placement is safe if no earlier queen shares the column or a diagonal.
;; `queens` is the column already chosen for each previous row, so the new
;; row's index is simply (count queens) and we check against every prior one.
(defn safe? [queens col]
  (let [row (count queens)]
    (every? (fn [r]
              (let [c (nth queens r)]
                (and (not= c col)                         ; same column
                     (not= (- row r) (- col c))           ; same ↘ diagonal
                     (not= (- row r) (- c col)))))         ; same ↙ diagonal
            (range row))))

;; Return every complete board (a vector of n column-choices). The recursion
;; tries each legal column for the current row and recurses; `mapcat` flattens
;; the per-column subtrees, and an empty board of size n is the base case.
(defn solve [n]
  (letfn [(place [queens]
            (if (= (count queens) n)
              [queens]
              (mapcat (fn [col]
                        (when (safe? queens col)
                          (place (conj queens col))))
                      (range n))))]
    (place [])))

(defn show-board [queens]
  (let [n (count queens)]
    (str/join "\n"
      (for [row (range n)]
        (apply str
          (for [col (range n)]
            (if (= (nth queens row) col) "Q " ". ")))))))

(let [n (if (seq *args*) (parse-long (first *args*)) 8)
      solutions (solve n)]
  (println (format "%d-Queens has %d solutions." n (count solutions)))
  (println)
  (println "One of them:")
  (println (show-board (first solutions))))
