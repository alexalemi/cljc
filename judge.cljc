(ns judge
  (:refer-clojure :exclude [test])      ; `test` is our macro, not clojure.core/test
  (:require [clojure.string :as str]))
;; judge.cljc — inline snapshot testing, in the spirit of ianthehenry/judge.
;; A .cljc so the WHOLE tool runs under cljc AND babashka/Clojure:
;;   cljc judge file.clj ...      ; the cljc subcommand
;;   bb judge.cljc file.clj ...   ; bb runs it as a script (auto-invokes -main)
;;   bb -m judge file.clj ...     ; or via -m
;; And a solution file that does (require '[judge :refer [test ...]]) gets the
;; no-op macros under both, so it runs unchanged with bb/clj and is testable
;; with cljc judge / bb judge.cljc.
;;
;;   (require '[judge])             ; or just write the forms — see below
;;   (test (+ 1 2))                 ; cljc judge fills in => (test (+ 1 2) 3)
;;   (test (+ 1 2) 3)               ; verified; corrected when wrong
;;   (test-error (/ 1 0))           ; snapshot of the error message
;;   (test-stdout (println "hi"))   ; snapshot of printed output (+ value)
;;   (trust (rand))                 ; filled once, never re-run
;;
;;   cljc judge file.clj [...]      ; check; corrections → file.clj.tested
;;   cljc judge -a file.clj         ; apply corrections to the source
;;   cljc judge -i file.clj         ; review each correction (y/n/q)
;;
;; In a normal run (cljc file.clj) the test macros are no-ops, so files
;; carrying tests execute at full speed with zero side effects. Only the
;; judge runner gives them meaning: it reads the source, tracks each
;; top-level form's text extent, and splices corrected values into the
;; original text — your formatting is never reflowed.
;; Exit codes: 0 all green, 1 corrections/failures, 2 load error.

;; ── no-op macros for normal evaluation ──

(defmacro test [& _] nil)
(defmacro test-error [& _] nil)
(defmacro test-stdout [& _] nil)
(defmacro trust [& _] nil)

;; ── the runner (portable: bare names, str/ required above, the only
;; cljc-specific bit — tty detection — guarded with a reader conditional). ──

;; ── source scanning: top-level forms with [start end] char extents ──

(defn judge-forms
  "Source text → vector of {:start :end :text} for each top-level form
  (comments and whitespace excluded). Char-offset based."
  [src]
  (let [n (count src)]
    (loop [i 0 forms [] start nil depth 0 instr false]
      (if (>= i n)
        forms
        (let [c (subs src i (inc i))]
          (cond
            instr
            (cond (= c "\\") (recur (+ i 2) forms start depth true)
                  (= c "\"") (recur (inc i) forms start depth false)
                  :else      (recur (inc i) forms start depth true))
            (= c ";")                       ; comment to end of line
            (let [j (or (str/index-of src "\n" i) n)]
              (recur j forms start depth false))
            (= c "\"")
            (recur (inc i) forms (or start i) depth true)
            (str/includes? "([{" c)
            (recur (inc i) forms (or start i) (inc depth) false)
            (str/includes? ")]}" c)
            (let [d (dec depth)]
              (if (and (zero? d) start)
                (recur (inc i) (conj forms {:start start :end (inc i)
                                            :text (subs src start (inc i))})
                       nil 0 false)
                (recur (inc i) forms start d false)))
            (str/blank? c)
            (if (and start (zero? depth))   ; bare atom form ended
              (recur (inc i) (conj forms {:start start :end i
                                          :text (subs src start i)})
                     nil 0 false)
             (recur (inc i) forms start depth false))
            :else
            (recur (inc i) forms (or start i) depth false)))))))

(defn judge-elements
  "Extents of the depth-1 elements inside a form's text: [[s e] ...],
  including the head symbol. Offsets are relative to the form text."
  [text]
  (let [n (count text)]
    (loop [i 1 els [] start nil depth 0 instr false]   ; skip opening paren
      (if (>= i (dec n))                               ; stop before closer
        (if start (conj els [start i]) els)
        (let [c (subs text i (inc i))]
          (cond
            instr
            (cond (= c "\\") (recur (+ i 2) els start depth true)
                  (= c "\"") (recur (inc i) (if (and start (zero? depth)) els els)
                                    start depth false)
                  :else (recur (inc i) els start depth true))
            (= c ";")
            (let [j (or (str/index-of text "\n" i) (dec n))]
              (recur j els start depth false))
            (= c "\"")
            (recur (inc i) els (or start i) depth true)
            (str/includes? "([{" c)
            (recur (inc i) els (or start i) (inc depth) false)
            (str/includes? ")]}" c)
            (let [d (dec depth)]
              (if (and (zero? d) start)
                (recur (inc i) (conj els [start (inc i)]) nil 0 false)
                (recur (inc i) els start d false)))
            (str/blank? c)
            (if (and start (zero? depth))
              (recur (inc i) (conj els [start i]) nil 0 false)
              (recur (inc i) els start depth false))
            :else
            (recur (inc i) els (or start i) depth false)))))))

;; ── corrections ──

(defn judge-pr
  "Snapshot rendering: like pr-str, but seq values are quoted so the
  written literal reads back as data, not a call — (sort x) snapshots
  as '(1 2 3)."
  [v]
  (if (or (list? v) (seq? v))
    (str "'" (pr-str v))
    (pr-str v)))

(defn judge-line-of [src pos]
  (inc (count (filter (fn [c] (= c \newline)) (seq (subs src 0 pos))))))

(defn judge-correct
  "New text for a judge form: keep everything up to (excluding) the
  element at drop-from (nil = keep all), then splice the new snapshot
  values before the closing paren. Multi-line forms put the snapshot on
  its own line, indented two from the form's column."
  [text els drop-from snaps indent]
  (let [keep-end (if (and drop-from (< drop-from (count els)))
                   (first (nth els drop-from))
                   (dec (count text)))
        kept (str/trim (subs text 0 keep-end))
        multiline (str/includes? kept "\n")
        sep (if multiline (str "\n" indent "  ") " ")]
    (str kept (apply str (map (fn [s] (str sep s)) snaps)) ")")))

;; ── the runner ──

(def judge-corrections (atom []))   ; {:file :line :old :new :start :end}
(def judge-pass (atom 0))
(def judge-fail (atom 0))

(defn judge-record! [file src form-rec drop-from snaps]
  (let [text (:text form-rec)
        els (judge-elements text)
        col (loop [p (dec (:start form-rec)) k 0]
              (if (or (neg? p) (= (subs src p (inc p)) "\n")) k (recur (dec p) (inc k))))
        new-text (judge-correct text els drop-from snaps
                                     (apply str (repeat col " ")))]
    (when (not= text new-text)
      (swap! judge-fail inc)
      (swap! judge-corrections conj
             {:file file
              :line (judge-line-of src (:start form-rec))
              :start (:start form-rec) :end (:end form-rec)
              :old text :new new-text}))
    (when (= text new-text) (swap! judge-pass inc))))

(defn judge-eval-test
  "Handle one (test ...) family form. kind ∈ :test :error :stdout :trust."
  [file src form-rec form kind]
  (let [args (rest form)
        expr (first args)
        expected (rest args)]                ; list of expected forms
    (case kind
      :trust
      (if (seq expected)
        (swap! judge-pass inc)          ; cached: never re-run
        (judge-record! file src form-rec nil [(judge-pr (eval expr))]))
      :test
      (let [actual (eval expr)
            snap (judge-pr actual)]
        (if (and (seq expected) (= actual (eval (first expected))))
          (swap! judge-pass inc)
          (judge-record! file src form-rec
                              (when (seq expected) 2) [snap])))
      :error
      (let [msg (try (do (eval expr) :judge-no-error)
                     (catch Exception e (ex-message e)))]
        (if (= msg :judge-no-error)
          (do (swap! judge-fail inc)
              (println (str file ":" (judge-line-of src (:start form-rec))
                            " test-error: expression did not throw")))
          (if (and (seq expected) (= msg (eval (first expected))))
            (swap! judge-pass inc)
            (judge-record! file src form-rec
                                (when (seq expected) 2) [(pr-str msg)]))))
      :stdout
      (let [val (atom nil)
            out (with-out-str (reset! val (eval expr)))
            snaps (if (nil? @val) [(pr-str out)] [(pr-str out) (pr-str @val)])
            want (map (fn [f] (eval f)) expected)
            got (if (nil? @val) [out] [out @val])]
        (if (and (seq expected) (= (seq got) (seq want)))
          (swap! judge-pass inc)
          (judge-record! file src form-rec
                              (when (seq expected) 2) snaps))))))

(def judge-heads
  {"test" :test "judge/test" :test
   "test-error" :error "judge/test-error" :error
   "test-stdout" :stdout "judge/test-stdout" :stdout
   "trust" :trust "judge/trust" :trust})

(defn judge-run-file
  "Eval a file judge-style. Returns nil, or :load-error."
  [file]
  (let [src (slurp file)]
    (try
      (doseq [form-rec (judge-forms src)]
        (let [form (read-string (:text form-rec))]
          (if-let [kind (and (list? form)
                             (symbol? (first form))
                             (get judge-heads (str (first form))))]
            (judge-eval-test file src form-rec form kind)
            (eval form))))
      nil
      (catch Exception e
        (println (str "judge: error loading " file ": " (ex-message e)))
        :load-error))))

;; ── diff display + application ──

(def judge-tty? #?(:cljc (cljc/isatty*) :default false))

(defn judge-color [code s]
  (if judge-tty? (str "[" code "m" s "[0m") s))

(defn judge-show [c]
  (println (str (judge-color "1" (str (:file c) ":" (:line c)))))
  (doseq [l (str/split (:old c) #"\n")]
    (println (judge-color "31" (str "- " l))))
  (doseq [l (str/split (:new c) #"\n")]
    (println (judge-color "32" (str "+ " l))))
  (println))

(defn judge-apply
  "Apply corrections (descending by :start so extents stay valid) to the
  file's source; write to out-path."
  [file corrections out-path]
  (let [src (slurp file)
        out (reduce (fn [s c]
                      (str (subs s 0 (:start c)) (:new c) (subs s (:end c))))
                    src
                    (sort-by :start > corrections))]
    (spit out-path out)
    out-path))

(defn judge-prompt [c]
  (judge-show c)
  (print "accept? [y/n/q] ")
  (flush)
  (let [ans (str/trim (or (read-line) "q"))]
    (cond (or (= ans "") (= ans "y")) :yes
          (= ans "q") :quit
          :else :no)))

(defn -main [& args]
  (let [accept? (contains? (set args) "-a")
        interactive? (contains? (set args) "-i")
        files (remove #{"-a" "-i"} args)]
    (if (empty? files)
      (do (println "usage: cljc judge [-a | -i] <files...>") 1)
      (let [load-err (atom false)]
        (doseq [f files]
          (when (= :load-error (judge-run-file f))
            (reset! load-err true)))
        (let [cs @judge-corrections
              by-file (group-by :file cs)]
          (cond
            @load-err 2
            (empty? cs)
            (do (println (str "judge: " @judge-pass " test"
                              (if (= 1 @judge-pass) "" "s") " passed"))
                0)
            interactive?
            (let [accepted (atom []) quit (atom false)]
              (doseq [c cs]
                (when-not @quit
                  (case (judge-prompt c)
                    :yes (swap! accepted conj c)
                    :quit (reset! quit true)
                    :no nil)))
              (doseq [[f fcs] (group-by :file @accepted)]
                (judge-apply f fcs f)
                (println (str "judge: updated " f " ("
                              (count fcs) " correction"
                              (if (= 1 (count fcs)) "" "s") ")")))
              (if (= (count @accepted) (count cs)) 0 1))
            accept?
            (do (doseq [[f fcs] by-file]
                  (judge-apply f fcs f)
                  (println (str "judge: updated " f " ("
                                (count fcs) " correction"
                                (if (= 1 (count fcs)) "" "s") ")")))
                0)
            :else
            (do (doseq [c cs] (judge-show c))
                (doseq [[f fcs] by-file]
                  (let [out (str f ".tested")]
                    (judge-apply f fcs out)
                    (println (str "judge: wrote " out))))
                (println (str "judge: " @judge-pass " passed, "
                              (count cs) " correction"
                              (if (= 1 (count cs)) "" "s")
                              " (run with -a to accept, -i to review)"))
                1)))))))

;; Run as a script under babashka — `bb judge.cljc file.clj ...` (or `bb -m
;; judge ...`) invokes the tool. Dormant when (require)d as a library.
#?(:bb (when (= *file* (System/getProperty "babashka.file"))
         (System/exit (apply -main *command-line-args*))))
