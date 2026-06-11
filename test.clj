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

(defn run-tests []
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
  (zero? @cljc/test-fail))
