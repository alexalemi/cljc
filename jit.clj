;; jit.clj — compile numeric cljc functions to native C, live.
;;
;;   (load-file "jit.clj")
;;   (jit/defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
;;   (jit/compile! 'fib)     ; generates C, compiles, swaps the binding
;;   (fib 35)                ; now runs as native machine code
;;
;; Supported subset (unboxed long long / double throughout):
;;   literals, params/locals, if, let, loop/recur, self-recursion,
;;   + - * / quot rem mod inc dec < > <= >= = not= zero? pos? neg? min max
;;   Math/sqrt, (double x), (long x)
;; Types default to long long; JVM-style ^long/^double hints on the fn name
;; (return) and params pick doubles, and types flow through arithmetic like
;; C promotion (any double operand => double):
;;   (jit/defn ^double dist [^double x ^double y]
;;     (Math/sqrt (+ (* x x) (* y y))))
;; quot/rem/mod stay integer-only; / needs at least one double operand (its
;; result is always double — use quot for integer division). Anything else
;; fails at compile time with a clear message; the interpreted version stays
;; in place. Args are checked (ints for ^long, numbers for ^double).

(def cljc/jit-sources (atom {}))
(def cljc/jit-lines (atom []))
(def cljc/jit-tmp (atom 0))

(defmacro jit/defn [name params & body]
  `(do (swap! cljc/jit-sources assoc '~name '(~name ~params ~@body))
       (defn ~name ~params ~@body)))

(defn cljc/jit-emit [line] (swap! cljc/jit-lines conj line))
(defn cljc/jit-tmpvar [] (str "t" (swap! cljc/jit-tmp inc)))
(defn cljc/cname [sym] (re-replace (str sym) "[^A-Za-z0-9_]" "_"))

(def cljc/jit-binops
  {'+ "+", '- "-", '* "*", 'quot "/", 'rem "%",
   '< "<", '> ">", '<= "<=", '>= ">=", '= "==", 'not= "!="})

;; ── types ──────────────────────────────────────────────────────────────────
;; Two unboxed types, :long and :double. tenv maps local symbol → type.

(defn cljc/jit-ctype [t] (if (= t :double) "double" "long long"))

(defn cljc/jit-hint [sym]              ; ^long/^double reader hint → type or nil
  (let [tag (:tag (meta sym))]
    (cond (nil? tag) nil
          (= tag 'double) :double
          (= tag 'long) :long
          :else (throw (ex-info (str "jit: unsupported type hint ^" tag
                                     " on " sym " (only ^long/^double)") {})))))

(defn cljc/jit-unify [a b] (if (or (= a :double) (= b :double)) :double :long))

;; Type of `form` under tenv. self: [:fn name params ptypes ret] for the
;; enclosing function (loop recur never produces a value, so loops carry no
;; type of their own).
(defn cljc/jit-type [form tenv self]
  (cond
    (int? form) :long
    (double? form) :double
    (symbol? form)
    (or (get tenv form)
        (throw (ex-info (str "jit: free variable " form) {})))
    (list? form)
    (let [op (first form) args (rest form)]
      (cond
        (contains? #{'+ '- '* 'min 'max} op)
        (reduce cljc/jit-unify :long
                (map (fn [a] (cljc/jit-type a tenv self)) args))
        (contains? #{'quot 'rem 'mod} op) :long
        (contains? #{'< '> '<= '>= '= 'not= 'zero? 'pos? 'neg?} op) :long
        (= op '/) :double
        (= op 'Math/sqrt) :double
        (= op 'double) :double
        (= op 'long) :long
        (contains? #{'inc 'dec} op) (cljc/jit-type (first args) tenv self)
        (= op 'if)
        (let [a (vec args)]
          (cljc/jit-unify (cljc/jit-type (nth a 1) tenv self)
                          (if (> (count a) 2)
                            (cljc/jit-type (nth a 2) tenv self)
                            :long)))
        (= op 'let)
        (let [bv (first args)
              tenv2 (loop [i 0 te tenv]
                      (if (< i (count bv))
                        (let [nm (nth bv i)]
                          (recur (+ i 2)
                                 (assoc te nm (or (cljc/jit-hint nm)
                                                  (cljc/jit-type (nth bv (inc i)) te self)))))
                        te))]
          (cljc/jit-type (last (rest args)) tenv2 self))
        (= op 'loop)
        (let [bv (first args)
              tenv2 (loop [i 0 te tenv]
                      (if (< i (count bv))
                        (let [nm (nth bv i)]
                          (recur (+ i 2)
                                 (assoc te nm (or (cljc/jit-hint nm)
                                                  (cljc/jit-type (nth bv (inc i)) te self)))))
                        te))]
          (cljc/jit-type (last (rest args)) tenv2 self))
        (= op 'recur) :long                  ; never used as a value
        (and (vector? self) (= :fn (nth self 0)) (= op (nth self 1)))
        (nth self 4)                         ; declared/derived return type
        :else (throw (ex-info (str "jit: unsupported form " (pr-str form)) {}))))
    :else (throw (ex-info (str "jit: unsupported literal " (pr-str form)) {}))))

;; ── emission ───────────────────────────────────────────────────────────────
;; Emit C statements computing `form` into C variable `dst` (already declared
;; by the caller, with the type cljc/jit-type reports for `form`; C's implicit
;; long long <-> double conversion covers promoting assignments).
;; loop-ctx: [names] of the innermost loop for recur, nil at fn level (recur
;; then rebinds the params). self as in cljc/jit-type.

(defn cljc/jit-tmp-for [form tenv self]  ; declare + fill a temp for form
  (let [t (cljc/jit-tmpvar)]
    (cljc/jit-emit (str (cljc/jit-ctype (cljc/jit-type form tenv self)) " " t ";"))
    (cljc/jit-expr form t tenv self)
    t))

(defn cljc/jit-expr [form dst tenv self]
  (cond
    (int? form) (cljc/jit-emit (str dst " = " form "LL;"))
    (double? form) (cljc/jit-emit (str dst " = " (pr-str form) ";"))
    (symbol? form)
    (if (contains? tenv form)
      (cljc/jit-emit (str dst " = " (cljc/cname form) ";"))
      (throw (ex-info (str "jit: free variable " form) {})))
    (list? form)
    (let [op (first form) args (rest form)]
      (cond
        (contains? cljc/jit-binops op)
        (let [_ (when (and (contains? #{'quot 'rem} op)
                           (some (fn [a] (= :double (cljc/jit-type a tenv self))) args))
                  (throw (ex-info (str "jit: " op " is integer-only (got a double operand)") {})))
              ts (mapv (fn [a] (cljc/jit-tmp-for a tenv self)) args)]
          (if (and (= op '-) (= 1 (count ts)))
            (cljc/jit-emit (str dst " = - " (first ts) ";"))
            (cljc/jit-emit (str dst " = "
                                (str/join (str " " (get cljc/jit-binops op) " ") (seq ts))
                                ";"))))
        (= op '/)
        (do (when-not (some (fn [a] (= :double (cljc/jit-type a tenv self))) args)
              (throw (ex-info "jit: / needs a double operand (use quot for integers)" {})))
            (let [ts (mapv (fn [a] (cljc/jit-tmp-for a tenv self)) args)]
              (if (= 1 (count ts))
                (cljc/jit-emit (str dst " = 1.0 / " (first ts) ";"))
                (cljc/jit-emit (str dst " = (double)" (str/join " / (double)" (seq ts)) ";")))))
        (= op 'Math/sqrt)
        (cljc/jit-emit (str dst " = sqrt((double)" (cljc/jit-tmp-for (first args) tenv self) ");"))
        (= op 'double)
        (cljc/jit-emit (str dst " = (double)" (cljc/jit-tmp-for (first args) tenv self) ";"))
        (= op 'long)
        (cljc/jit-emit (str dst " = (long long)" (cljc/jit-tmp-for (first args) tenv self) ";"))
        (= op 'if)
        (let [tc (cljc/jit-tmp-for (nth (vec args) 0) tenv self)]
          (cljc/jit-emit (str "if (" tc ") {"))
          (cljc/jit-expr (nth (vec args) 1) dst tenv self)
          (cljc/jit-emit "} else {")
          (if (> (count args) 2)
            (cljc/jit-expr (nth (vec args) 2) dst tenv self)
            (cljc/jit-emit (str dst " = 0;")))
          (cljc/jit-emit "}"))
        (= op 'let)
        (let [bv (first args)
              body (rest args)
              tenv2 (loop [i 0 te tenv]
                      (if (< i (count bv))
                        (let [nm (nth bv i)]
                          (when-not (symbol? nm)
                            (throw (ex-info "jit: let needs simple symbols" {})))
                          (let [ty (or (cljc/jit-hint nm)
                                       (cljc/jit-type (nth bv (inc i)) te self))]
                            (cljc/jit-emit (str (cljc/jit-ctype ty) " " (cljc/cname nm) ";"))
                            (cljc/jit-expr (nth bv (inc i)) (cljc/cname nm) te self)
                            (recur (+ i 2) (assoc te nm ty))))
                        te))]
          (doseq [b (butlast body)]
            (cljc/jit-tmp-for b tenv2 self))
          (cljc/jit-expr (last body) dst tenv2 self))
        (= op 'loop)
        (let [bv (first args)
              body (rest args)
              names (vec (map (fn [i] (nth bv (* 2 i))) (range (quot (count bv) 2))))
              tenv2 (loop [i 0 te tenv]
                      (if (< i (count bv))
                        (let [nm (nth bv i)]
                          (recur (+ i 2)
                                 (assoc te nm (or (cljc/jit-hint nm)
                                                  (cljc/jit-type (nth bv (inc i)) te self)))))
                        te))]
          (doseq [i (range (count names))]
            (let [nm (nth names i)]
              (cljc/jit-emit (str (cljc/jit-ctype (get tenv2 nm)) " " (cljc/cname nm) ";"))
              (cljc/jit-expr (nth bv (inc (* 2 i))) (cljc/cname nm) tenv self)))
          (cljc/jit-emit "for (;;) {")
          (cljc/jit-expr (last body) dst tenv2 [:loop names])
          (cljc/jit-emit "break; }"))
        (= op 'recur)
        (let [kind (nth self 0)
              names (if (= kind :fn) (nth self 2) (nth self 1))]
          (when-not kind (throw (ex-info "jit: recur outside loop/fn" {})))
          (let [tmps (mapv (fn [a] (cljc/jit-tmp-for a tenv self)) args)]
            (doseq [i (range (count names))]
              (cljc/jit-emit (str (cljc/cname (nth names i)) " = " (nth tmps i) ";")))
            (cljc/jit-emit "continue;")))
        (= op 'inc) (do (cljc/jit-expr (first args) dst tenv self)
                        (cljc/jit-emit (str dst " = " dst " + 1;")))
        (= op 'dec) (do (cljc/jit-expr (first args) dst tenv self)
                        (cljc/jit-emit (str dst " = " dst " - 1;")))
        (= op 'zero?) (cljc/jit-emit (str dst " = (" (cljc/jit-tmp-for (first args) tenv self) " == 0);"))
        (= op 'pos?) (cljc/jit-emit (str dst " = (" (cljc/jit-tmp-for (first args) tenv self) " > 0);"))
        (= op 'neg?) (cljc/jit-emit (str dst " = (" (cljc/jit-tmp-for (first args) tenv self) " < 0);"))
        (= op 'mod)
        (let [_ (when (some (fn [a] (= :double (cljc/jit-type a tenv self))) args)
                  (throw (ex-info "jit: mod is integer-only (got a double operand)" {})))
              ta (cljc/jit-tmp-for (nth (vec args) 0) tenv self)
              tb (cljc/jit-tmp-for (nth (vec args) 1) tenv self)]
          (cljc/jit-emit (str dst " = " ta " % " tb ";"))
          (cljc/jit-emit (str "if (" dst " != 0 && ((" dst " < 0) != (" tb " < 0))) "
                              dst " += " tb ";")))
        (contains? #{'min 'max} op)
        (let [ta (cljc/jit-tmp-for (nth (vec args) 0) tenv self)
              tb (cljc/jit-tmp-for (nth (vec args) 1) tenv self)
              cmp (if (= op 'min) " < " " > ")]
          (cljc/jit-emit (str dst " = " ta cmp tb " ? " ta " : " tb ";")))
        (and (vector? self) (= :fn (nth self 0)) (= op (nth self 1)))
        (let [ts (mapv (fn [a] (cljc/jit-tmp-for a tenv self)) args)]
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
    (let [name-sym (first src)
          params (second src)
          body (rest (rest src))
          cn (cljc/cname fname)
          ptypes (mapv (fn [p] (or (cljc/jit-hint p) :long)) params)
          tenv0 (into {} (map vector (seq params) (seq ptypes)))
          ;; return type: ^double/^long on the fn name, else inferred from the
          ;; body so the compiled fn matches the interpreted one. Inference
          ;; runs twice for self-recursion: if the body types :double under a
          ;; :long self-call assumption, promoting the assumption is final
          ;; (the type lattice is monotone, so a second pass reaches fixpoint).
          ret (or (cljc/jit-hint name-sym)
                  (let [body-type (fn [guess]
                                    (cljc/jit-type (last body) tenv0
                                                   [:fn fname (vec params) ptypes guess]))
                        t1 (body-type :long)]
                    (if (= t1 :double) (body-type :double) t1)))
          tenv tenv0
          self [:fn fname (vec params) ptypes ret]]
      (cljc/jit-emit (str "static " (cljc/jit-ctype ret) " impl_" cn "("
                          (str/join ", " (map (fn [p t] (str (cljc/jit-ctype t) " " (cljc/cname p)))
                                              (seq params) (seq ptypes)))
                          ") {"))
      (cljc/jit-emit (str (cljc/jit-ctype ret) " result;"))
      ;; fn-level recur: params are the loop vars
      (cljc/jit-emit "for (;;) {")
      (doseq [b (butlast body)]
        (cljc/jit-tmp-for b tenv self))
      (cljc/jit-expr (last body) "result" tenv self)
      (cljc/jit-emit "break; }")
      (cljc/jit-emit "return result; }")
      (let [code (str "#include <math.h>\n"
                      cljc/ffi-api-decl
                      (str/join "\n" @cljc/jit-lines) "\n"
                      "static void *w_" cn "(void *env, void *args, int nargs) {\n"
                      "  (void)env;\n"
                      "  if (nargs != " (count params) ") api->error(\"" fname ": wrong arity\");\n"
                      "  return api->" (if (= ret :double) "mk_double" "mk_int") "(impl_" cn "("
                      (str/join ", " (map (fn [i t]
                                            (str "api->" (if (= t :double) "as_double" "as_int")
                                                 "(api->nth_arg(args, " i "))"))
                                          (range (count params)) (seq ptypes)))
                      "));\n}\n"
                      "void cljc_module_init(void *env, CljcFfiApi *a) { api = a;\n"
                      "  api->def_native(env, \"" fname "\", w_" cn ");\n}\n")]
        (cljc/ffi-build code "-lm")
        fname))))
