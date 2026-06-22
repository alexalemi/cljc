;; brainfuck.clj — a complete Brainfuck interpreter in cljc.
;;
;; Showcases: hosting another language, mutable arrays (int-array/aget/aset —
;; cljc's transient-vector-backed arrays), precomputing jump tables, and
;; with-out-str-free streaming output via `print`.
;;
;; Brainfuck has 8 commands operating on a tape of byte cells:
;;   > <  move the data pointer       + -  inc/dec the current cell
;;   . ,  output / input a byte        [ ]  loop while current cell != 0
;;
;; Run:  cljc examples/brainfuck.clj            ; runs the built-in "Hello"
;;       cljc examples/brainfuck.clj prog.bf    ; run a program from a file

;; Precompute matching-bracket targets so `[` / `]` are O(1) jumps instead of
;; rescanning. A stack of `[` positions pairs each with its closing `]`.
(defn jump-table [code]
  (loop [i 0, stack '(), table {}]
    (if (= i (count code))
      table
      (let [c (subs code i (inc i))]
        (cond
          (= c "[") (recur (inc i) (cons i stack) table)
          (= c "]") (let [open (first stack)]
                      (recur (inc i) (rest stack)
                             (assoc table open i, i open)))
          :else     (recur (inc i) stack table))))))

(defn run [code]
  (let [code  (apply str (re-seq #"[><+\-.,\[\]]" code))   ; strip comments
        jumps (jump-table code)
        tape  (int-array 30000)]
    (loop [ip 0, dp 0]
      (when (< ip (count code))
        (let [c (subs code ip (inc ip))]
          (cond
            (= c ">") (recur (inc ip) (inc dp))
            (= c "<") (recur (inc ip) (dec dp))
            (= c "+") (do (aset tape dp (mod (inc (aget tape dp)) 256))
                          (recur (inc ip) dp))
            (= c "-") (do (aset tape dp (mod (+ 255 (aget tape dp)) 256))
                          (recur (inc ip) dp))
            (= c ".") (do (print (char (aget tape dp)))
                          (recur (inc ip) dp))
            ;; jump past the matching ] when the cell is zero, else step in
            (= c "[") (recur (if (zero? (aget tape dp))
                               (inc (get jumps ip)) (inc ip)) dp)
            ;; jump back to the matching [ when the cell is non-zero
            (= c "]") (recur (if (zero? (aget tape dp))
                               (inc ip) (inc (get jumps ip))) dp)
            :else     (recur (inc ip) dp)))))))

;; "Hello World!" — the canonical Brainfuck program.
(def hello
  "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++.")

(let [code (if (seq *args*) (slurp (first *args*)) hello)]
  (run code)
  (println))
