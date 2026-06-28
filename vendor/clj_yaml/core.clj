(ns clj-yaml.core
  "A pure-Clojure YAML subset (cljc has no SnakeYAML). Covers the common
   config-file surface: block mappings/sequences with indentation, the
   compact `- key: val` form, plain/quoted scalars, flow collections
   ([a, b] / {a: 1}), `#` comments, `---`/`...` document markers, and the
   `|` (literal) / `>` (folded) block scalars. Public API mirrors the real
   clj-yaml.core: parse-string / generate-string (+ -stream aliases)."
  (:require [clojure.string :as str]))

;; ---------------------------------------------------------------------------
;; Scalars (shared by block + flow parsers)
;; ---------------------------------------------------------------------------

(declare pf parse-scalar)

(defn- unescape-dq [s]
  (loop [i 0 acc []]
    (if (>= i (count s))
      (apply str acc)
      (let [c (nth s i)]
        (if (and (= c \\) (< (inc i) (count s)))
          (let [n (nth s (inc i))]
            (recur (+ i 2)
                   (conj acc (case n
                               \n \newline \t \tab \r \return
                               \" \" \\ \\ \0 \0 n))))
          (recur (inc i) (conj acc c)))))))

(def ^:private int-re #"[-+]?\d+")
(def ^:private float-re #"[-+]?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?")

(defn- parse-scalar [s kw]
  (let [s (str/trim s)]
    (cond
      (= s "") nil
      (#{"null" "Null" "NULL" "~"} s) nil
      (#{"true" "True" "TRUE"} s) true
      (#{"false" "False" "FALSE"} s) false
      (and (>= (count s) 2) (= \" (first s)) (= \" (last s)))
      (unescape-dq (subs s 1 (dec (count s))))
      (and (>= (count s) 2) (= \' (first s)) (= \' (last s)))
      (str/replace (subs s 1 (dec (count s))) "''" "'")
      (str/starts-with? s "[") (first (pf s 0 kw))
      (str/starts-with? s "{") (first (pf s 0 kw))
      (re-matches int-re s) (parse-long s)
      (and (re-matches float-re s) (re-find #"[.eE]" s)) (parse-double s)
      :else s)))

;; ---------------------------------------------------------------------------
;; Flow collections: [a, b]  {a: 1, b: 2}  (char-cursor recursive descent)
;; ---------------------------------------------------------------------------

(defn- skip-sp [s i]
  (loop [i i] (if (and (< i (count s)) (#{\space \tab} (nth s i))) (recur (inc i)) i)))

(defn- pf-dq [s i]                      ; i past opening "
  (loop [i i acc []]
    (let [c (nth s i)]
      (cond
        (= c \") [(unescape-dq (apply str acc)) (inc i)]
        (and (= c \\) (< (inc i) (count s))) (recur (+ i 2) (conj acc c (nth s (inc i))))
        :else (recur (inc i) (conj acc c))))))

(defn- pf-sq [s i]                      ; i past opening '
  (loop [i i acc []]
    (let [c (nth s i)]
      (cond
        (and (= c \') (< (inc i) (count s)) (= (nth s (inc i)) \')) (recur (+ i 2) (conj acc \'))
        (= c \') [(apply str acc) (inc i)]
        :else (recur (inc i) (conj acc c))))))

(defn- pf-plain [s i kw stops]
  (loop [j i]
    (if (or (>= j (count s)) (stops (nth s j)))
      [(parse-scalar (str/trim (subs s i j)) kw) j]
      (recur (inc j)))))

(defn- pf-seq [s i kw]
  (loop [i i acc []]
    (let [i (skip-sp s i)]
      (cond
        (>= i (count s)) [acc i]
        (= (nth s i) \]) [acc (inc i)]
        (= (nth s i) \,) (recur (inc i) acc)
        :else (let [[v ni] (pf s i kw)] (recur ni (conj acc v)))))))

(defn- pf-key [s i kw]
  (let [i (skip-sp s i) c (nth s i)]
    (cond
      (= c \") (let [[v ni] (pf-dq s (inc i))] [(if kw (keyword v) v) ni])
      (= c \') (let [[v ni] (pf-sq s (inc i))] [(if kw (keyword v) v) ni])
      :else (let [[v ni] (pf-plain s i false #{\: \, \}})]
              [(if (and kw (string? v)) (keyword v) v) ni]))))

(defn- pf-map [s i kw]
  (loop [i i acc {}]
    (let [i (skip-sp s i)]
      (cond
        (>= i (count s)) [acc i]
        (= (nth s i) \}) [acc (inc i)]
        (= (nth s i) \,) (recur (inc i) acc)
        :else (let [[k ni] (pf-key s i kw)
                    ni (skip-sp s ni)
                    ni (if (and (< ni (count s)) (= (nth s ni) \:)) (inc ni) ni)
                    [v nni] (pf s ni kw)]
                (recur nni (assoc acc k v)))))))

(defn- pf [s i kw]
  (let [i (skip-sp s i) c (nth s i)]
    (cond
      (= c \[) (pf-seq s (inc i) kw)
      (= c \{) (pf-map s (inc i) kw)
      (= c \") (pf-dq s (inc i))
      (= c \') (pf-sq s (inc i))
      :else (pf-plain s i kw #{\, \] \}}))))

;; ---------------------------------------------------------------------------
;; Line model: strip comments, record indent. Raw lines kept for block scalars.
;; ---------------------------------------------------------------------------

(defn- strip-comment [line]
  (loop [i 0 q nil]
    (if (>= i (count line))
      line
      (let [c (nth line i)]
        (cond
          q (recur (inc i) (if (= c q) nil q))
          (or (= c \") (= c \')) (recur (inc i) c)
          (and (= c \#) (or (zero? i) (#{\space \tab} (nth line (dec i))))) (subs line 0 i)
          :else (recur (inc i) nil))))))

(defn- indent-of [line] (count (take-while #(= % \space) line)))

(defn- to-lines [s]
  ;; vector of [indent text rawline]; blank/comment/`---`/`...` lines dropped
  (->> (str/split-lines s)
       (map (fn [raw] (let [t (str/trimr (strip-comment raw))]
                        [(indent-of raw) (str/trim t) raw])))
       (remove (fn [[_ t _]] (or (str/blank? t) (= t "---") (= t "...") (str/starts-with? t "%"))))
       vec))

;; ---------------------------------------------------------------------------
;; Block parser
;; ---------------------------------------------------------------------------

(defn- dash? [t] (or (= t "-") (str/starts-with? t "- ")))

(defn- key-colon
  "Index of the ':' separating key from value (followed by space or EOL),
   ignoring colons inside quotes or flow collections. nil if not a map line."
  [t]
  (loop [i 0 q nil depth 0]
    (if (>= i (count t))
      nil
      (let [c (nth t i)]
        (cond
          q (recur (inc i) (if (= c q) nil q) depth)
          (or (= c \") (= c \')) (recur (inc i) c depth)
          (#{\[ \{} c) (recur (inc i) nil (inc depth))
          (#{\] \}} c) (recur (inc i) nil (dec depth))
          (and (= c \:) (zero? depth)
               (or (= i (dec (count t))) (= \space (nth t (inc i))))) i
          :else (recur (inc i) nil depth))))))

(declare parse-block)

(defn- fold-lines
  "Folded (`>`) scalar joining: lines within a paragraph join with a space,
   a blank line becomes a paragraph break. No regex lookahead — cljc's
   engine doesn't support it."
  [lines]
  (reduce (fn [acc l]
            (cond
              (str/blank? l) (str acc "\n")
              (or (= acc "") (str/ends-with? acc "\n")) (str acc l)
              :else (str acc " " l)))
          "" lines))

(defn- block-scalar
  "Collect a `|` / `>` block scalar. indic is the indicator text (after | or >).
   Following raw lines more-indented than `indent` form the body."
  [lines i indent kind chomp kw]
  (let [body (loop [j i acc []]
               (if (>= j (count lines))
                 [acc j]
                 (let [raw (nth (nth lines j) 2)]
                   (if (str/blank? raw)
                     (recur (inc j) (conj acc ""))
                     (if (> (indent-of raw) indent)
                       (recur (inc j) (conj acc raw))
                       [acc j])))))
        [raws nexti] body
        ;; dedent by the block's base indentation (first non-blank line's indent)
        base (or (some #(when-not (str/blank? %) (indent-of %)) raws) (inc indent))
        stripped (map (fn [r] (if (>= (count r) base) (subs r base) (str/triml r))) raws)
        ;; drop trailing blank lines for folding/clipping
        stripped (vec stripped)
        joined (if (= kind \|)
                 (str/join "\n" stripped)
                 (fold-lines stripped))
        clipped (str/replace joined #"\n+$" "")
        result (case chomp
                 \- clipped
                 \+ (str clipped "\n")
                 (str clipped "\n"))]
    [result nexti]))

(defn- parse-value-after-key
  "Value when a `key:` has an inline remainder vstr and following lines."
  [lines i indent vstr kw]
  (cond
    ;; block scalar indicator
    (or (str/starts-with? vstr "|") (str/starts-with? vstr ">"))
    (let [kind (first vstr)
          chomp (when (> (count vstr) 1) (nth vstr 1))]
      (block-scalar lines (inc i) indent kind chomp kw))
    ;; inline scalar / flow collection
    (not= vstr "")
    [(parse-scalar vstr kw) (inc i)]
    ;; empty: nested block on following lines
    :else
    (let [ni (inc i)]
      (if (and (< ni (count lines))
               (let [[ci ct] (nth lines ni)]
                 (or (> ci indent) (and (= ci indent) (dash? ct)))))
        (parse-block lines ni kw)
        [nil ni]))))

(defn- parse-map [lines i indent kw]
  (loop [i i acc {}]
    (if (and (< i (count lines))
             (= (first (nth lines i)) indent)
             (let [t (second (nth lines i))] (and (not (dash? t)) (key-colon t))))
      (let [t (second (nth lines i))
            kc (key-colon t)
            kstr (str/trim (subs t 0 kc))
            vstr (str/trim (subs t (inc kc)))
            kv (parse-scalar kstr false)
            k (if (and kw (string? kv)) (keyword kv) kv)
            [v ni] (parse-value-after-key lines i indent vstr kw)]
        (recur ni (assoc acc k v)))
      [acc i])))

(defn- parse-seq [lines i indent kw]
  (loop [i i acc []]
    (if (and (< i (count lines))
             (= (first (nth lines i)) indent)
             (dash? (second (nth lines i))))
      (let [t (second (nth lines i))
            after (subs t 1)
            lead (count (take-while #(= % \space) after))
            content (subs after lead)
            child-indent (+ indent 1 lead)]
        (if (= content "")
          (let [ni (inc i)]
            (if (and (< ni (count lines)) (> (first (nth lines ni)) indent))
              (let [[v nni] (parse-block lines ni kw)] (recur nni (conj acc v)))
              (recur ni (conj acc nil))))
          ;; compact "- content": re-anchor this line at child-indent and parse a block
          (let [lines2 (assoc lines i [child-indent content (nth (nth lines i) 2)])
                [v ni] (parse-block lines2 i kw)]
            (recur ni (conj acc v)))))
      [acc i])))

(defn- parse-block [lines i kw]
  (let [[ind t] (nth lines i)]
    (cond
      (dash? t) (parse-seq lines i ind kw)
      (key-colon t) (parse-map lines i ind kw)
      :else [(parse-scalar t kw) (inc i)])))

;; ---------------------------------------------------------------------------
;; Public API
;; ---------------------------------------------------------------------------

(defn parse-string
  "Parse a YAML string into Clojure data. Options (keyword args):
   :keywords (default true) — convert string map keys to keywords."
  [s & opts]
  (let [opts (apply hash-map opts)
        kw (get opts :keywords true)
        lines (to-lines s)]
    (if (empty? lines) nil (first (parse-block lines 0 kw)))))

(def parse-stream parse-string)

;; ---------------------------------------------------------------------------
;; Emitter (block style)
;; ---------------------------------------------------------------------------

(defn- needs-quote? [s]
  (or (= s "")
      (re-matches int-re s)
      (and (re-matches float-re s) (re-find #"[.eE]" s))
      (#{"true" "false" "null" "~" "yes" "no" "on" "off"
         "True" "False" "Null" "Yes" "No" "On" "Off"} s)
      (boolean (re-find #":\s" s))
      (= \: (last s))
      (boolean (re-find #"\s#" s))
      (boolean (re-find #"[\n\t]" s))
      (#{\space \- \? \: \, \[ \] \{ \} \# \& \* \! \| \> \' \" \% \@ \`} (first s))
      (= \space (last s))))

(defn- scalar->yaml [v]
  (cond
    (nil? v) "null"
    (boolean? v) (str v)
    (keyword? v) (name v)
    (symbol? v) (str v)
    (number? v) (pr-str v)
    (string? v) (if (needs-quote? v) (pr-str v) v)
    :else (pr-str (str v))))

(declare gen-map gen-seq)

(defn- pad [n] (apply str (repeat n \space)))

(defn- gen-map [m indent]
  (let [p (pad indent)]
    (mapcat (fn [[k v]]
              (let [ks (scalar->yaml (if (keyword? k) k k))]
                (cond
                  (and (map? v) (seq v)) (cons (str p ks ":") (gen-map v (+ indent 2)))
                  (map? v) [(str p ks ": {}")]
                  (and (sequential? v) (seq v)) (cons (str p ks ":") (gen-seq v indent))
                  (sequential? v) [(str p ks ": []")]
                  :else [(str p ks ": " (scalar->yaml v))])))
            m)))

(defn- gen-seq [xs indent]
  (let [p (pad indent)]
    (mapcat (fn [v]
              (cond
                (and (map? v) (seq v))
                (let [ls (gen-map v (+ indent 2))]
                  (cons (str p "- " (str/triml (first ls))) (rest ls)))
                (and (sequential? v) (seq v))
                (let [ls (gen-seq v (+ indent 2))]
                  (cons (str p "- " (str/triml (first ls))) (rest ls)))
                :else [(str p "- " (scalar->yaml v))]))
            xs)))

(defn generate-string
  "Render Clojure data as a block-style YAML string."
  [data & _opts]
  (str (str/join "\n"
                 (cond
                   (and (map? data) (empty? data)) ["{}"]
                   (map? data) (gen-map data 0)
                   (and (sequential? data) (empty? data)) ["[]"]
                   (sequential? data) (gen-seq data 0)
                   :else [(scalar->yaml data)]))
       "\n"))

(def generate-stream generate-string)
