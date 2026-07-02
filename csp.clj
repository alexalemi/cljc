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
;; The scheduler itself lives in the runtime (the fiber/* layer in the
;; bootstrap): one shared ready queue + timer wheel + fd-park table that this
;; library, clojure.core futures/promises, and Thread/sleep all pump. That
;; sharing is the point — a main-thread (deref fut) resumes parked go blocks,
;; and <!! drives pending futures. fiber/*self* is the coroutine running the
;; current go block; <!/>! read it to register themselves as the parked party.
(def schedule! fiber/schedule!)

;; ── channels ───────────────────────────────────────────────────────────────
;; A taker is a HANDLER {:co coro :done (atom false) :sel (atom nil)} so a
;; single alts! can register on many channels and commit to exactly one (the
;; :done flag); :sel records which channel fired. <! uses a plain handler.
;; chan: optional buffer size and an optional TRANSDUCER applied to values as
;; they are put. The xform reduces into the buffer, so one put can yield many
;; values (mapcat), none (filter), or signal close (a reduced from take). An
;; xform channel needs a buffer ≥ 1 for the transformed outputs to land in.
(defn chan
  ([]      (chan 0 nil))
  ([n]     (chan n nil))
  ([n xform]
   (atom {:buf [] :n (if xform (max 1 n) n) :takers [] :putters [] :closed false
          :add (when xform
                 (xform (fn ([] []) ([b] b) ([b v] (conj b v)))))})))

(defn- handler [] {:co fiber/*self* :done (atom false) :sel (atom nil)})

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

;; Wake one parked putter when a buffer slot frees. For a transducer channel the
;; putter's value already went through the xform into the buffer, so we only
;; release its backpressure; for a plain channel we move its value into the buf.
(defn- release-putter! [ch]
  (when (and (seq (:putters @ch)) (< (count (:buf @ch)) (:n @ch)))
    (let [[pc pv] (first (:putters @ch))]
      (swap! ch update :putters subvec 1)
      (when-not (= pv ::xform) (swap! ch update :buf conj pv))
      (schedule! pc true))))

;; A taker's immediate path: a buffered value, or a parked putter handed off.
;; Returns [hit? value]; nil-on-closed counts as a hit.
(defn- take-ready! [ch]
  (let [c @ch]
    (cond
      (seq (:buf c))     (let [v (first (:buf c))]
                           (swap! ch update :buf subvec 1)
                           (release-putter! ch)
                           [true v])
      (and (not (:add c)) (seq (:putters c)))                   ; unbuffered handoff (plain only)
                         (let [[pc pv] (first (:putters c))]
                           (swap! ch update :putters subvec 1)
                           (schedule! pc true)
                           [true pv])
      (:closed c)        [true nil]
      :else              [false nil])))

;; Push buffered values to any waiting takers (transducer channels deliver via
;; the buffer, never by direct handoff).
(defn- flush-buf! [ch]
  (loop []
    (when (and (seq (:buf @ch)) (seq (:takers @ch)))
      (when-let [h (next-taker! ch)]
        (let [v (first (:buf @ch))]
          (swap! ch update :buf subvec 1)
          (release-putter! ch)
          (deliver-take! h v ch)
          (recur))))))

;; A put into a transducer channel: run v through the xform into the buffer,
;; flush to takers, then apply backpressure (park if the buffer is over cap).
(defn- xform-put! [ch v]
  (let [result ((:add @ch) (:buf @ch) v)
        closing? (reduced? result)]
    (swap! ch assoc :buf (unreduced result))
    (flush-buf! ch)
    (cond
      closing?                            (do (close! ch) true)
      (> (count (:buf @ch)) (:n @ch))     (do (swap! ch update :putters conj [fiber/*self* ::xform])
                                              (coro/yield nil) true)
      :else                               true)))

(defn <! [ch]                           ; take (parks inside a go)
  (let [[hit v] (take-ready! ch)]
    (if hit v
        (do (swap! ch update :takers conj (handler))
            (coro/yield nil)))))        ; resumed with the delivered value

(defn >! [ch v]                         ; put (parks inside a go)
  (let [c @ch]
    (cond
      (:closed c)  false                               ; put on closed → false (core.async)
      (:add c)     (xform-put! ch v)                   ; transducer channel
      :else
      (if-let [h (next-taker! ch)]
        (do (deliver-take! h v ch) true)               ; direct handoff
        (if (< (count (:buf c)) (:n c))
          (do (swap! ch update :buf conj v) true)       ; buffer
          (do (swap! ch update :putters conj [fiber/*self* v]); park
              (coro/yield nil) true))))))

(defn close! [ch]
  (when (and (:add @ch) (not (:closed @ch)))   ; flush the transducer's pending state
    (let [final ((:add @ch) (:buf @ch))]       ; 1-arity completion (e.g. partition-all)
      (swap! ch assoc :buf (unreduced final))
      (flush-buf! ch)))
  (swap! ch assoc :closed true)
  (let [ts (:takers @ch)]
    (swap! ch assoc :takers [])
    (doseq [h ts] (deliver-take! h nil ch)))  ; takers on a closed channel get nil
  (let [ps (:putters @ch)]
    (swap! ch assoc :putters [])
    (doseq [[pc _] ps] (schedule! pc false))) ; parked putters wake (don't hang)
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
;; A timeout is a channel closed by the shared timer wheel at its deadline.
(defn timeout [ms]
  (let [ch (chan)]
    (fiber/timer! (+ (cljc/now-ms*) ms) (fn [] (close! ch)))
    ch))

;; ── the async I/O event loop ────────────────────────────────────────────────
;; A goroutine parks on an fd via fiber/park-io!; the scheduler's idle phase
;; poll()s all parked fds (bounded by the nearest timer) and resumes whoever is
;; ready. This is what turns the cooperative scheduler into a real event loop —
;; the thing that makes non-blocking servers possible.
(def POLLIN  1)   ; wait for readable (or peer-closed)
(def POLLOUT 2)   ; wait for writable

(defn- park-io [fd ev] (fiber/park-io! fd ev))

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
  (loop [] (when (fiber/pump!) (recur))))

;; <!! — BLOCKING take from outside a go (pumps the scheduler). cljc's bonus
;; over JS core.async. Throws on deadlock.
(defn <!! [ch]
  (loop []
    (let [[hit v] (take-ready! ch)]
      (cond hit v
            (fiber/pump!) (recur)
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

;; ── pub / sub — topic-based broadcast ───────────────────────────────────────
;; A publication routes each value of `ch` by (topic-fn v) to a per-topic mult;
;; subscribers tap the topic they care about. Topics are created lazily on sub.
(defn pub [ch topic-fn]
  (let [mults (atom {})                  ; topic -> {:src chan :mult mult}
        p {:mults mults :topic-fn topic-fn}]
    (go-loop []
      (let [v (<! ch)]
        (if (nil? v)
          (doseq [e (vals @mults)] (close! (:src e)))   ; source closed → close topics
          (do (when-let [e (get @mults (topic-fn v))] (>! (:src e) v))
              (recur)))))
    p))

(defn- pub-topic! [p t]                  ; get-or-create the mult for a topic
  (or (get @(:mults p) t)
      (let [src (chan) e {:src src :mult (mult src)}]
        (swap! (:mults p) assoc t e)
        e)))

(defn sub
  ([p t ch] (sub p t ch true))
  ([p t ch close?] (tap (:mult (pub-topic! p t)) ch close?) ch))
(defn unsub [p t ch]
  (when-let [e (get @(:mults p) t)] (untap (:mult e) ch)) ch)

;; ── pipeline-async — N concurrent async workers ─────────────────────────────
;; Each worker pulls from `from`, calls (af v result-ch) — which does async work
;; (it may park) and puts result(s) onto result-ch, closing it when done — and
;; forwards results to `to`. With N>1 there can be N operations in flight at
;; once (e.g. N concurrent fetches), which is exactly what a single-threaded
;; event loop CAN parallelize: the waiting overlaps. Output order is not
;; preserved. `to` closes when `from` is exhausted and all workers finish.
(defn pipeline-async [n to af from]
  (let [left (atom n)]
    (dotimes [_ n]
      (go-loop []
        (let [v (<! from)]
          (if (nil? v)
            (when (zero? (swap! left dec)) (close! to))
            (let [rc (chan 1)]
              (af v rc)
              (loop [] (let [r (<! rc)] (when-not (nil? r) (>! to r) (recur))))
              (recur))))))
    to))
