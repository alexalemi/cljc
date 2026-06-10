; cljc regression tests — each (assert= expected actual) prints PASS/FAIL.
; Run: ./cljc < tests.clj   (or: make test)

(defn assert= [expected actual]
  (if (= expected actual)
    (println "PASS")
    (println "FAIL: expected" (pr-str expected) "got" (pr-str actual))))

; arithmetic & numbers
(assert= 6 (+ 1 2 3))
(assert= 2432902008176640000 (reduce * (range 1 21)))
(assert= 2 (mod -7 3))
(assert= -1 (rem -7 3))
(assert= 3.5 (/ 7.0 2))
(assert= true (< 1 2 3))
(assert= false (< 1 3 2))
(assert= true (>= 3 3 2))

; control flow
(assert= :yes (if (< 1 2) :yes :no))
(assert= :c (cond (< 3 1) :a (< 3 2) :b :else :c))
(assert= nil (and 1 2 nil 3))
(assert= 7 (or nil false 7))
(assert= nil (when false :never))
(assert= :ok (when true :ok))

; loop/recur — stack safe
(assert= 45 (loop [i 0 acc 0] (if (< i 10) (recur (inc i) (+ acc i)) acc)))
(assert= 1000000 (loop [i 0] (if (< i 1000000) (recur (inc i)) i)))

; fn-level recur
(defn countdown [n] (if (zero? n) :liftoff (recur (dec n))))
(assert= :liftoff (countdown 100000))

; variadics & apply
(defn my-sum [& xs] (reduce + 0 xs))
(assert= 10 (my-sum 1 2 3 4))
(assert= 10 (apply my-sum [1 2 3 4]))
(assert= 10 (apply my-sum 1 2 [3 4]))

; collections
(assert= [1 2 3 4] (conj [1 2] 3 4))
(assert= (list 0 1 2) (conj (list 1 2) 0))
(assert= 20 (nth [10 20 30] 1))
(assert= :dflt (nth [10] 5 :dflt))
(assert= true (= [1 2 3] (list 1 2 3)))
(assert= 2 (second [1 2 3]))
(assert= 3 (last [1 2 3]))
(assert= (list 3 2 1) (reverse [1 2 3]))

; maps
(def person {:name "Rich" :age 17})
(assert= "Rich" (:name person))
(assert= 18 (:age (assoc person :age 18)))
(assert= 17 (:age person))                       ; persistence!
(assert= {:name "Rich"} (dissoc person :age))
(assert= true (contains? person :name))
(assert= {:a 1 :b 3 :c 4} (merge {:a 1 :b 2} {:b 3 :c 4}))
(assert= true (= {:a 1 :b 2} {:b 2 :a 1}))
(assert= :v ({:k :v} :k))
(assert= :fallback (:missing person :fallback))
(assert= 42 (get {:x 42} :x))
(assert= [10 20] (assoc [10 99] 1 20))

; seq library
(assert= (list 1 4 9) (map (fn [x] (* x x)) [1 2 3]))
(assert= (list 0 2 4) (filter (fn [x] (zero? (mod x 2))) (range 6)))
(assert= 15 (reduce + (range 6)))
(assert= (list 0 1 2) (take 3 (range 100)))
(assert= (list 8 9) (drop 8 (range 10)))
(assert= (list 0 2 4 6 8) (range 0 10 2))
(assert= nil (seq []))

; strings
(assert= "n=42" (str "n=" 42))
(assert= "" (str nil))
(assert= 5 (count "hello"))
(assert= "[1 2]" (pr-str [1 2]))

; higher order
(defn comp2 [f g] (fn [x] (f (g x))))
(assert= 10 ((comp2 inc (fn [x] (* x 3))) 3))
(assert= (list 2 3 4) (map inc [1 2 3]))

; unary arithmetic
(assert= -5 (- 5))
(assert= 0.5 (/ 2))
(assert= 3.5 (/ 7 2))
(assert= 3 (/ 6 2))

