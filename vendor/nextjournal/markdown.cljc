(ns nextjournal.markdown
  "A pure-Clojure CommonMark-subset parser for cljc (the upstream :clj impl wraps
   the Java commonmark lib, which cljc has no access to). Produces the same AST
   nextjournal.markdown.transform/->hiccup consumes."
  (:require [clojure.string :as str]
            [nextjournal.markdown.transform :as transform]))

(def empty-doc {:type :doc :content []})

;; ── inline ────────────────────────────────────────────────────────────────
(defn- find-close [s open] ;; index in s of the next `open` substring, or nil
  (str/index-of s open))

(declare parse-inline)

(defn- text-node [t] {:type :text :text t})

(defn parse-inline [s]
  (loop [s s, acc []]
    (if (or (nil? s) (= "" s))
      acc
      (cond
        ;; code span  `code`
        (and (str/starts-with? s "`") (find-close (subs s 1) "`"))
        (let [e (inc (find-close (subs s 1) "`"))]
          (recur (subs s (inc e)) (conj acc {:type :monospace :content [(text-node (subs s 1 e))]})))
        ;; strong  **x**  or  __x__
        (and (str/starts-with? s "**") (find-close (subs s 2) "**"))
        (let [e (+ 2 (find-close (subs s 2) "**"))]
          (recur (subs s (+ e 2)) (conj acc {:type :strong :content (parse-inline (subs s 2 e))})))
        (and (str/starts-with? s "__") (find-close (subs s 2) "__"))
        (let [e (+ 2 (find-close (subs s 2) "__"))]
          (recur (subs s (+ e 2)) (conj acc {:type :strong :content (parse-inline (subs s 2 e))})))
        ;; emphasis  *x*  or  _x_
        (and (str/starts-with? s "*") (find-close (subs s 1) "*"))
        (let [e (inc (find-close (subs s 1) "*"))]
          (recur (subs s (inc e)) (conj acc {:type :em :content (parse-inline (subs s 1 e))})))
        (and (str/starts-with? s "_") (find-close (subs s 1) "_"))
        (let [e (inc (find-close (subs s 1) "_"))]
          (recur (subs s (inc e)) (conj acc {:type :em :content (parse-inline (subs s 1 e))})))
        ;; link  [text](url)
        (re-find #"^\[([^\]]*)\]\(([^)\s]*)[^)]*\)" s)
        (let [[m txt url] (re-find #"^\[([^\]]*)\]\(([^)\s]*)[^)]*\)" s)]
          (recur (subs s (count m)) (conj acc {:type :link :attrs {:href url} :content (parse-inline txt)})))
        ;; plain run: up to the next marker (at least one char)
        :else
        (let [idxs (keep #(let [i (find-close (subs s 1) %)] (when i (inc i))) ["`" "*" "_" "["])
              cut  (if (seq idxs) (apply min idxs) (count s))]
          (recur (subs s cut) (conj acc (text-node (subs s 0 cut)))))))))

;; ── blocks ────────────────────────────────────────────────────────────────
(defn- blank? [l] (str/blank? l))
(defn- heading? [l] (re-find #"^(#{1,6})\s+(.*)$" l))
(defn- fence? [l] (re-find #"^```\s*(\S*)\s*$" l))
(defn- hr? [l] (re-find #"^\s*(-{3,}|\*{3,}|_{3,})\s*$" l))
(defn- bullet? [l] (re-find #"^\s*[-*+]\s+(.*)$" l))
(defn- numbered? [l] (re-find #"^\s*\d+[.)]\s+(.*)$" l))
(defn- quote? [l] (re-find #"^>\s?(.*)$" l))

(declare parse-blocks)

(defn- list-item [text] {:type :list-item :content [{:type :plain :content (parse-inline text)}]})

(defn parse-blocks [lines]
  (loop [ls (seq lines) acc []]
    (if (nil? ls)
      acc
      (let [l (first ls)]
        (cond
          (blank? l) (recur (next ls) acc)

          (heading? l)
          (let [[_ hashes text] (heading? l)]
            (recur (next ls)
                   (conj acc {:type :heading :heading-level (count hashes)
                              :content (parse-inline text)})))

          (fence? l)
          (let [lang (second (fence? l))
                [body rest-ls] (split-with #(not (re-find #"^```" %)) (next ls))]
            (recur (next rest-ls)
                   (conj acc {:type :code :language (when (seq lang) lang)
                              :content [(text-node (str/join "\n" body))]})))

          (hr? l) (recur (next ls) (conj acc {:type :ruler}))

          (quote? l)
          (let [[qls rest-ls] (split-with quote? ls)
                inner (map #(second (quote? %)) qls)]
            (recur rest-ls (conj acc {:type :blockquote :content (parse-blocks inner)})))

          (or (bullet? l) (numbered? l))
          (let [num? (boolean (numbered? l))
                pred (if num? numbered? bullet?)
                [items rest-ls] (split-with #(or (pred %) (and (not (blank? %)) (str/starts-with? % " "))) ls)
                texts (keep #(let [m (pred %)] (when m (second m))) items)]
            (recur rest-ls
                   (conj acc {:type (if num? :numbered-list :bullet-list)
                              :content (mapv list-item texts)})))

          :else ;; paragraph: gather consecutive plain lines
          (let [stop? (fn [x] (or (blank? x) (heading? x) (fence? x) (hr? x)
                                  (bullet? x) (numbered? x) (quote? x)))
                [pls rest-ls] (split-with #(not (stop? %)) ls)]
            (recur rest-ls
                   (conj acc {:type :paragraph
                              :content (parse-inline (str/join " " pls))}))))))))

;; ── public API ──────────────────────────────────────────────────────────
(defn parse*
  ([markdown] (parse* {} markdown))
  ([_ctx markdown] {:type :doc :content (parse-blocks (str/split-lines (str markdown)))}))

(defn parse
  ([markdown] (parse {} markdown))
  ([_opts markdown] (parse* markdown)))

(def default-hiccup-renderers transform/default-hiccup-renderers)

(defn ->hiccup
  ([markdown] (->hiccup default-hiccup-renderers markdown))
  ([renderers markdown]
   (transform/->hiccup renderers (if (string? markdown) (parse markdown) markdown))))

(def node->text transform/->text)
(def into-hiccup transform/into-markup)
