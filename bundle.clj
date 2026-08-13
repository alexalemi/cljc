;; bundle.clj — build a self-contained native binary from a cljc script.
;;
;; Usage: cljc bundle [flags] <script.clj> <output>
;;   --library         build a shared library (.so/.dylib/.dll) with a C ABI
;;                     instead of an executable: exports cljc_lib_init /
;;                     cljc_lib_eval / cljc_lib_last_error and writes a
;;                     matching <output>.h header for C/C++/Rust hosts
;;   --static          link statically (no glibc dependency; best for sharing)
;;   --windows         cross-compile a Windows .exe via mingw-w64
;;                     (shorthand for --cc=x86_64-w64-mingw32-gcc --libs="-lm -lws2_32")
;;   --cc=<compiler>   compiler to use (default: $CLJC_CC or "cc")
;;   --libs=<libs>     link libraries (default: "-lm -ldl")
;;   --cflags=<flags>  extra compiler flags (e.g. a cross --target)
;;
;; Embeds the script bytes — plus every .clj it transitively require's or
;; load-file's (batteries, vendored libs) — next to the full runtime: one
;; executable, zero dependencies, the same ~2ms startup. Embedded files are
;; resolved by slurp/require/load-file at runtime, so a script that uses
;; clojure.test, clojure.set, csp, etc. runs standalone anywhere. (Bundling,
;; not compilation — the script still runs on the interpreter inside.)
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
        (= a "--library")              (recur (rest args) (assoc flags :library true) pos)
        (= a "--windows")              (recur (rest args) (assoc flags :windows true) pos)
        (str/starts-with? a "--cc=")     (recur (rest args) (assoc flags :cc (subs a 5)) pos)
        (str/starts-with? a "--libs=")   (recur (rest args) (assoc flags :libs (subs a 7)) pos)
        (str/starts-with? a "--cflags=") (recur (rest args) (assoc flags :cflags (subs a 9)) pos)
        :else                            (recur (rest args) flags (conj pos a)))
      [flags pos])))

;; ── dependency discovery ───────────────────────────────────────────────────
;; A bundle must be self-contained: embed every .clj the script (transitively)
;; require's or load-file's, since a standalone binary has no vendor/ or share
;; dir. We walk the require/ns/load-file graph and collect the source of each.

(defn ns->rel [n]
  (str/replace (str/replace (str n) "-" "_") "." "/"))

