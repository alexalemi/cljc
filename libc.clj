;; libc.clj — batteries module binding the useful libc surface via the FFI,
;; in the spirit of s7's libc.scm. Load with: (load-file "libc.clj")
;;
;; Everything lands in the flat global env. Pointers travel as integers
;; (0 is NULL). Compiled glue is cached in /tmp by content hash, so the
;; first load compiles once and later loads just dlopen.

(declare getpid getppid getenv setenv unsetenv system
         mkdir rmdir unlink rename chdir access cljc_libc_cwd
         sleep usleep)

;; getcwd wants a caller-owned buffer, and the buffer-free GNU alternative
;; (get_current_dir_name) doesn't exist on macOS — a tiny shim owns the buffer.
(spit "/tmp/cljc_libc_shim.h"
      "#include <unistd.h>\nstatic inline char *cljc_libc_cwd(void){ static char b[4096]; return getcwd(b, sizeof b); }\n")

(ffi/define
  [;; process & environment
   [:int getpid []]
   [:int getppid []]
   [:string getenv [:string]]
   [:int setenv [:string :string :int]]
   [:int unsetenv [:string]]
   [:int system [:string]]
   ;; filesystem
   [:int mkdir [:string :int]]
   [:int rmdir [:string]]
   [:int unlink [:string]]
   [:int rename [:string :string]]
   [:int chdir [:string]]
   [:int access [:string :int]]
   [:string cljc_libc_cwd []]
   ;; sleep — NOTE: no `time` binding: it would shadow the core time macro
   [:int sleep [:int]]
   [:int usleep [:int]]]
  {:headers ["unistd.h" "stdlib.h" "sys/stat.h" "stdio.h" "cljc_libc_shim.h"]
   :libs "-I/tmp"})

;; ── friendly wrappers ──

(def F_OK 0)
(def R_OK 4)
(def W_OK 2)
(def X_OK 1)

(defn file-exists? [path] (zero? (access path F_OK)))
(defn cwd [] (cljc_libc_cwd))
(defn now-epoch [] (cljc/epoch*))
(defn mkdir-p
  "Create path AND any missing parents (like mkdir -p); 0 when path exists after."
  [path]
  (loop [parts (remove empty? (str/split path "/"))
         cur   (when (str/starts-with? path "/") "")]
    (if (empty? parts)
      (if (zero? (access path F_OK)) 0 -1)
      (let [cur' (if (nil? cur) (first parts) (str cur "/" (first parts)))]
        (mkdir cur' 493)                       ; 0755; EEXIST is fine
        (recur (rest parts) cur')))))
(defn env
  "Environment variable as string, or default."
  ([k] (getenv k))
  ([k default] (or (getenv k) default)))
