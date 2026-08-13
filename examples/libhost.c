/* libhost.c — call a cljc script compiled to a shared library from C.
 *
 *   cljc bundle --library greet.clj libgreet.so     # + libgreet.so.h
 *   cc -O2 libhost.c ./libgreet.so -o libhost
 *   LD_LIBRARY_PATH=. ./libhost
 *
 * The library exports three functions (see the generated header):
 *   cljc_lib_init()        runs the embedded script (idempotent; 0 = ok)
 *   cljc_lib_eval(src)     evals a string, returns pr-str of the last form
 *                          (owned by the library, valid until the next call),
 *                          or NULL on error
 *   cljc_lib_last_error()  message for the last failed call
 *
 * One interpreter per process, single-threaded — same rules as embedding
 * cljc.c directly. Works the same from C++, Rust (extern "C"), Zig, etc.
 */
#include <stdio.h>

int cljc_lib_init(void);
const char *cljc_lib_eval(const char *src);
const char *cljc_lib_last_error(void);

int main(void) {
    if (cljc_lib_init() != 0) {
        fprintf(stderr, "init failed: %s\n", cljc_lib_last_error());
        return 1;
    }
    printf("greet => %s\n", cljc_lib_eval("(greet \"world\")"));
    printf("data  => %s\n", cljc_lib_eval("(into (sorted-map) {:b [1 2] :a \"x\"})"));

    const char *bad = cljc_lib_eval("(nosuchfn 1)");
    if (!bad) printf("error => %s\n", cljc_lib_last_error());

    printf("still => %s\n", cljc_lib_eval("(greet \"again\")"));  /* errors don't kill it */
    return 0;
}
