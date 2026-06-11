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
(assert= :int (get mixed 1.0))                      ; (= 1 1.0) → same bucket
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
(spit "/tmp/cljc-test.txt" "hello file")
(assert= "hello file" (slurp "/tmp/cljc-test.txt"))


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
(assert= (list "a" "b" "c") (seq "abc"))             ; strings seq as 1-char strings
(assert= nil (seq ""))
(assert= "h" (first "hello"))
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
(assert= {"l" 2 "h" 1 "e" 1 "o" 1} (frequencies (seq "hello")))

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
(assert= nil (require '[clojure.string :as str]))
(defn docd "has a docstring" [x] (* x 2))
(assert= 42 (docd 21))
(defmacro docd-m "macro docstring" [x] `(+ ~x 1))
(assert= 4 (docd-m 3))
(assert= "a" \a)
(assert= " " \space)
(assert= "\n" \newline)
(assert= 3 (count (filter #(= % \a) (seq "banana"))))
(assert= [1 2] ^:private [1 2])
(assert= {:private true} (meta ^:private [1 2]))
(assert= :ours #?(:clj :jvm :cljc :ours :default :other))
(assert= :fallback #?(:clj :jvm :default :fallback))
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
(assert= 42 (int 42))
(assert= [] *args*)                                  ; no args to the test run
(assert= "abc" (str/join "" (map (fn [c] c) (seq "abc"))))

; ── JIT: numeric functions compiled to native C ──
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

; ── library survey: upstream clojure.set via loading require ──
(require 'clojure.set)
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
(require 'clojure.walk)
(assert= {:a 2 :b [3 4]} (postwalk (fn [x] (if (int? x) (inc x) x)) {:a 1 :b [2 3]}))
(assert= {:a 1 :b {:c 2}} (keywordize-keys {"a" 1 "b" {"c" 2}}))
(assert= [:y [:y :z]] (prewalk-replace {:x :y} [:x [:x :z]]))

; ── metadata + clojure.zip (the structural unlock) ──
(assert= {:k :v} (meta (with-meta [1] {:k :v})))
(assert= nil (meta [1]))
(assert= [1] (with-meta [1] {:k :v}))                ; meta invisible to =
(assert= {:a 1 :b 2} (meta (vary-meta (with-meta [] {:a 1}) assoc :b 2)))
(assert= :no (try (with-meta 42 {}) (catch Exception e :no)))
(require 'clojure.zip)
(def zt (vector-zip [1 [2 3] 4]))
(assert= 3 (-> zt down right down right node))
(assert= [2 [2 3] 4] (root (edit (-> zt down) inc)))
(assert= [:X [2 3] 4] (-> zt down (replace :X) root))
(assert= [1 [3] 4] (-> zt down right down remove root))

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
(assert= "\f" \formfeed)
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

(println "tests complete")
