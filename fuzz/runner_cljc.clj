(doseq [line (str/split-lines (slurp (first *args*)))
        :when (not (or (str/blank? line) (str/starts-with? line ";")))]
  (prn (try (eval (read-string line)) (catch Exception e :FUZZERR))))
