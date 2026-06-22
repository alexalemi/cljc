;; csp-http-server.clj — a concurrent async HTTP server on cljc's coroutine
;; event loop. Every connection is handled by its own `go` block; the scheduler
;; multiplexes them all over a single thread with poll() — no threads, no locks.
;;
;; Showcases: the async I/O event loop (accept!/recv!/send! park on socket
;; readiness instead of blocking), one goroutine per connection, and shared
;; state (a hit counter) mutated safely across concurrent handlers because the
;; scheduler is cooperative.
;;
;; Run:  cljc examples/csp-http-server.clj          (port 8090, all interfaces)
;;       cljc examples/csp-http-server.clj 9000
;; Then: curl localhost:8090   (or open it in a browser, also over tailscale)

(def *load-path* (into [".." "."] *load-path*))
(require '[csp :as a])

(def hits (atom 0))

(defn page [n]
  (str "<!doctype html><meta charset=utf-8>"
       "<title>cljc async server</title>"
       "<body style='font:16px system-ui;max-width:40em;margin:3em auto'>"
       "<h1>Served by cljc 🧵</h1>"
       "<p>This response came from a <code>go</code> block, one of possibly many"
       " running concurrently on a <b>single thread</b> — multiplexed by a"
       " poll()-driven event loop built on stackful coroutines.</p>"
       "<p>Request #" n " since startup.</p>"
       "</body>"))

(defn http-response [body]
  (str "HTTP/1.1 200 OK\r\n"
       "Content-Type: text/html; charset=utf-8\r\n"
       "Content-Length: " (count body) "\r\n"
       "Connection: close\r\n\r\n" body))

;; Handle one connection: read the request (we ignore its contents), bump the
;; shared counter, write a response, close. Parks on the socket via recv!/etc,
;; so a slow client never stalls the others.
(defn handle [conn]
  (a/go
    (a/recv! conn)                       ; read (and discard) the request
    (let [n (swap! hits inc)]
      (a/send! conn (http-response (page n)))
      (tcp/close conn))))

(let [port (if (seq *args*) (parse-long (first *args*)) 8090)
      srv  (tcp/listen port "0.0.0.0")]
  (println (str "Async HTTP server on http://127.0.0.1:" port
                "  (also reachable on this machine's LAN/tailscale IP)"))
  (println "Each request is handled by its own coroutine. Ctrl-C to stop.")
  ;; accept forever; spawn a handler goroutine per connection
  (a/go-loop []
    (when-let [conn (a/accept! srv)]
      (handle conn))
    (recur))
  (a/run!))      ; enter the event loop (blocks in poll until connections arrive)
