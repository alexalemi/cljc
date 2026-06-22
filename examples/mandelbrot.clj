;; mandelbrot.clj — render the Mandelbrot set as ASCII art.
;;
;; Showcases: double arithmetic, loop/recur as the hot inner loop, and
;; building a big string with seq comprehensions instead of mutation.
;;
;; Run:  cljc examples/mandelbrot.clj
;;       cljc examples/mandelbrot.clj 100      ; max-iterations override

(def width  78)
(def height 30)

;; Classic escape-time test: iterate z = z^2 + c starting from 0, and count
;; how many steps it takes |z| to exceed 2. Points that never escape are "in"
;; the set. We use loop/recur so the inner loop never allocates.
(defn escape-count [cx cy max-iter]
  (loop [x 0.0, y 0.0, i 0]
    (let [x2 (* x x)
          y2 (* y y)]
      (if (or (>= i max-iter) (> (+ x2 y2) 4.0))
        i
        (recur (+ (- x2 y2) cx)
               (+ (* 2.0 x y) cy)
               (inc i))))))

;; A ramp from "deep inside" to "far outside" — denser glyphs mean more
;; iterations survived.
(def shades " .:-=+*#%@")

(defn glyph [n max-iter]
  (if (>= n max-iter)
    \space                                    ; never escaped — inside the set
    (let [idx (int (* (/ n (* 1.0 max-iter)) (dec (count shades))))]
      (subs shades idx (inc idx)))))

(defn render [max-iter]
  (str/join "\n"
    (for [row (range height)]
      (let [cy (- (* (/ row (* 1.0 height)) 2.0) 1.0)]        ; map to [-1, 1]
        (apply str
          (for [col (range width)]
            (let [cx (- (* (/ col (* 1.0 width)) 3.0) 2.0)]   ; map to [-2, 1]
              (glyph (escape-count cx cy max-iter) max-iter))))))))

(let [max-iter (if (seq *args*) (parse-long (first *args*)) 50)]
  (println (render max-iter)))
