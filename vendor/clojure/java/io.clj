(ns clojure.java.io
  "A pragmatic subset over cljc's slurp/spit. `file` yields a path string;
   reader/writer/streams are the path (slurp/spit accept paths)."
  (:require [clojure.string :as str]))
(defn file [& parts] (str/join "/" (map str parts)))
(defn as-relative-path [x] (str x))
(defn as-url [x] (str x))
(defn reader [x] (str x))
(defn writer [x] (str x))
(defn input-stream [x] (str x))
(defn output-stream [x] (str x))
(defn resource [_] nil)
(defn copy [from to] (spit (str to) (slurp (str from))) nil)
(defn make-parents [_] nil)
(defn delete-file [_ & [_silently]] nil)
