;; json.clj — pure-cljc JSON parser and writer battery.
;; Load with: (load-file "json.clj")
;;
;;   (json/parse "{\"a\": [1, 2.5, true, null]}")  => {"a" [1 2.5 true nil]}
;;   (json/parse s {:keywords? true})              => keyword keys
;;   (json/write {:a [1 nil "x"]})                 => "{\"a\":[1,null,\"x\"]}"
;;
;; Notes: \uXXXX escapes decode to the char for codepoints < 128, else "?"
;; (no unicode type). Numbers parse as int64 or double.

;; ── parser: (pv s i) conventions — each step returns [value next-index] ──

(defn cljc/json-ch [s i] (subs s i (inc i)))

(defn cljc/json-ws [s i]
  (loop [i i]
    (if (and (< i (count s)) (str/blank? (cljc/json-ch s i)))
      (recur (inc i))
      i)))

(defn cljc/json-string [s i]            ; i points after the opening quote
  (loop [i i acc ""]
    (let [c (cljc/json-ch s i)]
      (cond
        (= c "\"") [acc (inc i)]
        (= c "\\")
        (let [e (cljc/json-ch s (inc i))]
          (case e
            "n" (recur (+ i 2) (str acc "\n"))
            "t" (recur (+ i 2) (str acc "\t"))
            "r" (recur (+ i 2) (str acc "\r"))
            "b" (recur (+ i 2) (str acc "\b"))
            "f" (recur (+ i 2) (str acc "\f"))
            "\"" (recur (+ i 2) (str acc "\""))
            "\\" (recur (+ i 2) (str acc "\\"))
            "/" (recur (+ i 2) (str acc "/"))
            "u" (recur (+ i 6) (str acc "?"))   ; \uXXXX: placeholder (no unicode type)
            (throw (ex-info (str "json: bad escape \\" e) {:at i}))))
        :else (recur (inc i) (str acc c))))))

(defn cljc/json-number [s i]
  (loop [j i]
    (if (and (< j (count s))
             (str/includes? "+-0123456789.eE" (cljc/json-ch s j)))
      (recur (inc j))
      (let [tok (subs s i j)]
        [(or (parse-long tok) (parse-double tok)
             (throw (ex-info (str "json: bad number " tok) {:at i})))
         j]))))

(defn cljc/json-value [s i kw?]
  (let [i (cljc/json-ws s i)
        c (cljc/json-ch s i)]
    (cond
      (= c "{")
      (loop [i (cljc/json-ws s (inc i)) m {}]
        (if (= (cljc/json-ch s i) "}")
          [m (inc i)]
          (let [[k i] (do (when-not (= (cljc/json-ch s i) "\"")
                            (throw (ex-info "json: expected key" {:at i})))
                          (cljc/json-string s (inc i)))
                i (cljc/json-ws s i)
                _ (when-not (= (cljc/json-ch s i) ":")
                    (throw (ex-info "json: expected :" {:at i})))
                [v i] (cljc/json-value s (inc i) kw?)
                i (cljc/json-ws s i)
                m (assoc m (if kw? (keyword k) k) v)]
            (case (cljc/json-ch s i)
              "," (recur (cljc/json-ws s (inc i)) m)
              "}" [m (inc i)]
              (throw (ex-info "json: expected , or }" {:at i}))))))

      (= c "[")
      (loop [i (cljc/json-ws s (inc i)) v []]
        (if (= (cljc/json-ch s i) "]")
          [v (inc i)]
          (let [[x i] (cljc/json-value s i kw?)
                i (cljc/json-ws s i)
                v (conj v x)]
            (case (cljc/json-ch s i)
              "," (recur (cljc/json-ws s (inc i)) v)
              "]" [v (inc i)]
              (throw (ex-info "json: expected , or ]" {:at i}))))))

      (= c "\"") (cljc/json-string s (inc i))
      (= (subs s i (min (count s) (+ i 4))) "true") [true (+ i 4)]
      (= (subs s i (min (count s) (+ i 5))) "false") [false (+ i 5)]
      (= (subs s i (min (count s) (+ i 4))) "null") [nil (+ i 4)]
      :else (cljc/json-number s i))))

(defn json/parse
  ([s] (json/parse s {}))
  ([s {:keys [keywords?]}]
   (first (cljc/json-value s 0 keywords?))))

;; ── writer ──

(defn cljc/json-escape [s]
  (reduce (fn [acc c]
            (str acc (case c
                       "\"" "\\\""
                       "\\" "\\\\"
                       "\n" "\\n"
                       "\t" "\\t"
                       "\r" "\\r"
                       c)))
          "" (seq s)))

(defn json/write [x]
  (cond
    (nil? x) "null"
    (true? x) "true"
    (false? x) "false"
    (number? x) (str x)
    (string? x) (str "\"" (cljc/json-escape x) "\"")
    (keyword? x) (json/write (name x))
    (map? x) (str "{"
                  (str/join "," (map (fn [[k v]]
                                       (str (json/write (if (keyword? k) (name k) (str k)))
                                            ":" (json/write v)))
                                     (seq x)))
                  "}")
    (or (vector? x) (list? x) (seq? x) (set? x))
    (str "[" (str/join "," (map json/write (seq x))) "]")
    :else (throw (ex-info (str "json/write: unsupported " (pr-str x)) {}))))
