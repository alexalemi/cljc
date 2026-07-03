;; process.clj — babashka.process-flavored battery over (sh ...).
;; (load-file "process.clj")

(defn process/escape
  "Single-quote a string for the shell." [s]
  (str "'" (str/replace s "'" "'\\''") "'"))

(defn process/sh
  "Run cmd (+ args, each shell-escaped); returns {:exit n :out s}."
  [cmd & args]
  ;; clojure.core/sh, NOT bare sh: when this file loads under (require
  ;; 'process), in-ns resolution makes bare `sh` mean process/sh — this very
  ;; fn — and the tail self-call spins forever. The core-qualified form
  ;; always names the builtin.
  (clojure.core/sh (str/join " " (cons cmd (map process/escape args)))))

(defn process/shell
  "Like process/sh but throws ex-info on nonzero exit."
  [cmd & args]
  (let [r (apply process/sh cmd args)]
    (when-not (zero? (:exit r))
      (throw (ex-info (str "process/shell: " cmd " exited " (:exit r))
                      r)))
    r))

(defn process/out
  "stdout of cmd, trimmed; throws on nonzero exit."
  [cmd & args]
  (str/trim (:out (apply process/shell cmd args))))
