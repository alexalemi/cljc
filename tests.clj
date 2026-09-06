; cljc regression tests — each (assert= expected actual) prints PASS/FAIL.
; Run: ./cljc < tests.clj   (or: make test)

(defn assert= [expected actual]
  (if (= expected actual)
    (println "PASS")
    (println "FAIL: expected" (pr-str expected) "got" (pr-str actual))))

; Platform capability probes — sections that need a POSIX shell + cc (FFI,
; JIT, subprocess e2e) or coroutines (csp, futures, core.async) self-skip
; where those are absent (Windows; coro-less builds). Skipped sections print
; a SKIP marker so a sweep can tell "passed" from "not attempted".
(def cljc-test-unix? (not= :windows (cljc/os*)))
(def cljc-test-coro? (try (coro/alive? (coro/new (fn [] nil)))
                          (catch Exception e false)))
(when-not cljc-test-unix? (println "SKIP: unix-only sections (FFI/JIT/shell)"))
(when-not cljc-test-coro? (println "SKIP: coroutine sections (coro/csp/futures/core.async)"))

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
(assert= 1/2 (/ 2))       ; exact rationals now, like Clojure
(assert= 7/2 (/ 7 2))
(assert= 0.5 (/ 2.0))     ; float division stays float
(assert= 3.5 (/ 7.0 2))
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
(assert= "1/2" (str (/ 2)))
(assert= "0.5" (str (/ 2.0)))

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

; ── multi-arity fn ──

(defn greet
  ([] (greet "world"))
  ([who] (str "hello " who))
  ([who & more] (str "hello " who " and " (count more) " others")))
(assert= "hello world" (greet))
(assert= "hello rich" (greet "rich"))
(assert= "hello a and 2 others" (greet "a" "b" "c"))

; recur works inside an arity
(defn sum-to
  ([n] (sum-to n 0))
  ([n acc] (if (zero? n) acc (recur (dec n) (+ acc n)))))
(assert= 55 (sum-to 10))

; ── destructuring ──

; sequential
(assert= 3 (let [[a b] [1 2]] (+ a b)))
(assert= 1 (let [[a] [1 2 3]] a))
(assert= nil (let [[a b c] [1 2]] c))               ; nil-fills past the end
(assert= (list 2 3) (let [[_ & r] [1 2 3]] r))
(assert= [1 2 3] (let [[a :as whole] [1 2 3]] whole))
(assert= 6 (let [[[a b] c] [[1 2] 3]] (+ a b c)))   ; nested
(assert= 3 (let [[a b] (list 1 2)] (+ a b)))        ; lists destructure too

; map
(assert= 1 (let [{a :x} {:x 1}] a))
(assert= 3 (let [{:keys [x y]} {:x 1 :y 2}] (+ x y)))
(assert= 42 (let [{:keys [missing] :or {missing 42}} {}] missing))
(assert= {:x 1} (let [{:keys [x] :as m} {:x 1}] m))
(assert= 7 (let [{{:keys [inner]} :outer} {:outer {:inner 7}}] inner))

; in fn params
(defn dist2 [[x1 y1] [x2 y2]] (+ (* (- x2 x1) (- x2 x1)) (* (- y2 y1) (- y2 y1))))
(assert= 25 (dist2 [0 0] [3 4]))
(defn full-name [{:keys [first last]}] (str first " " last))
(assert= "Rich Hickey" (full-name {:first "Rich" :last "Hickey"}))

; destructured & rest
(defn first-two [& [a b]] [a b])
(assert= [1 2] (first-two 1 2 3 4))

; ── sort / compare ──
(assert= (list 1 2 3) (sort [3 1 2]))
(assert= (list 3 2 1) (sort (fn [a b] (> a b)) [1 3 2]))
(assert= (list "a" "aa" "aaa") (sort-by count ["aaa" "a" "aa"]))
(assert= -1 (compare 1 2))
(assert= 0 (compare [1 2] [1 2]))
(assert= 1 (compare [1 2 3] [1 2]))
(assert= -1 (compare [2] [1 10]))                          ; vectors: count first, then elements
(assert= (quote ([2] [1 2] [1 10])) (sort compare [[2] [1 10] [1 2]]))
(assert= -1 (compare nil 1))          ; nil sorts first
(assert= :incomparable (try (compare 1 "x") (catch Exception e :incomparable)))

; ── seq utilities ──
(assert= (list 1 1 2 2) (mapcat (fn [x] (list x x)) [1 2]))
(assert= (list 1 3) (remove even? (range 5)))
(assert= (list 0 1 2) (take-while (fn [x] (< x 3)) (range 10)))
(assert= (list 3 4) (drop-while (fn [x] (< x 3)) (range 5)))
(assert= true (every? even? [2 4 6]))
(assert= false (every? even? [2 3]))
(assert= true (some even? [1 2 3]))
(assert= nil (some even? [1 3 5]))
(assert= (list 1 :a 2 :b) (interleave [1 2] [:a :b]))
(assert= (list 1 :sep 2 :sep 3) (interpose :sep [1 2 3]))
(assert= (list (list 0 1) (list 2 3)) (partition 2 (range 5)))
(assert= (list 1 2 3) (distinct [1 2 1 3 2]))
(assert= (list 1 2) (butlast [1 2 3]))
(assert= {true [0 2] false [1 3]} (group-by even? (range 4)))
(assert= {:a 2 :b 1} (frequencies [:a :b :a]))
(assert= [3 1] ((juxt count first) [1 2 3]))
(assert= (list 2 3) (next [1 2 3]))
(assert= nil (next [1]))
(assert= [:a 1] (first {:a 1}))

; ── associative utilities ──
(assert= {:n 2} (update {:n 1} :n inc))
(assert= 3 (get-in {:a {:b 3}} [:a :b]))
(assert= :dflt (get-in {} [:a :b] :dflt))
(assert= {:a {:b 9}} (assoc-in {} [:a :b] 9))
(assert= {:a {:n 11}} (update-in {:a {:n 1}} [:a :n] + 10))
(assert= {:a 1} (select-keys {:a 1 :b 2} [:a :missing]))

; ── strings ──
(assert= "ABC" (str/upper-case "abc"))
(assert= "abc" (str/lower-case "ABC"))
(assert= "x" (str/trim "  x  "))
(assert= ["a" "b" "c"] (str/split "a,b,c" ","))
(assert= ["" "x"] (str/split ",x" ","))
(assert= "a-b-c" (str/join "-" ["a" "b" "c"]))
(assert= "abc" (str/join ["a" "b" "c"]))
(assert= true (str/starts-with? "hello" "he"))
(assert= true (str/ends-with? "hello" "lo"))
(assert= true (str/includes? "hello" "ell"))
(assert= false (str/includes? "hello" "xyz"))
(assert= true (str/blank? "  "))
(assert= false (str/blank? "x"))
(assert= "ell" (subs "hello" 1 4))
(assert= "llo" (subs "hello" 2))

; ── coercions ──
(assert= [1 2 3] (vec (list 1 2 3)))
(assert= "alpha" (name :alpha))
(assert= :beta (keyword "beta"))
(assert= true (symbol? (symbol "gamma")))
(assert= 3 (quot 7 2))
(assert= -3 (quot -7 2))

; ── control-flow macros ──
(assert= :two (case 2 1 :one 2 :two :other))
(assert= :other (case 99 1 :one :other))
(assert= :kw (case :k :k :kw :other))
(assert= 5 (if-let [x (get {:a 5} :a)] x :missing))
(assert= :missing (if-let [x (get {} :a)] x :missing))
(assert= 10 (when-let [x 5] (* x 2)))
(assert= nil (when-let [x nil] :never))
(def dots (atom 0))
(dotimes [i 4] (swap! dots + i))
(assert= 6 @dots)
(def seen (atom []))
(doseq [x [1 2 3]] (swap! seen conj x))
(assert= [1 2 3] @seen)
(def w (atom 0))
(while (< @w 5) (swap! w inc))
(assert= 5 @w)
(assert= (list 11 21 12 22) (for [x [1 2] y [10 20]] (+ x y)))
(assert= (list 1 4 9) (for [x [1 2 3]] (* x x)))

