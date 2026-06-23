;; http-app.clj — a small routed web app on cljc's async HTTP battery.
;;
;; Showcases http.clj's router (path params), response maps (custom status &
;; headers), and concurrent-safe shared state (an atom hit counter touched by
;; many connections) — all on the single-threaded coroutine event loop.
;;
;; Run:    cljc examples/http-app.clj            (port 8080, all interfaces)
;; Visit:  http://localhost:8080/   (also over LAN/tailscale on this host's IP)

(def *load-path* (into [".." "."] *load-path*))
(require '[http :as h] '[csp :as csp])

(def hits (atom 0))
(def guestbook (atom []))               ; in-memory state shared across requests

(defn home [_]
  (str "<!doctype html><meta charset=utf-8><title>cljc web app</title>"
       "<body style='font:16px system-ui;max-width:40em;margin:3em auto'>"
       "<h1>cljc async web app</h1>"
       "<p>Served by a <code>go</code> block on the coroutine event loop. "
       "This page has been served <b>" (swap! hits inc) "</b> times.</p>"
       "<ul>"
       "<li><a href='/hello/world'>/hello/:name</a> — path params</li>"
       "<li><a href='/api/stats'>/api/stats</a> — a JSON endpoint</li>"
       "<li><code>curl -d 'hi' localhost:8080/sign</code> — POST to the guestbook</li>"
       "</ul>"
       "<h3>Guestbook (" (count @guestbook) ")</h3><ul>"
       (str/join "" (for [m @guestbook] (str "<li>" m "</li>")))
       "</ul></body>"))

(defn hello [req]
  {:status 200 :body (str "<h1>Hello, " (:name (:params req)) "!</h1>")})

(defn stats [_]
  {:status 200
   :headers {"Content-Type" "application/json"}
   :body (str "{\"hits\":" @hits ",\"signatures\":" (count @guestbook) "}")})

(defn sign [req]
  (let [msg (str/trim (or (:body req) ""))]
    (when (seq msg) (swap! guestbook conj msg))
    {:status 201 :body (str "signed: " msg "\n")}))

(let [port (if (seq *args*) (parse-long (first *args*)) 8080)]
  (h/run-server port
    (h/router
      [[:get  "/"           home]
       [:get  "/hello/:name" hello]
       [:get  "/api/stats"  stats]
       [:post "/sign"       sign]])))
