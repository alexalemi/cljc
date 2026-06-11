(let [text (apply str (repeat 5000 "lorem ipsum dolor sit amet consectetur "))]
  (println (count (re-seq #"\w+" text))
           (get (frequencies (re-seq #"\w+" text)) "lorem")))
