;; calc.clj — a four-function calculator with a hand-written parser.
;;
;; Showcases: tokenizing with regex, a recursive-descent parser that respects
;; operator precedence, and returning [value remaining-tokens] pairs (a tiny
;; functional parser-combinator style — no mutable cursor).
;;
;; Grammar (precedence climbs as you descend):
;;   expr   = term   (('+' | '-') term)*
;;   term   = factor (('*' | '/') factor)*
;;   factor = number | '(' expr ')' | '-' factor
;;
;; Run:  cljc examples/calc.clj "2 + 3 * (4 - 1)"
;;       cljc examples/calc.clj            ; runs a built-in demo set

(defn tokenize [s]
  (re-seq #"\d+\.?\d*|[-+*/()]" s))

;; Each parse fn takes a token seq and returns [value rest-tokens]. The mutual
;; recursion between expr/term/factor is exactly the grammar above.
(declare parse-expr)

(defn parse-factor [tokens]
  (let [t (first tokens)]
    (cond
      (= t "(") (let [[v rst] (parse-expr (rest tokens))]
                  ;; rst should start with ")"
                  [v (rest rst)])
      (= t "-") (let [[v rst] (parse-factor (rest tokens))]
                  [(- v) rst])
      :else     [(parse-double t) (rest tokens)])))

;; Left-associative folding of a binary level: parse one higher-precedence
;; operand, then keep consuming (op operand) pairs while the operator matches.
(defn parse-binop [sub ops tokens]
  (loop [[v tokens] (sub tokens)]
    (let [op (first tokens)]
      (if (contains? ops op)
        (let [[rhs rst] (sub (rest tokens))
              f (get ops op)]
          (recur [(f v rhs) rst]))
        [v tokens]))))

(defn parse-term [tokens]
  (parse-binop parse-factor {"*" * "/" /} tokens))

(defn parse-expr [tokens]
  (parse-binop parse-term {"+" + "-" -} tokens))

(defn calc [s]
  (first (parse-expr (tokenize s))))

(if (seq *args*)
  (let [expr (str/join " " *args*)]
    (println (format "%s = %s" expr (calc expr))))
  (doseq [e ["2 + 3 * 4"
             "(2 + 3) * 4"
             "2 * -3 + 10"
             "100 / 5 / 2"
             "3.5 + 1.5 * (2 + 2)"]]
    (println (format "%-24s = %s" e (calc e)))))