(defn spec->ns [spec]                   ; a require/use spec → its namespace symbol
  (let [s (if (and (seq? spec) (= (first spec) 'quote)) (second spec) spec)]
    (cond (symbol? s) s
          (vector? s) (first s)
          (seq? s)    (first s)
          :else       nil)))

(defn find-on-path [rel]                ; ns rel path → [embedded-name content] or nil
  (some (fn [ext]
          (some (fn [d]
                  (let [p (str d "/" rel ext)
                        c (cljc/slurp-maybe p)]
                    (when c [(str rel ext) c])))
                *load-path*))
        [".clj" ".cljc"]))

(defn find-file [path]                  ; load-file path → [embedded-name content] or nil
  (let [c (or (cljc/slurp-maybe path)
              (some (fn [d] (cljc/slurp-maybe (str d "/" path))) *load-path*))]
    (when c [path c])))

(def deps (atom {}))                    ; embedded-name → content
(def seen (atom #{}))

(declare walk-source)
(defn add-dep! [name content]
  (when-not (contains? @seen name)
    (swap! seen conj name)
    (swap! deps assoc name content)
    (walk-source content)))            ; recurse into the dependency's own deps

(defn walk-source [src]
  (let [forms (try (read-string (str "(\n" src "\n)")) (catch Exception e nil))]
    (doseq [form forms]
      (when (seq? form)
        (let [h (first form)]
          (cond
            (or (= h 'require) (= h 'use))
            (doseq [s (rest form)]
              (when-let [n (spec->ns s)]
                (when-let [[nm c] (find-on-path (ns->rel n))] (add-dep! nm c))))
            (= h 'ns)
            (doseq [clause (rest form)]
              (when (and (sequential? clause) (or (= (first clause) :require) (= (first clause) :use)))
                (doseq [s (rest clause)]
                  (when-let [n (spec->ns s)]
                    (when-let [[nm c] (find-on-path (ns->rel n))] (add-dep! nm c))))))
            (= h 'load-file)
            (when (string? (second form))
              (when-let [[nm c] (find-file (second form))] (add-dep! nm c)))))))))

(let [[flags pos] (parse-args *args*)
      [script-path out-path] pos]
  (when-not (and script-path out-path)
    (throw (ex-info "usage: cljc bundle [--static] [--cc=CC] [--libs=...] [--cflags=...] <script.clj> <output>" {})))
  (let [srcdir (cond
                 (cljc/slurp-maybe "cljc.c") "."
                 (cljc/slurp-maybe (str (cljc/sharedir*) "/cljc.c")) (cljc/sharedir*)
                 :else (throw (ex-info "bundle: cljc.c not found (looked in . and the share dir)" {})))
        code   (slurp script-path)
        _      (walk-source code)        ; collect transitive .clj dependencies
        dep-list (vec @deps)             ; [[name content] ...]
        cfile  (str out-path ".gen.c")
        cc     (or (:cc flags) (cljc/env* "CLJC_CC") (if (:windows flags) "x86_64-w64-mingw32-gcc" "cc"))
        libs   (or (:libs flags) (if (:windows flags) "-lm -lws2_32" "-lm -ldl"))
        static (if (:static flags) " -static" "")
        extra  (if (:cflags flags) (str " " (:cflags flags)) "")]
    (when (seq dep-list)
      (println (str "  embedding " (count dep-list) " dependency file(s): "
                    (str/join " " (map first dep-list)))))
    (spit cfile
      (str "#define CLJC_NO_MAIN\n"
           "#include \"cljc.c\"\n"
           "static const unsigned char script[] = {"
           (str/join "," (map int (seq code)))
           ",0};\n"
           ;; embedded dependency files, each a NUL-terminated byte array
           (str/join "" (map-indexed
                          (fn [i [_ content]]
                            (str "static const unsigned char dep" i "[] = {"
                                 (str/join "," (map int (seq content))) ",0};\n"))
                          dep-list))
           "static const CljcEmbeddedFile bundled[] = {\n"
           (str/join "" (map-indexed
                          (fn [i [name _]]
                            (str "    {\"" name "\", (const char *)dep" i "},\n"))
                          dep-list))
           "    {0,0}\n};\n"           ; sentinel keeps the array non-empty in C
           (if-not (:library flags)
             (str
               "int main(int argc, char **argv) {\n"
               "    cljc_set_stack_base(&argc);\n"
               "    CljcEnv *env = cljc_new_env();\n"
               "    cljc_set_embedded_files(bundled, " (count dep-list) ");\n"
               "    Cljc *as = mk_empty_vec();\n"
               "    for (int i = 1; i < argc; i++)\n"
               "        as = vec_conj1(as, mk_str(argv[i], strlen(argv[i])));\n"
               "    env_define_root(env, intern(\"*args*\", 6), as);\n"
               "    cljc_eval_string(env, (const char *)script);\n"
               "    return 0;\n"
               "}\n")
             ;; --library: a minimal C ABI over the embedded interpreter.
             ;; cljc_eval_string anchors the conservative GC at its own frame
             ;; per call, so hosts may call from any stack depth; returned
             ;; strings stay valid until the next cljc_lib_* call.
             (str
               "#if defined(_WIN32)\n"
               "#define CLJC_LIB_EXPORT __declspec(dllexport)\n"
               "#else\n"
               "#define CLJC_LIB_EXPORT\n"
               "#endif\n"
               "static CljcEnv *cljc_lib_env;\n"
               "static char *cljc_lib_out;   /* last result, freed on next call */\n"
               "static char *cljc_lib_err;   /* last error, freed on next call */\n"
               "static void cljc_lib_seterr_(void) {\n"
               "    SBuf sb = {0}; sb_grow(&sb, 1); sb.data[0] = '\\0';\n"
               "    if (cur_exc && cur_exc != NIL) print_to(&sb, cur_exc, true);\n"
               "    else { for (const char *p = err_msg; *p; p++) sb_putc(&sb, *p); }\n"
               "    free(cljc_lib_err); cljc_lib_err = sb.data;\n"
               "}\n"
               "CLJC_LIB_EXPORT const char *cljc_lib_last_error(void) { return cljc_lib_err; }\n"
               "CLJC_LIB_EXPORT int cljc_lib_init(void) {\n"
               "    if (cljc_lib_env) return 0;\n"
               "    CljcEnv *env = cljc_new_env();\n"
               "    cljc_set_embedded_files(bundled, " (count dep-list) ");\n"
               "    env_define_root(env, intern(\"*args*\", 6), mk_empty_vec());\n"
               "    cljc_eval_string(env, (const char *)script);\n"
               "    if (cljc_eval_errored) { cljc_lib_seterr_(); return 1; }\n"
               "    cljc_lib_env = env;\n"
               "    return 0;\n"
               "}\n"
               "CLJC_LIB_EXPORT const char *cljc_lib_eval(const char *src) {\n"
               "    if (!cljc_lib_env && cljc_lib_init() != 0) return NULL;\n"
               "    Cljc *v = cljc_eval_string(cljc_lib_env, src);\n"
               "    if (cljc_eval_errored) { cljc_lib_seterr_(); return NULL; }\n"
               "    SBuf sb = {0}; sb_grow(&sb, 1); sb.data[0] = '\\0';\n"
               "    print_to(&sb, v, true);\n"
               "    free(cljc_lib_out); cljc_lib_out = sb.data;\n"
               "    return cljc_lib_out;\n"
               "}\n"))))
    (when (:library flags)
      (spit (str out-path ".h")
        (str "/* Generated by `cljc bundle --library`. Link against " out-path ".\n"
             " * All returned strings are owned by the library and valid until the\n"
             " * next cljc_lib_* call. Functions are not thread-safe: one\n"
             " * interpreter per process, single-threaded. */\n"
             "#ifndef CLJC_LIB_H\n#define CLJC_LIB_H\n"
             "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
             "int cljc_lib_init(void);              /* 0 = ok (runs the embedded script; idempotent) */\n"
             "const char *cljc_lib_eval(const char *src);  /* pr-str of the last form; NULL on error */\n"
             "const char *cljc_lib_last_error(void);       /* message for the last failed call */\n"
             "#ifdef __cplusplus\n}\n#endif\n"
             "#endif\n")))
    (let [shared (if (:library flags) (if (:windows flags) " -shared" " -shared -fPIC") "")
          cmd (str cc " -O2" static shared extra " -I" srcdir " -o " out-path " " cfile " " libs " 2>&1")
          r   (sh cmd)]
      (when-not (zero? (:exit r))
        (throw (ex-info (str "bundle: cc failed:\n" cmd "\n" (:out r)) {})))
      (sh (str "rm -f " cfile))
      (if (:library flags)
        (println "built:" out-path "(+ header" (str out-path ".h)"))
        (println "built:" out-path)))))
