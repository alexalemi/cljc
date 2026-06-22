;; hello-bundle.clj — a tiny standalone demo to send to friends.
;; Build:  cljc bundle.clj examples/hello-bundle.clj hello
;; Run:    ./hello            (or)   ./hello Ada Grace

(def who
  (let [names (seq *args*)]
    (if names (str/join ", " names) "stranger")))

;; A little proof-of-life computation: primes under 50 via the language's
;; own lazy seqs + higher-order fns — if this prints, the runtime works.
(defn prime? [n]
  (and (> n 1)
       (not-any? #(zero? (mod n %)) (range 2 n))))

(def primes (filter prime? (range 2 50)))

(println "  ___  ___  ___")
(println " / __|| |  |_ _|  cljc standalone binary")
(println "| (__ | |__ | |   no runtime, no deps — just this file")
(println " \\___||____|___|")
(println)
(println (str "Hello, " who "! This binary ran natively on your machine."))
(println "Primes under 50:" (str/join " " primes))
(println "2^20 =" (reduce * (repeat 20 2)))
(println "Try again with your name:  ./hello Your Name Here")
