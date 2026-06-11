/* host.c — embedding example for cljc.
 *
 * The embedding model is stb-style: define CLJC_NO_MAIN and #include
 * cljc.c directly into one translation unit. This gives your native
 * functions access to the interpreter's internals (value constructors
 * like mk_int/mk_str, the args-list accessors, NIL, ...) which is
 * exactly what you need to exchange values across the boundary.
 *
 * Build:  cc -O2 -Wall -o host examples/host.c -lm   (from the repo root)
 */

#include <unistd.h>   /* getpid, for the demo native */

#define CLJC_NO_MAIN
#include "../cljc.c"

/* C-side state that the script mutates through a native function. */
static int total_score = 0;

/* (add-score! n) — adds n to the C-side total, returns the new total. */
static Cljc *native_add_score(CljcEnv *env, Cljc *args) {
    (void)env;
    total_score += (int)as_int(args->as.cons.head, "add-score!");
    return mk_int(total_score);
}

/* (host-info) — returns a cljc map built from C. */
static Cljc *native_host_info(CljcEnv *env, Cljc *args) {
    (void)env; (void)args;
    Cljc *m = mk_map();
    m = map_assoc(m, mk_kw(intern("pid", 3)), mk_int((int64_t)getpid()));
    m = map_assoc(m, mk_kw(intern("lang", 4)), mk_str("C", 1));
    return m;
}

int main(int argc, char **argv) {
    (void)argv;
    cljc_set_stack_base(&argc);   /* required: GC scans the C stack for roots */
    CljcEnv *env = cljc_new_env();

    cljc_define_native(env, "add-score!", native_add_score);
    cljc_define_native(env, "host-info",  native_host_info);

    /* Script calls into C ... */
    cljc_eval_string(env,
        "(doseq [s [10 25 7]] (add-score! s))"
        "(println \"script sees total:\" (add-score! 0))"
        "(println \"host info:\" (host-info))");

    /* ... and C reads the result back. */
    printf("C sees total: %d\n", total_score);

    /* Evaluate an expression and consume the value in C. */
    Cljc *result = cljc_eval_string(env, "(reduce + (map (fn [x] (* x x)) (range 10)))");
    printf("sum of squares, computed in cljc, printed from C: ");
    cljc_print(result);
    printf("\n");
    return 0;
}
