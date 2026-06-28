(ns clojure.stacktrace
  "Minimal: cljc has no host stack traces, so these print the throwable's message/data.")
(defn root-cause [tr] tr)
(defn print-throwable [tr] (println (str (or (ex-message tr) tr)
                                         (when-let [d (ex-data tr)] (str " " (pr-str d))))))
(defn print-stack-trace [tr & _] (print-throwable tr))
(defn print-cause-trace [tr & _] (print-throwable tr))
(defn e [] (when *e (print-stack-trace *e)))
