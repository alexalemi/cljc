;; clerk.clj — Clerk-style literate notebooks for cljc.
;;
;;   cljc notebook notebook.clj [port]     live server (default 7878)
;;   cljc notebook somedir/ [port]         watch the tree; saving any .clj
;;                                         in it switches the view to it
;;   cljc notebook notebook.clj -o out.html  static single-file build
;;
;; A notebook is an ordinary .clj file: top-level ;; comments render as
;; markdown prose, every top-level form is a code cell evaluated in order
;; (shared env, errors isolated per cell). Single-; comments are invisible.
;; Directives above a form: ;; @clerk:hide-code  ;; @clerk:hide-result
;;
;; Uses only core natives (tcp/*, cljc/mtime*, with-out-str) — no FFI, so
;; it works without a C compiler available.

;; ── cell parser ──
;; Line scanner: track paren depth and in-string across lines; prose and
;; directives are only recognized at depth 0 between forms.

(defn cljc/clerk-scan-line
  "Advance [depth in-string?] across one line of code."
  [line depth instr]
  (loop [i 0 d depth instr instr]
    (if (>= i (count line))
      [d instr]
      (let [c (subs line i (inc i))]
        (cond
          instr (cond (= c "\\") (recur (+ i 2) d true)
                      (= c "\"") (recur (inc i) d false)
                      :else      (recur (inc i) d true))
          (= c ";")  [d false]                       ; comment to EOL
          (= c "\"") (recur (inc i) d true)
          (str/includes? "([{" c) (recur (inc i) (inc d) false)
          (str/includes? ")]}" c) (recur (inc i) (dec d) false)
          :else (recur (inc i) d false))))))

(defn clerk/parse
  "Notebook source → cells [{:kind :md :text s} {:kind :code :text s :dirs m}]."
  [src]
  (let [flush-cell (fn [cells mode acc dirs]
                     (let [acc (if (= mode :md)      ; drop trailing blank lines
                                 (loop [a acc]
                                   (if (and (seq a) (= (peek a) "")) (recur (pop a)) a))
                                 acc)]
                       (if (empty? acc)
                         cells
                         (conj cells (if (= mode :md)
                                       {:kind :md :text (str/join "\n" acc)}
                                       {:kind :code :text (str/join "\n" acc)
                                        :dirs dirs})))))]
    (loop [lines (str/split src "\n")
           cells [] mode nil acc [] depth 0 instr false dirs {}]
      (if (empty? lines)
        (flush-cell cells mode acc dirs)
        (let [line (first lines)
              more (rest lines)
              t (str/trim line)]
          (if (= mode :code)
            ;; inside a form: accumulate until depth returns to 0
            (let [[d i2] (cljc/clerk-scan-line line depth instr)
                  acc (conj acc line)]
              (if (and (<= d 0) (not i2))
                (recur more (flush-cell cells :code acc dirs) nil [] 0 false {})
                (recur more cells :code acc d i2 dirs)))
            ;; at top level
            (cond
              ;; directive: ;; @clerk:name [arg] — applies to the next code cell
              (re-matches #";; ?@clerk:.*" t)
              (let [[_ nm arg] (re-matches #";; ?@clerk:(\S+) ?(.*)" t)]
                (recur more (flush-cell cells mode acc dirs) nil []
                       0 false (assoc dirs nm (if (= arg "") true arg))))
              ;; prose: top-level ;; comment
              (str/starts-with? t ";;")
              (let [text (subs t 2)
                    text (if (str/starts-with? text " ") (subs text 1) text)]
                (if (= mode :md)
                  (recur more cells :md (conj acc text) 0 false dirs)
                  (recur more (flush-cell cells mode acc dirs) :md [text]
                         0 false dirs)))
              ;; blank: paragraph break inside prose, separator otherwise
              (= t "")
              (if (= mode :md)
                (recur more cells :md (conj acc "") 0 false dirs)
                (recur more (flush-cell cells mode acc dirs) nil [] 0 false dirs))
              ;; single-; comment: invisible (not rendered, not code)
              (str/starts-with? t ";")
              (recur more cells mode acc 0 false dirs)
              ;; code line
              :else
              (let [cells (flush-cell cells mode acc dirs)
                    [d i2] (cljc/clerk-scan-line line 0 false)]
                (if (and (<= d 0) (not i2))
                  (recur more (flush-cell cells :code [line] dirs) nil [] 0 false {})
                  (recur more cells :code [line] d i2 dirs))))))))))

;; ── evaluation ──

(defn clerk/eval-cell
  "Eval one code cell in the root env. Captures stdout; isolates errors."
  [text]
  (let [val (atom nil)
        err (atom nil)
        out (with-out-str
              (try (reset! val (eval (read-string (str "(do " text "\n)"))))
                   (catch Exception e (reset! err (ex-message e)))))]
    {:value @val :error @err :out out}))

;; ── incremental cache ──
;; Live editing re-renders the whole file on every save; naively that
;; re-evaluates every cell each time. We cache each code cell's eval RESULT
;; under a content key that folds in the cell text PLUS the keys of the earlier
;; cells it depends on — a cell depends on an earlier one when it refers to a
;; symbol that one defines (the def name is in a cell's refs too, so a redef
;; depends on the earlier def). So editing a cell re-runs it and everything
;; downstream, while untouched cells are reused.
;;
;; To keep the shared mutable env correct in CELL ORDER regardless of what
;; changed, a cached simple-def cell REPLAYS its binding each render (rebinds
;; the name to the cached value, no recompute) — so a later redef always wins,
;; and removing a redef falls back to the earlier def. Expression cells have no
;; env effect, so a cache hit just reuses them; macro/type/multi-form defs
;; (which we can't replay by value) recompute. Net: only changed cells and
;; their dependents recompute, but a def's expensive body never re-runs when
;; unchanged. Keyed by text (not value), so a whitespace edit to a dependency
;; still invalidates dependents: safe, just less optimal than nippy Clerk.

(def clerk/eval-cache (atom {}))

(defn clerk/syms-in
  "Every symbol appearing anywhere in a read form (an over-approx of refs)."
  [form]
  (cond
    (symbol? form) #{form}
    (or (list? form) (vector? form) (set? form))
      (reduce (fn [a f] (into a (clerk/syms-in f))) #{} (seq form))
    (map? form)
      (reduce (fn [a kv] (into (into a (clerk/syms-in (first kv)))
                               (clerk/syms-in (second kv)))) #{} (seq form))
    :else #{}))

(def clerk/def-heads
  #{'def 'defn 'defn- 'defmacro 'defonce 'defmulti 'defrecord 'deftype})

(defn clerk/form-defs
  "Top-level names a form defines (for dependency tracking), or nil."
  [form]
  (when (and (list? form) (symbol? (first form)))
    (cond
      (contains? clerk/def-heads (first form))
        (when (symbol? (second form)) #{(second form)})
      (= 'declare (first form)) (set (filter symbol? (rest form)))
      :else nil)))

(defn clerk/cell-analyze
  "{:defs #{names} :refs #{symbols}} for a cell's source (read errors -> empty)."
  [text]
  (let [forms (try (read-string (str "[" text "\n]")) (catch Exception e []))]
    {:defs (reduce (fn [a f] (into a (or (clerk/form-defs f) #{}))) #{} forms)
     :refs (reduce (fn [a f] (into a (clerk/syms-in f))) #{} forms)}))

(defn clerk/cell-key [text dep-keys]
  (hash [text (vec (sort dep-keys))]))

(defn clerk/simple-def-name
  "If a cell is exactly one simple def (def/defn/defn-/defonce name ...), the
  name — whose value we can re-assert from cache without recomputing the body.
  Expression cells and macro/type/multi-form defs return nil."
  [text]
  (let [forms (try (read-string (str "[" text "\n]")) (catch Exception e []))]
    (when (= 1 (count forms))
      (let [f (first forms)]
        (when (and (list? f)
                   (contains? #{'def 'defn 'defn- 'defonce} (first f))
                   (symbol? (second f)))
          (second f))))))

(defn clerk/replay-def!
  "Re-assert name -> cached value WITHOUT recomputing the body. Quote so any
  value (including a fn) binds literally."
  [name value]
  (eval (list 'def name (list 'quote value))))

;; ── html escaping + markdown subset ──

(defn clerk/escape [s]
  (-> (str s)
      (str/replace "&" "&amp;")
      (str/replace "<" "&lt;")
      (str/replace ">" "&gt;")))

(defn cljc/clerk-splice
  "Replace the leftmost re-find hit with (render groups), recursing on the
  text either side. Backref-free substitute for regex replace."
  [s re render recur-fn]
  (if-let [m (re-find re s)]
    (let [whole (if (vector? m) (first m) m)
          i (str/index-of s whole)]
      (str (recur-fn (subs s 0 i))
           (render m)
           (recur-fn (subs s (+ i (count whole))))))
    nil))

(defn cljc/clerk-inline-fmt
  "Links, bold, italic on already-escaped text (no backtick content)."
  [s]
  (or (cljc/clerk-splice s #"\[([^\]]+)\]\(([^)]+)\)"
                         (fn [[_ text url]]
                           (str "<a href=\"" url "\">" text "</a>"))
                         cljc/clerk-inline-fmt)
      (cljc/clerk-splice s #"\*\*([^*]+)\*\*"
                         (fn [[_ b]] (str "<strong>" b "</strong>"))
                         cljc/clerk-inline-fmt)
      (cljc/clerk-splice s #"\*([^*]+)\*"
                         (fn [[_ i]] (str "<em>" i "</em>"))
                         cljc/clerk-inline-fmt)
      s))

(defn cljc/clerk-inline
  "Inline markdown: split on backticks — odd segments are code spans."
  [s]
  (let [parts (str/split s "`")]
    (str/join ""
      (map-indexed (fn [i p]
                     (if (odd? i)
                       (str "<code>" (clerk/escape p) "</code>")
                       (cljc/clerk-inline-fmt (clerk/escape p))))
                   parts))))

(defn clerk/md->html
  "Markdown subset: #/##/### headers, - and 1. lists, paragraphs, inline
  code/links/bold/italic. $...$ passes through untouched for KaTeX."
  [text]
  (let [close-list (fn [html in-list] (if in-list (str html "</ul>") html))
        close-ol   (fn [html in-ol]   (if in-ol   (str html "</ol>") html))]
    (loop [lines (str/split text "\n") html "" para [] in-list false in-ol false]
      (let [flush-para (fn [html]
                         (if (empty? para)
                           html
                           (str html "<p>" (cljc/clerk-inline (str/join " " para)) "</p>")))]
        (if (empty? lines)
          (close-ol (close-list (flush-para html) in-list) in-ol)
          (let [line (str/trim (first lines))
                more (rest lines)]
            (cond
              (str/starts-with? line "### ")
              (recur more (str (close-ol (close-list (flush-para html) in-list) in-ol)
                               "<h3>" (cljc/clerk-inline (subs line 4)) "</h3>")
                     [] false false)
              (str/starts-with? line "## ")
              (recur more (str (close-ol (close-list (flush-para html) in-list) in-ol)
                               "<h2>" (cljc/clerk-inline (subs line 3)) "</h2>")
                     [] false false)
              (str/starts-with? line "# ")
              (recur more (str (close-ol (close-list (flush-para html) in-list) in-ol)
                               "<h1>" (cljc/clerk-inline (subs line 2)) "</h1>")
                     [] false false)
              (str/starts-with? line "- ")
              (recur more (str (close-ol (flush-para html) in-ol)
                               (if in-list "" "<ul>")
                               "<li>" (cljc/clerk-inline (subs line 2)) "</li>")
                     [] true false)
              (re-matches #"\d+\. .*" line)
              (let [[_ item] (re-matches #"\d+\. (.*)" line)]
                (recur more (str (close-list (flush-para html) in-list)
                                 (if in-ol "" "<ol>")
                                 "<li>" (cljc/clerk-inline item) "</li>")
                       [] false true))
              (= line "")
              (recur more (close-ol (close-list (flush-para html) in-list) in-ol)
                     [] false false)
              :else
              (recur more (close-ol (close-list html in-list) in-ol)
                     (conj para line) false false))))))))

;; ── code syntax highlighting (server-side, span classes) ──

(defn cljc/clerk-scan-while [s i pred]
  (loop [j i]
    (if (and (< j (count s)) (pred (subs s j (inc j)))) (recur (inc j)) j)))

;; Special forms + the def/macro family get their own colour so the most
;; salient tokens (defn, let, if, map-position keywords) actually stand out.
(def clerk/hl-specials
  #{"def" "defn" "defn-" "defmacro" "defmulti" "defmethod" "defrecord"
    "defprotocol" "deftype" "definline" "let" "letfn" "fn" "loop" "recur"
    "if" "if-not" "if-let" "if-some" "when" "when-not" "when-let" "when-some"
    "cond" "condp" "case" "do" "doseq" "dotimes" "for" "while" "try" "catch"
    "finally" "throw" "quote" "var" "ns" "require" "use" "import" "binding"
    "and" "or" "not" "->" "->>" "as->" "some->" "some->>" "cond->" "cond->>"
    "fn*" "let*" "lazy-seq" "delay" "future" "declare" "in-ns"})

(defn clerk/hl
  "Code → HTML with .com .str .kwd .lit .pun .spe spans."
  [code]
  (let [n (count code)
        token-char? (fn [c] (not (or (str/blank? c) (str/includes? "()[]{}\";" c))))]
    (loop [i 0 out ""]
      (if (>= i n)
        out
        (let [c (subs code i (inc i))]
          (cond
            (= c ";")
            (let [j (or (str/index-of code "\n" i) n)]
              (recur j (str out "<span class=\"com\">" (clerk/escape (subs code i j)) "</span>")))
            (= c "\"")
            (let [j (loop [k (inc i)]
                      (cond (>= k n) n
                            (= (subs code k (inc k)) "\\") (recur (+ k 2))
                            (= (subs code k (inc k)) "\"") (inc k)
                            :else (recur (inc k))))]
              (recur j (str out "<span class=\"str\">" (clerk/escape (subs code i j)) "</span>")))
            (= c ":")
            (let [j (cljc/clerk-scan-while code (inc i) token-char?)]
              (recur j (str out "<span class=\"kwd\">" (clerk/escape (subs code i j)) "</span>")))
            (str/includes? "0123456789" c)
            (let [j (cljc/clerk-scan-while code i token-char?)]
              (recur j (str out "<span class=\"lit\">" (clerk/escape (subs code i j)) "</span>")))
            (str/includes? "()[]{}" c)
            (recur (inc i) (str out "<span class=\"pun\">" (clerk/escape c) "</span>"))
            (token-char? c)
            (let [j (cljc/clerk-scan-while code i token-char?)
                  tok (subs code i j)]
              (recur j (str out (if (contains? clerk/hl-specials tok)
                                  (str "<span class=\"spe\">" (clerk/escape tok) "</span>")
                                  (clerk/escape tok)))))
            :else (recur (inc i) (str out (clerk/escape c)))))))))

;; ── viewers ──
;; Last registered wins. (clerk/register-viewer! pred render) from notebook
;; code customizes display; render returns an HTML string.

(def clerk/viewers (atom []))

(defn clerk/register-viewer! [pred render]
  (swap! clerk/viewers conj {:pred pred :render render}))

(defn cljc/clerk-table-row [tag cells]
  (str "<tr>" (str/join "" (map (fn [c] (str "<" tag ">" (clerk/escape (clerk/pr-bounded c)) "</" tag ">")) cells)) "</tr>"))

;; map → two-column key/value table
(clerk/register-viewer!
  (fn [v] (map? v))
  (fn [m]
    (str "<table>"
         (str/join "" (map (fn [[k v]] (cljc/clerk-table-row "td" [k v])) (seq m)))
         "</table>")))

;; seq of same-keyed maps → 2D table
(clerk/register-viewer!
  (fn [v] (and (sequential? v) (seq v) (every? map? v)))
  (fn [rows]
    (let [cols (keys (first rows))]
      (str "<table>"
           (cljc/clerk-table-row "th" cols)
           (str/join "" (map (fn [r] (cljc/clerk-table-row "td" (map (fn [k] (get r k)) cols))) rows))
           "</table>"))))

;; string that looks like markup → raw html (charts, svg, images)
(clerk/register-viewer!
  (fn [v] (and (string? v) (str/starts-with? v "<")))
  (fn [s] (str "<div class=\"raw\">" s "</div>")))

;; Bound a possibly-infinite/huge seq before printing — realize at most 100
;; elements so an infinite lazy seq (e.g. (iterate inc 0)) can't hang the
;; render. (count (take 101 v)) realizes ≤101 elements, never the whole seq.
(defn clerk/long-seq? [v]
  (and (sequential? v) (> (count (take 101 v)) 100)))

(defn clerk/pr-bounded [v]
  (if (clerk/long-seq? v)
    (str "(" (str/join " " (map clerk/pr-bounded (take 100 v))) " …)")
    (pr-str v)))

(defn clerk/render-value [v]
  (if (clerk/long-seq? v)
    (str "<pre class=\"value\">" (clerk/escape (clerk/pr-bounded v)) "</pre>")
    (or (some (fn [{:keys [pred render]}]
                (when (try (pred v) (catch Exception e nil)) (render v)))
              (reverse @clerk/viewers))
        (str "<pre class=\"value\">" (clerk/escape (pr-str v)) "</pre>"))))

;; (clerk/html "<svg>...</svg>") — explicit raw-html marker for notebooks
(defn clerk/html [s] (str s))

;; ── cell + page rendering ──

(defn clerk/render-md [c]
  (str "<section class=\"cell md\">" (clerk/md->html (:text c)) "</section>"))

(defn clerk/render-code
  "Render an ALREADY-evaluated code cell (result r) to HTML."
  [c r]
  (let [dirs (:dirs c)]
    (str "<section class=\"cell code" (when (:error r) " bad") "\">"
         (when-not (get dirs "hide-code")
           (str "<pre class=\"src\">" (clerk/hl (:text c)) "</pre>"))
         (when (seq (:out r))
           (str "<pre class=\"out\">" (clerk/escape (:out r)) "</pre>"))
         (cond
           (:error r) (str "<pre class=\"err\">" (clerk/escape (:error r)) "</pre>")
           (get dirs "hide-result") ""
           (nil? (:value r)) ""
           (fn? (:value r)) ""          ; defn cells: the source is the story
           :else (clerk/render-value (:value r)))
         "</section>")))

(defn clerk/render-cell
  "Render a single cell with no caching (md, or eval+render a code cell)."
  [c]
  (if (= (:kind c) :md)
    (clerk/render-md c)
    (clerk/render-code c (clerk/eval-cell (:text c)))))

(def cljc/clerk-css "
body{background:#f8f8f8;color:#383838;margin:0;
     font:17px/1.65 'Source Serif 4',Georgia,'Linux Libertine',serif}
main{max-width:760px;margin:2.5rem auto;padding:0 1.2rem}
h1,h2,h3{font-family:'Source Sans 3',Helvetica,Arial,sans-serif;line-height:1.25}
a{color:#804040}
pre,code{font:13.5px/1.55 'JetBrains Mono','Fira Code',Inconsolata,monospace}
p>code{background:#efeeea;padding:0 .25em;border-radius:3px}
.cell{margin:1.1rem 0}
pre{overflow-x:auto;margin:0}
.src{background:#efeeea;padding:.65rem .9rem;border-radius:6px}
.out{border-left:3px dashed #b8b2a7;padding:.25rem .9rem;color:#56524c;margin-top:.45rem}
.err{border-left:3px solid #b03030;background:#f6e7e7;padding:.5rem .9rem;margin-top:.45rem}
.value{border-left:3px solid #b8b2a7;padding:.25rem .9rem;margin-top:.45rem}
.raw{margin-top:.45rem}
table{border-collapse:collapse;margin-top:.45rem;
      font:13.5px/1.5 'JetBrains Mono',monospace}
td,th{border:1px solid #cfcabf;padding:.22rem .6rem;text-align:left}
th{background:#efeeea}
.com{color:#9a917f;font-style:italic}.str{color:#85605c}.kwd{color:#3a5e8c}
.lit{color:#406040}.pun{color:#9a917f}.spe{color:#8a3fa0;font-weight:600}
")

(def cljc/clerk-katex
  (str "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/katex@0.16.10/dist/katex.min.css\">"
       "<script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.10/dist/katex.min.js\"></script>"
       "<script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.10/dist/contrib/auto-render.min.js\" "
       "onload=\"renderMathInElement(document.body,{delimiters:[{left:'$$',right:'$$',display:true},{left:'$',right:'$',display:false}]})\"></script>"))

(def cljc/clerk-sse-js
  "<script>new EventSource('/events').onmessage=function(e){if(e.data==='reload')location.reload()};</script>")

(defn clerk/page [title cells-html live?]
  (str "<!doctype html><html><head><meta charset=\"utf-8\">"
       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
       "<title>" (clerk/escape title) "</title>"
       "<style>" cljc/clerk-css "</style>"
       cljc/clerk-katex
       "</head><body><main>" cells-html "</main>"
       (if live? cljc/clerk-sse-js "")
       "</body></html>"))

(defn clerk/render-file [path live?]
  ;; Walk cells in order, threading the prior code cells' {:defs :key}. Each
  ;; code cell's eval is cached by a key folding in its text + the keys of the
  ;; earlier cells it depends on, so a save re-runs only changed cells and
  ;; their downstream dependents (clerk/eval-cached). Markdown re-renders each
  ;; time — it's cheap and pure.
  (let [cells (clerk/parse (slurp path))]
    (loop [cs cells, priors [], acc []]
      (if (empty? cs)
        (clerk/page path (str/join "" acc) live?)
        (let [c (first cs)]
          (if (= (:kind c) :md)
            (recur (rest cs) priors (conj acc (clerk/render-md c)))
            (let [{:keys [defs refs]} (clerk/cell-analyze (:text c))
                  rname (clerk/simple-def-name (:text c))
                  dep-keys (->> priors
                                (filter (fn [p] (some (fn [r] (contains? (:defs p) r)) refs)))
                                (mapv :key))
                  k (clerk/cell-key (:text c) dep-keys)
                  cached (@clerk/eval-cache k)
                  ev (fn [] (let [res (clerk/eval-cell (:text c))]
                              (swap! clerk/eval-cache assoc k res) res))
                  r (cond
                      ;; cached simple def: re-assert its binding cheaply (no
                      ;; recompute), keeping the env in cell order — so a later
                      ;; redef wins, and removing a redef falls back correctly
                      (and cached rname) (do (clerk/replay-def! rname (:value cached)) cached)
                      ;; macro/type/multi-form def: can't replay by value, so
                      ;; recompute to keep the env correct (these are cheap)
                      (and (seq defs) (not rname)) (ev)
                      ;; cached pure expression: reuse (it has no env effect)
                      cached cached
                      ;; cache miss: evaluate and store
                      :else (ev))]
              (recur (rest cs)
                     (conj priors {:defs defs :key k})
                     (conj acc (clerk/render-code c r))))))))))

;; ── static build ──

(defn clerk/build [path out]
  (spit out (clerk/render-file path false))
  out)

;; ── live server: single-threaded HTTP + SSE ──
;; The 250ms accept timeout doubles as the file-watch tick; SSE clients are
;; kept as raw fds and receive \"data: reload\" when the notebook changes.

(defn cljc/clerk-respond [fd status ctype body]
  (tcp/send fd (str "HTTP/1.1 " status
                    "\r\nContent-Type: " ctype
                    "\r\nContent-Length: " (count body)
                    "\r\nConnection: close\r\n\r\n" body))
  (tcp/close fd))

(defn cljc/clerk-handle
  "Serve one request; returns the (possibly grown) SSE client vector."
  [fd html sse]
  (let [req (or (tcp/recv fd) "")
        m (re-find #"^GET (\S+)" req)
        path (if m (second m) nil)]
    (cond
      (= path "/events")
      (do (tcp/send fd (str "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream"
                            "\r\nCache-Control: no-cache\r\nConnection: keep-alive"
                            "\r\n\r\nretry: 500\n\n"))
          (conj sse fd))
      (= path "/")
      (do (cljc/clerk-respond fd "200 OK" "text/html; charset=utf-8" html) sse)
      :else
      (do (cljc/clerk-respond fd "404 Not Found" "text/plain" "not found") sse))))

(defn cljc/clerk-render-safe [path old-html]
  (try (clerk/render-file path true)
       (catch Exception e
         (println "clerk: render failed:" (ex-message e))
         old-html)))

(defn cljc/clerk-broadcast
  "Send a reload to every SSE client; returns the survivors."
  [sse]
  (filterv (fn [c] (tcp/send c "data: reload\n\n")) sse))

(defn cljc/clerk-serve-file [path port]
  (let [srv (tcp/listen port)]
    (println (str "clerk: " path " → http://127.0.0.1:" port "  (ctrl-c to stop)"))
    (loop [html (cljc/clerk-render-safe path "") mt (cljc/mtime* path) sse []]
      (let [fd (tcp/accept srv 250)
            sse (if fd (cljc/clerk-handle fd html sse) sse)
            nmt (cljc/mtime* path)]
        (if (and nmt (not= nmt mt))
          (recur (cljc/clerk-render-safe path html) nmt (cljc/clerk-broadcast sse))
          (recur html mt sse))))))

;; ── directory mode: watch the whole tree, show whichever file changed ──

(defn cljc/clerk-walk
  "All .clj files under dir, recursive; skips dot-directories."
  [dir]
  (mapcat (fn [n]
            (let [p (str dir "/" n)]
              (cond
                (str/starts-with? n ".") []
                (cljc/dir?* p) (cljc/clerk-walk p)
                (str/ends-with? n ".clj") [p]
                :else [])))
          (or (cljc/list-dir* dir) [])))

(defn cljc/clerk-snapshot
  "Map of path → mtime for every .clj under dir."
  [dir]
  (reduce (fn [m p] (assoc m p (cljc/mtime* p))) {} (cljc/clerk-walk dir)))

(defn cljc/clerk-changed
  "Some path that is new or has a different mtime than in old, else nil."
  [old new]
  (some (fn [[p t]] (when (not= t (get old p)) p)) (seq new)))

(defn cljc/clerk-newest [snap]
  (first (last (sort-by (fn [[_ t]] t) (seq snap)))))

(defn cljc/clerk-serve-dir [dir port]
  (let [srv (tcp/listen port)
        snap (cljc/clerk-snapshot dir)
        cur (cljc/clerk-newest snap)
        empty-page (clerk/page dir "<p>no .clj files here yet — save one and it will appear.</p>" true)]
    (println (str "clerk: " dir "/ (" (count snap) " files) → http://127.0.0.1:" port "  (ctrl-c to stop)"))
    (when cur (println (str "clerk: showing " cur)))
    (loop [cur cur
           html (if cur (cljc/clerk-render-safe cur empty-page) empty-page)
           snap snap sse []]
      (let [fd (tcp/accept srv 250)
            sse (if fd (cljc/clerk-handle fd html sse) sse)
            nsnap (cljc/clerk-snapshot dir)
            chg (cljc/clerk-changed snap nsnap)]
        (if chg
          (do (when (not= chg cur) (println (str "clerk: showing " chg)))
              (recur chg (cljc/clerk-render-safe chg html) nsnap (cljc/clerk-broadcast sse)))
          (recur cur html nsnap sse))))))

(defn clerk/serve
  "Serve a notebook file, or a directory — in directory mode every .clj in
  the tree is watched and saving any of them switches the view to it."
  ([path] (clerk/serve path 7878))
  ([path port]
   (if (cljc/dir?* path)
     (cljc/clerk-serve-dir path port)
     (cljc/clerk-serve-file path port))))

;; ── CLI entry (invoked by `cljc clerk ...`) ──

(defn clerk/main []
  (let [args *args*
        file (first args)]
    (cond
      (nil? file)
      (println "usage: cljc notebook <file.clj | dir> [port] [-o out.html]")
      (= (second args) "-o")
      (if-let [out (nth args 2 nil)]
        (println "clerk: wrote" (clerk/build file out))
        (println "usage: cljc clerk <notebook.clj> -o out.html"))
      :else
      (clerk/serve file (or (when (second args) (parse-long (second args))) 7878)))))
