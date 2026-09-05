;; test.clj — clojure.test-compatible battery for cljc.
;; (load-file "test.clj") then: deftest / is / testing / run-tests
;;
;; (deftest my-test
;;   (is (= 4 (+ 2 2)))
;;   (is (thrown? Exception (/ 1 0)) "divide by zero throws"))
;; (run-tests)

(def cljc/tests (atom []))                ; [[name fn] ...]
(def cljc/test-pass (atom 0))
(def cljc/test-fail (atom 0))
(def cljc/test-context (atom []))

(defmacro deftest [tname & body]
  `(swap! cljc/tests conj ['~tname (fn [] ~@body)]))

(defmacro testing [desc & body]
  `(do (swap! cljc/test-context conj ~desc)
       (let [r# (do ~@body)]
         (swap! cljc/test-context pop)
         r#)))

(defn cljc/test-report [ok form msg]
  (if ok
    (swap! cljc/test-pass inc)
    (do (swap! cljc/test-fail inc)
        (println "FAIL" (str/join " > " @cljc/test-context)
                 (or msg "") "\n  expected:" (pr-str form)))))

(defmacro is
  ([form] `(is ~form nil))
  ([form msg]
   (if (and (list? form) (= 'thrown? (first form)))
     `(cljc/test-report
        (try (do ~@(drop 2 form)) false (catch Exception e# true))
        '~form ~msg)
     `(cljc/test-report
        (try ~form (catch Exception e#
                     (println "ERROR in is:" (ex-message e#)) false))
        '~form ~msg))))

;; (are [x y] (= x y)  1 1  2 (inc 1)) — template substitution per group
(defn cljc/test-subst [smap form]
  (cond
    (contains? smap form) (get smap form)
    (list? form) (apply list (map (fn [f] (cljc/test-subst smap f)) form))
    (vector? form) (mapv (fn [f] (cljc/test-subst smap f)) form)
    (map? form) (into {} (map (fn [kv] [(cljc/test-subst smap (first kv))
                                        (cljc/test-subst smap (second kv))])
                              (seq form)))
    :else form))

(defmacro are [argv expr & args]
  (cons 'do
        (map (fn [group] (list 'is (cljc/test-subst (zipmap argv group) expr)))
             (partition (count argv) args))))

(defn use-fixtures [& _] nil)   ; fixtures are a no-op (flat env, no vars)

(defn run-tests
  ([& _]
   (reset! cljc/test-pass 0)
   (reset! cljc/test-fail 0)
   (doseq [[tname f] @cljc/tests]
     (swap! cljc/test-context conj (str tname))
     (try (f) (catch Exception e
                (swap! cljc/test-fail inc)
                (println "ERROR" tname ":" (ex-message e))))
     (swap! cljc/test-context pop))
   (println "\nRan" (count @cljc/tests) "tests:"
            @cljc/test-pass "passed," @cljc/test-fail "failed.")
   (zero? @cljc/test-fail)))

(def run-all-tests run-tests)

;; ── discovery ───────────────────────────────────────────────────────────────
;; `cljc test` with no file arguments walks the tree from . for *_test.clj(c).
;; cljc/list-dir* is a native primitive, unlike fs.clj's opendir/readdir FFI
;; binding, so discovery works on Windows and on builds without FFI. It
;; returns nil for a non-directory and [] for an empty one -- and [] is
;; truthy -- so it doubles as the directory test.
(def cljc/test-skip-dirs #{"vendor" "target" "node_modules" "out"})

(defn cljc/test-file? [n]
  (or (str/ends-with? n "_test.clj") (str/ends-with? n "_test.cljc")))

(defn cljc/find-test-files
  ([] (cljc/find-test-files "."))
  ([dir]
   (vec (mapcat
          (fn [n]
            (let [p (if (= dir ".") n (str dir "/" n))]
              (cond
                (str/starts-with? n ".")          []      ; .git, .hidden
                (contains? cljc/test-skip-dirs n) []
                (cljc/list-dir* p)                (cljc/find-test-files p)
                (cljc/test-file? n)               [p]
                :else                             [])))
          (sort (or (cljc/list-dir* dir) []))))))
