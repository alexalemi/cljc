;; nrepl.clj — a CONCURRENT, multi-client nREPL server in cljc, on the csp
;; coroutine event loop. The built-in C nREPL (./cljc --nrepl) is single-client
;; and serial; this one handles many editor sessions at once, each connection a
;; go block multiplexed on one thread.
;;
;; Speaks bencode nREPL (clone/describe/eval/load-file/close/ls-sessions), so
;; Conjure / CIDER / Calva can connect. All sessions share the one root env
;; (cljc has a single global environment), so defs are visible across clients.
;;
;; Run:  cljc -e "(require '[nrepl]) (nrepl/serve 7889)"
;;   or  add a wrapper script. Writes .nrepl-port for editor discovery.

(require '[csp :as csp])

;; ── bencode ────────────────────────────────────────────────────────────────
(defn bencode [v]
  (cond
    (string? v)  (str (count v) ":" v)
    (int? v)     (str "i" v "e")
    (vector? v)  (str "l" (apply str (map bencode v)) "e")
    (map? v)     (str "d" (apply str (for [k (sort (keys v))]
                                       (str (bencode k) (bencode (v k))))) "e")
    (nil? v)     "0:"
    :else        (bencode (str v))))

;; Decode ONE bencode value from the front of s. Returns [value rest] or nil
;; when s holds an incomplete value (the caller should read more bytes).
(defn bdecode [s]
  (when (pos? (count s))
    (let [c (subs s 0 1)]
      (cond
        (= c "i") (when-let [e (str/index-of s "e")]
                    [(parse-long (subs s 1 e)) (subs s (inc e))])
        (= c "l") (loop [r (subs s 1) acc []]
                    (cond (= "" r) nil
                          (str/starts-with? r "e") [acc (subs r 1)]
                          :else (when-let [[v r2] (bdecode r)] (recur r2 (conj acc v)))))
        (= c "d") (loop [r (subs s 1) acc {}]
                    (cond (= "" r) nil
                          (str/starts-with? r "e") [acc (subs r 1)]
                          :else (when-let [[k r1] (bdecode r)]
                                  (when-let [[v r2] (bdecode r1)] (recur r2 (assoc acc k v))))))
        (re-find #"[0-9]" c)
        (when-let [colon (str/index-of s ":")]
          (let [n (parse-long (subs s 0 colon)) start (inc colon)]
            (when (>= (count s) (+ start n))
              [(subs s start (+ start n)) (subs s (+ start n))])))
        :else nil))))

;; ── ops ──────────────────────────────────────────────────────────────────
(defn- reply [conn m] (csp/send! conn (bencode m)))

(defn- do-eval [conn id sess code]
  (let [v (atom nil) err (atom nil)
        out (with-out-str
              (try (reset! v (eval (read-string (str "(do " code "\n)"))))
                   (catch Exception e (reset! err (or (ex-message e) "error")))))]
    (when (seq out) (reply conn {"id" id "session" sess "out" out}))
    (if @err
      (reply conn {"id" id "session" sess "err" (str @err "\n")
                   "status" ["done" "eval-error"]})
      (reply conn {"id" id "session" sess "value" (pr-str @v)
                   "ns" "user" "status" ["done"]}))))

(defn- handle [conn msg]
  (let [op   (msg "op")
        id   (or (msg "id") "")
        sess (or (msg "session") "none")]
    (cond
      (= op "clone")    (reply conn {"id" id "session" sess
                                     "new-session" (str (gensym "session-")) "status" ["done"]})
      (= op "describe") (reply conn {"id" id "session" sess "status" ["done"]
                                     "ops" {"eval" {} "clone" {} "close" {}
                                            "describe" {} "load-file" {} "ls-sessions" {}}
                                     "versions" {"cljc" {"version-string" "0.1.0"}}})
      (= op "eval")     (do-eval conn id sess (or (msg "code") ""))
      (= op "load-file") (do-eval conn id sess (or (msg "file") ""))
      (= op "ls-sessions") (reply conn {"id" id "status" ["done"] "sessions" []})
      (= op "close")    (do (reply conn {"id" id "session" sess "status" ["done"]})
                            (tcp/close conn))
      :else             (reply conn {"id" id "session" sess "status" ["done" "unknown-op"]}))))

;; ── per-connection loop ──────────────────────────────────────────────────
;; Accumulate bytes; decode and dispatch every complete message; keep the
;; leftover partial bytes for the next read.
(defn- client-loop [conn]
  (loop [buf ""]
    (let [chunk (csp/recv! conn)]
      (if (nil? chunk)
        (tcp/close conn)
        (recur (loop [b (str buf chunk)]
                 (let [decoded (bdecode b)]
                   (if decoded
                     (let [[msg r] decoded] (handle conn msg) (recur r))
                     b))))))))

;; ── server ───────────────────────────────────────────────────────────────
;; start: set up the accept loop (one goroutine per client) and return the
;; listening socket. The caller drives the scheduler — serve does both.
(defn start [port]
  (let [srv (tcp/listen port "127.0.0.1")]      ; loopback, like the C nREPL
    (csp/go-loop []
      (when-let [conn (csp/accept! srv)]
        (csp/go (client-loop conn)))
      (recur))
    srv))

(defn serve
  ([] (serve 7889))
  ([port]
   (start port)
   (spit ".nrepl-port" (str port))              ; editor discovery
   (println (str "nrepl: concurrent bencode nREPL on 127.0.0.1:" port
                  "  (many clients) — ctrl-c to stop"))
   (csp/run!)))
