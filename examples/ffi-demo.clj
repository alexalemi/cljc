;; ffi-demo.clj — call real C functions from cljc through the FFI.
;;
;; Showcases cljc's signature feature: you declare C function signatures as
;; DATA, and (ffi/define ...) generates the marshalling glue, compiles a tiny
;; shared library with `cc`, dlopens it, and binds each name as a cljc fn.
;; Modules are cached by content hash, so the first run compiles (~100ms) and
;; later runs just dlopen.
;;
;; Requires a C compiler (cc) on PATH at runtime — that's the FFI's trade.
;;
;; Run:  cljc examples/ffi-demo.clj

;; clj-kondo can't see runtime-defined FFI names; declare them like libc.clj.
(declare c-hypot c-tgamma c-pow getpid strlen)

;; Bind two clusters: libm math functions and a couple of libc functions.
;; Types: :int :double :string :void :pointer (pointer travels as int64).
(ffi/define
  [[:double hypot  [:double :double]]
   [:double tgamma [:double]]                 ; the Gamma function: (n-1)! for ints
   [:double pow    [:double :double]]]
  {:headers ["math.h"] :libs "-lm"})

(ffi/define
  [[:int    getpid []]
   [:int    strlen [:string]]]               ; size_t fits in :int for short strings
  {:headers ["unistd.h" "string.h"]})

(println "Calling into C:")
(println (format "  hypot(3, 4)   = %.1f      (native libm)" (hypot 3.0 4.0)))
(println (format "  pow(2, 10)    = %.0f    (native libm)" (pow 2.0 10.0)))
(println (format "  tgamma(6)     = %.0f     (== 5! = 120)" (tgamma 6.0)))
(println (format "  getpid()      = %d   (this process, from libc)" (getpid)))
(println (format "  strlen(\"cljc\") = %d        (libc counted the bytes)" (strlen "cljc")))

(println)
(println "Each name above is a real C symbol the runtime compiled glue for and")
(println "dlopen'd at startup — no interpreter overhead inside the call itself.")