; macros: quasiquote
(assert= (list 1 2 3) `(1 2 ~(+ 1 2)))
(assert= (list 1 2 3 4) `(1 ~@(list 2 3) 4))
(assert= [1 2 3] `[1 ~(inc 1) 3])

; defmacro
(defmacro unless [test & body] `(if ~test nil (do ~@body)))
(assert= :ran (unless false :ran))
(assert= nil (unless true :never))
(defmacro twice [form] `(do ~form ~form))
(def counter-val 0)
(twice (def counter-val (inc counter-val)))
(assert= 2 counter-val)

; prelude fns
(assert= true (even? 4))
(assert= true (odd? 7))
(assert= 5 (abs -5))
(assert= 9 (max 3 9 1))
(assert= 1 (min 3 9 1))
(assert= true (not= 1 2))
(assert= 7 ((comp inc (partial * 2)) 3))
(assert= [1 2 3] (into [] (list 1 2 3)))
(assert= [2 4 6] (mapv (partial * 2) [1 2 3]))
(assert= [0 2 4] (filterv even? (range 6)))
(assert= (list :x :x :x) (repeat 3 :x))
(assert= (list 1 2 3 4) (concat [1 2] (list 3 4)))

; threading macros
(assert= 7 (-> 3 inc (* 2) dec))
(assert= (list 2 4) (->> (range 5) (map inc) (filter even?)))
(assert= :ok (when-not false :ok))
(assert= :b (if-not true :a :b))

; ── regression tests from the bug-hunt pass ──

; string escapes round-trip
(assert= 3 (count "a\nb"))
(assert= "a\nb" (str "a" "\n" "b"))
(assert= "\"a\\nb\"" (pr-str "a\nb"))
(assert= "tab\there" (str "tab\there"))

; ~@ splices vectors and maps, not just lists
(assert= (list 1 2 3 4) `(1 ~@[2 3] 4))
(assert= (list 0 [:a 1]) `(0 ~@{:a 1}))

; unquote inside map literals in templates
(assert= {:k 3} `{:k ~(+ 1 2)})

; cons keeps lists proper across collection types
(assert= (list 1 2 3) (cons 1 [2 3]))
(assert= (list 1) (cons 1 nil))

; doubles print distinctly from ints
(assert= "1.0" (str 1.0))
(assert= "1" (str 1))
(assert= "0.5" (str (/ 2)))

; conj onto nil
(assert= (list 2 1) (conj nil 1 2))

; ── GC ──

; survivors: data reachable from the root env must live through collections
(def gc-keep [1 2.5 {:a "alpha" :b (list :x :y)} "string" (fn [x] (* x x))])
(gc)
(assert= 1 (first gc-keep))
(assert= {:a "alpha" :b (list :x :y)} (nth gc-keep 2))
(assert= 49 ((nth gc-keep 4) 7))

; churn: generate garbage across collections, then verify survivors again
(loop [i 0]
  (when (< i 50000)
    (str "garbage" i [i {:k i}])
    (recur (inc i))))
(gc)
(gc)
(assert= "alpha" (:a (nth gc-keep 2)))
(assert= 49 ((nth gc-keep 4) 7))

; closures keep their captured environments alive
(def make-adder (fn [n] (fn [x] (+ x n))))
(def add10 (make-adder 10))
(gc)
(assert= 17 (add10 7))

; ── try/catch/finally + throw ──

(assert= :caught (try (throw (ex-info "boom" {})) (catch Exception e :caught)))
(assert= :ok (try :ok (catch Exception e :never)))
(assert= "boom" (ex-message (try (throw (ex-info "boom" {})) (catch Exception e e))))
(assert= 7 (:code (ex-data (try (throw (ex-info "boom" {:code 7})) (catch Exception e e)))))

; interpreter errors are catchable too; their value is the message string
(assert= true (string? (try (/ 1 0) (catch Exception e e))))
(assert= :div (try (/ 1 0) (catch Exception e :div)))
(assert= :unresolved (try no-such-symbol (catch Exception e :unresolved)))

; throw any value
(assert= 42 (try (throw 42) (catch Exception e e)))

; nested: inner try without catch rethrows outward
(assert= :outer (try (try (throw (ex-info "x" {}))) (catch Exception e :outer)))

; errors in the handler propagate
(assert= :outer2 (try
                   (try (throw 1) (catch Exception e (throw 2)))
                   (catch Exception e :outer2)))

; ── atoms ──

(def counter (atom 0))
(assert= 0 @counter)
(assert= 5 (reset! counter 5))
(assert= 6 (swap! counter inc))
(assert= 16 (swap! counter + 10))
(assert= 16 @counter)
(assert= 5 @(atom 5))

; atoms keep their contents alive across GC
(def boxed (atom [1 2 {:k "v"}]))
(gc)
(assert= [1 2 {:k "v"}] @boxed)

; finally runs on normal exit, on body throw, and on handler throw
(def fin-log (atom []))
(try :ok (finally (swap! fin-log conj :normal)))
(try (throw 1) (catch Exception e e) (finally (swap! fin-log conj :caught)))
(try (try (throw 1) (finally (swap! fin-log conj :uncaught))) (catch Exception e e))
(try
  (try (throw 1) (catch Exception e (throw 2)) (finally (swap! fin-log conj :handler-threw)))
  (catch Exception e e))
(assert= [:normal :caught :uncaught :handler-threw] @fin-log)

; exception value survives a GC between throw and catch
(assert= {:message "gced" :data {:n 1}}
         (try (throw (ex-info "gced" {:n 1}))
              (catch Exception e (do (gc) e))))

(println "tests complete")
