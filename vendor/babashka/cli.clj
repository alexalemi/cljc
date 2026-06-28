(ns babashka.cli
  "A pragmatic subset of babashka.cli: parse-opts / parse-args.
   Handles --long, --long=val, -short flags, and :keyword val pairs, with
   :coerce / :alias / :exec-args options."
  (:require [clojure.string :as str]))

(defn- coerce1 [c v]
  (cond
    (nil? c) v
    (fn? c)  (c v)
    (vector? c) v
    :else (case c
            (:long :int)     (if (string? v) (parse-long v) v)
            :double          (if (string? v) (parse-double v) v)
            (:boolean :bool) (if (string? v) (not (#{"false" "0" "no"} v)) (boolean v))
            :keyword         (keyword (str v))
            :symbol          (symbol (str v))
            :string          (str v)
            v)))

(defn- coerce-val [coerce k v] (coerce1 (get coerce k) v))

(defn- parse* [args {:keys [coerce alias exec-args]}]
  (loop [args (seq args) m (or exec-args {}) extra []]
    (if (empty? args)
      {:opts m :args extra}
      (let [a (first args)]
        (cond
          (= a "--") {:opts m :args (into extra (next args))}

          (and (string? a) (str/starts-with? a "--") (str/includes? a "="))
          (let [[k v] (str/split (subs a 2) #"=" 2) kk (keyword k)]
            (recur (next args) (assoc m kk (coerce-val coerce kk v)) extra))

          (and (string? a) (str/starts-with? a "--"))
          (let [kk (keyword (subs a 2)) nx (second args)]
            (if (and (string? nx) (not (str/starts-with? nx "-")))
              (recur (nnext args) (assoc m kk (coerce-val coerce kk nx)) extra)
              (recur (next args) (assoc m kk true) extra)))

          (or (keyword? a) (and (string? a) (str/starts-with? a ":")))
          (let [kk (if (keyword? a) a (keyword (subs a 1))) nx (second args)]
            (recur (nnext args) (assoc m kk (coerce-val coerce kk nx)) extra))

          (and (string? a) (str/starts-with? a "-") (> (count a) 1))
          (let [kk (let [s (keyword (subs a 1))] (get alias s s))]
            (recur (next args) (assoc m kk true) extra))

          :else (recur (next args) m (conj extra a)))))))

(defn parse-opts
  ([args] (parse-opts args {}))
  ([args opts] (:opts (parse* args opts))))

(defn parse-args
  ([args] (parse-args args {}))
  ([args opts] (parse* args opts)))
