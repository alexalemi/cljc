(ns clojure.core.async
  "A single-threaded, coroutine-backed implementation of core.async for cljc.
   A `go` block is a stackful coroutine that YIELDS when it parks on a channel;
   the scheduler resumes it when the rendezvous completes. No real threads —
   blocking ops (<!! / >!! / alts!!) drive the event loop until ready."
  (:refer-clojure :exclude [reduce transduce into merge map take partition partition-by]))

;; ── scheduler ───────────────────────────────────────────────────────────
;; Backed by the runtime's shared fiber layer (fiber/ready, fiber/timers,
;; fiber/io-waiters — the same queue that runs clojure.core futures/promises,
;; Thread/sleep and csp.clj). Sharing one scheduler means a blocking <!! also
;; drives pending futures, a main-thread (deref fut) also resumes parked go
;; blocks, and Thread/sleep or @promise INSIDE a go block parks the fiber
;; instead of stalling the whole loop. The go-coroutine currently running is
;; fiber/*self* (bound by fiber/schedule! around every resume).
(def ^:private pumping (atom false))

(defn- dispatch! [thunk] (fiber/dispatch! thunk))

(defn- run-loop! [done? block?]
  (loop []
    (when-not (and done? (done?))
      (cond
        (fiber/run-one!)     (recur)
        (fiber/fire-timers!) (recur)
        (and block? (or (fiber/io-poll!) (fiber/idle!))) (recur)
        :else nil))))

(defn- pump! []
  (when-not @pumping
    (reset! pumping true)
    (run-loop! nil false)
    (reset! pumping false)))

;; ── buffers ─────────────────────────────────────────────────────────────
(defn buffer [n]          (atom {:items [] :n n :type :fixed}))
(defn dropping-buffer [n] (atom {:items [] :n n :type :dropping}))
(defn sliding-buffer [n]  (atom {:items [] :n n :type :sliding}))
(defn- buf-count [b] (count (:items @b)))
(defn- buf-full? [b] (let [s @b] (and (= :fixed (:type s)) (>= (count (:items s)) (:n s)))))
(defn- buf-add! [b v]
  (swap! b (fn [s]
             (let [items (:items s) n (:n s)]
               (case (:type s)
                 :fixed    (update s :items conj v)
                 :dropping (if (>= (count items) n) s (update s :items conj v))
                 :sliding  (assoc s :items (conj (if (>= (count items) n) (subvec items 1) items) v)))))))
(defn- buf-remove! [b]
  (let [v (first (:items @b))] (swap! b update :items subvec 1) v))

;; ── channels ────────────────────────────────────────────────────────────
;; an atom holding {:buf buf-or-nil :takes [cb...] :puts [[val cb]...] :closed bool}
(defn chan
  ([] (atom {:buf nil :takes [] :puts [] :closed false}))
  ([n] (atom {:buf (if (number? n) (when (pos? n) (buffer n)) n)
              :takes [] :puts [] :closed false}))
  ([n _xform] (chan n)))

(defn chan? [c] (and (instance? clojure.lang.Atom c) (contains? (deref c) :takes)))

;; do-take: returns [v] if it completes now (cb NOT called), else nil (cb parked)
(defn- do-take [ch cb]
  (let [s @ch buf (:buf s)]
    (cond
      (and buf (pos? (buf-count buf)))
      (let [v (buf-remove! buf)]
        (when (seq (:puts s))                       ; let a parked putter fill the slot
          (let [pv (nth (:puts s) 0)]
            (swap! ch update :puts subvec 1)
            (buf-add! buf (nth pv 0))
            (dispatch! (fn [] ((nth pv 1) true)))))
        [v])
      (seq (:puts s))                               ; unbuffered handoff
      (let [pv (nth (:puts s) 0)]
        (swap! ch update :puts subvec 1)
        (dispatch! (fn [] ((nth pv 1) true)))
        [(nth pv 0)])
      (:closed s) [nil]
      :else (do (swap! ch update :takes conj cb) nil))))

;; do-put: returns [true]/[false] if it completes now, else nil (cb parked)
(defn- do-put [ch v cb]
  (when (nil? v) (throw (ex-info "Can't put nil on a channel" {})))
  (let [s @ch buf (:buf s)]
    (cond
      (:closed s) [false]
      (seq (:takes s))                              ; hand directly to a parked taker
      (let [tcb (nth (:takes s) 0)]
        (swap! ch update :takes subvec 1)
        (dispatch! (fn [] (tcb v)))
        [true])
      (and buf (not (buf-full? buf)))
      (do (buf-add! buf v) [true])
      :else (do (swap! ch update :puts conj [v cb]) nil))))

(defn close! [ch]
  (let [s @ch]
    (when-not (:closed s)
      (swap! ch assoc :closed true :takes [] :puts [])
      (doseq [tcb (:takes s)] (dispatch! (fn [] (tcb nil))))   ; takers drain to nil
      (doseq [pv (:puts s)] (dispatch! (fn [] ((nth pv 1) true))))))
  (pump!) nil)

(defn closed? [ch] (:closed (deref ch)))

;; ── go / parking ops ─────────────────────────────────────────────────────
(defn <! [ch]
  (let [co fiber/*self*
        box (do-take ch (fn [v] (fiber/schedule! co v)))]
    (if box (first box) (coro/yield))))

(defn >! [ch v]
  (let [co fiber/*self*
        box (do-put ch v (fn [ok] (fiber/schedule! co ok)))]
    (if box (first box) (coro/yield))))

(defn go-call [thunk]
  (let [c  (chan 1)
        co (coro/new (fn []
                       (let [r (thunk)]
                         (when (some? r) (do-put c r (fn [_])))
                         (close! c))))]
    (fiber/schedule! co nil)
    (pump!)
    c))

(defmacro go [& body] `(go-call (fn [] ~@body)))
(defmacro go-loop [binds & body] `(go (loop ~binds ~@body)))

;; ── blocking ops (drive the loop) ─────────────────────────────────────────
(defn <!! [ch]
  (let [res (atom ::none)
        box (do-take ch (fn [v] (reset! res v)))]
    (if box (first box)
        (do (run-loop! (fn [] (not= ::none @res)) true) @res))))

(defn >!! [ch v]
  (let [res (atom ::none)
        box (do-put ch v (fn [ok] (reset! res ok)))]
    (if box (first box)
        (do (run-loop! (fn [] (not= ::none @res)) true) @res))))

(defn put!
  ([ch v] (do-put ch v (fn [_])) (pump!) true)
  ([ch v cb] (let [box (do-put ch v (fn [ok] (dispatch! (fn [] (cb ok)))))]
               (when box (dispatch! (fn [] (cb (first box))))))
   (pump!) true))
(defn take!
  ([ch cb] (let [box (do-take ch (fn [v] (dispatch! (fn [] (cb v)))))]
             (when box (dispatch! (fn [] (cb (first box))))))
   (pump!) nil))

(defn offer! [ch v]
  (let [s @ch]
    (if (or (seq (:takes s)) (and (:buf s) (not (buf-full? (:buf s)))))
      (do (do-put ch v (fn [_])) (pump!) true)
      false)))
(defn poll! [ch]
  (let [s @ch]
    (if (or (seq (:puts s)) (and (:buf s) (pos? (buf-count (:buf s)))))
      (let [box (do-take ch (fn [_]))] (pump!) (when box (first box)))
      nil)))

;; ── timeout ────────────────────────────────────────────────────────────
(defn timeout [ms]
  (let [c (chan)]
    (fiber/timer! (+ (cljc/now-ms*) ms) (fn [] (close! c)))
    c))

;; ── alts! : first ready op wins (a shared one-shot flag) ──────────────────
(defn- alt-op [ports flag co]
  ;; try each port; ports are channel (take) or [channel val] (put)
  (loop [ps ports]
    (if (empty? ps)
      :parked
      (let [p (first ps)
            [ch v put?] (if (vector? p) [(nth p 0) (nth p 1) true] [p nil false])
            cb (fn [r] (when (compare-and-set! flag false true)
                         (fiber/schedule! co [r ch])))
            box (if put? (do-put ch v cb) (do-take ch cb))]
        (if box
          (when (compare-and-set! flag false true) [(first box) ch])
          (recur (rest ps)))))))

(defn alts! [ports & {:as opts}]
  (let [co   fiber/*self*
        flag (atom false)
        r    (alt-op ports flag co)]
    (cond
      (vector? r) r                          ; an op completed synchronously
      (and (contains? opts :default) (compare-and-set! flag false true))
      [(:default opts) :default]
      :else (coro/yield))))                  ; parked on all; resumed by the winner

(defn alts!! [ports & opts]
  (let [res (atom ::none)
        flag (atom false)
        try1 (loop [ps ports]
               (if (empty? ps) :parked
                   (let [p (first ps)
                         [ch v put?] (if (vector? p) [(nth p 0) (nth p 1) true] [p nil false])
                         cb (fn [r] (when (compare-and-set! flag false true) (reset! res [r ch])))
                         box (if put? (do-put ch v cb) (do-take ch cb))]
                     (if box (when (compare-and-set! flag false true) [(first box) ch])
                         (recur (rest ps))))))]
    (if (vector? try1) try1
        (do (run-loop! (fn [] (not= ::none @res)) true) @res))))

;; ── misc helpers commonly used ────────────────────────────────────────────
;; thread-call: a "thread" is a fiber here — f runs concurrently with the
;; caller (it may park), its result lands on the returned channel.
(defn thread-call [f]
  (let [c (chan 1)]
    (fiber/spawn! (fn []
                    (let [r (f)]
                      (when (some? r) (do-put c r (fn [_])))
                      (close! c))))
    c))
(defmacro thread [& body] `(thread-call (fn [] ~@body)))

(defn pipe
  ([from to] (pipe from to true))
  ([from to close?]
   (go-loop []
     (let [v (<! from)]
       (if (nil? v) (when close? (close! to))
           (do (>! to v) (recur)))))
   to))

(defn onto-chan!
  ([ch coll] (onto-chan! ch coll true))
  ([ch coll close?]
   (go-loop [xs (seq coll)]
     (if xs (do (>! ch (first xs)) (recur (next xs)))
         (when close? (close! ch))))
   ch))

(defn to-chan! [coll]
  (let [c (chan (max 1 (count coll)))]
    (doseq [x coll] (do-put c x (fn [_])))
    (close! c) c))

;; ── combinators ───────────────────────────────────────────────────────────
(defn reduce
  "Async reduce: returns a channel with (f (f (f init v1) v2) ...) when ch closes."
  [f init ch]
  (go-loop [acc init]
    (let [v (<! ch)]
      (if (nil? v) acc (recur (f acc v))))))

(defn into
  "Drain ch into coll; result delivered on the returned channel."
  [coll ch]
  (reduce conj coll ch))

(defn merge
  "Fan-in: a channel fed by every value from each of chs; closes when all do."
  ([chs] (merge chs nil))
  ([chs buf-or-n]
   (let [out (chan (or buf-or-n 0))
         live (atom (count chs))]
     (doseq [c chs]
       (go-loop []
         (let [v (<! c)]
           (if (nil? v)
             (when (zero? (swap! live dec)) (close! out))
             (do (>! out v) (recur))))))
     out)))

(defn mult
  "A mult(iple) of the source channel: tapped channels each receive every value."
  [ch]
  (let [taps (atom #{})]
    (go-loop []
      (let [v (<! ch)]
        (if (nil? v)
          (doseq [t @taps] (close! t))
          (do (doseq [t @taps] (>! t v)) (recur)))))
    {::taps taps}))
(defn tap [m ch] (swap! (::taps m) conj ch) ch)
(defn untap [m ch] (swap! (::taps m) disj ch) ch)
(defn untap-all [m] (reset! (::taps m) #{}) m)

(defn map
  "Map f over values taken from chs (one each), putting results on a new channel."
  [f chs]
  (let [out (chan)]
    (go-loop []
      (let [vs (loop [cs (seq chs) acc []]             ; take one from each, eagerly
                 (if (nil? cs) acc
                     (let [v (<! (first cs))]
                       (if (nil? v) ::closed (recur (next cs) (conj acc v))))))]
        (if (= vs ::closed) (close! out)
            (do (>! out (apply f vs)) (recur)))))
    out))
