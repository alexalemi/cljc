;; nextjournal.clerk — shim so Clerk-annotated notebooks run as plain
;; scripts (and render under `cljc notebook`, which reads the same files).
;; Viewer-ish constructors pass data through; side-effecting Clerk API
;; calls are no-ops.

(ns nextjournal.clerk)

(defn html [x] (str x))
(def md identity)
(def table identity)
(def plotly identity)
(def vl identity)
(def code identity)

(defn show! [& _] nil)
(defn serve! [& _] nil)
(defn build! [& _] nil)
(defn build-static-app! [& _] nil)
(defn clear-cache! [& _] nil)
(defn add-viewers! [& _] nil)
(defn recompute! [& _] nil)
(def doc-url (fn [& _] ""))
