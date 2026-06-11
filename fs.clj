;; fs.clj — babashka.fs-flavored filesystem battery.
;; (load-file "fs.clj") — pulls in libc.clj. Flat names: fs/exists? etc.

(load-file "libc.clj")

(ffi/define [[:pointer opendir [:string]]
             [:pointer readdir [:pointer]]
             [:int closedir [:pointer]]]
            {:headers ["dirent.h"]})
(ffi/defstruct dirent [[:string d_name]] {:headers ["dirent.h"]})

(defn fs/exists? [p] (file-exists? p))
(defn fs/directory? [p] (zero? (:exit (sh (str "test -d " (pr-str p))))))
(defn fs/regular-file? [p] (zero? (:exit (sh (str "test -f " (pr-str p))))))
(defn fs/list-dir
  "Names in directory p, excluding . and .." [p]
  (let [d (opendir p)]
    (when (zero? d) (throw (ex-info (str "fs/list-dir: cannot open " p) {})))
    (try
      (loop [acc []]
        (let [e (readdir d)]
          (if (zero? e)
            (sort acc)
            (let [n (dirent-d_name e)]
              (recur (if (contains? #{"." ".."} n) acc (conj acc n)))))))
      (finally (closedir d)))))
(defn fs/create-dir [p] (zero? (mkdir-p p)))
(defn fs/delete [p] (zero? (if (fs/directory? p) (rmdir p) (unlink p))))
(defn fs/move [src dst] (zero? (rename src dst)))
(defn fs/copy [src dst]
  (zero? (:exit (sh (str "cp " (pr-str src) " " (pr-str dst))))))
(defn fs/file-name [p] (last (str/split p "/")))
(defn fs/parent [p]
  (let [parts (butlast (str/split p "/"))]
    (when (seq parts) (str/join "/" parts))))
(defn fs/extension [p]
  (let [n (fs/file-name p)
        parts (str/split n ".")]
    (when (> (count parts) 1) (last parts))))
(defn fs/temp-dir [] (env "TMPDIR" "/tmp"))
(defn fs/cwd [] (cwd))
