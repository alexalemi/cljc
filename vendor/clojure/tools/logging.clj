(ns clojure.tools.logging
  "A minimal console logger (cljc has no slf4j/log4j): logs to *err*.")

(def ^:dynamic *level* :debug)
(def ^:private order {:trace 0 :debug 1 :info 2 :warn 3 :error 4 :fatal 5})

(defn enabled? [level] (>= (order level 2) (order *level* 2)))

(defn log*
  [level throwable message]
  (when (enabled? level)
    (binding [*out* *err*]
      (println (str "[" (clojure.string/upper-case (name level)) "]")
               message
               (if throwable (str \newline throwable) "")))))

(defmacro log   [level & args] `(log* ~level nil (print-str ~@args)))
(defmacro trace [& args] `(log* :trace nil (print-str ~@args)))
(defmacro debug [& args] `(log* :debug nil (print-str ~@args)))
(defmacro info  [& args] `(log* :info  nil (print-str ~@args)))
(defmacro warn  [& args] `(log* :warn  nil (print-str ~@args)))
(defmacro error [& args] `(log* :error nil (print-str ~@args)))
(defmacro fatal [& args] `(log* :fatal nil (print-str ~@args)))
(defmacro tracef [fmt & args] `(log* :trace nil (format ~fmt ~@args)))
(defmacro debugf [fmt & args] `(log* :debug nil (format ~fmt ~@args)))
(defmacro infof  [fmt & args] `(log* :info  nil (format ~fmt ~@args)))
(defmacro warnf  [fmt & args] `(log* :warn  nil (format ~fmt ~@args)))
(defmacro errorf [fmt & args] `(log* :error nil (format ~fmt ~@args)))
(defmacro fatalf [fmt & args] `(log* :fatal nil (format ~fmt ~@args)))
(defmacro spy
  ([expr] `(let [v# ~expr] (debug (pr-str '~expr) "=>" (pr-str v#)) v#))
  ([level expr] `(let [v# ~expr] (log ~level (pr-str '~expr) "=>" (pr-str v#)) v#)))
