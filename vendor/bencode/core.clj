(ns bencode.core
  "Bencode read/write over cljc's char reader/writer shims (StringReader./
   PushbackReader. for in, StringWriter. for out).")

(defn write-bencode [out x]
  (cond
    (integer? x) (.write out (str "i" x "e"))
    (or (string? x) (keyword? x) (symbol? x))
    (let [s (if (string? x) x (name x))] (.write out (str (count s) ":" s)))
    (map? x)
    (do (.write out "d")
        (doseq [k (sort (map #(if (or (keyword? %) (symbol? %)) (name %) (str %)) (keys x)))]
          (write-bencode out k)
          (write-bencode out (or (get x k) (get x (keyword k)))))
        (.write out "e"))
    (sequential? x)
    (do (.write out "l") (doseq [v x] (write-bencode out v)) (.write out "e"))
    :else (write-bencode out (str x)))
  out)

(defn- read-int-until [in end]
  (loop [acc "" ch (.read in)]
    (if (= ch (int end)) (parse-long acc) (recur (str acc (char ch)) (.read in)))))

(defn read-bencode [in]
  (let [ch (.read in)]
    (cond
      (= ch -1) nil
      (= ch (int \i)) (read-int-until in \e)
      (= ch (int \l)) (loop [acc []] (let [n (.read in)]
                                       (if (= n (int \e)) acc
                                           (do (.unread in n) (recur (conj acc (read-bencode in)))))))
      (= ch (int \d)) (loop [acc {}] (let [n (.read in)]
                                       (if (= n (int \e)) acc
                                           (do (.unread in n)
                                               (let [k (read-bencode in) v (read-bencode in)]
                                                 (recur (assoc acc k v)))))))
      :else  ;; a byte-string: <len>:<bytes>
      (let [len (loop [acc (str (char ch)) c (.read in)]
                  (if (= c (int \:)) (parse-long acc) (recur (str acc (char c)) (.read in))))]
        (apply str (repeatedly len #(char (.read in))))))))
