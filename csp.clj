;; csp.clj — a core.async-flavored CSP library for cljc, built on the C
;; coroutine primitive (coro/new, coro/resume, coro/yield).
;;
;; This is the ClojureScript model of core.async, not the JVM one: single-
;; threaded and cooperative. A `go` block is a coroutine; `<!`/`>!` suspend it
;; (coro/yield) back to a scheduler when they'd block, and the scheduler
;; resumes it when the matching op arrives. No locks, no threads — and because
;; cljc owns the main loop we can also offer `<!!` (blocking take), which the
;; JS flavor cannot.
;;
;; Load with: (require '[csp :as a])  or  (load-file "csp.clj")

;; ── scheduler ──────────────────────────────────────────────────────────────
;; *self* is the coroutine running the current go block; <!/>! read it to
;; register themselves as the parked party. The scheduler binds it per resume.
(def ^:dynamic *self* nil)
(def ready  (atom []))     ; queue of [coro resume-value] ready to run
(def timers (atom []))     ; vector of [deadline-ms chan]

(defn schedule! [coro val] (swap! ready conj [coro val]))

(defn- run-one! []         ; run one ready coro to its next park; true if any
  (let [item (first @ready)]
    (when item
      (swap! ready subvec 1)
      (let [[coro val] item]
        (when (coro/alive? coro)
          (binding [*self* coro] (coro/resume coro val))))
      true)))

;; ── channels ───────────────────────────────────────────────────────────────
;; A taker is a HANDLER {:co coro :done (atom false) :sel (atom nil)} so a
;; single alts! can register on many channels and commit to exactly one (the
;; :done flag); :sel records which channel fired. <! uses a plain handler.
(defn chan
  ([]  (chan 0))
  ([n] (atom {:buf [] :n n :takers [] :putters [] :closed false})))

(defn- handler [] {:co *self* :done (atom false) :sel (atom nil)})

(defn- deliver-take! [h v ch]           ; wake a taker handler once
  (when-not @(:done h)
    (reset! (:done h) true)
    (reset! (:sel h) ch)
    (schedule! (:co h) v)
    true))

(defn- next-taker! [ch]                  ; pop the next uncommitted taker, or nil
  (loop []
    (let [ts (:takers @ch)]
      (when (seq ts)
        (let [h (first ts)]
          (swap! ch update :takers subvec 1)
          (if @(:done h) (recur) h))))))

;; A taker's immediate path: a buffered value, or a parked putter handed off.
;; Returns [hit? value]; nil-on-closed counts as a hit.
(defn- take-ready! [ch]
  (let [c @ch]
    (cond
      (seq (:buf c))     (let [v (first (:buf c))]
                           (swap! ch update :buf subvec 1)
                           (when (seq (:putters @ch))            ; refill from a parked putter
                             (let [[pc pv] (first (:putters @ch))]
                               (swap! ch update :putters subvec 1)
                               (swap! ch update :buf conj pv)
                               (schedule! pc true)))
                           [true v])
      (seq (:putters c)) (let [[pc pv] (first (:putters c))]     ; unbuffered handoff
                           (swap! ch update :putters subvec 1)
                           (schedule! pc true)
                           [true pv])
      (:closed c)        [true nil]
      :else              [false nil])))

(defn <! [ch]                           ; take (parks inside a go)
  (let [[hit v] (take-ready! ch)]
    (if hit v
        (do (swap! ch update :takers conj (handler))
            (coro/yield nil)))))        ; resumed with the delivered value

(defn >! [ch v]                         ; put (parks inside a go)
  (let [c @ch]
    (cond
      (:closed c)  (throw (ex-info "put on closed channel" {}))
      :else
      (if-let [h (next-taker! ch)]
        (do (deliver-take! h v ch) true)               ; direct handoff
        (if (< (count (:buf c)) (:n c))
          (do (swap! ch update :buf conj v) true)       ; buffer
          (do (swap! ch update :putters conj [*self* v]); park
              (coro/yield nil) true))))))

(defn close! [ch]
  (swap! ch assoc :closed true)
  (let [ts (:takers @ch)]
    (swap! ch assoc :takers [])
    (doseq [h ts] (deliver-take! h nil ch)))  ; takers on a closed channel get nil
  nil)

;; ── go ───────────────────────────────────────────────────────────────────
(defn go* [thunk]
  (let [done (chan 1)
        coro (coro/new (fn [] (>! done (thunk)) (close! done)))]
    (schedule! coro nil)
    done))
(defmacro go [& body] `(go* (fn [] ~@body)))
(defmacro go-loop [binds & body] `(go (loop ~binds ~@body)))

;; ── timeouts ──────────────────────────────────────────────────────────────
(defn timeout [ms]
  (let [ch (chan)]
    (swap! timers conj [(+ (cljc/now-ms*) ms) ch])
    ch))

(defn- fire-timers! []
  (let [now (cljc/now-ms*)
        due (filterv (fn [[t _]] (<= t now)) @timers)]
    (when (seq due)
      (swap! timers (fn [ts] (vec (remove (fn [[t _]] (<= t now)) ts))))
      (doseq [[_ ch] due] (close! ch))
      true)))

(defn- idle-till-timer! []              ; sleep until the nearest deadline
  (when (seq @timers)
    (let [next-t (reduce min (map first @timers))]
      (cljc/sleep-ms* (max 0 (- next-t (cljc/now-ms*))))
      true)))

;; ── the async I/O event loop ────────────────────────────────────────────────
;; A goroutine parks on an fd via park-io; the scheduler's idle phase poll()s
;; all parked fds (bounded by the nearest timer) and resumes whoever is ready.
;; This is what turns the cooperative scheduler into a real event loop — the
;; thing that makes non-blocking servers possible.
(def POLLIN  1)   ; wait for readable (or peer-closed)
(def POLLOUT 2)   ; wait for writable
(def io-waiters (atom []))   ; vector of {:fd :ev :co}

(defn- park-io [fd ev]
  (swap! io-waiters conj {:fd fd :ev ev :co *self*})
  (coro/yield nil))

;; poll() the parked fds, bounded by the nearest timer (or block forever if
;; only I/O is pending). Resume every goroutine whose fd is ready. Returns true
;; if there was anything to wait on, so the driver loop keeps going.
(defn- io-poll! []
  (let [ws @io-waiters]
    (when (seq ws)
      (let [ts (seq @timers)
            timeout (if ts (long (max 0 (- (reduce min (map first @timers)) (cljc/now-ms*)))) -1)
            revents (cljc/poll-fds* (mapv :fd ws) (mapv :ev ws) timeout)
            kept (atom [])]
        (dotimes [i (count ws)]
          (let [w (nth ws i)]
            (if (pos? (bit-and (nth revents i) (:ev w)))
              (schedule! (:co w) nil)
              (swap! kept conj w))))
        (reset! io-waiters @kept)
        true))))

;; Async socket ops — park until the fd is ready, then do the (now non-blocking)
;; operation. Use these inside go blocks instead of the raw tcp/* primitives so
;; one slow connection never stalls the others.
(defn accept! [srv] (park-io srv POLLIN)  (tcp/accept srv 0))   ; → client fd or nil
(defn recv!   [fd]  (park-io fd POLLIN)   (tcp/recv fd))        ; → string or nil (EOF)

;; send! — backpressure-correct: one non-blocking send at a time, parking on
;; POLLOUT when the socket buffer is full, so a slow client can't stall the
;; loop. Returns true if fully sent, false on a dead peer.
(defn send! [fd s]
  (let [len (count s)]
    (loop [sent 0]
      (if (>= sent len)
        true
        (let [n (tcp/send-some* fd s sent)]
          (cond
            (= n -1) (do (park-io fd POLLOUT) (recur sent))   ; would block → wait writable
            (= n -2) false                                     ; dead peer
            :else    (recur (+ sent n))))))))

;; ── drivers ─────────────────────────────────────────────────────────────────
(defn run! []                           ; pump until everything is idle
  (loop [] (cond (run-one!) (recur) (fire-timers!) (recur)
                 (io-poll!) (recur) (idle-till-timer!) (recur) :else nil)))

;; <!! — BLOCKING take from outside a go (pumps the scheduler). cljc's bonus
;; over JS core.async. Throws on deadlock.
(defn <!! [ch]
  (loop []
    (let [[hit v] (take-ready! ch)]
      (cond hit v
            (run-one!) (recur)
            (fire-timers!) (recur)
            (io-poll!) (recur)
            (idle-till-timer!) (recur)
            :else (throw (ex-info "<!!: deadlock — no runnable work" {}))))))

;; ── alts! (take-only) ──────────────────────────────────────────────────────
;; Try each channel; return [value chan] for the first ready one. If none are
;; ready, park on all with ONE shared handler — the first delivery commits it
;; (others see :done and skip). A pragmatic subset: take-ops only.
(defn alts! [chs]
  (or (some (fn [ch] (let [[h v] (take-ready! ch)] (when h [v ch]))) chs)
      (let [h (handler)]
        (doseq [ch chs] (swap! ch update :takers conj h))
        (let [v (coro/yield nil)]
          [v @(:sel h)]))))

;; ── combinators (core.async-flavored) ──────────────────────────────────────
;; All of these spawn go blocks, so they only make progress while a driver
;; (run! or <!!) is pumping the scheduler.

(defn onto-chan!                        ; put every item of coll onto ch
  ([ch coll] (onto-chan! ch coll true))
  ([ch coll close?]
   (go (doseq [x coll] (>! ch x))
       (when close? (close! ch)))
   ch))

(defn to-chan! [coll]                   ; a channel that delivers coll, then closes
  (let [ch (chan (max 1 (count coll)))]
    (onto-chan! ch coll)
    ch))

(defn pipe                              ; forward from → to; close to when from ends
  ([from to] (pipe from to true))
  ([from to close?]
   (go-loop []
     (let [v (<! from)]
       (if (nil? v)
         (when close? (close! to))
         (do (>! to v) (recur)))))
   to))

(defn into                              ; drain ch into coll; result on a chan
  [coll ch]
  (go-loop [acc coll]
    (let [v (<! ch)]
      (if (nil? v) acc (recur (conj acc v))))))

(defn merge                             ; fan-in: one out-chan fed by all of chs
  ([chs] (merge chs 0))
  ([chs buf]
   (let [out (chan buf)
         left (atom (count chs))]
     (doseq [c chs]
       (go-loop []
         (let [v (<! c)]
           (if (nil? v)
             (when (zero? (swap! left dec)) (close! out))
             (do (>! out v) (recur))))))
     out)))

;; mult — broadcast one source to many taps. Every value read from the source
;; is put onto every tapped channel (skipping any that have been untapped).
(defn mult [source]
  (let [taps (atom #{})
        m {:taps taps}]
    (go-loop []
      (let [v (<! source)]
        (if (nil? v)
          (doseq [t @taps] (close! t))   ; source closed → close all taps
          (do (doseq [t @taps] (>! t v))
              (recur)))))
    m))

(defn tap
  ([m ch] (tap m ch true))
  ([m ch _close?] (swap! (:taps m) conj ch) ch))
(defn untap   [m ch] (swap! (:taps m) disj ch) ch)
(defn untap-all [m]  (reset! (:taps m) #{}) m)

(defn take-n                            ; take up to n items from ch into a vector
  [n ch]
  (go-loop [acc [] k 0]
    (if (= k n)
      acc
      (let [v (<! ch)]
        (if (nil? v) acc (recur (conj acc v) (inc k)))))))
