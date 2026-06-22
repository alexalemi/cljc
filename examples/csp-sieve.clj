;; csp-sieve.clj — the concurrent prime sieve, the classic CSP demo (Newsqueak
;; / Go's "prime sieve"), running on cljc's coroutine-backed core.async.
;;
;; Each prime spawns its own goroutine that filters out its multiples. Numbers
;; flow through a growing chain of filter coroutines; whatever pops out the far
;; end is prime. There is no shared mutable state — just channels and `go`.
;;
;; Showcases: the C coroutine primitive (coro/new/yield) driving csp.clj's
;; channels, `go`, `<!`/`>!`, and `<!!`. Hundreds of coroutines, cooperatively
;; scheduled on one thread.
;;
;; Run:  cljc examples/csp-sieve.clj          (from the repo root)
;;       cljc examples/csp-sieve.clj 40

;; csp.clj lives in the repo root; make it findable from examples/ too.
(def *load-path* (into [".." "."] *load-path*))
(require '[csp :as a])

;; Emit 2, 3, 4, 5, ... onto ch — an infinite producer. It blocks on >! until
;; something downstream pulls, so it's demand-driven (no runaway).
(defn counter [ch]
  (a/go-loop [i 2]
    (a/>! ch i)
    (recur (inc i))))

;; Read from `in`, forward everything NOT divisible by `prime` to `out`.
(defn filter-multiples [in out prime]
  (a/go-loop []
    (let [n (a/<! in)]
      (when (pos? (mod n prime))
        (a/>! out n))
      (recur))))

;; Pull `n` primes through a chain that grows by one filter goroutine per prime.
(defn primes [n]
  (let [source (a/chan)]
    (counter source)
    (a/<!! (a/go-loop [ch source, found [], k 0]
             (if (= k n)
               found
               (let [p   (a/<! ch)          ; the head of the chain is always prime
                     ch' (a/chan)]
                 (filter-multiples ch ch' p)
                 (recur ch' (conj found p) (inc k))))))))

(let [n (if (seq *args*) (parse-long (first *args*)) 25)]
  (println (format "First %d primes, each sieved by its own coroutine:" n))
  (println (primes n))
  (println)
  (println "That chain ran" n "filter goroutines plus a producer — all"
           "cooperatively scheduled on a single OS thread, with no locks."))
