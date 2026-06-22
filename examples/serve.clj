;; serve.clj — a self-contained gallery of the cljc examples, served over HTTP.
;;
;; This server is itself a cljc program: it uses the built-in tcp/* primitives
;; (the same ones clerk.clj uses) and shells out to ./cljc to run each example
;; and capture its real output. At startup it runs every example once, builds
;; one static HTML page (code + output, syntax-highlighted client-side by
;; highlight.js), then serves it.
;;
;; Run:  cljc examples/serve.clj          ; http://127.0.0.1:8088
;;       cljc examples/serve.clj 9000     ; custom port
;;
;; Run it from the repo root so ./cljc and examples/ resolve.

;; ── the gallery: filename, title, feature tag, and how to run it ──
;; A few examples take args or read stdin; we pin a representative invocation
;; and redirect </dev/null so nothing blocks on input.
(def gallery
  [{:file "mandelbrot.clj"  :title "Mandelbrot Set"      :tag "doubles · loop/recur"
    :run "./cljc examples/mandelbrot.clj </dev/null"}
   {:file "life.clj"        :title "Game of Life"        :tag "persistent sets · simulation"
    :run "./cljc examples/life.clj </dev/null"}
   {:file "primes.clj"      :title "Lazy Sequences"      :tag "lazy-seq · infinite streams"
    :run "./cljc examples/primes.clj </dev/null"}
   {:file "nqueens.clj"     :title "N-Queens"            :tag "backtracking · mapcat"
    :run "./cljc examples/nqueens.clj 6 </dev/null"}
   {:file "sudoku.clj"      :title "Sudoku Solver"       :tag "backtracking · clojure.set"
    :run "./cljc examples/sudoku.clj </dev/null"}
   {:file "calc.clj"        :title "Expression Parser"   :tag "recursive descent · regex"
    :run "./cljc examples/calc.clj </dev/null"}
   {:file "brainfuck.clj"   :title "Brainfuck Interpreter" :tag "mutable arrays · hosting a language"
    :run "./cljc examples/brainfuck.clj </dev/null"}
   {:file "macros.clj"      :title "Macros & DSLs"       :tag "defmacro · quasiquote · gensym"
    :run "./cljc examples/macros.clj </dev/null"}
   {:file "shapes.clj"      :title "Polymorphism"        :tag "protocols · multimethods · records"
    :run "./cljc examples/shapes.clj </dev/null"}
   {:file "dijkstra.clj"    :title "Shortest Paths"      :tag "maps as graphs · loop/recur"
    :run "./cljc examples/dijkstra.clj </dev/null"}
   {:file "wordfreq.clj"    :title "Word Frequency"      :tag "regex · frequencies · CLI"
    :run "./cljc examples/wordfreq.clj README.md 10 </dev/null"}
   {:file "bank.clj"        :title "Atoms & Exceptions"  :tag "atom · swap! · ex-info/try"
    :run "./cljc examples/bank.clj </dev/null"}
   {:file "ffi-demo.clj"    :title "C FFI"               :tag "ffi/define · native calls"
    :run "./cljc examples/ffi-demo.clj </dev/null"}
   {:file "sqlite.clj"      :title "SQLite via FFI"      :tag "libsqlite3 · shim header · prepare/step"
    :run "./cljc examples/sqlite.clj </dev/null"}
   {:file "fractal-svg.clj" :title "Fractal Tree (SVG)"  :tag "recursion · trig · file output"
    :run "./cljc examples/fractal-svg.clj /tmp/cljc-gallery-tree.svg 11 </dev/null"
    :svg "/tmp/cljc-gallery-tree.svg"}])

;; ── HTML helpers ──
(defn esc [s]
  (-> s
      (str/replace "&" "&amp;")
      (str/replace "<" "&lt;")
      (str/replace ">" "&gt;")))

(defn run-example [{:keys [run]}]
  (let [{:keys [out exit]} (sh run)]
    {:out (if (> (count out) 6000)
            (str (subs out 0 6000) "\n... (truncated)")
            out)
     :exit exit}))

(defn card [{:keys [file title tag svg] :as ex}]
  (let [src (slurp (str "examples/" file))
        {:keys [out exit]} (run-example ex)
        anchor (str/replace file ".clj" "")]
    (str "<section class='card' id='" anchor "'>"
         "<div class='cardhead'>"
         "<h2>" (esc title) "</h2>"
         "<span class='tag'>" (esc tag) "</span>"
         "<span class='file'>" file "</span>"
         "</div>"
         "<pre class='src'><code class='language-clojure'>" (esc src) "</code></pre>"
         "<div class='outlabel'>output" (when (not= exit 0)
                                          (str " <span class='err'>(exit " exit ")</span>")) "</div>"
         "<pre class='out'>" (esc out) "</pre>"
         (when svg
           (str "<div class='outlabel'>rendered artifact</div>"
                "<div class='svgwrap'>" (slurp svg) "</div>"))
         "</section>")))

(defn nav []
  (str "<nav>"
       (str/join ""
         (for [{:keys [file title]} gallery]
           (str "<a href='#" (str/replace file ".clj" "") "'>" (esc title) "</a>")))
       "</nav>"))

(def css "
:root{--bg:#0f1419;--panel:#1a2029;--ink:#d7dde5;--dim:#8a94a6;--accent:#7cc4ff;--green:#9ece6a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.55 -apple-system,Segoe UI,Roboto,sans-serif}
header{padding:48px 24px 24px;text-align:center;border-bottom:1px solid #232a35}
header h1{margin:0 0 8px;font-size:34px;letter-spacing:-.5px}
header p{margin:0;color:var(--dim);max-width:640px;margin-inline:auto}
header code{color:var(--accent)}
nav{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;padding:20px;max-width:1000px;margin:0 auto}
nav a{color:var(--dim);text-decoration:none;font-size:13px;padding:4px 10px;border:1px solid #2a323e;border-radius:20px}
nav a:hover{color:var(--accent);border-color:var(--accent)}
main{max-width:1000px;margin:0 auto;padding:0 24px 80px}
.card{background:var(--panel);border:1px solid #232a35;border-radius:12px;margin:28px 0;overflow:hidden}
.cardhead{display:flex;align-items:baseline;gap:12px;padding:16px 20px;border-bottom:1px solid #232a35;flex-wrap:wrap}
.cardhead h2{margin:0;font-size:20px}
.tag{color:var(--accent);font-size:12px;font-family:monospace}
.file{margin-left:auto;color:var(--dim);font-size:12px;font-family:monospace}
pre{margin:0;overflow-x:auto}
pre.src{padding:18px 20px;font-size:13px;background:#11161d}
pre.src code{font-family:'SF Mono',Menlo,Consolas,monospace;background:none;padding:0}
.outlabel{padding:10px 20px 4px;color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.err{color:#f7768e;text-transform:none}
pre.out{padding:4px 20px 18px;font-size:12.5px;line-height:1.4;color:var(--green);font-family:'SF Mono',Menlo,Consolas,monospace;white-space:pre}
.svgwrap{padding:16px 20px 22px;background:#fff;display:flex;justify-content:center}
.svgwrap svg{max-width:100%;height:auto}
footer{text-align:center;color:var(--dim);font-size:13px;padding:30px}
")

(defn page []
  (str "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
       "<title>cljc examples</title>"
       "<link rel='stylesheet' href='https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@11.9.0/build/styles/tokyo-night-dark.min.css'>"
       "<script src='https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@11.9.0/build/highlight.min.js'></script>"
       "<script src='https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@11.9.0/build/languages/clojure.min.js'></script>"
       "<script>addEventListener('DOMContentLoaded',()=>hljs.highlightAll())</script>"
       "<style>" css "</style></head><body>"
       "<header><h1>cljc · example gallery</h1>"
       "<p>A small Clojure in a single C file. Every program below was run by "
       "<code>cljc</code> to produce the output shown — this page was assembled and is served "
       "by <code>cljc</code> too.</p></header>"
       (nav)
       "<main>"
       (str/join "" (map card gallery))
       "</main>"
       "<footer>" (count gallery) " examples · generated by examples/serve.clj</footer>"
       "</body></html>"))

;; ── HTTP server (single-threaded, serves the prebuilt page) ──
(defn respond [fd status ctype body]
  (tcp/send fd (str "HTTP/1.1 " status
                    "\r\nContent-Type: " ctype
                    "\r\nContent-Length: " (count body)
                    "\r\nConnection: close\r\n\r\n" body))
  (tcp/close fd))

;; Bind all interfaces (0.0.0.0) so the gallery is reachable over a LAN or
;; tailscale, not just loopback. Pass a port as arg 1, a bind host as arg 2
;; (e.g. "127.0.0.1" to restrict to loopback again).
(let [port (if (seq *args*) (parse-long (first *args*)) 8088)
      host (if (>= (count *args*) 2) (second *args*) "0.0.0.0")]
  (println "Building gallery (running" (count gallery) "examples)...")
  (let [html (page)
        srv  (tcp/listen port host)]
    (println (str "Serving on " host ":" port "   (ctrl-c to stop)"))
    (println (str "  local: http://127.0.0.1:" port
                  "   (remote: http://<this-machine-ip>:" port ")"))
    (loop []
      (let [fd (tcp/accept srv 1000)]
        (when fd
          (let [req  (or (tcp/recv fd) "")
                m    (re-find #"^GET (\S+)" req)
                path (if m (second m) "/")]
            (if (or (= path "/") (str/starts-with? path "/#"))
              (respond fd "200 OK" "text/html; charset=utf-8" html)
              (respond fd "404 Not Found" "text/plain" "not found"))))
        (recur)))))
