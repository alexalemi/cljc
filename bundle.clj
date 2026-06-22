;; bundle.clj — build a self-contained native binary from a cljc script.
;;
;; Usage: cljc bundle [flags] <script.clj> <output>
;;   --static          link statically (no glibc dependency; best for sharing)
;;   --windows         cross-compile a Windows .exe via mingw-w64
;;                     (shorthand for --cc=x86_64-w64-mingw32-gcc --libs="-lm -lws2_32")
;;   --cc=<compiler>   compiler to use (default: $CLJC_CC or "cc")
;;   --libs=<libs>     link libraries (default: "-lm -ldl")
;;   --cflags=<flags>  extra compiler flags (e.g. a cross --target)
;;
;; Embeds the script bytes next to the full runtime: one executable,
;; zero dependencies, the same ~2ms startup. (Bundling, not compilation —
;; the script still runs on the interpreter inside.)
;;
;; Needs the cljc.c source: looked up in the cwd first (a source checkout),
;; then in the install's share dir.
;;
;; Cross-compiling: point --cc at a cross toolchain. Examples:
;;   cljc bundle --cc=x86_64-w64-mingw32-gcc --libs="-lm" app.clj app.exe
;;   cljc bundle --cc=aarch64-linux-gnu-gcc --static app.clj app-arm64
;;   cljc bundle --cc="zig cc" --cflags="-target aarch64-linux-musl" \
;;               --static app.clj app-arm64
;; (musl/mingw fold dlopen into libc, so drop -ldl via --libs="-lm".)

(defn parse-args [argv]
  (loop [args (seq argv)
         flags {}
         pos   []]
    (if-let [a (first args)]
      (cond
        (= a "--static")               (recur (rest args) (assoc flags :static true) pos)
        (= a "--windows")              (recur (rest args) (assoc flags :windows true) pos)
        (str/starts-with? a "--cc=")     (recur (rest args) (assoc flags :cc (subs a 5)) pos)
        (str/starts-with? a "--libs=")   (recur (rest args) (assoc flags :libs (subs a 7)) pos)
        (str/starts-with? a "--cflags=") (recur (rest args) (assoc flags :cflags (subs a 9)) pos)
        :else                            (recur (rest args) flags (conj pos a)))
      [flags pos])))

(let [[flags pos] (parse-args *args*)
      [script-path out-path] pos]
  (when-not (and script-path out-path)
    (throw (ex-info "usage: cljc bundle [--static] [--cc=CC] [--libs=...] [--cflags=...] <script.clj> <output>" {})))
  (let [srcdir (cond
                 (cljc/slurp-maybe "cljc.c") "."
                 (cljc/slurp-maybe (str (cljc/sharedir*) "/cljc.c")) (cljc/sharedir*)
                 :else (throw (ex-info "bundle: cljc.c not found (looked in . and the share dir)" {})))
        code   (slurp script-path)
        cfile  (str out-path ".gen.c")
        cc     (or (:cc flags) (cljc/env* "CLJC_CC") (if (:windows flags) "x86_64-w64-mingw32-gcc" "cc"))
        libs   (or (:libs flags) (if (:windows flags) "-lm -lws2_32" "-lm -ldl"))
        static (if (:static flags) " -static" "")
        extra  (if (:cflags flags) (str " " (:cflags flags)) "")]
    (spit cfile
      (str "#define CLJC_NO_MAIN\n"
           "#include \"cljc.c\"\n"
           "static const unsigned char script[] = {"
           (str/join "," (map int (seq code)))
           ",0};\n"
           "int main(int argc, char **argv) {\n"
           "    cljc_set_stack_base(&argc);\n"
           "    CljcEnv *env = cljc_new_env();\n"
           "    Cljc *as = mk_empty_vec();\n"
           "    for (int i = 1; i < argc; i++)\n"
           "        as = vec_conj1(as, mk_str(argv[i], strlen(argv[i])));\n"
           "    env_define_root(env, intern(\"*args*\", 6), as);\n"
           "    cljc_eval_string(env, (const char *)script);\n"
           "    return 0;\n"
           "}\n"))
    (let [cmd (str cc " -O2" static extra " -I" srcdir " -o " out-path " " cfile " " libs " 2>&1")
          r   (sh cmd)]
      (when-not (zero? (:exit r))
        (throw (ex-info (str "bundle: cc failed:\n" cmd "\n" (:out r)) {})))
      (sh (str "rm -f " cfile))
      (println "built:" out-path))))
