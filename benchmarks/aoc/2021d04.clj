;; AoC 2021 d4 (bingo) — winning board + draws up to the win
(def draws [7 4 9 5 11 17 23 2 0 14 21 24])
(def board [[14 21 17 24 4][10 16 15 9 19][18 8 23 26 20][22 11 13 6 5][2 0 12 3 7]])
(defn wins? [marked board] (or (some (fn [row] (every? marked row)) board)
                               (some (fn [c] (every? marked (map (fn [row] (nth row c)) board))) (range 5))))
(defn play [board]
  (loop [drawn [] remaining draws]
    (let [marked (set drawn)]
      (if (wins? marked board)
        (* (last drawn) (apply + (for [row board n row :when (not (marked n))] n)))
        (recur (conj drawn (first remaining)) (rest remaining))))))
(println "score" (play board))
(assert (= 4512 (play board)))
(println "2021d04 OK")
