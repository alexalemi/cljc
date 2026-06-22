;; wordfreq.clj — a word-frequency CLI, the "hello world" of text processing.
;;
;; Showcases: regex tokenizing, frequencies, sort-by, *args* for CLI input,
;; and reading either a file or stdin (cljc has no *in*, so we drain stdin
;; with a read-line loop — a useful idiom for pipe-friendly scripts).
;;
;; Run:  cljc examples/wordfreq.clj README.md
;;       cat README.md | cljc examples/wordfreq.clj
;;       cljc examples/wordfreq.clj README.md 15     ; top 15 instead of 10

(defn read-stdin []
  (loop [lines []]
    (let [line (read-line)]
      (if (nil? line)
        (str/join "\n" lines)
        (recur (conj lines line))))))

(defn top-words [text n]
  (->> (str/lower-case text)
       (re-seq #"[a-z']+")
       (remove #(< (count %) 3))            ; skip very short words
       frequencies
       (sort-by (fn [[_ c]] (- c)))         ; descending by count
       (take n)))

;; Parse args: an optional file path and an optional integer count, in any
;; order. parse-long returns nil on non-numbers, which we use to classify.
(let [nums   (keep parse-long *args*)
      files  (remove parse-long *args*)
      n      (if (seq nums) (first nums) 10)
      text   (if (seq files) (slurp (first files)) (read-stdin))
      total  (count (re-seq #"[a-z']+" (str/lower-case text)))]
  (println (format "%d words total. Top %d (length ≥ 3):\n" total n))
  (doseq [[word freq] (top-words text n)]
    (let [bar (apply str (repeat (min 40 freq) "▇"))]
      (println (format "  %-14s %4d  %s" word freq bar)))))
