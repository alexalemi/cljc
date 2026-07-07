(doseq [line (clojure.string/split-lines (slurp (first *command-line-args*)))]
  (prn (try (eval (read-string line)) (catch Exception e :FUZZERR) (catch Error e :FUZZERR))))
