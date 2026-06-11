;; clojure.test — namespace shim over the flat test.clj battery, so that
;; (require '[clojure.test :as test]) and test/deftest etc. work.

(ns clojure.test)

(load-file "test.clj")

;; Each def's value resolves to the flat battery binding; the def itself
;; lands under clojure.test/ (home-ns stamping). Macro-ness travels.
(def deftest deftest)
(def is is)
(def are are)
(def testing testing)
(def run-tests run-tests)
(def run-all-tests run-all-tests)
(def use-fixtures use-fixtures)
