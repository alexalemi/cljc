;; http.clj — a small async HTTP/1.1 server + client for cljc, on the csp
;; coroutine event loop. Everything is non-blocking: the server handles many
;; connections concurrently on one thread, and the client returns a channel so
;; it composes inside go blocks (N concurrent requests via pipeline-async).
;;
;; Load with: (require '[http :as h])
;;
;; Server:
;;   (h/run-server 8080 (h/router [[:get "/" (fn [req] "hello")]
;;                                 [:get "/u/:id" (fn [req] (:id (:params req)))]]))
;;   ;; a handler returns a string (→ 200 text/html) or
;;   ;; {:status 200 :headers {"Content-Type" "..."} :body "..."}
;; Client (returns a channel):
;;   (csp/<!! (h/get "http://127.0.0.1:8080/u/42"))   ; => {:status 200 :headers .. :body ..}

(require '[csp :as csp])

;; ── parsing ──────────────────────────────────────────────────────────────
(defn- parse-query [qs]
  (if (or (nil? qs) (= "" qs))
    {}
    (into {} (for [pair (str/split qs "&") :when (str/includes? pair "=")]
               (let [i (str/index-of pair "=")]
                 [(subs pair 0 i) (subs pair (inc i))])))))

(defn- parse-headers [lines]
  (into {} (for [l lines :when (str/includes? l ":")]
             (let [i (str/index-of l ":")]
               [(str/lower-case (str/trim (subs l 0 i))) (str/trim (subs l (inc i)))]))))

(defn- parse-request-head [head]
  (let [lines (str/split head "\r\n")
        [reqline & hdrs] lines
        parts (str/split reqline " ")
        target (or (second parts) "/")
        qmark (str/index-of target "?")]
    {:method  (keyword (str/lower-case (first parts)))
     :path    (if qmark (subs target 0 qmark) target)
     :query   (parse-query (when qmark (subs target (inc qmark))))
     :headers (parse-headers hdrs)}))

;; Read one full request off a connection (headers, then Content-Length bytes of
;; body). Returns the request map, or nil if the client hung up first.
(defn- read-request [conn]
  (loop [buf ""]
    (let [idx (str/index-of buf "\r\n\r\n")]
      (if idx
        (let [req  (parse-request-head (subs buf 0 idx))
              clen (parse-long (or ((:headers req) "content-length") "0"))]
          (loop [body (subs buf (+ idx 4))]
            (if (>= (count body) clen)
              (assoc req :body (subs body 0 clen))
              (let [more (csp/recv! conn)]
                (if (nil? more) (assoc req :body body) (recur (str body more)))))))
        (let [more (csp/recv! conn)]
          (if (nil? more) nil (recur (str buf more))))))))

(def ^:private status-text
  {200 "OK" 201 "Created" 204 "No Content" 301 "Moved Permanently"
   302 "Found" 304 "Not Modified" 400 "Bad Request" 401 "Unauthorized"
   403 "Forbidden" 404 "Not Found" 405 "Method Not Allowed"
   500 "Internal Server Error" 503 "Service Unavailable"})

(defn- normalize [resp]
  (cond (map? resp) resp
        (string? resp) {:status 200 :body resp}
        :else {:status 200 :body (str resp)}))

(defn- response->str [resp]
  (let [{:keys [status headers body] :or {status 200}} (normalize resp)
        body (or body "")
        has-ct? (some (fn [k] (= "content-type" (str/lower-case k))) (keys headers))]
    (str "HTTP/1.1 " status " " (or (status-text status) "OK") "\r\n"
         (when-not has-ct? "Content-Type: text/html; charset=utf-8\r\n")
         "Content-Length: " (count body) "\r\n"
         (str/join "" (for [[k v] headers] (str k ": " v "\r\n")))
         "Connection: close\r\n\r\n"
         body)))

;; ── routing ────────────────────────────────────────────────────────────────
;; A pattern segment ":name" captures that path segment into :params.
(defn- match-path [pattern path]
  (let [ps (str/split pattern "/")
        xs (str/split path "/")]
    (when (= (count ps) (count xs))
      (reduce (fn [acc [p x]]
                (cond (nil? acc) nil
                      (str/starts-with? p ":") (assoc acc (keyword (subs p 1)) x)
                      (= p x) acc
                      :else nil))
              {} (map vector ps xs)))))

(defn router [routes]
  (fn [req]
    (or (some (fn [[m pat h]]
                (when (= m (:method req))
                  (when-let [params (match-path pat (:path req))]
                    (h (assoc req :params params)))))
              routes)
        {:status 404 :body "Not Found"})))

;; ── server ───────────────────────────────────────────────────────────────
;; Returns the listening socket; spawns the accept loop as a go block. Call
;; (csp/run!) to drive it (or use run-server, which does both).
(defn serve [port handler]
  (let [srv (tcp/listen port "0.0.0.0")]
    (csp/go-loop []
      (when-let [conn (csp/accept! srv)]
        (csp/go                          ; one goroutine per connection
          (when-let [req (read-request conn)]
            (let [resp (try (handler req)
                            (catch Exception e
                              {:status 500 :body (str "Internal Error: " (ex-message e))}))]
              (csp/send! conn (response->str resp))))
          (tcp/close conn))
        (recur)))
    srv))

(defn run-server [port handler]
  (serve port handler)
  (println (str "http: serving on http://0.0.0.0:" port "  (ctrl-c to stop)"))
  (csp/run!))

;; ── client (returns a channel) ─────────────────────────────────────────────
(defn- parse-url [url]
  (let [u (if (str/starts-with? url "http://") (subs url 7) url)
        slash (str/index-of u "/")
        hostport (if slash (subs u 0 slash) u)
        path (if slash (subs u slash) "/")
        colon (str/index-of hostport ":")]
    [(if colon (subs hostport 0 colon) hostport)
     (if colon (parse-long (subs hostport (inc colon))) 80)
     path]))

(defn- parse-response [raw]
  (let [idx (str/index-of raw "\r\n\r\n")
        head (if idx (subs raw 0 idx) raw)
        body (if idx (subs raw (+ idx 4)) "")
        lines (str/split head "\r\n")
        status (parse-long (second (str/split (first lines) " ")))]
    {:status status :headers (parse-headers (rest lines)) :body body}))

(defn request [{:keys [method url headers body]}]
  (csp/go
    (let [[host port path] (parse-url url)
          fd  (tcp/connect host port)
          req (str (str/upper-case (name (or method :get))) " " path " HTTP/1.1\r\n"
                   "Host: " host "\r\n"
                   "Connection: close\r\n"
                   (when body (str "Content-Length: " (count body) "\r\n"))
                   (str/join "" (for [[k v] headers] (str k ": " v "\r\n")))
                   "\r\n" (or body ""))]
      (csp/send! fd req)
      (let [resp (loop [buf ""]
                   (let [m (csp/recv! fd)]
                     (if (nil? m) buf (recur (str buf m)))))]
        (tcp/close fd)
        (parse-response resp)))))

(defn get  [url]      (request {:method :get  :url url}))
(defn post [url body] (request {:method :post :url url :body body}))
