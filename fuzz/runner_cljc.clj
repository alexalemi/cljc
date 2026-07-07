(doseq [line (str/split-lines (slurp (first *args*)))]
  (prn (try (eval (read-string line)) (catch Exception e :FUZZERR))))
