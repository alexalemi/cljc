;; macros.clj — writing your own syntax with defmacro + quasiquote.
;;
;; Showcases: code-as-data, quasiquote/unquote (` ~ ~@), explicit gensym for
;; hygiene (cljc has no auto-gensym `x#`), and recursive macros that rewrite
;; whole expression trees.

;; 1. `unless` — the mirror of `when`. The simplest useful macro: it receives
;; its arguments UNEVALUATED and splices them into a template.
(defmacro unless [test & body]
  `(if ~test nil (do ~@body)))

(unless false (println "1. unless: this prints because the test is false"))
(unless true  (println "    (this never prints)"))

;; 2. `dbg` — print an expression's SOURCE and its value, then return the
;; value so it's transparent to drop into any position. We bind to a gensym
;; so the argument is evaluated exactly once (hygiene: a naive `~x` repeated
;; in the template would double-evaluate side effects).
(defmacro dbg [x]
  (let [v (gensym "v")]
    `(let [~v ~x]
       (println "2. dbg:" '~x "=>" ~v)
       ~v)))

(def result (* (dbg (+ 1 2)) (dbg (- 10 4))))
(println "    product of the two debugged values:" result)

;; 3. `my-and` — short-circuiting AND, defined recursively. A macro can expand
;; into a call to ITSELF, and the expander keeps running until the base cases
;; (0 or 1 args) are reached. Shows macros are just compile-time functions.
(defmacro my-and
  ([] true)
  ([x] x)
  ([x & rest] (let [v (gensym "v")]
                `(let [~v ~x] (if ~v (my-and ~@rest) ~v)))))

(println "3. my-and:" (my-and 1 2 3) "/" (my-and 1 nil 3) "/" (my-and))

;; 4. `infix` — rewrite (a op b) arithmetic into Clojure prefix form. A small
;; tree-transforming macro: numbers pass through, a 3-element [a op b] vector
;; becomes (op a b), recursively. This is the "macros let you grow the
;; language" payoff — we just added infix notation to a Lisp.
(defmacro infix [form]
  (if (and (sequential? form) (= 3 (count form)))
    (let [[a op b] form]
      (list op (list 'infix a) (list 'infix b)))
    form))

(println "4. infix: (2 + 3 * 4) =>"
         (infix [[2 + [3 * 4]] + [10 / 2]]))    ; = 2 + 12 + 5 = 19