; ── batch 5-lite ──
(assert= (list [0 :a] [1 :b]) (map-indexed (fn [i x] [i x]) [:a :b]))
(assert= (list 0 2) (keep-indexed (fn [i x] (when (even? i) i)) [:a :b :c]))
(assert= (list (list 0 1) (list 2 3) (list 4)) (partition-all 2 (range 5)))
(assert= {:a 1 :b 2} (zipmap [:a :b] [1 2]))
(assert= {:a 4 :b 2} (merge-with + {:a 1} {:a 3 :b 2}))
(assert= 6 (reduce-kv (fn [acc k v] (+ acc v)) 0 {:a 1 :b 2 :c 3}))
(assert= (list 7 7 7) (repeatedly 3 (constantly 7)))
(assert= true (boolean 1))
(assert= false (boolean nil))
(assert= true (int? 3))
(assert= false (int? 3.5))
(assert= true (double? 3.5))
(def dlog (atom []))
(assert= 5 (doto 5 (->> (swap! dlog conj))))
(assert= [5] @dlog)
(assert= 8 (letfn [(f [x] (if (zero? x) 0 (+ 2 (g (dec x))))) (g [x] (f x))] (f 4)))
(assert= :ok (try (assert (= 1 1)) :ok))
(assert= true (string? (ex-message (try (assert (= 1 2)) (catch Exception e e)))))
(assert= [1 2 3] `[1 ~@(list 2 3)])   ; splice inside vector template

; ── HAMT engine ──

; big map: build, count, lookup, overwrite, dissoc back down
(def big (reduce (fn [m i] (assoc m i (* i i))) {} (range 2000)))
(assert= 2000 (count big))
(assert= 1024 (get big 32))
(assert= 3996001 (get big 1999))
(assert= :missing (get big 99999 :missing))
(assert= 2000 (count (assoc big 7 :changed)))       ; overwrite doesn't grow
(assert= :changed (get (assoc big 7 :changed) 7))
(assert= 49 (get big 7))                            ; original untouched
(def shrunk (reduce dissoc big (range 1000)))
(assert= 1000 (count shrunk))
(assert= false (contains? shrunk 500))
(assert= true (contains? shrunk 1500))
(assert= 0 (count (reduce dissoc big (range 2000))))

; structural sharing survives GC
(def base-m (reduce (fn [m i] (assoc m (keyword (str "k" i)) i)) {} (range 100)))
(def derived (assoc base-m :extra :v))
(gc)
(assert= 100 (count base-m))
(assert= 101 (count derived))
(assert= 50 (:k50 derived))

; mixed key types, hash/equality coherence
(def mixed {1 :int "1" :string :one :kw [1 2] :vec})
(assert= :int (get mixed 1))
(assert= nil (get mixed 1.0))                       ; (= 1 1.0) is false in Clojure: distinct keys
(assert= :string (get mixed "1"))
(assert= :vec (get mixed [1 2]))
(assert= :vec (get mixed (list 1 2)))               ; seq equality crosses types
(assert= 4 (count mixed))

; maps as keys (recursive hashing); map hash is order-independent
(def mk {{:a 1 :b 2} :found})
(assert= :found (get mk {:b 2 :a 1}))

; keys/vals/seq agree with each other
(def kvm {:a 1 :b 2 :c 3})
(assert= 3 (count (keys kvm)))
(assert= 6 (reduce + (vals kvm)))
(assert= (sort [:a :b :c]) (sort (keys kvm)))
(assert= true (every? (fn [[k v]] (= v (get kvm k))) (seq kvm)))

; duplicate literal keys are a reader error
; (checked manually: {:a 1 :a 2} => error: duplicate key in map literal)

; ── file IO round trip ──
(when cljc-test-unix?
(spit "/tmp/cljc-test.txt" "hello file")
(assert= "hello file" (slurp "/tmp/cljc-test.txt"))
) ; end when-unix


; ── persistent vectors ──
(def bigv (reduce conj [] (range 5000)))
(assert= 5000 (count bigv))
(assert= 0 (nth bigv 0))
(assert= 31 (nth bigv 31))      ; tail boundary
(assert= 32 (nth bigv 32))      ; first tree leaf
(assert= 1023 (nth bigv 1023))  ; one level
(assert= 1024 (nth bigv 1024))  ; two levels
(assert= 4999 (nth bigv 4999))
(assert= 4999 (last bigv))
(def bv2 (assoc bigv 2500 :changed))
(assert= :changed (nth bv2 2500))
(assert= 2500 (nth bigv 2500))  ; persistence
(assert= 5000 (count bv2))
(def bv3 (conj bigv :end))
(assert= :end (nth bv3 5000))
(assert= 5000 (count bigv))
(gc)
(assert= 2500 (nth bigv 2500))  ; structural sharing survives GC
(assert= :changed (nth bv2 2500))
(assert= true (= (vec (range 100)) (reduce conj [] (range 100))))
(assert= [1 2 :x] (assoc [1 2] 2 :x))   ; assoc at len appends
; ── sets ──
(assert= true (set? #{1 2}))
(assert= true (contains? #{:a :b} :a))
(assert= false (contains? #{:a} :z))
(assert= :a (#{:a :b} :a))
(assert= nil (#{:a} :z))
(assert= :dflt (get #{:a} :z :dflt))
(assert= :a (get #{:a} :a))
(assert= true (= #{1 2 3} #{3 2 1}))
(assert= false (= #{1 2} #{1 2 3}))
(assert= 3 (count (conj #{1 2} 3 3)))
(assert= #{1 3} (disj #{1 2 3} 2))
(assert= #{1 2 3} (set [1 2 2 3 3]))
(assert= #{1 2 3} (set/union #{1 2} #{2 3}))
(assert= #{2 3} (set/intersection #{1 2 3} #{2 3 4}))
(assert= #{1 3} (set/difference #{1 2 3} #{2}))
(assert= #{1 2} (into #{} [1 1 2 2]))
(assert= (list 1 2 3) (sort (seq #{3 1 2})))
(assert= #{[1 2] {:a 1}} (conj #{[1 2]} {:a 1}))        ; composite elements
(assert= true (contains? #{[1 2]} (list 1 2)))           ; seq-equality keys
(def bigset (reduce conj #{} (range 1000)))
(assert= 1000 (count bigset))
(assert= true (contains? bigset 999))
(assert= 999 (count (disj bigset 500)))
(gc)
(assert= true (contains? bigset 500))                    ; original survives
(assert= (list 0 2 4) (filter bigset [0 1.5 2 4 9999.5]))  ; set as predicate fn
(def evens (set (range 0 10 2)))
(assert= (list 0 2 4) (filter evens [0 1 2 3 4 5]))

; ── regex ──
(assert= "123" (re-find #"\d+" "abc123def"))
(assert= "123" (re-find "\\d+" "abc123def"))      ; plain strings work too
(assert= nil (re-find #"\d+" "abcdef"))
(assert= "aaab" (re-matches #"a+b" "aaab"))
(assert= nil (re-matches #"a+b" "aaabc"))          ; must consume all input
(assert= ["bob@example" "bob" "example"] (re-find #"(\w+)@(\w+)" "mail: bob@example"))
(assert= (list "1" "22" "333") (re-seq #"\d+" "a1b22c333"))
(assert= "color" (re-find #"colou?r" "my color!"))
(assert= "colour" (re-find #"colou?r" "my colour!"))
(assert= "start" (re-find #"^start" "start here"))
(assert= nil (re-find #"^start" "false start"))
(assert= "end" (re-find #"end$" "the end"))
(assert= "Hello" (re-find #"[A-Z][a-z]+" "say Hello there"))
(assert= "xyz" (re-find #"[^0-9]+" "123xyz"))
(assert= ["abbac" "a"] (re-find #"(a|b)+c" "xabbac!"))
(assert= "" (re-find #"x*" "yyy"))                 ; empty match is a match
(assert= "aaa" (re-find #"a*" "aaab"))             ; greedy
(assert= "" (re-find #"a*?" "aaab"))               ; lazy
(assert= ["ab" nil] (re-find #"a(z)?b" "ab"))      ; unmatched group => nil
(assert= "a.b" (re-find #"a\.b" "xa.bx"))          ; escaped dot
(assert= "2026-06-10" (re-find #"\d\d\d\d-\d\d-\d\d" "on 2026-06-10 we"))
(assert= (list "cat" "cow") (re-seq #"c\w+" "cat dog cow"))
(assert= ["key=val" "key" "val"] (re-matches #"(\w+)=(\w+)" "key=val"))
(assert= "no space" (re-find #"\S+\s\S+" "no space"))

; ── format / replace / split ──
(assert= "cart has 3 items (99.50%)" (format "%s has %d items (%.2f%%)" "cart" 3 99.5))
(assert= "x=  5;" (format "x=%3d;" 5))
(assert= "ff" (format "%x" 255))
(assert= "a-b-c" (str/replace "a.b.c" "." "-"))          ; literal, not regex
(assert= "a<1>b<22>c" (re-replace "a1b22c" #"\d+" "<$0>"))
(assert= "smith, john" (re-replace "john smith" #"(\w+) (\w+)" "$2, $1"))
(assert= "cost: $5" (re-replace "cost: 5" #"(\d+)" "$$$1"))
(assert= ["a" "b" "c" "d"] (re-split "a1b22c333d" #"\d+"))
(assert= ["a" "b"] (re-split "a,b,," ","))               ; trailing empties dropped
(assert= ["" "a" "b"] (re-split ",a,b" ","))             ; leading empty kept

; ── condp / for modifiers ──
(assert= :three (condp = 3 1 :one 3 :three :other))
(assert= :other (condp = 9 1 :one :other))
(assert= :big (condp < 5 10 :small 3 :big :other))       ; (pred test expr)
(assert= :no-clause (try (condp = 9 1 :one) (catch Exception e :no-clause)))
(assert= (list 0 20) (for [x (range 4) :when (even? x) :let [y (* x 10)]] y))
(assert= (list [1 10] [1 20] [2 10] [2 20]) (for [x [1 2] y [10 20]] [x y]))
(assert= (list 3) (for [x [1 2 3] y [3] :when (= x y)] x))

; ── math / random ──
(assert= 4.0 (Math/sqrt 16))
(assert= 1024.0 (Math/pow 2 10))
(assert= 3.0 (Math/floor 3.7))
(assert= 4.0 (Math/ceil 3.2))
(assert= 4 (Math/round 3.6))
(assert= 5 (Math/abs -5))
(assert= 2.5 (Math/abs -2.5))
(assert= true (and (<= 0 (rand)) (< (rand) 1)))
(assert= true (every? (fn [_] (<= 0 (rand-int 10) 9)) (range 50)))
(assert= true (contains? #{1 2 3} (rand-nth [1 2 3])))
(assert= 3 (count (max-key count [1] [1 2 3] [1 2])))
(assert= [1] (min-key count [1] [1 2 3] [1 2]))

; ── threading variants ──
(assert= 6 (some-> {:a {:b 5}} :a :b inc))
(assert= nil (some-> {:a 1} :missing inc))           ; short-circuits on nil
(assert= nil (some-> nil inc))
(assert= (list 3 4) (some->> [1 2 3] (map inc) (filter (fn [x] (> x 2)))))
(assert= 11 (cond-> 10 true inc false (* 100)))
(assert= 1000 (cond-> 10 false inc true (* 100)))
(assert= 10 (cond-> 10))
(assert= (list 2 4) (cond->> (range 5) true (map inc) true (filter even?)))
(assert= 30 (as-> 5 x (* x 2) (+ x 20)))

; ── read-string / eval ──
(assert= 3 (eval (read-string "(+ 1 2)")))
(assert= (list '+ 1 2) (read-string "(+ 1 2)"))
(assert= [1 2] (read-string "[1 2]"))
(assert= 42 (eval 42))
(def evaled (eval (read-string "(defn dyn-fn [x] (* x 7))")))
(assert= 21 (dyn-fn 3))

; ── peek / pop / empty ──
(assert= 3 (peek [1 2 3]))
(assert= 1 (peek (list 1 2 3)))                      ; list peeks the front
(assert= [1 2] (pop [1 2 3]))
(assert= (list 2 3) (pop (list 1 2 3)))
(assert= 33 (count (pop (vec (range 34)))))          ; pop across tail boundary
(assert= 32 (peek (pop (vec (range 34)))))
(assert= [] (empty [1 2]))
(assert= {} (empty {:a 1}))
(assert= #{} (empty #{1}))
(assert= nil (empty nil))

; ── utilities ──
(assert= nil (not-empty []))
(assert= [1] (not-empty [1]))
(assert= (list 1 2 3 4) (flatten [1 [2 [3 4]]]))
(assert= 1 ((fnil inc 0) nil))
(assert= 6 ((fnil inc 0) 5))
(assert= 3 ((fnil + 0) nil 3))
(assert= [1 2 3] (doall [1 2 3]))
(assert= nil (dorun [1 2 3]))

; ── quality-pass regressions (regex/format/splice bugs) ──
(assert= (list "" "" "") (re-seq #"x*" "ab"))        ; empty matches at 0,1,2
(assert= (list "a" "a") (re-seq #"a" "aba"))
(assert= "-a-b-" (re-replace "ab" #"x*" "-"))        ; trailing empty match
(assert= :guarded (try (re-find #"(a+)+b" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac")
                       (catch Exception e :guarded)))  ; catastrophic backtracking
(assert= 1001 (count (format "%s!" (str/join (repeat 100 "0123456789")))))
(assert= #{3 9} (let [x 3] `#{~x 9}))                ; unquote in set templates
(assert= 6 (apply + #{1 2 3}))                       ; apply splices sets
(assert= 0 (apply + {}))                             ; and (empty) maps
(assert= 14 (apply + 1 2 [4 7]))

; ── symbol-cache / root-redefinition semantics ──
(defn cache-probe [] cached-global)        ; forward reference, resolved at call
(def cached-global 1)
(assert= 1 (cache-probe))                  ; first call fills the cache
(def cached-global 2)
(assert= 2 (cache-probe))                  ; redefinition seen through mutation
(defn cache-probe2 [cached-global] cached-global)
(assert= :local (cache-probe2 :local))     ; locals still shadow cached roots
(assert= 99 (let [cached-global 99] cached-global))
(assert= 2 cached-global)
(defn redefn-test [] :v1)
(defn redefn-test [] :v2)
(assert= :v2 (redefn-test))
(def fn-using-redef (fn [] (redefn-test)))
(defn redefn-test [] :v3)
(assert= :v3 (fn-using-redef))             ; calls see the latest def

; ── AoC-portability compat additions ──
(assert= (list \a \b \c) (seq "abc"))             ; strings seq into chars
(assert= nil (seq ""))
(assert= \h (first "hello"))
(assert= 42 (parse-long "42"))
(assert= -7 (parse-long "-7"))
(assert= nil (parse-long "4x"))
(assert= nil (parse-long ""))
(assert= 2.5 (parse-double "2.5"))
(assert= nil (parse-double "abc"))
(assert= (list (list 1 2) (list 2 3) (list 3 4)) (partition 2 1 [1 2 3 4]))
(assert= (list (list 1 2) (list 3 4)) (partition 2 [1 2 3 4 5]))
(assert= [(list 1 2) (list 3 4 5)] (split-at 2 [1 2 3 4 5]))
(assert= ["a" "b" "c"] (str/split-lines "a\nb\nc"))
(assert= ["x" "y"] (str/split-lines "x\r\ny"))
(assert= [3 4] (max-key count [1 2] [3 4]))          ; ties: LAST wins, like Clojure
(assert= :b (max-key (constantly 1) :a :b))
(assert= :b (min-key (constantly 1) :a :b))
(assert= {\l 2 \h 1 \e 1 \o 1} (frequencies (seq "hello")))

; ── perf round 2: recur spill path (>3 args goes to the heap) ──
(assert= 100014 (loop [a 1 b 2 c 3 d 4 e 5]
                  (if (< a 100000) (recur (inc a) b c d e) (+ a b c d e))))
(defn five-recur [a b c d e]
  (if (< a 1000) (recur (inc a) b c d e) (+ a b c d e)))
(assert= 1014 (five-recur 1 2 3 4 5))

; ── Tier 1 compat batch ──
(assert= (list 2 4 6) (map #(* % 2) [1 2 3]))
(assert= 7 (#(+ %1 %2) 3 4))
(assert= 6 (#(apply + %&) 1 2 3))
(assert= (list 2 3) (filter #(> % 1) [0 1 2 3]))
(assert= nil (ns my.ns (:require [clojure.string :as str])))
; (ns ...) now ENTERS the namespace (defs land under my.ns/) — leave it
; again so the rest of this suite stays in the flat globals
(cljc/in-ns* nil)
(assert= nil (require '[clojure.string :as str]))
(defn docd "has a docstring" [x] (* x 2))
(assert= 42 (docd 21))
(defmacro docd-m "macro docstring" [x] `(+ ~x 1))
(assert= 4 (docd-m 3))
(assert= "a" (str \a))
(assert= " " (str \space))
(assert= "\n" (str \newline))
(assert= 3 (count (filter #(= % \a) (seq "banana"))))
; ── char type ──
(assert= true (char? \a))
(assert= false (char? "a"))                          ; a char is NOT a 1-char string
(assert= \h (first "hello"))                         ; first/nth/seq over strings yield chars
(assert= \l (nth "hello" 2))
(assert= \o (last "hello"))
(assert= \b (get "abc" 1))
(assert= 97 (int \a))
(assert= \a (char 97))
(assert= "abc" (apply str [\a \b \c]))               ; (str char) round-trips
(assert= \a (read-string (pr-str \a)))               ; readable char round-trips
(assert= \newline (read-string "\\newline"))
(assert= 233 (int \u00e9))                   ; \uXXXX hex codepoint
(assert= true (< (compare \a \b) 0))                 ; chars are comparable
(assert= (list \a \b \c) (sort "cab"))
(assert= nil (keyword \a))                            ; (keyword char) => nil, like Clojure
(assert= [1 2] ^:private [1 2])
(assert= {:private true} (meta ^:private [1 2]))
(assert= :ours #?(:clj :jvm :cljc :ours :default :other))
(assert= :fallback #?(:clj :jvm :default :fallback))
(assert= 1 #?(:clj 1 :cljs 2))                       ; :clj is a last resort (load clj-only .cljc)
(assert= :p #?(:clj :j :default :p))                 ; but :default still beats :clj
; Clojure-compat gaps (found bringing up SCI)
(def ^:dynamic ^:no-doc ^:private *stacked-meta* 9)  ; stacked metadata on def
(assert= 9 *stacked-meta*)
(defprotocol Areable (area [s]))
(extend-protocol Areable                             ; grouped-by-type protocol extension
  :int (area [n] (* n n))
  :vector (area [v] (reduce + v)))
(assert= 16 (area 4))
(assert= 6 (area [1 2 3]))
; real deftype: mutable field (set!) + protocol method dispatch on the type
(defprotocol Bumpable (bump! [x]) (cur [x]))
(deftype Cntr [^:unsynchronized-mutable c]
  Bumpable (bump! [this] (set! c (inc c)) c) (cur [this] c))
(def cntr-inst (Cntr. 10))
(bump! cntr-inst) (bump! cntr-inst)
(assert= 12 (cur cntr-inst))
(assert= :Cntr (type cntr-inst))
(deftype CompatPt [^:mutable px py])
(assert= 2 (:py (CompatPt. 1 2)))                    ; immutable field: a direct value
(assert= 1 (deref (:px (CompatPt. 1 2))))            ; mutable field: an atom
(assert= nil (comment (this is ignored)))
(assert= 11 (loop [[a b] [1 10] acc 0] (if a (recur [b nil] (+ acc a)) acc)))
(assert= 6 (loop [{:keys [n total]} {:n 3 :total 0}]
             (if (zero? n) total (recur {:n (dec n) :total (+ total n)}))))

; ── lazy sequences ──
(assert= (list 0 1 2 3 4) (take 5 (range)))            ; infinite range
(assert= (list 1 2 4) (take 3 (iterate #(* % 2) 1)))
(assert= (list :x :x :x) (take 3 (repeat :x)))
(assert= (list 1 2 1 2 1 2) (take 6 (cycle [1 2])))
(assert= (list 0 1 4 9 16) (take 5 (map #(* % %) (range))))
(assert= (list 0 2 4) (take 3 (filter even? (range))))
(assert= 1024 (first (filter #(> % 1000) (iterate #(* 2 %) 1))))
(assert= 4950 (reduce + (take 100 (range))))
(assert= (list 1 2 0 1) (take 4 (concat [1 2] (range))))
(assert= (list 5 7 9) (map + [1 2 3] [4 5 6]))         ; 2-coll map
(assert= true (= (take 3 (range)) (list 0 1 2)))       ; eq across lazy/list
(assert= true (seq? (map inc [1])))
(assert= false (seq? [1]))
(assert= 3 (count (take 3 (range))))
(assert= nil (seq (take 0 (range))))
; call-by-need with CHUNKED realization (like Clojure: <=32 at a time)
(def lz-side (atom 0))
(def lz (map (fn [x] (swap! lz-side inc) x) [1 2 3]))
(assert= 0 @lz-side)                ; nothing runs at creation
(assert= 1 (first lz))
(assert= 3 @lz-side)                ; first realizes the whole (small) chunk
(def lz-side2 (atom 0))
(def lz2 (map (fn [x] (swap! lz-side2 inc) x) (range 100)))
(first lz2)
(assert= 32 @lz-side2)              ; exactly one chunk of a big seq
; threading macros still work (their expansions are lazy concats now)
(assert= (list 2 4) (->> (range 5) (map inc) (filter even?)))
(assert= (list 2 3 4) (take-while #(< % 5) (iterate inc 2)))
(assert= (list 7 7) (take 2 (repeatedly (constantly 7))))
(gc)
(assert= (list 2 3) (take 2 (rest lz)))                ; half-realized chain survives GC

; ── Tier 3: vars/binding, multimethods, protocols, records ──
(def *lvl* 1)
(defn lvl-probe [] *lvl*)
(assert= 99 (binding [*lvl* 99] (lvl-probe)))
(assert= 1 (lvl-probe))                                   ; restored
(assert= 1 (try (binding [*lvl* 5] (throw 1)) (catch Exception e (lvl-probe))))
(assert= 50 (binding [*lvl* 5] (with-redefs [*lvl* 50] (lvl-probe))))
(defmulti t3-area :shape)
(defmethod t3-area :rect [{:keys [w h]}] (* w h))
(defmethod t3-area :default [_] :unknown)
(assert= 12 (t3-area {:shape :rect :w 3 :h 4}))
(assert= :unknown (t3-area {:shape :blob}))
(defprotocol T3Speak (t3-speak [x]))
(extend-type :string (t3-speak [s] (str s "!")))
(extend-type :int (t3-speak [n] (str "n" n)))
(assert= "hi!" (t3-speak "hi"))
(assert= "n42" (t3-speak 42))
(assert= true (satisfies? T3Speak "x"))
(assert= false (satisfies? T3Speak []))
(assert= :no-method (try (t3-speak []) (catch Exception e :no-method)))
(defrecord T3Point [x y])
(def t3p (->T3Point 3 4))
(assert= 3 (:x t3p))
(assert= :T3Point (type t3p))
(assert= true (record? t3p))
(assert= false (record? {:plain 1}))
(extend-type :T3Point (t3-speak [p] (str (:x p) "," (:y p))))
(assert= "3,4" (t3-speak t3p))
(assert= :int (type 5))
(assert= :vector (type []))

; ── bughunt: lazy stragglers (used to hang on infinite seqs) ──
(assert= (list 2 3 4) (take 3 (drop 2 (range))))
(assert= (list 0 0 1) (take 3 (mapcat (fn [x] [x x]) (range))))
(assert= (list 0 :a 1 :a) (take 4 (interleave (range) (cycle [:a]))))
(assert= (list 3 4) (drop 2 [1 2 3 4]))          ; finite drop still right
(assert= (list 1 1 2 2) (mapcat (fn [x] [x x]) [1 2]))

; ── FFI (s7 cload model: generate glue, compile, dlopen) ──
(when cljc-test-unix?  ; FFI/sh sections need cc + dlopen + a POSIX shell
(assert= 0 (:exit (sh "true")))
(assert= 1 (:exit (sh "false")))
(assert= "ping\n" (:out (sh "echo ping")))
(ffi/define [[:double cos [:double]]
             [:double hypot [:double :double]]
             [:int abs [:int]]
             [:string getenv [:string]]]
            {:headers ["math.h" "stdlib.h" "unistd.h"] :libs "-lm"})
(assert= 1.0 (cos 0.0))
(assert= 5.0 (hypot 3.0 4.0))
(assert= 7 (abs -7))
(assert= true (string? (getenv "HOME")))

; ── libc.clj batteries + FFI :pointer/caching/NULL safety ──
(load-file "libc.clj")
(assert= true (> (getpid) 0))
(assert= true (file-exists? "cljc.c"))
(assert= false (file-exists? "/no/such/path"))
(assert= nil (getenv "CLJC_DEFINITELY_UNSET"))      ; NULL char* => nil
(assert= "fb" (env "CLJC_DEFINITELY_UNSET" "fb"))
(setenv "CLJC_T" "v" 1)
(assert= "v" (getenv "CLJC_T"))
(assert= true (> (now-epoch) 1700000000))           ; :pointer arg (NULL) + ret
(assert= true (string? (cwd)))
(load-file "libc.clj")                              ; reload: cached, idempotent
(assert= true (> (getpid) 0))

; ── ffi/defstruct + json.clj ──
(ffi/defstruct timeval [[:int tv_sec] [:int tv_usec]] {:headers ["sys/time.h"]})
(ffi/define [[:int gettimeofday [:pointer :pointer]] [:void free [:pointer]]]
            {:headers ["sys/time.h" "stdlib.h"]})
(def tv (make-timeval))
(gettimeofday tv 0)
(assert= true (> (timeval-tv_sec tv) 1700000000))
(set-timeval-tv_sec! tv 42)
(assert= 42 (timeval-tv_sec tv))
(free tv)

; ── FFI :float + struct-by-value (the raylib enablers) ──
; :float marshals through as_double/mk_double with a (float) cast.
(ffi/define [[:float sqrtf [:float]] [:float powf [:float :float]]]
            {:headers ["math.h"] :libs "-lm"})
(assert= 4.0 (sqrtf 16.0))
(assert= 8.0 (powf 2.0 3.0))
; struct RETURN by value: div() returns div_t {int quot; int rem;} — comes
; back as a cljc vector of the declared fields.
(ffi/define [["div_t" div [:int :int]]]
            {:headers ["stdlib.h"]
             :structs {"div_t" [[:int "quot"] [:int "rem"]]}})
(assert= [3 2] (div 17 5))
; struct ARG by value (via a temp header): a vector is destructured into the
; struct fields with nth_elem. Covers both struct arg and struct return.
(spit "/tmp/cljc_test_ffi.h"
      (str "typedef struct { int r,g,b,a; } TColor;\n"
           "static inline int tc_luma(TColor c){ return (c.r+c.g+c.b)/3; }\n"
           "static inline TColor tc_gray(int v){ return (TColor){v,v,v,255}; }\n"))
(ffi/define [[:int "tc_luma" ["TColor"]] ["TColor" "tc_gray" [:int]]]
            {:headers ["cljc_test_ffi.h"] :libs "-I/tmp"
             :structs {"TColor" [[:int "r"] [:int "g"] [:int "b"] [:int "a"]]}})
(assert= 100 (tc_luma [90 100 110 255]))
(assert= [42 42 42 255] (tc_gray 42))
) ; end when-unix

(load-file "json.clj")
(assert= {"a" [1 2.5 true nil]} (json/parse "{\"a\": [1, 2.5, true, null]}"))
(assert= {:a {:b [1 -3]}} (json/parse "{\"a\": {\"b\": [1, -3]}}" {:keywords? true}))
(assert= [1 2 3] (json/parse "[1,2,3]"))
(assert= "x\ny" (json/parse "\"x\\ny\""))
(assert= "{\"k\":[1,null,true]}" (json/write {"k" [1 nil true]}))
(assert= {:r [1 {:d true}]} (json/parse (json/write {:r [1 {:d true}]}) {:keywords? true}))
(assert= "\"a\\\"b\"" (json/write "a\"b"))
(assert= :bad (try (json/parse "{bad}") (catch Exception e :bad)))
(assert= "a\bb" (str "a" "\b" "b"))                  ; reader \b \f escapes

; ── transient vectors ──
(def tv-base (vec (range 100)))
(def tv-done (persistent! (reduce conj! (transient tv-base) (range 100 200))))
(assert= 200 (count tv-done))
(assert= 150 (nth tv-done 150))
(assert= 100 (count tv-base))                       ; original untouched
(def tv-t (transient tv-base))
(assoc! tv-t 50 :edited)
(def tv-done2 (persistent! tv-t))
(assert= :edited (nth tv-done2 50))
(assert= 50 (nth tv-base 50))                       ; structural sharing intact
(assert= :dead (try (conj! tv-t 1) (catch Exception e :dead)))
(assert= :dead (try (assoc! tv-t 0 1) (catch Exception e :dead)))
(assert= [0 1 2] (persistent! (conj! (conj! (conj! (transient []) 0) 1) 2)))
(assert= 1002 (count (into [1 2] (range 1000))))    ; into fast path
(assert= 5000 (count (persistent! (reduce conj! (transient []) (range 5000)))))
(def tv-gc (transient (vec (range 64))))
(conj! tv-gc :x)
(gc)
(assert= :x (nth tv-gc 64))                         ; transient survives GC
(assert= 65 (count (persistent! tv-gc)))

; ── int coercion + *args* ──
(assert= 97 (int \a))
(assert= 65 (int "A"))
(assert= 3 (int 3.7))
; conformance-corpus catches: gaps found by fuzz/conformance.txt
(assert= '((1 2 3) (3 4 5) (5)) (partition-all 3 2 [1 2 3 4 5]))
(assert= '(1 :a "x" 2 :b "y") (interleave [1 2] [:a :b] ["x" "y"]))
(assert= '(1 2 3) (interleave [1 2 3]))
(assert= [[1 2] [3]] (into [] (partition-all 2) [1 2 3]))   ; xf arity intact
(assert= "AB" (.toUpperCase "ab"))
(assert= "ab" (.toLowerCase "AB"))
(assert= 1/2 (rationalize 0.5))
(assert= 1/10 (rationalize 0.1))
(assert= -3/4 (rationalize -0.75))
(assert= 3/200000 (rationalize 1.5E-5))
(assert= 2 (rationalize 2))
(assert= 1/3 (rationalize 1/3))
; range element types follow start/step promotion, end only bounds (JVM parity)
(assert= '(0 1 2) (range 2.5))
(assert= '(0 1 2) (range 0 2.5))
(assert= '(0 0.25 0.5 0.75) (range 0 1 0.25))
(assert= '(0.5 1.5 2.5) (range 0.5 3))
(assert= '(0 0.5 1.0 1.5) (range 0 2 0.5))
(assert= 0 (first (range 0 2 0.5)))                  ; int start stays an int
(assert= 0.5 (second (range 0 2 0.5)))
(assert= 42 (int 42))
(assert= [] *args*)                                  ; no args to the test run
(assert= "abc" (str/join "" (map (fn [c] c) (seq "abc"))))

; ── JIT: numeric functions compiled to native C ──
(when cljc-test-unix?  ; jit/compile! shells out to cc
(load-file "jit.clj")
(jit/defn jit-fib [n] (if (< n 2) n (+ (jit-fib (- n 1)) (jit-fib (- n 2)))))
(def jit-interp-answer (jit-fib 20))
(jit/compile! 'jit-fib)
(assert= jit-interp-answer (jit-fib 20))            ; identical semantics
(assert= 6765 (jit-fib 20))
(jit/defn jit-sum [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc i)) acc)))
(jit/compile! 'jit-sum)
(assert= 4999950000 (jit-sum 100000))
(jit/defn jit-gcd [a b] (if (zero? b) a (jit-gcd b (mod a b))))
(jit/compile! 'jit-gcd)
(assert= 6 (jit-gcd 48 18))
(jit/defn jit-lets [x] (let [a (* x 2) b (+ a 1)] (- b x)))
(jit/compile! 'jit-lets)
(assert= 11 (jit-lets 10))
(assert= :arity (try (jit-fib 1 2) (catch Exception e :arity)))
(assert= :not-int (try (jit-fib 2.5) (catch Exception e :not-int)))
(jit/defn jit-no [s] (str s "!"))                   ; outside the subset
(assert= :unsupported (try (jit/compile! 'jit-no) (catch Exception e :unsupported)))
(assert= "still works!" (jit-no "still works"))     ; interpreted version intact
; ^long/^double hints: unboxed doubles, hinted params and return
(assert= {:tag 'double} (meta '^double x))          ; reader keeps hints on symbols
(jit/defn ^double jit-dist [^double x ^double y] (Math/sqrt (+ (* x x) (* y y))))
(def jit-dist-interp (jit-dist 3.0 4.0))
(jit/compile! 'jit-dist)
(assert= jit-dist-interp (jit-dist 3.0 4.0))        ; identical semantics
(assert= 5.0 (jit-dist 3 4))                        ; int args coerce to double params
(jit/defn ^double jit-harm [^long n]                ; mixed loop: long counter, double acc
  (loop [i 1 acc 0.0] (if (> i n) acc (recur (inc i) (+ acc (/ 1.0 i))))))
(def jit-harm-interp (jit-harm 50))
(jit/compile! 'jit-harm)
(assert= jit-harm-interp (jit-harm 50))
(jit/defn jit-avg [^double a ^double b] (/ (+ a b) 2.0))
(def jit-avg-interp (jit-avg 1.0 2.0))              ; no ret hint: return type inferred
(jit/compile! 'jit-avg)
(assert= jit-avg-interp (jit-avg 1.0 2.0))
(jit/defn jit-geo [^double q ^long n] (if (zero? n) 1.0 (* q (jit-geo q (dec n)))))
(def jit-geo-interp (jit-geo 0.5 10))               ; inference through self-recursion
(jit/compile! 'jit-geo)
(assert= jit-geo-interp (jit-geo 0.5 10))
(jit/defn ^long jit-truncr [^double x] (+ x 1))     ; explicit ^long ret truncates
(jit/compile! 'jit-truncr)
(assert= 3 (jit-truncr 2.5))
(jit/defn jit-negl [^double x] (long (- x)))        ; casts + unary minus
(jit/compile! 'jit-negl)
(assert= -3 (jit-negl 3.7))
(jit/defn jit-fbad [^float x] x)                    ; only ^long/^double allowed
(assert= :badhint (try (jit/compile! 'jit-fbad) (catch Exception e :badhint)))
(jit/defn jit-idiv [a b] (/ a b))                   ; / needs a double operand
(assert= :intdiv (try (jit/compile! 'jit-idiv) (catch Exception e :intdiv)))
(jit/defn jit-drem [^double a] (rem a 2))           ; rem/quot/mod stay integer-only
(assert= :drem (try (jit/compile! 'jit-drem) (catch Exception e :drem)))
) ; end when-unix

; ── vendor: deps.edn resolution against local fixtures (no network) ──
(def cljc-test-vendor?
  (and cljc-test-unix?
       (zero? (:exit (sh "command -v git && command -v zip && command -v unzip && command -v curl")))))
(when-not cljc-test-vendor? (println "SKIP: vendor deps.edn section (needs git/zip/unzip/curl)"))
(when cljc-test-vendor?
(require 'fs)
; pure pom parsing: compile+runtime kept; test/optional/property-versioned/clojure skipped
(assert= '[[g/a {:mvn/version "1"}] [g/r {:mvn/version "2"}]]
         (vec (cljc/pom-parse-deps* (str "<project><dependencies>"
              "<dependency><groupId>g</groupId><artifactId>a</artifactId><version>1</version></dependency>"
              "<dependency><groupId>g</groupId><artifactId>t</artifactId><version>1</version><scope>test</scope></dependency>"
              "<dependency><groupId>g</groupId><artifactId>o</artifactId><version>1</version><optional>true</optional></dependency>"
              "<dependency><groupId>g</groupId><artifactId>p</artifactId><version>${v}</version></dependency>"
              "<dependency><groupId>org.clojure</groupId><artifactId>clojure</artifactId><version>1.12.0</version></dependency>"
              "<dependency><groupId>g</groupId><artifactId>r</artifactId><version>2</version><scope>runtime</scope></dependency>"
              "</dependencies></project>"))))
(assert= nil (cljc/pom-parse-deps* "<project></project>"))
; end-to-end in a subprocess (so ./vendor of this repo stays untouched):
; file:// maven repo with a pom-transitive chain, a git dep carrying its own
; deps.edn, and a :local/root dep
(let [T (str (fs/temp-dir) "/cljc-vendor-selftest")
      exe (str (cljc/exe-dir*) "/cljc")]
  (sh (str "rm -rf " (pr-str T)))
  (fs/create-dir (str T "/build/liba/liba"))
  (spit (str T "/build/liba/liba/core.clj")
        "(ns liba.core) (defn greet [n] (str \"hello \" n))")
  (fs/create-dir (str T "/build/liba/META-INF/maven/com.example/liba"))
  (spit (str T "/build/liba/META-INF/maven/com.example/liba/pom.xml")
        "<project><groupId>com.example</groupId><artifactId>liba</artifactId><version>1.0.0</version></project>")
  (fs/create-dir (str T "/build/libb/libb"))
  (spit (str T "/build/libb/libb/core.clj")
        "(ns libb.core (:require [liba.core :as a])) (defn shout [n] (str/upper-case (a/greet n)))")
  (fs/create-dir (str T "/build/libb/META-INF/maven/com.example/libb"))
  (spit (str T "/build/libb/META-INF/maven/com.example/libb/pom.xml")
        (str "<project><groupId>com.example</groupId><artifactId>libb</artifactId><version>2.0.0</version>"
             "<dependencies><dependency><groupId>com.example</groupId><artifactId>liba</artifactId><version>1.0.0</version></dependency>"
             "<dependency><groupId>org.clojure</groupId><artifactId>clojure</artifactId><version>1.12.0</version></dependency>"
             "</dependencies></project>"))
  (fs/create-dir (str T "/m2/com/example/liba/1.0.0"))
  (fs/create-dir (str T "/m2/com/example/libb/2.0.0"))
  (sh (str "cd " T "/build/liba && zip -qr " T "/m2/com/example/liba/1.0.0/liba-1.0.0.jar ."))
  (sh (str "cd " T "/build/libb && zip -qr " T "/m2/com/example/libb/2.0.0/libb-2.0.0.jar ."))
  (fs/create-dir (str T "/gitrepo/src/gitlib"))
  (spit (str T "/gitrepo/src/gitlib/core.clj")
        "(ns gitlib.core (:require [libb.core :as b])) (defn loud [n] (str (b/shout n) \"!\"))")
  (spit (str T "/gitrepo/deps.edn") "{:deps {com.example/libb {:mvn/version \"2.0.0\"}}}")
  (sh (str "cd " T "/gitrepo && git init -q && git add -A"
           " && git -c user.email=t@t -c user.name=t commit -qm x"))
  (let [sha (str/trim (:out (sh (str "cd " T "/gitrepo && git rev-parse HEAD"))))]
    (fs/create-dir (str T "/localproj/src/localx"))
    (spit (str T "/localproj/src/localx/core.clj") "(ns localx.core) (def answer 42)")
    (fs/create-dir (str T "/proj"))
    (spit (str T "/proj/deps.edn")
          (str "{:deps {io.example/gitlib {:git/url \"" T "/gitrepo\" :git/sha \"" sha "\"}"
               " local/x {:local/root \"../localproj\"}}}"))
    (spit (str T "/proj/run.clj")
          (str "(require 'fs) (require 'process)"
               "(reset! cljc/vendor-repos* [[\"file://" T "/m2/\" \"local-m2\"]])"
               "(cljc/vendor-deps! \"deps.edn\")"))
    (let [r (sh (str "cd " T "/proj && " exe " run.clj"))]
      (assert= 0 (:exit r))
      (assert= true (str/includes? (:out r) "(require 'gitlib.core)   ; loads OK"))
      (assert= true (str/includes? (:out r) "(require 'liba.core)   ; loads OK"))
      (assert= true (str/includes? (:out r) "(require 'libb.core)   ; loads OK"))
      (assert= true (str/includes? (:out r) "(require 'localx.core)   ; loads OK"))
      (assert= true (fs/exists? (str T "/proj/vendor/liba/core.clj")))
      (assert= true (fs/exists? (str T "/proj/vendor/gitlib/core.clj")))))
  (sh (str "rm -rf " (pr-str T))))
) ; end when cljc-test-vendor?

; ── bundle --library: C-ABI shared library round-trip ──
; Compiles all of cljc.c once (-O0 keeps it quick); unix-only, needs cc and
; the cljc.c source in cwd. CLJC_GC_STRESS is cleared for the subprocesses:
; the generated C is a megabyte-scale string and stress-GC'ing its assembly
; tests nothing this section is about.
(when (and cljc-test-unix? (cljc/slurp-maybe "cljc.c"))
(require 'fs)
(let [T (str (fs/temp-dir) "/cljc-libtest")
      exe (str (cljc/exe-dir*) "/cljc")]
  (sh (str "rm -rf " (pr-str T)))
  (fs/create-dir T)
  (spit (str T "/greet.clj") "(defn greet [n] (str \"hello, \" n \"!\"))")
  (spit (str T "/host.c")
        (str "#include <stdio.h>\n"
             "int cljc_lib_init(void);\n"
             "const char *cljc_lib_eval(const char *src);\n"
             "const char *cljc_lib_last_error(void);\n"
             "int main(void) {\n"
             "  if (cljc_lib_init() != 0) return 1;\n"
             "  printf(\"%s\\n\", cljc_lib_eval(\"(greet \\\"lib\\\")\"));\n"
             "  const char *bad = cljc_lib_eval(\"(nope)\");\n"
             "  printf(\"%s\\n\", bad ? \"BAD\" : \"got-error\");\n"
             "  printf(\"%s\\n\", cljc_lib_eval(\"(+ 1 2)\"));\n"
             "  return 0;\n}\n"))
  (let [rb (sh (str "env -u CLJC_GC_STRESS " exe " bundle --library --cflags=-O0 "
                    T "/greet.clj " T "/libgreet.so"))]
    (assert= 0 (:exit rb))
    (assert= true (fs/exists? (str T "/libgreet.so.h")))
    (let [rc (sh (str "cd " T " && cc -O0 host.c ./libgreet.so -o host 2>&1"))]
      (assert= 0 (:exit rc))
      ; subshell: cljc's sh appends 2>&1, which would override a bare
      ; 2>/dev/null and pull the expected eval-error print into :out
      (let [r (sh (str "(cd " T " && LD_LIBRARY_PATH=. ./host 2>/dev/null)"))]
        (assert= 0 (:exit r))
        (assert= "\"hello, lib!\"\ngot-error\n3\n" (:out r)))))
  (sh (str "rm -rf " (pr-str T)))))

; ── library survey: upstream clojure.set via loading require ──
(require '[clojure.set :refer [union intersection difference rename-keys map-invert project join select subset?]])
(assert= #{1 2 3} (union #{1 2} #{2 3}))
(assert= #{2 3} (intersection #{1 2 3} #{2 3 4}))
(assert= #{1 3} (difference #{1 2 3} #{2}))
(assert= {:x 1 :b 2} (rename-keys {:a 1 :b 2} {:a :x}))
(assert= {1 :a 2 :b} (map-invert {:a 1 :b 2}))
(assert= #{{:a 1} {:a 3}} (project #{{:a 1 :b 2} {:a 3 :b 4}} [:a]))
(assert= #{{:id 1 :name :ann :age 30}}
         (join #{{:id 1 :name :ann}} #{{:id 1 :age 30}}))
(assert= #{1 3 5} (select odd? #{1 2 3 4 5}))
(assert= true (subset? #{1} #{1 2}))
(require 'clojure.set)                               ; idempotent reload
(assert= #{1 2} (union #{1} #{2}))
(assert= true (identical? :a :a))
(assert= false (identical? [1] [1]))
(assert= {:a 1} (persistent! (assoc! (transient {}) :a 1)))   ; map shim
(assert= #{:x} (persistent! (conj! (transient #{}) :x)))      ; set shim

; ── library survey round 2: medley + clojure.walk + enabling compat ──
(assert= [1 2 3 4] [1 #?@(:cljc [2 3]) 4])          ; #?@ splicing
(assert= [1 2] [1 #_(ignored junk) 2])               ; #_ discard
(def coll' [1 2])                                    ; apostrophe in symbols
(assert= 2 (count coll'))
(assert= 120 ((fn fact [n] (if (zero? n) 1 (* n (fact (dec n))))) 5))  ; named fn
(assert= :early (reduce (fn [a x] (if (> a 10) (reduced :early) (+ a x))) 0 (range 100)))
(assert= 6 (unreduced (reduce + [1 2 3])))
(assert= {:a 1 :b 2} (conj {:a 1} [:b 2]))           ; entries conj onto maps
(assert= {:a 1 :b 2} (conj {:a 1} {:b 2}))
(assert= true (coll? [1]))
(assert= false (coll? "s"))
(require '[medley.core :as m])
(assert= {:a 1 :c 3} (m/assoc-some {:a 1} :b nil :c 3))
(assert= 4 (m/find-first even? [1 3 4 5]))
(assert= {:a {:x 1 :y 2}} (m/deep-merge {:a {:x 1}} {:a {:y 2}}))
(assert= {:a 1} (m/remove-vals nil? {:a 1 :b nil}))
(assert= (list 1 :a 2 :b 3) (m/interleave-all [1 2 3] [:a :b]))
(assert= {:b 3 :a 2} (m/map-vals inc {:a 1 :b 2}))
(require '[clojure.walk :refer [postwalk keywordize-keys prewalk-replace]])
(assert= {:a 2 :b [3 4]} (postwalk (fn [x] (if (int? x) (inc x) x)) {:a 1 :b [2 3]}))
(assert= {:a 1 :b {:c 2}} (keywordize-keys {"a" 1 "b" {"c" 2}}))
(assert= [:y [:y :z]] (prewalk-replace {:x :y} [:x [:x :z]]))

; ── metadata + clojure.zip (the structural unlock) ──
(assert= {:k :v} (meta (with-meta [1] {:k :v})))
(assert= nil (meta [1]))
(assert= [1] (with-meta [1] {:k :v}))                ; meta invisible to =
(assert= {:a 1 :b 2} (meta (vary-meta (with-meta [] {:a 1}) assoc :b 2)))
(assert= 42 (with-meta 42 {}))   ; lenient: metadata on an immutable scalar is ignored, not an error
(require '[clojure.zip :as z])
(def zt (z/vector-zip [1 [2 3] 4]))
(assert= 3 (-> zt z/down z/right z/down z/right z/node))
(assert= [2 [2 3] 4] (z/root (z/edit (-> zt z/down) inc)))
(assert= [:X [2 3] 4] (-> zt z/down (z/replace :X) z/root))
(assert= [1 [3] 4] (-> zt z/down z/right z/down z/remove z/root))

; ── metadata propagation through collection ops ──
(assert= {:m 1} (meta (conj (with-meta [] {:m 1}) 2)))
(assert= {:m 2} (meta (assoc (with-meta {} {:m 2}) :k :v)))
(assert= {:m 3} (meta (dissoc (assoc (with-meta {} {:m 3}) :k 1) :k)))
(assert= {:m 4} (meta (conj (with-meta #{} {:m 4}) :x)))
(assert= {:m 5} (meta (conj (with-meta (list 1) {:m 5}) 0)))
(assert= {:m 6} (meta (assoc (with-meta [1 2] {:m 6}) 0 :x)))
(assert= {:m 7} (meta (into (with-meta [] {:m 7}) [1 2 3])))  ; transient roundtrip
(assert= {:m 8} (meta (update (with-meta {:n 1} {:m 8}) :n inc)))
(assert= nil (meta (conj [] 1)))                     ; no meta, no cost
; ── reify over the type/multimethod machinery ──
(defprotocol RTest (r-go [x]))
(def r-obj (reify RTest (r-go [_] :reified)))
(assert= :reified (r-go r-obj))
(assert= [1 2] (subvec [0 1 2 3] 1 3))
(assert= "\f" (str \formfeed))
(assert= 11 (int \o013))

; ── lazy-tail truncation bug (the reify reproducer, now fixed) ──
(defmacro lt-do [& forms]
  (cons 'do (concat (list (first forms)) (rest forms))))   ; lazy-tailed expansion
(def lt-probe (atom []))
(lt-do (swap! lt-probe conj 1) (swap! lt-probe conj 2) (swap! lt-probe conj 3))
(assert= [1 2 3] @lt-probe)                                ; was [] — silent truncation
(defmacro lt-reify [& clauses]                             ; the original lazy reify shape
  (let [t (keyword (str (gensym)))
        impls (filter list? clauses)]
    `(do ~@(map (fn [[m params & body]]
                  `(defmethod ~m ~t ~(vec params) ~@body))
                impls)
         {:cljc/type ~t})))
(defprotocol LTP (lt-go [x]) (lt-go2 [x]))
(def lt-obj (lt-reify LTP (lt-go [_] :a) (lt-go2 [_] :b)))
(assert= :a (lt-go lt-obj))
(assert= :b (lt-go2 lt-obj))
(assert= 6 (eval (cons '+ (map inc [0 1 2]))))             ; lazy call form

; ── transducers ──
(assert= 30 (transduce (comp (map-xf inc) (filter-xf even?)) + 0 (range 10)))
(assert= [0 1 2] (transduce (take-xf 3) conj (range)))      ; infinite + early exit
(assert= (list 0 1 4 9) (sequence*2 (comp (map-xf (fn [x] (* x x))) (take-xf 4)) (range)))
(assert= (list 1 2 3) (eduction (map-xf inc) (take-xf 3) (range)))
(assert= [1 3] (transduce (keep-xf (fn [x] (when (odd? x) x))) conj [1 2 3 4]))
(assert= [0 0 1 1] (transduce (comp (mapcat-xf (fn [x] [x x])) (take-xf 4)) conj (range)))
(assert= [1 2 3] (transduce (distinct-xf) conj [1 1 2 3 2]))
(assert= [] (conj))                                          ; (conj) => []
(assert= [3 4] (reduce (fn [a x] (if (> x 4) (reduced a) (conj a x))) [3] [4 5 6]))
(assert= :inf (reduce (fn [a x] (if (> x 100) (reduced :inf) a)) nil (range)))  ; lazy reduce

; ── cheap tier ──
(assert= (list 1 2 3 1) (dedupe [1 1 2 2 3 1]))
(assert= (list (list 1 1) (list 2 4) (list 5)) (partition-by even? [1 1 2 4 5]))
(assert= [(list 0 1 2) (list 3 4 5)] (split-with (fn [x] (< x 3)) (range 6)))
(assert= 5 (count (tree-seq list? seq (list 1 (list 2 3)))))
(assert= (list 1 2) (take 2 (lazy-cat [1] [2 3])))
(assert= true (not-any? even? [1 3]))
(assert= true (not-every? even? [2 3]))
(assert= {:a [1 2]} (edn/read-string "{:a [1 2]}"))

; ── fs.clj / process.clj batteries ──
(when cljc-test-unix?  ; fs.clj is FFI-built; process tests use POSIX tools
(load-file "fs.clj")
(load-file "process.clj")
(assert= true (fs/exists? "cljc.c"))
(assert= true (fs/directory? "vendor"))
(assert= false (fs/directory? "cljc.c"))
(assert= true (contains? (set (fs/list-dir "vendor")) "clojure"))
(assert= "c.clj" (fs/file-name "/a/b/c.clj"))
(assert= "/a/b" (fs/parent "/a/b/c.clj"))
(assert= "gz" (fs/extension "x.tar.gz"))
(fs/create-dir "/tmp/cljc-fs-suite")
(assert= true (fs/directory? "/tmp/cljc-fs-suite"))
(fs/delete "/tmp/cljc-fs-suite")
(assert= false (fs/exists? "/tmp/cljc-fs-suite"))
(assert= "hello world's" (process/out "echo" "hello world's"))
(assert= 0 (:exit (process/sh "true")))
(assert= :threw (try (process/shell "false") (catch Exception e :threw)))
;; regression: under (require 'process) the ns-local resolution used to make
;; process/sh's internal bare `sh` mean ITSELF — an infinite tail-call loop
(require 'process)
(assert= 0 (:exit (process/sh "true")))
;; regression: mkdir-p creates PARENTS (it was a bare one-level mkdir)
(assert= true (fs/create-dir "/tmp/cljc-fs-suite/deep/nested"))
(assert= true (fs/directory? "/tmp/cljc-fs-suite/deep/nested"))
(sh "rm -rf /tmp/cljc-fs-suite")
) ; end when-unix

; str/index-of
(assert= 6 (str/index-of "hello world" "world"))
(assert= 2 (str/index-of "ababab" "ab" 1))
(assert= nil (str/index-of "abc" "zz"))
(assert= 0 (str/index-of "abc" "abc"))

; with-out-str
(assert= "hi42" (with-out-str (print "hi") (print 42)))
(assert= "" (with-out-str))
(assert= :threw (try (with-out-str (throw (ex-info "x" {}))) (catch Exception e :threw)))
(assert= "restored" (do (try (with-out-str (throw (ex-info "x" {}))) (catch Exception e nil))
                        (with-out-str (print "restored"))))
(assert= "ab" (with-out-str (print "a") (with-out-str (print "hidden")) (print "b")))

; cljc/mtime* — milliseconds since epoch, nil when missing
(assert= true (> (cljc/mtime* "tests.clj") 1500000000000))
(assert= nil (cljc/mtime* "/no/such/file/anywhere"))

; tcp primitives — listen + accept timeout + close (loopback)
(assert= true (let [srv (tcp/listen 17893)]
                (let [r (tcp/accept srv 10)]   ; no client: times out → nil
                  (tcp/close srv)
                  (nil? r))))

; clerk notebook battery
(load-file "clerk.clj")
(assert= [{:kind :md :text "# Title"}
          {:kind :code :text "(+ 1 2)" :dirs {}}]
         (clerk/parse ";; # Title\n\n(+ 1 2)\n"))
(assert= 2 (count (clerk/parse "(defn f [x]\n  (* x 2))\n\n(f 21)\n")))
(assert= :code (:kind (first (clerk/parse "(def s \";; not prose\")\n"))))
(assert= 1 (count (clerk/parse "(def a\n  1)\n")))           ; multiline form = one cell
(assert= {"hide-code" true} (:dirs (second (clerk/parse ";; intro\n;; @clerk:hide-code\n(+ 1 1)\n"))))
(assert= {:value 3 :error nil :out ""} (clerk/eval-cell "(+ 1 2)"))
(assert= {:value 2 :error nil :out "side"} (clerk/eval-cell "(print \"side\") (+ 1 1)"))
(assert= true (string? (:error (clerk/eval-cell "(/ 1 0)"))))
(assert= 42 (do (clerk/eval-cell "(def clerk-test-x 42)") clerk-test-x))  ; defs persist
(assert= "&lt;a&gt; &amp; b" (clerk/escape "<a> & b"))
(assert= "<h1>Hi</h1>" (clerk/md->html "# Hi"))
(assert= "<p>a <strong>b</strong> <em>c</em> <code>d</code></p>" (clerk/md->html "a **b** *c* `d`"))
(assert= "<p><a href=\"http://x.y\">go</a></p>" (clerk/md->html "[go](http://x.y)"))
(assert= "<ul><li>one</li><li>two</li></ul>" (clerk/md->html "- one\n- two"))
(assert= "<ol><li>a</li><li>b</li></ol>" (clerk/md->html "1. a\n2. b"))
(assert= "<p>x</p><p>y</p>" (clerk/md->html "x\n\ny"))
(assert= "<p>math $e^x$ stays</p>" (clerk/md->html "math $e^x$ stays"))
(assert= true (str/includes? (clerk/hl "(def x \"s\")") "<span class=\"str\">\"s\"</span>"))
(assert= true (str/includes? (clerk/hl "; note") "class=\"com\""))
(assert= true (str/includes? (clerk/hl ":kw") "class=\"kwd\""))
(assert= true (str/includes? (clerk/render-value {:a 1}) "<table>"))
(assert= true (str/includes? (clerk/render-value [{:a 1} {:a 2}]) "<th>"))
(assert= "<div class=\"raw\"><b>x</b></div>" (clerk/render-value "<b>x</b>"))
(assert= true (str/includes? (clerk/render-value [1 2 3]) "class=\"value\""))
(when cljc-test-unix?  ; notebook render tests write under /tmp
; incremental render cache: re-render evaluates only changed cells + dependents
(def clerk-log (atom []))
(spit "/tmp/cljc-nb.clj"
      ";; # t\n(def na 1)\n(def nb (do (swap! clerk-log conj :b) (* na 10)))\n(def nc (do (swap! clerk-log conj :c) 9))\n")
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= [:b :c] @clerk-log)                          ; first render: both cells run
(reset! clerk-log [])
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= [] @clerk-log)                               ; unchanged: nothing re-runs
(spit "/tmp/cljc-nb.clj"
      ";; # t\n(def na 2)\n(def nb (do (swap! clerk-log conj :b) (* na 10)))\n(def nc (do (swap! clerk-log conj :c) 9))\n")
(reset! clerk-log [])
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= [:b] @clerk-log)                             ; edit na: only dependent nb re-runs
(sh "rm -f /tmp/cljc-nb.clj")
; redefinition ordering across renders: a later cell's redef wins; editing the
; earlier def keeps the later one winning; removing the redef falls back
(spit "/tmp/cljc-nb.clj" ";; #\n(defn rf [] 1)\n(defn rf [] 2)\n(def rres (rf))\n")
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= 2 rres)                                      ; B's redef wins
(spit "/tmp/cljc-nb.clj" ";; #\n(defn rf [] 5)\n(defn rf [] 2)\n(def rres (rf))\n")
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= 2 rres)                                      ; edit A: B's redef still wins (not 5)
(spit "/tmp/cljc-nb.clj" ";; #\n(defn rf [] 5)\n(def junk 7)\n(def rres (rf))\n")
(clerk/render-file "/tmp/cljc-nb.clj" false)
(assert= 5 rres)                                      ; remove B's redef: falls back to A
(sh "rm -f /tmp/cljc-nb.clj")
) ; end when-unix

; defonce
(defonce cljc-defonce-probe (atom 0))
(swap! cljc-defonce-probe inc)
(defonce cljc-defonce-probe (atom 0))
(assert= 1 @cljc-defonce-probe)

; dir natives + clerk directory walking
(assert= true (cljc/dir?* "vendor"))
(assert= false (cljc/dir?* "cljc.c"))
(assert= false (cljc/dir?* "/no/such/dir"))
(assert= true (vector? (cljc/list-dir* "vendor")))
(assert= nil (cljc/list-dir* "/no/such/dir"))
(when cljc-test-unix?  ; clerk dir walking builds a /tmp tree via fs.clj
(fs/create-dir "/tmp/cljc-clerk-walk")
(fs/create-dir "/tmp/cljc-clerk-walk/sub")
(fs/create-dir "/tmp/cljc-clerk-walk/.git")
(spit "/tmp/cljc-clerk-walk/a.clj" "(+ 1 1)")
(spit "/tmp/cljc-clerk-walk/sub/b.clj" "(+ 2 2)")
(spit "/tmp/cljc-clerk-walk/.git/c.clj" "(+ 3 3)")
(spit "/tmp/cljc-clerk-walk/notes.txt" "hi")
(assert= #{"/tmp/cljc-clerk-walk/a.clj" "/tmp/cljc-clerk-walk/sub/b.clj"}
         (set (cljc/clerk-walk "/tmp/cljc-clerk-walk")))
(assert= "/tmp/cljc-clerk-walk/sub/b.clj"
         (cljc/clerk-changed (dissoc (cljc/clerk-snapshot "/tmp/cljc-clerk-walk")
                                     "/tmp/cljc-clerk-walk/sub/b.clj")
                             (cljc/clerk-snapshot "/tmp/cljc-clerk-walk")))
(assert= nil (let [s (cljc/clerk-snapshot "/tmp/cljc-clerk-walk")] (cljc/clerk-changed s s)))
(sh "rm -rf /tmp/cljc-clerk-walk")
) ; end when-unix

; transducer arities — (map f) etc. are transducers
(assert= [1 3 5 7 9] (into [] (comp (map inc) (filter odd?)) (range 10)))
(assert= [0 2 4 6] (into [] (remove odd?) (range 8)))
(assert= '([0 0] [1 1] [2 2]) (sequence (comp (take 3) (map-indexed vector)) (range)))
(assert= [1 2 3 4] (transduce (comp cat (distinct)) conj [[1 2] [2 3] [3 4]]))
(assert= [[0 1 2] [3 4 5] [6 7]] (into [] (partition-all 3) (range 8)))
(assert= [[1 1] [2 4] [5]] (into [] (partition-by odd?) [1 1 2 4 5]))
(assert= [1 :sep 2 :sep 3] (into [] (interpose :sep) [1 2 3]))
(assert= [1 2 3 1] (into [] (dedupe) [1 1 2 2 3 1]))
(assert= [0 1 2] (into [] (comp (drop-while neg?) (take-while (fn [x] (< x 3)))) [-2 -1 0 1 2 3 4]))
(assert= [:b :d] (into [] (keep-indexed (fn [i x] (when (odd? i) x))) [:a :b :c :d]))
(assert= [2 3] (into [] (comp (drop 1) (take 2)) [1 2 3 4]))
(assert= '(3 4 1 2) (into () (mapcat reverse) [[1 2] [3 4]]))
(assert= [:x] (into [] (keep (fn [v] (when (= v 1) :x))) [0 1 2]))
(assert= 3 (transduce (map identity) (completing conj count) [] [9 9 9]))
(assert= 26 (transduce (comp (map rest)
                             (map (fn [v] (map parse-long v)))
                             (map (partial apply *)))
                       + (re-seq #"mul\((\d+),(\d+)\)" "xmul(2,3)ymul(4,5)z")))
(assert= 3 (transduce (take 3) + (range)))          ; early termination
(assert= [1 2] (sequence (map identity) [1 2]))
(assert= [9 1 2] (into [9] (map inc) [0 1]))
; 2-arity (and seq) forms unchanged by the redefinition
(assert= '(2 3) (map inc [1 2]))
(assert= '(11 22) (map + [1 2] [10 20]))
(assert= '(1 3) (filter odd? [1 2 3]))
(assert= '(0 1 2) (take 3 (range)))
(assert= '(2 3) (drop 1 [1 2 3]))
(assert= '(1 2) (distinct [1 1 2]))
(assert= '((1 2) (3)) (partition-all 2 [1 2 3]))

; case — list branches match members; no match without default throws
(assert= :slide (case \> (\> \<) :slide :other))
(assert= :other (case \^ (\> \<) :slide :other))
(assert= :mid (case 5 (1 2 3) :low (4 5 6) :mid :high))
(assert= :threw (try (case :zz :a 1) (catch Exception e :threw)))
(assert= :dflt (case :zz :a 1 :dflt))

; bit ops
(assert= 7 (bit-or 1 2 4))
(assert= 2 (bit-and 6 3))
(assert= 6 (bit-xor 5 3))
(assert= -1 (bit-not 0))
(assert= 1024 (bit-shift-left 1 10))
(assert= -4 (bit-shift-right -16 2))
(assert= 15 (unsigned-bit-shift-right -1 60))
(assert= true (bit-test 5 2))
(assert= 8 (bit-set 0 3))
(assert= 14 (bit-clear 15 0))
(assert= 4 (bit-and-not 5 1))

; missing-builtins batch
(assert= \a (char 97))
(assert= \a (char "a"))
(assert= '(1 3 6) (reductions + [1 2 3]))
(assert= '(10 11 13) (reductions + 10 [1 2]))
(assert= true ((every-pred pos? odd?) 3 5))
(assert= false ((every-pred pos? odd?) 3 4))
(assert= true ((some-fn neg? odd?) 2 3))
(assert= false ((some-fn neg? odd?) 2 4))   ; some-fn returns logical false, not nil
(assert= 3 ((memoize +) 1 2))
(assert= '(3 4) (take-last 2 [1 2 3 4]))
(assert= '(1 2) (drop-last [1 2 3]))
(assert= '(1) (drop-last 2 [1 2 3]))
(assert= '(0 3 6 9) (take-nth 3 (range 10)))
(assert= [0 2 4] (into [] (take-nth 2) (range 6)))
(assert= {"a" 1} (update-keys {:a 1} name))
(assert= {:a 2} (update-vals {:a 1} inc))
(assert= "a-bXc" (str/replace-first "aXbXc" "X" "-"))
(assert= "aXbXc" (str/replace-first "aXbXc" "Z" "-"))
(assert= "aa" (re-find (re-pattern "a+") "caat"))
(assert= :int (class 1))
(assert= true (boolean? false))
(assert= false (boolean? nil))
(assert= true (nat-int? 0))
(assert= false (nat-int? -1))
(assert= true (sequential? [1]))
(assert= false (sequential? {:a 1}))

; cons onto lazy-seq: print/equality/nth/count see through lazy tails
(assert= "(10 11 13)" (pr-str (cons 10 (lazy-seq (list 11 13)))))
(assert= true (= (cons 1 (lazy-seq (list 2))) (list 1 2)))
(assert= 6 (nth (map inc (range)) 5))
(assert= 2 (nth (cons 0 (lazy-seq (list 1 2))) 2))
(assert= 3 (count (cons 0 (lazy-seq (list 1 2)))))

; clojure.test + clojure.string shims; ns with [:require ...] vectors
(ns cljc-test-shim-probe [:require [clojure.test :as ctest]
                          [clojure.string :as string]])
(ctest/deftest cljc-shim-works (ctest/is (= 4 (+ 2 2))) (ctest/are [x y] (= x y) 1 1 2 2))
(assert= true (fn? ctest/run-tests))
(assert= ["a" "b"] (string/split "a b" #" "))
(assert= "HI" (clojure.string/upper-case "hi"))

; regex-aware str/split (regex literals carry :regex meta; strings stay literal)
(assert= ["a" "b"] (str/split "a\n\nb" #"\n\n"))
(assert= ["x" "y" "z"] (str/split "x1y22z" #"\d+"))
(assert= ["10" "5" "5"] (str/split "10R5L5" #"R|L"))
(assert= ["a" "b"] (str/split "a.b" "."))          ; literal: dot not regex
(assert= ["a" "b"] (str/split "a b" (re-pattern " ")))
(assert= {:regex true} (meta #"x+"))
(assert= "tagged" (with-meta "tagged" {:m 1}))     ; strings can carry meta now
(assert= {:m 1} (meta (with-meta "s" {:m 1})))

; def with meta on the name; ## literals; assert with message
(def ^:dynamic cljc-dyn-probe 7)
(assert= 7 cljc-dyn-probe)
(assert= true (< 1e300 ##Inf))
(assert= true (> -1e300 ##-Inf))
(assert= false (= ##NaN ##NaN))
(assert= :threw (try (assert false "context here") (catch Exception e :threw)))
(assert= nil (assert true "fine"))

; time / == / distinct? / deftype-stub / queue / priority peek-pop on maps
(assert= 7 (let [v (time (+ 3 4))] v))
(assert= true (== 1 1.0))
(assert= true (distinct? 1 2 3))
(assert= false (distinct? 1 1))
(deftype CljcProbeT [a b])                            ; plain (immutable) deftype
(assert= [1 2 :CljcProbeT] [(:a (CljcProbeT. 1 2)) (:b (CljcProbeT. 1 2)) (type (CljcProbeT. 1 2))])
(assert= [] clojure.lang.PersistentQueue/EMPTY)
(assert= [:b 2] (peek {:a 5 :b 2 :c 9}))
(assert= {:a 5} (pop {:a 5 :b 2}))
(assert= 3 (peek [1 2 3]))            ; vector peek unchanged
(assert= [1 2] (pop [1 2 3]))
(assert= 1 (peek (list 1 2)))

; vendor shims: priority-map, json, combinatorics
(require '[clojure.data.priority-map :refer [priority-map]])
(assert= [:y 1] (peek (assoc (priority-map :x 4) :y 1)))
(require '[clojure.math.combinatorics :as combo])
(assert= '([1 2] [1 3] [2 3]) (combo/combinations [1 2 3] 2))
(assert= 6 (count (combo/permutations [1 2 3])))
(assert= 8 (count (combo/subsets [1 2 3])))
(assert= '((1 :a) (1 :b) (2 :a) (2 :b)) (combo/cartesian-product [1 2] [:a :b]))
(require '[clojure.data.json :as cjson])
(assert= {:a 1} (cjson/read-str "{\"a\": 1}" :key-fn keyword))
(assert= "{\"a\":1}" (cjson/write-str {:a 1}))

; batch 4: flatten over lazy, top-level #_, mapv multi-coll, aliases, re-matches backtracking
(assert= '(1 2 3 1 2 3) (flatten (repeat 2 (concat [1 2] [3]))))
(assert= '(1 2 3 4) (flatten [[1 [2 3]] 4]))
#_(this form is discarded entirely at top level)
(assert= [2 4 6] (mapv * (repeat 3 2) [1 2 3]))
(assert= 12 (*' 3 4))
(assert= 5 (+' 2 3))
(assert= true (char? \a))
(assert= false (char? "ab"))
(assert= false (char? 7))
(assert= "xy" (re-matches #"x|xy" "xy"))           ; backtracks into alternation
(assert= ["steps" "steps"] (re-matches #"(step|steps)" "steps"))
(assert= ["rotate left 6 steps" "left" "6" "steps"]
         (re-matches #"rotate (left|right) (\d+) (step|steps)" "rotate left 6 steps"))
(assert= nil (re-matches #"a+" "aab"))
(assert= ["aab" "aa" "b"] (re-matches #"(a*)(b)" "aab"))
(assert= "x" (re-find #"x|xy" "xy"))               ; re-find stays unanchored

; md5 native + the canonical Java digest idiom verbatim
(assert= "d41d8cd98f00b204e9800998ecf8427e" (cljc/md5* ""))
(assert= "900150983cd24fb0d6963f7d28e17f72" (cljc/md5* "abc"))
(assert= "9e107d9d372bb6826bd81d3542a419d6" (cljc/md5* "The quick brown fox jumps over the lazy dog"))
(defn cljc-test-md5 [s]
  (let [algorithm (MessageDigest/getInstance "MD5")
        raw (.digest algorithm (.getBytes s))]
    (format "%032x" (BigInteger. 1 raw))))
(assert= "000001dbbfa3a5c83a2d506429c7b00e" (cljc-test-md5 "abcdef609043"))

; interop shims
(assert= 2 (.indexOf "hello" "llo"))
(assert= -1 (.indexOf "hello" "z"))
(assert= 2 (.indexOf [4 5 6] 6))
(assert= -1 (.indexOf [1] 9))
(assert= 5 (Integer/parseInt "101" 2))
(assert= 42 (Integer/parseInt "42"))
(assert= 15 (Character/digit \f 16))
(assert= -1 (Character/digit \z 10))
(assert= 2.0 (Math/sqrt 4))
(assert= :threw (try (throw (AssertionError. "boom")) (catch Exception e :threw)))

; variadic map (transpose idiom) + regex lookahead
(assert= '((1 3 5) (2 4 6)) (apply map list [[1 2] [3 4] [5 6]]))
(assert= '(9 18) (apply map - [[10 20] [1 2]]))
(assert= '(111 222) (map + [1 2] [10 20] [100 200]))
(assert= '("o" "2" "tw") (re-seq #"\d|o(?=ne)|tw(?=o)" "one2two"))
(assert= "a" (re-find #"a(?!b)" "ab ac"))
(assert= "foobar" (re-matches #"foo(?=bar)bar" "foobar"))
(assert= nil (re-find #"a(?!b)" "ab"))

; (ns x) enters x: defs land under x/, syntax-quote qualifies defined syms
(ns cljc.nstest)
(defn nstest-fn [v] v)
(def nstest-form `(nstest-fn 1))
(assert= 'cljc.nstest/nstest-fn (first nstest-form))
(assert= :ok (case (first nstest-form) cljc.nstest/nstest-fn :ok :nope))
(assert= 5 (nstest-fn 5))                  ; home-ns resolution
(assert= 6 (+ 1 5))                        ; core fns still reachable
(cljc/in-ns* nil)

; batch 7: conj on lazy seqs, dynamic vars under ns, variadic set ops,
; mapv n-coll, sort-by comparator, alias-over-bare precedence
(assert= '(:x 0 1) (conj (take 2 (range 5)) :x))
(assert= '(:y :x 0) (conj (take 1 (range 5)) :x :y))
(ns cljc.bindns)
(def ^:dynamic *bindns-probe* 1)
(assert= 9 (binding [*bindns-probe* 9] *bindns-probe*))
(assert= 1 *bindns-probe*)
(cljc/in-ns* nil)
(assert= #{2} (set/intersection #{1 2} #{2 3} #{2}))
(assert= #{3} (set/difference #{1 2 3} #{1} #{2}))
(assert= #{1 2 3} (set/union #{1} #{2} #{3}))
(require '[clojure.set :as cset])
(assert= #{2} (cset/intersection #{1 2} #{2 3} #{2}))     ; vendor, not bare-2-arity
(assert= [3 6] (apply mapv + [[1 2] [2 4]]))
(assert= '([1 2 3] [1 2] [1]) (sort-by count > [[1] [1 2 3] [1 2]]))
(assert= '([1] [2] [3]) (sort-by first compare [[3] [1] [2]]))
(assert= 11 (count (re-matches #"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)" "abcdefghij")))

; judge battery — scanner, element extents, correction building, end-to-end
(require '[judge])
(assert= 2 (count (judge/judge-forms "(+ 1 2)\n;; note\n(def x 1)\n")))
(assert= "(+ 1 2)" (:text (first (judge/judge-forms "(+ 1 2)\n(def x 1)"))))
(assert= [{:start 0 :end 14 :text "(test (f)\n  9)"}]
         (vec (judge/judge-forms "(test (f)\n  9)\n")))
(assert= 3 (count (judge/judge-elements "(test (+ 1 2) 3)")))
(assert= "(+ 1 2)" (let [[s e] (second (judge/judge-elements "(test (+ 1 2) 3)"))]
                     (subs "(test (+ 1 2) 3)" s e)))
(assert= "3" (let [[s e] (nth (judge/judge-elements "(test (+ 1 2) 3)") 2)]
               (subs "(test (+ 1 2) 3)" s e)))
(assert= "(test (+ 1 2) 3)"
         (judge/judge-correct "(test (+ 1 2))" (judge/judge-elements "(test (+ 1 2))") nil ["3"] ""))
(assert= "(test (+ 1 2) 3)"
         (judge/judge-correct "(test (+ 1 2) 99)" (judge/judge-elements "(test (+ 1 2) 99)") 2 ["3"] ""))
(assert= "'(1 2)" (judge/judge-pr (list 1 2)))
(assert= "[1 2]" (judge/judge-pr [1 2]))
(assert= "\"s\"" (judge/judge-pr "s"))
(assert= 1 (judge/judge-line-of "abc" 2))            ; line counting (char-aware)
(assert= 2 (judge/judge-line-of "a\nb\nc" 2))
(when cljc-test-unix?  ; judge e2e + deps.edn walk-up run ./cljc as a subprocess
; end-to-end: fill, apply, recheck green; normal run is a no-op
(spit "/tmp/cljc-judge-e2e.clj"
      "(require '[judge :refer [test trust]])\n(defn dbl [x] (* 2 x))\n(test (dbl 21))\n(test (dbl 2) 5)\n")
(assert= 1 (:exit (sh "./cljc judge /tmp/cljc-judge-e2e.clj")))
(assert= 0 (:exit (sh "./cljc judge -a /tmp/cljc-judge-e2e.clj")))
(assert= 0 (:exit (sh "./cljc judge /tmp/cljc-judge-e2e.clj")))
(assert= true (str/includes? (slurp "/tmp/cljc-judge-e2e.clj") "(test (dbl 21) 42)"))
(assert= true (str/includes? (slurp "/tmp/cljc-judge-e2e.clj") "(test (dbl 2) 4)"))
(assert= 0 (:exit (sh "./cljc /tmp/cljc-judge-e2e.clj")))
(sh "rm -f /tmp/cljc-judge-e2e.clj /tmp/cljc-judge-e2e.clj.tested")

; bundle e2e: flags reach bundle.clj (the C gate used to reject anything but
; exactly 2 args), a script's -main is auto-called with argv, its int return
; is the exit status, and -m <ns> bundles a namespace + its transitive
; requires from *load-path*. --cflags=-O0 keeps the cc step fast AND
; regression-tests the flag path itself.
(sh "rm -rf /tmp/cljc-bundle-e2e && mkdir -p /tmp/cljc-bundle-e2e/src")
(spit "/tmp/cljc-bundle-e2e/app.clj"
      "(defn -main [& args]\n  (println \"got\" (count args))\n  (if (= (first args) \"fail\") 3 0))\n")
(assert= 0 (:exit (sh "./cljc bundle --cflags=-O0 /tmp/cljc-bundle-e2e/app.clj /tmp/cljc-bundle-e2e/app")))
(assert= "got 2\n" (:out (sh "/tmp/cljc-bundle-e2e/app a b")))
(assert= 3 (:exit (sh "/tmp/cljc-bundle-e2e/app fail")))
(spit "/tmp/cljc-bundle-e2e/src/bndl_lib.clj" "(ns bndl-lib)\n(defn shout [s] (clojure.string/upper-case s))\n")
(spit "/tmp/cljc-bundle-e2e/src/bndl_app.clj"
      "(ns bndl-app (:require [bndl-lib] [clojure.string :as str]))\n(defn -main [& args]\n  (println (bndl-lib/shout (str/join \"-\" args)))\n  0)\n")
(assert= 0 (:exit (sh "CLJC_PATH=/tmp/cljc-bundle-e2e/src ./cljc bundle -m bndl-app --cflags=-O0 /tmp/cljc-bundle-e2e/bapp")))
(assert= "X-Y\n" (:out (sh "/tmp/cljc-bundle-e2e/bapp x y")))   ; runs w/o CLJC_PATH: deps embedded
(sh "rm -rf /tmp/cljc-bundle-e2e")

; project config: :paths from deps.edn/bb.edn feed *load-path*
(spit "/tmp/cljc-deps-test.edn" "{:paths [\"a\" \"b\"] :deps {x {:mvn/version \"1\"}}}")
(assert= ["a" "b"] (cljc/edn-paths "/tmp/cljc-deps-test.edn"))
(assert= nil (cljc/edn-paths "/tmp/cljc-no-such-file.edn"))      ; missing -> nil
(spit "/tmp/cljc-deps-test.edn" "{:deps {x {:mvn/version \"1\"}}}")
(assert= nil (cljc/edn-paths "/tmp/cljc-deps-test.edn"))         ; no :paths -> nil
(sh "rm -f /tmp/cljc-deps-test.edn")
; walk-up: a deps.edn at the project root is found from a nested subdir, and
; its :paths resolve so `require` locates the lib (verified via a subprocess;
; $PWD captures this repo's cljc before we cd into the temp project).
(sh "rm -rf /tmp/cljc-wu && mkdir -p /tmp/cljc-wu/p/lib /tmp/cljc-wu/p/a/b")
(spit "/tmp/cljc-wu/p/deps.edn" "{:paths [\"lib\"]}")
(spit "/tmp/cljc-wu/p/lib/wu.clj" "(ns wu)\n(defn ping [] :pong)\n")
(assert= ":pong" (:out (sh "C=\"$PWD/cljc\"; cd /tmp/cljc-wu/p/a/b && \"$C\" -e \"(require '[wu]) (print (wu/ping))\"")))
(sh "rm -rf /tmp/cljc-wu")
) ; end when-unix

; def docstrings, get-on-string, FIFO queue, lazy n-coll mapcat
(def cljc-doc-probe "the docstring" 42)
(assert= 42 cljc-doc-probe)
(def cljc-str-probe "just-a-value")
(assert= "just-a-value" cljc-str-probe)
(assert= \b (get "abc" 1))
(assert= nil (get "abc" 9))
(assert= :d (get "abc" 9 :d))
(assert= 1 (peek (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)))     ; FIFO front
(assert= 2 (peek (pop (conj clojure.lang.PersistentQueue/EMPTY 1 2 3))))
(assert= 3 (count (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)))
(assert= '(:a 1 :b 2) (mapcat (fn [a b] [a b]) [:a :b] [1 2]))
(assert= '(:r :u) (take 2 (mapcat (fn [a b] (repeat b a)) (cycle [:r :u]) (range 1 99))))
; chunked map/filter must not force unrealized lazy tails (iterate +
; take-while over-realization); iterate defers (f x)
(def cljc-halt-seq
  (iterate (fn [x] (if (keyword? x) (throw (ex-info "forced past halt" {}))
                       (if (= x 3) :halted (inc x)))) 0))
(assert= '(0 1 2 3) (take-while (fn [x] (not= x :halted)) (map identity cljc-halt-seq)))
(assert= '(0 2 4) (filter even? (take 6 (iterate inc 0))))
; multi-binding doseq; into low arities; transients callable
(assert= [[1 :x] [1 :y] [2 :x] [2 :y]]
         (let [acc (atom [])]
           (doseq [a [1 2] b [:x :y]] (swap! acc conj [a b]))
           @acc))
(assert= [1 2] (into [1 2]))
(assert= [] (into))
(assert= 20 ((transient [10 20 30]) 1))

; deep structures must not crash the GC mark (worklist, not recursion)
(assert= 20000 (count (reduce (fn [acc i] (cons i (lazy-seq acc))) nil (range 20000))))
(gc)
(assert= true (vector? (vec (range 3))))

; bounded regex quantifiers + Integer/toString
(assert= "aaa" (re-matches #"a{3}" "aaa"))
(assert= nil (re-matches #"a{3}" "aa"))
(assert= nil (re-matches #"a{3}" "aaaa"))
(assert= "aaa" (re-matches #"a{2,4}" "aaa"))
(assert= nil (re-matches #"a{2,4}" "aaaaa"))
(assert= "aaaaaa" (re-matches #"a{2,}" "aaaaaa"))
(assert= nil (re-matches #"a{2,}" "a"))
(assert= ["R 6 (#70c710)" "R" "6" "#70c710"]
         (re-matches #"(U|D|L|R) (\d+) \((#[0-9a-f]{6})\)" "R 6 (#70c710)"))
(assert= ["ab" "b"] (re-matches #"a(b){1}" "ab"))
(assert= "ff" (Integer/toString 255 16))
(assert= "101" (Integer/toString 5 2))
(assert= "1010" (Integer/toBinaryString 10))
(assert= "42" (Integer/toString 42))
(assert= "0" (Integer/toString 0 2))
; Negatives: toString/radix is SIGNED (Java prefixes '-'), toBinaryString is
; UNSIGNED (raw two's-complement word). Defining one as the other dropped the
; sign entirely -- (Integer/toBinaryString -1) was "1", and +2^54 and -2^54
; printed identically. Values below are byte-identical to the JVM.
(assert= "-101" (Integer/toString -5 2))
(assert= "-ff" (Integer/toString -255 16))
(assert= "11111111111111111111111111111011" (Integer/toBinaryString -5))   ; 32-bit
(assert= "11111111111111111111111111111111" (Integer/toBinaryString -1))
(assert= (apply str (repeat 64 "1")) (Long/toBinaryString -1))             ; 64-bit
(assert= "1111111111000000000000000000000000000000000000000000000000000000"
         (Long/toBinaryString (bit-shift-left 1023 54)))                   ; ten 1s, not one
; Long/MIN_VALUE: negating it overflows, so it must not be negated
(assert= "-1000000000000000000000000000000000000000000000000000000000000000"
         (Integer/toString -9223372036854775808 2))
(assert= "1000000000000000000000000000000000000000000000000000000000000000"
         (Long/toBinaryString -9223372036854775808))
; +2^54 and -2^54 must not render alike
(assert= false (= (Long/toBinaryString (bit-shift-left 1023 54))
                  (Long/toBinaryString (bit-shift-left 1 54))))

; lazy param destructuring (was: to_seq realized infinite seqs — 17GB)
(defn cljc-lazy-destructure-probe [[x & xs]] (cons x (lazy-seq (cljc-lazy-destructure-probe xs))))
(assert= '(0 1 2) (take 3 (cljc-lazy-destructure-probe (range))))
(assert= [1 2 '(3 4)] (let [[a b & c] [1 2 3 4]] [a b c]))
(assert= [1 nil] (let [[a b] [1]] [a b]))

; mutable arrays as transients
(def cljc-arr (int-array 5))
(assert= 0 (aget cljc-arr 3))
(assert= 99 (aset cljc-arr 2 99))
(assert= 99 (aget cljc-arr 2))
(assert= 5 (alength cljc-arr))
(assert= '(2 3 4) (map inc (byte-array [1 2 3])))
(assert= [7 8] (vec (int-array [7 8])))

; VM bughunt regressions (vm branch review, 2026-06-12)
(defmacro cljc-vmtest-mym [a] :macro)
(def cljc-vmtest-f (let [cljc-vmtest-mym (fn [a] :local)] (fn [] (cljc-vmtest-mym 1))))
(assert= :local (cljc-vmtest-f))                       ; closure local shadows macro
(defn cljc-vmtest-g [] (loop [x 0] (if (< x 3) (try (recur (inc x)) (catch Exception e :c)) x)))
(assert= 3 (cljc-vmtest-g))                            ; recur through try in loop
(defn cljc-vmtest-h [a] (loop [x 0] (if (< x 3) (try (recur (inc x))) [a x])))
(assert= [5 3] (cljc-vmtest-h 5))      ; fn params not rebound by escaped recur
(defn cljc-vmtest-fwd [x] (if x (cljc-vmtest-mm 1 2) :no))
(assert= :no (cljc-vmtest-fwd false))                  ; compiles the chunk
(defmacro cljc-vmtest-mm [a b] (list '+ a b 100))
(assert= 103 (cljc-vmtest-fwd true))                   ; late macro deopts to eval
(def cljc-vmtest-r1 (let [cljc-vmtest-twice (fn [x] (* 10 x))] (fn [y] (cljc-vmtest-twice y))))
(defmacro cljc-vmtest-twice [x] (list '* 5 x))
(assert= 50 (cljc-vmtest-r1 5))                        ; captured local beats root macro
(defn cljc-vmtest-or [or] (or 1 2))
(assert= 1 (cljc-vmtest-or (fn [a b] :param)))         ; special form beats param (eval parity)
(assert= :threw (try (do ((fn [] (cond :x)))) (catch Exception e :threw)))  ; odd cond errors

; ── coroutines (the C primitive) + csp.clj (core.async) ──
; ── STM basics (no fibers needed): tx isolation, ops, guards ──
(def cljc-stm-r1 (ref 100)) (def cljc-stm-r2 (ref 0))
(assert= 30 (dosync (alter cljc-stm-r1 - 30) (alter cljc-stm-r2 + 30)))
(assert= [70 30] [@cljc-stm-r1 @cljc-stm-r2])
(assert= 7 (dosync (ref-set cljc-stm-r2 7)))          ; ref-set returns the value
(assert= 2 (let [c (ref 0)] (dosync (commute c inc) (commute c inc))))
(assert= :threw (try (alter cljc-stm-r1 inc) (catch Exception e :threw)))  ; no tx
(assert= :threw (try (ref-set cljc-stm-r1 0) (catch Exception e :threw)))
(assert= :threw (try (dosync (io! :x)) (catch Exception e :threw)))  ; io! in tx
(assert= :ok (io! :ok))
(assert= 42 (sync nil 42))
(assert= 2 (let [n (ref 0)] (dosync (alter n inc) (dosync (alter n inc))) @n))  ; nested joins
; in-tx reads see in-tx writes; watches fire at commit with old and new
(assert= [5 6] (let [r (ref 5) saw (atom nil)]
                 (add-watch r :w (fn [k rf o n] (reset! saw [o n])))
                 (dosync (ref-set r 6) (ensure r))
                 @saw))
(assert= 6 (let [r (ref 5)] (dosync (alter r inc) @r)))   ; deref inside tx = tx value
(assert= 2 (let [r (ref 0)] (dosync (alter r inc) (commute r inc)) @r))   ; commute not re-applied over alter
(assert= 6 (let [r (ref 0)] (dosync (ref-set r 5) (commute r inc)) @r))  ; same for ref-set
(assert= [[0 2]] (let [r (ref 0) log (atom [])]                          ; one watch fire, not two
                   (add-watch r :k (fn [_ _ o n] (swap! log conj [o n])))
                   (dosync (alter r inc) (commute r inc)) @log))
(assert (nil? (get (int-array 4) 4294967296)))                ; get: 64-bit index must not truncate
(assert= :d (get (int-array 4) 4294967296 :d))

(when cljc-test-coro?  ; coro through futures: all need the ucontext engine
(def cljc-coro-g (coro/new (fn [] (coro/yield 1) (coro/yield 2) :done)))
(assert= :new (coro/status cljc-coro-g))
(assert= 1 (coro/resume cljc-coro-g))
(assert= 2 (coro/resume cljc-coro-g))
(assert= :suspended (coro/status cljc-coro-g))
(assert= :done (coro/resume cljc-coro-g))
(assert= :dead (coro/status cljc-coro-g))
; value passing + operands live on the vstack across a yield (segment save)
(def cljc-coro-vp (coro/new (fn [] (+ 100 (* 2 (coro/yield :a))))))
(assert= :a (coro/resume cljc-coro-vp))
(assert= 110 (coro/resume cljc-coro-vp 5))             ; 100 + 2*5, partials survived
; a suspended coro holds the only refs to heap data across a GC
(def cljc-coro-gc (coro/new (fn [] (let [v (vec (range 64))] (coro/yield :built) (reduce + v)))))
(coro/resume cljc-coro-gc)
(gc)
(assert= 2016 (coro/resume cljc-coro-gc))              ; sum 0..63, survived collection
; exceptions escape the body and propagate to the resumer
(def cljc-coro-boom (coro/new (fn [] (coro/yield :ok) (throw (ex-info "x" {:n 9})))))
(assert= :ok (coro/resume cljc-coro-boom))
(assert= 9 (try (coro/resume cljc-coro-boom) (catch Exception e (:n (ex-data e)))))
; per-coro vstack: yield DEEP inside nested calls, with sibling coros resumed
; from separate top-level calls (caches absolute argv pointers — segment
; relocation used to corrupt this; each coro now keeps its own value stack)
(defn cljc-coro-y3 [m] (coro/yield m))
(defn cljc-coro-y2 [m] (+ 0 (cljc-coro-y3 m)))
(defn cljc-coro-y1 [m] (* 1 (cljc-coro-y2 m)))
(def cljc-coro-da (coro/new (fn [] (+ 100 (cljc-coro-y1 :a1) (cljc-coro-y1 :a2)))))
(def cljc-coro-db (coro/new (fn [] (+ 200 (cljc-coro-y1 :b1)))))
(coro/resume cljc-coro-da)
(coro/resume cljc-coro-db)
(assert= :a2 (coro/resume cljc-coro-da 5))     ; sibling interleave, separate pump
(assert= 207 (coro/resume cljc-coro-db 7))
(assert= 114 (coro/resume cljc-coro-da 9))     ; 100 + 5 + 9, operands intact

(require '[csp :as cljc-a])
; producer/consumer over a buffered channel
(def cljc-csp-out (atom nil))
(let [ch (cljc-a/chan 4)]
  (cljc-a/go (dotimes [i 5] (cljc-a/>! ch (* i i))) (cljc-a/close! ch))
  (reset! cljc-csp-out
    (cljc-a/<!! (cljc-a/go-loop [acc []]
                  (let [v (cljc-a/<! ch)]
                    (if (nil? v) acc (recur (conj acc v))))))))
(assert= [0 1 4 9 16] @cljc-csp-out)
; <!! of a go's return value
(assert= 4950 (cljc-a/<!! (cljc-a/go (reduce + (range 100)))))
; unbuffered rendezvous between two go blocks
(def cljc-csp-pp (atom []))
(let [ping (cljc-a/chan) pong (cljc-a/chan)]
  (cljc-a/go (dotimes [i 3] (cljc-a/>! ping i) (swap! cljc-csp-pp conj (cljc-a/<! pong))))
  (cljc-a/go-loop [] (let [v (cljc-a/<! ping)] (cljc-a/>! pong (* v 10)) (recur)))
  (cljc-a/run!))
(assert= [0 10 20] @cljc-csp-pp)
; alts! picks a ready channel
(assert= :hit (let [r (cljc-a/chan 1)]
                (cljc-a/<!! (cljc-a/go (cljc-a/>! r :hit) (first (cljc-a/alts! [r]))))))

; ── futures & promises (fiber-scheduler-backed, blocking deref) ──
; NOTE: these run BEFORE any server test — the bare-@(promise) deadlock check
; below is only detectable while nothing is parked on an accept fd (a live
; server means "something could still deliver", so deref rightly blocks).
(assert= 3 @(future (+ 1 2)))
(assert= :fast (deref (future :fast) 100 :timeout))        ; timeout arity, realized in time
(assert= :timeout (deref (promise) 50 :timeout))           ; timeout arity, never delivered
(assert= :timeout (deref (future (Thread/sleep 1500) :slow) 30 :timeout))
; blocking deref of a promise delivered by a future (deref pumps the scheduler)
(assert= :delivered (let [p (promise)]
                      (future (Thread/sleep 20) (deliver p :delivered))
                      @p))
; a failed future rethrows at the deref site
(assert= "boom" (try @(future (throw (ex-info "boom" {})))
                     (catch Exception e (ex-message e))))
; deref of an undeliverable promise is a deadlock error, not a hang or nil
(assert= :deadlock (try @(promise) (catch Exception e :deadlock)))
(assert= [false true] (let [p (promise)] [(realized? p) (do (deliver p 1) (realized? p))]))
(assert= 1 (let [p (promise)] (deliver p 1) (deliver p 2) @p))   ; first deliver wins
(assert= true (let [f (future 1)] @f (and (realized? f) (future-done? f) (future? f))))
(assert= [true :cancelled] (let [f (future :never-runs)]
                             [(future-cancel f)
                              (try @f (catch Exception e :cancelled))]))
(assert= false (let [f (future 1)] @f (future-cancel f)))  ; too late to cancel
(assert= [2 4 6] (pvalues (+ 1 1) (+ 2 2) (+ 3 3)))
; fiber-aware Thread/sleep: two 200ms futures overlap instead of serializing.
; SELF-CALIBRATING: fixed wall-clock bounds flake on loaded/emulated CI
; runners (75ms and 350ms both did), so measure a single-future baseline and
; require the dual run to stay well under the 2x a serializing scheduler
; would take. (Skipped under GC stress, per file convention.)
(when-not (cljc/env* "CLJC_GC_STRESS")
  (assert= true (let [b0 (cljc/now-ms*)
                      _ @(future (Thread/sleep 200) :warm)
                      base (max 1 (- (cljc/now-ms*) b0))
                      t0 (cljc/now-ms*)
                      f1 (future (Thread/sleep 200) :one)
                      f2 (future (Thread/sleep 200) :two)
                      ok (and (= :one @f1) (= :two @f2))
                      dual (- (cljc/now-ms*) t0)]
                  (and ok (< dual (* 1.7 base))))))
; one shared scheduler: csp go blocks and futures pump each other
(assert= :from-future (let [ch (cljc-a/chan 1)]
                        (future (cljc-a/go (cljc-a/>! ch :from-future)))
                        (cljc-a/<!! ch)))
(assert= :from-go (let [p (promise)]
                    (cljc-a/go (cljc-a/<! (cljc-a/timeout 15)) (deliver p :from-go))
                    @p))
; clojure.core.async on the shared scheduler: sleep in a go block parks the
; fiber (doesn't stall the loop), and @future inside a go block parks too
(require '[clojure.core.async :as cljc-ca])
(when-not (cljc/env* "CLJC_GC_STRESS")
  (assert= true (let [b0 (cljc/now-ms*)
                      _ (cljc-ca/<!! (cljc-ca/go (Thread/sleep 200) :warm))
                      base (max 1 (- (cljc/now-ms*) b0))
                      t0 (cljc/now-ms*)
                      c1 (cljc-ca/go (Thread/sleep 200) :a)
                      c2 (cljc-ca/go (Thread/sleep 200) :b)
                      ok (and (= :a (cljc-ca/<!! c1)) (= :b (cljc-ca/<!! c2)))
                      dual (- (cljc/now-ms*) t0)]
                  (and ok (< dual (* 1.7 base))))))
(assert= 43 (let [f (future (Thread/sleep 10) 42)]
              (cljc-ca/<!! (cljc-ca/go (+ 1 @f)))))
(assert= :threaded (cljc-ca/<!! (cljc-ca/thread (Thread/sleep 10) :threaded)))

; async I/O event loop: a loopback echo server + client, both go blocks, served
; through poll() — no blocking, one thread
(def cljc-csp-srv (tcp/listen 8094 "127.0.0.1"))
(cljc-a/go (let [c (cljc-a/accept! cljc-csp-srv)]
             (cljc-a/send! c (str "echo:" (cljc-a/recv! c)))
             (tcp/close c)))
(assert= "echo:ping"
  (cljc-a/<!! (cljc-a/go (let [fd (tcp/connect "127.0.0.1" 8094)]
                           (cljc-a/send! fd "ping")
                           (let [r (cljc-a/recv! fd)] (tcp/close fd) r)))))
(tcp/close cljc-csp-srv)
; combinators: merge (fan-in) + into (drain)
(assert= 136 (reduce + (cljc-a/<!! (cljc-a/into []
                         (cljc-a/merge [(cljc-a/to-chan! [1 2 3])
                                        (cljc-a/to-chan! [10 20])
                                        (cljc-a/to-chan! [100])])))))
; mult/tap: one source broadcast to two taps
(let [src (cljc-a/chan) m (cljc-a/mult src) t1 (cljc-a/chan 8) t2 (cljc-a/chan 8)]
  (cljc-a/tap m t1) (cljc-a/tap m t2)
  (cljc-a/onto-chan! src [:a :b :c])
  (assert= [:a :b :c] (cljc-a/<!! (cljc-a/into [] t1)))
  (assert= [:a :b :c] (cljc-a/<!! (cljc-a/into [] t2))))
; pipe + take-n
(assert= [0 1 2 3 4]
  (let [to (cljc-a/chan 4)] (cljc-a/pipe (cljc-a/to-chan! (range 50)) to)
       (cljc-a/<!! (cljc-a/take-n 5 to))))
; transducer channel: inc then keep evens
(assert= [2 4 6 8]
  (let [ch (cljc-a/chan 16 (comp (map-xf inc) (filter-xf even?)))]
    (cljc-a/go (dotimes [i 8] (cljc-a/>! ch i)) (cljc-a/close! ch))
    (cljc-a/<!! (cljc-a/into [] ch))))
; transducer channel: mapcat (one in, many out) + completion flush on close
(assert= [[0 1] [2 3] [4]]
  (let [ch (cljc-a/chan 8 (partition-all 2))]
    (cljc-a/go (dotimes [i 5] (cljc-a/>! ch i)) (cljc-a/close! ch))
    (cljc-a/<!! (cljc-a/into [] ch))))
; pub/sub: route by topic; non-subscribed topics are dropped
(let [in (cljc-a/chan) p (cljc-a/pub in :t) ca (cljc-a/chan 8) da (cljc-a/chan 8)]
  (cljc-a/sub p :cat ca) (cljc-a/sub p :dog da)
  (cljc-a/go (doseq [m [{:t :cat :n 1} {:t :dog :n 2} {:t :cat :n 3} {:t :fish}]]
               (cljc-a/>! in m)) (cljc-a/close! in))
  (assert= [1 3] (mapv :n (cljc-a/<!! (cljc-a/into [] ca))))
  (assert= [2]   (mapv :n (cljc-a/<!! (cljc-a/into [] da)))))
; ── agents (Tier 3.6): serial actions on a drain fiber, error modes, watches ──
(def cljc-ag (agent 0))
(send cljc-ag inc) (send cljc-ag inc) (send cljc-ag + 5)
(assert= nil (await cljc-ag))
(assert= 7 @cljc-ag)
(assert= true (await-for 1000 cljc-ag))
; :fail mode (default): deref keeps last value, send throws, restart reruns held queue
(def cljc-agf (agent 10))
(send cljc-agf (fn [_] (throw (ex-info "boom" {}))))
(send cljc-agf inc)                       ; queued behind the failure -> held
(Thread/sleep 5)
(assert= true (some? (agent-error cljc-agf)))
(assert= 10 @cljc-agf)
(assert= :threw (try (send cljc-agf inc) :sent (catch Exception e :threw)))
(restart-agent cljc-agf 99)
(await cljc-agf)
(assert= 100 @cljc-agf)                   ; the held inc ran after restart
(assert= nil (agent-error cljc-agf))
; :continue via error-handler: errors reported, queue keeps going
(def cljc-ag-errs (atom 0))
(def cljc-agc (agent 0 :error-handler (fn [a e] (swap! cljc-ag-errs inc))))
(send cljc-agc (fn [_] (throw (ex-info "x" {})))) (send cljc-agc + 3)
(await cljc-agc)
(assert= [3 1 :continue] [@cljc-agc @cljc-ag-errs (error-mode cljc-agc)])
; watches are real on agents (fire per action, like ARef.notifyWatches)
(def cljc-ag-log (atom []))
(def cljc-agw (agent 1))
(add-watch cljc-agw :w (fn [k r o n] (swap! cljc-ag-log conj [o n])))
(send cljc-agw inc) (await cljc-agw)
(assert= [1 2] (first @cljc-ag-log))
; ── STM refs: real conflict detection across fiber yields ──
; a tx that parks mid-body while the main thread commits under it RETRIES
(def cljc-r (ref 0))
(def cljc-tx-tries (atom 0))
(def cljc-tx-f (future (dosync (swap! cljc-tx-tries inc)
                               (let [v (alter cljc-r inc)] (Thread/sleep 20) v))))
(Thread/sleep 5)
(dosync (alter cljc-r + 100))             ; commit under the parked tx
(assert= 101 @cljc-tx-f)                  ; retried: re-read 100, inc
(assert= 101 @cljc-r)
(assert= 2 @cljc-tx-tries)
; ensure participates in conflict detection (read-only retry)
(def cljc-er (ref 1))
(def cljc-er-f (future (dosync (let [v (ensure cljc-er)] (Thread/sleep 20) v))))
(Thread/sleep 5)
(dosync (ref-set cljc-er 2))
(assert= 2 @cljc-er-f)
; commute never conflicts: re-applied on the latest value at commit
(def cljc-cr (ref 0))
(def cljc-cr-tries (atom 0))
(def cljc-cr-f (future (dosync (swap! cljc-cr-tries inc)
                               (commute cljc-cr inc) (Thread/sleep 20) :done)))
(Thread/sleep 5)
(dosync (alter cljc-cr + 100))
(assert= [:done 101 1] [@cljc-cr-f @cljc-cr @cljc-cr-tries])
; sends inside a transaction are held until commit
(def cljc-ag-tx (agent 0))
(dosync (send cljc-ag-tx + 10) (send cljc-ag-tx + 1))
(await cljc-ag-tx)
(assert= 11 @cljc-ag-tx)
; http.clj: a routed server + the client, full round-trip through the event loop
(require '[http :as cljc-h])
(cljc-h/serve 8093
  (cljc-h/router [[:get  "/hi/:who" (fn [req] (str "hi " (:who (:params req))))]
                  [:post "/echo"    (fn [req] {:status 201 :body (:body req)})]]))
(assert= "hi bob" (:body (cljc-a/<!! (cljc-h/get "http://127.0.0.1:8093/hi/bob"))))
(let [r (cljc-a/<!! (cljc-h/post "http://127.0.0.1:8093/echo" "payload"))]
  (assert= 201 (:status r))
  (assert= "payload" (:body r)))
; nrepl.clj: bencode round-trip + a concurrent server eval through the loop
(require '[nrepl :as cljc-n])
(assert= [{"op" "eval" "id" "1" "code" "(+ 2 3)"} ""]      ; bencode encode/decode round-trip
         (cljc-n/bdecode (cljc-n/bencode {"op" "eval" "id" "1" "code" "(+ 2 3)"})))
(cljc-n/start 8092)
; The read loop parks on POLLIN with no deadline. If the server fiber ever
; dies without closing the socket, recv! never returns, "done" never arrives,
; and the suite wedges at 100% CPU -- taking ./install.sh down with it. Race
; the reader against a timer so a stall FAILs loudly instead of hanging.
(let [reader (cljc-a/go
               (let [fd (tcp/connect "127.0.0.1" 8092)]
                 (csp/send! fd (cljc-n/bencode {"op" "eval" "id" "1" "code" "(+ 2 3)"}))
                 (loop [b ""]
                   (let [m (csp/recv! fd)]
                     (if (or (nil? m) (str/includes? (str b m) "done"))
                       (do (tcp/close fd) (str b m))
                       (recur (str b m)))))))
      ; timeout wins -> its closed chan delivers nil; the reader can only
      ; ever deliver a string, so nil? tells the two apart.
      resp (cljc-a/<!! (cljc-a/go
                         (let [[v _] (cljc-a/alts! [reader (cljc-a/timeout 10000)])]
                           (if (nil? v) ::nrepl-timeout v))))]
  (assert= false (= ::nrepl-timeout resp))                 ; reader stalled, not a protocol bug
  (assert= true (and (string? resp) (str/includes? resp "5:value1:5")))   ; value 5 came back
  (assert= true (and (string? resp) (str/includes? resp "4:done"))))      ; status done
) ; end when-coro

; ── AoC regressions ──
; get on a transient vector (was returning nil for every index) — AoC 2017 d5
(let [t (transient [10 20 30])]
  (assert= 10 (get t 0))
  (assert= 30 (get t 2))
  (assert= nil (get t 9))
  (assert= :d (get t 9 :d))
  (assoc! t 0 99)
  (assert= 99 (get t 0)))
; hex (0x) and radix (NrDDD) integer literals — AoC 2021 d16
(assert= 138 0x8A)
(assert= 255 0xff)
(assert= 255 16rFF)
(assert= 10 2r1010)
(assert= 35 36rZ)
(assert= -16 -0x10)
(assert= 17 017)                                           ; leading 0 stays decimal (not octal)
(assert= 256 (+ 0xff 1))
(assert= 100000.0 1e5)                                     ; float reader unaffected
; trampoline: stack-safe mutual recursion (Clojure's escape hatch, no TCO)
(defn cljc-tramp-ev? [n] (if (zero? n) true #(cljc-tramp-od? (dec n))))
(defn cljc-tramp-od? [n] (if (zero? n) false #(cljc-tramp-ev? (dec n))))
(assert= true  (trampoline cljc-tramp-ev? 100000))
(assert= false (trampoline cljc-tramp-ev? 100001))
(assert= 42 (trampoline (fn [] 42)))                       ; non-fn result returned as-is
; ── general tail-call optimization (beyond Clojure) ──
; non-tail calls must NOT be mis-TCO'd (value is processed after the call)
(defn cljc-tco-h [n] (if (zero? n) 0 (inc (cljc-tco-h (dec n)))))
(assert= 100 (cljc-tco-h 100))                             ; (inc (h …)) — not tail
(defn cljc-tco-ten [x] (* x 10))
(assert= 24 (+ 1 (cljc-tco-ten 2) 3))                      ; call value used in +
(assert= 100 (cljc-tco-ten (cljc-tco-ten 1)))              ; nested
(assert= 30 (and (cljc-tco-ten 1) (cljc-tco-ten 2) (cljc-tco-ten 3)))
; deep proper tail calls (would overflow the C stack without TCO) — skip GC-stress
(defn cljc-tco-ev? [n] (if (zero? n) true  (cljc-tco-od? (dec n))))
(defn cljc-tco-od? [n] (if (zero? n) false (cljc-tco-ev? (dec n))))
(defn cljc-tco-sum ([n] (cljc-tco-sum n 0))                ; multi-arity self tail-call
  ([n acc] (if (zero? n) acc (cljc-tco-sum (dec n) (+ acc n)))))
(when-not (cljc/env* "CLJC_GC_STRESS")
  (assert= true  (cljc-tco-ev? 3000000))                   ; mutual recursion, 3M deep
  (assert= false (cljc-tco-ev? 3000001))
  (assert= 500000500000 (cljc-tco-sum 1000000)))           ; self tail-call, 1M deep
; nested tail call through a native: reduce → fn body (into s x) → into-impl.
; the sentinel owning the args must stay GC-rooted across the native dispatch
; (a `return apply(...)` would let -O2 free it mid-call) — AoC 2015 d18.
(assert= #{0 1 2 3 4 5} (reduce (fn [s x] (into s x)) #{} [#{0 1} #{2 3} #{4 5}]))
; lazy-seq consumers (first/reduce/nth) advance their arg slot, so realizing a
; deep prefix stays O(1) live instead of O(n) — these would crawl/OOM before the
; seq1_slot fix. Skip GC-stress (realizes millions of cells). AoC 2015 d11.
(when-not (cljc/env* "CLJC_GC_STRESS")
  (assert= 3000000 (first (filter #(= % 3000000) (iterate inc 0))))
  (assert= 2000000 (nth (iterate inc 0) 2000000))
  (assert= 4499998500000 (reduce + 0 (take 3000000 (iterate inc 0)))))
; huge apply: splicing > vstack-cap args must not overflow (heap argv path) —
; AoC 2016 d16 / 2021 d20. Skip under GC stress (too slow with 1.1M elements).
(when-not (cljc/env* "CLJC_GC_STRESS")
  (assert= 1100000 (count (apply str (repeat 1100000 "x"))))        ; native, lazy seq
  (assert= 1100000 (apply + (repeat 1100000 1)))                    ; native +
  (assert= 1100000 (count (apply concat (repeat 1100000 [1]))))     ; lazy concat fn
  (assert= 1100000 (apply (fn [& xs] (count xs)) (repeat 1100000 1)))) ; user variadic fn

; ── sorted collections (comparator-ordered set/map: CLJC_SORTED) ──
(assert= [1 2 3] (vec (sorted-set 3 1 2 1)))            ; ordered + deduped
(assert= [3 2 1] (vec (sorted-set-by > 1 2 3)))         ; custom comparator
(assert= [[:a 1] [:b 2] [:c 3]] (vec (sorted-map :c 3 :a 1 :b 2)))
(assert= true  (sorted? (sorted-set 1)))
(assert= true  (set? (sorted-set 1)))
(assert= true  (map? (sorted-map :a 1)))
(assert= false (sorted? #{1 2}))
; key identity is the comparator: cmp-equal keys collapse
(assert= 1 (count (sorted-set-by (fn [a b] (compare (count a) (count b))) "aa" "bb")))
; conj / disj / assoc / dissoc / get / contains?
(assert= [0 1 2 3 5] (vec (conj (sorted-set 1 2 3) 5 0)))
(assert= [1 3] (vec (disj (sorted-set 1 2 3) 2)))
(assert= 2 (get (sorted-map :a 1 :b 2) :b))
(assert= 99 (get (sorted-map :a 1) :z 99))
(assert= 2 ((sorted-map :a 1 :b 2) :b))                 ; callable as a fn
(assert= [[:a 1] [:c 3]] (vec (dissoc (sorted-map :a 1 :b 2 :c 3) :b)))
(assert= true (contains? (sorted-set 1 2) 2))
(assert= false (contains? (sorted-set 1 2) 9))
; = and hash agree with the hash-set/map category (so usable as map keys)
(assert= true (= (sorted-set 1 2 3) #{3 2 1}))
(assert= true (= (sorted-map :a 1 :b 2) {:b 2 :a 1}))
(assert= false (= (sorted-set 1 2) #{1 2 3}))
(assert= true (= (hash (sorted-set 1 2)) (hash #{1 2})))
(assert= :x (get {(sorted-set 1 2) :x} (sorted-set 2 1)))
; range queries
(assert= [5 6 7 8 9] (vec (subseq (apply sorted-set (range 10)) >= 5)))
(assert= [3 4 5 6] (vec (subseq (apply sorted-set (range 10)) > 2 < 7)))
(assert= [9 8 7 6 5] (vec (rsubseq (apply sorted-set (range 10)) >= 5)))
(assert= [9 8 7 6 5 4 3 2 1 0] (vec (rseq (apply sorted-set (range 10)))))
; seq-derived ops, empty preserves type, into/merge
(assert= [:a :b :c] (keys (sorted-map :c 3 :a 1 :b 2)))
(assert= [1 2 3] (vals (sorted-map :c 3 :a 1 :b 2)))
(assert= 15 (reduce + 0 (sorted-set 1 2 3 4 5)))
(assert= [1 2 3] (vec (into (sorted-set) [3 1 2 3])))
(assert= true (sorted? (empty (sorted-set 1))))
(assert= {:a 1 :b 2} (merge (sorted-map :a 1) {:b 2}))
(assert= true (sorted? (merge (sorted-map :a 1) {:b 2})))
; weight-balanced tree stays balanced: 50k ASCENDING inserts (worst case for a
; naive BST) + deletes must be O(n log n), not O(n^2). Skip under GC stress.
(when-not (cljc/env* "CLJC_GC_STRESS")
  (let [n 50000
        s (reduce conj (sorted-set) (range n))]
    (assert= n (count s))
    (assert= true (= (seq s) (range n)))                  ; ordered
    (assert= true (contains? s 25000))
    (assert= 5 (count (subseq s >= (- n 5))))
    (assert= (range 1 n 2) (seq (reduce disj s (range 0 n 2))))))

; ── clojure.core.async (vendored, coroutine-backed) ──
(when cljc-test-coro?
(require '[clojure.core.async :as async])
(assert= 3 (async/<!! (async/go (+ 1 2))))                  ; go + blocking take
(let [c (async/chan)]                                       ; unbuffered rendezvous
  (async/go (async/>! c :hi))
  (assert= :hi (async/<!! (async/go (async/<! c)))))
(let [bc (async/chan 4)]                                    ; buffered producer/consumer
  (async/go (doseq [i (range 5)] (async/>! bc i)) (async/close! bc))
  (assert= [0 1 2 3 4]
           (async/<!! (async/go (loop [acc []]
                                  (let [v (async/<! bc)]
                                    (if (nil? v) acc (recur (conj acc v)))))))))
(let [jobs (async/chan 10) out (async/chan 10)]             ; 3-worker pool
  (dotimes [_ 3]
    (async/go (loop [] (when-let [j (async/<! jobs)] (async/>! out (* j j)) (recur)))))
  (async/go (doseq [j (range 1 6)] (async/>! jobs j)) (async/close! jobs))
  (assert= [1 4 9 16 25]
           (sort (async/<!! (async/go (loop [acc [] n 0]
                                        (if (< n 5) (recur (conj acc (async/<! out)) (inc n)) acc)))))))
(assert= [:nothing :default]                                ; alts! :default
         (async/<!! (async/go (async/alts! [(async/chan)] :default :nothing))))
(assert= (range 5) (async/<!! (async/into [] (async/to-chan! (range 5)))))  ; combinators
(let [t0 (cljc/now-ms*)]                                    ; timeout actually waits
  (async/<!! (async/timeout 30))
  (assert= true (>= (- (cljc/now-ms*) t0) 25)))

; ── refer of a core-shadowing name is a per-ns (user) alias, NOT a global clobber ──
(assert= 6 (clojure.core/reduce + [1 2 3]))            ; core reduce intact before
(require '[clojure.core.async :refer [reduce]])        ; shadows reduce in `user`
(assert= 6 (clojure.core/reduce + [1 2 3]))            ; core reduce STILL intact
(assert= 24 (clojure.core/reduce * [1 2 3 4]))         ; and other core fns (frequencies etc.)
(assert= {:a 2 :b 1} (frequencies [:a :a :b]))         ; frequencies uses reduce internally
(assert= 10 (async/<!! (reduce + 0 (async/to-chan! [1 2 3 4]))))  ; referred reduce = async/reduce
(require '[clojure.core :refer [reduce]])              ; re-refer RESTORES core reduce
(assert= 6 (reduce + [1 2 3]))                         ; 2-arity works again for tests below
) ; end when-coro

; ── namespaced :keys destructuring binds the BARE local (Clojure semantics) ──
(assert= [1 2] (let [{:keys [a/b c]} {:a/b 1 :c 2}] [b c]))
(assert= 9 (let [{:keys [:ns/foo]} {:ns/foo 9}] foo))
(assert= 5 (let [{:keys [n/y] :or {y 5}} {}] y))        ; :or keys on the bare name
; ── multi-coll transducers (sequence/into with several colls) ──
(assert= '([:a 1] [:b 2]) (sequence (map conj) [[:a] [:b]] [1 2]))
(assert= '([1 3] [2 4]) (sequence (map vector) [1 2] [3 4]))

; ── case with a multi-constant clause key built by a macro (a LAZY seq) ──
(assert= :b (case :y (:x :y :z) :b :other))                 ; literal list
(assert= :b (let [k (map identity [:x :y])]                 ; lazy-seq clause key (hiccup)
              (eval (list 'case :y k :b :other))))
; ── (str deftype) routes to its toString; pr-str keeps the structural form ──
(deftype Boxed [v] Object (toString [_] (str "Box:" v)))
(assert= "Box:7" (str (->Boxed 7)))
; ── thread-binding primitives (with-bindings / push-/pop-) ──
(def ^:dynamic *probe* 1)
(assert= 10 (with-bindings {(var *probe*) 10} *probe*))
(assert= 1 *probe*)                                          ; restored
; ── host method stubs ──
(assert= "hello" (.replace "heLLo" "LL" "ll"))
(assert= "42" (String/valueOf 42))

; ── :pre / :post conditions (% is the return value) ──
(defn pp-checked [x] {:pre [(pos? x)] :post [(> % x)]} (* x 2))
(assert= 6 (pp-checked 3))
(assert= true (try (pp-checked -1) false (catch Exception e (clojure.string/includes? (ex-message e) "Assert"))))
; ── partition 4-arity (pad) + when-first ──
(assert= '((1 2 3) (4 5 :p)) (partition 3 3 [:p :p] [1 2 3 4 5]))
(assert= :a (when-first [x [:a :b]] x))
(assert= nil (when-first [x []] x))
; ── clojure.string completeness under a non-:str alias ──
(assert= ["ab" "Foo" "cba"] (let [s clojure.string/triml] [(clojure.string/trimr "ab  ") (clojure.string/capitalize "foo") (clojure.string/reverse "abc")]))
; ── qualified host constructor resolves to the short ctor ──
(assert= "" (str (java.io.StringWriter.)))

; ── friendlier error messages name the value's type / the fn ──
(defn- emsg [f] (try (f) "no-error" (catch Exception e (ex-message e))))
(assert= true (clojure.string/includes? (emsg #(+ 1 :x)) "got a keyword"))
(assert= true (clojure.string/includes? (emsg #(42 1)) "not callable"))
(assert= true (clojure.string/includes? (emsg #(count 5)) "not countable"))
(assert= true (clojure.string/includes? (emsg #(swap! {} inc)) "expected an atom"))
(assert= true (clojure.string/includes? (emsg #((fn ([a] a)) 1 2)) "no matching arity"))

; ── *out* / *err*: print routes through the *out* var; with-out-str captures ──
(assert= "a\nb" (with-out-str (println "a") (print "b")))
(assert= "x12" (with-out-str (print "x") (pr 1) (print 2)))
(assert= "" (with-out-str (binding [*out* *err*] (println "to stderr"))))  ; not captured

; ── regex backreferences \1..\9 ──
(assert= ["aa" "a"] (re-find #"(\w)\1" "aa"))
(assert= nil (re-find #"(\w)\1" "ab"))
(assert= ["abba" "a" "b"] (re-matches #"(.)(.)\2\1" "abba"))      ; palindrome
(assert= ["the the" "the"] (re-find #"(\w+) \1" "the the end"))   ; doubled word
(assert= ["<b>x</b>" "b"] (re-find #"<(\w+)>.*</\1>" "<b>x</b>")) ; matching tag

; ── clj-yaml.core: pure-Clojure YAML subset (no SnakeYAML) ──
(require '[clj-yaml.core :as yaml])
(assert= {:name "Alex" :age 42 :ok true :nil nil}
         (yaml/parse-string "name: Alex\nage: 42\nok: true\nnil: null"))
(assert= {:server {:host "localhost" :port 8080}}
         (yaml/parse-string "server:\n  host: localhost\n  port: 8080"))
(assert= {:fruits ["apple" "banana"]}              ; seq value flush with key
         (yaml/parse-string "fruits:\n- apple\n- banana"))
(assert= [{:name "a" :id 1} {:name "b" :id 2}]      ; compact "- key: val"
         (yaml/parse-string "- name: a\n  id: 1\n- name: b\n  id: 2"))
(assert= {:nums [1 2 3] :pt {:x 1 :y 2}}            ; flow collections
         (yaml/parse-string "nums: [1, 2, 3]\npt: {x: 1, y: 2}"))
(assert= {:a "hello: world" :b "it's ok"}           ; quoted scalars
         (yaml/parse-string "a: \"hello: world\"\nb: 'it''s ok'"))
(assert= {:a 1 :b 2}                                 ; comments + doc markers
         (yaml/parse-string "---\n# top\na: 1 # trailing\nb: 2\n..."))
(assert= {"a" 1 "b" 2} (yaml/parse-string "a: 1\nb: 2" :keywords false))
(assert= {:script "line1\nline2\n"}                 ; literal block scalar
         (yaml/parse-string "script: |\n  line1\n  line2\n"))
(assert= {:text "a b\n"}                             ; folded block scalar
         (yaml/parse-string "text: >\n  a\n  b\n"))
(let [data {:name "config" :count 3 :tags ["x" "y"] :nested {:a 1 :b [2 3]}}]
  (assert= data (yaml/parse-string (yaml/generate-string data))))  ; round-trip
;; bug-pass fixes (2026-06-28)
(assert= {:desc "First.\n\nSecond.\n"}               ; block scalar interior blanks
         (yaml/parse-string "desc: |\n  First.\n\n  Second.\n"))
(assert= {:a 1} (yaml/parse-string "a: 1\n---\nb: 2\n"))   ; multi-doc → first
(assert= {:a 1 :b 2} (yaml/parse-string "a: 1\n\nb: 2"))   ; blank lines between keys
(assert= "123456789012345678901234567890"             ; out-of-range int kept as string
         (:n (yaml/parse-string "n: 123456789012345678901234567890")))

; ── regex: capturing groups inside +/{n}/{n,m} numbered correctly (last wins) ──
(assert= ["abc" "c"] (re-find #"(.)+" "abc"))
(assert= ["abc" "c"] (re-find #"(.){3}" "abc"))
(assert= ["aa" "a" nil] (re-find #"(a)+(b)?" "aa"))        ; following group not leaked
(assert= ["12-x" "2" "x"] (re-find #"(\d)+-(\w)" "12-x"))

; ── str/replace / replace-first dispatch on a regex pattern (Clojure semantics) ──
(assert= "a-b-c" (clojure.string/replace "a.b.c" #"\." "-"))
(assert= "hello_world" (clojure.string/replace "hello world" #"\s+" "_"))
(assert= "01/2024-02" (clojure.string/replace "2024-01-02" #"(\d+)-(\d+)" "$2/$1"))
(assert= "X a a" (clojure.string/replace-first "a a a" #"a" "X"))
(assert= "a-b-c" (clojure.string/replace "a.b.c" "." "-"))  ; plain string still literal

; ── parse-long overflow → nil (was clamped to Long/MAX) ──
(assert= nil (parse-long "123456789012345678901234567890"))
(assert= 42 (parse-long "42"))

; ── friendlier / consistent error messages ──
(defn- emsg2 [f] (try (f) "no-error" (catch Exception e (ex-message e))))
(assert= true (clojure.string/includes? (emsg2 #(inc :a)) "got a keyword"))
(assert= true (clojure.string/includes? (emsg2 #(bit-and :a 1)) "bit-and"))      ; not bit_and
(assert= true (clojure.string/includes? (emsg2 #(/ 1 0)) "Divide by zero"))
(assert= true (clojure.string/includes? (emsg2 #(nth [1 2 3] 9)) "length 3"))
(assert= true (clojure.string/includes? (emsg2 #(deref 5)) "derefable"))
(assert= true (clojure.string/includes? (emsg2 #(compare 1 "a")) "not comparable"))
(assert= true (clojure.string/includes? (emsg2 #(:a :b :c :d)) "1 or 2"))         ; arity

; ── markdown: CommonMark flanking (no spurious emphasis), coalesced text ──
(require '[nextjournal.markdown :as md])
(assert= [:div [:p "snake_case_var"]] (md/->hiccup "snake_case_var"))
(assert= [:div [:p "2 * 3 * 4"]] (md/->hiccup "2 * 3 * 4"))
(assert= [:div [:p "x " [:em "y"] " z"]] (md/->hiccup "x *y* z"))
(assert= [:div [:h1 {:id "hello-world"} "Hello World"]] (md/->hiccup "# Hello World"))

; ── babashka.cli: negative-number values + collection coercion ──
(require '[babashka.cli :as cli])
(assert= {:threshold -5} (cli/parse-opts ["--threshold" "-5"] {:coerce {:threshold :int}}))
(assert= {:id [1 2]} (cli/parse-opts ["--id" "1" "--id" "2"] {:coerce {:id [:int]}}))
; Short flags take a value, like real babashka.cli -- and like the --flag
; branch already did. Assuming `true` dropped the value AND let it fall to
; the positional branch, so -n 2 file yielded {:args ["2" "file"]}.
(assert= {:limit "2"} (cli/parse-opts ["-n" "2"] {:alias {:n :limit}}))
(assert= {:n "2"}     (cli/parse-opts ["-n" "2"] {}))            ; alias only renames
(assert= 2            (:limit (cli/parse-opts ["-n" "2"] {:alias {:n :limit}
                                                          :coerce {:limit :int}})))
(assert= {:args ["file.txt"] :opts {:limit "2"}}                 ; no phantom positional
         (cli/parse-args ["-n" "2" "file.txt"] {:alias {:n :limit}}))
(assert= {:limit "-5"} (cli/parse-opts ["-n" "-5"] {:alias {:n :limit}}))  ; negatives are values
(assert= {:verbose true} (cli/parse-opts ["-v"] {:alias {:v :verbose}}))   ; bare flag still true
(assert= {:verbose true :limit "3"}                              ; flag then flag-with-value
         (cli/parse-opts ["-v" "-n" "3"] {:alias {:v :verbose :n :limit}}))

;; ── second bug pass (2026-06-28): differential-tested against real Clojure ──
; doubles print at full precision (was %g, 6 sig figs), Clojure Double.toString style
(assert= "0.3333333333333333" (pr-str (/ 1.0 3.0)))
(assert= "1.2100000000000002" (pr-str (* 1.1 1.1)))
(assert= "1.0" (pr-str 1.0))
(assert= "1000000.0" (pr-str 1000000.0))
(assert= "1.0E10" (pr-str 1.0e10))
(assert= "1.0E-6" (pr-str 0.000001))
(assert= "##Inf" (pr-str (/ 1.0 0.0)))
(assert= "##-Inf" (pr-str (/ -1.0 0.0)))
(assert= "##NaN" (pr-str (Math/sqrt -1)))
; regex: lazy/possessive counted quantifiers, word boundaries
(assert= "aa" (re-find #"a{2,4}?" "aaaa"))
(assert= "aaa" (re-find #"a++" "aaa"))
(assert= "cat" (re-find #"\bcat\b" "the cat sat"))
(assert= "var" (re-find #"\Bvar" "myvar"))
; str/split: limit, negative limit, empty pattern, empty input
(assert= ["a" "b,c,d"] (clojure.string/split "a,b,c,d" #"," 2))
(assert= ["a" "b" "" "" ""] (clojure.string/split "a,b,,," #"," -1))
(assert= ["a" "b" "c"] (clojure.string/split "abc" #""))
(assert= [""] (clojure.string/split "" #","))
(assert= [] (clojure.string/split ",," #","))
; str/replace: function replacement, backslash escapes; replace-first zero-width at EOS
(assert= "a[1]b[2]" (clojure.string/replace "a1b2" #"\d" (fn [m] (str "[" m "]"))))
(assert= "a<1>" (clojure.string/replace "a1" #"(\d)" (fn [[_ g]] (str "<" g ">"))))
(assert= "a$c" (clojure.string/replace "abc" #"b" "\\$"))
(assert= "abcX" (clojure.string/replace-first "abc" #"$" "X"))
; misc number / seq / destructure fixes
(assert= -2 (Math/round -2.5))               ; Java half-up (was -3)
(assert= 0 (Math/round -0.5))
(assert= '(1.0 1.5 2.0 2.5) (range 1.0 3.0 0.5))   ; range accepts floats
(assert= nil (nth nil 0))                    ; nth on nil
(assert= :d (nth nil 5 :d))
(assert= nil (parse-long " 42"))             ; leading whitespace rejected
(assert= [1 2] (let [{:strs [a b]} {"a" 1 "b" 2}] [a b]))      ; :strs destructure
(assert= [3 4] (let [{:syms [p q]} '{p 3 q 4}] [p q]))         ; :syms destructure
(assert= false ((some-fn neg? even?) 3))     ; some-fn returns false not nil
(assert= [1 2] (swap-vals! (atom 1) inc))
(assert= [1 9] (reset-vals! (atom 1) 9))

; ── lazy + chunked range (perf pass) ──
(assert= '(0 1 2 3 4) (take 5 (range)))                 ; (range) is infinite
(assert= '(1000000 1000001 1000002) (take 3 (drop 1000000 (range)))) ; deep lazy
(assert= '() (range 5 5))                               ; empty range → ()
(assert= '(0 1 2) (range 3))                            ; small bounded (eager path)
(assert= 5000 (count (range 5000)))                     ; crosses chunk boundary
(assert= 4999 (last (range 5000)))
(assert= 50 (nth (range 100) 50))
(assert= 8390656 (apply + (range 4097)))                ; crosses eager threshold
                                                        ; (reduce is core.async's here)
(assert= [1 2 3 4 5] (into [] (map inc (range 5))))
(assert= 10 (count (filter even? (range 20))))          ; lazy range through filter

; ── differential bug-hunt + fuzzer fixes ──
(require '[clojure.set :as cset2])
(assert= "5" (clojure.string/join "," [5]))             ; join single elt → string
(assert= "1-2-3" (clojure.string/join "-" [1 2 3]))
(assert= true (sorted? (cset2/union (sorted-set 3 1) (sorted-set 2 5))))   ; type preserved
(assert= '(1 2 3 5) (seq (cset2/union (sorted-set 3 1) (sorted-set 2 5))))
(assert= true (sorted? (cset2/intersection (sorted-set 1 2 3 4) (sorted-set 2 3))))
(assert= 63 (reduce-kv + 0 [10 20 30]))                 ; reduce-kv on a vector
(assert= [[0 :x] [1 :y]] (reduce-kv (fn [a k v] (conj a [k v])) [] [:x :y]))
(assert= '(1 1) (reductions (fn [a b] (reduced a)) [1 2 3]))  ; reductions honors reduced
(assert= [:a 2 :a] (into [] (replace {1 :a}) [1 2 1]))  ; replace transducer arity
(assert= [0 1 2] (into [] (random-sample 1.0) (range 3))) ; random-sample transducer arity
(assert= :a/b (keyword "a" "b"))                        ; 2-arg keyword keeps ns
(assert= "a" (namespace (keyword "a" "b")))
(assert= "b" (ex-message (ex-cause (ex-info "a" {} (ex-info "b" {})))))  ; ex-info cause
(assert= "#\"ab.c\"" (pr-str #"ab.c"))                  ; regex prints as #"..."
(assert= 3 (read-string "#_ #_ 1 2 3"))                 ; nested discard
(assert= [1 3] (read-string "[1 #_2 3]"))
(do (defmulti mmf identity) (defmethod mmf :a [_] 1) (defmethod mmf :default [_] 0)
    (assert= true (contains? (methods mmf) :a))          ; method table visible
    (assert= 1 ((get-method mmf :a) nil))
    (remove-method mmf :a)
    (assert= 0 (mmf :a)))                                ; remove-method works
(do (defmulti mmg identity) (prefer-method mmg :a :b)
    (assert= {:a #{:b}} (prefers mmg)))

; ── backlog fixes ──
(assert= 1 (into [] (halt-when odd?) [2 4 6 1 8]))      ; halt-when terminates
(assert= 4 (transduce (halt-when #(> % 3)) conj (range 10)))
(do (defprotocol Pml (gml [this] [this x]))             ; multi-arity protocol via extend-type
    (extend-type java.lang.String Pml (gml ([s] s) ([s x] (str s x))))
    (assert= ["a" "ab"] [(gml "a") (gml "a" "b")]))
(assert= "\\µ" (pr-str (char 181)))                     ; char >127 prints literal (µ = 181)
(assert= 17 017)                                        ; leading zero stays decimal (kept)
(assert= "(0 1 2 ...)" (binding [*print-length* 3] (pr-str (range 10))))
(assert= "[1 2 ...]" (binding [*print-length* 2] (pr-str [1 2 3 4 5])))
(assert= "(1 (2 #))" (binding [*print-level* 2] (pr-str '(1 (2 (3 (4)))))))
(assert= 1 (:person/name #:person{:name 1 :age 2}))     ; namespaced map literal
(assert= 1 (:bare #:m{:_/bare 1 :x 2}))                 ; :_/k means no namespace
(assert= "R" (str (reify Object (toString [_] "R"))))   ; reify toString via str
(assert= true (isa? java.lang.String java.lang.Object)) ; Object universal super
(assert= true (isa? [java.lang.String java.lang.Long] [java.lang.Object java.lang.Object]))
(do (derive ::cat ::feline)                             ; hierarchy 3-arg forms
    (assert= true (isa? (make-hierarchy) ::cat ::feline))
    (assert= #{::feline} (parents (make-hierarchy) ::cat)))
(do (derive ::ha ::hx) (derive ::ha ::hy)               ; prefer-method disambiguates dispatch
    (defmulti pmf identity) (defmethod pmf ::hx [_] :x) (defmethod pmf ::hy [_] :y)
    (prefer-method pmf ::hx ::hy)
    (assert= :x (pmf ::ha)))

; ── doall/dorun force side effects (were no-op stubs); chunked repeat/repeatedly ──
(assert= 3 (let [a (atom 0)] (doall (map (fn [_] (swap! a inc)) [1 2 3])) @a))
(assert= 5 (let [a (atom 0)] (dorun (map (fn [_] (swap! a inc)) (range 5))) @a))
(assert= '(2 3 4) (doall (map inc [1 2 3])))
(assert= nil (dorun (range 3)))
(assert= [:x :x :x] (vec (repeat 3 :x)))
(assert= 1000 (count (vec (repeat 1000 0))))             ; (vec (repeat ...)) grid-init idiom
(assert= '(0 0 0 0 0) (take 5 (repeat 0)))
(assert= '(:y :y :y) (take 3 (repeat :y)))
(assert= 7 (let [a (atom 0)] (last (repeatedly 7 #(swap! a inc)))))
; for: innermost simple binding compiles to map (no mapcat+concat); all forms intact
(assert= '(1 4 9) (for [x [1 2 3]] (* x x)))
(assert= '([1 3] [1 4] [2 3] [2 4]) (for [x [1 2] y [3 4]] [x y]))
(assert= '(0 2 4) (for [x (range 5) :when (even? x)] x))
(assert= '(0 10 20) (for [x (range 3) :let [y (* x 10)]] y))
(assert= '(nil nil) (for [x [1 2]] nil))   ; for keeps nil bodies (uses map, not keep)
(assert= '(0 1 4 9 16) (take 5 (for [x (range)] (* x x))))  ; still lazy/infinite
(assert= [] (vec (repeat 0 :x)))

; ── bugs found by the AoC fleet (2026-07-01) ──
; sort re-entrancy: a nested sort inside the comparator/keyfn clobbered the
; global sort state → NULL-fn apply segfault (AoC 2023 d7 camel cards)
(assert= 1000 (count (sort-by (fn [x] (vec (sort > (vals (frequencies (str "x" (mod x 13))))))) (range 1000))))
(assert= '(7 11 3 14) (sort-by (fn [x] (vec (sort > [(mod x 3) (mod x 5)]))) [14 3 7 11]))
(assert= true (empty? (sorted-set)))                     ; was: "not a collection" error
(assert= false (empty? (sorted-set 1)))
(assert= true (empty? (sorted-map)))
(assert= 5 (first (keep-indexed (fn [i n] (when (= n 5) i)) (iterate inc 0))))  ; lazy now
(assert= '([0 :a] [1 :b]) (map-indexed vector [:a :b]))
(assert= '(0 1 2) (for [a (range 5) :while (< a 3)] a))  ; :while in for
(assert= '([1 0] [2 0] [2 1] [3 0] [3 1] [3 2]) (for [x [1 2 3] y (range 9) :while (< y x)] [x y]))
(assert= 7 (nth (long-array 5 7) 3))                     ; 2-arity int/long-array
; doseq modifiers (:let/:when/:while) + get-semantics map destructuring
(assert= "1 2\n" (with-out-str (doseq [x [[1 2]] :let [[a b] x]] (println a b))))
(assert= "1\n3\n" (with-out-str (doseq [x [1 2 3] :when (odd? x)] (println x))))
(assert= "0\n1\n2\n" (with-out-str (doseq [x (range 9) :while (< x 3)] (println x))))
(assert= [5 6] (let [{c 0 d 1} [5 6]] [c d]))            ; int-key destructure on vector
(assert= '(1) (for [x [[1 2]] :let [{a 0} x]] a))

; ── load-file evaluates top-level forms in the ROOT env ──
; A required file defining (defn paths ...) used to resolve its own call sites
; to require-one's `paths` let-local (the candidate-file lazy seq) instead.
(spit "cljc_shadowtest.clj"   ; require munges - to _ when resolving files
      "(ns cljc-shadowtest)\n(defn paths [a b] [(str a b)])\n(def result (first (paths \"x\" \"y\")))\n")
(require 'cljc-shadowtest)
(assert= "xy" cljc-shadowtest/result)
(sh "rm -f cljc_shadowtest.clj")

; ── namespace-object layer: *ns*/find-ns/all-ns/ns-publics + private vars ──
(spit "cljc_nstest_a.clj"
      (str "(ns cljc-nstest-a)\n"
           "(def pub 1)\n"
           "(def ^:private hidden 2)\n"
           "(defn- phelper [] :ph)\n"
           "(defn caller [] (phelper))\n"))   ; same-ns private use
(spit "cljc_nstest_b.clj"
      (str "(ns cljc-nstest-b (:require [cljc-nstest-a :as na :refer [pub]]))\n"
           "(def saw-ns (str *ns*))\n"        ; *ns* tracks the ns form
           "(def refer-val pub)\n"            ; :refer from a ns form
           "(def alias-val na/pub)\n"
           "(def own-aliases (ns-aliases 'cljc-nstest-b))\n"
           "(def denied (try (na/phelper) (catch Exception e :denied)))\n"
           "(def via-var @#'cljc-nstest-a/hidden)\n"  ; var access bypasses privacy
           "(def same-ns-priv (na/caller))\n"))
(require 'cljc-nstest-b)
(assert= "cljc-nstest-b" cljc-nstest-b/saw-ns)
(assert= 1 cljc-nstest-b/refer-val)
(assert= 1 cljc-nstest-b/alias-val)
(assert= '{na cljc-nstest-a} cljc-nstest-b/own-aliases)
(assert= :denied cljc-nstest-b/denied)
(assert= 2 cljc-nstest-b/via-var)
(assert= :ph cljc-nstest-b/same-ns-priv)
(assert= 'cljc-nstest-a (find-ns 'cljc-nstest-a))
(assert= nil (find-ns 'no.such.ns))
(assert= true (contains? (set (all-ns)) 'cljc-nstest-b))
(assert= ["caller" "pub"] (vec (sort (map str (keys (ns-publics 'cljc-nstest-a))))))
(assert= ["caller" "hidden" "phelper" "pub"]
         (vec (sort (map str (keys (ns-interns 'cljc-nstest-a))))))
(assert= "user" (str *ns*))                   ; require didn't leak its ns
(assert= 'x.y (create-ns 'x.y))
(assert= 'x.y (find-ns 'x.y))
(sh "rm -f cljc_nstest_a.clj cljc_nstest_b.clj")

; ── closures created in a loop capture THAT iteration's bindings ──
; VOP_REBIND used to mutate the loop frame's slots in place, so a closure
; (incl. a lazy seq) made in iteration k saw iteration k+1's values.
(defn loop-closure-lazy []
  (loop [xs (vec (range 100)) i 0 acc []]
    (if (= i 3) acc
        (recur (filter #(< % (* 10 (inc i))) xs) (inc i) (conj acc (count xs))))))
(assert= [100 10 10] (loop-closure-lazy))
(defn loop-closure-fns []
  (loop [i 0 fs []]
    (if (= i 3) (mapv (fn [h] (h)) fs)
        (recur (inc i) (conj fs (fn [] i))))))
(assert= [0 1 2] (loop-closure-fns))
(assert= [0 1 2]                      ; top-level loops VM-compile too
         (loop [i 0 fs []]
           (if (= i 3) (mapv (fn [h] (h)) fs)
               (recur (inc i) (conj fs (fn [] i))))))
(assert= 10000                        ; closure-free loops still rebind in place
         (loop [i 0] (if (< i 10000) (recur (inc i)) i)))

; ── docs / source / reader-error QoL ──
(defn- doc-str [sym] (with-out-str (cljc/print-doc* sym)))
(assert= true (clojure.string/includes? (doc-str 'map) "lazy sequence"))       ; core docstring
(assert= true (clojure.string/includes? (doc-str 'str/join) "separated"))     ; flat str/ alias doc
(defn docqol-f "My test doc." [x] (* x 2))
(assert= true (clojure.string/includes? (doc-str 'docqol-f) "My test doc."))  ; user docstring
(assert= true (clojure.string/includes?                                        ; source reconstructs
               (with-out-str (cljc/print-source* 'docqol-f)) "(* x 2)"))
(assert= true (clojure.string/includes?                                        ; reader errors carry a line
               (ex-message (try (read-string "(def a\n  (let [x 1\n") (catch Exception e e)))
               "(line 3)"))

; ── var meta carries :doc/:arglists; error traces reach into VM'd bodies ──
(let [m (meta (var docqol-f))]
  (assert= 'docqol-f (:name m))
  (assert= "My test doc." (:doc m))
  (assert= '([x]) (:arglists m)))
(spit "cljc_tracetest.clj"
      "(defn tt-f [x]\n  (nth [1 2 3] x))\n(defn tt-g [y]\n  (tt-f (+ y 90)))\n(tt-g 9)\n")
(let [tr (do (try (load-file "cljc_tracetest.clj") (catch Exception e nil))
             (cljc/last-trace*))]
  ;; frames from a loaded file carry file:line (main-script frames stay bare "line N")
  (assert= true (clojure.string/includes? tr "at (nth ...) cljc_tracetest.clj:2"))
  (assert= true (clojure.string/includes? tr "at (tt-g ...) cljc_tracetest.clj:5")))
(sh "rm -f cljc_tracetest.clj")

; ── QoL round 3: pprint, native arglists, exe-dir ──
(assert= "{:a 1}" (cljc/pprint-str* {:a 1} 80))
(let [pp (cljc/pprint-str* (vec (range 40)) 40)]
  (assert= true (clojure.string/includes? pp "\n"))     ; wide vector breaks across lines
  (assert= (vec (range 40)) (read-string pp)))          ; and still reads back
(let [pp (cljc/pprint-str* {:k (vec (range 30)) :other "x"} 50)]
  (assert= {:k (vec (range 30)) :other "x"} (read-string pp)))
(assert= true (clojure.string/includes? (doc-str 'conj) "[coll x & xs]"))  ; native arglists in doc
(assert= '([map key] [map key not-found]) (:arglists (meta (var get))))
(assert= true (string? (cljc/exe-dir*)))

; ── QoL round 4: special-form docs, dir, dbg/#p, named fn printing, pst ──
(assert= true (clojure.string/includes? (doc-str 'if) "Special form"))
(assert= true (clojure.string/includes? (doc-str 'when) "truthy"))
(assert= true (clojure.string/includes? (with-out-str (cljc/dir* 'clojure.set)) "union"))
(assert= true (clojure.string/includes? (pr-str (fn [ab cd] ab)) "[ab cd]"))
(defn qol4-named [x] x)
(assert= true (clojure.string/includes? (pr-str qol4-named) "qol4-named"))
(let [out (with-out-str (dbg (* 2 3)))]
  (assert= true (clojure.string/includes? out "(* 2 3) => 6"))
  (assert= true (clojure.string/includes? out "tests.clj")))   ; &form carries the location
(assert= 6 #p (* 2 3))                                          ; reader shorthand evaluates through
(assert= true (fn? pst))

; ── java.time / UUID / boxed-number interop shims ──
(assert= "2025-07-02T23:46:40Z" (str (java.time.Instant/ofEpochMilli 1751500000000)))
(assert= "1970-01-01T00:00:00.123Z" (str (java.time.Instant/ofEpochMilli 123)))
(assert= 1751500000000 (.toEpochMilli (java.time.Instant/ofEpochMilli 1751500000000)))
; Wall clock, NOT the monotonic clock behind now-ms*. Deriving these from
; now-ms* reported machine uptime instead: Instant/now said 1970-01-16 and
; currentTimeMillis returned ~1.4e12 rather than ~1.8e12 (fixed 2026-09-05).
; Asserted as bounds, not exact values, so the test stays deterministic.
(assert= true (> (System/currentTimeMillis) 1750000000000))   ; after 2025-06-21
(assert= "20" (subs (str (java.time.Instant/now)) 0 2))       ; 21st century, not 1970
(let [us (quot (cljc/now-us*) 1000)                           ; read first, so ms >= us
      ms (System/currentTimeMillis)]
  (assert= true (and (<= us ms) (< (- ms us) 5000))))         ; one clock, one epoch
; getpid: native, so no require and no FFI (libc.clj also binds it, but that
; needs dlopen and self-skips on Windows). Bounds only -- the value varies.
(assert= true (pos? (cljc/getpid)))
(assert= :int (type (cljc/getpid)))
(assert= true (= (cljc/getpid) (cljc/getpid)))               ; stable within a process
(assert= :int (type (cljc/getuid)))
(assert= true (>= (cljc/getuid) 0))

; The FFI module cache is dlopen'd, so it must not sit at a path shared with
; other users in a world-writable /tmp: anyone could plant a .so there and get
; it executed in this process. It lives in a private 0700 per-user dir, and is
; loaded only when `test -O` says this user owns it. (ls, not stat -c: BSD stat
; takes different flags and CI runs macOS.)
(when cljc-test-unix?
(let [d (cljc/user-tmp-dir*)]
  (assert= true (str/includes? d (str "cljc-" (cljc/getuid))))     ; per-user path
  (assert= 0 (:exit (sh (str "test -d " d " && test -O " d))))     ; exists, ours
  (assert= true (str/starts-with? (:out (sh (str "ls -ld " d))) "drwx------")))
) ; end when-unix

; System/exit really exits, with the given status, and stops execution. Was a
; no-op stub: it returned nil and the script ran on, so `cmd || echo failed`
; never fired. Needs a subprocess -- calling it here would end this run --
; so it is unix-gated like the other exe-spawning sections: the binary is
; cljc.exe on Windows, and the temp path must not be an absolute POSIX one
; (a mingw build resolves /tmp against the current drive root).
(when cljc-test-unix?
(let [exe (str (cljc/exe-dir*) "/cljc")
      f   (str (or (System/getenv "TMPDIR") "/tmp") "/cljc-exit-" (cljc/getpid) ".clj")]
  (spit f "(println \"before\")(System/exit 3)(println \"after\")")
  (let [r (sh (str exe " " f))]
    (assert= 3 (:exit r))
    (assert= "before" (str/trim (:out r))))                  ; did not run on
  (spit f "(System/exit 0)")
  (assert= 0 (:exit (sh (str exe " " f))))
  (spit f "(+ 1 1)")
  (assert= 0 (:exit (sh (str exe " " f))))                   ; no exit call -> 0
  (sh (str "rm -f " f)))
) ; end when-unix

; ── TUI natives: raw-mode* / read-key* / term-size* ──
; term-size* is portable: a real ioctl when there is a tty, [24 80] otherwise.
(let [sz (cljc/term-size*)]
  (assert= 2 (count sz))
  (assert= :int (type (first sz)))
  (assert= true (and (pos? (first sz)) (pos? (second sz)))))

(when cljc-test-unix?
; read-key* reads fd 0 with read()/select(), which do not require a tty -- so
; these drive it from a pipe, with no pty and no sleeps to go flaky. Escape
; sequences must arrive whole, a lone ESC must fall out of the 30ms window,
; and multi-byte UTF-8 must be gathered into ONE character (count 1, not 2).
(let [exe (str (cljc/exe-dir*) "/cljc")
      f   (str "/tmp/cljc-key-" (cljc/getpid) ".clj")
      run (fn [in] (str/trim (:out (sh (str "printf '" in "' | " exe " " f)))))]
  (spit f "(println (str/join \",\" (map int (seq (cljc/read-key*)))))")
  (assert= "27,91,65"           (run "\\033[A"))        ; Up arrow: whole CSI
  (assert= "113"                (run "q"))               ; plain key
  (assert= "27"                 (run "\\033"))          ; lone Esc, not a prefix
  (assert= "233"                (run "\\303\\251"))    ; é: one char, not two bytes
  (assert= "27,91,49,59,53,65"  (run "\\033[1;5A"))     ; CSI with parameters
  (spit f "(prn (cljc/read-key*))")
  (assert= "nil" (run ""))                               ; EOF
  ; raw-mode* declines when stdin is not a tty, rather than erroring
  (spit f "(prn (cljc/raw-mode* true))")
  (assert= "false" (str/trim (:out (sh (str exe " " f " < /dev/null")))))
  (sh (str "rm -f " f)))
) ; end when-unix

; `cljc test` discovery: recursive, sorted, skips vendor/target/hidden dirs.
; Uses the native cljc/list-dir* (nil for a non-dir, [] -- truthy -- for an
; empty one), so it needs no FFI. Exercised against a temp tree.
; Unix-gated: builds the tree with mkdir -p / rm -rf, and an absolute POSIX
; temp path is meaningless to a mingw build. find-test-files itself is portable.
(load-file "test.clj")   ; explicit: otherwise bound only via the clojure.test
                         ; shim required far above, which is cached/idempotent
(when cljc-test-unix?
(let [root (str (or (System/getenv "TMPDIR") "/tmp") "/cljc-disc-" (cljc/getpid))]
  (sh (str "rm -rf " root "; mkdir -p " root "/test/sub " root "/vendor " root "/.hid"))
  (spit (str root "/a_test.clj") "")
  (spit (str root "/test/sub/b_test.cljc") "")
  (spit (str root "/vendor/skip_test.clj") "")            ; skipped dir
  (spit (str root "/.hid/skip_test.clj") "")              ; hidden dir
  (spit (str root "/notatest.clj") "")                    ; wrong suffix
  (assert= ["a_test.clj" "test/sub/b_test.cljc"]
           (mapv #(subs % (inc (count root)))
                 (cljc/find-test-files root)))
  (sh (str "rm -rf " root)))
) ; end when-unix
(assert= "2025-07-02T23:46:40Z"
         (.format java.time.format.DateTimeFormatter/ISO_INSTANT
                  (java.time.Instant/ofEpochMilli 1751500000000)))
(assert= true (some? (re-matches #"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"
                                 (random-uuid))))                ; real v4, not the zeros stub
(assert= false (= (random-uuid) (random-uuid)))
(assert= 12345678901234567890123456N (bigint "12345678901234567890123456"))  ; parses strings
(assert= 42 (Long/valueOf "42"))
(assert= "2a" (Integer/toHexString 42))
(assert= true (.isInfinite ##Inf))
(assert= false (.isInfinite 1.5))
(assert= true (.isNaN ##NaN))
(let [buf (char-array 5)]
  (.getChars "hello" 1 4 buf 0)                                  ; copy "ell" into buf[0..3)
  (assert= "ell" (String. buf 0 3)))
(assert= :boolean java.lang.Boolean)                             ; class keys resolve for (extend ...)
(assert= :Instant java.time.Instant)

; ── codepoint-indexed strings (storage stays UTF-8 bytes) ──
(let [s "héllo → ∞"]
  (assert= 9 (count s))                       ; codepoints, not bytes (13)
  (assert= \é (nth s 1))
  (assert= \→ (nth s 6))
  (assert= \é (get s 1))
  (assert= "héllo" (subs s 0 5))
  (assert= "→ ∞" (subs s 6))
  (assert= 6 (str/index-of s "→"))            ; index agrees with subs
  (assert= "→ ∞" (subs s (str/index-of s "→")))
  (assert= 3 (str/last-index-of s "l"))
  (assert= "∞ → olléh" (str/reverse s))
  (assert= [\h \é \l] (vec (take 3 s)))
  (assert= \∞ (last s))
  (assert= 9 (.length s))
  (assert= \é (.charAt s 1)))
(let [s "𝄞clef"]                              ; astral plane: 1 char, not 2 (JVM counts UTF-16 units)
  (assert= 5 (count s))
  (assert= \𝄞 (first s))
  (assert= "clef" (subs s 1))
  (assert= \𝄞 (read-string (pr-str \𝄞))))     ; 4-byte chars survive print/read
(assert= 11 (count "plain ascii"))            ; ASCII fast path intact
(assert= \a (nth "plain ascii" 6))
(assert= 2 (count (str \é \𝄞)))               ; str of wide chars re-encodes correctly

; ── codepoint-aware regex ──
(assert= '("h" "é" "ā") (re-seq #"." "héā"))            ; . = one codepoint
(assert= ["hél" "é"] (re-find #"h(.)l" "héllo"))        ; group captures a whole char
(assert= "ééé" (re-matches #"é+" "ééé"))                ; quantifier binds the full literal
(assert= '("é" "ā" "→") (re-seq #"[éā→]" "a é b ā c →")) ; unicode class members
(assert= "λ" (re-find #"[α-ω]+" "abc λόγος xyz"))        ; unicode range (ό = U+03CC is outside)
(assert= '("a" "b") (re-seq #"[^é]" "aéb"))              ; negated class consumes codepoints
(assert= ["h" "é" "l" "l" "o"] (str/split "héllo" #"")) ; empty split → per char
(assert= "*****" (str/replace "héllo" #"." "*"))
(assert= "𝄞𝄞" (re-find #"𝄞+" "𝄞𝄞x"))                    ; astral literal + quantifier
(assert= '("1" "22" "333") (re-seq #"\d+" "a1 b22 c333")) ; ASCII path untouched

; ── REPL-survival round: value-naming errors, capped lazy print, trace-vars ──
(assert= true (clojure.string/includes?
               (ex-message (try (+ 1 :kw) (catch Exception e e))) ":kw"))
(assert= true (clojure.string/includes?
               (ex-message (try (inc "41") (catch Exception e e))) "\"41\""))
(def *print-length* 3)
(assert= "(0 1 2 ...)" (pr-str (range)))          ; infinite seq prints capped, not forever
(assert= "(0 1 2 ...)" (pr-str (iterate inc 0)))
(def *print-length* nil)
(defn traced-fact [n] (if (zero? n) 1 (* n (traced-fact (dec n)))))
(trace-vars traced-fact)
(let [out (with-out-str (assert= 6 (traced-fact 3)))]
  (assert= true (clojure.string/includes? out "TRACE (traced-fact 3)"))
  (assert= true (clojure.string/includes? out "|  |  (traced-fact 1)"))  ; depth indent
  (assert= true (clojure.string/includes? out "=> 6")))
(untrace-vars traced-fact)
(assert= "" (with-out-str (traced-fact 3)))        ; untraced: silent again
(assert= true (fn? vendor!))                       ; vendor! callable from the REPL

; ── ecosystem-sweep round: #^ metadata, regex escape pairs, new, with-open ──
(cljc/eval-forms* "(ns #^{:author \"a\"} sweep.metans2) (def in-ns-val 7)")
(assert= true (some? (find-ns 'sweep.metans2)))       ; ns registers despite name meta
(assert= 7 sweep.metans2/in-ns-val)
(assert= "aQb" (str/replace "a\\b" #"\\" "Q"))       ; #"\\" = one literal backslash...
(assert= "Q" (str/replace "\\\\" #"\\\\" "Q"))      ; ...#"\\\\" = two (escape PAIRS)
(assert= "x" (re-find #"\"|x" "axb"))                 ; \" inside a pattern
(assert= "ok" (str (new StringBuilder) "ok"))         ; (new Cls args*) sugar
(assert= 3 (with-open [r {:cljc/type :NopCloseable}] 3))  ; with-open runs + closes
(in-ns 'user)

; ── prim_sort GC pinning: sorting a MAP's entries with an allocating
;    comparator used to crash — the entry chain was reachable only through a
;    dead local while the elements sat in a malloc'd array the GC can't see
(assert= 26 (count (sort-by val > (frequencies
                                   (apply str (repeat 200 "abcdefghijklmnopqrstuvwxyz"))))))
(assert= [\a \b \c] (map key (take 3 (sort-by key (frequencies "cabcabcab")))))

; ── library bring-up round (core.match / markdown-clj / meander) ──
;; metadata in binding positions binds the symbol (core.match's ocr- bindings)
(assert= 5 (let [^{:tag :x} mb 5] mb))
(assert= 7 (loop [^{:m 1} lb 7] lb))
(assert= [3 4] (let [[^{:m 1} da ^{:m 2} db] [3 4]] [da db]))
;; declare never clobbers a bound var (core.match forward-declares after defn)
(defn decl-victim [] :alive)
(declare decl-victim)
(assert= :alive (decl-victim))
;; deftype methods: quasiquote templates aren't macroexpanded/field-rewritten,
;; but unquote payloads are; quoted forms stay quoted
(defprotocol QQP (qq-emit [this]) (qq-quote [this]))
(defrecord QQR [bindings]
  QQP
  (qq-emit [this] `(let [~@bindings] :body))
  (qq-quote [this] 'bindings))
(assert= '(let [q 5] :body) (qq-emit (QQR. '(q 5))))
(assert= 'bindings (qq-quote (QQR. :whatever)))
;; keys/vals accept sequences of map entries (Clojure parity; meander needs it)
(assert= [1 2] (vec (sort (vals (sort-by key {:a 1 :b 2})))))
(assert= [:a :b] (vec (sort (keys (seq {:a 1 :b 2})))))
;; syntax-quote resolves ALIAS-qualified symbols to the full ns
(cljc/eval-forms* "(ns sq.provider) (defn pfn [] :from-provider)
(ns sq.consumer (:require [sq.provider :as sqp]))
(defmacro emit-call [] `(sqp/pfn))" "sqtest")
(in-ns 'user)
(assert= 'sq.provider/pfn (first (macroexpand-1 '(sq.consumer/emit-call))))
(assert= :from-provider (eval (list 'sq.consumer/emit-call)))
;; macroexpand (full) exists
(assert= '(if (not true) 1 2) (macroexpand '(if-not true 1 2)))
;; line-reader shims: BufferedReader./.readLine/line-seq over strings
(let [r (java.io.BufferedReader. (StringReader. "a\nb\nc"))]
  (assert= "a" (.readLine r))
  (assert= ["b" "c"] (vec (line-seq r)))
  (assert= nil (.readLine r)))
(assert= 3 (with-open [r (java.io.BufferedReader. (StringReader. "x\ny\nz"))]
             (count (line-seq r))))

;; PLAN item 26 (closed 2026-07-09): macros whose expansions are built from
;; LAZY seq compositions. Historically (0ac1817) a lazy seq in form position
;; dropped its args ((cons '+ (concat lazy ...)) => 0) and lazy reify-style
;; splices threw arity errors. Fixed en route (lazy-as-form + VM bughunt);
;; these assertions pin every shape so it can't regress silently.
;; expansion IS a lazy seq (cons onto concat) — the silent-0 shape
(defmacro i26-sum [& xs]
  (cons '+ (concat (map (fn [x] (* 2 x)) xs) '(1000))))
(assert= 1012 (i26-sum 1 2 3))
;; ~@ splice across chunk boundaries (>32 elements)
(defmacro i26-big [& xs] `(+ ~@(map (fn [x] `(inc ~x)) xs)))
(assert= (+ (reduce + (range 100)) 100) (eval (cons 'i26-big (range 100))))
;; lazy mapcat splice inside a VM-compiled fn, call site evaluated repeatedly
(defmacro i26-pairs [& xs]
  `(vector ~@(mapcat (fn [x] [`(quote ~x) `(str '~x)]) xs)))
(defn i26-use-pairs [] (i26-pairs a b c))
(dotimes [_ 3] (assert= '[a "a" b "b" c "c"] (i26-use-pairs)))
;; splice-side effects realize exactly once despite repeated call-site eval
(def i26-count (atom 0))
(defmacro i26-counted [& xs]
  `(list ~@(map (fn [x] (swap! i26-count inc) x) xs)))
(defn i26-caller [] (i26-counted 10 20 30))
(dotimes [_ 5] (assert= '(10 20 30) (i26-caller)))
(assert= 3 @i26-count)
;; the original reproducer: reify-shaped defmethod generation via lazy splice
(defmulti i26-area (fn [s] (type s)))
(defmacro i26-reify [& clauses]
  (let [t (keyword (str (gensym)))
        impls (filter list? clauses)]
    `(do ~@(map (fn [[m params & body]]
                  `(defmethod ~m ~t ~(vec params) ~@body))
                impls)
         {:cljc/type ~t})))
(def i26-a (i26-reify (i26-area [s] 25)))
(def i26-b (i26-reify (i26-area [s] 49)))
(assert= 25 (i26-area i26-a))
(assert= 49 (i26-area i26-b))
;; macro-defining-macro through a lazy splice
(defmacro i26-getters [rec & fields]
  `(do ~@(map (fn [f]
                `(defmacro ~(symbol (str rec "-" f)) [m#]
                   (list 'get m# ~(keyword (str f)))))
              fields)))
(i26-getters i26pt x y z)
(assert= 1 (i26pt-x {:x 1 :y 2 :z 3}))
(assert= 3 (i26pt-z {:x 1 :y 2 :z 3}))
;; lazy splice building fn PARAMS (the arity-error symptom class)
(defmacro i26-defn-n [name n & body]
  (let [params (map (fn [i] (symbol (str "i26a" i))) (range n))]
    `(defn ~name [~@params] ~@body)))
(i26-defn-n i26-add35 35 (+ i26a0 i26a34))
(assert= 36 (apply i26-add35 (map inc (range 35))))

;; ── specter bring-up round (2026-08-27): six interpreter fixes ──
;; top-level (do ...) evaluates subforms one at a time (JVM semantics): a
;; macro defined by an earlier subform expands at a later one. Compiled as one
;; chunk, the use site's ARGS were compiled as evaluations before the defmacro
;; ran (specter's #?(:clj (do (defmacro defmacroalias ..) (defmacroalias ..))))
(do (defmacro s50-twice [x] `(* 2 ~x))
    (def s50-twice-val (s50-twice 21)))
(assert= 42 s50-twice-val)
;; macro alias through a Var: (def alias (var m)) + :macro meta (defmacroalias)
(defmacro s50-m1 [x] `(inc ~x))
(def s50-m2 (var s50-m1))
(alter-meta! (var s50-m2) merge {:macro true})
(assert= 4 (s50-m2 3))
(assert= '(inc 3) (macroexpand-1 '(s50-m2 3)))
(defn s50-in-fn [] (s50-m2 10))          ; VM-compiled body path
(assert= 11 (s50-in-fn))
;; :bb reader-conditional feature: priority cljc > bb > default > clj
(assert= :bb #?(:bb :bb :clj :clj))
(assert= :bb #?(:clj :clj :bb :bb))
(assert= :bb #?(:bb :bb :default :d))
(assert= [1 2 3] [1 #?@(:bb [2 3])])
;; the empty vector is a singleton, like PersistentVector/EMPTY — specter's
;; terminal* tests (identical? vals [])
(assert= true (identical? [] (vector)))
(assert= true (identical? [] (empty [1 2])))
(assert= true (identical? [] (persistent! (transient []))))
(assert= true (identical? [] (pop [1])))
(assert= true (identical? [] (vec ())))
(assert= {:a 1} (meta (persistent! (transient (with-meta [] {:a 1})))))
(assert= nil (meta []))
(assert= [1 2] (conj (conj [] 1) 2))
;; fn bodies whose list spine ends in a lazy seq (macro-built via cons/drop)
;; evaluated to nil — the clause walker stepped raw cons tails
(def s50-f (eval (list 'fn (cons '[x] (drop 0 (list '(+ x 1)))))))
(assert= 6 (s50-f 5))
(def s50-f2 (eval (list 'fn (cons '[x] (drop 0 (list 1))) (cons '[x y] (drop 0 (list 2))))))
(assert= [1 2] [(s50-f2 0) (s50-f2 0 0)])
;; reify: same-name clauses become ONE multi-arity fn (a hash-map kept the
;; last); reduce/into/transduce honor a reify's IReduce; count sees Counted
(def s50-r (reify clojure.lang.IReduce
             (reduce [this f] (clojure.core/reduce f [1 2 3]))
             (reduce [this f init] (clojure.core/reduce f init [1 2 3]))))
(assert= 6 (clojure.core/reduce + s50-r))
(assert= 16 (clojure.core/reduce + 10 s50-r))
(assert= [1 2 3] (into [] s50-r))
(assert= 9 (transduce (map inc) + 0 s50-r))
(assert= 42 (count (reify clojure.lang.Counted (count [this] 42))))
;; host class names specter's extend-protocol clauses dispatch on
(assert= true (isa? (type [1]) java.util.List))
(assert= true (isa? (type (list 1)) java.util.List))
(assert= false (isa? (type #{1}) java.util.List))
(assert= true (isa? (type #{1}) java.util.Set))
(assert= :sorted-map clojure.lang.PersistentTreeMap)
(assert= true (isa? (type (sorted-map)) clojure.lang.IPersistentMap))
(assert= :transient-vector (type (transient [])))
(defrecord S50Rec [a])
(assert= true (isa? (type (->S50Rec 1)) clojure.lang.IRecord))
(assert= false (isa? (type {:a 1}) clojure.lang.IRecord))
(assert= true (instance? clojure.lang.Cons (list 1 2)))

;; macros expand under the namespace the form was WRITTEN in (JVM: definition-
;; time expansion), not the caller's: cljc expands lazily at first call, so
;; a macro reading *ns* / (ns-aliases *ns*) inside a library fn called from
;; user saw `user` (meander's alias-qualified operator symbols). *ns* is
;; restored afterwards, including when the expansion throws (ErrFrame).
(spit "cljc_s50_nslib.clj" "(ns s50ns.lib (:require [clojure.string :as s50str]))
(defmacro s50-ns-now [] (list 'quote *ns*))
(defmacro s50-alias-of [a] (list 'quote (get (ns-aliases *ns*) a)))
(defn s50-ns-probe [] (s50-ns-now))
(defn s50-alias-probe [] (s50-alias-of s50str))
(defmacro s50-boom [] (throw (ex-info \"boom\" {})))
(defn s50-boom-fn [] (s50-boom))")
(load-file "cljc_s50_nslib.clj")
(assert= 'user *ns*)
(assert= 's50ns.lib (s50ns.lib/s50-ns-probe))
(assert= 'clojure.string (s50ns.lib/s50-alias-probe))
(assert= 'user *ns*)
(assert= "boom" (try (s50ns.lib/s50-boom-fn) (catch Exception e (ex-message e))))
(assert= 'user *ns*)
(sh "rm -f cljc_s50_nslib.clj")

(println "tests complete")
