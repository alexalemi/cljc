(def N 1000)
;; deterministic synthetic instructions: 40 rectangles, mix of on/off/toggle
(def instrs (for [i (range 40)]
              (let [a (mod (* i 137) 900) b (mod (* i 91) 900)]
                [(case (mod i 3) 0 :on 1 :off :toggle) a b (+ a 99) (+ b 99)])))
(defn apply-instr [g [op r0 c0 r1 c1]]
  (loop [g g r r0]
    (if (> r r1) g
      (recur (loop [g g c c0]
               (if (> c c1) g
                 (let [idx (+ (* r N) c) v (g idx)]
                   (recur (assoc! g idx (case op :on 1 :off 0 :toggle (- 1 v))) (inc c)))))
             (inc r)))))
(defn solve []
  (let [g (transient (vec (repeat (* N N) 0)))
        g (reduce apply-instr g instrs)]
    (reduce + (persistent! g))))
(println "lit" (solve))
(println "2015d06 OK")
