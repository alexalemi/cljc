;; shapes.clj — polymorphism in cljc: protocols, records, and multimethods.
;;
;; Showcases all three dispatch mechanisms cljc supports, and how they differ.
;; A note on the dialect: dispatch is on `type`, which returns a *keyword*
;; (e.g. :Circle for a record) rather than a Java class. Records are
;; tagged maps, so they print and destructure like maps.

;; --- Protocols + records --------------------------------------------------
;; A protocol is a named set of functions; records implement them. Under the
;; hood cljc compiles protocol methods to multimethods that dispatch on the
;; record's type keyword.
(defprotocol Shape
  (area [s])
  (perimeter [s]))

(defrecord Circle [r])
(defrecord Rectangle [w h])

;; DIALECT NOTE: cljc's extend-type takes the type KEYWORD and method impls
;; directly — there is no protocol-name argument like Clojure's
;; (extend-type T Proto (m [..] ..)). Records get type keywords like
;; :Circle automatically. Math/PI is a real constant (a bare symbol — don't
;; wrap it in parens, it isn't a function).
(extend-type :Circle
  (area [s] (* Math/PI (:r s) (:r s)))
  (perimeter [s] (* 2 Math/PI (:r s))))

(extend-type :Rectangle
  (area [s] (* (:w s) (:h s)))
  (perimeter [s] (* 2 (+ (:w s) (:h s)))))

(def shapes [(->Circle 1.0) (->Rectangle 3 4) (->Circle 2.0)])

(println "Areas via protocol dispatch:")
(doseq [s shapes]
  (println (format "  %-22s area=%.3f  perimeter=%.3f"
                   (pr-str s) (area s) (perimeter s))))

(println "Total area:" (format "%.3f" (reduce + (map area shapes))))

;; --- Multimethods ---------------------------------------------------------
;; When dispatch isn't tied to a type, a multimethod lets you pick the method
;; with an arbitrary function of the arguments. Here we dispatch on a :kind key.
(defmulti describe :kind)

(defmethod describe :dog [_] "woof")
(defmethod describe :cat [_] "meow")
(defmethod describe :default [animal] (str "(" (name (:kind animal)) " says nothing)"))

(println)
(doseq [a [{:kind :dog} {:kind :cat} {:kind :fish}]]
  (println " " (:kind a) "->" (describe a)))

;; Multimethods can dispatch on a computed value, not just a key — classic
;; FizzBuzz where the dispatch function returns the *category* of a number.
(defmulti fizzbuzz (fn [n] [(zero? (mod n 3)) (zero? (mod n 5))]))
(defmethod fizzbuzz [true true]  [_] "FizzBuzz")
(defmethod fizzbuzz [true false] [_] "Fizz")
(defmethod fizzbuzz [false true] [_] "Buzz")
(defmethod fizzbuzz [false false] [n] (str n))

(println)
(println "FizzBuzz 1..20:" (str/join " " (map fizzbuzz (range 1 21))))
