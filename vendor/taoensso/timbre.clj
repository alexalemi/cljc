(ns taoensso.timbre
  "A minimal Timbre-compatible console logger (logs to *err*)."
  (:require [clojure.string :as str]))

(def ^:private order {:trace 0 :debug 1 :info 2 :warn 3 :error 4 :fatal 5 :report 6})
(def ^:dynamic *config* {:min-level :debug})
(defn set-min-level! [level] (alter-var-root #'*config* assoc :min-level level))
(defn level-enabled? [level] (>= (order level 2) (order (:min-level *config* :debug) 2)))

(defn log! [level args]
  (when (level-enabled? level)
    (cljc/eprintln* (str (str/upper-case (name level)) " ") (apply print-str args))))

(defmacro log   [level & args] `(log! ~level [~@args]))
(defmacro trace [& args] `(log! :trace  [~@args]))
(defmacro debug [& args] `(log! :debug  [~@args]))
(defmacro info  [& args] `(log! :info   [~@args]))
(defmacro warn  [& args] `(log! :warn   [~@args]))
(defmacro error [& args] `(log! :error  [~@args]))
(defmacro fatal [& args] `(log! :fatal  [~@args]))
(defmacro report [& args] `(log! :report [~@args]))
(defmacro tracef [fmt & a] `(log! :trace [(format ~fmt ~@a)]))
(defmacro debugf [fmt & a] `(log! :debug [(format ~fmt ~@a)]))
(defmacro infof  [fmt & a] `(log! :info  [(format ~fmt ~@a)]))
(defmacro warnf  [fmt & a] `(log! :warn  [(format ~fmt ~@a)]))
(defmacro errorf [fmt & a] `(log! :error [(format ~fmt ~@a)]))
(defmacro spy
  ([expr] `(let [v# ~expr] (debug (pr-str '~expr) "=>" (pr-str v#)) v#))
  ([level expr] `(let [v# ~expr] (log ~level (pr-str '~expr) "=>" (pr-str v#)) v#)))
