;; libc.clj — batteries module binding the useful libc surface via the FFI,
;; in the spirit of s7's libc.scm. Load with: (load-file "libc.clj")
;;
;; Everything lands in the flat global env. Pointers travel as integers
;; (0 is NULL). Compiled glue is cached in /tmp by content hash, so the
;; first load compiles once and later loads just dlopen.

(declare getpid getppid getenv setenv unsetenv system
         mkdir rmdir unlink rename chdir access get_current_dir_name
         sleep usleep)

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
   [:string get_current_dir_name []]
   ;; sleep — NOTE: no `time` binding: it would shadow the core time macro
   [:int sleep [:int]]
   [:int usleep [:int]]]
  {:headers ["unistd.h" "stdlib.h" "sys/stat.h" "stdio.h"]})

;; ── friendly wrappers ──

(def F_OK 0)
(def R_OK 4)
(def W_OK 2)
(def X_OK 1)

(defn file-exists? [path] (zero? (access path F_OK)))
(defn cwd [] (get_current_dir_name))
(defn now-epoch [] (cljc/epoch*))
(defn mkdir-p [path] (mkdir path 493))   ; 0755
(defn env
  "Environment variable as string, or default."
  ([k] (getenv k))
  ([k default] (or (getenv k) default)))
