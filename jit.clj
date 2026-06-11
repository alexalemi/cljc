;; jit.clj — compile numeric cljc functions to native C, live.
;;
;;   (load-file "jit.clj")
;;   (jit/defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
;;   (jit/compile! 'fib)     ; generates C, compiles, swaps the binding
;;   (fib 35)                ; now runs as native machine code
;;
;; Supported subset (v1, integers only — unboxed long long throughout):
;;   literals, params/locals, if, let, loop/recur, self-recursion,
;;   + - * quot rem mod inc dec < > <= >= = not= zero? pos? neg? min max
;; Anything else fails at compile time with a clear message; the
;; interpreted version stays in place. Args are checked (must be ints).

(def cljc/jit-sources (atom {}))
(def cljc/jit-lines (atom []))
(def cljc/jit-tmp (atom 0))

(defmacro jit/defn [name params & body]
  `(do (swap! cljc/jit-sources assoc '~name '(~params ~@body))
       (defn ~name ~params ~@body)))

(defn cljc/jit-emit [line] (swap! cljc/jit-lines conj line))
(defn cljc/jit-tmpvar [] (str "t" (swap! cljc/jit-tmp inc)))
(defn cljc/cname [sym] (re-replace (str sym) "[^A-Za-z0-9_]" "_"))

(def cljc/jit-binops
  {'+ "+", '- "-", '* "*", 'quot "/", 'rem "%",
   '< "<", '> ">", '<= "<=", '>= ">=", '= "==", 'not= "!="})

;; Emit C statements computing `form` into C variable `dst`.
;; locals: set of in-scope symbols. self: [name nparams] for direct recursion.
(defn cljc/jit-expr [form dst locals self]
  (cond
    (int? form) (cljc/jit-emit (str dst " = " form "LL;"))
    (symbol? form)
    (if (contains? locals form)
      (cljc/jit-emit (str dst " = " (cljc/cname form) ";"))
      (throw (ex-info (str "jit: free variable " form) {})))
    (list? form)
    (let [op (first form) args (rest form)]
      (cond
        (contains? cljc/jit-binops op)
        (let [ts (mapv (fn [a] (let [t (cljc/jit-tmpvar)]
                                 (cljc/jit-emit (str "long long " t ";"))
                                 (cljc/jit-expr a t locals self)
                                 t))
                       args)]
          (cljc/jit-emit (str dst " = "
                              (str/join (str " " (get cljc/jit-binops op) " ") (seq ts))
                              ";")))
        (= op 'if)
        (let [tc (cljc/jit-tmpvar)]
          (cljc/jit-emit (str "long long " tc ";"))
          (cljc/jit-expr (nth (vec args) 0) tc locals self)
          (cljc/jit-emit (str "if (" tc ") {"))
          (cljc/jit-expr (nth (vec args) 1) dst locals self)
          (cljc/jit-emit "} else {")
          (if (> (count args) 2)
            (cljc/jit-expr (nth (vec args) 2) dst locals self)
            (cljc/jit-emit (str dst " = 0;")))
          (cljc/jit-emit "}"))
        (= op 'let)
        (let [bv (first args)
              body (rest args)
              locals2 (loop [i 0 ls locals]
                        (if (< i (count bv))
                          (let [nm (nth bv i)]
                            (when-not (symbol? nm)
                              (throw (ex-info "jit: let needs simple symbols" {})))
                            (cljc/jit-emit (str "long long " (cljc/cname nm) ";"))
                            (cljc/jit-expr (nth bv (inc i)) (cljc/cname nm)
                                           ls self)
                            (recur (+ i 2) (conj ls nm)))
                          ls))]
          (doseq [b (butlast body)]
            (let [tt (cljc/jit-tmpvar)]
              (cljc/jit-emit (str "long long " tt ";"))
              (cljc/jit-expr b tt locals2 self)))
          (cljc/jit-expr (last body) dst locals2 self))
        (= op 'loop)
        (let [bv (first args)
              body (rest args)
              names (vec (map (fn [i] (nth bv (* 2 i))) (range (quot (count bv) 2))))
              locals2 (reduce conj locals (seq names))]
          (doseq [i (range (count names))]
            (cljc/jit-emit (str "long long " (cljc/cname (nth names i)) ";"))
            (cljc/jit-expr (nth bv (inc (* 2 i))) (cljc/cname (nth names i))
                           locals self))
          (cljc/jit-emit "for (;;) {")
          (cljc/jit-expr (last body) dst locals2 [:loop names])
          (cljc/jit-emit "break; }"))
        (= op 'recur)
        (let [[kind maybe-name maybe-params] self
              names (if (= kind :fn) maybe-params maybe-name)]
          (when-not kind (throw (ex-info "jit: recur outside loop/fn" {})))
          (let [tmps (mapv (fn [a] (let [t (cljc/jit-tmpvar)]
                                     (cljc/jit-emit (str "long long " t ";"))
                                     (cljc/jit-expr a t locals self)
                                     t))
                           args)]
            (doseq [i (range (count names))]
              (cljc/jit-emit (str (cljc/cname (nth names i)) " = " (nth tmps i) ";")))
            (cljc/jit-emit "continue;")))
        (= op 'inc) (do (cljc/jit-expr (first args) dst locals self)
                        (cljc/jit-emit (str dst " = " dst " + 1;")))
        (= op 'dec) (do (cljc/jit-expr (first args) dst locals self)
                        (cljc/jit-emit (str dst " = " dst " - 1;")))
        (= op 'zero?) (do (cljc/jit-expr (first args) dst locals self)
                          (cljc/jit-emit (str dst " = (" dst " == 0);")))
        (= op 'pos?) (do (cljc/jit-expr (first args) dst locals self)
                         (cljc/jit-emit (str dst " = (" dst " > 0);")))
        (= op 'neg?) (do (cljc/jit-expr (first args) dst locals self)
                         (cljc/jit-emit (str dst " = (" dst " < 0);")))
        (= op 'mod)
        (let [ta (cljc/jit-tmpvar) tb (cljc/jit-tmpvar)]
          (cljc/jit-emit (str "long long " ta "; long long " tb ";"))
          (cljc/jit-expr (nth (vec args) 0) ta locals self)
          (cljc/jit-expr (nth (vec args) 1) tb locals self)
          (cljc/jit-emit (str dst " = " ta " % " tb ";"))
          (cljc/jit-emit (str "if (" dst " != 0 && ((" dst " < 0) != (" tb " < 0))) "
                              dst " += " tb ";")))
        (= op 'min) (let [ta (cljc/jit-tmpvar) tb (cljc/jit-tmpvar)]
                      (cljc/jit-emit (str "long long " ta "; long long " tb ";"))
                      (cljc/jit-expr (nth (vec args) 0) ta locals self)
                      (cljc/jit-expr (nth (vec args) 1) tb locals self)
                      (cljc/jit-emit (str dst " = " ta " < " tb " ? " ta " : " tb ";")))
        (= op 'max) (let [ta (cljc/jit-tmpvar) tb (cljc/jit-tmpvar)]
                      (cljc/jit-emit (str "long long " ta "; long long " tb ";"))
                      (cljc/jit-expr (nth (vec args) 0) ta locals self)
                      (cljc/jit-expr (nth (vec args) 1) tb locals self)
                      (cljc/jit-emit (str dst " = " ta " > " tb " ? " ta " : " tb ";")))
        (and (vector? self) (= :fn (nth self 0)) (= op (nth self 1)))
        (let [ts (mapv (fn [a] (let [t (cljc/jit-tmpvar)]
                                 (cljc/jit-emit (str "long long " t ";"))
                                 (cljc/jit-expr a t locals self)
                                 t))
                       args)]
          (cljc/jit-emit (str dst " = impl_" (cljc/cname (nth self 1))
                              "(" (str/join ", " (seq ts)) ");")))
        :else (throw (ex-info (str "jit: unsupported form " (pr-str form)) {}))))
    :else (throw (ex-info (str "jit: unsupported literal " (pr-str form)) {}))))

(defn jit/compile! [fname]
  (let [src (get @cljc/jit-sources fname)]
    (when-not src (throw (ex-info (str "jit: no source for " fname
                                       " (define with jit/defn)") {})))
    (reset! cljc/jit-lines [])
    (reset! cljc/jit-tmp 0)
    (let [params (first src)
          body (rest src)
          cn (cljc/cname fname)
          locals (into #{} (seq params))]
      (cljc/jit-emit (str "static long long impl_" cn "("
                          (str/join ", " (map (fn [p] (str "long long " (cljc/cname p)))
                                              (seq params)))
                          ") {"))
      (cljc/jit-emit "long long result;")
      ;; fn-level recur: params are the loop vars
      (cljc/jit-emit "for (;;) {")
      (doseq [b (butlast body)]
        (let [tt (cljc/jit-tmpvar)]
          (cljc/jit-emit (str "long long " tt ";"))
          (cljc/jit-expr b tt locals [:fn fname (vec params)])))
      (cljc/jit-expr (last body) "result" locals [:fn fname (vec params)])
      (cljc/jit-emit "break; }")
      (cljc/jit-emit "return result; }")
      (let [code (str cljc/ffi-api-decl
                      (str/join "\n" @cljc/jit-lines) "\n"
                      "static void *w_" cn "(void *env, void *args, int nargs) {\n"
                      "  (void)env;\n"
                      "  if (nargs != " (count params) ") api->error(\"" fname ": wrong arity\");\n"
                      "  return api->mk_int(impl_" cn "("
                      (str/join ", " (map (fn [i] (str "api->as_int(api->nth_arg(args, " i "))"))
                                          (range (count params))))
                      "));\n}\n"
                      "void cljc_module_init(void *env, CljcFfiApi *a) { api = a;\n"
                      "  api->def_native(env, \"" fname "\", w_" cn ");\n}\n")]
        (cljc/ffi-build code "")
        fname))))
