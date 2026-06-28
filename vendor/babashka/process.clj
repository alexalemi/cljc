(ns babashka.process
  "babashka.process API over cljc's process.clj battery (FFI fork/exec).")
;; load the battery as top-level (user ns) so its bare internals stay global —
;; loading under babashka.process would mis-namespace them.
(cljc/in-ns* nil)
(load-file "process.clj")
(cljc/in-ns* "babashka.process")
(def sh    process/sh)
(def shell process/shell)
(defn process [& args] (apply process/sh (remove map? args)))
(defn check [p] p)
