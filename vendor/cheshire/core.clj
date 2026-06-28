(ns cheshire.core
  "cheshire.core API over the vendored clojure.data.json (babashka's `json` alias)."
  (:require [clojure.data.json :as json]))

(defn generate-string [x & [_opts]] (json/write-str x))
(defn encode [x & [_opts]] (json/write-str x))
(defn parse-string
  ([s] (json/read-str s))
  ([s key-fn & _]
   (json/read-str s :key-fn (if (or (true? key-fn) (= key-fn keyword)) keyword identity))))
(defn parse-string-strict
  ([s] (parse-string s))
  ([s key-fn & _] (parse-string s key-fn)))
(def decode parse-string)
