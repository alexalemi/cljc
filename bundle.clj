;; bundle.clj — build a self-contained native binary from a cljc script.
;; Usage: cljc bundle myscript.clj mybinary
;; Embeds the script bytes next to the full runtime: one executable,
;; zero dependencies, the same ~2ms startup. (Bundling, not compilation —
;; the script still runs on the interpreter inside.)
;;
;; Needs the cljc.c source: looked up in the cwd first (a source checkout),
;; then in the install's share dir.

(let [[script-path out-path] *args*]
  (when-not (and script-path out-path)
    (throw (ex-info "usage: cljc bundle <script.clj> <output>" {})))
  (let [srcdir (cond
                 (cljc/slurp-maybe "cljc.c") "."
                 (cljc/slurp-maybe (str (cljc/sharedir*) "/cljc.c")) (cljc/sharedir*)
                 :else (throw (ex-info "bundle: cljc.c not found (looked in . and the share dir)" {})))
        code (slurp script-path)
        cfile (str out-path ".gen.c")]
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
    (let [r (sh (str "cc -O2 -I" srcdir " -o " out-path " " cfile " -lm -ldl"))]
      (when-not (zero? (:exit r))
        (throw (ex-info (str "bundle: cc failed:\n" (:out r)) {})))
      (sh (str "rm -f " cfile))
      (println "built:" out-path))))
