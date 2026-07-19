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
           ;; raw UTF-8 bytes, not codepoints — non-ASCII must round-trip
           (str/join "," (cljc/str-bytes* code))
           ",0};\n"
           ;; embedded dependency files, each a NUL-terminated byte array
           (str/join "" (map-indexed
                          (fn [i [_ content]]
                            (str "static const unsigned char dep" i "[] = {"
                                 (str/join "," (cljc/str-bytes* content)) ",0};\n"))
                          dep-list))
           "static const CljcEmbeddedFile bundled[] = {\n"
           (str/join "" (map-indexed
                          (fn [i [name _]]
                            (str "    {\"" name "\", (const char *)dep" i "},\n"))
                          dep-list))
           "    {0,0}\n};\n"           ; sentinel keeps the array non-empty in C
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
           "}\n"))
    (let [cmd (str cc " -O2" static extra " -I" srcdir " -o " out-path " " cfile " " libs " 2>&1")
          r   (sh cmd)]
      (when-not (zero? (:exit r))
        (throw (ex-info (str "bundle: cc failed:\n" cmd "\n" (:out r)) {})))
      (sh (str "rm -f " cfile))
      (println "built:" out-path))))
