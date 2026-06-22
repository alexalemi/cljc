;; primes.clj — a tour of cljc's lazy, possibly-infinite sequences.
;;
;; Showcases: lazy-seq, infinite (range)/(iterate), take/take-while, and the
;; fact that you can define an unbounded stream and only pay for what you pull.

;; An infinite stream of primes by trial division. `sieve` is genuinely lazy:
;; nothing is computed until something downstream asks for an element, and the
;; recursion `(sieve (filter ...))` would loop forever if it weren't deferred
;; behind `lazy-seq`/`filter`.
(defn sieve [s]
  (let [p (first s)]
    (cons p (lazy-seq (sieve (filter #(not (zero? (mod % p))) (rest s)))))))

(def primes (sieve (iterate inc 2)))    ; 2 3 5 7 11 ... forever

(println "First 20 primes:")
(println (take 20 primes))

(println)
(println "Primes below 100:")
(println (take-while #(< % 100) primes))

(println)
(println "The 100th prime is" (nth primes 99))

;; The Collatz ("3n+1") trajectory — another infinite-process-made-finite via
;; take-while. Each number's orbit length, then the longest under 30.
(defn collatz [n]
  (iterate (fn [x] (if (even? x) (quot x 2) (inc (* 3 x)))) n))

(defn orbit-length [n]
  (count (take-while #(not= % 1) (collatz n))))

(println)
(println "Collatz orbit of 27 has" (orbit-length 27) "steps before reaching 1.")

(let [[best len] (apply max-key second
                        (for [n (range 1 30)] [n (orbit-length n)]))]
  (println (format "Longest orbit under 30: n=%d takes %d steps." best len)))

;; Fibonacci as a self-referential lazy stream — the textbook one-liner that
;; only works because `map` and `lazy-seq` never force more than asked.
(def fibs (lazy-cat [0 1] (map + fibs (rest fibs))))
(println)
(println "First 15 Fibonacci numbers:" (take 15 fibs))
