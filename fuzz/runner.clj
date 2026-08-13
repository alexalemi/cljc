(doseq [line (clojure.string/split-lines (slurp (first *command-line-args*)))
        :when (not (or (clojure.string/blank? line)
                       (clojure.string/starts-with? line ";")))]
  (prn (try (eval (read-string line)) (catch Exception e :FUZZERR) (catch Error e :FUZZERR))))
