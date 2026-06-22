;; fractal-svg.clj — draw a recursive fractal tree and write it to an SVG file.
;;
;; Showcases: recursion that accumulates geometry, trig from Math/*, building
;; structured text output, and `spit` to produce a real file artifact you can
;; open in any browser.
;;
;; Run:  cljc examples/fractal-svg.clj            ; writes /tmp/tree.svg
;;       cljc examples/fractal-svg.clj out.svg 11 ; custom path + recursion depth

(def deg (/ Math/PI 180.0))   ; degrees -> radians

;; Each call draws one branch (a line) and recurses into two shorter, rotated
;; child branches until depth runs out. We return a flat seq of SVG <line>
;; strings — purely functional, no mutable canvas.
(defn branch [x y angle length depth]
  (if (zero? depth)
    []
    (let [x2 (+ x (* length (Math/cos (* angle deg))))
          y2 (- y (* length (Math/sin (* angle deg))))   ; SVG y grows downward
          width (max 1 (quot depth 2))
          line (format "<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='hsl(%d,60%%,35%%)' stroke-width='%d'/>"
                       x y x2 y2 (* depth 25) width)]
      (concat [line]
              (branch x2 y2 (+ angle 25) (* length 0.75) (dec depth))
              (branch x2 y2 (- angle 22) (* length 0.72) (dec depth))))))

(defn svg [depth]
  (let [lines (branch 300.0 580.0 90 140.0 depth)]
    (str "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='600'>\n"
         "<rect width='600' height='600' fill='#fdfcf7'/>\n"
         (str/join "\n" lines)
         "\n</svg>\n")))

(let [path  (if (>= (count *args*) 1) (first *args*) "/tmp/tree.svg")
      depth (if (>= (count *args*) 2) (parse-long (second *args*)) 10)
      doc   (svg depth)]
  (spit path doc)
  (println (format "Wrote a depth-%d fractal tree (%d branches) to %s"
                   depth
                   (count (re-seq #"<line" doc))
                   path))
  (println "Open it in a browser to see the tree."))
