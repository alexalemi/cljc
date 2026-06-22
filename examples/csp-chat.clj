;; csp-chat.clj — a broadcast chat server: every line a client sends is
;; broadcast to all connected clients. Built on the coroutine event loop +
;; core.async's mult/tap fan-out. Many concurrent clients, one thread, no locks.
;;
;; Showcases everything together: the async I/O event loop (accept!/recv!/send!
;; park on socket readiness), one pair of go blocks per client (a reader and a
;; writer), and `mult`/`tap` to broadcast one stream to all of them.
;;
;; Run:    cljc examples/csp-chat.clj           (port 8077, all interfaces)
;; Connect (in other terminals):
;;         nc localhost 8077        (or:  telnet localhost 8077)
;; Type in one; watch it appear in the others.

(def *load-path* (into [".." "."] *load-path*))
(require '[csp :as a])

(def bcast (a/chan))        ; everything anyone says flows in here ...
(def hub   (a/mult bcast))  ; ... and mult fans it out to every client's tap
(def users (atom 0))

(defn broadcast! [line] (a/go (a/>! bcast line)))

(defn handle-client [conn name]
  (let [out (a/chan 32)]                 ; this client's personal outbound queue
    (a/tap hub out)                      ; subscribe it to the broadcast
    (a/send! conn (str "*** welcome, " name " — type away ***\n"))
    (broadcast! (str "*** " name " joined ***\n"))
    ;; writer: drain this client's tap → its socket
    (a/go-loop []
      (when-let [msg (a/<! out)]
        (a/send! conn msg)
        (recur)))
    ;; reader: this client's socket → the broadcast
    (a/go-loop []
      (let [line (a/recv! conn)]
        (if (nil? line)                  ; disconnected
          (do (a/untap hub out) (a/close! out) (tcp/close conn)
              (broadcast! (str "*** " name " left ***\n")))
          (do (broadcast! (str name "> " (str/trim line) "\n"))
              (recur)))))))

(let [port (if (seq *args*) (parse-long (first *args*)) 8077)
      srv  (tcp/listen port "0.0.0.0")]
  (println (str "Chat server on port " port
                " — connect with `nc localhost " port "` from a few terminals."))
  (a/go-loop []
    (when-let [conn (a/accept! srv)]
      (handle-client conn (str "user-" (swap! users inc))))
    (recur))
  (a/run!))
