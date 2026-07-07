(ns clojure.java.io
  "A pragmatic subset over cljc's slurp/spit. `file` yields a path string;
   reader/writer/streams are the path (slurp/spit accept paths)."
  (:require [clojure.string :as str]))
(defn file [& parts] (str/join "/" (map str parts)))
(defn as-relative-path [x] (str x))
(defn as-url [x] (str x))
(defn reader
  "A line-reader over reader-ish things (StringReader., another reader, a
   StringBuilder); plain strings stay path strings for slurp compatibility."
  [x]
  (if (and (map? x) (contains? #{:java.io.Reader :LineReader :StringBuilder}
                               (:cljc/type x)))
    (java.io.BufferedReader. x)
    (str x)))
(defn writer
  "A StringWriter/StringBuilder passes through (its .write/.append/.toString
   accumulate); anything else is a path string for spit."
  [x]
  (if (and (map? x) (= :StringBuilder (:cljc/type x))) x (str x)))
(defn input-stream [x] (str x))
(defn output-stream [x] (str x))
(defn resource [_] nil)
(defn copy [from to] (spit (str to) (slurp (str from))) nil)
(defn make-parents [_] nil)
(defn delete-file [_ & [_silently]] nil)
