;; clojure.data.json — shim over the json.clj battery.

(ns clojure.data.json)

(load-file "json.clj")

(defn read-str
  "(read-str s) or (read-str s :key-fn keyword). Only keyword key-fns
  change behavior (string keys otherwise)."
  [s & kvs]
  (let [opts (apply hash-map kvs)]
    (json/parse s {:keywords? (= (get opts :key-fn) keyword)})))

(def write-str json/write)
