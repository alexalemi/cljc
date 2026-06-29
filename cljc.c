/* cljc.c — a small Clojure-in-C, single-file embeddable interpreter.
 *
 * Build REPL:   cc -O2 -Wall -o cljc cljc.c
 * Embed:        compile with -DCLJC_NO_MAIN and link cljc.o into your program;
 *               see the public API block below.
 *
 * Memory: mark-and-sweep GC over block-pooled cells and environments, with
 * conservative C-stack scanning for roots (so interpreter C locals are safe
 * without explicit root registration). Embedders: call cljc_set_stack_base
 * near the top of the thread's stack before evaluating, and do not hold Cljc
 * pointers across evaluations unless they are reachable from the root env.
 * Assumes a downward-growing stack (x86/ARM/RISC-V).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <math.h>
#include <time.h>
#include <unistd.h>      /* isatty, close — REPL detection, tcp primitives */
#include <sys/stat.h>    /* stat — file mtimes (clerk file watching) */
#include <dirent.h>      /* opendir — directory walking (clerk dir mode) */
#ifdef _WIN32
/* ── Windows portability shims ──
 * mingw-w64 ships unistd/sys/stat/dirent, but not the BSD sockets, poll,
 * setrlimit, or dlopen families. Map them onto winsock2 and the Win32
 * loader so the whole file compiles; features that can't be emulated
 * cheaply (nREPL fd-as-socket, raw-mode line editor) degrade at their
 * call sites. Bundles (-DCLJC_NO_MAIN) only need the socket + dlopen
 * shims; the rest is for a native cljc.exe build. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>    /* WSAPoll, inet_pton */
#include <windows.h>     /* LoadLibraryA / GetProcAddress for ffi-load* */
#undef TRUE              /* windows.h defines these as 1/0; cljc uses them */
#undef FALSE             /* as the names of its boolean singleton globals  */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* no SIGPIPE on Windows; nothing to suppress */
#endif
#define poll       WSAPoll
#define sock_close closesocket
#define popen      _popen
#define pclose     _pclose
#define WEXITSTATUS(s) (s)            /* _pclose already returns the exit code */
#define RTLD_NOW   0
#define RTLD_LOCAL 0
static void *dlopen(const char *path, int flags) {
    (void)flags; return (void *)LoadLibraryA(path);
}
static void *dlsym(void *h, const char *sym) {
    return (void *)(uintptr_t)GetProcAddress((HMODULE)h, sym);
}
static int dlclose(void *h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static const char *dlerror(void) { return "dynamic load failed"; }
static int cljc_wsa_init(void) {   /* idempotent winsock startup */
    static int done = 0;
    if (!done) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1; done = 1; }
    return 0;
}
#else
#include <sys/socket.h>  /* tcp primitives (clerk notebook server) */
#include <netinet/in.h>
#include <arpa/inet.h>   /* inet_addr — tcp/listen host binding */
#include <poll.h>
#include <sys/resource.h>  /* setrlimit — main() raises the stack ceiling */
#include <dlfcn.h>          /* dlopen — FFI module loading */
#include <ucontext.h>       /* makecontext/swapcontext — coroutine primitive */
#define CLJC_HAVE_CORO 1
#define sock_close close
#define cljc_wsa_init() 0
#endif  /* _WIN32 */

#define CLJC_VERSION "0.1.0"
#ifndef CLJC_SHAREDIR
#define CLJC_SHAREDIR "/usr/local/share/cljc"
#endif

/* Interpreter output streams — swappable so the nREPL server can capture
 * println/error output into protocol messages. NULL means stdout/stderr. */
static FILE *cljc_out, *cljc_err;
#define COUT (cljc_out ? cljc_out : stdout)
#define CERR (cljc_err ? cljc_err : stderr)

/* ───── Value representation ─────────────────────────────────────────── */

typedef enum {
    CLJC_NIL,
    CLJC_BOOL,
    CLJC_INT,
    CLJC_DOUBLE,
    CLJC_SYMBOL,    /* interned */
    CLJC_KEYWORD,   /* interned, stored without leading ':' */
    CLJC_STRING,
    CLJC_CHAR,      /* a single character — codepoint in as.chr (immediate, no GC) */
    CLJC_LIST,      /* singly-linked cons; CLJC_NIL terminates */
    CLJC_VECTOR,
    CLJC_MAP,       /* v0: assoc-array with copy-on-write; HAMT later */
    CLJC_FN,        /* interpreted */
    CLJC_NATIVE,    /* C function */
    CLJC_ATOM,      /* mutable box: (atom x), @a, swap!, reset! */
    CLJC_LAZY,      /* lazy seq: thunk forced once, result cached */
    CLJC_TVEC,      /* transient vector: in-place edits, finalized by persistent! */
    CLJC_SET,       /* persistent set: a HAMT keyed by its elements (uses as.map) */
    CLJC_HNODE,     /* internal: HAMT tree node — never user-visible */
    CLJC_RECUR,     /* sentinel: (recur args...) — bubbles to enclosing loop */
    CLJC_CHUNK,     /* internal: compiled bytecode for one fn arity body */
    CLJC_CORO,      /* stackful coroutine (coro/new): own C stack + vstack segment */
    CLJC_FREE,      /* internal: swept cell on the free list — never user-visible */
    CLJC_EMPTY,     /* the empty list () — a distinct singleton: truthy, seq?/list?
                     * true, (= () nil) false, but seq/to_seq of it is NIL so the
                     * seq machinery and cons-traversal still terminate cleanly */
    CLJC_BIGINT,    /* arbitrary-precision integer: sign + base-2^32 magnitude.
                     * Only exists when a value exceeds int64 — results that fit
                     * demote back to CLJC_INT, so the common case never allocates */
    CLJC_RATIO,     /* exact rational num/den (each INT or BIGINT), den>0, gcd-
                     * reduced, den!=1 (den==1 demotes to the integer) */
    CLJC_VAR,       /* a Var: a named reference to a global binding. Derefs to the
                     * binding's current value, is IFn (calls the value), and
                     * carries {:name :ns} metadata — what (resolve sym) returns */
    CLJC_SORTED,    /* sorted set/map: comparator-ordered. A persistent weight-
                     * balanced tree (CLJC_TNODE root) + the comparator (NIL =
                     * default). Key identity is "cmp == 0", not =, so custom
                     * comparators collapse cmp-equal keys. O(log n) everything. */
    CLJC_TNODE,     /* internal: a node of a sorted collection's weight-balanced
                     * tree (Adams' tree). Holds key, val (set: val==key; map: the
                     * value), left/right subtrees, and subtree size. Not user-
                     * visible. */
} CljcTag;

typedef struct Cljc Cljc;
typedef struct CljcEnv CljcEnv;
typedef struct Binding Binding;
typedef Cljc *(*CljcNativeFn)(CljcEnv *env, Cljc **argv, int nargs);

struct Cljc {
    CljcTag tag;
    uint8_t gcmark;
    Cljc *meta;     /* metadata map or NULL; ignored by equality/hash */
    union {
        bool b;
        int64_t i;
        int32_t chr;    /* CLJC_CHAR codepoint */
        double d;
        const char *sym;
        /* Wider view of the same symbol cell: name aliases .sym; root_cache
         * memoizes the resolved ROOT binding (stable — root def mutates). */
        struct { const char *name; Binding *root_cache; const char *home_ns;
                 bool plain; } symc;  /* plain: an unqualified, non-special, non-dot
                                       * call head — skip the dispatch cascade */
        const char *kw;
        char *str;
        struct { Cljc *head; Cljc *tail; } cons;
        /* Persistent vector: 32-way position trie + tail of the last ≤32
         * elements. tail is owned by THIS cell (copied per derived vector);
         * tree nodes are shared CLJC_HNODE cells. root NULL → all in tail. */
        /* vec doubles as the transient view (CLJC_TVEC): edit_id marks tree
         * nodes this transient owns (may mutate); alive flips off at
         * persistent!. Transient tails are full 32-cap chunk buffers. */
        struct { Cljc *root; Cljc **tail; uint32_t count; uint32_t edit_id;
                 uint8_t shift; uint8_t taillen; bool alive; } vec;
        /* HAMT persistent map: root tree node (NULL when empty) + entry count */
        struct { Cljc *root; size_t count; } map;
        /* sorted set/map: root of a weight-balanced tree (NIL when empty); cmp is
         * the comparator fn (NIL = default cmp_values). */
        struct { Cljc *root; Cljc *cmp; bool is_map; } sorted;
        /* a weight-balanced tree node: left/right subtrees, the comparison key,
         * its value (set: == key; map: the value), and subtree node count. */
        struct { Cljc *left; Cljc *right; Cljc *key; Cljc *val; uint32_t size; } tnode;
        /* HAMT node. kids interleaves [k1,v1,k2,v2...]; k==NULL → v is a
         * subnode. Collision nodes hold same-hash entries linearly. */
        struct { Cljc **kids; uint32_t bitmap; uint32_t chash; uint16_t nkids;
                 bool collision; uint32_t edit_id; } hnode;
        /* arities: list of (params-list . body-list) pairs; dispatch by argc.
         * fast-call cache (lazy, filled on first apply): fc_arity is the lone
         * fixed arity whose params are all simple symbols, count fc_n <=
         * ENV_SLOTS — lets apply skip arity_info + bind_params/destructure and
         * fill the call frame's slots directly. NULL => always use slow path. */
        struct { Cljc *arities; CljcEnv *env; bool is_macro;
                 bool fc_ready; uint8_t fc_n; Cljc *fc_arity; } fn;
        CljcNativeFn native;
        struct { Cljc *value; } atom;
        /* CLJC_BIGINT: sign in {-1,+1}; mag is n little-endian base-2^32 limbs,
         * mag[n-1] != 0 (never stored as zero — zero demotes to CLJC_INT). */
        struct { int sign; uint32_t n; uint32_t *mag; } big;
        struct { Cljc *num; Cljc *den; } ratio;   /* CLJC_RATIO */
        struct { const char *name; } var;          /* CLJC_VAR: canonical binding name */
        struct { Cljc *thunk; Cljc *cached; bool done; } lazy;
        /* recur sentinel: up to 3 values inline (covers real loops);
         * wider recurs spill to a heap array stored in iv[0]. */
        struct { Cljc *iv[3]; uint8_t n; bool spill; } recur;
        struct { uint32_t *code; Cljc **consts;
                 uint32_t ncode; uint16_t nconst; } chunk;
        struct Coro *coro;   /* CLJC_CORO: heap-allocated coroutine state */
    } as;
};

/* ───── Environment (lexical scope, linked frames) ───────────────────── */

struct Binding {
    const char *name;   /* interned symbol pointer — compare by == */
    Cljc *value;
    struct Binding *next;
};

/* Non-root envs bind into inline slots first (fn params, let locals) —
 * one Binding allocation per param was ~18% of hot-loop runtime. The
 * ROOT env never uses slots: root Binding* must stay stable for the
 * per-symbol root_cache, and def mutates bindings in place. */
#define ENV_SLOTS 4
struct CljcEnv {
    Binding *bindings;  /* overflow chain — and ALL root bindings */
    CljcEnv *parent;    /* doubles as the free-list next pointer when swept */
    uint8_t gcmark;
    uint8_t gcfree;
    uint8_t nslots;
    const char *sname[ENV_SLOTS];
    Cljc *sval[ENV_SLOTS];
};

/* ───── Forward declarations ─────────────────────────────────────────── */

static Cljc *eval(CljcEnv *env, Cljc *form);
static Cljc *read_form(const char **src);
static void print(Cljc *v);
static const char *intern(const char *s, size_t n);
static bool cljc_eq(Cljc *a, Cljc *b);
static bool map_find(Cljc *m, Cljc *key, Cljc **out);
static double as_num(Cljc *v);
static Cljc *big_from_decimal(const char *s);
static char *big_to_decimal(Cljc *v);
static Cljc *make_ratio(Cljc *num, Cljc *den);
static Cljc *to_seq(Cljc *v);
static Cljc *seq1(Cljc *v);
static Cljc *seq1_slot(Cljc **slot);
static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc **argv, int nargs);
static Cljc *sorted_entry_list(Cljc *coll);
static bool sorted_get(CljcEnv *env, Cljc *coll, Cljc *key, Cljc **out);
static Cljc *sorted_put(CljcEnv *env, Cljc *coll, Cljc *key, Cljc *val);
static Cljc *sorted_remove(CljcEnv *env, Cljc *coll, Cljc *key);
static bool sorted_contains(CljcEnv *env, Cljc *coll, Cljc *key);
static uint32_t sorted_count(Cljc *coll);
static Cljc *mk_map_entry(Cljc *k, Cljc *v);
static Cljc *prim_conj(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_assoc(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_dissoc(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_disj(CljcEnv *env, Cljc **argv, int nargs);
/* GC-rooted value stack: call arguments live here (heap argv buffers would
 * be invisible to the conservative stack scan). Frames push, call, restore. */
#define VSTACK_CAP (1u << 20)
static Cljc **vstack;
static size_t vsp;
static size_t vstack_cap = VSTACK_CAP;   /* swapped to the active coro's cap */
static void vpush(Cljc *v);
void cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn);
/* Namespace aliases for the flat-global model: (require '[x.y :as m])
 * registers "m"; on lookup miss, m/foo retries as bare foo. */
#define MAX_ALIASES 8192
static const char *alias_table[MAX_ALIASES];   /* alias prefix */
static const char *alias_ns[MAX_ALIASES];      /* full namespace it names */
static const char *alias_owner[MAX_ALIASES];   /* ns that registered it (per-ns
                                                * aliases): NULL = REPL/global */
static int n_aliases;
/* Resolve an alias prefix to its target ns, preferring the one registered in the
 * referencing symbol's OWN namespace (home_ns) — so a macro defined in ns A whose
 * body says c/foo finds A's `c`, not some other ns's `c` from the flat table.
 * Falls back to any-owner so REPL/global aliases still work. */
static const char *alias_lookup(const char *prefix, const char *home_ns) {
    if (home_ns)
        for (int i = 0; i < n_aliases; i++)
            if (alias_table[i] == prefix && alias_owner[i] == home_ns) return alias_ns[i];
    for (int i = 0; i < n_aliases; i++)
        if (alias_table[i] == prefix) return alias_ns[i];
    return NULL;
}

/* Symbol-level refer aliases: (require '[x.y :refer [v]]) inside (ns a.b)
 * registers "a.b/v" -> "x.y/v" so all referrers resolve to the ONE source
 * global. Unlike a value copy, this preserves shared identity — essential for
 * a dynamic var that one ns `binding`s and another reads. */
#define MAX_REFERS 4096
static const char *refer_from[MAX_REFERS];     /* "<referring-ns>/name" */
static const char *refer_to[MAX_REFERS];       /* "<source-ns>/name"    */
static int n_refers;

static Cljc *cell_alloc(bool zero);
static CljcEnv *env_alloc(void);
static Cljc **chunk32_alloc(void);
static void chunk32_free(Cljc **p);
static Cljc **tail_alloc(uint8_t n);
static void tail_free(Cljc **p, uint8_t n);
static size_t vec_len(Cljc *v);
static Cljc *vec_nth(Cljc *v, size_t i);
static char *as_str(Cljc *v, const char *what);
static int64_t as_int(Cljc *v, const char *what);
static Cljc *NIL, *TRUE, *FALSE, *EMPTY;

/* ───── Error handling ───────────────────────────────────────────────── */

/* Errors unwind to the innermost handler frame — pushed by the `try`
 * special form — or, with no try in flight, to the top-level err_jmp set by
 * the REPL/script/eval-string entry points. The exception itself is a value:
 * cur_exc when (throw x) raised it, or NULL meaning "use err_msg" for
 * interpreter-raised errors. */

typedef struct ErrFrame { jmp_buf jb; struct ErrFrame *prev; size_t vsp_save; int esp_save; } ErrFrame;
#define EVAL_STACK_MAX 4096
static Cljc *eval_stack[EVAL_STACK_MAX];
static int eval_sp;
static char err_trace[1536];
static const char *err_src_text;   /* retained main-script source */
static const char *err_src_name;   /* its display name */
static long long err_line = -1;    /* innermost located frame at raise */
static const char *err_token;      /* offending symbol, when known */
static long long err_col = -1;
static ErrFrame *err_top;
static jmp_buf err_jmp;
static char err_msg[256];
static Cljc *cur_exc;   /* exception value in flight; a GC root */

/* ───── Coroutines (stackful, ucontext-backed) ───────────────────────────
 * A Coro owns its own C stack; the interpreter runs on it normally (eval
 * recurses), and coro/yield does a swapcontext back to whoever resumed it.
 * Single-threaded + cooperative, so no locks. The fiddly parts are all about
 * the GC and the few interpreter globals that are really "per execution":
 *   - each coro has its OWN value stack (a stable array). The interpreter
 *     caches absolute pointers into the active vstack (argv = &vstack[base]),
 *     so a coro must always resume onto the SAME array — sharing one vstack and
 *     relocating operands broke those pointers (only when a yield happened deep
 *     in nested calls). We swap the global vstack/vsp/vstack_cap on every switch.
 *   - err_top / cur_exc / eval_sp are saved/restored per switch; each coro
 *     installs its own base ErrFrame so a throw never longjmps across stacks.
 *   - the conservative GC scans each reachable suspended coro's live C-stack
 *     range + its saved register blob (the ucontext) + its own vstack. */
#ifdef CLJC_HAVE_CORO
typedef enum { CORO_NEW, CORO_SUSPENDED, CORO_RUNNING, CORO_NORMAL, CORO_DEAD } CoroStatus;
typedef struct Coro {
    Cljc *self;            /* the CLJC_CORO cell wrapping this (GC back-ref) */
    Cljc *thunk;           /* zero-arg fn run by the body (GC root) */
    Cljc *xfer;            /* value handed across resume/yield (GC root) */
    Cljc *error;           /* value of an uncaught throw, else NULL (GC root) */
    ucontext_t ctx;        /* machine context saved while suspended */
    char *stack;           /* malloc'd stack (low address) */
    size_t stack_size;
    char *stack_top;       /* stack + stack_size (high address) */
    void *saved_sp;        /* approx SP at last switch-away — GC scan low bound */
    CoroStatus status;
    struct Coro *resumer;  /* who resumed us; NULL = resumed by main */
    /* Each coro has its OWN value stack — a stable allocation. The interpreter
     * caches absolute pointers into the active vstack (argv = &vstack[base]),
     * so a coro must always resume onto the SAME array; sharing one vstack and
     * relocating segments broke those pointers. We swap the global vstack/vsp/
     * vstack_cap to the active coro's on every context switch. */
    Cljc **s_vstack;       /* the coro's vstack array (saved globals while away) */
    size_t s_vsp;
    size_t s_vstack_cap;
    ErrFrame *s_err_top;   /* saved err_top while suspended */
    Cljc *s_cur_exc;
    int s_eval_sp;
} Coro;

static Coro *coro_current;        /* NULL = main is the active context */
static ucontext_t coro_main_ctx;  /* main's context saved while a coro runs */
static void *coro_main_saved_sp;  /* main's SP at the point it entered coro-land */
/* main's execution state saved while a coro runs (see state_save/state_load) */
static ErrFrame *main_s_err_top;
static Cljc *main_s_cur_exc;
static int main_s_eval_sp;
static Cljc **main_s_vstack;
static size_t main_s_vsp;
static size_t main_s_vstack_cap;
static void coro_mark_scan(Coro *c);   /* GC: scan a coro's roots + stack */
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void cljc_raise(void) {
    if (err_top) longjmp(err_top->jb, 1);
    longjmp(err_jmp, 1);
}

static void trace_snapshot(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
static void cljc_error(const char *fmt, ...) {
    cur_exc = NULL;  /* message-style error */
    /* err_token persists only when the caller set it just before */
    va_list ap; va_start(ap, fmt);
    vsnprintf(err_msg, sizeof err_msg, fmt, ap);
    va_end(ap);
    trace_snapshot();
    cljc_raise();
}

/* A short human description of a value's type, for friendlier error messages
 * ("expected a number, got a keyword"). */
static const char *val_type_name(Cljc *v) {
    if (v == NIL || v == NULL) return "nil";
    switch (v->tag) {
        case CLJC_BOOL:    return "a boolean";
        case CLJC_INT: case CLJC_BIGINT: return "an integer";
        case CLJC_DOUBLE:  return "a float";
        case CLJC_RATIO:   return "a ratio";
        case CLJC_STRING:  return "a string";
        case CLJC_CHAR:    return "a character";
        case CLJC_KEYWORD: return "a keyword";
        case CLJC_SYMBOL:  return "a symbol";
        case CLJC_LIST: case CLJC_EMPTY: return "a list";
        case CLJC_LAZY:    return "a lazy seq";
        case CLJC_VECTOR:  return "a vector";
        case CLJC_TVEC:    return "a transient vector";
        case CLJC_MAP:     return "a map";
        case CLJC_SET:     return "a set";
        case CLJC_SORTED:  return v->as.sorted.is_map ? "a sorted map" : "a sorted set";
        case CLJC_FN: case CLJC_NATIVE: return "a function";
        case CLJC_ATOM:    return "an atom";
        case CLJC_VAR:     return "a var";
        case CLJC_CORO:    return "a coroutine";
        default:           return "a value";
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void cljc_throw_value(Cljc *v) {
    cur_exc = v;
    snprintf(err_msg, sizeof err_msg, "uncaught exception");
    trace_snapshot();
    cljc_raise();
}

/* ───── C-stack depth guard ──────────────────────────────────────────────
 * Exceptions here are setjmp/longjmp, which CANNOT recover from a real C
 * stack overflow — that's a SIGSEGV, and the process just dies. So before
 * each function application we check how much C-stack headroom is left and,
 * while there's still room to unwind, raise an ordinary catchable error
 * instead. This is what lets the nREPL / notebook report "stack overflow"
 * rather than crash on runaway recursion. It matters most for csp
 * coroutines: each go block runs on a fixed CORO_STACK_SIZE (1 MiB) C stack,
 * far smaller than the main thread's (which main() raises toward 1 GiB), so
 * the value-stack cap — a proxy for depth tuned to the main stack — never
 * trips before the coro's real C stack is exhausted. */
#define STACK_SAFETY_MARGIN (96u * 1024)   /* keep this much headroom free */
static const char *main_stack_floor;       /* lowest safe SP on the main stack */

static void stack_floor_init(const char *base) {
    size_t budget = 8u * 1024 * 1024;       /* assume 8 MiB if rlimit unknown */
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
        && rl.rlim_cur > STACK_SAFETY_MARGIN)
        budget = (size_t)rl.rlim_cur;
#endif
    main_stack_floor = base - budget + STACK_SAFETY_MARGIN;
}

static void cljc_check_stack(void) {
    char probe;
    const char *sp = &probe;               /* approximate current stack pointer */
    const char *floor;
#ifdef CLJC_HAVE_CORO
    if (coro_current)                       /* coro: stack is [stack, stack_top) */
        floor = coro_current->stack + STACK_SAFETY_MARGIN;
    else
#endif
        floor = main_stack_floor;
    if (floor && sp < floor) cljc_error("stack overflow");
}

static void vpush(Cljc *v) {
    if (vsp >= vstack_cap) cljc_error("value stack overflow");
    vstack[vsp++] = v;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) cljc_error("out of memory");
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);   /* old block survives an OOM longjmp */
    if (!q) { free(p); cljc_error("out of memory"); }
    return q;
}

/* ───── Constructors ─────────────────────────────────────────────────── */

static Cljc *alloc(CljcTag t) {
    Cljc *v = cell_alloc(t != CLJC_INT && t != CLJC_DOUBLE && t != CLJC_CHAR);
    v->tag = t;
    return v;
}

/* Small ints are preallocated outside the GC pools (immortal, childless,
 * never swept) — loop counters and small arithmetic allocate nothing. */
#define SMALLINT_MIN (-128)
#define SMALLINT_MAX 1023
static Cljc smallints[SMALLINT_MAX - SMALLINT_MIN + 1];

static Cljc *mk_int(int64_t i) {
    if (i >= SMALLINT_MIN && i <= SMALLINT_MAX) return &smallints[i - SMALLINT_MIN];
    Cljc *v = alloc(CLJC_INT);
    v->as.i = i;
    return v;
}
static Cljc *mk_double(double d)     { Cljc *v = alloc(CLJC_DOUBLE); v->as.d = d; return v; }
/* Byte-valued chars (0..255) are preallocated and immortal — string seqs
 * (which yield one char per byte) then allocate nothing. */
static Cljc smallchars[256];
static Cljc *mk_char(int32_t cp) {
    if (cp >= 0 && cp < 256) return &smallchars[cp];
    Cljc *v = alloc(CLJC_CHAR);
    v->as.chr = cp;
    return v;
}
static Cljc *mk_bool(bool b)         { return b ? TRUE : FALSE; }
static const char *cur_reader_ns;   /* set while require loads a library */
static Cljc *mk_sym(const char *s) {
    Cljc *v = alloc(CLJC_SYMBOL);
    v->as.sym = s;
    v->as.symc.home_ns = cur_reader_ns;  /* NULL outside library loads */
    return v;
}
/* Keyword CELLS are interned too (like Clojure): one immortal cell per
 * name, allocated outside the GC pools — so (identical? :a :a) holds and
 * keyword-heavy code allocates nothing per read. */
typedef struct KwNode { const char *name; Cljc *cell; struct KwNode *next; } KwNode;
static KwNode *kw_table[256];

static Cljc *mk_kw(const char *s) {   /* s is already interned */
    uint32_t h = (uint32_t)((uintptr_t)s >> 4) & 255u;
    for (KwNode *n = kw_table[h]; n; n = n->next)
        if (n->name == s) return n->cell;
    Cljc *v = xmalloc(sizeof *v);     /* immortal: never in a pool, never swept */
    memset(v, 0, sizeof *v);
    v->tag = CLJC_KEYWORD;
    v->gcmark = 1;
    v->as.kw = s;
    KwNode *n = xmalloc(sizeof *n);
    n->name = s; n->cell = v; n->next = kw_table[h];
    kw_table[h] = n;
    return v;
}
static size_t gc_extra_bytes;   /* string buffer bytes since last GC —
                                 * a 1MB string is ONE gc_allocs tick, so
                                 * string churn must trigger by bytes too
                                 * (a 17GB runaway found the gap) */

static Cljc *mk_str(const char *s, size_t n) {
    Cljc *v = alloc(CLJC_STRING);
    gc_extra_bytes += n;
    v->as.str = xmalloc(n + 1);
    memcpy(v->as.str, s, n);
    v->as.str[n] = '\0';
    return v;
}
static Cljc *mk_cons(Cljc *h, Cljc *t) {
    Cljc *v = alloc(CLJC_LIST);
    v->as.cons.head = h;
    v->as.cons.tail = t;
    return v;
}
static Cljc *mk_native(CljcNativeFn fn) {
    Cljc *v = alloc(CLJC_NATIVE);
    v->as.native = fn;
    return v;
}

static bool is_truthy(Cljc *v) {
    /* Clojure: only nil and false are falsy. */
    if (v == NIL) return false;
    if (v->tag == CLJC_BOOL && !v->as.b) return false;
    return true;
}

static size_t list_len(Cljc *l) {
    size_t n = 0;
    while (l && l->tag == CLJC_LIST) { n++; l = l->as.cons.tail; }
    return n;
}

/* ───── Symbol/keyword interning ─────────────────────────────────────── */

/* Tiny chained hash table. Pointers returned here are stable for the
 * lifetime of the interpreter, so symbol equality is pointer equality. */

#define INTERN_BUCKETS 1024
typedef struct InternNode { const char *str; struct InternNode *next; } InternNode;
static InternNode *intern_table[INTERN_BUCKETS];

static uint32_t fnv1a(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static const char *intern(const char *s, size_t n) {
    uint32_t h = fnv1a(s, n) % INTERN_BUCKETS;
    for (InternNode *node = intern_table[h]; node; node = node->next) {
        if (strlen(node->str) == n && memcmp(node->str, s, n) == 0) return node->str;
    }
    char *copy = xmalloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    InternNode *node = xmalloc(sizeof *node);
    node->str = copy;
    node->next = intern_table[h];
    intern_table[h] = node;
    return copy;
}

/* ───── Environment ──────────────────────────────────────────────────── */

static CljcEnv *env_new(CljcEnv *parent) {
    CljcEnv *e = env_alloc();  /* pooled; may trigger a GC */
    e->bindings = NULL;
    e->parent = parent;
    e->nslots = 0;
    return e;
}

/* Find a local in ONE env frame: overflow chain first (newer than any
 * slot — sequential let rebinds shadow), then slots newest-first. */
static inline Cljc **env_local_find(CljcEnv *e, const char *name) {
    for (Binding *b = e->bindings; b; b = b->next)
        if (b->name == name) return &b->value;
    for (int i = (int)e->nslots - 1; i >= 0; i--)
        if (e->sname[i] == name) return &e->sval[i];
    return NULL;
}

/* Bindings are pooled like cells/envs (a fn call allocates one per param —
 * too hot for malloc). Freed bindings recycle through a free list when their
 * env is swept. binding_alloc never triggers GC (env_define runs mid-bind). */
#define BINDING_BLOCK_N 4096
typedef struct BindingBlock { struct BindingBlock *next; size_t used; Binding b[BINDING_BLOCK_N]; } BindingBlock;
static BindingBlock *binding_blocks;
static Binding *binding_freelist;

static Binding *binding_alloc(void) {
    Binding *b;
    if (binding_freelist) {
        b = binding_freelist;
        binding_freelist = b->next;
    } else {
        if (!binding_blocks || binding_blocks->used == BINDING_BLOCK_N) {
            BindingBlock *blk = malloc(sizeof *blk);
            if (!blk) cljc_error("out of memory");
            blk->used = 0;
            blk->next = binding_blocks;
            binding_blocks = blk;
        }
        b = &binding_blocks->b[binding_blocks->used++];
    }
    return b;
}

static void env_define(CljcEnv *env, const char *name, Cljc *value) {
    if (env->parent && env->nslots < ENV_SLOTS) {  /* root: Bindings only */
        env->sname[env->nslots] = name;
        env->sval[env->nslots] = value;
        env->nslots++;
        return;
    }
    Binding *b = binding_alloc();
    b->name = name; b->value = value; b->next = env->bindings;
    env->bindings = b;
}

/* def/defmacro/native install: mutate an existing root binding in place
 * rather than shadowing it. This keeps root Binding pointers stable for
 * the lifetime of the interpreter, which is what makes the per-symbol
 * root_cache sound — a cached binding sees redefinitions through the
 * mutation instead of going stale. */
static void env_define_root(CljcEnv *root, const char *name, Cljc *value) {
    /* While a library loads, its BARE defs land under "ns/name" — isolation
     * from the flat globals (and from each other). An ALREADY-qualified name
     * (e.g. (defn cljc/foo ...) inside an (ns bar) file) keeps its explicit
     * namespace — Clojure semantics — so we don't double-prefix it to
     * bar/cljc/foo. This is what lets a battery carry an (ns ...) header (for
     * babashka/clj compatibility) yet still define its cljc-prefixed helpers
     * in place. */
    /* `/` (the division symbol) is itself a slash but is NOT namespace-qualified
     * — without this a library that redefines / (e.g. a generic-arithmetic ns
     * with :refer-clojure :exclude [/]) would clobber the global core / for
     * everyone instead of isolating it under its own ns. */
    if (cur_reader_ns && strcmp(cur_reader_ns, "user") &&
        (!strchr(name, '/') || !strcmp(name, "/"))) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s", cur_reader_ns, name);
        name = intern(buf, strlen(buf));
    }
    for (Binding *b = root->bindings; b; b = b->next)
        if (b->name == name) { b->value = value; return; }
    env_define(root, name, value);
}

static Cljc *env_lookup_maybe(CljcEnv *env, const char *raw) {
    const char *name = intern(raw, strlen(raw));
    for (CljcEnv *e = env; e; e = e->parent) {
        Cljc **p = env_local_find(e, name);
        if (p) return *p;
    }
    return NULL;
}

static void trace_snapshot(void) {
    err_trace[0] = '\0';
    err_line = -1;
    err_col = -1;
    size_t off = 0;
    int shown = 0;
    for (int i = eval_sp - 1; i >= 0 && shown < 8; i--) {
        Cljc *f = eval_stack[i];
        if (f == NULL || f == NIL || f->tag != CLJC_LIST) continue;
        const char *head = f->as.cons.head->tag == CLJC_SYMBOL
            ? f->as.cons.head->as.sym : "...";
        long long line = -1;
        if (f->meta) {
            Cljc *lv;
            if (map_find(f->meta, mk_kw(intern("line", 4)), &lv) && lv->tag == CLJC_INT)
                line = (long long)lv->as.i;
        }
        int n;
        if (line >= 0 && err_line < 0) {
            err_line = line;
            Cljc *cv;
            if (f->meta && map_find(f->meta, mk_kw(intern("col", 3)), &cv) &&
                cv->tag == CLJC_INT)
                err_col = (long long)cv->as.i;
        }
        if (line >= 0)
            n = snprintf(err_trace + off, sizeof err_trace - off,
                         "  at (%s ...) line %lld\n", head, line);
        else
            n = snprintf(err_trace + off, sizeof err_trace - off,
                         "  at (%s ...)\n", head);
        if (n < 0 || off + (size_t)n >= sizeof err_trace - 1) break;
        off += (size_t)n;
        shown++;
    }
}

static CljcEnv *env_root(CljcEnv *e) {
    while (e->parent) e = e->parent;
    return e;
}

/* ───── Garbage collector ────────────────────────────────────────────── */

/* Mark-and-sweep over two block pools (cells, envs). Roots:
 *   1. NIL/TRUE/FALSE and every registered root env (cljc_new_env)
 *   2. the C stack between the current frame and gc_stack_base, scanned
 *      conservatively — any word that points into a pool block (including
 *      interior pointers) marks the containing object
 *   3. callee-saved registers, flushed onto the stack via setjmp
 * Bindings are owned by their env (freed when the env is swept); a cell's
 * interior arrays (string bytes, vector items, map keys/vals, recur vals)
 * are owned by the cell. Interned symbol text is immortal by design. */

#define CELL_BLOCK_N 8192
typedef struct CellBlock { struct CellBlock *next; size_t used; Cljc cells[CELL_BLOCK_N]; } CellBlock;
static CellBlock *cell_blocks;
static Cljc *cell_freelist;        /* linked through as.cons.head */

#define ENV_BLOCK_N 1024
typedef struct EnvBlock { struct EnvBlock *next; size_t used; CljcEnv envs[ENV_BLOCK_N]; } EnvBlock;
static EnvBlock *env_blocks;
static CljcEnv *env_freelist;      /* linked through ->parent */

/* Floor on allocations between collections. Tight loops with small live
 * sets are collection-bound: at 64k this was thousands of collections on
 * AoC hot loops (~38% of runtime). ~1M cells ≈ tens of MB of slack. */
#define GC_MIN_THRESHOLD (1u << 20)
#define GC_CHURN_CAP (4u << 20)   /* adaptive floor ceiling: ~8M cells (~320MB)
                                   * of garbage between collections, max */
static size_t gc_allocs, gc_threshold = GC_MIN_THRESHOLD;
static size_t churn_floor = GC_MIN_THRESHOLD;   /* adaptive collection floor */
static size_t gc_freed_last;
static bool gc_stress;             /* CLJC_GC_STRESS=1: collect every 512 allocs */
static void *gc_stack_base;
static CljcEnv *gc_root_envs[8];
static int gc_n_root_envs;

static void gc_mark_env(CljcEnv *e);

/* Marking uses an explicit worklist, not C-stack recursion: a 10k-deep
 * closure/env chain (long reduce over lazy pipelines) overflowed the C
 * stack when gc_mark recursed (found by 2017 d16). Cells are marked when
 * PUSHED, so each enters the worklist at most once. */
static Cljc **mark_stack;
static size_t mark_sp, mark_cap;

static void mark_push(Cljc *v) {
    if (!v || v->gcmark || v->tag == CLJC_FREE) return;
    v->gcmark = 1;
    if (mark_sp >= mark_cap) {
        mark_cap = mark_cap ? mark_cap * 2 : 4096;
        mark_stack = realloc(mark_stack, sizeof(Cljc *) * mark_cap);
        if (!mark_stack) { fputs("gc: out of memory\n", stderr); exit(1); }
    }
    mark_stack[mark_sp++] = v;
}

static void mark_env_chain(CljcEnv *e) {
    while (e && !e->gcmark && !e->gcfree) {
        e->gcmark = 1;
        for (Binding *b = e->bindings; b; b = b->next) mark_push(b->value);
        for (int i = 0; i < (int)e->nslots; i++) mark_push(e->sval[i]);
        e = e->parent;
    }
}

static void gc_drain(void) {
    while (mark_sp) {
        Cljc *v = mark_stack[--mark_sp];
        if (v->meta) mark_push(v->meta);
        switch (v->tag) {
            case CLJC_LIST:
                mark_push(v->as.cons.head);
                mark_push(v->as.cons.tail);
                break;
            case CLJC_RATIO:
                mark_push(v->as.ratio.num);
                mark_push(v->as.ratio.den);
                break;
            case CLJC_VECTOR:
            case CLJC_TVEC:
                for (size_t i = 0; i < v->as.vec.taillen; i++) mark_push(v->as.vec.tail[i]);
                mark_push(v->as.vec.root);          /* NULL-safe */
                break;
            case CLJC_MAP:
            case CLJC_SET:
                mark_push(v->as.map.root);          /* NULL-safe */
                break;
            case CLJC_SORTED:
                mark_push(v->as.sorted.root);
                mark_push(v->as.sorted.cmp);
                break;
            case CLJC_TNODE:
                mark_push(v->as.tnode.left);
                mark_push(v->as.tnode.right);
                mark_push(v->as.tnode.key);
                mark_push(v->as.tnode.val);
                break;
            case CLJC_HNODE:
                for (size_t i = 0; i < v->as.hnode.nkids; i++)
                    mark_push(v->as.hnode.kids[i]); /* NULL slots skip */
                break;
            case CLJC_RECUR: {
                Cljc **vals = v->as.recur.spill ? (Cljc **)v->as.recur.iv[0] : v->as.recur.iv;
                for (size_t i = 0; i < v->as.recur.n; i++) mark_push(vals[i]);
                break;
            }
            case CLJC_FN:
                mark_env_chain(v->as.fn.env);
                mark_push(v->as.fn.arities);
                break;
            case CLJC_ATOM:
                mark_push(v->as.atom.value);
                break;
            case CLJC_LAZY:
                mark_push(v->as.lazy.thunk);
                mark_push(v->as.lazy.cached);
                break;
            case CLJC_CHUNK:
                for (uint16_t i = 0; i < v->as.chunk.nconst; i++)
                    mark_push(v->as.chunk.consts[i]);
                break;
#ifdef CLJC_HAVE_CORO
            case CLJC_CORO:
                if (v->as.coro) coro_mark_scan(v->as.coro);
                break;
#endif
            default:
                break;
        }
    }
}

static void gc_mark(Cljc *v) {
    mark_push(v);
    gc_drain();
}

static void gc_mark_env(CljcEnv *e) {
    mark_env_chain(e);
    gc_drain();
}

/* Global pool address range, refreshed at the start of each collection:
 * an O(1) reject for the conservative scan — most stack words are not
 * pool pointers, and the per-word block walk was ~13% of runtime. */
static uintptr_t gc_pool_lo = UINTPTR_MAX, gc_pool_hi;

static void gc_refresh_bounds(void) {
    gc_pool_lo = UINTPTR_MAX;
    gc_pool_hi = 0;
    for (CellBlock *b = cell_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->cells, hi = (uintptr_t)(b->cells + b->used);
        if (lo < gc_pool_lo) gc_pool_lo = lo;
        if (hi > gc_pool_hi) gc_pool_hi = hi;
    }
    for (EnvBlock *b = env_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->envs, hi = (uintptr_t)(b->envs + b->used);
        if (lo < gc_pool_lo) gc_pool_lo = lo;
        if (hi > gc_pool_hi) gc_pool_hi = hi;
    }
}

/* If w points into a pool block (interior pointers included), mark the
 * containing object. */
static void gc_mark_conservative(uintptr_t w) {
    if (w < gc_pool_lo || w >= gc_pool_hi) return;
    for (CellBlock *b = cell_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->cells, hi = (uintptr_t)(b->cells + b->used);
        if (w >= lo && w < hi) { gc_mark(&b->cells[(w - lo) / sizeof(Cljc)]); return; }
    }
    for (EnvBlock *b = env_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->envs, hi = (uintptr_t)(b->envs + b->used);
        if (w >= lo && w < hi) { gc_mark_env(&b->envs[(w - lo) / sizeof(CljcEnv)]); return; }
    }
}

#ifdef CLJC_HAVE_CORO
/* Worklist-safe variants of the conservative scan: they PUSH found objects
 * instead of gc_mark'ing (which would re-enter gc_drain). Used to scan a
 * suspended coroutine's stack from inside the drain loop. */
static void mark_cons_word(uintptr_t w) {
    if (w < gc_pool_lo || w >= gc_pool_hi) return;
    for (CellBlock *b = cell_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->cells, hi = (uintptr_t)(b->cells + b->used);
        if (w >= lo && w < hi) { mark_push(&b->cells[(w - lo) / sizeof(Cljc)]); return; }
    }
    for (EnvBlock *b = env_blocks; b; b = b->next) {
        uintptr_t lo = (uintptr_t)b->envs, hi = (uintptr_t)(b->envs + b->used);
        if (w >= lo && w < hi) { mark_env_chain(&b->envs[(w - lo) / sizeof(CljcEnv)]); return; }
    }
}
static void scan_range_push(void *lo_, void *hi_) {
    uintptr_t lo = ((uintptr_t)lo_ + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
    for (uintptr_t *p = (uintptr_t *)lo; (void *)p < hi_; p++) mark_cons_word(*p);
}

/* Mark a coroutine's roots. For a SUSPENDED coro we also conservatively scan
 * its live C-stack range, its saved registers (the ucontext blob), and its
 * saved vstack segment. The ACTIVE coro's live stack is scanned by gc_collect
 * with the real SP, so we skip the stack scan here for it. */
static void coro_mark_scan(Coro *c) {
    mark_push(c->thunk);
    mark_push(c->xfer);
    mark_push(c->error);
    if (c->resumer) mark_push(c->resumer->self);
    /* The active coro's vstack is the global one (scanned by gc_collect); a
     * suspended coro's operands live in its own saved vstack array. */
    if (c != coro_current && c->s_vstack)
        for (size_t i = 0; i < c->s_vsp; i++) mark_push(c->s_vstack[i]);
    if (c != coro_current &&
        (c->status == CORO_SUSPENDED || c->status == CORO_NORMAL) &&
        c->saved_sp) {
        scan_range_push(c->saved_sp, c->stack_top);          /* live stack */
        scan_range_push(&c->ctx, (char *)&c->ctx + sizeof c->ctx);  /* regs */
    }
}
#endif

/* The conservative scan reads raw stack memory — dead frames, redzones, all
 * of it. That is by design, but it is exactly what AddressSanitizer polices,
 * so exempt this one function. (ASan runs also need
 * ASAN_OPTIONS=detect_stack_use_after_return=0 — fake stacks would move
 * locals off the real stack and genuinely break root scanning.) */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((no_sanitize_address))
#endif
static void gc_scan_range(void *lo_, void *hi_) {
    /* Align up to pointer size; scan every word in [lo, hi). */
    uintptr_t lo = ((uintptr_t)lo_ + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
    for (uintptr_t *p = (uintptr_t *)lo; (void *)p < hi_; p++)
        gc_mark_conservative(*p);
}

static void gc_collect(void) {
    gc_allocs = 0;
    gc_extra_bytes = 0;
    gc_refresh_bounds();   /* O(1) reject range for the conservative scan */

    /* Flush callee-saved registers onto the stack so the scan sees them. */
    jmp_buf regs;
    setjmp(regs);

    gc_mark(NIL); gc_mark(TRUE); gc_mark(FALSE); gc_mark(EMPTY);
    gc_mark(cur_exc);  /* exception value may be in flight between throw and catch */
    for (size_t vi = 0; vi < vsp; vi++) gc_mark(vstack[vi]);
    for (int i = 0; i < gc_n_root_envs; i++) gc_mark_env(gc_root_envs[i]);

    /* Active stack scan. When a coroutine is running, the live C stack is the
     * coro's, not main's — scan between the real SP and the active stack's
     * high bound (the coro's stack top, or main's gc_stack_base). */
    void *active_top = gc_stack_base;
#ifdef CLJC_HAVE_CORO
    if (coro_current) {
        active_top = coro_current->stack_top;
        gc_mark(coro_current->self);   /* root the running coro + its resume chain */
        /* main's value stack is suspended too (the global vstack is the coro's). */
        if (main_s_vstack)
            for (size_t vi = 0; vi < main_s_vsp; vi++) gc_mark(main_s_vstack[vi]);
        /* main is suspended at the point it entered coro-land: scan its frames
         * plus its saved registers (held in coro_main_ctx). */
        if (coro_main_saved_sp && gc_stack_base) {
            void *lo = coro_main_saved_sp < gc_stack_base ? coro_main_saved_sp : gc_stack_base;
            void *hi = coro_main_saved_sp < gc_stack_base ? gc_stack_base : coro_main_saved_sp;
            gc_scan_range(lo, hi);
        }
        gc_scan_range(&coro_main_ctx, (char *)&coro_main_ctx + sizeof coro_main_ctx);
    }
#endif
    if (active_top) {
        void *sp = &regs;
        void *lo = sp < active_top ? sp : active_top;
        void *hi = sp < active_top ? active_top : sp;
        gc_scan_range(lo, hi);
    }
    gc_scan_range(&regs, (char *)&regs + sizeof regs);

    /* Sweep cells: free owned arrays once, rebuild the free list. */
    size_t live = 0, freed = 0;
    cell_freelist = NULL;
    for (CellBlock *b = cell_blocks; b; b = b->next) {
        for (size_t i = 0; i < b->used; i++) {
            Cljc *c = &b->cells[i];
            if (c->gcmark) { c->gcmark = 0; live++; continue; }
            if (c->tag != CLJC_FREE) {
                switch (c->tag) {
                    case CLJC_STRING: free(c->as.str); break;
                    case CLJC_BIGINT: free(c->as.big.mag); break;
                    case CLJC_CHUNK:
                        free(c->as.chunk.code);
                        free(c->as.chunk.consts);
                        break;
                    case CLJC_VECTOR: tail_free(c->as.vec.tail, c->as.vec.taillen); break;
                    case CLJC_TVEC:   chunk32_free(c->as.vec.tail); break;
                    case CLJC_HNODE:
                        if (c->as.hnode.nkids == 32) chunk32_free(c->as.hnode.kids);
                        else free(c->as.hnode.kids);
                        break;
                    case CLJC_RECUR:
                        if (c->as.recur.spill) free(c->as.recur.iv[0]);
                        break;
#ifdef CLJC_HAVE_CORO
                    case CLJC_CORO:
                        if (c->as.coro) {
                            free(c->as.coro->stack);
                            free(c->as.coro->s_vstack);
                            free(c->as.coro);
                        }
                        break;
#endif
                    default: break;  /* maps own nothing — their root is a cell */
                }
                c->tag = CLJC_FREE;
                freed++;
            }
            memset(&c->as, 0, sizeof c->as);
            c->as.cons.head = cell_freelist;
            cell_freelist = c;
        }
    }

    /* Sweep envs: free owned binding chains, rebuild the free list. */
    env_freelist = NULL;
    for (EnvBlock *b = env_blocks; b; b = b->next) {
        for (size_t i = 0; i < b->used; i++) {
            CljcEnv *e = &b->envs[i];
            if (e->gcmark) { e->gcmark = 0; live++; continue; }
            if (!e->gcfree) {
                for (Binding *bn = e->bindings; bn; ) {
                    Binding *next = bn->next;
                    bn->next = binding_freelist;
                    binding_freelist = bn;
                    bn = next;
                }
                e->gcfree = 1;
            }
            e->bindings = NULL;
            e->nslots = 0;
            e->parent = env_freelist;
            env_freelist = e;
        }
    }

    gc_freed_last = freed;
    if (getenv("CLJC_GC_LOG")) {
        size_t tagn[24] = {0};
        for (CellBlock *b = cell_blocks; b; b = b->next)
            for (size_t i = 0; i < b->used; i++)
                if (b->cells[i].tag != CLJC_FREE && b->cells[i].tag < 24)
                    tagn[b->cells[i].tag]++;
        fprintf(stderr, "[gc] live=%zu freed=%zu", live, freed);
        for (int t = 0; t < 24; t++)
            if (tagn[t] > 100000) fprintf(stderr, " tag%d=%zu", t, tagn[t]);
        fprintf(stderr, "\n");
    }
    /* 4x headroom: GC was ~60% of runtime on allocation-heavy workloads
     * at 2x (AoC profile, 2026-06). Mark cost scales with live data, so
     * collecting half as often nearly halves GC time for 2x peak waste.
     *
     * Adaptive floor: compute-heavy code keeps a tiny live set but churns
     * millions of short-lived cells (e.g. lazy-seq pipelines), so the fixed
     * per-collection cost (conservative stack scan + root marking) dominates
     * — collecting every 1M allocs wastes it. When a collection reclaims
     * almost everything (live ≪ heap), grow the floor so we collect less
     * often; when much survives (retentive), shrink it back to cap memory.
     * Bounded by GC_CHURN_CAP (peak ≈ that many cells of garbage). */
    size_t total = live + freed;
    if (live * 8 < total && churn_floor < GC_CHURN_CAP)
        churn_floor *= 2;                 /* <12.5% live: amortize fixed cost */
    else if (live * 2 > total && churn_floor > GC_MIN_THRESHOLD)
        churn_floor /= 2;                 /* >50% live: tighten, save memory */
    size_t floor = churn_floor;
    gc_threshold = live * 4 > floor ? live * 4 : floor;
}

#define GC_BYTES_CAP (256u << 20)   /* string-churn slack ceiling: 256MB */

static void maybe_gc(void) {
    if (++gc_allocs >= (gc_stress ? 512 : gc_threshold) ||
        gc_extra_bytes >= GC_BYTES_CAP)
        gc_collect();
}

/* ───── Hashing ──────────────────────────────────────────────────────── */

/* Must agree with cljc_eq: (= 1 1.0) → same hash; (= [1 2] '(1 2)) → lists
 * and vectors hash identically (ordered); maps hash order-independently. */

static uint32_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static Cljc *map_entry_list(Cljc *m);

static uint32_t cljc_hash(Cljc *v) {
    if (v == NULL || v == NIL) return 0x9e3779b9u;
    switch (v->tag) {
        case CLJC_NIL:  return 0x9e3779b9u;
        case CLJC_BOOL: return v->as.b ? 1231u : 1237u;
        case CLJC_INT:  return mix64((uint64_t)v->as.i);
        case CLJC_BIGINT: { uint64_t h = v->as.big.sign < 0 ? 2166136261u : 0;
            for (uint32_t i = 0; i < v->as.big.n; i++) h = h * 31 + v->as.big.mag[i];
            return mix64(h); }
        case CLJC_RATIO: return mix64(cljc_hash(v->as.ratio.num) * 31 + cljc_hash(v->as.ratio.den));
        case CLJC_DOUBLE: {
            double d = v->as.d;
            if (d == (double)(int64_t)d) return mix64((uint64_t)(int64_t)d);  /* = int cross-equality */
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            return mix64(bits);
        }
        case CLJC_STRING:  return fnv1a(v->as.str, strlen(v->as.str));
        case CLJC_CHAR:    return mix64((uint64_t)(uint32_t)v->as.chr) ^ 0x9b3d6f2au;
        case CLJC_KEYWORD: return fnv1a(v->as.kw, strlen(v->as.kw)) ^ 0x517cc1b7u;
        case CLJC_SYMBOL:  return fnv1a(v->as.sym, strlen(v->as.sym)) ^ 0x2545f491u;
        case CLJC_LAZY:
            return cljc_hash(to_seq(v));
        case CLJC_EMPTY:   /* () hashes like an empty list/vector (= () []) */
        case CLJC_LIST: {
            uint32_t h = 1;
            for (Cljc *l = v; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
                h = h * 31 + cljc_hash(l->as.cons.head);
            return h;
        }
        case CLJC_VECTOR: {
            uint32_t h = 1;  /* same scheme as lists — sequential equality */
            for (size_t i = 0; i < vec_len(v); i++)
                h = h * 31 + cljc_hash(vec_nth(v, i));
            return h;
        }
        case CLJC_MAP: case CLJC_SET: {
            uint32_t h = v->tag == CLJC_SET ? 0xa5e3u : 0;  /* order-independent sum */
            for (Cljc *e = map_entry_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
                h += cljc_hash(e->as.cons.head->as.cons.head) * 31
                   + cljc_hash(e->as.cons.head->as.cons.tail);
            return h;
        }
        case CLJC_SORTED: {  /* must match the hash of an equal hash set/map */
            bool is_map = v->as.sorted.is_map;
            uint32_t h = is_map ? 0 : 0xa5e3u;
            for (Cljc *e = sorted_entry_list(v); e != NIL; e = e->as.cons.tail) {
                Cljc *ent = e->as.cons.head;
                Cljc *k = is_map ? vec_nth(ent, 0) : ent;
                h += cljc_hash(k) * 31 + cljc_hash(is_map ? vec_nth(ent, 1) : ent);
            }
            return h;
        }
        default:  /* fns, natives, atoms: identity hash (cells never move) */
            return mix64((uint64_t)(uintptr_t)v);
    }
}

/* ───── HAMT persistent maps ─────────────────────────────────────────── */

#define HAMT_BITS 5u
#define HAMT_MASK 31u

static unsigned popcnt32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcount(x);
#else
    unsigned n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
#endif
}

/* Pool of 32-pointer (256 B) chunks for vector tails and 32-slot trie
 * nodes — both die young, so a free list beats malloc/free churn. The
 * next-chunk pointer reuses the first slot. */
static Cljc **chunk32_freelist;

static Cljc **chunk32_alloc(void) {
    if (chunk32_freelist) {
        Cljc **p = chunk32_freelist;
        chunk32_freelist = (Cljc **)p[0];
        return p;
    }
    return xmalloc(sizeof(Cljc *) * 32);
}

static void chunk32_free(Cljc **p) {
    p[0] = (Cljc *)chunk32_freelist;
    chunk32_freelist = p;
}

/* Size-classed free lists for vector tails (1..32 pointers). Tail sizes
 * repeat heavily under conj churn, so exact-size recycling avoids both
 * malloc traffic and the memory inflation of always-32 chunks. */
static Cljc **tail_freelist[33];

static Cljc **tail_alloc(uint8_t n) {
    if (n == 0) n = 1;
    gc_extra_bytes += sizeof(Cljc *) * n;   /* counted toward the byte cap */
    if (tail_freelist[n]) {
        Cljc **p = tail_freelist[n];
        tail_freelist[n] = (Cljc **)p[0];
        return p;
    }
    return xmalloc(sizeof(Cljc *) * n);
}

static void tail_free(Cljc **p, uint8_t n) {
    if (n == 0) n = 1;
    p[0] = (Cljc *)tail_freelist[n];
    tail_freelist[n] = p;
}

/* Allocate a node with zeroed kid slots. Callers fill the slots immediately
 * (no allocation in between), so a mid-fill GC can never trace garbage. */
static Cljc *mk_hnode(uint32_t bitmap, uint16_t nkids, bool collision, uint32_t chash) {
    Cljc *n = alloc(CLJC_HNODE);
    gc_extra_bytes += sizeof(Cljc *) * (nkids ? nkids : 1);  /* kid arrays
        dodge gc_allocs ticks — HAMT churn built 17GB of slack unseen */
    n->as.hnode.kids = nkids == 32 ? chunk32_alloc()
                                   : xmalloc(sizeof(Cljc *) * (nkids ? nkids : 1));
    memset(n->as.hnode.kids, 0, sizeof(Cljc *) * (nkids ? nkids : 1));
    n->as.hnode.bitmap = bitmap;
    n->as.hnode.nkids = nkids;
    n->as.hnode.collision = collision;
    n->as.hnode.chash = chash;
    return n;
}

static bool hamt_find(Cljc *node, uint32_t shift, uint32_t hash, Cljc *key, Cljc **out) {
    while (node) {
        if (node->as.hnode.collision) {
            if (hash != node->as.hnode.chash) return false;
            for (size_t i = 0; i < node->as.hnode.nkids; i += 2)
                if (cljc_eq(node->as.hnode.kids[i], key)) {
                    if (out) *out = node->as.hnode.kids[i + 1];
                    return true;
                }
            return false;
        }
        uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
        if (!(node->as.hnode.bitmap & bit)) return false;
        size_t pos = popcnt32(node->as.hnode.bitmap & (bit - 1));
        Cljc *k = node->as.hnode.kids[2 * pos];
        Cljc *v = node->as.hnode.kids[2 * pos + 1];
        if (k) {
            if (cljc_eq(k, key)) { if (out) *out = v; return true; }
            return false;
        }
        node = v;          /* descend into subnode */
        shift += HAMT_BITS;
    }
    return false;
}

/* Build the smallest tree holding two distinct-hash entries below `shift`. */
static Cljc *hamt_two(uint32_t shift, uint32_t h1, Cljc *k1, Cljc *v1,
                      uint32_t h2, Cljc *k2, Cljc *v2) {
    if (shift >= 32) {  /* full hash consumed: true collision */
        Cljc *n = mk_hnode(0, 4, true, h1);
        n->as.hnode.kids[0] = k1; n->as.hnode.kids[1] = v1;
        n->as.hnode.kids[2] = k2; n->as.hnode.kids[3] = v2;
        return n;
    }
    uint32_t i1 = (h1 >> shift) & HAMT_MASK, i2 = (h2 >> shift) & HAMT_MASK;
    if (i1 == i2) {
        Cljc *child = hamt_two(shift + HAMT_BITS, h1, k1, v1, h2, k2, v2);
        Cljc *n = mk_hnode(1u << i1, 2, false, 0);
        n->as.hnode.kids[0] = NULL; n->as.hnode.kids[1] = child;
        return n;
    }
    Cljc *n = mk_hnode((1u << i1) | (1u << i2), 4, false, 0);
    if (i1 < i2) {
        n->as.hnode.kids[0] = k1; n->as.hnode.kids[1] = v1;
        n->as.hnode.kids[2] = k2; n->as.hnode.kids[3] = v2;
    } else {
        n->as.hnode.kids[0] = k2; n->as.hnode.kids[1] = v2;
        n->as.hnode.kids[2] = k1; n->as.hnode.kids[3] = v1;
    }
    return n;
}

/* Path-copying insert. Children are computed before parents are allocated,
 * so every intermediate node is stack-reachable when a GC can fire. */
static Cljc *hamt_assoc(Cljc *node, uint32_t shift, uint32_t hash,
                        Cljc *key, Cljc *val, bool *added) {
    if (!node) {
        Cljc *n = mk_hnode(1u << ((hash >> shift) & HAMT_MASK), 2, false, 0);
        n->as.hnode.kids[0] = key; n->as.hnode.kids[1] = val;
        *added = true;
        return n;
    }
    if (node->as.hnode.collision) {
        if (hash == node->as.hnode.chash) {
            uint16_t n_old = node->as.hnode.nkids;
            for (size_t i = 0; i < n_old; i += 2) {
                if (cljc_eq(node->as.hnode.kids[i], key)) {
                    Cljc *n = mk_hnode(0, n_old, true, hash);
                    memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * n_old);
                    n->as.hnode.kids[i + 1] = val;
                    *added = false;
                    return n;
                }
            }
            Cljc *n = mk_hnode(0, (uint16_t)(n_old + 2), true, hash);
            memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * n_old);
            n->as.hnode.kids[n_old] = key;
            n->as.hnode.kids[n_old + 1] = val;
            *added = true;
            return n;
        }
        /* Different hash: wrap the collision node one level down and retry. */
        Cljc *wrap = mk_hnode(1u << ((node->as.hnode.chash >> shift) & HAMT_MASK), 2, false, 0);
        wrap->as.hnode.kids[0] = NULL;
        wrap->as.hnode.kids[1] = node;
        return hamt_assoc(wrap, shift, hash, key, val, added);
    }
    uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
    size_t pos = popcnt32(node->as.hnode.bitmap & (bit - 1));
    uint16_t n_old = node->as.hnode.nkids;
    if (!(node->as.hnode.bitmap & bit)) {
        /* New slot: copy with a 2-wide gap at the insertion point. */
        Cljc *n = mk_hnode(node->as.hnode.bitmap | bit, (uint16_t)(n_old + 2), false, 0);
        memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * 2 * pos);
        n->as.hnode.kids[2 * pos] = key;
        n->as.hnode.kids[2 * pos + 1] = val;
        memcpy(n->as.hnode.kids + 2 * pos + 2, node->as.hnode.kids + 2 * pos,
               sizeof(Cljc *) * (n_old - 2 * pos));
        *added = true;
        return n;
    }
    Cljc *ek = node->as.hnode.kids[2 * pos];
    Cljc *ev = node->as.hnode.kids[2 * pos + 1];
    Cljc *replacement_k, *replacement_v;
    if (!ek) {                                   /* descend into subnode */
        replacement_k = NULL;
        replacement_v = hamt_assoc(ev, shift + HAMT_BITS, hash, key, val, added);
    } else if (cljc_eq(ek, key)) {               /* replace value in place */
        replacement_k = ek;
        replacement_v = val;
        *added = false;
    } else {                                     /* push both entries deeper */
        uint32_t ehash = cljc_hash(ek);
        replacement_k = NULL;
        if (ehash == hash) {
            Cljc *coll = mk_hnode(0, 4, true, hash);
            coll->as.hnode.kids[0] = ek;  coll->as.hnode.kids[1] = ev;
            coll->as.hnode.kids[2] = key; coll->as.hnode.kids[3] = val;
            replacement_v = coll;
        } else {
            replacement_v = hamt_two(shift + HAMT_BITS, ehash, ek, ev, hash, key, val);
        }
        *added = true;
    }
    Cljc *n = mk_hnode(node->as.hnode.bitmap, n_old, false, 0);
    memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * n_old);
    n->as.hnode.kids[2 * pos] = replacement_k;
    n->as.hnode.kids[2 * pos + 1] = replacement_v;
    return n;
}

static Cljc *hamt_dissoc(Cljc *node, uint32_t shift, uint32_t hash,
                         Cljc *key, bool *removed) {
    if (!node) { *removed = false; return NULL; }
    if (node->as.hnode.collision) {
        if (hash != node->as.hnode.chash) { *removed = false; return node; }
        uint16_t n_old = node->as.hnode.nkids;
        for (size_t i = 0; i < n_old; i += 2) {
            if (cljc_eq(node->as.hnode.kids[i], key)) {
                *removed = true;
                if (n_old == 2) return NULL;
                Cljc *n = mk_hnode(0, (uint16_t)(n_old - 2), true, hash);
                memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * i);
                memcpy(n->as.hnode.kids + i, node->as.hnode.kids + i + 2,
                       sizeof(Cljc *) * (n_old - i - 2));
                return n;
            }
        }
        *removed = false;
        return node;
    }
    uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
    if (!(node->as.hnode.bitmap & bit)) { *removed = false; return node; }
    size_t pos = popcnt32(node->as.hnode.bitmap & (bit - 1));
    uint16_t n_old = node->as.hnode.nkids;
    Cljc *ek = node->as.hnode.kids[2 * pos];
    Cljc *ev = node->as.hnode.kids[2 * pos + 1];
    if (ek) {
        if (!cljc_eq(ek, key)) { *removed = false; return node; }
        *removed = true;
        if (n_old == 2) return NULL;
        Cljc *n = mk_hnode(node->as.hnode.bitmap & ~bit, (uint16_t)(n_old - 2), false, 0);
        memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * 2 * pos);
        memcpy(n->as.hnode.kids + 2 * pos, node->as.hnode.kids + 2 * pos + 2,
               sizeof(Cljc *) * (n_old - 2 * pos - 2));
        return n;
    }
    Cljc *child = hamt_dissoc(ev, shift + HAMT_BITS, hash, key, removed);
    if (!*removed) return node;
    if (child == NULL) {
        if (n_old == 2) return NULL;
        Cljc *n = mk_hnode(node->as.hnode.bitmap & ~bit, (uint16_t)(n_old - 2), false, 0);
        memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * 2 * pos);
        memcpy(n->as.hnode.kids + 2 * pos, node->as.hnode.kids + 2 * pos + 2,
               sizeof(Cljc *) * (n_old - 2 * pos - 2));
        return n;
    }
    Cljc *n = mk_hnode(node->as.hnode.bitmap, n_old, false, 0);
    memcpy(n->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * n_old);
    n->as.hnode.kids[2 * pos + 1] = child;
    return n;
}

/* ── public map interface (same contract as the old assoc-array engine) ── */

static Cljc *mk_map(void) {
    return alloc(CLJC_MAP);  /* union zeroed: root NULL, count 0 */
}

static bool map_find(Cljc *m, Cljc *key, Cljc **out) {
    if (!m->as.map.root) return false;
    return hamt_find(m->as.map.root, 0, cljc_hash(key), key, out);
}

static Cljc *map_assoc(Cljc *m, Cljc *k, Cljc *v) {
    bool added = false;
    Cljc *root = hamt_assoc(m->as.map.root, 0, cljc_hash(k), k, v, &added);
    Cljc *nm = alloc(CLJC_MAP);  /* root is stack-rooted here */
    nm->as.map.root = root;
    nm->as.map.count = m->as.map.count + (added ? 1 : 0);
    nm->meta = m->meta;
    return nm;
}

static Cljc *map_dissoc_one(Cljc *m, Cljc *k) {
    bool removed = false;
    Cljc *root = hamt_dissoc(m->as.map.root, 0, cljc_hash(k), k, &removed);
    if (!removed) return m;
    Cljc *nm = alloc(CLJC_MAP);
    nm->as.map.root = root;
    nm->as.map.count = m->as.map.count - 1;
    nm->meta = m->meta;
    return nm;
}

/* Flatten to a list of (k . v) cons pairs for iteration (print, eq, seq). */
static void hamt_entries(Cljc *node, Cljc ***t) {
    if (!node) return;
    for (size_t i = 0; i < node->as.hnode.nkids; i += 2) {
        Cljc *k = node->as.hnode.kids[i];
        if (k == NULL && !node->as.hnode.collision) {
            hamt_entries(node->as.hnode.kids[i + 1], t);
        } else {
            **t = mk_cons(mk_cons(k, node->as.hnode.kids[i + 1]), NIL);
            *t = &(**t)->as.cons.tail;
        }
    }
}

static Cljc *map_entry_list(Cljc *m) {
    Cljc *out = NIL, **t = &out;
    hamt_entries(m->as.map.root, &t);
    return out;
}

/* ── Sets: a HAMT keyed by its elements (each element is its own value) ── */

static Cljc *mk_set(void) {
    return alloc(CLJC_SET);  /* union zeroed: root NULL, count 0 */
}

static Cljc *set_conj(Cljc *s, Cljc *x) {
    bool added = false;
    Cljc *root = hamt_assoc(s->as.map.root, 0, cljc_hash(x), x, x, &added);
    Cljc *ns = alloc(CLJC_SET);
    ns->as.map.root = root;
    ns->as.map.count = s->as.map.count + (added ? 1 : 0);
    ns->meta = s->meta;
    return ns;
}

static Cljc *set_disj(Cljc *s, Cljc *x) {
    bool removed = false;
    Cljc *root = hamt_dissoc(s->as.map.root, 0, cljc_hash(x), x, &removed);
    if (!removed) return s;
    Cljc *ns = alloc(CLJC_SET);
    ns->as.map.root = root;
    ns->as.map.count = s->as.map.count - 1;
    ns->meta = s->meta;
    return ns;
}

static bool set_contains(Cljc *s, Cljc *x, Cljc **out) {
    if (!s->as.map.root) return false;
    return hamt_find(s->as.map.root, 0, cljc_hash(x), x, out);
}

/* Set elements as a list (each HAMT entry's key). */
static Cljc *set_element_list(Cljc *s) {
    Cljc *out = NIL, **t = &out;
    hamt_entries(s->as.map.root, &t);
    for (Cljc *l = out; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        l->as.cons.head = l->as.cons.head->as.cons.head;  /* (x . x) -> x */
    return out;
}

/* ───── Persistent vectors ───────────────────────────────────────────── */

/* Clojure-style: a 32-way trie indexed by position bits plus a tail array
 * holding the last ≤32 elements. conj is amortized O(1) (tail copy); every
 * 32nd conj pushes the tail into the trie as a leaf by path copying.
 * Trie nodes reuse CLJC_HNODE with 32 fixed kid slots (bitmap unused). */

static size_t vec_len(Cljc *v) { return v->as.vec.count; }

static size_t vec_tailoff(Cljc *v) {
    uint32_t cnt = v->as.vec.count;
    return cnt < 32 ? 0 : ((size_t)((cnt - 1) >> 5)) << 5;
}

static Cljc *vec_nth(Cljc *v, size_t i) {
    if (i >= v->as.vec.count) cljc_error("index %zu out of bounds for length %u", i, v->as.vec.count);
    size_t off = vec_tailoff(v);
    if (i >= off) return v->as.vec.tail[i - off];
    Cljc *node = v->as.vec.root;
    for (int level = v->as.vec.shift; level > 0; level -= 5)
        node = node->as.hnode.kids[(i >> level) & 31];
    return node->as.hnode.kids[i & 31];
}

static Cljc *vec_node32(void) { return mk_hnode(0, 32, false, 0); }

/* Fresh vector cell sharing v's tree but with its own (copied) tail. */
static Cljc *vec_cell(Cljc *root, uint8_t shift, uint32_t count,
                      Cljc **tail_src, uint8_t taillen, Cljc *append) {
    Cljc *nv = alloc(CLJC_VECTOR);  /* union zeroed: safe if GC fires below */
    uint8_t n = (uint8_t)(taillen + (append ? 1 : 0));
    nv->as.vec.tail = tail_alloc(n);
    if (taillen) memcpy(nv->as.vec.tail, tail_src, sizeof(Cljc *) * taillen);
    if (append) nv->as.vec.tail[taillen] = append;
    nv->as.vec.taillen = n;
    nv->as.vec.root = root;
    nv->as.vec.shift = shift;
    nv->as.vec.count = count;
    return nv;
}

static Cljc *mk_empty_vec(void) { return vec_cell(NULL, 0, 0, NULL, 0, NULL); }

static Cljc *vec_newpath(int level, Cljc *node) {
    while (level > 0) {
        Cljc *wrap = vec_node32();  /* node stays stack-rooted */
        wrap->as.hnode.kids[0] = node;
        node = wrap;
        level -= 5;
    }
    return node;
}

static Cljc *vec_pushtail(int level, Cljc *parent, Cljc *tailnode, uint32_t cnt) {
    size_t subidx = ((cnt - 1) >> level) & 31;
    Cljc *newchild;
    if (level == 5) {
        newchild = tailnode;
    } else {
        Cljc *child = parent->as.hnode.kids[subidx];
        newchild = child ? vec_pushtail(level - 5, child, tailnode, cnt)
                         : vec_newpath(level - 5, tailnode);
    }
    Cljc *ret = vec_node32();  /* newchild computed first: stack-rooted */
    memcpy(ret->as.hnode.kids, parent->as.hnode.kids, sizeof(Cljc *) * 32);
    ret->as.hnode.kids[subidx] = newchild;
    return ret;
}

static Cljc *vec_conj1(Cljc *v, Cljc *x) {
    uint32_t cnt = v->as.vec.count;
    if (cnt - vec_tailoff(v) < 32) {  /* room in tail: the cheap path */
        Cljc *nv = vec_cell(v->as.vec.root, v->as.vec.shift, cnt + 1,
                            v->as.vec.tail, v->as.vec.taillen, x);
        nv->meta = v->meta;
        return nv;
    }
    /* Tail full: push it into the trie as a leaf. */
    Cljc *leaf = vec_node32();
    memcpy(leaf->as.hnode.kids, v->as.vec.tail, sizeof(Cljc *) * 32);
    Cljc *newroot;
    uint8_t newshift = v->as.vec.shift;
    if (v->as.vec.root == NULL) {
        newroot = leaf;             /* first leaf IS the root (shift 0) */
        newshift = 0;
    } else if ((cnt >> 5) > (1u << v->as.vec.shift)) {
        Cljc *path = vec_newpath(v->as.vec.shift, leaf);
        newroot = vec_node32();
        newroot->as.hnode.kids[0] = v->as.vec.root;
        newroot->as.hnode.kids[1] = path;
        newshift = (uint8_t)(v->as.vec.shift + 5);
    } else {
        newroot = vec_pushtail(v->as.vec.shift, v->as.vec.root, leaf, cnt);
    }
    Cljc *nv = vec_cell(newroot, newshift, cnt + 1, &x, 0, x); /* tail = [x] */
    nv->meta = v->meta;
    return nv;
}

static Cljc *vec_doassoc(int level, Cljc *node, size_t i, Cljc *x) {
    Cljc *newchild = NULL;
    size_t subidx;
    if (level == 0) {
        subidx = i & 31;
    } else {
        subidx = (i >> level) & 31;
        newchild = vec_doassoc(level - 5, node->as.hnode.kids[subidx], i, x);
    }
    Cljc *ret = vec_node32();
    memcpy(ret->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * 32);
    ret->as.hnode.kids[subidx] = level == 0 ? x : newchild;
    return ret;
}

static Cljc *vec_assoc_idx(Cljc *v, size_t i, Cljc *x) {
    uint32_t cnt = v->as.vec.count;
    if (i == cnt) return vec_conj1(v, x);  /* assoc at len appends */
    if (i > cnt) cljc_error("assoc: index %zu out of bounds for length %u", i, cnt);
    size_t off = vec_tailoff(v);
    if (i >= off) {
        Cljc *nv = vec_cell(v->as.vec.root, v->as.vec.shift, cnt,
                            v->as.vec.tail, v->as.vec.taillen, NULL);
        nv->as.vec.tail[i - off] = x;
        nv->meta = v->meta;
        return nv;
    }
    Cljc *newroot = vec_doassoc(v->as.vec.shift, v->as.vec.root, i, x);
    Cljc *nv = vec_cell(newroot, v->as.vec.shift, cnt,
                        v->as.vec.tail, v->as.vec.taillen, NULL);
    nv->meta = v->meta;
    return nv;
}

/* ── transient vectors: in-place edits behind transient/persistent! ──
 * Node ownership by monotonically increasing edit id (never reused, so a
 * pool-recycled cell can't inherit ownership of someone else's nodes). A
 * node is copied at most once per transient, then mutated freely. */

static uint32_t tvec_next_edit = 1;

static Cljc *tvec_ensure_owned(Cljc *node, uint32_t id) {
    if (node->as.hnode.edit_id == id) return node;
    Cljc *copy = vec_node32();
    memcpy(copy->as.hnode.kids, node->as.hnode.kids, sizeof(Cljc *) * 32);
    copy->as.hnode.edit_id = id;
    return copy;
}

static Cljc *as_tvec(Cljc *v, const char *what) {
    if (v == NIL || v->tag != CLJC_TVEC) cljc_error("%s: not a transient", what);
    if (!v->as.vec.alive) cljc_error("%s: transient used after persistent!", what);
    return v;
}

static Cljc *prim_transient(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    /* Maps/sets: persistent-fallback shim — transient ops delegate to the
     * persistent versions (same results, no speedup; real HAMT transients
     * are future work). Vectors get the true in-place implementation. */
    if (v != NIL && (v->tag == CLJC_MAP || v->tag == CLJC_SET)) return v;
    if (v == NIL || v->tag != CLJC_VECTOR)
        cljc_error("transient: unsupported collection");
    Cljc *t = alloc(CLJC_TVEC);
    t->as.vec.tail = chunk32_alloc();   /* private, mutable, full capacity */
    memset(t->as.vec.tail, 0, sizeof(Cljc *) * 32);
    memcpy(t->as.vec.tail, v->as.vec.tail, sizeof(Cljc *) * v->as.vec.taillen);
    t->as.vec.root = v->as.vec.root;    /* shared until first owned copy */
    t->as.vec.count = v->as.vec.count;
    t->as.vec.shift = v->as.vec.shift;
    t->as.vec.taillen = v->as.vec.taillen;
    t->as.vec.edit_id = tvec_next_edit++;
    t->as.vec.alive = true;
    t->meta = v->meta;          /* carried through the transient roundtrip */
    if (tvec_next_edit == 0) cljc_error("transient: edit ids exhausted");
    return t;
}

static Cljc *prim_persistent_bang(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    if (argv[0] != NIL && (argv[0]->tag == CLJC_MAP || argv[0]->tag == CLJC_SET))
        return argv[0];                       /* shim passthrough */
    Cljc *t = as_tvec(argv[0], "persistent!");
    t->as.vec.alive = false;            /* invalidate further edits */
    Cljc *nv = vec_cell(t->as.vec.root, t->as.vec.shift, t->as.vec.count,
                        t->as.vec.tail, t->as.vec.taillen, NULL);
    nv->meta = t->meta;
    return nv;
}

static Cljc *tvec_pushtail(int level, Cljc *parent, Cljc *tailnode,
                           uint32_t cnt, uint32_t id) {
    Cljc *p = tvec_ensure_owned(parent, id);
    size_t subidx = ((cnt - 1) >> level) & 31;
    if (level == 5) {
        p->as.hnode.kids[subidx] = tailnode;
    } else {
        Cljc *child = p->as.hnode.kids[subidx];
        p->as.hnode.kids[subidx] = child
            ? tvec_pushtail(level - 5, child, tailnode, cnt, id)
            : vec_newpath(level - 5, tailnode);
    }
    return p;
}

static Cljc *prim_conj_bang(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    if (argv[0] != NIL && (argv[0]->tag == CLJC_MAP || argv[0]->tag == CLJC_SET))
        return prim_conj(env, argv, nargs);   /* shim: persistent conj */
    Cljc *t = as_tvec(argv[0], "conj!");
    Cljc *x = argv[1];
    uint32_t cnt = t->as.vec.count;
    if (cnt - vec_tailoff(t) < 32) {            /* in-place: zero alloc */
        t->as.vec.tail[t->as.vec.taillen++] = x;
        t->as.vec.count = cnt + 1;
        return t;
    }
    Cljc *leaf = vec_node32();                  /* tail becomes an owned leaf */
    memcpy(leaf->as.hnode.kids, t->as.vec.tail, sizeof(Cljc *) * 32);
    leaf->as.hnode.edit_id = t->as.vec.edit_id;
    if (t->as.vec.root == NULL) {
        t->as.vec.root = leaf;
        t->as.vec.shift = 0;
    } else if ((cnt >> 5) > (1u << t->as.vec.shift)) {
        Cljc *path = vec_newpath(t->as.vec.shift, leaf);
        Cljc *newroot = vec_node32();
        newroot->as.hnode.edit_id = t->as.vec.edit_id;
        newroot->as.hnode.kids[0] = t->as.vec.root;
        newroot->as.hnode.kids[1] = path;
        t->as.vec.root = newroot;
        t->as.vec.shift = (uint8_t)(t->as.vec.shift + 5);
    } else {
        t->as.vec.root = tvec_pushtail(t->as.vec.shift, t->as.vec.root,
                                       leaf, cnt, t->as.vec.edit_id);
    }
    t->as.vec.tail[0] = x;                      /* reuse the tail buffer */
    t->as.vec.taillen = 1;
    t->as.vec.count = cnt + 1;
    return t;
}

static Cljc *prim_assoc_bang(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    if (argv[0] != NIL && argv[0]->tag == CLJC_MAP)
        return prim_assoc(env, argv, nargs);  /* shim: persistent assoc */
    Cljc *t = as_tvec(argv[0], "assoc!");
    size_t i = (size_t)as_int(argv[1], "assoc!");
    Cljc *x = argv[2];
    uint32_t cnt = t->as.vec.count;
    if (i == cnt) { Cljc *cargs[2] = {t, x}; return prim_conj_bang(env, cargs, 2); }
    if (i > cnt) cljc_error("assoc!: index %zu out of bounds for length %u", i, cnt);
    size_t off = vec_tailoff(t);
    if (i >= off) {                              /* in-place: zero alloc */
        t->as.vec.tail[i - off] = x;
        return t;
    }
    uint32_t id = t->as.vec.edit_id;
    Cljc *node = t->as.vec.root = tvec_ensure_owned(t->as.vec.root, id);
    for (int level = t->as.vec.shift; level > 0; level -= 5) {
        size_t sub = (i >> level) & 31;
        Cljc *child = tvec_ensure_owned(node->as.hnode.kids[sub], id);
        node->as.hnode.kids[sub] = child;        /* owned: safe to mutate */
        node = child;
    }
    node->as.hnode.kids[i & 31] = x;
    return t;
}

/* Build a vector from a C array (small n — entry pairs, literals). */
static Cljc *mk_vector(Cljc **items, size_t n) {
    Cljc *v = mk_empty_vec();
    for (size_t i = 0; i < n; i++) v = vec_conj1(v, items[i]);
    return v;
}

static Cljc *cell_alloc(bool zero) {
    maybe_gc();
    Cljc *c;
    if (cell_freelist) {
        c = cell_freelist;
        cell_freelist = c->as.cons.head;
    } else {
        if (!cell_blocks || cell_blocks->used == CELL_BLOCK_N) {
            CellBlock *b = malloc(sizeof *b);
            if (!b) cljc_error("out of memory");
            b->used = 0;
            b->next = cell_blocks;
            cell_blocks = b;
        }
        c = &cell_blocks->cells[cell_blocks->used++];
    }
    /* Zeroing keeps half-built cells safe to mark/sweep. Leaf tags (ints,
     * doubles) have no owned pointers and no children, so their callers
     * skip it — they overwrite the value slot immediately. */
    if (zero) memset(&c->as, 0, sizeof c->as);
    c->gcmark = 0;
    c->meta = NULL;
    return c;
}

static CljcEnv *env_alloc(void) {
    maybe_gc();
    CljcEnv *e;
    if (env_freelist) {
        e = env_freelist;
        env_freelist = e->parent;
    } else {
        if (!env_blocks || env_blocks->used == ENV_BLOCK_N) {
            EnvBlock *b = malloc(sizeof *b);
            if (!b) cljc_error("out of memory");
            b->used = 0;
            b->next = env_blocks;
            env_blocks = b;
        }
        e = &env_blocks->envs[env_blocks->used++];
    }
    e->bindings = NULL;
    e->parent = NULL;
    e->gcmark = 0;
    e->gcfree = 0;
    e->nslots = 0;
    return e;
}

/* ───── Argument validation helpers ──────────────────────────────────── */

static void need_args(Cljc *rest, size_t n, const char *what) {
    if (list_len(rest) < n)
        cljc_error("%s: expected at least %zu argument%s", what, n, n == 1 ? "" : "s");
}

static const char *sym_name(Cljc *v, const char *what) {
    if (v == NULL || v->tag != CLJC_SYMBOL) cljc_error("%s: expected a symbol", what);
    return v->as.sym;
}

/* Peel reader metadata off a def/defmacro name: (def ^:private x ..) reads the
 * name as (with-meta x m) — possibly stacked. Metadata is not retained. */
static Cljc *peel_meta_sym(Cljc *v) {
    while (v != NIL && v->tag == CLJC_LIST &&
           v->as.cons.head->tag == CLJC_SYMBOL &&
           !strcmp(v->as.cons.head->as.sym, "with-meta") &&
           v->as.cons.tail != NIL)
        v = v->as.cons.tail->as.cons.head;
    return v;
}

static int64_t as_int(Cljc *v, const char *what) {
    if (v == NULL || v->tag != CLJC_INT)
        cljc_error("%s: expected a number, got %s", what, val_type_name(v));
    return v->as.i;
}

/* ───── Growable string buffer ───────────────────────────────────────── */

/* Shared by the reader (string escapes) and the printer, so that `str`,
 * `pr-str`, and stdout printing all use a single implementation. */
typedef struct { char *data; size_t len, cap; } SBuf;

static void sb_grow(SBuf *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    sb->cap = sb->cap ? sb->cap * 2 : 64;
    while (sb->len + need + 1 > sb->cap) sb->cap *= 2;
    sb->data = realloc(sb->data, sb->cap);
    if (!sb->data) cljc_error("out of memory");
}
static void sb_putc(SBuf *sb, char c) { sb_grow(sb, 1); sb->data[sb->len++] = c; sb->data[sb->len] = '\0'; }
static void sb_puts(SBuf *sb, const char *s) {
    size_t n = strlen(s);
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n; sb->data[sb->len] = '\0';
}
static void sb_printf(SBuf *sb, const char *fmt, ...) {
    char tmp[64];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_puts(sb, tmp);
}

/* ───── Reader ───────────────────────────────────────────────────────── */

static int rd_line;                 /* 1-based; 0 = no tracking */
static const char *rd_line_start;   /* for column computation */

static void skip_ws(const char **p) {
    while (**p) {
        if (**p == '\n') { if (rd_line) { rd_line++; rd_line_start = *p + 1; } (*p)++; }
        else if (isspace((unsigned char)**p) || **p == ',') { (*p)++; }
        else if (**p == ';') { while (**p && **p != '\n') (*p)++; }
        else break;
    }
}

static bool is_sym_char(int c) {
    if (c == '\0' || isspace(c)) return false;
    return !strchr("()[]{}\";`,~@", c);  /* ' is legal INSIDE symbols (coll') */
}

static Cljc *read_atom(const char **p) {
    const char *start = *p;
    while (is_sym_char((unsigned char)**p)) (*p)++;
    size_t n = *p - start;
    if (n == 0) cljc_error("unexpected character: %c", **p);

    /* Number? Accept [+-]?(digits)(.digits)? */
    bool is_num = (isdigit((unsigned char)start[0]) ||
                   ((start[0] == '+' || start[0] == '-') && n > 1 &&
                    isdigit((unsigned char)start[1])));
    if (is_num) {
        /* Plain decimal integer (optional sign, digits, optional trailing N):
         * may exceed int64 -> bigint. Detected first, before the 64-char buf cap,
         * so arbitrarily long literals parse. big_from_decimal demotes if it fits. */
        {
            const char *p = start; size_t m = n;
            if (m && (*p == '+' || *p == '-')) { p++; m--; }
            bool hasN = m > 0 && p[m - 1] == 'N';
            size_t dn = hasN ? m - 1 : m;
            bool alldig = dn > 0;
            for (size_t i = 0; i < dn && alldig; i++) if (p[i] < '0' || p[i] > '9') alldig = false;
            if (alldig && (hasN || dn >= 19)) {
                char *tmp = xmalloc(n + 1); memcpy(tmp, start, n); tmp[n] = '\0';
                Cljc *r = big_from_decimal(tmp);   /* stops at the trailing N */
                free(tmp);
                return r;
            }
        }
        /* ratio literal: [+-]?digits/digits -> exact rational */
        {
            const char *slash = memchr(start, '/', n);
            const char *ns = start; if (*ns == '+' || *ns == '-') ns++;
            if (slash && slash > ns && slash + 1 < start + n) {
                bool ok = true;
                for (const char *q = ns; q < slash && ok; q++) if (*q < '0' || *q > '9') ok = false;
                for (const char *q = slash + 1; q < start + n && ok; q++) if (*q < '0' || *q > '9') ok = false;
                if (ok) {
                    char *nb = xmalloc(n + 1), *db = xmalloc(n + 1);
                    size_t nl = (size_t)(slash - start), dl = (size_t)(start + n - slash - 1);
                    memcpy(nb, start, nl); nb[nl] = '\0';
                    memcpy(db, slash + 1, dl); db[dl] = '\0';
                    Cljc *num = big_from_decimal(nb), *den = big_from_decimal(db);
                    free(nb); free(db);
                    return make_ratio(num, den);
                }
            }
        }
        char buf[64]; if (n >= sizeof buf) cljc_error("number too long");
        memcpy(buf, start, n); buf[n] = '\0';
        int64_t sign = 1;
        char *d = buf;
        if (*d == '+' || *d == '-') { if (*d == '-') sign = -1; d++; }
        /* hex: 0x1F / 0XFF — parse UNSIGNED then cast, so a full-width literal
         * like 0xbf58476d1ce4e5b9 wraps to its signed long value (Java-like)
         * instead of clamping to LLONG_MAX. */
        if (d[0] == '0' && (d[1] == 'x' || d[1] == 'X') && d[2])
            return mk_int(sign * (int64_t)strtoull(d + 2, NULL, 16));
        /* radix: 2r1010, 16rFF, 36rZ (Clojure's <radix>r<digits>) */
        char *rp = strchr(d, 'r'); if (!rp) rp = strchr(d, 'R');
        if (rp && rp != d && rp[1]) {
            *rp = '\0';
            long radix = strtol(d, NULL, 10);
            if (radix >= 2 && radix <= 36)
                return mk_int(sign * (int64_t)strtoull(rp + 1, NULL, (int)radix));
            *rp = 'r';
        }
        bool is_float = false;
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') is_float = true;
        if (is_float) return mk_double(strtod(buf, NULL));
        /* NB: leading-zero stays DECIMAL (017 => 17), not octal — a deliberate
         * cljc choice to avoid the octal footgun (see tests.clj). */
        return mk_int(strtoll(buf, NULL, 10));
    }

    /* Keyword? */
    if (start[0] == ':') {
        if (n == 1) cljc_error("invalid token: :");
        if (start[1] == ':') {
            /* ::name -> :current-ns/name ; ::alias/name -> :full-ns/name */
            if (n == 2) cljc_error("invalid token: ::");
            const char *body = start + 2;
            size_t blen = n - 2;
            const char *slash = memchr(body, '/', blen);
            char buf[256];
            if (slash) {
                size_t alen = (size_t)(slash - body);
                const char *al = intern(body, alen);
                const char *full = alias_lookup(al, cur_reader_ns);
                if (!full) full = cur_reader_ns;
                snprintf(buf, sizeof buf, "%s/%.*s", full ? full : "",
                         (int)(blen - alen - 1), slash + 1);
            } else {
                snprintf(buf, sizeof buf, "%s/%.*s",
                         cur_reader_ns ? cur_reader_ns : "", (int)blen, body);
            }
            return mk_kw(intern(buf, strlen(buf)));
        }
        return mk_kw(intern(start + 1, n - 1));
    }

    /* Literals */
    if (n == 3 && memcmp(start, "nil", 3) == 0) return NIL;
    if (n == 4 && memcmp(start, "true", 4) == 0) return TRUE;
    if (n == 5 && memcmp(start, "false", 5) == 0) return FALSE;

    return mk_sym(intern(start, n));
}

static Cljc *read_string(const char **p) {
    (*p)++; /* consume opening " */
    SBuf sb = {0};
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            switch (**p) {
                case 'n':  sb_putc(&sb, '\n'); break;
                case 't':  sb_putc(&sb, '\t'); break;
                case 'r':  sb_putc(&sb, '\r'); break;
                case 'b':  sb_putc(&sb, '\b'); break;
                case 'f':  sb_putc(&sb, '\f'); break;
                case '0':  sb_putc(&sb, '\0'); break;
                case '\\': sb_putc(&sb, '\\'); break;
                case '"':  sb_putc(&sb, '"');  break;
                case 'u': {  /* \uXXXX — UTF-8 encode into the byte string */
                    int cp = 0, k = 0;
                    for (; k < 4; k++) {
                        char h = (*p)[1 + k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) break;
                        cp = cp * 16 + d;
                    }
                    if (k == 0) cljc_error("invalid \\u escape");
                    if (cp < 0x80) sb_putc(&sb, (char)cp);
                    else if (cp < 0x800) {
                        sb_putc(&sb, (char)(0xC0 | (cp >> 6)));
                        sb_putc(&sb, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        sb_putc(&sb, (char)(0xE0 | (cp >> 12)));
                        sb_putc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        sb_putc(&sb, (char)(0x80 | (cp & 0x3F)));
                    }
                    *p += k;  /* past hex digits; outer (*p)++ eats the last */
                    break;
                }
                case '\0': cljc_error("unterminated string");
                default:   cljc_error("unsupported escape: \\%c", **p);
            }
        } else {
            if (c == '\n' && rd_line) { rd_line++; rd_line_start = *p + 1; }
            sb_putc(&sb, c);
        }
        (*p)++;
    }
    if (**p != '"') cljc_error("unterminated string");
    (*p)++; /* consume closing " */
    Cljc *r = mk_str(sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return r;
}

static Cljc *read_list(const char **p, char close) {
    int col0 = (rd_line && rd_line_start && *p >= rd_line_start)
        ? (int)(*p - rd_line_start) + 1 : 0;
    (*p)++; /* consume open */
    int line0 = rd_line;
    Cljc *head = NIL, **tail = &head;
    for (;;) {
        skip_ws(p);
        if (**p == '\0') cljc_error("unterminated list");
        if (**p == close) {
            (*p)++;
            if (line0 && head != NIL) {   /* location for error traces */
                Cljc *m = mk_map();
                m = map_assoc(m, mk_kw(intern("line", 4)), mk_int(line0));
                if (col0) m = map_assoc(m, mk_kw(intern("col", 3)), mk_int(col0));
                head->meta = m;
            }
            return head;
        }
        Cljc *item = read_form(p);
        /* unpack #?@ splice markers into this list */
        if (item != NULL && item != NIL && item->tag == CLJC_LIST &&
            item->as.cons.head->tag == CLJC_SYMBOL &&
            item->as.cons.head->as.sym == intern("**reader-splice**", 17)) {
            Cljc *branch = item->as.cons.tail->as.cons.head;
            for (Cljc *s = to_seq(branch); s && s->tag == CLJC_LIST; s = s->as.cons.tail) {
                *tail = mk_cons(s->as.cons.head, NIL);
                tail = &(*tail)->as.cons.tail;
            }
            continue;
        }
        *tail = mk_cons(item, NIL);
        tail = &(*tail)->as.cons.tail;
    }
}

static Cljc *read_form(const char **p) {
    skip_ws(p);
    char c = **p;
    if (c == '\0') return NULL;
    if (c == '(') { Cljc *l = read_list(p, ')'); return l == NIL ? EMPTY : l; }
    if (c == '[') {
        /* v0: treat [ … ] as a list tagged for vector semantics later. For
         * now use the list reader and rewrap. Persistent vectors arrive
         * with the PVec milestone. */
        Cljc *list = read_list(p, ']');
        Cljc *v = mk_empty_vec();
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
            v = vec_conj1(v, l->as.cons.head);
        return v;
    }
    if (c == '^') {
        /* ^{...} and ^:kw compile to (with-meta form m), evaluated at
         * runtime like Clojure; ^Tag type hints are discarded. */
        (*p)++;
        Cljc *m = read_form(p);
        Cljc *form = read_form(p);
        if (m != NIL && m->tag == CLJC_KEYWORD) {
            Cljc *mm = mk_map();
            m = map_assoc(mm, m, TRUE);
        }
        if (m == NIL || m->tag != CLJC_MAP) return form;  /* type hint */
        return mk_cons(mk_sym(intern("with-meta", 9)),
                       mk_cons(form, mk_cons(m, NIL)));
    }
    if (c == '\\') {  /* char literal => CLJC_CHAR */
        (*p)++;
        const char *start = *p;
        while (is_sym_char((unsigned char)**p)) (*p)++;
        size_t n = (size_t)(*p - start);
        if (n == 0) { (*p)++; return mk_char((unsigned char)*(*p - 1)); }  /* \( etc */
        if (n == 1) return mk_char((unsigned char)start[0]);
        if (n == 5 && !memcmp(start, "space", 5)) return mk_char(' ');
        if (n == 7 && !memcmp(start, "newline", 7)) return mk_char('\n');
        if (n == 3 && !memcmp(start, "tab", 3)) return mk_char('\t');
        if (n == 6 && !memcmp(start, "return", 6)) return mk_char('\r');
        if (n == 8 && !memcmp(start, "formfeed", 8)) return mk_char('\f');
        if (n == 9 && !memcmp(start, "backspace", 9)) return mk_char('\b');
        if (start[0] == 'u' && n == 5) {             /* \uXXXX hex codepoint */
            int32_t cp = 0;
            for (size_t i = 1; i < n; i++) {
                char d = start[i]; int hv;
                if (d >= '0' && d <= '9') hv = d - '0';
                else if (d >= 'a' && d <= 'f') hv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') hv = d - 'A' + 10;
                else cljc_error("bad \\u char literal");
                cp = cp * 16 + hv;
            }
            return mk_char(cp);
        }
        if (start[0] == 'o' && n >= 2 && n <= 4) {   /* \oNNN octal */
            int32_t cp = 0;
            for (size_t i = 1; i < n; i++) cp = cp * 8 + (start[i] - '0');
            return mk_char(cp);
        }
        if ((unsigned char)start[0] >= 0x80) {   /* a literal multi-byte UTF-8 char, e.g. \⁹ */
            const unsigned char *s = (const unsigned char *)start;
            int32_t cp = 0; size_t len = 0;
            if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
            else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
            else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; len = 4; }
            if (len && len == n) {
                for (size_t i = 1; i < len; i++) cp = (cp << 6) | (s[i] & 0x3F);
                return mk_char(cp);
            }
        }
        cljc_error("unsupported char literal");
    }
    if (c == '#' && (*p)[1] == '_') {
        *p += 2;
        read_form(p);   /* read and discard the next form */
        skip_ws(p);
        /* return the FOLLOWING real form (so stacked #_ #_ discard two, and a
         * #_ at a call/collection position yields the next element). At a
         * closing delimiter or EOF there is none → splice zero elements. */
        char d = **p;
        if (d == '\0' || d == ')' || d == ']' || d == '}')
            return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                           mk_cons(NIL, NIL));
        return read_form(p);
    }
    if (c == '#' && (*p)[1] == '\'') {  /* #'x => (var x) */
        *p += 2;
        Cljc *form = read_form(p);
        return mk_cons(mk_sym(intern("var", 3)), mk_cons(form, NIL));
    }
    if (c == '#' && (*p)[1] == '#') {  /* ##Inf ##-Inf ##NaN */
        *p += 2;
        if (!strncmp(*p, "Inf", 3))  { *p += 3; return mk_double(INFINITY); }
        if (!strncmp(*p, "-Inf", 4)) { *p += 4; return mk_double(-INFINITY); }
        if (!strncmp(*p, "NaN", 3))  { *p += 3; return mk_double(NAN); }
        cljc_error("unknown ## literal (expected ##Inf, ##-Inf, ##NaN)");
    }
    if (c == '#' && (*p)[1] == '?') {
        /* reader conditional: pick a branch by PRIORITY :cljc > :default >
         * :clj. :cljc always wins (cljc-specific); :default beats :clj so a
         * portable #?(:clj java :default portable) still takes the portable
         * branch; :clj is a last resort so cljc can still load clj-only
         * conditionals like #?(:clj X :cljs Y) (taking X). #?@(...) splices the
         * branch into the enclosing list — a marker cons read_list unpacks. */
        *p += 2;
        bool splicing = **p == '@';
        if (splicing) (*p)++;
        skip_ws(p);
        if (**p != '(') cljc_error("#? expects a list");
        Cljc *clauses = read_list(p, ')');
        static const char *KW_CLJC, *KW_CLJ, *KW_DEFAULT;
        if (!KW_CLJC) { KW_CLJC = intern("cljc", 4); KW_CLJ = intern("clj", 3);
                        KW_DEFAULT = intern("default", 7); }
        Cljc *b_cljc = NULL, *b_clj = NULL, *b_default = NULL;
        for (Cljc *l = clauses; l && l->tag == CLJC_LIST && l->as.cons.tail != NIL;
             l = l->as.cons.tail->as.cons.tail) {
            Cljc *k = l->as.cons.head;
            Cljc *branch = l->as.cons.tail->as.cons.head;
            if (k->tag == CLJC_KEYWORD) {
                if (k->as.kw == KW_CLJC && !b_cljc) b_cljc = branch;
                else if (k->as.kw == KW_CLJ && !b_clj) b_clj = branch;
                else if (k->as.kw == KW_DEFAULT && !b_default) b_default = branch;
            }
            if (l->as.cons.tail == NIL) break;
        }
        {
            Cljc *branch = b_cljc ? b_cljc : b_default ? b_default : b_clj;
            if (branch) {
                if (!splicing) return branch;
                return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                               mk_cons(branch, NIL));
            }
        }
        /* no matching branch: vanish entirely (read_list — which also backs
         * vector reading — unpacks the marker), matching Clojure where the form
         * is elided, NOT read as nil. A spurious nil here would otherwise land
         * as a fn param / call arg / :keys entry and skew arities. */
        return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                       mk_cons(NIL, NIL));
    }
    if (c == '#' && (*p)[1] == '(') {
        /* #(...) => (fn [%1 ...] (...)); % aliases %1, %& is the rest arg */
        (*p)++;
        Cljc *body = read_form(p);
        int maxn = 0;
        bool pct = false, variadic = false;
        /* scan for %-symbols (iterative worklist over nested collections) */
        Cljc *work[256]; int wn = 0;
        bool overflow = false;
        work[wn++] = body;
        while (wn > 0) {
            Cljc *f = work[--wn];
            if (!f || f == NIL) continue;
            if (f->tag == CLJC_SYMBOL && f->as.sym[0] == '%') {
                const char *s = f->as.sym;
                if (s[1] == '\0') { pct = true; if (maxn < 1) maxn = 1; }
                else if (s[1] == '&' && s[2] == '\0') variadic = true;
                else if (s[1] >= '1' && s[1] <= '9' && s[2] == '\0') {
                    if (s[1] - '0' > maxn) maxn = s[1] - '0';
                }
            } else if (f->tag == CLJC_LIST) {
                for (Cljc *l = f; l && l->tag == CLJC_LIST && wn < 254; l = l->as.cons.tail)
                    work[wn++] = l->as.cons.head;
            } else if (f->tag == CLJC_VECTOR) {
                for (size_t i = 0; i < vec_len(f) && wn < 254; i++)
                    work[wn++] = vec_nth(f, i);
            } else if (f->tag == CLJC_MAP) {
                for (Cljc *e = map_entry_list(f); e && e->tag == CLJC_LIST && wn < 253;
                     e = e->as.cons.tail) {
                    work[wn++] = e->as.cons.head->as.cons.head;
                    work[wn++] = e->as.cons.head->as.cons.tail;
                }
            }
        }
        if (wn >= 253) overflow = true;
        if (overflow) cljc_error("#(): body too complex to scan for %% params");
        Cljc *items[11];
        size_t ni = 0;
        /* params are always %1..%maxn; bare % is an alias for %1 (Clojure), so a
         * body mixing % and %1 is fine — we bind % via a let below. */
        for (int i = 1; i <= maxn; i++) {
            char nm[4] = {'%', (char)('0' + i), 0, 0};
            items[ni++] = mk_sym(intern(nm, 2));
        }
        if (variadic) {
            items[ni++] = mk_sym(intern("&", 1));
            items[ni++] = mk_sym(intern("%&", 2));
        }
        Cljc *params = mk_vector(items, ni);
        if (pct) {   /* (let [% %1] body) so bare % refers to the first param */
            Cljc *bv2[2] = { mk_sym(intern("%", 1)), mk_sym(intern("%1", 2)) };
            body = mk_cons(mk_sym(intern("let", 3)),
                           mk_cons(mk_vector(bv2, 2), mk_cons(body, NIL)));
        }
        return mk_cons(mk_sym(intern("fn", 2)),
                       mk_cons(params, mk_cons(body, NIL)));
    }
    if (c == '#' && (*p)[1] == '"') {
        /* Raw string for regex patterns: backslashes pass through verbatim;
         * \" is the only escape (yields a quote char in the pattern). */
        *p += 2;
        SBuf sb = {0};
        while (**p && **p != '"') {
            if (**p == '\\' && (*p)[1] == '"') { sb_putc(&sb, '"'); *p += 2; continue; }
            sb_putc(&sb, **p);
            (*p)++;
        }
        if (**p != '"') cljc_error("unterminated regex literal");
        (*p)++;
        Cljc *r = mk_str(sb.data ? sb.data : "", sb.len);
        free(sb.data);
        /* Tag as a regex: str/split etc. dispatch on this meta. The map is
         * rebuilt per literal (cheap; a static would dodge the GC roots). */
        r->meta = map_assoc(mk_map(), mk_kw(intern("regex", 5)), TRUE);
        return r;
    }
    if (c == '#' && (*p)[1] == ':') {
        /* namespaced map: #:ns{...} => {:ns/k v}; #::{...} uses the current ns.
         * A key that's already qualified is kept; :_/k means "no namespace". */
        *p += 2;
        bool auto_ns = (**p == ':');
        if (auto_ns) (*p)++;
        char nsbuf[256]; size_t nl = 0;
        while (**p && **p != '{' && !isspace((unsigned char)**p) && nl < 255) nsbuf[nl++] = *(*p)++;
        nsbuf[nl] = '\0';
        skip_ws(p);
        if (**p != '{') cljc_error("namespaced map literal expects {");
        const char *ns = nl ? nsbuf : (cur_reader_ns ? cur_reader_ns : "user");
        Cljc *list = read_list(p, '}');
        if (list_len(list) % 2 != 0) cljc_error("map literal must contain an even number of forms");
        Cljc *m = mk_map();
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail->as.cons.tail) {
            Cljc *k = l->as.cons.head, *val = l->as.cons.tail->as.cons.head;
            const char *nm = k->tag == CLJC_KEYWORD ? k->as.kw : k->tag == CLJC_SYMBOL ? k->as.sym : NULL;
            if (nm) {
                const char *slash = strchr(nm, '/');
                if (slash) {                       /* already qualified (or _/k) */
                    if (nm[0] == '_' && nm[1] == '/') {
                        const char *bare = nm + 2;
                        k = k->tag == CLJC_KEYWORD ? mk_kw(intern(bare, strlen(bare)))
                                                   : mk_sym(intern(bare, strlen(bare)));
                    }
                } else {
                    char buf[512]; snprintf(buf, sizeof buf, "%s/%s", ns, nm);
                    k = k->tag == CLJC_KEYWORD ? mk_kw(intern(buf, strlen(buf)))
                                               : mk_sym(intern(buf, strlen(buf)));
                }
            }
            m = map_assoc(m, k, val);
        }
        return m;
    }
    if (c == '#' && (*p)[1] == '{') {
        (*p)++;  /* consume '#'; read_list consumes '{' */
        Cljc *list = read_list(p, '}');
        /* Elements are unevaluated forms; eval builds the live set.
         * Duplicate literal elements are a reader error, as in Clojure. */
        Cljc *s = mk_set();
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
            if (set_contains(s, l->as.cons.head, NULL))
                cljc_error("duplicate element in set literal");
            s = set_conj(s, l->as.cons.head);
        }
        return s;
    }
    if (c == '{') {
        Cljc *list = read_list(p, '}');
        if (list_len(list) % 2 != 0)
            cljc_error("map literal must contain an even number of forms");
        /* Keys here are unevaluated FORMS — eval() builds the live map.
         * Duplicate literal keys are a reader error, as in Clojure. */
        Cljc *m = mk_map();
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail->as.cons.tail) {
            if (map_find(m, l->as.cons.head, NULL))
                cljc_error("duplicate key in map literal");
            m = map_assoc(m, l->as.cons.head, l->as.cons.tail->as.cons.head);
        }
        return m;
    }
    if (c == '"') return read_string(p);
    if (c == '\'') {
        (*p)++;
        Cljc *quoted = read_form(p);
        return mk_cons(mk_sym(intern("quote", 5)), mk_cons(quoted, NIL));
    }
    if (c == '`') {
        (*p)++;
        Cljc *form = read_form(p);
        return mk_cons(mk_sym(intern("quasiquote", 10)), mk_cons(form, NIL));
    }
    if (c == '~') {
        (*p)++;
        /* qualified like Clojure (and like @ -> clojure.core/deref above), so
         * code that inspects the form (e.g. Emmy's unquote?) sees the same head */
        const char *tag = "clojure.core/unquote";
        size_t taglen = 20;
        if (**p == '@') { (*p)++; tag = "clojure.core/unquote-splicing"; taglen = 29; }
        Cljc *form = read_form(p);
        return mk_cons(mk_sym(intern(tag, taglen)), mk_cons(form, NIL));
    }
    if (c == '@') {
        (*p)++;
        Cljc *form = read_form(p);
        /* @x is clojure.core/deref (qualified, like Clojure) so a namespace that
         * shadows `deref` with its own (e.g. malli's ref deref) doesn't capture it */
        return mk_cons(mk_sym(intern("clojure.core/deref", 18)), mk_cons(form, NIL));
    }
    return read_atom(p);
}

/* ───── Evaluator ────────────────────────────────────────────────────── */

/* Evaluate each form in sequence, return the last result (nil if empty).
 * The shape of fn bodies, do, let, when, and loop. */
static Cljc *eval_body(CljcEnv *env, Cljc *body) {
    Cljc *r = NIL;
    for (; body && body->tag == CLJC_LIST; body = body->as.cons.tail)
        r = eval(env, body->as.cons.head);
    return r;
}

static const char *sym_amp(void) {
    static const char *amp;          /* interning is a hash lookup — this
                                      * ran per param per call in dispatch */
    if (!amp) amp = intern("&", 1);
    return amp;
}

/* Recursive destructuring: bind `pattern` to `value` in `scope`.
 *   symbol            → plain binding
 *   [a b & r :as v]   → sequential destructuring (nil-fills past the end)
 *   {a :k}            → bind a to (get value :k)
 *   {:keys [a b] :or {a 1} :as m}
 * :or defaults are forms, evaluated in `scope` when the key is missing. */
static Cljc *kvseq_to_map(Cljc *s);
static void destructure(CljcEnv *scope, Cljc *pattern, Cljc *value) {
    if (pattern != NIL && pattern->tag == CLJC_SYMBOL) {
        env_define(scope, pattern->as.sym, value);
        return;
    }
    if (pattern != NIL && pattern->tag == CLJC_VECTOR) {
        static const char *KW_AS;
        if (!KW_AS) KW_AS = intern("as", 2);
        /* seq1 cursor, NOT to_seq: [[x & xs]] on an infinite lazy seq must
         * realize one element per binding — to_seq realized the whole seq
         * (a self-recursive sieve def ran away to 17GB). */
        Cljc *s = value == NIL ? NIL : seq1(value);
        for (size_t i = 0; i < vec_len(pattern); i++) {
            Cljc *pe = vec_nth(pattern, i);
            if (pe->tag == CLJC_SYMBOL && pe->as.sym == sym_amp()) {
                if (i + 1 >= vec_len(pattern))
                    cljc_error("destructure: & needs a binding form");
                Cljc *restpat = vec_nth(pattern, ++i);
                destructure(scope, restpat,
                            (restpat != NIL && restpat->tag == CLJC_MAP)
                            ? kvseq_to_map(s) : s);
                continue;
            }
            if (pe->tag == CLJC_KEYWORD && pe->as.kw == KW_AS) {
                if (i + 1 >= vec_len(pattern))
                    cljc_error("destructure: :as needs a symbol");
                env_define(scope, sym_name(vec_nth(pattern, ++i), ":as"), value);
                continue;
            }
            destructure(scope, pe, s == NIL ? NIL : s->as.cons.head);
            if (s != NIL) s = seq1(s->as.cons.tail);
        }
        return;
    }
    if (pattern != NIL && pattern->tag == CLJC_MAP) {
        static const char *KW_KEYS, *KW_AS, *KW_OR, *KW_STRS, *KW_SYMS;
        if (!KW_KEYS) {
            KW_KEYS = intern("keys", 4);
            KW_AS = intern("as", 2);
            KW_OR = intern("or", 2);
            KW_STRS = intern("strs", 4);
            KW_SYMS = intern("syms", 4);
        }
        Cljc *defaults = NULL;  /* the :or map, if present */
        Cljc *pentries = map_entry_list(pattern);
        for (Cljc *e = pentries; e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *k = e->as.cons.head->as.cons.head;
            if (k->tag == CLJC_KEYWORD && k->as.kw == KW_OR) defaults = e->as.cons.head->as.cons.tail;
        }
        for (Cljc *e = pentries; e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *k = e->as.cons.head->as.cons.head;
            Cljc *spec = e->as.cons.head->as.cons.tail;
            if (k->tag == CLJC_KEYWORD) {
                if (k->as.kw == KW_OR) continue;
                if (k->as.kw == KW_AS) {
                    env_define(scope, sym_name(spec, ":as"), value);
                    continue;
                }
                if (k->as.kw == KW_KEYS) {
                    if (spec == NIL || spec->tag != CLJC_VECTOR)
                        cljc_error("destructure: :keys needs a vector of symbols");
                    for (size_t j = 0; j < vec_len(spec); j++) {
                        /* {:keys [a b]} or the keyword shorthand {:keys [:a :b]} */
                        Cljc *elt = vec_nth(spec, j);
                        if (elt == NIL) continue;   /* a #?(:cljs ..)-only key elides to nil */
                        const char *nm = elt->tag == CLJC_KEYWORD
                                         ? elt->as.kw : sym_name(elt, ":keys");
                        Cljc *kw = mk_kw(nm);   /* lookup key keeps the namespace */
                        /* a namespaced key ({:keys [a/b]} / {:keys [::foo]}) binds
                         * the BARE local name (b / foo) to the namespaced value. */
                        const char *slash = strrchr(nm, '/');
                        const char *local = (slash && slash[1])
                                            ? intern(slash + 1, strlen(slash + 1)) : nm;
                        Cljc *v = NIL;
                        bool found = value != NIL && value->tag == CLJC_MAP &&
                                     map_find(value, kw, &v);
                        if (!found && defaults && defaults->tag == CLJC_MAP) {
                            Cljc *d;
                            if (map_find(defaults, mk_sym(local), &d))
                                v = eval(scope, d);
                        }
                        env_define(scope, local, v);
                    }
                    continue;
                }
                if (k->as.kw == KW_STRS || k->as.kw == KW_SYMS) {
                    /* {:strs [a b]} → (get m "a"); {:syms [a b]} → (get m 'a) */
                    if (spec == NIL || spec->tag != CLJC_VECTOR)
                        cljc_error("destructure: :%s needs a vector of symbols", k->as.kw);
                    bool syms = (k->as.kw == KW_SYMS);
                    for (size_t j = 0; j < vec_len(spec); j++) {
                        Cljc *elt = vec_nth(spec, j);
                        if (elt == NIL) continue;
                        const char *nm = sym_name(elt, syms ? ":syms" : ":strs");
                        Cljc *key = syms ? mk_sym(nm) : mk_str(nm, strlen(nm));
                        Cljc *v = NIL;
                        bool found = value != NIL && value->tag == CLJC_MAP &&
                                     map_find(value, key, &v);
                        if (!found && defaults && defaults->tag == CLJC_MAP) {
                            Cljc *d;
                            if (map_find(defaults, mk_sym(nm), &d)) v = eval(scope, d);
                        }
                        env_define(scope, nm, v);
                    }
                    continue;
                }
                cljc_error("destructure: unsupported map directive :%s", k->as.kw);
            }
            /* {binding-form lookup-key} — the key is evaluated (a keyword self-
             * evaluates, but a symbol key is written quoted: {n '?n} looks up the
             * symbol ?n, not the literal form (quote ?n)). */
            Cljc *key = eval(scope, spec);
            Cljc *v = NIL;
            bool found = value != NIL && value->tag == CLJC_MAP && map_find(value, key, &v);
            if (!found && defaults && defaults->tag == CLJC_MAP && k->tag == CLJC_SYMBOL) {
                Cljc *d;
                if (map_find(defaults, k, &d)) v = eval(scope, d);
            }
            destructure(scope, k, v);
        }
        return;
    }
    cljc_error("unsupported binding form");
}

/* Bind an arity's params to argv. Each param may be any destructuring
 * pattern; '&' collects the remaining args as a (freshly built) list. */
/* [& {:keys [a b]}] kwargs: turn the trailing args (k1 v1 k2 v2 ..) into a map
 * so the map pattern can bind them. A lone trailing map is used as-is (the
 * Clojure 1.11 (f {:a 1}) form). */
static Cljc *kvseq_to_map(Cljc *s) {
    if (s != NIL && s->tag == CLJC_LIST && s->as.cons.tail == NIL &&
        s->as.cons.head != NIL && s->as.cons.head->tag == CLJC_MAP)
        return s->as.cons.head;
    Cljc *m = mk_map();
    for (; s != NIL && s->tag == CLJC_LIST && s->as.cons.tail != NIL &&
           s->as.cons.tail->tag == CLJC_LIST;
         s = s->as.cons.tail->as.cons.tail)
        m = map_assoc(m, s->as.cons.head, s->as.cons.tail->as.cons.head);
    return m;
}

static void bind_params(CljcEnv *call, Cljc *params, Cljc **argv, int nargs) {
    Cljc *p = params;
    int i = 0;
    while (p && p->tag == CLJC_LIST) {
        Cljc *pat = p->as.cons.head;
        if (pat->tag == CLJC_SYMBOL && pat->as.sym == sym_amp()) {
            Cljc *restl = NIL, **t = &restl;
            for (int j = i; j < nargs; j++) {
                *t = mk_cons(argv[j], NIL);
                t = &(*t)->as.cons.tail;
            }
            Cljc *restpat = p->as.cons.tail->as.cons.head;
            destructure(call, restpat,
                        (restpat != NIL && restpat->tag == CLJC_MAP)
                        ? kvseq_to_map(restl) : restl);
            return;
        }
        if (i >= nargs) cljc_error("not enough arguments");
        destructure(call, pat, argv[i++]);
        p = p->as.cons.tail;
    }
    if (i < nargs) cljc_error("too many arguments");
}

static void arity_info(Cljc *params, size_t *fixed, bool *variadic) {
    *fixed = 0; *variadic = false;
    for (Cljc *p = params; p && p->tag == CLJC_LIST; p = p->as.cons.tail) {
        Cljc *h = p->as.cons.head;
        if (h->tag == CLJC_SYMBOL && h->as.sym == sym_amp()) { *variadic = true; return; }
        (*fixed)++;
    }
}

/* Symbol resolution with per-cell caching: locals (slot envs) shadow;
 * root hits memoize a stable Binding* on the symbol cell. Shared by the
 * tree-walker and the VM's VOP_SYM. */
/* Root binding lookup: home-ns qualification first (library isolation),
 * then bare, then the (require :as) alias fallback. Shared by the
 * tree-walker's resolve_symbol and the compiler's vm_resolve_maybe —
 * copy-drift between those two paths already shipped one bug. */
static Binding *root_find(CljcEnv *root, const char *name, const char *home_ns) {
    if (home_ns) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s", home_ns, name);
        const char *qual = intern(buf, strlen(buf));
        for (Binding *b = root->bindings; b; b = b->next)
            if (b->name == qual) return b;
        /* refer alias: <home-ns>/name -> <source-ns>/name (shared global) */
        for (int i = 0; i < n_refers; i++)
            if (refer_from[i] == qual)
                for (Binding *b = root->bindings; b; b = b->next)
                    if (b->name == refer_to[i]) return b;
    }
    for (Binding *b = root->bindings; b; b = b->next)
        if (b->name == name) return b;
    /* refer alias on an already-qualified name */
    for (int i = 0; i < n_refers; i++)
        if (refer_from[i] == name)
            for (Binding *b = root->bindings; b; b = b->next)
                if (b->name == refer_to[i]) return b;
    /* clojure.core / cljs.core / clojure.lang qualified names resolve to the
     * bare global (cljc's core lives in the flat global namespace). Lets code
     * (and macroexpansions) that fully-qualify core refer to cljc's builtins. */
    {
        const char *slash = strchr(name, '/');
        if (slash && slash != name) {
            size_t plen = (size_t)(slash - name);
            if ((plen == 12 && !memcmp(name, "clojure.core", 12)) ||
                (plen == 9  && !memcmp(name, "cljs.core", 9)) ||
                (plen == 12 && !memcmp(name, "clojure.lang", 12))) {
                const char *bare = intern(slash + 1, strlen(slash + 1));
                for (Binding *b = root->bindings; b; b = b->next)
                    if (b->name == bare) return b;
            }
        }
    }
    /* alias fallback: m/foo -> <full-ns>/foo -> bare foo; the qualified
     * def must win over any bare global (two passes) */
    {
        const char *slash = strchr(name, '/');
        if (slash && slash != name) {
            const char *pre = intern(name, (size_t)(slash - name));
            const char *full = alias_lookup(pre, home_ns);
            if (full) {
                char buf[256];
                snprintf(buf, sizeof buf, "%s/%s", full, slash + 1);
                const char *qual = intern(buf, strlen(buf));
                const char *bare = intern(slash + 1, strlen(slash + 1));
                for (Binding *b = root->bindings; b; b = b->next)
                    if (b->name == qual) return b;
                for (Binding *b = root->bindings; b; b = b->next)
                    if (b->name == bare) return b;
            }
        }
    }
    /* JVM-qualified type/ctor name (sci.lang.Var, sci.lang.Var., java.io.Writer):
     * cljc stores a deftype's name/ctor as ns/Name (slash) via home-ns stamping,
     * so map the "." before the Capitalized class segment to "/" and retry. */
    if (!strchr(name, '/')) {
        for (const char *p = name; *p; p++) {
            if (*p == '.' && p[1] >= 'A' && p[1] <= 'Z') {
                size_t pre = (size_t)(p - name);
                char buf[256];
                if (pre + strlen(p) < sizeof buf) {
                    memcpy(buf, name, pre);
                    buf[pre] = '/';
                    strcpy(buf + pre + 1, p + 1);
                    const char *q = intern(buf, strlen(buf));
                    for (Binding *b = root->bindings; b; b = b->next)
                        if (b->name == q) return b;
                }
                break;
            }
        }
    }
    /* a qualified constructor name (pkg.Class.) resolves to the bare Class.
     * ctor — cljc registers host constructors under the short class name. */
    {
        size_t ln = strlen(name);
        if (ln > 1 && name[ln - 1] == '.') {
            const char *lastdot = NULL;
            for (const char *p = name; p < name + ln - 1; p++)
                if (*p == '.') lastdot = p;
            if (lastdot) {
                const char *shortn = intern(lastdot + 1, strlen(lastdot + 1));
                for (Binding *b = root->bindings; b; b = b->next)
                    if (b->name == shortn) return b;
            }
        }
    }
    /* Host-class fallback: an unresolved DOTTED, Capitalized-last-segment name
     * (e.g. sci.lang.IVar, java.io.Writer) is a JVM class cljc has no instance
     * of. Synthesize (and memoize) a keyword token so references to it resolve
     * — protocol extensions / prefer-method / instance? on it become harmless
     * no-ops. (Bare Capitalized names like Object are stubbed explicitly.) */
    {
        const char *dot = strrchr(name, '.');
        if (dot && dot[1] >= 'A' && dot[1] <= 'Z' && !strchr(name, '/')) {
            env_define(root, name, mk_kw(name));
            for (Binding *b = root->bindings; b; b = b->next)
                if (b->name == name) return b;
        }
    }
    return NULL;
}

/* True if `name` is a LOCAL binding in env — a let/loop/fn/letfn local shadowing
 * a special form (Clojure lets (letfn [(loop [..] ..)] (loop ..)) call the local).
 * Only consulted for the function-like special forms loop/let/fn, never the hot
 * syntactic ones (if/when/cond/..), so ordinary code pays nothing. */
static bool head_shadowed(CljcEnv *env, const char *name) {
    for (CljcEnv *e = env; e->parent; e = e->parent)
        if (env_local_find(e, name)) return true;
    return false;
}

static Cljc *resolve_symbol(CljcEnv *env, Cljc *form) {
    const char *name = form->as.symc.name;
    CljcEnv *e = env;
    for (; e->parent; e = e->parent) {
        Cljc **p = env_local_find(e, name);
        if (p) return *p;
    }
    Binding *cb = form->as.symc.root_cache;
    if (cb) return cb->value;
    Binding *b = root_find(e, name, form->as.symc.home_ns);
    if (b) { form->as.symc.root_cache = b; return b->value; }
    err_token = name;
    cljc_error("I don't know what `%s` refers to.", name);
    return NIL;
}

/* ───── Bytecode VM ──────────────────────────────────────────────────────
 *
 * Each interpreted fn arity compiles its body to a chunk on first call
 * (cached in the arity cell's meta slot). The compiler covers the hot
 * forms — if/do/let/loop/recur/and/or/when/cond, calls, literals,
 * vectors, closures, lazy-seq — and falls back two ways: VOP_EVAL
 * (tree-walk this sub-form) for rare forms, or c->ok=false (the whole
 * arity tree-walks forever) for shapes the ops can't express — recur
 * escaping through a tree-walked subform, malformed if/when/cond,
 * >255-ary calls. Correct without being complete. Macros expand at
 * compile time through apply with the expansion spliced into the source
 * (like eval); a callee that turns out to be a macro only at runtime
 * (forward ref, fn->macro redef) makes VOP_CALL deopt to eval of the
 * call-site form.
 *
 * The operand stack is the GC-rooted vstack; locals stay in slot-envs
 * (closures capture them unchanged); VOP_SYM resolves through the same
 * symbol cells, so root_cache caching carries over. Error traces inside
 * compiled bodies are coarser (no per-subform eval_stack frames). */

static Cljc *make_fn(CljcEnv *env, Cljc *forms, bool is_macro);
static void print_to(SBuf *sb, Cljc *v, bool readably);

/* Instruction = arg:24 | op:8. Sub-packings: CALL arg = call-site form
 * const:16 | argc:8; REBIND arg = unwind-depth:8 | names const:16.
 * Hence vm_compile's bails: ncode <= 0xffffff, nconst <= 0xffff. */
enum {
    VOP_NIL, VOP_TRUE, VOP_FALSE, VOP_CONST, VOP_SYM, VOP_LOCAL,
    VOP_BIND, VOP_DESTRUCT, VOP_NEWENV, VOP_POPENV, VOP_POP,
    VOP_CALL, VOP_JMP, VOP_JMPF, VOP_JMPF_KEEP, VOP_JMPT_KEEP,
    VOP_EVAL, VOP_CLOSURE, VOP_LAZY, VOP_VEC,
    VOP_REBIND, VOP_RECURFN, VOP_TAILCALL, VOP_RET
};

/* Lexical addressing: every simple-symbol binding records the frame it
 * lives in and its slot index within that frame, so a reference compiles
 * to VOP_LOCAL (depth, slot) — `depth` parent hops then sval[slot] — with
 * no name comparison or chain scan. A frame goes "opaque" (no indexing for
 * its own vars) the moment a destructuring bind makes the runtime slot
 * count unpredictable, or it overflows ENV_SLOTS into the binding chain;
 * those vars fall back to name-based VOP_SYM. Enclosing indexable frames
 * stay indexable — opaqueness never changes the env-chain shape, only
 * whether a var is reachable by slot. Closures compile separate chunks, so
 * captured (free) vars are never found here and resolve by name, unchanged. */
#define VMC_MAXFRAME 64
typedef struct {
    uint32_t *code; uint32_t ncode, ccap;
    Cljc **consts; uint32_t nconst, kcap;
    struct { const char *name; int16_t frame; int16_t slot; } locals[256];
    int nlocals;                               /* compile-time scope names */
    int frame;                                 /* current frame nesting    */
    uint8_t frame_n[VMC_MAXFRAME];             /* slots used per frame      */
    bool frame_opaque[VMC_MAXFRAME];           /* indexing off for frame    */
    int loop_pc;                               /* innermost loop target    */
    Cljc *loop_names;                          /* its binding symbols      */
    uint32_t loop_nbind;
    int loop_depth;                            /* NEWENVs since loop entry */
    bool ok;
} VmC;

/* Back-patch a forward jump's 24-bit target to the current pc. */
static void vmc_patch(VmC *c, uint32_t at) {
    if (!c->ok) return;
    c->code[at] = (c->code[at] & 0xff) | (c->ncode << 8);
}

static void vmc_emit(VmC *c, uint8_t op, uint32_t arg) {
    if (!c->ok) return;                /* doomed chunk: stop growing it */
    if (c->ncode >= c->ccap) {
        c->ccap = c->ccap ? c->ccap * 2 : 64;
        c->code = xrealloc(c->code, sizeof(uint32_t) * c->ccap);
    }
    c->code[c->ncode++] = (uint32_t)op | (arg << 8);
}

static uint32_t vmc_const(VmC *c, Cljc *v) {
    if (!c->ok) return 0;              /* doomed chunk: no vstack growth */
    for (uint32_t i = 0; i < c->nconst; i++)      /* dedup (small pools) */
        if (c->consts[i] == v) return i;
    if (c->nconst >= c->kcap) {
        c->kcap = c->kcap ? c->kcap * 2 : 16;
        c->consts = xrealloc(c->consts, sizeof(Cljc *) * c->kcap);
    }
    vpush(v);                  /* GC root until the chunk cell owns them */
    c->consts[c->nconst] = v;
    return c->nconst++;
}

static bool vmc_local_p(VmC *c, const char *name) {
    for (int i = c->nlocals - 1; i >= 0; i--)
        if (c->locals[i].name == name) return true;
    return false;
}

/* Record a name in the current frame. `simple` symbols claim the next slot
 * (and stay indexable) until the frame goes opaque or fills ENV_SLOTS;
 * everything else records slot -1 (name-based resolution). */
static void vmc_bind_name(VmC *c, const char *name, bool simple) {
    if (c->nlocals >= 256) { c->ok = false; return; }   /* chunk bails */
    int slot = -1;
    if (c->frame < VMC_MAXFRAME) {
        if (simple && !c->frame_opaque[c->frame] &&
            c->frame_n[c->frame] < ENV_SLOTS) {
            slot = c->frame_n[c->frame]++;
        } else if (simple) {
            /* overflowed ENV_SLOTS into the binding chain — the slot
             * counter can no longer be trusted for this frame */
            c->frame_opaque[c->frame] = true;
        }
    }
    c->locals[c->nlocals].name = name;
    c->locals[c->nlocals].frame = (int16_t)c->frame;
    c->locals[c->nlocals].slot = (int16_t)slot;
    c->nlocals++;
}

/* names bound by a destructuring pattern (slot -1: order/count of the
 * runtime env_defines isn't statically known, so never indexed) */
static void vmc_pattern_locals(VmC *c, Cljc *pat) {
    if (pat == NIL) return;
    if (pat->tag == CLJC_SYMBOL) { vmc_bind_name(c, pat->as.sym, false); return; }
    if (pat->tag == CLJC_VECTOR)
        for (size_t i = 0; i < vec_len(pat); i++)
            vmc_pattern_locals(c, vec_nth(pat, i));
    if (pat->tag == CLJC_MAP)
        for (Cljc *e = map_entry_list(pat); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *k = e->as.cons.head->as.cons.head;
            Cljc *v = e->as.cons.head->as.cons.tail;
            if (k->tag == CLJC_KEYWORD) vmc_pattern_locals(c, v);
            else vmc_pattern_locals(c, k);     /* {sym :key} */
        }
}

/* Bind a let/param pattern: simple symbol → indexable slot; anything else
 * → the frame goes opaque and its names resolve by name. */
static void vmc_bind(VmC *c, Cljc *pat) {
    if (pat != NIL && pat->tag == CLJC_SYMBOL) {
        vmc_bind_name(c, pat->as.sym, true);
    } else {
        if (c->frame < VMC_MAXFRAME) c->frame_opaque[c->frame] = true;
        vmc_pattern_locals(c, pat);
    }
}

/* Enter/leave a runtime env frame (one VOP_NEWENV / VOP_POPENV). */
static void vmc_frame_enter(VmC *c) {
    c->frame++;
    if (c->frame < VMC_MAXFRAME) {
        c->frame_n[c->frame] = 0;
        c->frame_opaque[c->frame] = false;
    }
}
static void vmc_frame_leave(VmC *c) { c->frame--; }

static bool vm_special_name(const char *s) {
    /* must stay in sync with eval_inner's special forms (no shared
     * list); the first 11 are handled explicitly in vmc_form before
     * this is consulted — listed anyway as drift insurance */
    static const char *names[24];
    if (!names[0]) {
        static const char *txt[24] = {
            "quote", "if", "do", "def", "let", "fn", "loop", "recur",
            "and", "or", "when", "cond", "defn", "defmacro", "quasiquote",
            "try", "catch", "finally", "ns", "use", "import", "binding",
            "with-redefs", "**reader-splice**"};
        for (int i = 0; i < 24; i++) names[i] = intern(txt[i], strlen(txt[i]));
    }
    for (int i = 0; i < 24; i++)
        if (s == names[i]) return true;
    return false;
}

/* Compile-time macro lookup: NULL when the head is shadowed by any
 * local in the compile env's chain (closure captures included — the
 * lexical shadow structure is identical for every closure sharing the
 * arities), else the full root resolution (home_ns + alias paths, the
 * same ones resolve_symbol takes) without erroring. */
static Cljc *vm_resolve_maybe(CljcEnv *env, Cljc *sym) {
    const char *name = sym->as.symc.name;
    CljcEnv *e = env;
    for (; e->parent; e = e->parent)
        if (env_local_find(e, name)) return NULL;     /* local shadows */
    Binding *cb = sym->as.symc.root_cache;
    if (cb) return cb->value;
    Binding *b = root_find(e, name, sym->as.symc.home_ns);
    return b ? b->value : NULL;
}

/* Does this form contain a (recur ...) that would belong to the
 * enclosing loop? Skips nested fn/loop/lazy-seq (they own their recur).
 * Compile-time only — guards VOP_EVAL emission inside compiled loops. */
static bool vmc_contains_recur(Cljc *form) {
    static const char *S_R, *S_F, *S_L, *S_LZ;
    if (!S_R) {
        S_R = intern("recur", 5); S_F = intern("fn", 2);
        S_L = intern("loop", 4);  S_LZ = intern("lazy-seq", 8);
    }
    if (form == NIL || form == NULL) return false;
    if (form->tag == CLJC_SYMBOL) return form->as.sym == S_R;
    if (form->tag == CLJC_VECTOR) {
        for (size_t i = 0; i < vec_len(form); i++)
            if (vmc_contains_recur(vec_nth(form, i))) return true;
        return false;
    }
    if (form->tag != CLJC_LIST) return false;
    Cljc *h = form->as.cons.head;
    if (h != NIL && h->tag == CLJC_SYMBOL &&
        (h->as.sym == S_F || h->as.sym == S_L || h->as.sym == S_LZ))
        return false;
    for (Cljc *l = form; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        if (vmc_contains_recur(l->as.cons.head)) return true;
    return false;
}

/* tail = this form's value is the enclosing fn's result (so a call here is a
 * proper tail call → VOP_TAILCALL, which trampolines in apply instead of
 * recursing). Propagated through if/do/let/loop/when/cond result positions. */
/* clojure.core/let, cljs.core/let, or <alias>/let (alias naming a *.core ns)
 * all name the bare special form `let`. Returns the bare name for such a head,
 * else the name unchanged. Lets qualified core specials dispatch as specials. */
static const char *strip_core_qualifier(const char *s) {
    const char *slash = strchr(s, '/');
    if (!slash || slash == s) return s;
    size_t plen = (size_t)(slash - s);
    bool core = (plen == 12 && !memcmp(s, "clojure.core", 12)) ||
                (plen == 9  && !memcmp(s, "cljs.core", 9));
    if (!core) {
        const char *full = alias_lookup(intern(s, plen), NULL);
        if (full) core = !strcmp(full, "clojure.core") || !strcmp(full, "cljs.core");
    }
    return core ? intern(slash + 1, strlen(slash + 1)) : s;
}

static void vmc_form(VmC *c, CljcEnv *cenv, Cljc *form, bool tail);

static void vmc_body(VmC *c, CljcEnv *cenv, Cljc *body, bool tail) {  /* do semantics */
    if (body == NIL || body->tag != CLJC_LIST) { vmc_emit(c, VOP_NIL, 0); return; }
    for (Cljc *b = body; b && b->tag == CLJC_LIST && c->ok; b = b->as.cons.tail) {
        bool last = b->as.cons.tail == NIL || b->as.cons.tail->tag != CLJC_LIST;
        vmc_form(c, cenv, b->as.cons.head, tail && last);  /* only the last form is tail */
        if (!last) vmc_emit(c, VOP_POP, 0);
    }
}

static void vmc_form(VmC *c, CljcEnv *cenv, Cljc *form, bool tail) {
    if (!c->ok) return;
    if (form == NIL) { vmc_emit(c, VOP_NIL, 0); return; }
    switch (form->tag) {
        case CLJC_BOOL:
            vmc_emit(c, form->as.b ? VOP_TRUE : VOP_FALSE, 0);
            return;
        case CLJC_INT: case CLJC_DOUBLE: case CLJC_STRING: case CLJC_CHAR:
        case CLJC_KEYWORD: case CLJC_BIGINT: case CLJC_RATIO:
        case CLJC_FN: case CLJC_NATIVE: case CLJC_MAP: case CLJC_SET:
            /* map/set literals with computed elements are rare in hot
             * bodies; constant ones are common — non-constant fall back */
            if (form->tag == CLJC_MAP || form->tag == CLJC_SET) {
                vmc_emit(c, VOP_EVAL, vmc_const(c, form));
                return;
            }
            vmc_emit(c, VOP_CONST, vmc_const(c, form));
            return;
        case CLJC_SYMBOL: {
            const char *nm = form->as.sym;
            for (int i = c->nlocals - 1; i >= 0; i--) {
                if (c->locals[i].name != nm) continue;
                int slot = c->locals[i].slot;
                int depth = c->frame - c->locals[i].frame;
                if (slot >= 0 && depth >= 0 && depth <= 0xffff) {
                    vmc_emit(c, VOP_LOCAL, (uint32_t)slot | ((uint32_t)depth << 8));
                    return;
                }
                break;                   /* shadowed but not indexable */
            }
            vmc_emit(c, VOP_SYM, vmc_const(c, form));
            return;
        }
        case CLJC_VECTOR: {
            size_t n = vec_len(form);
            if (n > 0xff) { vmc_emit(c, VOP_EVAL, vmc_const(c, form)); return; }
            for (size_t i = 0; i < n; i++) vmc_form(c, cenv, vec_nth(form, i), false);
            vmc_emit(c, VOP_VEC, (uint32_t)n);
            return;
        }
        case CLJC_LAZY:
            form = to_seq(form);
            if (form == NIL || form->tag != CLJC_LIST) { vmc_form(c, cenv, form, tail); return; }
            break;  /* fall through to list handling */
        case CLJC_LIST:
            break;
        default:
            vmc_emit(c, VOP_EVAL, vmc_const(c, form));
            return;
    }
    /* list form — realize lazy tails up front, like eval */
    {
        Cljc *l = form;
        while (l->tag == CLJC_LIST) l = l->as.cons.tail;
        if (l != NIL && l->tag == CLJC_LAZY) form = to_seq(form);
    }
    Cljc *head = form->as.cons.head;
    Cljc *rest = form->as.cons.tail;
    if (head != NIL && head->tag == CLJC_SYMBOL) {
        const char *s = strip_core_qualifier(head->as.sym);
        /* (.-field obj) / an undefined (.foo x) field access: deopt to the
         * tree-walker, which resolves it as a deftype field read */
        if ((s[0] == '.' && s[1] == '-' && s[2]) ||
            (s[0] == '.' && s[1] && s[1] != '-' &&
             !root_find(env_root(cenv), s, NULL))) {
            vmc_emit(c, VOP_EVAL, vmc_const(c, form));
            return;
        }
        static const char *S_QUOTE, *S_IF, *S_DO, *S_LET, *S_FN, *S_LOOP,
                          *S_RECUR, *S_AND, *S_OR, *S_WHEN, *S_COND, *S_LAZY;
        if (!S_QUOTE) {
            S_QUOTE = intern("quote", 5); S_IF = intern("if", 2);
            S_DO = intern("do", 2);       S_LET = intern("let", 3);
            S_FN = intern("fn", 2);       S_LOOP = intern("loop", 4);
            S_RECUR = intern("recur", 5); S_AND = intern("and", 3);
            S_OR = intern("or", 2);       S_WHEN = intern("when", 4);
            S_COND = intern("cond", 4);   S_LAZY = intern("lazy-seq", 8);
        }
        if (s == S_QUOTE) {
            vmc_emit(c, VOP_CONST, vmc_const(c, rest->as.cons.head));
            return;
        }
        if (s == S_IF) {
            if (rest == NIL || rest->tag != CLJC_LIST ||
                rest->as.cons.tail == NIL) { c->ok = false; return; }
            Cljc *cond = rest->as.cons.head;
            Cljc *then = rest->as.cons.tail != NIL ? rest->as.cons.tail->as.cons.head : NIL;
            Cljc *els = rest->as.cons.tail != NIL && rest->as.cons.tail->as.cons.tail != NIL
                        ? rest->as.cons.tail->as.cons.tail->as.cons.head : NIL;
            vmc_form(c, cenv, cond, false);
            uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
            vmc_form(c, cenv, then, tail);
            uint32_t je = c->ncode; vmc_emit(c, VOP_JMP, 0);
            vmc_patch(c, jf);
            vmc_form(c, cenv, els, tail);
            vmc_patch(c, je);
            return;
        }
        if (s == S_DO) { vmc_body(c, cenv, rest, tail); return; }
        if (s == S_WHEN) {
            if (rest == NIL || rest->tag != CLJC_LIST) { c->ok = false; return; }
            vmc_form(c, cenv, rest->as.cons.head, false);
            uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
            vmc_body(c, cenv, rest->as.cons.tail, tail);
            uint32_t je = c->ncode; vmc_emit(c, VOP_JMP, 0);
            vmc_patch(c, jf);
            vmc_emit(c, VOP_NIL, 0);
            vmc_patch(c, je);
            return;
        }
        if (s == S_AND || s == S_OR) {
            if (rest == NIL || rest->tag != CLJC_LIST) {
                vmc_emit(c, s == S_AND ? VOP_TRUE : VOP_NIL, 0);
                return;
            }
            uint32_t jumps[128]; int nj = 0;
            for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                vmc_form(c, cenv, a->as.cons.head, false);
                if (a->as.cons.tail != NIL && a->as.cons.tail->tag == CLJC_LIST) {
                    if (nj >= 128) { c->ok = false; return; }
                    jumps[nj++] = c->ncode;
                    vmc_emit(c, s == S_AND ? VOP_JMPF_KEEP : VOP_JMPT_KEEP, 0);
                    vmc_emit(c, VOP_POP, 0);
                }
            }
            for (int i = 0; i < nj; i++) vmc_patch(c, jumps[i]);
            return;
        }
        if (s == S_COND) {
            uint32_t ends[128]; int ne = 0;
            Cljc *a = rest;
            while (a && a->tag == CLJC_LIST && a->as.cons.tail != NIL &&
                   a->as.cons.tail->tag == CLJC_LIST) {
                vmc_form(c, cenv, a->as.cons.head, false);
                uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
                vmc_form(c, cenv, a->as.cons.tail->as.cons.head, tail);
                if (ne >= 128) { c->ok = false; return; }
                ends[ne++] = c->ncode; vmc_emit(c, VOP_JMP, 0);
                vmc_patch(c, jf);
                a = a->as.cons.tail->as.cons.tail;
            }
            if (a != NIL && a->tag == CLJC_LIST) { c->ok = false; return; }
            vmc_emit(c, VOP_NIL, 0);
            for (int i = 0; i < ne; i++) vmc_patch(c, ends[i]);
            return;
        }
        if (s == S_LET) {
            Cljc *bv = rest->as.cons.head;
            if (bv == NIL || bv->tag != CLJC_VECTOR || vec_len(bv) % 2 != 0) {
                c->ok = false; return;
            }
            int save_locals = c->nlocals;
            c->loop_depth++;
            vmc_emit(c, VOP_NEWENV, 0);
            vmc_frame_enter(c);
            for (size_t i = 0; i < vec_len(bv); i += 2) {
                Cljc *pat = vec_nth(bv, i);
                vmc_form(c, cenv, vec_nth(bv, i + 1), false);
                if (pat != NIL && pat->tag == CLJC_SYMBOL)
                    vmc_emit(c, VOP_BIND, vmc_const(c, pat));
                else
                    vmc_emit(c, VOP_DESTRUCT, vmc_const(c, pat));
                vmc_bind(c, pat);
            }
            vmc_body(c, cenv, rest->as.cons.tail, tail);
            vmc_emit(c, VOP_POPENV, 0);
            vmc_frame_leave(c);
            c->loop_depth--;
            c->nlocals = save_locals;
            return;
        }
        if (s == S_LOOP) {
            Cljc *bv = rest->as.cons.head;
            if (bv == NIL || bv->tag != CLJC_VECTOR || vec_len(bv) % 2 != 0) {
                c->ok = false; return;
            }
            for (size_t i = 0; i < vec_len(bv); i += 2)
                if (vec_nth(bv, i)->tag != CLJC_SYMBOL) {
                    /* pattern loops: let eval desugar+splice at runtime */
                    vmc_emit(c, VOP_EVAL, vmc_const(c, form));
                    return;
                }
            int save_locals = c->nlocals;
            int save_pc = c->loop_pc;
            Cljc *save_names = c->loop_names;
            uint32_t save_nb = c->loop_nbind;
            int save_depth = c->loop_depth;
            vmc_emit(c, VOP_NEWENV, 0);
            vmc_frame_enter(c);
            Cljc *names = NIL, **t = &names;
            for (size_t i = 0; i < vec_len(bv); i += 2) {
                Cljc *sym = vec_nth(bv, i);
                vmc_form(c, cenv, vec_nth(bv, i + 1), false);
                vmc_emit(c, VOP_BIND, vmc_const(c, sym));
                vmc_bind_name(c, sym->as.sym, true);
                *t = mk_cons(sym, NIL);
                t = &(*t)->as.cons.tail;
            }
            c->loop_names = names;
            vmc_const(c, names);              /* root it on the vstack */
            c->loop_nbind = (uint32_t)(vec_len(bv) / 2);
            c->loop_pc = (int)c->ncode;
            c->loop_depth = 0;
            vmc_body(c, cenv, rest->as.cons.tail, tail);
            vmc_emit(c, VOP_POPENV, 0);
            vmc_frame_leave(c);
            c->loop_pc = save_pc;
            c->loop_names = save_names;
            c->loop_nbind = save_nb;
            c->loop_depth = save_depth;
            c->nlocals = save_locals;
            return;
        }
        if (s == S_RECUR) {
            uint32_t n = 0;
            for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                vmc_form(c, cenv, a->as.cons.head, false);
                n++;
            }
            if (c->loop_pc >= 0) {
                if (n != c->loop_nbind) { c->ok = false; return; }
                if (c->loop_depth > 0xff) { c->ok = false; return; }
                vmc_emit(c, VOP_REBIND,
                         vmc_const(c, c->loop_names) | ((uint32_t)c->loop_depth << 16));
                vmc_emit(c, VOP_JMP, (uint32_t)c->loop_pc);
            } else {
                if (n > 0xff) { c->ok = false; return; }  /* recur.n is u8 */
                vmc_emit(c, VOP_RECURFN, n);
                vmc_emit(c, VOP_RET, 0);
            }
            return;
        }
        if (s == S_FN) {
            /* named fns get name-stripping in the tree-walk path */
            if (rest != NIL && rest->as.cons.head->tag == CLJC_SYMBOL) {
                vmc_emit(c, VOP_EVAL, vmc_const(c, form));
                return;
            }
            vmc_emit(c, VOP_CLOSURE, vmc_const(c, rest));
            return;
        }
        if (s == S_LAZY) {
            /* thunk forms = ([] body...) — same shape SYM_LAZY_SEQ builds */
            vmc_emit(c, VOP_LAZY, vmc_const(c, mk_cons(mk_empty_vec(), rest)));
            return;
        }
        if (vm_special_name(s)) {            /* def/try/quasiquote/ns/... */
            /* a recur for OUR loop inside a tree-walked subform would
             * surface as a sentinel value and rebind the fn params —
             * tree-walk the whole body instead (rare; (try (recur))
             * is illegal in JVM Clojure anyway) */
            if (c->loop_pc >= 0 && vmc_contains_recur(form)) { c->ok = false; return; }
            vmc_emit(c, VOP_EVAL, vmc_const(c, form));
            return;
        }
        /* compile-time macro expansion, spliced like eval does;
         * any local shadowing the name suppresses it (vm_resolve_maybe
         * scans the compile env chain AND c's own body locals below) */
        Cljc *mfn = vmc_local_p(c, s) ? NULL : vm_resolve_maybe(cenv, head);
        if (mfn && mfn->tag == CLJC_FN && mfn->as.fn.is_macro) {
            size_t base = vsp;
            for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
                vpush(a->as.cons.head);
            Cljc *expansion = apply(cenv, mfn, &vstack[base], (int)(vsp - base));
            vsp = base;
            if (expansion != NIL && expansion->tag == CLJC_LIST) {
                form->as.cons.head = expansion->as.cons.head;
                form->as.cons.tail = expansion->as.cons.tail;
            }
            vmc_const(c, expansion);          /* keep rooted */
            vmc_form(c, cenv, expansion, tail);
            return;
        }
    }
    /* ordinary call: head expr, args, CALL n. The arg also carries the
     * call-site form's const index so a callee that turns out to be a
     * macro at runtime (forward ref, fn->macro redef) deopts to eval.
     * In tail position emit VOP_TAILCALL, which trampolines in apply
     * (replaces the frame) instead of recursing — proper tail calls. */
    {
        uint32_t n = 0;
        vmc_form(c, cenv, head, false);
        for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
            vmc_form(c, cenv, a->as.cons.head, false);
            n++;
        }
        if (n > 0xff) { c->ok = false; return; }
        vmc_emit(c, tail ? VOP_TAILCALL : VOP_CALL, n | (vmc_const(c, form) << 8));
    }
}

/* Compile one arity body. Returns a chunk cell, or NULL (caller falls
 * back to the tree-walker permanently). */
static Cljc *vm_compile(CljcEnv *cenv, Cljc *params, Cljc *body) {
    VmC c;
    memset(&c, 0, sizeof c);
    c.ok = true;
    c.loop_pc = -1;
    size_t base = vsp;                 /* consts rooted here during build */
    for (Cljc *p = params; p && p->tag == CLJC_LIST; p = p->as.cons.tail) {
        Cljc *pe = p->as.cons.head;
        if (pe->tag == CLJC_SYMBOL && pe->as.sym == sym_amp()) {
            /* the rest binding lands in the next slot (one env_define);
             * a destructuring rest makes the param frame opaque */
            Cljc *tail = p->as.cons.tail;
            if (tail != NIL && tail->tag == CLJC_LIST)
                vmc_bind(&c, tail->as.cons.head);
            break;
        }
        vmc_bind(&c, pe);
    }
    vmc_body(&c, cenv, body, true);        /* the fn body is in tail position */
    vmc_emit(&c, VOP_RET, 0);
    if (!c.ok || c.ncode > 0xffffff || c.nconst > 0xffff) {
        if (getenv("CLJC_VM_LOG") && body != NIL && body->tag == CLJC_LIST) {
            SBuf sb = {0};
            print_to(&sb, body->as.cons.head, true);
            fprintf(stderr, "[vm] compile bail: %.80s%s\n",
                    sb.data ? sb.data : "", sb.len > 80 ? "…" : "");
            free(sb.data);
        }
        free(c.code); free(c.consts);
        vsp = base;
        return NULL;
    }
    Cljc *chunk = alloc(CLJC_CHUNK);   /* consts still vstack-rooted here */
    /* counted for byte-pressure consistency with vec tails / HAMT kids,
     * though chunks are effectively immortal once cached */
    gc_extra_bytes += sizeof(uint32_t) * c.ncode + sizeof(Cljc *) * c.nconst;
    chunk->as.chunk.code = c.code;
    chunk->as.chunk.consts = c.consts;
    chunk->as.chunk.ncode = c.ncode;
    chunk->as.chunk.nconst = (uint16_t)c.nconst;
    vsp = base;
    return chunk;
}

static Cljc *vm_run(CljcEnv *env_in, Cljc *chunk) {
    uint32_t *code = chunk->as.chunk.code;
    Cljc **K = chunk->as.chunk.consts;
    uint32_t pc = 0;
    size_t base = vsp;
    /* volatile: env and the chunk itself must stay visible to the
     * conservative GC scan — code/K are interior malloc pointers the
     * scan can't see, so the chunk cell is their only root here. */
    Cljc * volatile chunk_keep = chunk;
    (void)chunk_keep;
    CljcEnv * volatile env_keep = env_in;
    CljcEnv *env = env_in;
    uint32_t ins, a;
    /* Dispatch: threaded code (one indirect jump per op — far better branch
     * prediction than a switch's single shared jump) on GCC/Clang via
     * labels-as-values; a plain switch everywhere else. The op bodies below
     * are written once and shared by both via the VM_CASE/VM_NEXT macros. */
#if defined(__GNUC__)
    static const void *const vmlbl[] = {
        [VOP_NIL]=&&op_VOP_NIL, [VOP_TRUE]=&&op_VOP_TRUE, [VOP_FALSE]=&&op_VOP_FALSE,
        [VOP_CONST]=&&op_VOP_CONST, [VOP_SYM]=&&op_VOP_SYM, [VOP_LOCAL]=&&op_VOP_LOCAL,
        [VOP_BIND]=&&op_VOP_BIND, [VOP_DESTRUCT]=&&op_VOP_DESTRUCT, [VOP_NEWENV]=&&op_VOP_NEWENV,
        [VOP_POPENV]=&&op_VOP_POPENV, [VOP_POP]=&&op_VOP_POP, [VOP_CALL]=&&op_VOP_CALL,
        [VOP_JMP]=&&op_VOP_JMP, [VOP_JMPF]=&&op_VOP_JMPF, [VOP_JMPF_KEEP]=&&op_VOP_JMPF_KEEP,
        [VOP_JMPT_KEEP]=&&op_VOP_JMPT_KEEP, [VOP_EVAL]=&&op_VOP_EVAL, [VOP_CLOSURE]=&&op_VOP_CLOSURE,
        [VOP_LAZY]=&&op_VOP_LAZY, [VOP_VEC]=&&op_VOP_VEC, [VOP_REBIND]=&&op_VOP_REBIND,
        [VOP_RECURFN]=&&op_VOP_RECURFN, [VOP_TAILCALL]=&&op_VOP_TAILCALL, [VOP_RET]=&&op_VOP_RET,
    };
    #define VM_CASE(op) op_##op:
    #define VM_NEXT()   do { ins = code[pc++]; a = ins >> 8; goto *vmlbl[ins & 0xff]; } while (0)
    VM_NEXT();
#else
    #define VM_CASE(op) case op:
    #define VM_NEXT()   break
    for (;;) {
        ins = code[pc++];
        a = ins >> 8;
        switch (ins & 0xff) {
#endif
            VM_CASE(VOP_NIL)   vpush(NIL); VM_NEXT();
            VM_CASE(VOP_TRUE)  vpush(TRUE); VM_NEXT();
            VM_CASE(VOP_FALSE) vpush(FALSE); VM_NEXT();
            VM_CASE(VOP_CONST) vpush(K[a]); VM_NEXT();
            VM_CASE(VOP_SYM)   vpush(resolve_symbol(env, K[a])); VM_NEXT();
            VM_CASE(VOP_LOCAL) {
                /* lexical address: `depth` parent hops, then sval[slot] */
                CljcEnv *e = env;
                for (uint32_t d = a >> 8; d; d--) e = e->parent;
                vpush(e->sval[a & 0xff]);
                VM_NEXT();
            }
            VM_CASE(VOP_BIND)
                env_define(env, K[a]->as.sym, vstack[vsp - 1]);
                vsp--;
                VM_NEXT();
            VM_CASE(VOP_DESTRUCT)
                destructure(env, K[a], vstack[vsp - 1]);
                vsp--;
                VM_NEXT();
            VM_CASE(VOP_NEWENV) env = env_new(env); env_keep = env; VM_NEXT();
            VM_CASE(VOP_POPENV) env = env->parent; env_keep = env; VM_NEXT();
            VM_CASE(VOP_POP)    vsp--; VM_NEXT();
            VM_CASE(VOP_CALL) {
                uint32_t n = a & 0xff;
                Cljc *f = vstack[vsp - n - 1];
                Cljc *r;
                if (f != NIL && f->tag == CLJC_FN && f->as.fn.is_macro) {
                    /* late-resolved macro: deopt to eval of the source
                     * form (expands + splices, same as the tree-walk) */
                    if (getenv("CLJC_VM_LOG")) {
                        SBuf sb = {0};
                        print_to(&sb, K[a >> 8], true);
                        fprintf(stderr, "[vm] macro deopt: %.60s\n", sb.data ? sb.data : "");
                        free(sb.data);
                    }
                    vsp -= n + 1;
                    r = eval(env, K[a >> 8]);
                } else {
                    r = apply(env, f, &vstack[vsp - n], (int)n);
                    vsp -= n + 1;
                }
                vpush(r);
                VM_NEXT();
            }
            VM_CASE(VOP_JMP)  pc = a; VM_NEXT();
            VM_CASE(VOP_JMPF) if (!is_truthy(vstack[--vsp])) pc = a; VM_NEXT();
            VM_CASE(VOP_JMPF_KEEP) if (!is_truthy(vstack[vsp - 1])) pc = a; VM_NEXT();
            VM_CASE(VOP_JMPT_KEEP) if (is_truthy(vstack[vsp - 1])) pc = a; VM_NEXT();
            VM_CASE(VOP_EVAL) vpush(eval(env, K[a])); VM_NEXT();
            VM_CASE(VOP_CLOSURE) vpush(make_fn(env, K[a], false)); VM_NEXT();
            VM_CASE(VOP_LAZY) {
                Cljc *t = make_fn(env, K[a], false);
                Cljc *l = alloc(CLJC_LAZY);
                l->as.lazy.thunk = t;
                vpush(l);
                VM_NEXT();
            }
            VM_CASE(VOP_VEC) {
                Cljc *v = mk_vector(&vstack[vsp - a], a);
                vsp -= a;
                vpush(v);
                VM_NEXT();
            }
            VM_CASE(VOP_REBIND) {
                /* arg: low 16 = names-list const idx, high 8 = NEWENVs to
                 * unwind (recur from inside nested lets) */
                uint32_t k = a & 0xffff, depth = a >> 16;
                for (uint32_t d = 0; d < depth; d++) env = env->parent;
                env_keep = env;
                uint32_t n = 0;
                for (Cljc *nm = K[k]; nm && nm->tag == CLJC_LIST; nm = nm->as.cons.tail) n++;
                Cljc **vals = &vstack[vsp - n];
                uint32_t i = 0;
                for (Cljc *nm = K[k]; nm && nm->tag == CLJC_LIST; nm = nm->as.cons.tail, i++) {
                    Cljc **p = env_local_find(env, nm->as.cons.head->as.sym);
                    if (p) *p = vals[i];
                }
                vsp -= n;
                VM_NEXT();
            }
            VM_CASE(VOP_RECURFN) {
                Cljc *r = alloc(CLJC_RECUR);
                Cljc **vals = r->as.recur.iv;
                if (a > 3) {
                    vals = xmalloc(sizeof(Cljc *) * a);
                    r->as.recur.iv[0] = (Cljc *)vals;
                    r->as.recur.spill = true;
                }
                for (uint32_t i = 0; i < a; i++)
                    vals[i] = vstack[vsp - a + i];   /* no alloc: safe to */
                r->as.recur.n = (uint8_t)a;          /* set n once after */
                vsp -= a;
                vpush(r);
                VM_NEXT();
            }
            VM_CASE(VOP_TAILCALL) {
                /* (f a b) in tail position: build a recur sentinel carrying the
                 * args, with the TARGET fn in meta. apply's loop re-dispatches
                 * to it instead of recursing — a proper tail call. */
                uint32_t n = a & 0xff;
                Cljc *f = vstack[vsp - n - 1];
                if (f != NIL && f->tag == CLJC_FN && f->as.fn.is_macro) {
                    vsp -= n + 1;                    /* late macro: deopt, no TCO */
                    vpush(eval(env, K[a >> 8]));
                    VM_NEXT();
                }
                Cljc *r = alloc(CLJC_RECUR);         /* args still rooted on the vstack */
                Cljc **vals = r->as.recur.iv;
                if (n > 3) {
                    vals = xmalloc(sizeof(Cljc *) * n);
                    r->as.recur.iv[0] = (Cljc *)vals;
                    r->as.recur.spill = true;
                }
                for (uint32_t i = 0; i < n; i++) vals[i] = vstack[vsp - n + i];
                r->as.recur.n = (uint8_t)n;
                r->meta = f;                         /* tailcall target (NULL = self-recur) */
                vsp -= n + 1;                        /* drop args + fn */
                vpush(r);
                VM_NEXT();
            }
            VM_CASE(VOP_RET) {
                Cljc *r = vstack[vsp - 1];
                vsp = base;
                (void)env_keep;
                return r;
            }
#if !defined(__GNUC__)
        }
    }
#endif
    #undef VM_CASE
    #undef VM_NEXT
}

/* Decide once whether `fn` qualifies for the direct-slot fast call path:
 * exactly one arity, fixed (no &), every param a plain symbol, count within
 * the inline slots. Result cached on the cell (arities are immutable). */
static void fastcall_init(Cljc *fn) {
    fn->as.fn.fc_ready = true;
    fn->as.fn.fc_arity = NULL;
    fn->as.fn.fc_n = 0;
    Cljc *arities = fn->as.fn.arities;
    if (!arities || arities->tag != CLJC_LIST) return;
    if (arities->as.cons.tail != NIL) return;          /* multi-arity */
    Cljc *arity = arities->as.cons.head;
    int n = 0;
    for (Cljc *p = arity->as.cons.head; p && p->tag == CLJC_LIST; p = p->as.cons.tail) {
        Cljc *pe = p->as.cons.head;
        if (pe == NIL || pe->tag != CLJC_SYMBOL) return;   /* destructuring */
        if (pe->as.sym == sym_amp()) return;               /* variadic     */
        n++;
    }
    if (n > ENV_SLOTS) return;
    fn->as.fn.fc_arity = arity;
    fn->as.fn.fc_n = (uint8_t)n;
}

/* If `inst` is a deftype instance (:cljc/type-tagged map) whose type has a
 * method `mname` registered in cljc/multi-tables, call it with the instance
 * prepended to argv and store the result in *out; return true. Else false.
 * This is how cljc bridges to protocol methods a host would dispatch via an
 * interface — invoke (IFn), deref (IDeref), etc. */
static bool dispatch_deftype_method(CljcEnv *env, Cljc *inst, const char *mname,
                                    Cljc **argv, int nargs, Cljc **out) {
    if (inst == NIL || inst->tag != CLJC_MAP) return false;
    /* cache the :cljc/type keyword: this runs on every count/get-miss/assoc/nth,
     * so re-interning + re-allocating the keyword each time is pure waste. */
    static Cljc *KW_CLJC_TYPE;
    if (!KW_CLJC_TYPE) KW_CLJC_TYPE = mk_kw(intern("cljc/type", 9));
    Cljc *tykw;
    if (!map_find(inst, KW_CLJC_TYPE, &tykw)) return false;
    Binding *mtb = root_find(env_root(env), intern("cljc/deftype-methods", 20), NULL);
    if (!mtb || mtb->value == NIL || mtb->value->tag != CLJC_ATOM) return false;
    Cljc *tables = mtb->value->as.atom.value, *tab, *method;
    if (tables == NIL || tables->tag != CLJC_MAP) return false;
    if (!map_find(tables, mk_sym(intern(mname, strlen(mname))), &tab)) return false;
    if (tab == NIL || tab->tag != CLJC_MAP) return false;
    if (!map_find(tab, tykw, &method)) return false;
    Cljc *iargv[256];
    if (nargs + 1 > 256) cljc_error("too many args to %s", mname);
    iargv[0] = inst;
    for (int i = 0; i < nargs; i++) iargv[i + 1] = argv[i];
    *out = apply(env, method, iargv, nargs + 1);
    return true;
}

static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc **argv, int nargs) {
    cljc_check_stack();   /* raise a catchable error before the C stack overflows */
    /* NATIVE/FN are the overwhelmingly common cases — check them before VAR so
     * the hot path (every +, <, fn call) doesn't pay an extra branch. */
    if (fn->tag == CLJC_NATIVE) return fn->as.native(env, argv, nargs);
    if (fn->tag == CLJC_VAR) {     /* a Var is IFn: call its current value */
        Binding *b = root_find(env_root(env), fn->as.var.name, NULL);
        if (b && b->value != fn) return apply(env, b->value, argv, nargs);
        cljc_error("var is unbound or not callable");
    }
    if (fn->tag == CLJC_FN) {
        /* volatile: when recur swaps argv to the sentinel's (possibly heap-
         * spilled) value array, this slot is the cell's only GC root — the
         * optimizer must not elide it. */
        Cljc * volatile recur_keep = NIL;
        (void)recur_keep;
        for (;;) {
            if (!fn->as.fn.fc_ready) fastcall_init(fn);  /* fn can change via a tail call */
            Cljc *chosen;
            CljcEnv *call;
            if (fn->as.fn.fc_arity && (size_t)nargs == fn->as.fn.fc_n) {
                /* fast path: fill the frame's slots straight from argv, no
                 * arity_info walk and no bind_params/destructure calls. */
                chosen = fn->as.fn.fc_arity;
                call = env_new(fn->as.fn.env);
                Cljc *p = chosen->as.cons.head;
                for (int i = 0; i < nargs; i++) {
                    call->sname[i] = p->as.cons.head->as.sym;
                    call->sval[i] = argv[i];
                    p = p->as.cons.tail;
                }
                call->nslots = (uint8_t)nargs;
            } else {
                /* slow dispatch: exact param-count match wins; variadic is fallback. */
                Cljc *fallback = NULL;
                chosen = NULL;
                for (Cljc *ar = fn->as.fn.arities; ar && ar->tag == CLJC_LIST; ar = ar->as.cons.tail) {
                    Cljc *arity = ar->as.cons.head;
                    size_t fixed; bool variadic;
                    arity_info(arity->as.cons.head, &fixed, &variadic);
                    if (!variadic && (size_t)nargs == fixed) { chosen = arity; break; }
                    if (variadic && (size_t)nargs >= fixed && !fallback) fallback = arity;
                }
                if (!chosen) chosen = fallback;
                if (!chosen) {
                    /* name the fn and list the arities it DOES take, so the
                     * caller knows which call is wrong (not just the count). */
                    char ab[160]; int al = 0; ab[0] = '\0';
                    for (Cljc *ar = fn->as.fn.arities; ar && ar->tag == CLJC_LIST; ar = ar->as.cons.tail) {
                        size_t fx; bool va; arity_info(ar->as.cons.head->as.cons.head, &fx, &va);
                        int n = snprintf(ab + al, sizeof(ab) - al, "%s%zu%s",
                                         al ? " " : "", fx, va ? "+" : "");
                        if (n > 0 && al + n < (int)sizeof(ab)) al += n; else break;
                    }
                    const char *nm = NULL;
                    if (fn->meta && fn->meta->tag == CLJC_MAP) {
                        Cljc *o;
                        if (map_find(fn->meta, mk_kw(intern("name", 4)), &o) &&
                            o != NIL && o->tag == CLJC_SYMBOL) nm = o->as.sym;
                    }
                    cljc_error("no matching arity: %s called with %d arg%s (takes %s)",
                               nm ? nm : "fn", nargs, nargs == 1 ? "" : "s", ab);
                }
                call = env_new(fn->as.fn.env);
                bind_params(call, chosen->as.cons.head, argv, nargs);
            }
            /* first call compiles the body; failures pin TRUE = tree-walk.
             * TRUE is pinned BEFORE compiling: a throw mid-compilation
             * (macro expander erroring) must not retry-and-leak the
             * compiler's buffers on every subsequent call. */
            if (chosen->meta == NULL) {
                chosen->meta = TRUE;
                Cljc *ch = vm_compile(call, chosen->as.cons.head,
                                      chosen->as.cons.tail);
                if (ch) chosen->meta = ch;
            }
            Cljc *result = chosen->meta->tag == CLJC_CHUNK
                ? vm_run(call, chosen->meta)
                : eval_body(call, chosen->as.cons.tail);
            if (!(result && result->tag == CLJC_RECUR)) return result;
            /* recur/tailcall: the sentinel's value array IS the next argv. */
            recur_keep = result;   /* root the cell across the next iteration */
            argv = result->as.recur.spill
                ? (Cljc **)result->as.recur.iv[0] : result->as.recur.iv;
            nargs = (int)result->as.recur.n;
            if (result->meta) {    /* tail call to ANOTHER fn (meta = target) */
                Cljc *newfn = result->meta;
                if (newfn->tag == CLJC_FN) fn = newfn;         /* loop, replacing this frame */
                else {
                    /* native/keyword/map: dispatch here. NOT `return apply(...)`
                     * — that C tail call lets -O2 free this frame (and the
                     * volatile recur_keep rooting the sentinel that owns argv),
                     * so the GC could reclaim the args mid-call. Reading
                     * recur_keep after the call pins the sentinel live. */
                    Cljc *rv = apply(env, newfn, argv, nargs);
                    recur_keep = result;
                    return rv;
                }
            }
        }
    }
    Cljc *a0 = nargs > 0 ? argv[0] : NIL;
    Cljc *a1 = nargs > 1 ? argv[1] : NIL;
    /* Keywords as functions: (:key m) and (:key m default). */
    if (fn->tag == CLJC_KEYWORD) {
        if (nargs < 1 || nargs > 2) cljc_error("keyword lookup takes 1 or 2 args, got %d", nargs);
        Cljc *out;
        if (a0 != NIL && a0->tag == CLJC_MAP && map_find(a0, fn, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_MAP) {
        /* a deftype instance implementing `invoke` is callable (SCI's Var-as-fn
         * / fn objects); otherwise it's a plain map used as a fn: (m key dflt) */
        Cljc *out;
        if (dispatch_deftype_method(env, fn, "invoke", argv, nargs, &out)) return out;
        /* a reify keeps `invoke` per-instance in :cljc/impls (e.g. DataScript's
         * Comparator reify is called as a 2-arg fn) */
        Cljc *impls, *inv;
        if (map_find(fn, mk_kw(intern("cljc/impls", 10)), &impls) &&
            impls != NIL && impls->tag == CLJC_MAP &&
            map_find(impls, mk_sym(intern("invoke", 6)), &inv)) {
            Cljc *iargv[256]; iargv[0] = fn;
            if (nargs + 1 > 256) cljc_error("too many args to invoke");
            for (int i = 0; i < nargs; i++) iargv[i + 1] = argv[i];
            return apply(env, inv, iargv, nargs + 1);
        }
        if (nargs < 1 || nargs > 2) cljc_error("map lookup takes 1 or 2 args, got %d", nargs);
        if (map_find(fn, a0, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_SET) {
        if (nargs < 1 || nargs > 2) cljc_error("set lookup takes 1 or 2 args, got %d", nargs);
        Cljc *out;
        if (set_contains(fn, a0, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_SORTED) {
        if (nargs < 1 || nargs > 2) cljc_error("collection lookup takes 1 or 2 args, got %d", nargs);
        Cljc *out;
        if (sorted_get(env, fn, a0, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_VECTOR || fn->tag == CLJC_TVEC) {  /* transients too */
        if (a0->tag != CLJC_INT) cljc_error("vector lookup needs an integer index");
        if (a0->as.i < 0 || (size_t)a0->as.i >= vec_len(fn))
            cljc_error("index %lld out of bounds for length %zu", (long long)a0->as.i, vec_len(fn));
        return vec_nth(fn, (size_t)a0->as.i);
    }
    /* Symbols as functions: ('k m) and ('k m default), like keywords. */
    if (fn->tag == CLJC_SYMBOL) {
        if (nargs < 1 || nargs > 2) cljc_error("symbol lookup takes 1 or 2 args, got %d", nargs);
        Cljc *out;
        if (a0 != NIL && a0->tag == CLJC_MAP && map_find(a0, fn, &out)) return out;
        return a1;
    }
    cljc_error("%s is not callable as a function", val_type_name(fn));
    return NIL;
}

/* One arity: validate a [params] vector and pair it with its body. */
static Cljc *build_arity(Cljc *params_vec, Cljc *body) {
    /* a return-type hint reads as (with-meta [params] {..}) (reader turns ^{..}
     * into a with-meta call) — unwrap to the param vector. ^Symbol hints are
     * dropped by the reader already, so only the map-meta form reaches here. */
    if (params_vec != NIL && params_vec->tag == CLJC_LIST &&
        params_vec->as.cons.head->tag == CLJC_SYMBOL &&
        params_vec->as.cons.head->as.sym == intern("with-meta", 9) &&
        params_vec->as.cons.tail != NIL)
        params_vec = params_vec->as.cons.tail->as.cons.head;
    if (params_vec == NIL || params_vec->tag != CLJC_VECTOR)
        cljc_error("fn params must be a vector");
    Cljc *params = NIL, **t = &params;
    for (size_t i = 0; i < vec_len(params_vec); i++) {
        Cljc *p = peel_meta_sym(vec_nth(params_vec, i));  /* drop ^:foo / ^{..} on a param */
        if (p == NIL) continue;     /* a #?(:cljs ..)-only param elides to nil */
        bool is_amp = p->tag == CLJC_SYMBOL && p->as.sym == sym_amp();
        if (is_amp && i + 2 != vec_len(params_vec))
            cljc_error("fn params: & must be followed by exactly one binding");
        if (!is_amp && p->tag != CLJC_SYMBOL && p->tag != CLJC_VECTOR && p->tag != CLJC_MAP)
            cljc_error("fn params: unsupported binding form");
        *t = mk_cons(p, NIL);
        t = &(*t)->as.cons.tail;
    }
    /* {:pre [..] :post [..]} as the first of MULTIPLE body forms: assert each
     * :pre before the body, each :post after with % bound to the return value
     * — (assert p)... (let [% (do body)] (assert q)... %). */
    if (body != NIL && body->tag == CLJC_LIST && body->as.cons.head != NIL &&
        body->as.cons.head->tag == CLJC_MAP && body->as.cons.tail != NIL) {
        Cljc *pp = body->as.cons.head, *pres = NIL, *posts = NIL;
        map_find(pp, mk_kw(intern("pre", 3)), &pres);
        map_find(pp, mk_kw(intern("post", 4)), &posts);
        if ((pres != NIL && pres->tag == CLJC_VECTOR) ||
            (posts != NIL && posts->tag == CLJC_VECTOR)) {
            const char *ASSERT = intern("assert", 6);
            Cljc *rest = body->as.cons.tail, *out = NIL, **ot = &out;
            if (pres != NIL && pres->tag == CLJC_VECTOR)
                for (size_t i = 0; i < vec_len(pres); i++) {
                    *ot = mk_cons(mk_cons(mk_sym(ASSERT), mk_cons(vec_nth(pres, i), NIL)), NIL);
                    ot = &(*ot)->as.cons.tail;
                }
            if (posts != NIL && posts->tag == CLJC_VECTOR) {
                Cljc *pct = mk_sym(intern("%", 1));
                Cljc *bv[2] = { pct, mk_cons(mk_sym(intern("do", 2)), rest) };
                Cljc *lbody = NIL, **lt = &lbody;
                for (size_t i = 0; i < vec_len(posts); i++) {
                    *lt = mk_cons(mk_cons(mk_sym(ASSERT), mk_cons(vec_nth(posts, i), NIL)), NIL);
                    lt = &(*lt)->as.cons.tail;
                }
                *lt = mk_cons(pct, NIL);   /* the return value */
                *ot = mk_cons(mk_cons(mk_sym(intern("let", 3)),
                                      mk_cons(mk_vector(bv, 2), lbody)), NIL);
            } else {
                *ot = rest;
            }
            body = out;
        }
    }
    return mk_cons(params, body);
}

/* Build an interpreted fn from the forms after `fn` (or after a defn name):
 * single arity ([params] body...) or multi-arity (([p] b...) ([p q] b...)). */
/* The **arities** cache marker on fn source spines (see make_fn). */
static bool is_arities_meta(Cljc *m) {
    static const char *SYM_ARITIES;
    if (!SYM_ARITIES) SYM_ARITIES = intern("**arities**", 11);
    return m != NULL && m != NIL && m->tag == CLJC_LIST &&
           m->as.cons.head->tag == CLJC_SYMBOL &&
           m->as.cons.head->as.sym == SYM_ARITIES;
}

static Cljc *make_fn(CljcEnv *env, Cljc *forms, bool is_macro) {
    /* Arities (and the chunks cached on them) are pure structure: share
     * them across every closure built from the same source forms. A hot
     * loop creating a closure per iteration otherwise RECOMPILES its
     * body each call (5M+ vm_compiles in one profile). */
    if (forms != NIL && forms->tag == CLJC_LIST &&
        is_arities_meta(forms->meta)) {
        Cljc *f = alloc(CLJC_FN);
        f->as.fn.arities = forms->meta->as.cons.tail;
        f->as.fn.env = env;
        f->as.fn.is_macro = is_macro;
        return f;
    }
    /* single arity with a return-type hint: (fn ^{:tag T} [x] ..) reads its
     * params as (with-meta [x] {..}); unwrap so it's seen as a [params] head. */
    if (forms != NIL && forms->as.cons.head->tag == CLJC_LIST) {
        Cljc *h = forms->as.cons.head;
        if (h->as.cons.head->tag == CLJC_SYMBOL &&
            h->as.cons.head->as.sym == intern("with-meta", 9) &&
            h->as.cons.tail != NIL && h->as.cons.tail->as.cons.head->tag == CLJC_VECTOR)
            forms = mk_cons(h->as.cons.tail->as.cons.head, forms->as.cons.tail);
    }
    Cljc *arities = NIL, **t = &arities;
    if (forms != NIL && forms->as.cons.head->tag == CLJC_VECTOR) {
        *t = mk_cons(build_arity(forms->as.cons.head, forms->as.cons.tail), NIL);
    } else {
        for (Cljc *c = forms; c && c->tag == CLJC_LIST; c = c->as.cons.tail) {
            Cljc *clause = c->as.cons.head;
            if (clause == NIL || clause->tag != CLJC_LIST)
                cljc_error("fn: expected ([params] body...) clauses");
            *t = mk_cons(build_arity(clause->as.cons.head, clause->as.cons.tail), NIL);
            t = &(*t)->as.cons.tail;
        }
        if (arities == NIL) cljc_error("fn needs at least one arity");
    }
    /* NOTE: cache only engages when the spine carries no other meta —
     * any foreign meta silently reverts to rebuild-per-closure (the 5M-
     * compile cliff); log it so the cliff is visible. */
    if (forms != NIL && forms->tag == CLJC_LIST) {
        if (forms->meta == NULL)
            forms->meta = mk_cons(mk_sym(intern("**arities**", 11)), arities);
        else if (!is_arities_meta(forms->meta) && getenv("CLJC_VM_LOG"))
            fprintf(stderr, "[vm] arities cache bypassed (foreign meta)\n");
    }
    Cljc *f = alloc(CLJC_FN);
    f->as.fn.arities = arities;
    f->as.fn.env = env;
    f->as.fn.is_macro = is_macro;
    return f;
}

/* Quasiquote template expansion: copy structure, evaluating ~x holes and
 * splicing ~@xs. Nested quasiquotes are not given Clojure's full treatment
 * (no level counting, no auto-gensym #-suffixes, no namespace resolution). */
static Cljc *qq_expand(CljcEnv *env, Cljc *form) {
    static const char *SYM_UQ, *SYM_UQS;
    if (!SYM_UQ) { SYM_UQ = intern("clojure.core/unquote", 20); SYM_UQS = intern("clojure.core/unquote-splicing", 29); }

    if (form == NULL || form == NIL) return NIL;

    if (form->tag == CLJC_LIST) {
        Cljc *head = form->as.cons.head;
        if (head->tag == CLJC_SYMBOL && head->as.sym == SYM_UQ)
            return eval(env, form->as.cons.tail->as.cons.head);
        Cljc *out = NIL, **t = &out;
        for (Cljc *l = form; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
            Cljc *el = l->as.cons.head;
            if (el != NIL && el->tag == CLJC_LIST && el->as.cons.head->tag == CLJC_SYMBOL
                && el->as.cons.head->as.sym == SYM_UQS) {
                /* to_seq so ~@ splices vectors and maps too, not just lists. */
                Cljc *spliced = to_seq(eval(env, el->as.cons.tail->as.cons.head));
                for (Cljc *s = spliced; s && s->tag == CLJC_LIST; s = s->as.cons.tail) {
                    *t = mk_cons(s->as.cons.head, NIL);
                    t = &(*t)->as.cons.tail;
                }
            } else {
                *t = mk_cons(qq_expand(env, el), NIL);
                t = &(*t)->as.cons.tail;
            }
        }
        return out;
    }
    if (form->tag == CLJC_VECTOR) {
        /* Expand into a list first so ~@ splices work inside vector
         * templates ([~@(...)]), then convert. The list is stack-rooted. */
        Cljc *out = NIL, **t = &out;
        for (size_t i = 0; i < vec_len(form); i++) {
            Cljc *el = vec_nth(form, i);
            if (el != NIL && el->tag == CLJC_LIST && el->as.cons.head->tag == CLJC_SYMBOL
                && el->as.cons.head->as.sym == SYM_UQS) {
                Cljc *spliced = to_seq(eval(env, el->as.cons.tail->as.cons.head));
                for (Cljc *s = spliced; s && s->tag == CLJC_LIST; s = s->as.cons.tail) {
                    *t = mk_cons(s->as.cons.head, NIL);
                    t = &(*t)->as.cons.tail;
                }
            } else {
                *t = mk_cons(qq_expand(env, el), NIL);
                t = &(*t)->as.cons.tail;
            }
        }
        Cljc *v = mk_empty_vec();
        for (Cljc *l = out; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
            v = vec_conj1(v, l->as.cons.head);
        return v;
    }
    if (form->tag == CLJC_MAP) {
        Cljc *m = mk_map();
        for (Cljc *e = map_entry_list(form); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *k = qq_expand(env, e->as.cons.head->as.cons.head);
            Cljc *val = qq_expand(env, e->as.cons.head->as.cons.tail);
            m = map_assoc(m, k, val);
        }
        return m;
    }
    if (form->tag == CLJC_SET) {
        Cljc *s = mk_set();
        for (Cljc *e = set_element_list(form); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
            s = set_conj(s, qq_expand(env, e->as.cons.head));
        return s;
    }
    /* Syntax-quote qualification (Clojure-faithful-ish): a bare template
     * symbol whose home ns defines it expands to the qualified symbol, so
     * `(move ~a) inside (ns p21 ...) yields p21/move — matching the case
     * constants real Clojure code writes against syntax-quoted forms. */
    if (form->tag == CLJC_SYMBOL && form->as.symc.home_ns &&
        !strchr(form->as.symc.name, '/')) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s",
                 form->as.symc.home_ns, form->as.symc.name);
        const char *qual = intern(buf, strlen(buf));
        for (Binding *b = env_root(env)->bindings; b; b = b->next)
            if (b->name == qual) return mk_sym(qual);
    }
    /* Atoms are template literals. */
    return form;
}

static Cljc *eval_inner(CljcEnv *env, Cljc *form);

/* eval wrapper: maintains the form stack that error traces snapshot.
 * longjmp unwinds restore eval_sp from ErrFrames / top-level handlers. */
static Cljc *eval(CljcEnv *env, Cljc *form) {
    if (form == NULL || form == NIL) return NIL;
    if (form->tag != CLJC_LIST) return eval_inner(env, form);
    if (eval_sp < EVAL_STACK_MAX) {
        eval_stack[eval_sp++] = form;
        Cljc *r = eval_inner(env, form);
        eval_sp--;
        return r;
    }
    return eval_inner(env, form);
}

static Cljc *eval_inner(CljcEnv *env, Cljc *form) {
    if (form == NULL || form == NIL) return NIL;
    switch (form->tag) {
        case CLJC_INT: case CLJC_DOUBLE: case CLJC_BOOL: case CLJC_NIL: case CLJC_EMPTY:
        case CLJC_BIGINT: case CLJC_RATIO: case CLJC_VAR:
        case CLJC_STRING: case CLJC_CHAR: case CLJC_KEYWORD: case CLJC_FN: case CLJC_NATIVE:
        case CLJC_ATOM: case CLJC_TVEC: case CLJC_CORO:
        case CLJC_SORTED:  /* a constructed sorted coll has no reader literal */
        case CLJC_TNODE:   /* internal sorted-tree node; self-evaluates if it leaks */
        case CLJC_RECUR:   /* not produced by the reader; appears only inside loop */
        case CLJC_CHUNK:   /* internal; self-evaluates if it ever leaks */
            return form;
        case CLJC_LAZY:
            /* Lazy seqs ARE seqs: in form position (e.g. a macro expansion
             * built with lazy concat) they evaluate as call forms. */
            return eval(env, to_seq(form));
        case CLJC_FREE:
            cljc_error("internal: evaluated a freed value (GC bug)");
        case CLJC_VECTOR: {
            /* Vector literals evaluate each element: [(+ 1 2)] => [3].
             * len grows as slots fill so a mid-eval GC marks exactly the
             * elements written so far. */
            Cljc *v = mk_empty_vec();
            for (size_t i = 0; i < vec_len(form); i++)
                v = vec_conj1(v, eval(env, vec_nth(form, i)));
            return v;
        }
        case CLJC_MAP: {
            /* Map literal: evaluate each key/value form, assoc into a fresh
             * map. Evaluation order follows hash order, not source order. */
            Cljc *m = mk_map();
            for (Cljc *e = map_entry_list(form); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                Cljc *k = eval(env, e->as.cons.head->as.cons.head);
                Cljc *val = eval(env, e->as.cons.head->as.cons.tail);
                m = map_assoc(m, k, val);
            }
            return m;
        }
        case CLJC_SET: {
            /* Set literal: evaluate each element form. */
            Cljc *s = mk_set();
            for (Cljc *e = set_element_list(form); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
                s = set_conj(s, eval(env, e->as.cons.head));
            return s;
        }
        case CLJC_HNODE:
            cljc_error("internal: evaluated a HAMT node");
        case CLJC_SYMBOL:
            return resolve_symbol(env, form);
        case CLJC_LIST: {
            /* Macro expansions built with lazy concat/map can carry LAZY
             * tails mid-chain; every special-form walker iterates raw cons
             * tails and would silently truncate. Realize once, up front. */
            {
                Cljc *l = form;
                while (l->tag == CLJC_LIST) l = l->as.cons.tail;
                if (l != NIL && l->tag == CLJC_LAZY) form = to_seq(form);
            }
            Cljc *head = form->as.cons.head;
            Cljc *rest = form->as.cons.tail;

            /* Special forms — dispatch by interned symbol pointer. A head that
             * resolved to an ordinary call last time is flagged `plain`, so we
             * skip the strip + dot + special-form cascade on every repeat. */
            if (head->tag == CLJC_SYMBOL && !head->as.symc.plain) {
                const char *s = strip_core_qualifier(head->as.sym);
                /* (.-field obj): deftype field access -> field of the instance
                 * map (cljc represents a deftype instance as a tagged map) */
                if (s[0] == '.' && s[1] == '-' && s[2] &&
                    rest != NIL && rest->tag == CLJC_LIST) {
                    Cljc *obj = eval(env, rest->as.cons.head);
                    Cljc *out;
                    if (obj != NIL && obj->tag == CLJC_MAP &&
                        map_find(obj, mk_kw(intern(s + 2, strlen(s + 2))), &out))
                        return out;
                    return NIL;
                }
                /* (.foo x): if .foo is not a defined method, treat as a deftype
                 * field access (Java's field-access syntax === method syntax) */
                if (s[0] == '.' && s[1] && s[1] != '-' &&
                    rest != NIL && rest->tag == CLJC_LIST &&
                    !root_find(env_root(env), s, NULL)) {
                    Cljc *obj = eval(env, rest->as.cons.head);
                    Cljc *out;
                    /* (.method obj args..): route to the deftype's method (sans
                     * dot) — a reify/deftype's own methods, e.g. a Comparator's
                     * .compare, aren't predefined global .fns. */
                    Cljc *margv[64]; int mn = 0;
                    for (Cljc *a = rest->as.cons.tail; a && a->tag == CLJC_LIST && mn < 64;
                         a = a->as.cons.tail)
                        margv[mn++] = eval(env, a->as.cons.head);
                    if (dispatch_deftype_method(env, obj, s + 1, margv, mn, &out)) return out;
                    /* a reify keeps its methods per-instance in :cljc/impls */
                    if (obj != NIL && obj->tag == CLJC_MAP) {
                        Cljc *impls, *m;
                        if (map_find(obj, mk_kw(intern("cljc/impls", 10)), &impls) &&
                            impls != NIL && impls->tag == CLJC_MAP &&
                            map_find(impls, mk_sym(intern(s + 1, strlen(s + 1))), &m)) {
                            Cljc *iargv[65]; iargv[0] = obj;
                            for (int i = 0; i < mn; i++) iargv[i + 1] = margv[i];
                            return apply(env, m, iargv, mn + 1);
                        }
                    }
                    /* one arg, no such method -> field access on the instance map */
                    if (rest->as.cons.tail == NIL && obj != NIL && obj->tag == CLJC_MAP &&
                        map_find(obj, mk_kw(intern(s + 1, strlen(s + 1))), &out))
                        return out;
                    err_token = s;
                    cljc_error("I don't know what `%s` refers to.", s);
                }
                static const char *SYM_QUOTE, *SYM_IF, *SYM_DO, *SYM_DEF, *SYM_LET, *SYM_FN, *SYM_LOOP, *SYM_RECUR,
                                  *SYM_AND, *SYM_OR, *SYM_WHEN, *SYM_COND, *SYM_DEFN,
                                  *SYM_DEFMACRO, *SYM_QUASIQUOTE, *SYM_TRY, *SYM_CATCH, *SYM_FINALLY;
                if (!SYM_QUOTE) {
                    SYM_QUOTE = intern("quote", 5); SYM_IF = intern("if", 2);
                    SYM_DO = intern("do", 2);       SYM_DEF = intern("def", 3);
                    SYM_LET = intern("let", 3);     SYM_FN = intern("fn", 2);
                    SYM_LOOP = intern("loop", 4);   SYM_RECUR = intern("recur", 5);
                    SYM_AND = intern("and", 3);     SYM_OR = intern("or", 2);
                    SYM_WHEN = intern("when", 4);   SYM_COND = intern("cond", 4);
                    SYM_DEFN = intern("defn", 4);
                    SYM_DEFMACRO = intern("defmacro", 8);
                    SYM_QUASIQUOTE = intern("quasiquote", 10);
                    SYM_TRY = intern("try", 3);
                    SYM_CATCH = intern("catch", 5);
                    SYM_FINALLY = intern("finally", 7);
                }
                {
                    /* A top-level #_form (or #?@ branch) arrives as the
                     * reader's splice marker — evaluate to nil / last. */
                    static const char *SYM_SPLICE;
                    if (!SYM_SPLICE) SYM_SPLICE = intern("**reader-splice**", 17);
                    if (s == SYM_SPLICE) {
                        /* payload: list of forms to splice (nil for #_) */
                        Cljc *items = rest != NIL ? rest->as.cons.head : NIL;
                        Cljc *r = NIL;
                        for (Cljc *c = items; c && c->tag == CLJC_LIST; c = c->as.cons.tail)
                            r = eval(env, c->as.cons.head);
                        return r;
                    }
                }
                if (s == SYM_TRY) {
                    /* (try body... (catch ExClass e handler...) (finally fin...))
                     * The class symbol is accepted and ignored (untyped catch —
                     * a documented divergence). finally runs on every exit:
                     * normal, body throw, or handler throw. */
                    Cljc *body = NIL, **bt = &body;
                    Cljc *catch_clause = NULL, *finally_clause = NULL;
                    for (Cljc *c = rest; c && c->tag == CLJC_LIST; c = c->as.cons.tail) {
                        Cljc *f = c->as.cons.head;
                        const char *fh = (f != NIL && f->tag == CLJC_LIST &&
                                          f->as.cons.head->tag == CLJC_SYMBOL)
                                         ? f->as.cons.head->as.sym : NULL;
                        if (fh == SYM_CATCH) {
                            if (finally_clause) cljc_error("try: catch must precede finally");
                            /* catch is untyped here, so the FIRST catch handles
                             * any exception; extra (catch ..) clauses for other
                             * classes are accepted and skipped. */
                            if (!catch_clause) catch_clause = f;
                        } else if (fh == SYM_FINALLY) {
                            if (finally_clause) cljc_error("try: only one finally clause is allowed");
                            finally_clause = f;
                        } else {
                            if (catch_clause || finally_clause)
                                cljc_error("try: body form after catch/finally");
                            *bt = mk_cons(f, NIL);
                            bt = &(*bt)->as.cons.tail;
                        }
                    }
                    if (catch_clause) {
                        need_args(catch_clause->as.cons.tail, 2, "catch");
                        sym_name(catch_clause->as.cons.tail->as.cons.head, "catch class");
                        sym_name(catch_clause->as.cons.tail->as.cons.tail->as.cons.head,
                                 "catch binding");
                    }

                    /* volatile: read after a longjmp back into this frame. */
                    Cljc * volatile body_v = body;
                    Cljc * volatile catch_v = catch_clause;
                    Cljc * volatile finally_v = finally_clause;
                    Cljc * volatile result = NIL;
                    volatile bool pending = false;  /* unhandled exception to rethrow */

                    ErrFrame frame;
                    frame.prev = err_top;
                    frame.vsp_save = vsp;
                    frame.esp_save = eval_sp;
                    err_top = &frame;
                    if (setjmp(frame.jb) == 0) {
                        result = eval_body(env, body_v);
                        err_top = frame.prev;
                    } else {
                        err_top = frame.prev;
                        vsp = frame.vsp_save;
                        eval_sp = frame.esp_save;
                        if (catch_v) {
                            /* Bind the exception value; run the handler under
                             * its own frame so finally still runs if it throws. */
                            Cljc *exc = cur_exc ? cur_exc : mk_str(err_msg, strlen(err_msg));
                            cur_exc = NULL;
                            CljcEnv *scope = env_new(env);
                            Cljc *cc = catch_v->as.cons.tail;       /* (Class e body...) */
                            env_define(scope, cc->as.cons.tail->as.cons.head->as.sym, exc);
                            ErrFrame hframe;
                            hframe.prev = err_top;
                            hframe.vsp_save = vsp;
                            hframe.esp_save = eval_sp;
                            err_top = &hframe;
                            if (setjmp(hframe.jb) == 0) {
                                result = eval_body(scope, cc->as.cons.tail->as.cons.tail);
                                err_top = hframe.prev;
                            } else {
                                err_top = hframe.prev;
                                vsp = hframe.vsp_save;
                                eval_sp = hframe.esp_save;
                                pending = true;     /* handler threw */
                            }
                        } else {
                            pending = true;         /* no catch: rethrow after finally */
                        }
                    }
                    if (finally_v)  /* a throwing finally replaces any pending exception */
                        eval_body(env, finally_v->as.cons.tail);
                    if (pending) cljc_raise();
                    return result;
                }
                {
                    static const char *SYM_NS, *SYM_REQUIRE, *SYM_USE, *SYM_IMPORT;
                    if (!SYM_NS) {
                        SYM_NS = intern("ns", 2);
                        SYM_REQUIRE = intern("require", 7);
                        SYM_USE = intern("use", 3);
                        SYM_IMPORT = intern("import", 6);
                    }
                    /* import is a compat no-op (flat globals already match the
                     * universal alias convention); `use` is a real macro
                     * (require + refer-all) defined in the preamble, so it must
                     * fall through to macro resolution rather than short-circuit. */
                    if (s == SYM_IMPORT) return NIL;
                    if (s == SYM_NS) {
                        /* (ns name (:require [a.b :as x] ...)) — load the
                         * :require clauses; everything else is tolerated. */
                        static const char *KW_REQ, *KW_IMPORT, *KW_USE;
                        if (!KW_REQ) { KW_REQ = intern("require", 7);
                                       KW_IMPORT = intern("import", 6);
                                       KW_USE = intern("use", 3); }
                        for (Cljc *c = rest; c && c->tag == CLJC_LIST; c = c->as.cons.tail) {
                            Cljc *cl = c->as.cons.head;
                            /* clause head keyword -> which per-spec handler */
                            const char *kw = NULL;
                            if (cl != NIL && cl->tag == CLJC_LIST &&
                                cl->as.cons.head->tag == CLJC_KEYWORD)
                                kw = cl->as.cons.head->as.kw;
                            else if (cl != NIL && cl->tag == CLJC_VECTOR && vec_len(cl) > 0 &&
                                     vec_nth(cl, 0)->tag == CLJC_KEYWORD)
                                kw = vec_nth(cl, 0)->as.kw;
                            const char *callee_name =
                                kw == KW_REQ    ? "cljc/require-one" :
                                kw == KW_USE    ? "cljc/use-one"     :
                                kw == KW_IMPORT ? "cljc/import-one"  : NULL;
                            if (!callee_name) continue;
                            Cljc *callee = env_lookup_maybe(env, callee_name);
                            if (!callee) continue;
                            if (cl->tag == CLJC_LIST) {
                                for (Cljc *sp = cl->as.cons.tail; sp && sp->tag == CLJC_LIST;
                                     sp = sp->as.cons.tail) {
                                    Cljc *one[1] = {sp->as.cons.head};
                                    apply(env, callee, one, 1);
                                }
                            } else {
                                for (uint32_t vi = 1; vi < vec_len(cl); vi++) {
                                    Cljc *one[1] = {vec_nth(cl, vi)};
                                    apply(env, callee, one, 1);
                                }
                            }
                        }
                        /* Enter the namespace: subsequent reads stamp
                         * home_ns and defs land under name/ — the same
                         * model require-loaded libraries already use.
                         * (run_stream alternates read/eval, so the rest
                         * of the file is read with the ns active.) */
                        {
                            Cljc *nsn = rest != NIL && rest->tag == CLJC_LIST
                                        ? rest->as.cons.head : NIL;
                            if (nsn != NIL && nsn->tag == CLJC_SYMBOL)
                                cur_reader_ns = nsn->as.symc.name;
                        }
                        return NIL;
                    }
                    (void)SYM_REQUIRE; (void)SYM_USE;
                }
                {
                    static const char *SYM_BINDING, *SYM_WITH_REDEFS;
                    if (!SYM_BINDING) {
                        SYM_BINDING = intern("binding", 7);
                        SYM_WITH_REDEFS = intern("with-redefs", 11);
                    }
                    if (s == SYM_BINDING || s == SYM_WITH_REDEFS) {
                        /* (binding [*v* val ...] body) — root bindings mutate
                         * in place (single-threaded), restored on every exit
                         * path via a handler frame. with-redefs is identical. */
                        Cljc *bv = rest->as.cons.head;
                        if (bv == NIL || bv->tag != CLJC_VECTOR || vec_len(bv) % 2 != 0)
                            cljc_error("binding needs an even-sized vector");
                        size_t n = vec_len(bv) / 2;
                        if (n > 16) cljc_error("binding: too many vars");
                        Binding *slots[16];
                        Cljc *saved[16];
                        CljcEnv *root = env_root(env);
                        for (size_t i = 0; i < n; i++) {
                            Cljc *symc = vec_nth(bv, i * 2);
                            const char *nm = sym_name(symc, "binding");
                            Binding *b = NULL;
                            /* home-ns first, like symbol resolution: a
                             * (def ^:dynamic *v*) inside (ns foo) lands
                             * under foo/, and binding must find it there */
                            if (symc->as.symc.home_ns && !strchr(nm, '/')) {
                                char buf[256];
                                snprintf(buf, sizeof buf, "%s/%s",
                                         symc->as.symc.home_ns, nm);
                                const char *qual = intern(buf, strlen(buf));
                                for (Binding *x = root->bindings; x; x = x->next)
                                    if (x->name == qual) { b = x; break; }
                                /* binding a referred var rebinds the shared
                                 * source global, so other ns's reads see it */
                                if (!b)
                                    for (int ri = 0; ri < n_refers; ri++)
                                        if (refer_from[ri] == qual) {
                                            for (Binding *x = root->bindings; x; x = x->next)
                                                if (x->name == refer_to[ri]) { b = x; break; }
                                            if (b) break;
                                        }
                            }
                            if (!b)
                                for (Binding *x = root->bindings; x; x = x->next)
                                    if (x->name == nm) { b = x; break; }
                            /* aliased dynamic var like cfg/-star-v-star-: resolve
                             * the alias to its full ns, else the bare var name */
                            if (!b) {
                                const char *slash = strchr(nm, '/');
                                if (slash && slash != nm) {
                                    const char *pre = intern(nm, (size_t)(slash - nm));
                                    const char *base = slash + 1;
                                    const char *full = alias_lookup(pre, symc->as.symc.home_ns);
                                    if (full) {
                                        char qb[256];
                                        snprintf(qb, sizeof qb, "%s/%s", full, base);
                                        const char *q = intern(qb, strlen(qb));
                                        for (Binding *x = root->bindings; x; x = x->next)
                                            if (x->name == q) { b = x; break; }
                                    }
                                    if (!b) {
                                        const char *bb = intern(base, strlen(base));
                                        for (Binding *x = root->bindings; x; x = x->next)
                                            if (x->name == bb) { b = x; break; }
                                    }
                                }
                            }
                            if (!b) cljc_error("binding: unable to resolve %s", nm);
                            slots[i] = b;
                            saved[i] = b->value;
                            b->value = eval(env, vec_nth(bv, i * 2 + 1));
                        }
                        ErrFrame frame;
                        frame.prev = err_top;
                        frame.vsp_save = vsp;
                        frame.esp_save = eval_sp;
                        err_top = &frame;
                        Cljc * volatile result = NIL;
                        volatile bool threw = false;
                        if (setjmp(frame.jb) == 0) {
                            result = eval_body(env, rest->as.cons.tail);
                            err_top = frame.prev;
                        } else {
                            err_top = frame.prev;
                            vsp = frame.vsp_save;
                            eval_sp = frame.esp_save;
                            threw = true;
                        }
                        for (size_t i = 0; i < n; i++) slots[i]->value = saved[i];
                        if (threw) cljc_raise();
                        return result;
                    }
                }
                if (s == SYM_QUASIQUOTE) return qq_expand(env, rest->as.cons.head);
                if (s == SYM_DEFMACRO) {
                    /* (defmacro name [params] body...) — a fn flagged so that
                     * eval calls it on unevaluated forms and re-evals the result. */
                    need_args(rest, 2, "defmacro");
                    const char *name = sym_name(peel_meta_sym(rest->as.cons.head), "defmacro");
                    Cljc *mbody = rest->as.cons.tail;
                    if (mbody->as.cons.head->tag == CLJC_STRING &&
                        mbody->as.cons.tail != NIL)
                        mbody = mbody->as.cons.tail;  /* skip docstring */
                    if (mbody->as.cons.head->tag == CLJC_MAP &&
                        mbody->as.cons.tail != NIL)
                        mbody = mbody->as.cons.tail;  /* skip attr-map */
                    Cljc *m = make_fn(env, mbody, true);
                    env_define_root(env_root(env), name, m);
                    return m;
                }
                if (s == SYM_DEFN) {
                    /* (defn name [params] body...) ≡ (def name (fn [params] body...)) */
                    need_args(rest, 2, "defn");
                    Cljc *name = rest->as.cons.head;
                    Cljc *fbody = rest->as.cons.tail;
                    if (fbody->as.cons.head->tag == CLJC_STRING &&
                        fbody->as.cons.tail != NIL)
                        fbody = fbody->as.cons.tail;  /* skip docstring */
                    if (fbody->as.cons.head->tag == CLJC_MAP &&
                        fbody->as.cons.tail != NIL)
                        fbody = fbody->as.cons.tail;  /* skip attr-map */
                    Cljc *fn_form = mk_cons(mk_sym(SYM_FN), fbody);
                    Cljc *def_form = mk_cons(mk_sym(SYM_DEF),
                                       mk_cons(name, mk_cons(fn_form, NIL)));
                    return eval(env, def_form);
                }
                if (s == SYM_QUOTE) return rest->as.cons.head;
                if (s == SYM_IF) {
                    need_args(rest, 2, "if");
                    Cljc *test = eval(env, rest->as.cons.head);
                    Cljc *then = rest->as.cons.tail->as.cons.head;
                    Cljc *els  = rest->as.cons.tail->as.cons.tail;
                    if (is_truthy(test)) return eval(env, then);
                    return els == NIL ? NIL : eval(env, els->as.cons.head);
                }
                if (s == SYM_DO) return eval_body(env, rest);
                if (s == SYM_DEF) {
                    need_args(rest, 1, "def");
                    Cljc *namef = rest->as.cons.head;
                    /* (def ^:dynamic x v): the reader wrapped the name as
                     * (with-meta x m) — unwrap; def meta is not retained.
                     * Stacked metadata (^:dynamic ^:no-doc x) nests several
                     * with-meta forms, so peel all of them. */
                    while (namef != NIL && namef->tag == CLJC_LIST &&
                           namef->as.cons.head->tag == CLJC_SYMBOL &&
                           !strcmp(namef->as.cons.head->as.sym, "with-meta") &&
                           namef->as.cons.tail != NIL)
                        namef = namef->as.cons.tail->as.cons.head;
                    /* a #?(:cljs ..)-only def name elides to nil here — skip it */
                    if (namef == NIL) return NIL;
                    const char *name = sym_name(namef, "def");
                    Cljc *valf = rest->as.cons.tail;
                    /* (def name): no value — an unbound-var declaration => nil */
                    if (valf == NIL) { env_define_root(env_root(env), name, NIL); return NIL; }
                    /* (def name "docstring" value): skip the docstring */
                    if (valf->as.cons.tail != NIL &&
                        valf->as.cons.tail->tag == CLJC_LIST &&
                        valf->as.cons.head != NIL &&
                        valf->as.cons.head->tag == CLJC_STRING)
                        valf = valf->as.cons.tail;
                    Cljc *val = eval(env, valf->as.cons.head);
                    env_define_root(env_root(env), name, val);  /* def is always global */
                    /* Attach :name metadata to a def'd function (fresh per defn, so
                     * no shared-cell hazard) the first time it's named, so
                     * (:name (meta (resolve sym))) works for potemkin-style
                     * import-def without cljc having real vars. */
                    if (val != NIL && val->tag == CLJC_FN) {
                        bool has_name = false;
                        if (val->meta && val->meta->tag == CLJC_MAP) {
                            Cljc *tmp; has_name = map_find(val->meta, mk_kw(intern("name", 4)), &tmp);
                        }
                        if (!has_name) {
                            const char *slash = strrchr(name, '/');
                            const char *bare = slash ? slash + 1 : name;
                            Cljc *m = (val->meta && val->meta->tag == CLJC_MAP) ? val->meta : mk_map();
                            val->meta = map_assoc(m, mk_kw(intern("name", 4)),
                                                  mk_sym(intern(bare, strlen(bare))));
                        }
                    }
                    return val;
                }
                if (s == SYM_LET && !head_shadowed(env, head->as.sym)) {
                    /* (let [a 1 b 2] body...) — bindings live in a vector. */
                    Cljc *binds_vec = rest->as.cons.head;
                    if (binds_vec == NIL || binds_vec->tag != CLJC_VECTOR ||
                        vec_len(binds_vec) % 2 != 0)
                        cljc_error("let needs an even-sized binding vector");
                    CljcEnv *scope = env_new(env);
                    for (size_t i = 0; i < vec_len(binds_vec); i += 2) {
                        Cljc *val = eval(scope, vec_nth(binds_vec, i + 1));
                        destructure(scope, vec_nth(binds_vec, i), val);
                    }
                    return eval_body(scope, rest->as.cons.tail);
                }
                /* and/or/when/cond are macros in real Clojure; until defmacro
                 * lands they are special forms (they must short-circuit). */
                if (s == SYM_AND) {
                    Cljc *r = TRUE;
                    for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                        r = eval(env, a->as.cons.head);
                        if (!is_truthy(r)) return r;
                    }
                    return r;
                }
                if (s == SYM_OR) {
                    Cljc *r = NIL;
                    for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                        r = eval(env, a->as.cons.head);
                        if (is_truthy(r)) return r;
                    }
                    return r;
                }
                if (s == SYM_WHEN) {
                    need_args(rest, 1, "when");
                    if (!is_truthy(eval(env, rest->as.cons.head))) return NIL;
                    return eval_body(env, rest->as.cons.tail);
                }
                if (s == SYM_COND) {
                    static const char *SYM_ELSE;
                    if (!SYM_ELSE) SYM_ELSE = intern("else", 4);
                    for (Cljc *c = rest; c && c->tag == CLJC_LIST; c = c->as.cons.tail->as.cons.tail) {
                        if (c->as.cons.tail == NIL) cljc_error("cond requires an even number of forms");
                        Cljc *test = c->as.cons.head;
                        bool is_else = test->tag == CLJC_KEYWORD && test->as.kw == SYM_ELSE;
                        if (is_else || is_truthy(eval(env, test)))
                            return eval(env, c->as.cons.tail->as.cons.head);
                    }
                    return NIL;
                }
                if (s == SYM_RECUR) {
                    /* Evaluate each arg eagerly, then package into a sentinel
                     * that the enclosing loop will catch. If nothing catches
                     * it, a non-loop consumer (e.g. +) will hit the default
                     * arm of its tag-switch and error out — that's our
                     * runtime tail-position check. */
                    size_t n = list_len(rest);
                    if (n > 255) cljc_error("recur: too many arguments");
                    Cljc *r = alloc(CLJC_RECUR);
                    Cljc **vals = r->as.recur.iv;
                    if (n > 3) {  /* spill flag set before any slot fills */
                        vals = xmalloc(sizeof(Cljc *) * n);
                        r->as.recur.iv[0] = (Cljc *)vals;
                        r->as.recur.spill = true;
                    }
                    size_t i = 0;  /* n grows as slots fill — GC safety */
                    for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                        vals[i++] = eval(env, a->as.cons.head);
                        r->as.recur.n = (uint8_t)i;
                    }
                    return r;
                }
                if (s == SYM_LOOP && !head_shadowed(env, head->as.sym)) {
                    /* (loop [name1 init1 name2 init2 ...] body...) — like let,
                     * but body re-runs whenever it produces a (recur ...) value. */
                    Cljc *binds_vec = rest->as.cons.head;
                    if (binds_vec == NIL || binds_vec->tag != CLJC_VECTOR ||
                        vec_len(binds_vec) % 2 != 0)
                        cljc_error("loop needs an even-sized binding vector");
                    size_t nparams = vec_len(binds_vec) / 2;
                    /* Destructuring patterns: rewrite to gensym bindings with
                     * an inner let, so recur rebinds the gensyms and the
                     * patterns re-destructure each iteration (Clojure does
                     * the same rewrite). */
                    bool plain = true;
                    for (size_t i = 0; i < nparams; i++)
                        if (vec_nth(binds_vec, i * 2)->tag != CLJC_SYMBOL) plain = false;
                    if (!plain) {
                        static int loopg;
                        Cljc *gb[64], *lb[64];   /* new loop binds / let binds */
                        if (nparams > 32) cljc_error("loop: too many bindings");
                        for (size_t i = 0; i < nparams; i++) {
                            char nm[24];
                            snprintf(nm, sizeof nm, "loop__%d", ++loopg);
                            Cljc *g = mk_sym(intern(nm, strlen(nm)));
                            gb[i * 2] = g;
                            gb[i * 2 + 1] = vec_nth(binds_vec, i * 2 + 1);
                            lb[i * 2] = vec_nth(binds_vec, i * 2);
                            lb[i * 2 + 1] = g;
                        }
                        Cljc *letform = mk_cons(mk_sym(SYM_LET),
                            mk_cons(mk_vector(lb, nparams * 2), rest->as.cons.tail));
                        Cljc *newform = mk_cons(mk_sym(SYM_LOOP),
                            mk_cons(mk_vector(gb, nparams * 2), mk_cons(letform, NIL)));
                        /* splice: desugar once per site, not per entry */
                        form->as.cons.head = newform->as.cons.head;
                        form->as.cons.tail = newform->as.cons.tail;
                        return eval(env, newform);
                    }
                    const char **names = xmalloc(sizeof(char *) * (nparams ? nparams : 1));
                    CljcEnv *scope = env_new(env);
                    for (size_t i = 0; i < nparams; i++) {
                        names[i] = sym_name(vec_nth(binds_vec, i * 2), "loop binding");
                        Cljc *val = eval(scope, vec_nth(binds_vec, i * 2 + 1));
                        env_define(scope, names[i], val);
                    }
                    Cljc *body = rest->as.cons.tail;
                    for (;;) {
                        Cljc *r = eval_body(scope, body);
                        if (!(r && r->tag == CLJC_RECUR)) { free(names); return r; }

                        if (r->as.recur.n != nparams)
                            cljc_error("recur arity mismatch: expected %zu, got %d",
                                       nparams, (int)r->as.recur.n);
                        Cljc **rvals = r->as.recur.spill
                            ? (Cljc **)r->as.recur.iv[0] : r->as.recur.iv;
                        /* Fresh binding frame each pass — a body that closes over a
                         * loop var (e.g. (loop [x ..] (recur (conj acc (fn [] x))))
                         * must capture THIS iteration's value, not see it mutated by
                         * later passes. Old frames stay intact for their closures. */
                        scope = env_new(env);
                        for (size_t i = 0; i < nparams; i++)
                            env_define(scope, names[i], rvals[i]);
                    }
                }
                {
                    static const char *SYM_LAZY_SEQ;
                    if (!SYM_LAZY_SEQ) SYM_LAZY_SEQ = intern("lazy-seq", 8);
                    if (s == SYM_LAZY_SEQ) {
                        /* (lazy-seq body...) => lazy cell over (fn [] body...) */
                        Cljc *thunk = make_fn(env, mk_cons(mk_empty_vec(), rest), false);
                        Cljc *l = alloc(CLJC_LAZY);  /* union zeroed: done=false */
                        l->as.lazy.thunk = thunk;
                        return l;
                    }
                }
                if (s == SYM_FN && !head_shadowed(env, head->as.sym)) {
                    /* (fn [x] ...) | (fn ([x] ...) ...) | (fn name [x] ...)
                     * Named fns see themselves via late binding: the name is
                     * defined into the closure env after construction. */
                    Cljc *body = rest;
                    const char *self_name = NULL;
                    if (body != NIL && body->as.cons.head->tag == CLJC_SYMBOL) {
                        self_name = body->as.cons.head->as.sym;
                        body = body->as.cons.tail;
                    }
                    if (!self_name) return make_fn(env, body, false);
                    CljcEnv *fenv = env_new(env);
                    Cljc *f = make_fn(fenv, body, false);
                    env_define(fenv, self_name, f);
                    return f;
                }
                /* Fell through every special/dot form → an ordinary call. Cache
                 * it on the head cell so repeats skip the cascade. Unqualified
                 * only: then strip_core_qualifier was a guaranteed no-op and the
                 * classification can't change under a later alias. */
                if (!strchr(head->as.sym, '/')) head->as.symc.plain = true;
            }

            /* Application. Macros get the unevaluated forms; the expansion
             * is then evaluated in the caller's environment. */
            Cljc *fn = eval(env, head);
            size_t base = vsp;
            if (fn->tag == CLJC_FN && fn->as.fn.is_macro) {
                for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
                    vpush(a->as.cons.head);    /* unevaluated forms */
                Cljc *expansion = apply(env, fn, &vstack[base], (int)(vsp - base));
                vsp = base;
                /* Splice the expansion into the call site: each macro use
                 * expands ONCE (like compiled Clojure — re-expansion was
                 * ~5%+ of hot loops). Redefining a macro doesn't reach
                 * already-spliced sites, matching JVM compiled code. */
                if (expansion != NIL && expansion->tag == CLJC_LIST) {
                    form->as.cons.head = expansion->as.cons.head;
                    form->as.cons.tail = expansion->as.cons.tail;
                } else {
                    Cljc *one = mk_cons(expansion, NIL);  /* (do <atom>) */
                    form->as.cons.head = mk_sym(intern("do", 2));
                    form->as.cons.tail = one;
                }
                return eval(env, expansion);
            }
            for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
                vpush(eval(env, a->as.cons.head));
            Cljc *result = apply(env, fn, &vstack[base], (int)(vsp - base));
            vsp = base;
            return result;
        }
    }
    return NIL;
}

/* ───── Printer ──────────────────────────────────────────────────────── */

/* Format a double like Clojure's Double.toString: the SHORTEST decimal that
 * round-trips, with decimal notation for 1e-3 <= |d| < 1e7 and d.dddExx
 * scientific otherwise; ##NaN / ##Inf / ##-Inf for non-finite. (C's %g loses
 * precision at its 6-sig-fig default and uses an "e+NN" exponent.) */
static void format_double(SBuf *sb, double d) {
    if (isnan(d)) { sb_puts(sb, "##NaN"); return; }
    if (isinf(d)) { sb_puts(sb, d < 0 ? "##-Inf" : "##Inf"); return; }
    if (d == 0.0) { sb_puts(sb, signbit(d) ? "-0.0" : "0.0"); return; }
    /* fewest significant digits that parse back to exactly d */
    int prec = 17;
    char tmp[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*e", p - 1, d);
        if (strtod(tmp, NULL) == d) { prec = p; break; }
    }
    snprintf(tmp, sizeof tmp, "%.*e", prec - 1, d);   /* -d.ddde±xx */
    char digits[20]; int ndig = 0, exp10 = 0; bool neg = false;
    const char *q = tmp;
    if (*q == '-') { neg = true; q++; }
    digits[ndig++] = *q++;                              /* leading digit */
    if (*q == '.') q++;
    while (*q && *q != 'e' && *q != 'E') digits[ndig++] = *q++;
    if (*q == 'e' || *q == 'E') exp10 = atoi(q + 1);
    while (ndig > 1 && digits[ndig - 1] == '0') ndig--; /* trim trailing zeros */
    digits[ndig] = '\0';
    if (neg) sb_putc(sb, '-');
    if (exp10 >= -3 && exp10 < 7) {                     /* decimal notation */
        if (exp10 >= 0) {
            int intlen = exp10 + 1;
            for (int i = 0; i < intlen; i++) sb_putc(sb, i < ndig ? digits[i] : '0');
            sb_putc(sb, '.');
            if (intlen >= ndig) sb_putc(sb, '0');
            else for (int i = intlen; i < ndig; i++) sb_putc(sb, digits[i]);
        } else {
            sb_puts(sb, "0.");
            for (int i = 0; i < -exp10 - 1; i++) sb_putc(sb, '0');
            for (int i = 0; i < ndig; i++) sb_putc(sb, digits[i]);
        }
    } else {                                            /* d.dddExx scientific */
        sb_putc(sb, digits[0]);
        sb_putc(sb, '.');
        if (ndig == 1) sb_putc(sb, '0');
        else for (int i = 1; i < ndig; i++) sb_putc(sb, digits[i]);
        char ebuf[8]; snprintf(ebuf, sizeof ebuf, "E%d", exp10);
        sb_puts(sb, ebuf);
    }
}

/* *print-length* / *print-level* support. Printing is non-reentrant, so a
 * static depth counter suffices; the var values are read through cached root
 * bindings (a binding mutates in place, so the cache stays valid). -1 = unset. */
static int g_print_depth;
static Binding *g_plen_b, *g_plevel_b;
static int print_var_int(Binding **cache, const char *nm, size_t n) {
    if (!*cache && gc_root_envs[0])
        *cache = root_find(env_root(gc_root_envs[0]), intern(nm, n), NULL);
    Cljc *v = *cache ? (*cache)->value : NIL;
    return (v != NIL && v->tag == CLJC_INT) ? (int)v->as.i : -1;
}
#define PRINT_LEN()   print_var_int(&g_plen_b,   "*print-length*", 14)
#define PRINT_LEVEL() print_var_int(&g_plevel_b, "*print-level*",  13)
/* at a collection: true if we're too deep to print it (render "#" instead) */
static bool print_too_deep(void) { int l = PRINT_LEVEL(); return l >= 0 && g_print_depth >= l; }

/* readably=true  → pr semantics: strings get quotes (read-back form)
 * readably=false → str/print semantics: strings render raw */
static void print_to(SBuf *sb, Cljc *v, bool readably) {
    if (v == NULL || v == NIL) { sb_puts(sb, "nil"); return; }
    switch (v->tag) {
        case CLJC_NIL: sb_puts(sb, "nil"); break;
        case CLJC_EMPTY: sb_puts(sb, "()"); break;
        case CLJC_BOOL: sb_puts(sb, v->as.b ? "true" : "false"); break;
        case CLJC_INT: sb_printf(sb, "%lld", (long long)v->as.i); break;
        case CLJC_BIGINT: { char *s = big_to_decimal(v); sb_puts(sb, s); if (readably) sb_putc(sb, 'N'); free(s); } break;
        case CLJC_RATIO: { char *a = big_to_decimal(v->as.ratio.num), *b = big_to_decimal(v->as.ratio.den);
            sb_puts(sb, a); sb_putc(sb, '/'); sb_puts(sb, b); free(a); free(b); } break;
        case CLJC_VAR: sb_puts(sb, "#'"); sb_puts(sb, v->as.var.name); break;
        case CLJC_DOUBLE: format_double(sb, v->as.d); break;
        case CLJC_SYMBOL: sb_puts(sb, v->as.sym); break;
        case CLJC_KEYWORD: sb_putc(sb, ':'); sb_puts(sb, v->as.kw); break;
        case CLJC_STRING:
            if (v->meta) {   /* a #"..."/re-pattern carries {:regex true} → #"..." */
                Cljc *rg;
                if (map_find(v->meta, mk_kw(intern("regex", 5)), &rg) && is_truthy(rg)) {
                    sb_putc(sb, '#'); sb_putc(sb, '"');
                    sb_puts(sb, v->as.str);
                    sb_putc(sb, '"');
                    break;
                }
            }
            if (readably) {
                sb_putc(sb, '"');
                for (const char *c = v->as.str; *c; c++) {
                    switch (*c) {
                        case '\n': sb_puts(sb, "\\n");  break;
                        case '\t': sb_puts(sb, "\\t");  break;
                        case '\r': sb_puts(sb, "\\r");  break;
                        case '\\': sb_puts(sb, "\\\\"); break;
                        case '"':  sb_puts(sb, "\\\""); break;
                        default:   sb_putc(sb, *c);
                    }
                }
                sb_putc(sb, '"');
            } else sb_puts(sb, v->as.str);
            break;
        case CLJC_CHAR: {
            int32_t cp = v->as.chr;
            if (readably) {
                /* round-trips through the reader: named, then printable
                 * ASCII as \x, else \uXXXX. */
                switch (cp) {
                    case '\n': sb_puts(sb, "\\newline"); break;
                    case ' ':  sb_puts(sb, "\\space"); break;
                    case '\t': sb_puts(sb, "\\tab"); break;
                    case '\r': sb_puts(sb, "\\return"); break;
                    case '\f': sb_puts(sb, "\\formfeed"); break;
                    case '\b': sb_puts(sb, "\\backspace"); break;
                    default:
                        if (cp >= 33 && cp < 127) { sb_putc(sb, '\\'); sb_putc(sb, (char)cp); }
                        else if (cp >= 127) {       /* printable non-ASCII: \<utf8>, like Clojure */
                            sb_putc(sb, '\\');
                            if (cp < 0x800) {
                                sb_putc(sb, (char)(0xC0 | (cp >> 6)));
                                sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
                            } else {
                                sb_putc(sb, (char)(0xE0 | (cp >> 12)));
                                sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                                sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
                            }
                        }
                        else { char tmp[8]; snprintf(tmp, sizeof tmp, "\\u%04X", cp); sb_puts(sb, tmp); }
                }
            } else {                       /* (str \a) => "a": UTF-8 encode */
                if (cp < 0x80) sb_putc(sb, (char)cp);
                else if (cp < 0x800) {
                    sb_putc(sb, (char)(0xC0 | (cp >> 6)));
                    sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
                } else {
                    sb_putc(sb, (char)(0xE0 | (cp >> 12)));
                    sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
                }
            }
            break;
        }
        case CLJC_LIST: {
            if (print_too_deep()) { sb_putc(sb, '#'); break; }
            sb_putc(sb, '(');
            bool first = true; int plen = PRINT_LEN(), cnt = 0;
            g_print_depth++;
            /* seq1 step: a lazy tail (cons onto lazy-seq) keeps printing */
            for (Cljc *l = v; l && l->tag == CLJC_LIST; l = seq1(l->as.cons.tail)) {
                if (!first) sb_putc(sb, ' ');
                first = false;
                if (plen >= 0 && cnt >= plen) { sb_puts(sb, "..."); break; }
                print_to(sb, l->as.cons.head, readably);
                cnt++;
            }
            g_print_depth--;
            sb_putc(sb, ')');
            break;
        }
        case CLJC_VECTOR: {
            if (print_too_deep()) { sb_putc(sb, '#'); break; }
            sb_putc(sb, '[');
            int plen = PRINT_LEN();
            g_print_depth++;
            for (size_t i = 0; i < vec_len(v); i++) {
                if (i) sb_putc(sb, ' ');
                if (plen >= 0 && (int)i >= plen) { sb_puts(sb, "..."); break; }
                print_to(sb, vec_nth(v, i), readably);
            }
            g_print_depth--;
            sb_putc(sb, ']');
            break;
        }
        case CLJC_MAP: {
            /* a StringBuilder ({:cljc/type :StringBuilder :v <atom-of-string>})
             * renders as its accumulated content — so (str sb) gives the token */
            Cljc *ty = NULL;
            if (map_find(v, mk_kw(intern("cljc/type", 9)), &ty) &&
                ty == mk_kw(intern("StringBuilder", 13))) {
                Cljc *vv;
                if (map_find(v, mk_kw(intern("v", 1)), &vv) && vv != NIL &&
                    vv->tag == CLJC_ATOM && vv->as.atom.value != NIL &&
                    vv->as.atom.value->tag == CLJC_STRING)
                    sb_puts(sb, vv->as.atom.value->as.str);
                break;
            }
            /* (str x) on a deftype with a toString method renders via toString
             * (hiccup's RawString); pr-str / print keep the structural map form.
             * `out != v` guards a toString that returns its own instance. */
            if (!readably && ty != NULL) {
                Cljc *out;
                if (dispatch_deftype_method(gc_root_envs[0], v, "toString", NULL, 0, &out) &&
                    out != v) {
                    print_to(sb, out, false);
                    break;
                }
            }
            (void)ty;
            if (print_too_deep()) { sb_putc(sb, '#'); break; }
            sb_putc(sb, '{');
            bool first = true; int plen = PRINT_LEN(), cnt = 0;
            g_print_depth++;
            for (Cljc *e = map_entry_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                if (!first) sb_puts(sb, ", ");
                first = false;
                if (plen >= 0 && cnt >= plen) { sb_puts(sb, "..."); break; }
                print_to(sb, e->as.cons.head->as.cons.head, readably);
                sb_putc(sb, ' ');
                print_to(sb, e->as.cons.head->as.cons.tail, readably);
                cnt++;
            }
            g_print_depth--;
            sb_putc(sb, '}');
            break;
        }
        case CLJC_SET: {
            if (print_too_deep()) { sb_putc(sb, '#'); break; }
            sb_puts(sb, "#{");
            bool sfirst = true; int plen = PRINT_LEN(), cnt = 0;
            g_print_depth++;
            for (Cljc *e = set_element_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                if (!sfirst) sb_putc(sb, ' ');
                sfirst = false;
                if (plen >= 0 && cnt >= plen) { sb_puts(sb, "..."); break; }
                print_to(sb, e->as.cons.head, readably);
                cnt++;
            }
            g_print_depth--;
            sb_putc(sb, '}');
            break;
        }
        case CLJC_SORTED: {
            /* a sorted map's entries are [k v] vectors; a sorted set's are bare */
            bool is_map = v->as.sorted.is_map;
            sb_puts(sb, is_map ? "{" : "#{");
            bool first = true;
            for (Cljc *e = sorted_entry_list(v); e != NIL; e = e->as.cons.tail) {
                if (!first) sb_puts(sb, is_map ? ", " : " ");
                first = false;
                Cljc *ent = e->as.cons.head;
                if (is_map) {
                    print_to(sb, vec_nth(ent, 0), readably);
                    sb_putc(sb, ' ');
                    print_to(sb, vec_nth(ent, 1), readably);
                } else {
                    print_to(sb, ent, readably);
                }
            }
            sb_putc(sb, '}');
            break;
        }
        case CLJC_TVEC:  sb_puts(sb, "#<transient-vector>"); break;
        case CLJC_HNODE: sb_puts(sb, "#<hamt-node>"); break;  /* never user-visible */
        case CLJC_TNODE: sb_puts(sb, "#<tree-node>"); break;  /* never user-visible */
        case CLJC_FN:     sb_puts(sb, "#<fn>"); break;
        case CLJC_NATIVE: sb_puts(sb, "#<native>"); break;
        case CLJC_CORO:   sb_puts(sb, "#<coroutine>"); break;
        case CLJC_ATOM:
            sb_puts(sb, "#atom[");
            print_to(sb, v->as.atom.value, readably);
            sb_putc(sb, ']');
            break;
        case CLJC_LAZY: { Cljc *s = to_seq(v);   /* realizes! empty seq prints () */
            print_to(sb, s == NIL ? EMPTY : s, readably); break; }
        case CLJC_RECUR:  sb_puts(sb, "#<recur>"); break;
        case CLJC_CHUNK:  sb_puts(sb, "#<chunk>"); break;
        case CLJC_FREE:   sb_puts(sb, "#<freed!>"); break;  /* seeing this is a GC bug */
    }
}

static void print(Cljc *v) {
    SBuf sb = {0};
    print_to(&sb, v, true);
    if (sb.data) { fwrite(sb.data, 1, sb.len, COUT); free(sb.data); }
}

/* Levenshtein distance, capped — for "did you mean" suggestions. */
static int lev(const char *a, const char *b, int cap) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la - lb > cap || lb - la > cap) return cap + 1;
    int prev[64], cur[64];
    if (lb >= 63) return cap + 1;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        int rowmin = cur[0];
        for (int j = 1; j <= lb; j++) {
            int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
            int d = (prev[j] < cur[j - 1] ? prev[j] : cur[j - 1]) + 1;
            cur[j] = c < d ? c : d;
            if (cur[j] < rowmin) rowmin = cur[j];
        }
        if (rowmin > cap) return cap + 1;
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

static const char *suggest(const char *token) {
    if (!gc_n_root_envs) return NULL;
    const char *best = NULL;
    int bestd = 3;  /* accept distance <= 2 */
    for (Binding *b = gc_root_envs[0]->bindings; b; b = b->next) {
        if (strstr(b->name, "**") || !strncmp(b->name, "cljc/", 5)) continue;
        int d = lev(token, b->name, 2);
        if (d < bestd) { bestd = d; best = b->name; }
    }
    return best;
}

static bool term_utf8(void) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_CTYPE");
    if (!l || !*l) l = getenv("LANG");
    return l && (strstr(l, "UTF-8") || strstr(l, "utf8") || strstr(l, "UTF8"));
}

/* Top-level (uncaught) error report, Elm-style: header, message, the
 * offending source line with a caret, a suggestion, then the trace. */
static void print_error(void) {
    bool color = isatty(fileno(stderr));
    bool u8 = term_utf8();
    const char *DASH  = u8 ? "\u2500" : "-";   /* ─ */
    const char *GUT   = u8 ? "\u2502" : "|";   /* │ */
    const char *CARET = u8 ? "\u2594" : "^";   /* ▔ */
    const char *RED = color ? "\033[31;1m" : "";
    const char *CYN = color ? "\033[36m" : "";
    const char *YEL = color ? "\033[33m" : "";
    const char *DIM = color ? "\033[2m" : "";
    const char *OFF = color ? "\033[0m" : "";

    fprintf(CERR, "%s%s%s ERROR ", RED, DASH, DASH);
    int pad = 58 - (int)(err_src_name ? strlen(err_src_name) : 0);
    for (int i = 0; i < pad; i++) fputs(DASH, CERR);
    fprintf(CERR, " %s%s\n\n", err_src_name ? err_src_name : "", OFF);

    if (cur_exc) {
        SBuf sb = {0};
        print_to(&sb, cur_exc, true);
        fputs("Uncaught exception: ", CERR);
        if (sb.data) { fwrite(sb.data, 1, sb.len, CERR); free(sb.data); }
        fputc('\n', CERR);
        cur_exc = NULL;
    } else {
        fprintf(CERR, "%s\n", err_msg);
    }

    /* the offending source line, caret under the token when findable */
    if (err_line > 0 && err_src_text) {
        const char *p = err_src_text;
        for (long long l = 1; l < err_line && p; l++) {
            p = strchr(p, '\n');
            if (p) p++;
        }
        if (p) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            fprintf(CERR, "\n%s%4lld %s%s ", CYN, err_line, GUT, OFF);
            fwrite(p, 1, len, CERR);
            fputc('\n', CERR);
            const char *hit = NULL;
            size_t hitlen = 1;
            if (err_token) {   /* precise: the named token, searched from the form's col */
                const char *from = (err_col > 0 && (size_t)err_col <= len)
                    ? p + err_col - 1 : p;
                hit = strstr(from, err_token);
                if (!hit || (e && hit >= e)) hit = strstr(p, err_token);
                if (hit && e && hit >= e) hit = NULL;
                if (hit) hitlen = strlen(err_token);
            }
            if (!hit && err_col > 0 && (size_t)err_col <= len) {
                hit = p + err_col - 1;   /* fallback: the form's opening paren */
                hitlen = 1;
            }
            if (hit) {
                fprintf(CERR, "     %s%s ", CYN, GUT);
                for (const char *c = p; c < hit; c++)
                    fputc(*c == '\t' ? '\t' : ' ', CERR);
                fprintf(CERR, "%s", RED);
                for (size_t i = 0; i < hitlen; i++) fputs(CARET, CERR);
                fprintf(CERR, "%s\n", OFF);
            }
        }
    }

    if (err_token) {
        const char *s = suggest(err_token);
        if (s) fprintf(CERR, "\n%sDid you mean %s`%s`%s?%s\n", YEL, OFF, s, YEL, OFF);
        err_token = NULL;
    }

    if (err_trace[0]) fprintf(CERR, "\n%s%s%s", DIM, err_trace, OFF);
    err_top = NULL;  /* hygiene: no handler frames survive a top-level unwind */
}

/* ───── Primitives ───────────────────────────────────────────────────── */

/* Arithmetic fold with Clojure semantics:
 *   - ints stay ints; any double promotes the whole result
 *   - unary: (- x) negates, (/ x) reciprocates
 *   - integer / that doesn't divide evenly promotes to double
 *     (real Clojure makes a Ratio — a deliberate v0 divergence) */
/* ───── Arbitrary-precision integers ─────────────────────────────────────
 * Sign-magnitude, base-2^32 little-endian limbs. Only created when a value
 * exceeds int64; big_norm demotes back to CLJC_INT whenever the result fits,
 * so CLJC_INT vs CLJC_BIGINT is purely a function of magnitude. Local Cljc*
 * are GC roots (conservative C-stack scan), so no manual rooting is needed;
 * malloc'd limb arrays are owned by their cell and freed in the GC sweep. */
static uint32_t *umalloc(uint32_t n) { return xmalloc(sizeof(uint32_t) * (n ? n : 1)); }

typedef struct { int sign; uint32_t n; const uint32_t *mag; } BigView;

/* View any integer (CLJC_INT or CLJC_BIGINT) as sign + magnitude limbs. For an
 * int64 the up-to-2 limbs land in the caller's scratch. sign==0 means zero. */
static void big_view(Cljc *v, BigView *bv, uint32_t scratch[2]) {
    if (v->tag == CLJC_BIGINT) {
        bv->sign = v->as.big.sign; bv->n = v->as.big.n; bv->mag = v->as.big.mag; return;
    }
    int64_t x = v->as.i;
    uint64_t u = x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;   /* INT64_MIN-safe */
    bv->sign = x < 0 ? -1 : (x ? 1 : 0);
    scratch[0] = (uint32_t)u; scratch[1] = (uint32_t)(u >> 32);
    bv->n = scratch[1] ? 2 : (scratch[0] ? 1 : 0);
    bv->mag = scratch;
}

/* Wrap sign + freshly malloc'd magnitude (n limbs, ownership taken) as a value,
 * trimming leading zero limbs and demoting to CLJC_INT when it fits. */
static Cljc *big_norm(int sign, uint32_t *mag, uint32_t n) {
    while (n > 0 && mag[n - 1] == 0) n--;
    if (n == 0) { free(mag); return mk_int(0); }
    if (n <= 2) {
        uint64_t u = mag[0] | (n == 2 ? ((uint64_t)mag[1] << 32) : 0);
        if (u <= (uint64_t)INT64_MAX) { free(mag); return mk_int(sign < 0 ? -(int64_t)u : (int64_t)u); }
        if (sign < 0 && u == (uint64_t)INT64_MAX + 1) { free(mag); return mk_int(INT64_MIN); }
    }
    Cljc *v = alloc(CLJC_BIGINT);
    v->as.big.sign = sign < 0 ? -1 : 1; v->as.big.n = n; v->as.big.mag = mag;
    return v;
}

static int umag_cmp(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (uint32_t i = na; i-- > 0; ) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static uint32_t umag_add(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, uint32_t *out) {
    if (na < nb) { const uint32_t *t = a; a = b; b = t; uint32_t k = na; na = nb; nb = k; }
    uint64_t carry = 0; uint32_t i;
    for (i = 0; i < nb; i++) { uint64_t s = (uint64_t)a[i] + b[i] + carry; out[i] = (uint32_t)s; carry = s >> 32; }
    for (; i < na; i++) { uint64_t s = (uint64_t)a[i] + carry; out[i] = (uint32_t)s; carry = s >> 32; }
    if (carry) out[i++] = (uint32_t)carry;
    return i;
}
static uint32_t umag_sub(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, uint32_t *out) {
    int64_t borrow = 0; uint32_t i;            /* requires a >= b */
    for (i = 0; i < nb; i++) { int64_t d = (int64_t)a[i] - b[i] - borrow; borrow = d < 0; if (d < 0) d += (int64_t)1 << 32; out[i] = (uint32_t)d; }
    for (; i < na; i++) { int64_t d = (int64_t)a[i] - borrow; borrow = d < 0; if (d < 0) d += (int64_t)1 << 32; out[i] = (uint32_t)d; }
    return na;
}
static uint32_t umag_mul(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, uint32_t *out) {
    for (uint32_t i = 0; i < na + nb; i++) out[i] = 0;
    for (uint32_t i = 0; i < na; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < nb; j++) { uint64_t s = (uint64_t)a[i] * b[j] + out[i + j] + carry; out[i + j] = (uint32_t)s; carry = s >> 32; }
        out[i + nb] += (uint32_t)carry;
    }
    return na + nb;
}

static Cljc *big_addsub(Cljc *A, Cljc *B, int sub) {
    uint32_t sa[2], sb[2]; BigView a, b; big_view(A, &a, sa); big_view(B, &b, sb);
    int bsign = sub ? -b.sign : b.sign;
    if (a.sign == 0) { if (bsign == 0) return mk_int(0); uint32_t *m = umalloc(b.n); memcpy(m, b.mag, b.n * 4); return big_norm(bsign, m, b.n); }
    if (bsign == 0) { uint32_t *m = umalloc(a.n); memcpy(m, a.mag, a.n * 4); return big_norm(a.sign, m, a.n); }
    if (a.sign == bsign) { uint32_t cap = (a.n > b.n ? a.n : b.n) + 1; uint32_t *out = umalloc(cap); uint32_t n = umag_add(a.mag, a.n, b.mag, b.n, out); return big_norm(a.sign, out, n); }
    int c = umag_cmp(a.mag, a.n, b.mag, b.n);
    if (c == 0) return mk_int(0);
    uint32_t cap = a.n > b.n ? a.n : b.n; uint32_t *out = umalloc(cap);
    if (c > 0) return big_norm(a.sign, out, umag_sub(a.mag, a.n, b.mag, b.n, out));
    return big_norm(bsign, out, umag_sub(b.mag, b.n, a.mag, a.n, out));
}
static Cljc *big_mul(Cljc *A, Cljc *B) {
    uint32_t sa[2], sb[2]; BigView a, b; big_view(A, &a, sa); big_view(B, &b, sb);
    if (a.sign == 0 || b.sign == 0) return mk_int(0);
    uint32_t *out = umalloc(a.n + b.n); uint32_t n = umag_mul(a.mag, a.n, b.mag, b.n, out);
    return big_norm(a.sign * b.sign, out, n);
}
static int big_cmp(Cljc *A, Cljc *B) {
    uint32_t sa[2], sb[2]; BigView a, b; big_view(A, &a, sa); big_view(B, &b, sb);
    if (a.sign != b.sign) return a.sign < b.sign ? -1 : 1;
    if (a.sign == 0) return 0;
    int c = umag_cmp(a.mag, a.n, b.mag, b.n);
    return a.sign < 0 ? -c : c;
}

/* Truncating divmod (quot toward zero, rem takes the dividend's sign). Bit-by-
 * bit long division on magnitudes — simple and correct; fine at our scale. */
static void big_divmod(Cljc *A, Cljc *B, Cljc **Q, Cljc **R) {
    uint32_t sa[2], sb[2]; BigView a, b; big_view(A, &a, sa); big_view(B, &b, sb);
    if (b.sign == 0) cljc_error("Divide by zero");
    if (a.sign == 0 || umag_cmp(a.mag, a.n, b.mag, b.n) < 0) {
        if (Q) *Q = mk_int(0);
        if (R) { uint32_t *m = umalloc(a.n ? a.n : 1); memcpy(m, a.mag, a.n * 4); *R = big_norm(a.sign, m, a.n); }
        return;
    }
    uint32_t qn = a.n, rn = a.n + 1;
    uint32_t *q = umalloc(qn); for (uint32_t i = 0; i < qn; i++) q[i] = 0;
    uint32_t *r = umalloc(rn); for (uint32_t i = 0; i < rn; i++) r[i] = 0;
    for (uint32_t bit = a.n * 32; bit-- > 0; ) {
        uint32_t carry = 0;                                   /* r <<= 1 */
        for (uint32_t i = 0; i < rn; i++) { uint32_t nx = r[i] >> 31; r[i] = (r[i] << 1) | carry; carry = nx; }
        if ((a.mag[bit / 32] >> (bit % 32)) & 1) r[0] |= 1;   /* bring down next bit */
        uint32_t rl = rn; while (rl > 0 && r[rl - 1] == 0) rl--;
        if (umag_cmp(r, rl, b.mag, b.n) >= 0) { umag_sub(r, rl, b.mag, b.n, r); q[bit / 32] |= 1u << (bit % 32); }
    }
    if (Q) *Q = big_norm(a.sign * b.sign, q, qn); else free(q);
    if (R) *R = big_norm(a.sign, r, rn); else free(r);
}
static Cljc *big_abs(Cljc *A) {
    if (A->tag == CLJC_INT) return A->as.i < 0 ? big_addsub(mk_int(0), A, 1) : A;
    uint32_t *m = umalloc(A->as.big.n); memcpy(m, A->as.big.mag, A->as.big.n * 4);
    return big_norm(1, m, A->as.big.n);
}
static bool big_is_zero(Cljc *A) { return A->tag == CLJC_INT && A->as.i == 0; }
static Cljc *big_gcd(Cljc *A, Cljc *B) {
    Cljc *a = big_abs(A), *b = big_abs(B);
    while (!big_is_zero(b)) { Cljc *r; big_divmod(a, b, NULL, &r); a = b; b = r; }
    return a;
}
static double big_to_double(Cljc *v) {
    if (v->tag == CLJC_INT) return (double)v->as.i;
    double d = 0; for (uint32_t i = v->as.big.n; i-- > 0; ) d = d * 4294967296.0 + v->as.big.mag[i];
    return v->as.big.sign < 0 ? -d : d;
}
static uint32_t udiv_small_inplace(uint32_t *d, uint32_t n, uint32_t div) {
    uint64_t rem = 0;
    for (uint32_t i = n; i-- > 0; ) { uint64_t cur = (rem << 32) | d[i]; d[i] = (uint32_t)(cur / div); rem = cur % div; }
    return (uint32_t)rem;
}
/* Decimal text (with sign). Caller frees. */
static char *big_to_decimal(Cljc *v) {
    if (v->tag == CLJC_INT) { char *b = xmalloc(24); snprintf(b, 24, "%lld", (long long)v->as.i); return b; }
    uint32_t n = v->as.big.n, tn = n; uint32_t *t = umalloc(n); memcpy(t, v->as.big.mag, n * 4);
    uint32_t *chunks = xmalloc(sizeof(uint32_t) * ((size_t)n + 2)); size_t nc = 0;
    while (tn > 0) { uint32_t r = udiv_small_inplace(t, tn, 1000000000u); while (tn > 0 && t[tn - 1] == 0) tn--; chunks[nc++] = r; }
    free(t);
    size_t cap = nc * 9 + 4; char *buf = xmalloc(cap); size_t p = 0;
    if (v->as.big.sign < 0) buf[p++] = '-';
    p += (size_t)snprintf(buf + p, cap - p, "%u", chunks[nc - 1]);
    for (size_t i = nc - 1; i-- > 0; ) p += (size_t)snprintf(buf + p, cap - p, "%09u", chunks[i]);
    buf[p] = 0; free(chunks); return buf;
}
/* Parse a decimal integer (optional sign, all digits) into INT or BIGINT. */
static Cljc *big_from_decimal(const char *s) {
    int sign = 1; if (*s == '+') s++; else if (*s == '-') { sign = -1; s++; }
    Cljc *acc = mk_int(0), *ten = mk_int(10);
    for (; *s; s++) { if (*s < '0' || *s > '9') break; acc = big_addsub(big_mul(acc, ten), mk_int(*s - '0'), 0); }
    return sign < 0 ? big_addsub(mk_int(0), acc, 1) : acc;
}

/* ───── Exact rationals ──────────────────────────────────────────────── */
static Cljc *rat_num(Cljc *v) { return v->tag == CLJC_RATIO ? v->as.ratio.num : v; }
static Cljc *rat_den(Cljc *v) { return v->tag == CLJC_RATIO ? v->as.ratio.den : mk_int(1); }

/* Build a reduced rational from num/den (each INT or BIGINT): force den>0,
 * divide out the gcd, and demote to an integer when den becomes 1. */
static Cljc *make_ratio(Cljc *num, Cljc *den) {
    if (big_is_zero(den)) cljc_error("Divide by zero");
    if (big_cmp(den, mk_int(0)) < 0) { num = big_addsub(mk_int(0), num, 1); den = big_addsub(mk_int(0), den, 1); }
    if (big_is_zero(num)) return mk_int(0);
    Cljc *g = big_gcd(num, den);
    if (!(g->tag == CLJC_INT && g->as.i == 1)) {
        Cljc *qn, *qd; big_divmod(num, g, &qn, NULL); big_divmod(den, g, &qd, NULL);
        num = qn; den = qd;
    }
    if (den->tag == CLJC_INT && den->as.i == 1) return num;
    Cljc *r = alloc(CLJC_RATIO);
    r->as.ratio.num = num; r->as.ratio.den = den;
    return r;
}
static double ratio_to_double(Cljc *v) {
    return big_to_double(v->as.ratio.num) / big_to_double(v->as.ratio.den);
}

typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV } ArithOp;

static Cljc *big_binop(ArithOp op, Cljc *a, Cljc *b) {
    switch (op) {
        case OP_ADD: return big_addsub(a, b, 0);
        case OP_SUB: return big_addsub(a, b, 1);
        case OP_MUL: return big_mul(a, b);
        default: return mk_int(0);  /* DIV handled separately */
    }
}

/* promote=false: int64 ops wrap (the +,-,* used everywhere, incl. unchecked).
 * promote=true: int64 overflow auto-promotes to bigint (the +',-',*' ops). A
 * bigint operand always takes the exact path regardless of `promote`. */
static Cljc *arith(ArithOp op, Cljc **argv, int nargs, bool promote) {
    size_t n = (size_t)nargs;
    bool is_float = false, is_big = false, is_ratio = false;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        Cljc *v = argv[ai_];
        if (v->tag == CLJC_DOUBLE) is_float = true;
        else if (v->tag == CLJC_BIGINT) is_big = true;
        else if (v->tag == CLJC_RATIO) is_ratio = true;
        else if (v->tag != CLJC_INT) cljc_error("expected a number, got %s", val_type_name(v));
    }
    if (n == 0) {
        if (op == OP_ADD) return mk_int(0);
        if (op == OP_MUL) return mk_int(1);
        cljc_error("wrong number of args (0)");
    }
    if (is_float) goto float_path;
    /* division of exact integers yields an exact ratio (Clojure), so all
     * non-float division and anything touching a ratio takes the ratio path */
    if (op == OP_DIV || is_ratio) goto ratio_path;
    if (is_big) goto big_path;

    {
        int64_t acc = argv[0]->as.i;
        int ai_;
        if (n == 1) {
            if (op == OP_SUB) {  /* negate; INT64_MIN overflows -> promote */
                if (promote && acc == INT64_MIN) return big_addsub(mk_int(0), argv[0], 1);
                return mk_int((int64_t)(0u - (uint64_t)acc));  /* unsigned: no UB at MIN */
            }
            if (op == OP_DIV) {
                if (acc == 0) cljc_error("Divide by zero");
                return acc == 1 || acc == -1 ? mk_int(acc) : mk_double(1.0 / (double)acc);
            }
            return mk_int(acc);
        }
        for (ai_ = 1; ai_ < nargs; ai_++) {
            int64_t x = argv[ai_]->as.i;
            if (op == OP_DIV) {
                if (x == 0) cljc_error("Divide by zero");
                if (acc % x != 0) { is_float = true; goto float_path; }
                acc /= x;
                continue;
            }
            if (promote) {            /* checked: spill into bigint on overflow */
                int64_t r; bool ovf;
                if (op == OP_ADD) ovf = __builtin_add_overflow(acc, x, &r);
                else if (op == OP_SUB) ovf = __builtin_sub_overflow(acc, x, &r);
                else ovf = __builtin_mul_overflow(acc, x, &r);
                if (ovf) {
                    Cljc *b = mk_int(acc);
                    for (; ai_ < nargs; ai_++) b = big_binop(op, b, argv[ai_]);
                    return b;
                }
                acc = r;
            } else {   /* unchecked: wrap in unsigned to keep it well-defined (no UB) */
                uint64_t u = (uint64_t)acc, ux = (uint64_t)x;
                switch (op) {
                    case OP_ADD: u += ux; break;
                    case OP_SUB: u -= ux; break;
                    case OP_MUL: u *= ux; break;
                    default: break;
                }
                acc = (int64_t)u;
            }
        }
        return mk_int(acc);
    }

big_path: {
        Cljc *acc = argv[0];
        if (n == 1) {
            if (op == OP_SUB) return big_addsub(mk_int(0), acc, 1);
            if (op == OP_DIV) return mk_double(1.0 / big_to_double(acc));
            return acc;
        }
        for (int ai_ = 1; ai_ < nargs; ai_++) {
            if (op == OP_DIV) {       /* exact if it divides, else fall to double */
                Cljc *q, *r; big_divmod(acc, argv[ai_], &q, &r);
                if (!big_is_zero(r)) { is_float = true; goto float_path; }
                acc = q;
            } else acc = big_binop(op, acc, argv[ai_]);
        }
        return acc;
    }

ratio_path: {
        Cljc *rn = rat_num(argv[0]), *rd = rat_den(argv[0]);
        if (n == 1) {
            if (op == OP_DIV) return make_ratio(rd, rn);                 /* reciprocal */
            if (op == OP_SUB) return make_ratio(big_addsub(mk_int(0), rn, 1), rd);
            return argv[0];
        }
        for (int ai_ = 1; ai_ < nargs; ai_++) {
            Cljc *n2 = rat_num(argv[ai_]), *d2 = rat_den(argv[ai_]);
            switch (op) {                                               /* a/b OP c/d */
                case OP_ADD: rn = big_addsub(big_mul(rn, d2), big_mul(n2, rd), 0); rd = big_mul(rd, d2); break;
                case OP_SUB: rn = big_addsub(big_mul(rn, d2), big_mul(n2, rd), 1); rd = big_mul(rd, d2); break;
                case OP_MUL: rn = big_mul(rn, n2); rd = big_mul(rd, d2); break;
                case OP_DIV: rn = big_mul(rn, d2); rd = big_mul(rd, n2); break;
            }
            Cljc *g = big_gcd(rn, rd);    /* reduce each step to bound intermediates */
            if (!(g->tag == CLJC_INT && g->as.i == 1)) { Cljc *a, *b; big_divmod(rn, g, &a, NULL); big_divmod(rd, g, &b, NULL); rn = a; rd = b; }
        }
        return make_ratio(rn, rd);
    }

float_path:;
    double facc = as_num(argv[0]);
    int aj_;
    if (n == 1) {
        if (op == OP_SUB) return mk_double(-facc);
        if (op == OP_DIV) return mk_double(1.0 / facc);
        return mk_double(facc);
    }
    for (aj_ = 1; aj_ < nargs; aj_++) {
        double x = as_num(argv[aj_]);
        switch (op) {
            case OP_ADD: facc += x; break;
            case OP_SUB: facc -= x; break;
            case OP_MUL: facc *= x; break;
            case OP_DIV: facc /= x; break;  /* /0.0 → ±Infinity, like Clojure doubles */
        }
    }
    return mk_double(facc);
}

static Cljc *prim_add(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_ADD, argv, nargs, false); }
static Cljc *prim_sub(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_SUB, argv, nargs, false); }
static Cljc *prim_mul(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_MUL, argv, nargs, false); }
static Cljc *prim_div(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_DIV, argv, nargs, false); }
/* auto-promoting +' -' *' (Clojure): int64 overflow grows to bigint */
static Cljc *prim_addq(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_ADD, argv, nargs, true); }
static Cljc *prim_subq(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_SUB, argv, nargs, true); }
static Cljc *prim_mulq(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_MUL, argv, nargs, true); }
static Cljc *prim_numerator(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs; Cljc *v = argv[0];
    if (v->tag == CLJC_RATIO) return v->as.ratio.num;
    if (v->tag == CLJC_INT || v->tag == CLJC_BIGINT) return v;
    cljc_error("numerator: not a rational");  return NIL;
}
static Cljc *prim_denominator(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs; Cljc *v = argv[0];
    if (v->tag == CLJC_RATIO) return v->as.ratio.den;
    if (v->tag == CLJC_INT || v->tag == CLJC_BIGINT) return mk_int(1);
    cljc_error("denominator: not a rational");  return NIL;
}

/* Sequential equality across lists and vectors, Clojure-style:
 * (= [1 2 3] '(1 2 3)) is true. */
static bool seq_eq(Cljc *a, Cljc *b) {
    /* Normalize both to an index/cursor walk. */
    Cljc *la = (a->tag == CLJC_LIST) ? a : NULL;
    Cljc *lb = (b->tag == CLJC_LIST) ? b : NULL;
    size_t ia = 0, ib = 0;
    for (;;) {
        bool done_a = la ? (la == NIL) : (ia >= vec_len(a));
        bool done_b = lb ? (lb == NIL) : (ib >= vec_len(b));
        if (done_a || done_b) return done_a && done_b;
        Cljc *xa = la ? la->as.cons.head : vec_nth(a, ia);
        Cljc *xb = lb ? lb->as.cons.head : vec_nth(b, ib);
        if (!cljc_eq(xa, xb)) return false;
        if (la) la = la->as.cons.tail; else ia++;
        if (lb) lb = lb->as.cons.tail; else ib++;
    }
}

/* A sorted set/map is = to a hash set/map (or another sorted coll) with the same
 * contents — Clojure's = compares collections by category, not representation. */
static size_t collcount_(Cljc *c) {
    return c->tag == CLJC_SORTED ? sorted_count(c) : c->as.map.count;
}
static bool setlike_member_(Cljc *s, Cljc *x) {
    if (s->tag == CLJC_SET) return set_contains(s, x, NULL);
    return sorted_contains(gc_root_envs[0], s, x);
}
static bool setlike_eq_(Cljc *a, Cljc *b) {
    if (collcount_(a) != collcount_(b)) return false;
    Cljc *es = a->tag == CLJC_SORTED ? sorted_entry_list(a) : set_element_list(a);
    for (Cljc *e = es; e != NIL; e = e->as.cons.tail)
        if (!setlike_member_(b, e->as.cons.head)) return false;
    return true;
}
static bool maplike_eq_(Cljc *a, Cljc *b) {
    if (collcount_(a) != collcount_(b)) return false;
    Cljc *es = a->tag == CLJC_SORTED ? sorted_entry_list(a) : map_entry_list(a);
    for (Cljc *e = es; e != NIL; e = e->as.cons.tail) {
        Cljc *entry = e->as.cons.head, *k, *v, *bv;
        if (a->tag == CLJC_SORTED) { k = vec_nth(entry, 0); v = vec_nth(entry, 1); }
        else { k = entry->as.cons.head; v = entry->as.cons.tail; }
        if (b->tag == CLJC_MAP ? !map_find(b, k, &bv)
                               : !sorted_get(gc_root_envs[0], b, k, &bv)) return false;
        if (!cljc_eq(v, bv)) return false;
    }
    return true;
}

static bool cljc_eq(Cljc *a, Cljc *b) {
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    bool a_seq = a->tag == CLJC_LIST || a->tag == CLJC_VECTOR || a->tag == CLJC_LAZY || a->tag == CLJC_EMPTY;
    bool b_seq = b->tag == CLJC_LIST || b->tag == CLJC_VECTOR || b->tag == CLJC_LAZY || b->tag == CLJC_EMPTY;
    if (a_seq && b_seq)  /* to_seq also surfaces lazy tails hidden in lists */
        return seq_eq(a->tag == CLJC_VECTOR ? a : to_seq(a),
                      b->tag == CLJC_VECTOR ? b : to_seq(b));
    /* sorted collections compare as their set/map category, across tags */
    if (a->tag == CLJC_SORTED || b->tag == CLJC_SORTED) {
        bool a_set = a->tag == CLJC_SET || (a->tag == CLJC_SORTED && !a->as.sorted.is_map);
        bool b_set = b->tag == CLJC_SET || (b->tag == CLJC_SORTED && !b->as.sorted.is_map);
        if (a_set && b_set) return setlike_eq_(a, b);
        bool a_map = a->tag == CLJC_MAP || (a->tag == CLJC_SORTED && a->as.sorted.is_map);
        bool b_map = b->tag == CLJC_MAP || (b->tag == CLJC_SORTED && b->as.sorted.is_map);
        if (a_map && b_map) return maplike_eq_(a, b);
        return false;
    }
    if (a->tag != b->tag) {
        /* Clojure's = is category-sensitive across the numeric tower: an exact
         * integer never equals a floating-point value (only == crosses that),
         * so (= -1 -1.0) is false. This matters for set/map literals like
         * #{-1 -1.0}, which are two distinct elements. Integer<->bigint DO
         * compare (bignums demote to int when they fit, so same tag here). */
        return false;
    }
    switch (a->tag) {
        case CLJC_NIL: return true;
        case CLJC_BOOL: return a->as.b == b->as.b;
        case CLJC_INT: return a->as.i == b->as.i;
        case CLJC_BIGINT: return big_cmp(a, b) == 0;
        case CLJC_RATIO: return big_cmp(a->as.ratio.num, b->as.ratio.num) == 0 &&
                                big_cmp(a->as.ratio.den, b->as.ratio.den) == 0;
        case CLJC_DOUBLE: return a->as.d == b->as.d;
        case CLJC_SYMBOL: return a->as.sym == b->as.sym;
        case CLJC_KEYWORD: return a->as.kw == b->as.kw;
        case CLJC_STRING: return strcmp(a->as.str, b->as.str) == 0;
        case CLJC_CHAR: return a->as.chr == b->as.chr;
        case CLJC_MAP: {
            /* a deftype with a custom equiv/equals (e.g. DataScript's Datom
             * compares only e/a/v/tx, ignoring its mutable hash/idx cache fields)
             * defines its own equality — Clojure's = dispatches to equiv, so
             * field-by-field map comparison is wrong for it. */
            { Cljc *ty;
              static const char *KW_CT; if (!KW_CT) KW_CT = intern("cljc/type", 9);
              if (map_find(a, mk_kw(KW_CT), &ty)) {
                  Cljc *barg[1] = { b }, *out;
                  if (dispatch_deftype_method(gc_root_envs[0], a, "equiv", barg, 1, &out) ||
                      dispatch_deftype_method(gc_root_envs[0], a, "equals", barg, 1, &out))
                      return is_truthy(out);
              }
            }
            if (a->as.map.count != b->as.map.count) return false;
            for (Cljc *e = map_entry_list(a); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                Cljc *bv;
                if (!map_find(b, e->as.cons.head->as.cons.head, &bv)) return false;
                if (!cljc_eq(e->as.cons.head->as.cons.tail, bv)) return false;
            }
            return true;
        }
        case CLJC_SET: {
            if (a->as.map.count != b->as.map.count) return false;
            for (Cljc *e = set_element_list(a); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
                if (!set_contains(b, e->as.cons.head, NULL)) return false;
            return true;
        }
        default: return false;
    }
}

static Cljc *prim_eq(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs == 0) return TRUE;
    Cljc *first = argv[0];
    for (int i = 1; i < nargs; i++)
        if (!cljc_eq(first, argv[i])) return FALSE;
    return TRUE;
}

static double as_num(Cljc *v) {
    if (v->tag == CLJC_INT) return (double)v->as.i;
    if (v->tag == CLJC_DOUBLE) return v->as.d;
    if (v->tag == CLJC_BIGINT) return big_to_double(v);
    if (v->tag == CLJC_RATIO) return ratio_to_double(v);
    cljc_error("expected a number, got %s", val_type_name(v));
    return 0;
}

/* Chained comparisons: (< 1 2 3) is true iff each adjacent pair satisfies OP. */
/* Numeric ordering across int/bigint/double; exact for bigints (a double
 * comparison would lose precision between adjacent large integers). */
static int num_cmp(Cljc *a, Cljc *b) {
    int ta = a->tag, tb = b->tag;
#define CLJC_ISNUM(t) ((t) == CLJC_INT || (t) == CLJC_DOUBLE || (t) == CLJC_BIGINT || (t) == CLJC_RATIO)
    if (!CLJC_ISNUM(ta) || !CLJC_ISNUM(tb))
        cljc_error("can't compare numerically: got %s", val_type_name(!CLJC_ISNUM(ta) ? a : b));
    if (ta == CLJC_DOUBLE || tb == CLJC_DOUBLE) {
        double x = as_num(a), y = as_num(b); return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (ta == CLJC_RATIO || tb == CLJC_RATIO)   /* a/b ? c/d  <=>  a*d ? c*b (dens > 0) */
        return big_cmp(big_mul(rat_num(a), rat_den(b)), big_mul(rat_num(b), rat_den(a)));
    if (ta == CLJC_BIGINT || tb == CLJC_BIGINT) return big_cmp(a, b);
    int64_t x = a->as.i, y = b->as.i; return x < y ? -1 : (x > y ? 1 : 0);
}

#define COMPARISON(NAME, OP) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        Cljc *prev = NULL; \
        for (int ai_ = 0; ai_ < nargs; ai_++) { \
            Cljc *v = argv[ai_]; \
            if (prev && !(num_cmp(prev, v) OP 0)) return FALSE; \
            prev = v; \
        } \
        return TRUE; \
    }

COMPARISON(lt, <)
COMPARISON(gt, >)
COMPARISON(le, <=)
COMPARISON(ge, >=)
COMPARISON(num_eq, ==)   /* == : numeric value-equality across the tower (1 == 1.0) */

/* Write `len` bytes to the current value of *out* (or *err* via binding): the
 * :cljc/stdout / :cljc/stderr sentinels go to the real streams (so nREPL capture
 * via cljc_out still works); a StringWriter (a :StringBuilder) accumulates, which
 * is what binds with-out-str. The *out* binding pointer is cached — cljc's
 * `binding` mutates the binding in place, so the cache stays correct. */
static Binding *g_out_binding;
static const char *g_kw_stderr;
static void emit_out(CljcEnv *env, const char *data, size_t len) {
    if (!g_out_binding) g_out_binding = root_find(env_root(env), intern("*out*", 5), NULL);
    Cljc *w = g_out_binding ? g_out_binding->value : NIL;
    if (w == NIL) { fwrite(data, 1, len, COUT); return; }
    if (w->tag == CLJC_KEYWORD) {
        if (!g_kw_stderr) g_kw_stderr = intern("cljc/stderr", 11);
        fwrite(data, 1, len, w->as.kw == g_kw_stderr ? CERR : COUT);
        return;
    }
    if (w->tag == CLJC_MAP) {            /* a StringWriter: append to its :v atom */
        Cljc *vv;
        if (map_find(w, mk_kw(intern("v", 1)), &vv) && vv != NIL && vv->tag == CLJC_ATOM) {
            Cljc *old = vv->as.atom.value;
            const char *os = (old != NIL && old->tag == CLJC_STRING) ? old->as.str : "";
            size_t ol = strlen(os);
            char *buf = xmalloc(ol + len + 1);
            memcpy(buf, os, ol); memcpy(buf + ol, data, len); buf[ol + len] = '\0';
            vv->as.atom.value = mk_str(buf, ol + len);
            free(buf);
            return;
        }
    }
    fwrite(data, 1, len, COUT);          /* unknown writer: fall back to stdout */
}

static Cljc *prim_println(CljcEnv *env, Cljc **argv, int nargs) {
    SBuf sb = {0};
    bool first = true;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, argv[ai_], false);
    }
    sb_putc(&sb, '\n');
    emit_out(env, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return NIL;
}

static Cljc *prim_str(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    SBuf sb = {0};
    sb_grow(&sb, 1); sb.data[0] = '\0';
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        Cljc *v = argv[ai_];
        if (v != NIL) print_to(&sb, v, false);  /* (str nil) => "" */
    }
    Cljc *r = mk_str(sb.data, sb.len);
    free(sb.data);
    return r;
}

static Cljc *prim_pr_str(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    SBuf sb = {0};
    sb_grow(&sb, 1); sb.data[0] = '\0';
    bool first = true;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, argv[ai_], true);
    }
    Cljc *r = mk_str(sb.data, sb.len);
    free(sb.data);
    return r;
}

static Cljc *prim_not(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_bool(!is_truthy(argv[0]));
}

static Cljc *prim_count(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    if (argv[0] == NIL || argv[0]->tag == CLJC_EMPTY) return mk_int(0);
    /* a deftype that implements Counted/count (e.g. a matrix) counts itself */
    { Cljc *out; if (dispatch_deftype_method(env, argv[0], "count", NULL, 0, &out)) return out; }
    /* read the tag as an int — keeping no Cljc* head copy. A live local holding
     * argv[0] would conservatively pin the whole realized chain through the walk
     * below (count O(n) live); the int dispatch leaves nothing for the scan. */
    int tag = argv[0]->tag;
    if (tag == CLJC_LIST || tag == CLJC_LAZY) {
        /* walk advancing the root — don't materialize the whole seq (to_seq
         * would also make count O(n) live). */
        int64_t n = 0;
        for (Cljc *s = seq1_slot(&argv[0]); s != NIL; s = seq1_slot(&argv[0])) {
            n++;
            argv[0] = s->as.cons.tail;
        }
        return mk_int(n);
    }
    if (tag == CLJC_VECTOR || tag == CLJC_TVEC)
        return mk_int((int64_t)vec_len(argv[0]));
    if (tag == CLJC_MAP || tag == CLJC_SET)
        return mk_int((int64_t)argv[0]->as.map.count);
    if (tag == CLJC_SORTED)
        return mk_int((int64_t)sorted_count(argv[0]));
    if (tag == CLJC_STRING) return mk_int((int64_t)strlen(argv[0]->as.str));
    cljc_error("count: %s is not countable", val_type_name(argv[0]));
    return NIL;
}

static Cljc *prim_nth(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *coll = argv[0];
    int64_t n = as_int(argv[1], "nth");
    int64_t orig_idx = n;            /* n is consumed by the seq loops below */
    Cljc *not_found = nargs > 2
        ? argv[2] : NULL;
    if (coll == NIL) return not_found ? not_found : NIL;  /* (nth nil i) => nil */
    /* a deftype with Indexed/nth uses it; one that is only Sequential (a matrix
     * via its rows) falls back to seq-based nth, like Clojure. */
    if (coll && coll->tag == CLJC_MAP) {
        Cljc *out;
        if (dispatch_deftype_method(env, coll, "nth", argv + 1, nargs - 1, &out)) return out;
        if (dispatch_deftype_method(env, coll, "seq", NULL, 0, &out)) {
            for (Cljc *l = to_seq(out); l != NIL; l = to_seq(l->as.cons.tail))
                if (n-- == 0) return l->as.cons.head;
            if (not_found) return not_found;
            cljc_error("nth: index %lld out of bounds", (long long)orig_idx);
        }
    }
    if (coll && (coll->tag == CLJC_VECTOR || coll->tag == CLJC_TVEC)) {
        if (n >= 0 && (size_t)n < vec_len(coll)) return vec_nth(coll, (size_t)n);
        if (not_found) return not_found;
        cljc_error("nth: index %lld out of bounds for length %zu", (long long)orig_idx, vec_len(coll));
    } else if (coll && coll->tag == CLJC_STRING) {
        if (n >= 0 && (size_t)n < strlen(coll->as.str))
            return mk_char((unsigned char)coll->as.str[n]);   /* (nth "abc" 1) => \b */
        if (not_found) return not_found;
        cljc_error("nth: index %lld out of bounds for length %zu", (long long)orig_idx, strlen(coll->as.str));
    } else if (coll && (coll->tag == CLJC_LIST || coll->tag == CLJC_LAZY)) {
        /* advance argv[0] so a deep index into a lazy seq stays O(1) live */
        for (Cljc *l = seq1_slot(&argv[0]); l && l->tag == CLJC_LIST;
             argv[0] = l->as.cons.tail, l = seq1_slot(&argv[0]))
            if (n-- == 0) return l->as.cons.head;
    }
    if (not_found) return not_found;
    cljc_error("nth: index %lld out of bounds", (long long)orig_idx);
    return NIL;
}

static Cljc *prim_conj(CljcEnv *env, Cljc **argv, int nargs) {
    if (nargs == 0) return mk_empty_vec();   /* (conj) => [] */
    Cljc *r = argv[0];  /* nil works: conj onto nil yields a list */
    for (int i = 1; i < nargs; i++) {
        Cljc *x = argv[i];
        if (r == NIL || r->tag == CLJC_LIST || r->tag == CLJC_LAZY ||
            r->tag == CLJC_EMPTY) {
            Cljc *prev = r;                         /* lazy: cons keeps it lazy */
            r = mk_cons(x, r->tag == CLJC_EMPTY ? NIL : r);  /* () conjs to (x) */
            if (prev != NIL && prev->tag == CLJC_LIST &&
                !is_arities_meta(prev->meta))   /* internal marker stays */
                r->meta = prev->meta;
        } else if (r->tag == CLJC_VECTOR) {
            Cljc *prev = r;
            r = vec_conj1(r, x);                    /* vectors grow at the back */
            if (prev->meta) r->meta = prev->meta;   /* queue tag etc. survive */
        } else if (r->tag == CLJC_SET) {
            r = set_conj(r, x);
        } else if (r->tag == CLJC_SORTED) {
            if (!r->as.sorted.is_map) r = sorted_put(env, r, x, x);
            else if (x == NIL) { /* no-op */ }
            else if (x->tag == CLJC_VECTOR && vec_len(x) == 2)
                r = sorted_put(env, r, vec_nth(x, 0), vec_nth(x, 1));
            else if (x->tag == CLJC_SORTED || x->tag == CLJC_MAP) {
                for (Cljc *e = to_seq(x); e != NIL; e = e->as.cons.tail) {
                    Cljc *kv = e->as.cons.head;   /* [k v] (sorted) or (k . v) (map) */
                    Cljc *k2 = kv->tag == CLJC_VECTOR ? vec_nth(kv, 0) : kv->as.cons.head;
                    Cljc *v2 = kv->tag == CLJC_VECTOR ? vec_nth(kv, 1) : kv->as.cons.tail;
                    r = sorted_put(env, r, k2, v2);
                }
            } else cljc_error("conj on sorted map: expected a [k v] entry or a map");
        } else if (r->tag == CLJC_MAP) {
            /* (conj m [k v]) and (conj m {k v ...}) — Clojure semantics; nil is
             * a no-op, and a 2-element seq is accepted like a vector entry. */
            if (x == NIL) { /* no-op */ }
            else if (x->tag == CLJC_VECTOR && vec_len(x) == 2) {
                r = map_assoc(r, vec_nth(x, 0), vec_nth(x, 1));
            } else if (x->tag == CLJC_MAP) {
                for (Cljc *e = map_entry_list(x); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
                    r = map_assoc(r, e->as.cons.head->as.cons.head,
                                  e->as.cons.head->as.cons.tail);
            } else if (x->tag == CLJC_LIST || x->tag == CLJC_LAZY) {
                Cljc *s = to_seq(x);
                if (s != NIL && s->as.cons.tail != NIL)
                    r = map_assoc(r, s->as.cons.head, s->as.cons.tail->as.cons.head);
                else cljc_error("conj on map: expected a [k v] entry or a map");
            } else cljc_error("conj on map: expected a [k v] entry or a map");
        } else cljc_error("conj: can't add to %s", val_type_name(r));
    }
    return r;
}

static Cljc *prim_vector(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    size_t n = (size_t)nargs;
    Cljc **items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
    size_t i = 0;
    for (int ai_ = 0; ai_ < nargs; ai_++)
        items[i++] = argv[ai_];
    Cljc *v = mk_vector(items, n);
    free(items);
    return v;
}


static Cljc *prim_apply(CljcEnv *env, Cljc **argv, int nargs) {
    /* (apply f a b [c d]) => (f a b c d) — last arg is spliced. */
    Cljc *fn = argv[0];
    size_t base = vsp;
    bool overflow = false;
    for (int i = 1; i < nargs - 1; i++) {
        if (vsp >= vstack_cap) { overflow = true; break; }
        vstack[vsp++] = argv[i];
    }
    /* Reserve headroom: seq1 below realizes the next lazy cell, whose thunk
     * needs operand space — if we fill the vstack to the brim, that realization
     * overflows. Bail to the heap path (which realizes from a clean vstack)
     * well before the cap. */
    size_t splice_lim = vstack_cap > 65536 ? vstack_cap - 65536 : vstack_cap;
    if (!overflow && nargs > 1)  /* splice the final collection — any seqable */
        for (Cljc *s = seq1(argv[nargs - 1]); s != NIL; s = seq1(s->as.cons.tail)) {
            if (vsp >= splice_lim) { overflow = true; break; }
            vstack[vsp++] = s->as.cons.head;
        }
    if (!overflow) {
        Cljc *r = apply(env, fn, &vstack[base], (int)(vsp - base));
        vsp = base;
        return r;
    }
    /* Huge apply ((apply str/concat/+ million-element-seq)): the args don't fit
     * on the value stack. Build argv on the heap instead. The spliced elements
     * stay GC-reachable through the seq, which is still on the vstack at
     * argv[nargs-1], so no extra rooting is needed. */
    vsp = base;
    size_t total = (nargs > 2 ? (size_t)(nargs - 2) : 0);
    for (Cljc *s = seq1(argv[nargs - 1]); s != NIL; s = seq1(s->as.cons.tail)) total++;
    Cljc **big = xmalloc(sizeof(Cljc *) * (total ? total : 1));
    size_t k = 0;
    for (int i = 1; i < nargs - 1; i++) big[k++] = argv[i];
    for (Cljc *s = seq1(argv[nargs - 1]); s != NIL; s = seq1(s->as.cons.tail))
        big[k++] = s->as.cons.head;
    Cljc *r = apply(env, fn, big, (int)total);
    free(big);
    return r;
}

#define TYPE_PRED(NAME, EXPR) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; Cljc *v = argv[0]; (void)v; \
        return mk_bool(EXPR); \
    }

TYPE_PRED(nil_p,     v == NIL)
TYPE_PRED(map_p,     v != NIL && (v->tag == CLJC_MAP || (v->tag == CLJC_SORTED && v->as.sorted.is_map)))
TYPE_PRED(set_p,     v != NIL && (v->tag == CLJC_SET || (v->tag == CLJC_SORTED && !v->as.sorted.is_map)))
TYPE_PRED(sorted_p,  v != NIL && v->tag == CLJC_SORTED)
TYPE_PRED(list_p,    v != NIL && (v->tag == CLJC_LIST || v->tag == CLJC_EMPTY))
TYPE_PRED(vector_p,  v != NIL && v->tag == CLJC_VECTOR)
TYPE_PRED(number_p,  v != NIL && (v->tag == CLJC_INT || v->tag == CLJC_DOUBLE || v->tag == CLJC_BIGINT || v->tag == CLJC_RATIO))
TYPE_PRED(int_p,     v != NIL && v->tag == CLJC_INT)
TYPE_PRED(double_p,  v != NIL && v->tag == CLJC_DOUBLE)
TYPE_PRED(string_p,  v != NIL && v->tag == CLJC_STRING)
TYPE_PRED(char_p,    v != NIL && v->tag == CLJC_CHAR)
TYPE_PRED(keyword_p, v != NIL && v->tag == CLJC_KEYWORD)
TYPE_PRED(symbol_p,  v != NIL && v->tag == CLJC_SYMBOL)
TYPE_PRED(fn_p,      v != NIL && (v->tag == CLJC_FN || v->tag == CLJC_NATIVE))
TYPE_PRED(zero_p,    as_num(v) == 0)
TYPE_PRED(pos_p,     as_num(v) > 0)
TYPE_PRED(neg_p,     as_num(v) < 0)

static Cljc *prim_empty_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL || v->tag == CLJC_EMPTY) return TRUE;
    if (v->tag == CLJC_LIST) return FALSE;  /* a cons is never empty */
    if (v->tag == CLJC_LAZY) return mk_bool(seq1(v) == NIL);
    if (v->tag == CLJC_VECTOR) return mk_bool(vec_len(v) == 0);
    if (v->tag == CLJC_MAP || v->tag == CLJC_SET)
        return mk_bool(v->as.map.count == 0);
    if (v->tag == CLJC_STRING) return mk_bool(v->as.str[0] == '\0');
    cljc_error("empty?: %s is not a collection", val_type_name(v));
    return NIL;
}

static Cljc *prim_inc(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v->tag == CLJC_DOUBLE) return mk_double(v->as.d + 1);
    return mk_int(as_int(v, "inc") + 1);
}

static Cljc *prim_dec(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v->tag == CLJC_DOUBLE) return mk_double(v->as.d - 1);
    return mk_int(as_int(v, "dec") - 1);
}

static Cljc *prim_mod(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (argv[0]->tag == CLJC_BIGINT || argv[1]->tag == CLJC_BIGINT) {
        Cljc *r; big_divmod(argv[0], argv[1], NULL, &r);   /* mod follows divisor sign */
        if (!big_is_zero(r) && (num_cmp(r, mk_int(0)) < 0) != (num_cmp(argv[1], mk_int(0)) < 0))
            r = big_addsub(r, argv[1], 0);
        return r;
    }
    int64_t a = as_int(argv[0], "mod");
    int64_t b = as_int(argv[1], "mod");
    if (b == 0) cljc_error("Divide by zero");
    int64_t m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;  /* Clojure mod follows divisor's sign */
    return mk_int(m);
}

/* ── Map primitives (HAMT engine — see the HAMT section above) ── */

static Cljc *prim_hash_map(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs % 2 != 0) cljc_error("hash-map needs an even number of arguments");
    Cljc *m = mk_map();
    for (int i = 0; i < nargs; i += 2)
        m = map_assoc(m, argv[i], argv[i + 1]);
    return m;
}

static Cljc *prim_get(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *coll = argv[0];
    Cljc *k = argv[1];
    Cljc *dflt = nargs > 2
        ? argv[2] : NIL;
    if (coll != NIL && coll->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(coll, k, &out)) return out;
        /* not a field key — a deftype implementing ILookup/valAt (e.g. a matrix
         * indexed by row) gets the lookup; plain maps/records just fall through. */
        if (dispatch_deftype_method(env, coll, "valAt", argv + 1, nargs - 1, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_SET) {
        Cljc *out;
        if (set_contains(coll, k, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_SORTED) {
        Cljc *out;
        if (sorted_get(env, coll, k, &out)) return out;
    } else if (coll != NIL && (coll->tag == CLJC_VECTOR || coll->tag == CLJC_TVEC)
               && k->tag == CLJC_INT) {
        if (k->as.i >= 0 && (size_t)k->as.i < vec_len(coll))
            return vec_nth(coll, (size_t)k->as.i);
    } else if (coll != NIL && coll->tag == CLJC_STRING && k->tag == CLJC_INT) {
        size_t len = strlen(coll->as.str);   /* (get s i) => char */
        if (k->as.i >= 0 && (size_t)k->as.i < len)
            return mk_char((unsigned char)coll->as.str[k->as.i]);
    }
    return dflt;
}

static Cljc *prim_assoc(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *coll = argv[0];
    if (coll == NIL) coll = mk_map();
    Cljc *r = coll;
    if ((nargs - 1) % 2 != 0) cljc_error("assoc needs key-value pairs");
    for (int i = 1; i < nargs; i += 2) {
        Cljc *k = argv[i], *v = argv[i + 1];
        if (r->tag == CLJC_MAP) {
            /* a deftype implementing Associative/assoc (a structure/matrix indexed
             * by position) updates through its own assoc, not the field map. This
             * is what lets reverse-mode AD's (assoc-in x [i] perturbation) work. */
            Cljc *out, *kv[2] = { k, v };
            if (dispatch_deftype_method(env, r, "assoc", kv, 2, &out)) { r = out; continue; }
            r = map_assoc(r, k, v);
        }
        else if (r->tag == CLJC_SORTED && r->as.sorted.is_map) {
            r = sorted_put(env, r, k, v);
        }
        else if (r->tag == CLJC_VECTOR) {
            if (k->tag != CLJC_INT)
                cljc_error("assoc: vector index must be an integer, got %s", val_type_name(k));
            if (k->as.i < 0)
                cljc_error("assoc: index %lld out of bounds for length %zu", (long long)k->as.i, vec_len(r));
            r = vec_assoc_idx(r, (size_t)k->as.i, v);  /* assoc at len appends */
        } else cljc_error("assoc: %s is not associative", val_type_name(r));
    }
    return r;
}

static Cljc *prim_dissoc(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag == CLJC_SORTED && m->as.sorted.is_map) {
        for (int i = 1; i < nargs; i++) m = sorted_remove(env, m, argv[i]);
        return m;
    }
    if (m->tag != CLJC_MAP) cljc_error("dissoc: expected a map, got %s", val_type_name(m));
    for (int i = 1; i < nargs; i++)
        m = map_dissoc_one(m, argv[i]);
    return m;
}

/* keys (kv=0) / vals (kv=1) for a hash or sorted map. */
static Cljc *map_kv_list(Cljc *m, int kv) {
    Cljc *out = NIL, **t = &out;
    if (m->tag == CLJC_SORTED) {
        for (Cljc *e = sorted_entry_list(m); e != NIL; e = e->as.cons.tail) {
            *t = mk_cons(vec_nth(e->as.cons.head, kv), NIL);
            t = &(*t)->as.cons.tail;
        }
    } else {
        for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            *t = mk_cons(kv ? e->as.cons.head->as.cons.tail : e->as.cons.head->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
    }
    return out;
}

static Cljc *prim_keys(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP && m->tag != CLJC_SORTED) cljc_error("keys: expected a map, got %s", val_type_name(m));
    return map_kv_list(m, 0);
}

static Cljc *prim_vals(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP && m->tag != CLJC_SORTED) cljc_error("vals: expected a map, got %s", val_type_name(m));
    return map_kv_list(m, 1);
}

static Cljc *prim_contains_p(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *coll = argv[0];
    Cljc *k = argv[1];
    if (coll == NIL) return FALSE;
    if (coll->tag == CLJC_MAP) return mk_bool(map_find(coll, k, NULL));
    if (coll->tag == CLJC_SET) return mk_bool(set_contains(coll, k, NULL));
    if (coll->tag == CLJC_SORTED) return mk_bool(sorted_contains(env, coll, k));
    if (coll->tag == CLJC_VECTOR)  /* contains? checks INDEX presence on vectors */
        return mk_bool(k->tag == CLJC_INT && k->as.i >= 0 && (size_t)k->as.i < vec_len(coll));
    cljc_error("contains?: %s is not associative", val_type_name(coll));
    return NIL;
}

static Cljc *prim_merge(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *r = NIL;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        Cljc *m = argv[ai_];
        if (m == NIL) continue;
        if (m->tag != CLJC_MAP && m->tag != CLJC_SORTED) cljc_error("merge: %s is not a map", val_type_name(m));
        if (r == NIL) { r = m; continue; }
        /* preserve the first map's type (a sorted map merges into a sorted map) */
        if (m->tag == CLJC_SORTED) {
            for (Cljc *e = sorted_entry_list(m); e != NIL; e = e->as.cons.tail) {
                Cljc *ca[2] = { r, e->as.cons.head };   /* head is a [k v] entry */
                r = prim_conj(env, ca, 2);
            }
        } else {
            for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                Cljc *pair[2] = { e->as.cons.head->as.cons.head, e->as.cons.head->as.cons.tail };
                Cljc *ca[2] = { r, mk_vector(pair, 2) };
                r = prim_conj(env, ca, 2);
            }
        }
    }
    return r;
}

static Cljc *prim_rem(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (argv[0]->tag == CLJC_BIGINT || argv[1]->tag == CLJC_BIGINT) {
        Cljc *r; big_divmod(argv[0], argv[1], NULL, &r); return r;
    }
    int64_t a = as_int(argv[0], "rem");
    int64_t b = as_int(argv[1], "rem");
    if (b == 0) cljc_error("Divide by zero");
    return mk_int(a % b);
}

static Cljc *prim_list(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs == 0) return EMPTY;   /* (list) => () */
    Cljc *out = NIL;
    for (int i = nargs - 1; i >= 0; i--) out = mk_cons(argv[i], out);
    return out;
}

static Cljc *prim_first(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = seq1_slot(&argv[0]);  /* advances the root: skipping into a long
                                       lazy chain (e.g. filter) stays O(1) live */
    return s == NIL ? NIL : s->as.cons.head;
}

static Cljc *prim_rest(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = seq1(argv[0]);
    if (s == NIL) return EMPTY;                 /* (rest ()) / (rest nil) => () */
    Cljc *t = s->as.cons.tail;                  /* tail may be lazy */
    return (t == NIL) ? EMPTY : t;              /* a finished list rests to () */
}

static Cljc *prim_second(CljcEnv *env, Cljc **argv, int nargs) {
    /* (first (rest x)) — go through first so a LAZY rest (e.g. a partition
     * group) is realized rather than reading an unforced cons head. */
    Cljc *r = prim_rest(env, argv, nargs);
    Cljc *fargv[1] = { r };
    return prim_first(env, fargv, 1);
}

static Cljc *prim_cons(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *h = argv[0];
    Cljc *t = argv[1];
    /* Lazy tails stay lazy — the backbone of lazy-seq pipelines. */
    if (!(t != NIL && (t->tag == CLJC_LAZY || t->tag == CLJC_LIST)))
        t = to_seq(t);  /* (cons 1 [2 3]) => (1 2 3); (cons 1 2) errors */
    return mk_cons(h, t);
}

/* ── Seq library ── */

/* Force a lazy cell once; thunk dropped after so its closure can be GC'd.
 * The thunk often returns ANOTHER lazy seq (e.g. (lazy-seq (map f xs)) yields
 * the map's own lazy); force through that chain to a realized cons/NIL before
 * caching, the way Clojure's LazySeq.seq() flattens nested LazySeqs. Crucially
 * `done` stays false during this, so a self-referential seq that re-enters its
 * own force (chunk-map* walking the tail) still sees this cell as unrealized
 * and stops there instead of looping forever. */
static Cljc *lazy_force(Cljc *l) {
    if (!l->as.lazy.done) {
        Cljc *r = apply(gc_root_envs[0], l->as.lazy.thunk, NULL, 0);
        while (r != NIL && r != NULL && r->tag == CLJC_LAZY) r = lazy_force(r);
        l->as.lazy.cached = r;
        l->as.lazy.done = true;
        l->as.lazy.thunk = NIL;
    }
    return l->as.lazy.cached;
}

/* Single-step seq: force AT MOST the head cell. Returns NIL or a cons whose
 * tail may itself be lazy. This is what keeps pipelines lazy. */
static Cljc *seq1(Cljc *v) {
    for (;;) {
        if (v == NULL || v == NIL || v->tag == CLJC_EMPTY) return NIL;
        if (v->tag == CLJC_LIST) return v;
        if (v->tag == CLJC_LAZY) { v = lazy_force(v); continue; }
        return to_seq(v);  /* finite collections materialize */
    }
}

/* Like seq1, but advances the caller's GC root slot as it forces, so a long
 * lazy chain doesn't stay pinned by its head. *slot must be a live root (a
 * vstack arg slot): a single-pass consumer (first/count/reduce/nth/…) walks
 * with seq1_slot(&argv[i]) instead of seq1(argv[i]) and the realized prefix
 * is collected behind it — O(1) live set instead of O(n). */
static Cljc *seq1_slot(Cljc **slot) {
    Cljc *v = *slot;
    for (;;) {
        if (v == NULL || v == NIL || v->tag == CLJC_EMPTY) return NIL;
        if (v->tag == CLJC_LIST) { *slot = v; return v; }
        if (v->tag == CLJC_LAZY) {
            *slot = v;             /* keep the cell being forced rooted... */
            v = lazy_force(v);     /* ...then advance: the old head drops */
            continue;
        }
        *slot = v; return to_seq(v);
    }
}

/* Normalize any seqable to a FULLY REALIZED plain list (eager consumers).
 * Plain lists pass through untouched unless a lazy tail hides inside. */
static Cljc *to_seq(Cljc *v) {
    if (v == NIL || v->tag == CLJC_EMPTY) return NIL;
    if (v->tag == CLJC_LAZY || v->tag == CLJC_LIST) {
        if (v->tag == CLJC_LIST) {  /* fast path: no lazy tails => as-is */
            Cljc *l = v;
            while (l->tag == CLJC_LIST) l = l->as.cons.tail;
            if (l == NIL) return v;
        }
        Cljc *out = NIL, **t = &out;
        for (Cljc *s = seq1(v); s != NIL; s = seq1(s->as.cons.tail)) {
            *t = mk_cons(s->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    if (v->tag == CLJC_VECTOR || v->tag == CLJC_TVEC) {  /* arrays seq too */
        Cljc *out = NIL, **t = &out;
        for (size_t i = 0; i < vec_len(v); i++) {
            *t = mk_cons(vec_nth(v, i), NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    if (v->tag == CLJC_STRING) {
        /* Strings seq into chars (one per byte, matching byte-oriented count). */
        Cljc *out = NIL, **t = &out;
        for (const char *c = v->as.str; *c; c++) {
            *t = mk_cons(mk_char((unsigned char)*c), NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    if (v->tag == CLJC_SORTED) return sorted_entry_list(v);
    if (v->tag == CLJC_SET) return set_element_list(v);
    if (v->tag == CLJC_MAP) {
        /* A deftype implementing Seqable (its own `seq` method, e.g. instaparse's
         * AutoFlattenSeq): use it instead of seqing the field map. */
        Cljc *sq;
        if (dispatch_deftype_method(gc_root_envs[0], v, "seq", NULL, 0, &sq))
            /* seq1, not to_seq: the seq method may return an INFINITE lazy seq
             * (e.g. an Emmy power series' coefficients); keep it lazy rather
             * than realizing the whole thing here. */
            return (sq == NIL || sq == v) ? NIL : seq1(sq);
        /* Maps seq into [k v] entry vectors. */
        Cljc *out = NIL, **t = &out;
        for (Cljc *e = map_entry_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *entry_items[2] = { e->as.cons.head->as.cons.head, e->as.cons.head->as.cons.tail };
            *t = mk_cons(mk_vector(entry_items, 2), NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    cljc_error("don't know how to make a seq from %s", val_type_name(v));
    return NIL;
}

static Cljc *prim_map(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *f = argv[0];
    Cljc *seq = to_seq(argv[1]);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *t = mk_cons(apply(env, f, &l->as.cons.head, 1), NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_filter(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *f = argv[0];
    Cljc *seq = to_seq(argv[1]);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        if (is_truthy(apply(env, f, &l->as.cons.head, 1))) {
            *t = mk_cons(l->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
    }
    return out;
}

static Cljc *prim_reduce(CljcEnv *env, Cljc **argv, int nargs) {
    /* (reduce f coll) or (reduce f init coll) */
    Cljc *f = argv[0];
    Cljc *acc;
    Cljc **slot;                           /* the input-seq arg: advanced as we
                                              walk so the head isn't pinned */
    if (nargs < 3) {
        slot = &argv[1];
        Cljc *s = seq1_slot(slot);         /* lazy cursor: no realization */
        if (s == NIL) return apply(env, f, NULL, 0);  /* (reduce f []) => (f) */
        acc = s->as.cons.head;
        *slot = s->as.cons.tail;
    } else {
        acc = argv[1];
        slot = &argv[2];
    }
    for (Cljc *l = seq1_slot(slot); l != NIL; *slot = l->as.cons.tail, l = seq1_slot(slot)) {
        Cljc *two[2] = {acc, l->as.cons.head};
        acc = apply(env, f, two, 2);
        /* (reduced x) terminates early — works on infinite seqs now */
        if (acc != NIL && acc->tag == CLJC_LIST &&
            acc->as.cons.head->tag == CLJC_SYMBOL &&
            acc->as.cons.head->as.sym == intern("**reduced**", 11))
            return acc->as.cons.tail->as.cons.head;
    }
    return acc;
}

static Cljc *prim_range(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    size_t n = (size_t)nargs;
    bool flt = false;                       /* float range if any arg is a double */
    for (size_t i = 0; i < n && i < 3; i++)
        if (argv[i] != NIL && argv[i]->tag == CLJC_DOUBLE) flt = true;
    if (flt) {
        double start = 0, end = 0, step = 1;
        if (n == 1) end = as_num(argv[0]);
        else { start = as_num(argv[0]); end = as_num(argv[1]); if (n >= 3) step = as_num(argv[2]); }
        if (step == 0) cljc_error("range: step must be nonzero");
        Cljc *out = NIL, **t = &out;
        for (double i = start; step > 0 ? i < end : i > end; i += step) {
            *t = mk_cons(mk_double(i), NIL); t = &(*t)->as.cons.tail;
        }
        return out;
    }
    int64_t start = 0, end = 0, step = 1;
    if (n == 1) end = as_int(argv[0], "range");
    else if (n >= 2) {
        start = as_int(argv[0], "range");
        end = as_int(argv[1], "range");
        if (n >= 3) step = as_int(argv[2], "range");
    }
    if (step == 0) cljc_error("range: step must be nonzero");
    Cljc *out = NIL, **t = &out;
    for (int64_t i = start; step > 0 ? i < end : i > end; i += step) {
        *t = mk_cons(mk_int(i), NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

/* (cljc/range-chunk* start end step n) => [strict-list-of-up-to-n next-start].
 * end == nil means an unbounded range. Backs the lazy, chunked `range` so that
 * streaming consumers keep only ~32 elements live (the eager builder above held
 * the whole range, inflating GC and peak memory). */
static Cljc *prim_range_chunk(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *sv = argv[0], *ev = argv[1], *stv = argv[2];
    int64_t cnt = as_int(argv[3], "range");
    bool inf = (ev == NIL);
    bool flt = sv->tag == CLJC_DOUBLE || stv->tag == CLJC_DOUBLE ||
               (!inf && ev->tag == CLJC_DOUBLE);
    Cljc *out = NIL, **t = &out, *next;
    if (flt) {
        double i = as_num(sv), end = inf ? 0 : as_num(ev), step = as_num(stv);
        if (step == 0) cljc_error("range: step must be nonzero");
        for (; cnt > 0 && (inf || (step > 0 ? i < end : i > end)); cnt--, i += step) {
            *t = mk_cons(mk_double(i), NIL); t = &(*t)->as.cons.tail;
        }
        next = mk_double(i);
    } else {
        int64_t i = as_int(sv, "range"), end = inf ? 0 : as_int(ev, "range"), step = as_int(stv, "range");
        if (step == 0) cljc_error("range: step must be nonzero");
        for (; cnt > 0 && (inf || (step > 0 ? i < end : i > end)); cnt--, i += step) {
            *t = mk_cons(mk_int(i), NIL); t = &(*t)->as.cons.tail;
        }
        next = mk_int(i);
    }
    Cljc *pair[2] = {out, next};
    return mk_vector(pair, 2);
}

static Cljc *prim_take(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t n = as_int(argv[0], "take");
    Cljc *seq = to_seq(argv[1]);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; n-- > 0 && l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *t = mk_cons(l->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_drop(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t n = as_int(argv[0], "drop");
    Cljc *seq = to_seq(argv[1]);
    while (n-- > 0 && seq != NIL && seq->tag == CLJC_LIST) seq = seq->as.cons.tail;
    return seq;
}

static Cljc *prim_reverse(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *out = NIL;
    bool any = false;
    for (Cljc *l = to_seq(argv[0]); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        { out = mk_cons(l->as.cons.head, out); any = true; }
    return any ? out : EMPTY;   /* (reverse []) => () */
}

static Cljc *prim_last(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *r = NIL;
    for (Cljc *l = to_seq(argv[0]); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        r = l->as.cons.head;
    return r;
}

static Cljc *prim_concat(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *out = NIL, **t = &out;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        for (Cljc *l = to_seq(argv[ai_]); l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
            *t = mk_cons(l->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
    }
    return out;
}

static Cljc *prim_gensym(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    static int counter = 0;
    char buf[64];
    const char *prefix = "G__";
    if (nargs > 0 && argv[0]->tag == CLJC_STRING)
        prefix = argv[0]->as.str;
    snprintf(buf, sizeof buf, "%s%d", prefix, counter++);
    return mk_sym(intern(buf, strlen(buf)));
}

static Cljc *prim_seq(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = seq1(argv[0]);
    return s == NIL ? NIL : s;  /* (seq []) => nil, matching Clojure */
}

static Cljc *prim_seq_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    return mk_bool(v != NIL && (v->tag == CLJC_LIST || v->tag == CLJC_LAZY ||
                                v->tag == CLJC_EMPTY));
}

/* ───── Regex engine ─────────────────────────────────────────────────── */

/* Tiny backtracking matcher. Supported: literals . ^ $ [abc] [a-z] [^...]
 * \d \D \w \W \s \S \n \t \r, ( ) capture groups, (?: ) non-capturing,
 * (?=X) (?!X) lookahead, | alternation, * + ? quantifiers with lazy
 * variants (*? +? ??), X{n} X{n,} X{n,m} bounded repeats (n,m ≤ 64,
 * desugared to copies).
 * Not supported: lookbehind. Patterns are plain
 * strings; the #"..." reader literal passes backslashes through raw.
 * Parse-time desugaring: X+ => X X* (atom re-parsed), X? => ALT(X, empty);
 * only star is a real loop, guarded against empty-match cycles. */

enum { RX_CHAR, RX_ANY, RX_CLASS, RX_BOL, RX_EOL, RX_STAR, RX_LOOP,
       RX_ALT, RX_JOIN, RX_GS, RX_GE, RX_LA, RX_BACKREF, RX_WB };

typedef struct Rx Rx;
struct Rx {
    uint8_t type;
    bool lazy;
    bool neg;
    char ch;
    int group;
    uint8_t bits[32];          /* 256-bit set for classes */
    Rx *next, *child, *alt, *owner;
    const char *last;          /* star: last entry position (cycle guard) */
};

#define RX_MAX_NODES 1024
#define RX_MAX_GROUPS 32

typedef struct {
    const char *p;             /* parse cursor */
    Rx *pool;
    int npool;
    int ngroups;
} RxC;

typedef struct { Rx *h, *t; } RxChain;

static Rx *rx_node(RxC *c, int type) {
    if (c->npool >= RX_MAX_NODES) cljc_error("regex too complex");
    Rx *r = &c->pool[c->npool++];
    memset(r, 0, sizeof *r);
    r->type = (uint8_t)type;
    return r;
}

static void rx_bit(Rx *r, unsigned char ch) { r->bits[ch >> 3] |= (uint8_t)(1u << (ch & 7)); }
static bool rx_bit_test(const Rx *r, unsigned char ch) {
    bool in = (r->bits[ch >> 3] >> (ch & 7)) & 1;
    return r->neg ? !in : in;
}

static void rx_class_shorthand(Rx *r, char c) {
    switch (c) {
        case 'd': for (int i = '0'; i <= '9'; i++) rx_bit(r, (unsigned char)i); break;
        case 'w': for (int i = '0'; i <= '9'; i++) rx_bit(r, (unsigned char)i);
                  for (int i = 'a'; i <= 'z'; i++) rx_bit(r, (unsigned char)i);
                  for (int i = 'A'; i <= 'Z'; i++) rx_bit(r, (unsigned char)i);
                  rx_bit(r, '_'); break;
        case 's': rx_bit(r, ' '); rx_bit(r, '\t'); rx_bit(r, '\n');
                  rx_bit(r, '\r'); rx_bit(r, '\f'); rx_bit(r, '\v'); break;
        default: cljc_error("regex: unknown class \\%c", c);
    }
}

static RxChain rx_parse_alt(RxC *c);

/* One atom: a single char-matcher, class, group, or anchor. */
static RxChain rx_parse_atom(RxC *c) {
    RxChain ch = {NULL, NULL};
    char c0 = *c->p;
    if (c0 == '(') {
        c->p++;
        bool capture = true;
        if (c->p[0] == '?' && (c->p[1] == '=' || c->p[1] == '!')) {
            /* lookahead (?=X) / (?!X): zero-width assertion */
            bool neg = c->p[1] == '!';
            c->p += 2;
            RxChain inner = rx_parse_alt(c);
            if (*c->p != ')') cljc_error("regex: missing )");
            c->p++;
            Rx *la = rx_node(c, RX_LA);
            la->neg = neg;
            la->child = inner.h;   /* tail's next stays NULL: bare sub-match */
            ch.h = ch.t = la;
            return ch;
        }
        if (c->p[0] == '?' && c->p[1] == ':') { capture = false; c->p += 2; }
        int idx = 0;
        if (capture) {
            if (c->ngroups >= RX_MAX_GROUPS) cljc_error("regex: too many groups");
            idx = c->ngroups++;
        }
        RxChain inner = rx_parse_alt(c);
        if (*c->p != ')') cljc_error("regex: missing )");
        c->p++;
        if (!capture) return inner;
        Rx *gs = rx_node(c, RX_GS); gs->group = idx;
        Rx *ge = rx_node(c, RX_GE); ge->group = idx;
        gs->next = inner.h ? inner.h : ge;
        if (inner.t) inner.t->next = ge;
        ch.h = gs; ch.t = ge;
        return ch;
    }
    if (c0 == '[') {
        c->p++;
        Rx *r = rx_node(c, RX_CLASS);
        if (*c->p == '^') { r->neg = true; c->p++; }
        while (*c->p && *c->p != ']') {
            if (*c->p == '\\' && c->p[1]) {
                char e = c->p[1]; c->p += 2;
                if (e == 'd' || e == 'w' || e == 's') rx_class_shorthand(r, e);
                else if (e == 'n') rx_bit(r, '\n');
                else if (e == 't') rx_bit(r, '\t');
                else if (e == 'r') rx_bit(r, '\r');
                else rx_bit(r, (unsigned char)e);
                continue;
            }
            unsigned char lo = (unsigned char)*c->p++;
            if (*c->p == '-' && c->p[1] && c->p[1] != ']') {
                unsigned char hi = (unsigned char)c->p[1];
                c->p += 2;
                for (unsigned i = lo; i <= hi; i++) rx_bit(r, (unsigned char)i);
            } else rx_bit(r, lo);
        }
        if (*c->p != ']') cljc_error("regex: missing ]");
        c->p++;
        ch.h = ch.t = r;
        return ch;
    }
    if (c0 == '\\') {
        char e = c->p[1];
        if (!e) cljc_error("regex: trailing backslash");
        c->p += 2;
        Rx *r;
        if (e == 'd' || e == 'w' || e == 's') {
            r = rx_node(c, RX_CLASS); rx_class_shorthand(r, e);
        } else if (e == 'D' || e == 'W' || e == 'S') {
            r = rx_node(c, RX_CLASS); rx_class_shorthand(r, (char)tolower(e)); r->neg = true;
        } else if (e >= '1' && e <= '9') {        /* \1..\9 backreference */
            r = rx_node(c, RX_BACKREF); r->group = e - '0';
        } else if (e == 'b' || e == 'B') {        /* \b / \B word boundary */
            r = rx_node(c, RX_WB); r->neg = (e == 'B');
        } else {
            r = rx_node(c, RX_CHAR);
            r->ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        ch.h = ch.t = r;
        return ch;
    }
    c->p++;
    Rx *r;
    if (c0 == '.') r = rx_node(c, RX_ANY);
    else if (c0 == '^') r = rx_node(c, RX_BOL);
    else if (c0 == '$') r = rx_node(c, RX_EOL);
    else { r = rx_node(c, RX_CHAR); r->ch = c0; }
    ch.h = ch.t = r;
    return ch;
}

/* Wrap a chain in a star node (child loops back via RX_LOOP). */
static RxChain rx_star(RxC *c, RxChain atom, bool lazy) {
    Rx *s = rx_node(c, RX_STAR);
    s->lazy = lazy;
    Rx *loop = rx_node(c, RX_LOOP);
    loop->owner = s;
    s->child = atom.h ? atom.h : loop;
    if (atom.t) atom.t->next = loop;
    RxChain ch = {s, s};
    return ch;
}

static RxChain rx_parse_cat(RxC *c) {
    RxChain out = {NULL, NULL};
    while (*c->p && *c->p != '|' && *c->p != ')') {
        const char *atom_src = c->p;
        int groups_before = c->ngroups;          /* group # at the atom's start */
        RxChain atom = rx_parse_atom(c);
        int groups_after = c->ngroups;           /* ... and just after it */
        char q = *c->p;
        RxChain unit = atom;
        if (q == '{' && c->p[1] >= '0' && c->p[1] <= '9') {
            /* X{n} / X{n,} / X{n,m}: desugar by re-parsing the atom —
             * n required copies, then a star ({n,}) or m-n optionals. */
            c->p++;
            int lo = 0, hi = -1;             /* -1: exactly lo; -2: open */
            while (*c->p >= '0' && *c->p <= '9') lo = lo * 10 + (*c->p++ - '0');
            if (*c->p == ',') {
                c->p++;
                if (*c->p == '}') hi = -2;
                else {
                    hi = 0;
                    while (*c->p >= '0' && *c->p <= '9') hi = hi * 10 + (*c->p++ - '0');
                }
            }
            if (*c->p != '}') cljc_error("regex: bad {n,m} quantifier");
            c->p++;
            bool qlazy = false;
            if (*c->p == '?') { qlazy = true; c->p++; }   /* {n,m}? lazy */
            else if (*c->p == '+') c->p++;                /* {n,m}+ possessive → greedy */
            if (hi == -1) hi = lo;
            if (lo > 64 || (hi != -2 && (hi > 64 || hi < lo)))
                cljc_error("regex: {n,m} out of range");
            const char *after = c->p;
            RxChain seq = lo > 0 ? atom : (RxChain){NULL, NULL};
            for (int k = 1; k < lo; k++) {       /* required copies 2..n */
                c->p = atom_src;
                c->ngroups = groups_before;      /* copies REUSE the atom's group #s */
                RxChain copy = rx_parse_atom(c);
                c->ngroups = groups_after;       /* (last iteration wins, like Java) */
                if (!seq.h) seq = copy;
                else if (copy.h) { seq.t->next = copy.h; seq.t = copy.t; }
            }
            if (hi == -2) {                      /* {n,}: append X* */
                c->p = atom_src;
                c->ngroups = groups_before;
                RxChain copy = rx_parse_atom(c);
                c->ngroups = groups_after;
                RxChain star = rx_star(c, copy, qlazy);
                if (!seq.h) seq = star;
                else { seq.t->next = star.h; seq.t = star.t; }
            } else {
                for (int k = lo; k < hi; k++) {  /* optional copies */
                    c->p = atom_src;
                    c->ngroups = groups_before;
                    RxChain copy = rx_parse_atom(c);
                    c->ngroups = groups_after;
                    Rx *a = rx_node(c, RX_ALT);
                    Rx *j1 = rx_node(c, RX_JOIN); j1->owner = a;
                    Rx *j2 = rx_node(c, RX_JOIN); j2->owner = a;
                    if (copy.t) copy.t->next = j1;
                    Rx *xbranch = copy.h ? copy.h : j1;
                    a->child = qlazy ? j2 : xbranch;   /* lazy prefers the empty branch */
                    a->alt   = qlazy ? xbranch : j2;
                    if (!seq.h) { seq.h = seq.t = a; }
                    else { seq.t->next = a; seq.t = a; }
                }
            }
            c->p = after;
            if (!out.h) out = seq;
            else if (seq.h) { out.t->next = seq.h; out.t = seq.t; }
            continue;
        }
        if (q == '*' || q == '+' || q == '?') {
            c->p++;
            bool lazy = *c->p == '?';
            if (lazy) c->p++;
            else if (*c->p == '+') c->p++;   /* possessive (a++, a*+) → treat greedy */
            if (q == '*') {
                unit = rx_star(c, atom, lazy);
            } else if (q == '+') {
                /* X+ => X X*: re-parse the atom for the star's copy. */
                const char *save = c->p;
                c->p = atom_src;
                c->ngroups = groups_before;  /* copy REUSES the atom's group #s */
                RxChain atom2 = rx_parse_atom(c);
                c->ngroups = groups_after;
                c->p = save;
                RxChain star = rx_star(c, atom2, lazy);
                atom.t->next = star.h;
                unit.h = atom.h; unit.t = star.t;
            } else {  /* ? => ALT(X, empty); lazy ?? prefers empty */
                Rx *a = rx_node(c, RX_ALT);
                Rx *j1 = rx_node(c, RX_JOIN); j1->owner = a;
                Rx *j2 = rx_node(c, RX_JOIN); j2->owner = a;
                if (atom.t) atom.t->next = j1;
                Rx *xbranch = atom.h ? atom.h : j1;
                a->child = lazy ? j2 : xbranch;
                a->alt   = lazy ? xbranch : j2;
                unit.h = unit.t = a;
            }
        }
        if (!out.h) out = unit;
        else { out.t->next = unit.h; out.t = unit.t; }
    }
    return out;
}

static RxChain rx_parse_alt(RxC *c) {
    RxChain left = rx_parse_cat(c);
    if (*c->p != '|') return left;
    c->p++;
    RxChain right = rx_parse_alt(c);
    Rx *a = rx_node(c, RX_ALT);
    Rx *j1 = rx_node(c, RX_JOIN); j1->owner = a;
    Rx *j2 = rx_node(c, RX_JOIN); j2->owner = a;
    if (left.t) left.t->next = j1;
    if (right.t) right.t->next = j2;
    a->child = left.h ? left.h : j1;
    a->alt = right.h ? right.h : j2;
    RxChain ch = {a, a};
    return ch;
}

/* ── matcher ── */

static const char *rx_str_begin;
static bool rx_dotall;   /* (?s): . matches newline too. Set by rx_compile. */
static const char *rx_match_end;
static const char *rx_cap_s[RX_MAX_GROUPS], *rx_cap_e[RX_MAX_GROUPS];
static long rx_steps;          /* backtracking budget per match attempt */
#define RX_MAX_STEPS 2000000L

static bool rx_m(Rx *r, const char *s) {
    if (++rx_steps > RX_MAX_STEPS)
        cljc_error("regex: too much backtracking");
    if (!r) { rx_match_end = s; return true; }
    switch (r->type) {
        case RX_CHAR:  return *s == r->ch && rx_m(r->next, s + 1);
        case RX_ANY:   return *s && (rx_dotall || *s != '\n') && rx_m(r->next, s + 1);
        case RX_CLASS: return *s && rx_bit_test(r, (unsigned char)*s) && rx_m(r->next, s + 1);
        case RX_BOL:   return s == rx_str_begin && rx_m(r->next, s);
        case RX_EOL:   return *s == '\0' && rx_m(r->next, s);
        case RX_WB: {  /* \b boundary / \B non-boundary (\w = [A-Za-z0-9_]) */
            int wprev = (s != rx_str_begin) && (isalnum((unsigned char)s[-1]) || s[-1] == '_');
            int wcur  = (*s != '\0') && (isalnum((unsigned char)*s) || *s == '_');
            return ((wprev != wcur) != r->neg) && rx_m(r->next, s);
        }
        case RX_GS: {
            const char *save = rx_cap_s[r->group];
            rx_cap_s[r->group] = s;
            if (rx_m(r->next, s)) return true;
            rx_cap_s[r->group] = save;
            return false;
        }
        case RX_GE: {
            const char *save = rx_cap_e[r->group];
            rx_cap_e[r->group] = s;
            if (rx_m(r->next, s)) return true;
            rx_cap_e[r->group] = save;
            return false;
        }
        case RX_BACKREF: {
            /* match the exact text captured by group N (empty if it never matched) */
            const char *cs = rx_cap_s[r->group], *ce = rx_cap_e[r->group];
            if (!cs || !ce || ce < cs) return rx_m(r->next, s);
            size_t len = (size_t)(ce - cs);
            if (strncmp(s, cs, len) != 0) return false;
            return rx_m(r->next, s + len);
        }
        case RX_LA: {
            /* zero-width lookahead: sub-match here, consume nothing */
            const char *save_end = rx_match_end;
            bool m = rx_m(r->child, s);
            rx_match_end = save_end;
            if (m == r->neg) return false;
            return rx_m(r->next, s);
        }
        case RX_ALT:  return rx_m(r->child, s) || rx_m(r->alt, s);
        case RX_JOIN: return rx_m(r->owner->next, s);
        case RX_LOOP: return rx_m(r->owner, s);
        case RX_STAR: {
            if (r->lazy && rx_m(r->next, s)) return true;
            if (s != r->last) {           /* empty-iteration cycle guard */
                const char *save = r->last;
                r->last = s;
                if (rx_m(r->child, s)) { r->last = save; return true; }
                r->last = save;
            }
            if (!r->lazy) return rx_m(r->next, s);
            return false;
        }
    }
    return false;
}

/* Compile into a malloc'd pool; caller frees pool. */
/* Handle inline flag groups (?x)(?s)(?i)(?m): extract the flags, drop the
 * (?flags) tokens, and in extended mode (x) strip unescaped whitespace and
 * #-to-end-of-line comments. Char classes and escaped pairs are copied raw.
 * Returns a malloc'd cleaned pattern; *dotall reports the (?s) flag. */
static char *rx_preprocess(const char *p, bool *dotall) {
    *dotall = false;
    char *out = xmalloc(strlen(p) + 1);
    size_t o = 0;
    bool extended = false, in_class = false;
    for (size_t i = 0; p[i]; ) {
        char ch = p[i];
        if (ch == '\\' && p[i + 1]) { out[o++] = p[i++]; out[o++] = p[i++]; continue; }
        if (in_class) { if (ch == ']') in_class = false; out[o++] = p[i++]; continue; }
        if (ch == '[') { in_class = true; out[o++] = p[i++]; continue; }
        if (ch == '(' && p[i + 1] == '?') {           /* maybe a flag-only group */
            size_t j = i + 2;
            while (p[j] && strchr("ixsmu", p[j])) j++;
            if (p[j] == ')' && j > i + 2) {           /* (?flags) — apply and drop */
                for (size_t k = i + 2; k < j; k++) {
                    if (p[k] == 's') *dotall = true;
                    if (p[k] == 'x') extended = true;
                }
                i = j + 1;
                continue;
            }
        }
        if (extended) {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { i++; continue; }
            if (ch == '#') { while (p[i] && p[i] != '\n') i++; continue; }
        }
        out[o++] = p[i++];
    }
    out[o] = '\0';
    return out;
}

static Rx *rx_compile(const char *pattern, Rx **pool_out, int *ngroups_out) {
    RxC c;
    bool dotall;
    char *clean = rx_preprocess(pattern, &dotall);
    rx_dotall = dotall;
    c.p = clean;
    c.pool = xmalloc(sizeof(Rx) * RX_MAX_NODES);
    c.npool = 0;
    c.ngroups = 1;  /* group 0 = whole match */
    RxChain top = rx_parse_alt(&c);
    if (*c.p) { free(c.pool); free(clean); cljc_error("regex: unexpected )"); }
    free(clean);  /* nodes copied what they need; chars no longer referenced */
    *pool_out = c.pool;
    *ngroups_out = c.ngroups;
    return top.h;  /* may be NULL: empty pattern matches everywhere */
}

/* Compile with an end-of-string anchor INSIDE the program, so a failed
 * full match backtracks into alternations/quantifiers — (step|steps)
 * must retry the longer branch when "step" leaves input unconsumed.
 * Wrapping textually as (...)$ would shift group numbers; instead the
 * whole parse is treated as one alternation and EOL is chained after. */
static Rx *rx_compile_full(const char *pattern, Rx **pool_out, int *ngroups_out) {
    RxC c;
    bool dotall;
    char *clean = rx_preprocess(pattern, &dotall);
    rx_dotall = dotall;
    c.p = clean;
    c.pool = xmalloc(sizeof(Rx) * RX_MAX_NODES);
    c.npool = 0;
    c.ngroups = 1;
    RxChain top = rx_parse_alt(&c);
    if (*c.p) { free(c.pool); free(clean); cljc_error("regex: unexpected )"); }
    free(clean);
    Rx *eol = rx_node(&c, RX_EOL);
    if (top.t) top.t->next = eol;
    *pool_out = c.pool;
    *ngroups_out = c.ngroups;
    return top.h ? top.h : eol;
}

/* Build the Clojure-style result: string when no groups, else
 * [full g1 g2 ...] with nil for unmatched groups. */
static Cljc *rx_result(const char *mstart, int ngroups) {
    Cljc *full = mk_str(mstart, (size_t)(rx_match_end - mstart));
    if (ngroups == 1) return full;
    Cljc *v = mk_empty_vec();
    v = vec_conj1(v, full);
    for (int i = 1; i < ngroups; i++) {
        if (rx_cap_s[i] && rx_cap_e[i] && rx_cap_e[i] >= rx_cap_s[i])
            v = vec_conj1(v, mk_str(rx_cap_s[i], (size_t)(rx_cap_e[i] - rx_cap_s[i])));
        else
            v = vec_conj1(v, NIL);
    }
    return v;
}

static void rx_reset_caps(void) {
    memset(rx_cap_s, 0, sizeof rx_cap_s);
    memset(rx_cap_e, 0, sizeof rx_cap_e);
    rx_steps = 0;
}

static Cljc *prim_re_find(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *pat = as_str(argv[0], "re-find");
    char *s = as_str(argv[1], "re-find");
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    for (const char *start = s; ; start++) {
        rx_reset_caps();
        if (rx_m(prog, start)) {
            Cljc *r = rx_result(start, ngroups);
            free(pool);
            return r;
        }
        if (!*start) break;
    }
    free(pool);
    return NIL;
}

static Cljc *prim_re_matches(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *pat = as_str(argv[0], "re-matches");
    char *s = as_str(argv[1], "re-matches");
    Rx *pool; int ngroups;
    /* anchored compile: full-match failure backtracks into alternations */
    Rx *prog = rx_compile_full(pat, &pool, &ngroups);
    rx_str_begin = s;
    rx_reset_caps();
    Cljc *r = NIL;
    if (rx_m(prog, s))
        r = rx_result(s, ngroups);
    free(pool);
    return r;
}

/* Front-anchored match (Java Matcher.lookingAt): match at position 0 only, NOT
 * required to reach the end. Returns the matched prefix (string, or [whole g1..]
 * when there are groups) or nil. Backs instaparse's re-match-at-front. */
static Cljc *prim_re_match_front(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    char *pat = as_str(argv[0], "cljc/re-match-front");
    char *s = as_str(argv[1], "cljc/re-match-front");
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    rx_reset_caps();
    Cljc *r = NIL;
    if (rx_m(prog, s)) r = rx_result(s, ngroups);
    free(pool);
    return r;
}

static Cljc *prim_re_seq(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *pat = as_str(argv[0], "re-seq");
    char *s = as_str(argv[1], "re-seq");
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    Cljc *out = NIL, **t = &out;
    const char *pos = s;
    for (;;) {
        /* Find the next match at or after pos. */
        const char *start = pos;
        bool found = false;
        for (;; start++) {
            rx_reset_caps();
            if (rx_m(prog, start)) { found = true; break; }
            if (!*start) break;
        }
        if (!found) break;
        *t = mk_cons(rx_result(start, ngroups), NIL);
        t = &(*t)->as.cons.tail;
        if (rx_match_end > start) {
            pos = rx_match_end;       /* continue after the match */
        } else {
            if (!*start) break;       /* empty match at end: done */
            pos = start + 1;          /* empty match: advance one char */
        }
    }
    free(pool);
    return out;
}

static Cljc *prim_set(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = mk_set();
    for (Cljc *l = to_seq(argv[0]); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        s = set_conj(s, l->as.cons.head);
    return s;
}

static Cljc *prim_hash_set(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = mk_set();
    for (int ai_ = 0; ai_ < nargs; ai_++)
        s = set_conj(s, argv[ai_]);
    return s;
}

static Cljc *prim_disj(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *s = argv[0];
    if (s == NIL) return NIL;
    if (s->tag == CLJC_SORTED && !s->as.sorted.is_map) {
        for (int i = 1; i < nargs; i++) s = sorted_remove(env, s, argv[i]);
        return s;
    }
    if (s->tag != CLJC_SET) cljc_error("disj: not a set");
    for (int i = 1; i < nargs; i++)
        s = set_disj(s, argv[i]);
    return s;
}


static Cljc *prim_alias(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *a = as_str(argv[0], "alias*");
    const char *ns = as_str(argv[1], "alias*");
    const char *in = intern(a, strlen(a));
    const char *nsin = intern(ns, strlen(ns));
    const char *owner = cur_reader_ns;   /* the ns this alias belongs to */
    for (int i = 0; i < n_aliases; i++)
        if (alias_table[i] == in && alias_owner[i] == owner) { alias_ns[i] = nsin; return NIL; }
    if (n_aliases >= MAX_ALIASES) cljc_error("too many aliases");
    alias_table[n_aliases] = in;
    alias_ns[n_aliases] = nsin;
    alias_owner[n_aliases] = owner;
    n_aliases++;
    return NIL;
}

/* (cljc/refer* "from-ns/name" "source-ns/name") — register a refer alias. */
static Cljc *prim_refer(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *f = as_str(argv[0], "refer*");
    const char *t = as_str(argv[1], "refer*");
    const char *fi = intern(f, strlen(f));
    const char *ti = intern(t, strlen(t));
    for (int i = 0; i < n_refers; i++)
        if (refer_from[i] == fi) { refer_to[i] = ti; return NIL; }
    if (n_refers >= MAX_REFERS) cljc_error("too many refers");
    refer_from[n_refers] = fi; refer_to[n_refers] = ti; n_refers++;
    return NIL;
}

/* (cljc/current-ns*) — the namespace being loaded ("user" at the top level). */
static Cljc *prim_current_ns(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    return cur_reader_ns ? mk_str(cur_reader_ns, strlen(cur_reader_ns))
                         : mk_str("", 0);
}

/* (cljc/in-ns* name-or-nil) — set the reader/def namespace, return the old.
 * nil means "back to the top level", which is the `user` namespace (so a later
 * top-level (:refer [reduce ..]) aliases under user/ instead of clobbering core). */
static Cljc *prim_in_ns(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *old = cur_reader_ns;
    Cljc *v = argv[0];
    cur_reader_ns = (v == NIL) ? intern("user", 4)
        : intern(v->as.str, strlen(v->as.str));
    return old ? mk_str(old, strlen(old)) : NIL;
}

/* (cljc/ns-publics* "ns") — vector of the bare names (strings) of every root
 * binding defined under the "ns/" prefix. Backs `use`, which refers them all. */
static Cljc *prim_ns_publics(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    const char *ns = as_str(argv[0], "ns-publics*");
    size_t nlen = strlen(ns);
    Cljc *out = mk_empty_vec();
    for (Binding *b = env_root(env)->bindings; b; b = b->next) {
        const char *nm = b->name;
        /* match "ns/<bare>" but not a deeper "ns/sub/..." qualified name */
        if (strncmp(nm, ns, nlen) == 0 && nm[nlen] == '/') {
            const char *bare = nm + nlen + 1;
            if (*bare && !strchr(bare, '/'))
                out = vec_conj1(out, mk_str(bare, strlen(bare)));
        }
    }
    return out;
}

/* (cljc/resolve-maybe "name") — the value bound to that (string) name, or nil
 * if unbound. Used by `cljc -m` to find a namespace's -main without throwing. */
static Cljc *prim_resolve_maybe(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    /* A symbol arg keeps its home_ns so per-ns aliases / home-ns-qualified defs
     * resolve (a stringified name loses it); a string arg resolves globally. */
    Cljc *arg = argv[0];
    if (arg == NIL) return NIL;
    const char *raw, *home_ns = NULL;
    if (arg->tag == CLJC_SYMBOL) { raw = arg->as.symc.name; home_ns = arg->as.symc.home_ns; }
    else raw = as_str(arg, "resolve-maybe");
    const char *name = intern(raw, strlen(raw));
    /* root_find handles the (require :as) alias + clojure.core fallbacks, so
     * a qualified name like edn/read-char* resolves to its real binding. */
    Binding *b = root_find(env_root(env), name, home_ns);
    return b ? b->value : NIL;
}

/* (cljc/resolve-var name) -> a Var (named reference to the binding) or nil. The
 * Var derefs to the binding's CURRENT value (so it tracks redefs) and carries
 * {:name :ns} metadata — this is what the public resolve / (var x) / #'x give,
 * so reflective code like potemkin's import-def can read a def's name. */
static Cljc *prim_resolve_var(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    /* A symbol arg keeps its home_ns so per-ns aliases resolve correctly (a
     * stringified name would lose it); a string arg resolves globally. */
    Cljc *arg = argv[0];
    if (arg == NIL) return NIL;
    const char *raw, *home_ns = NULL;
    if (arg->tag == CLJC_SYMBOL) { raw = arg->as.symc.name; home_ns = arg->as.symc.home_ns; }
    else raw = as_str(arg, "resolve");
    const char *name = intern(raw, strlen(raw));
    Binding *b = root_find(env_root(env), name, home_ns);
    if (!b) return NIL;
    Cljc *v = alloc(CLJC_VAR);
    v->as.var.name = b->name;
    const char *slash = strrchr(b->name, '/');
    const char *bare = slash ? slash + 1 : b->name;
    Cljc *m = map_assoc(mk_map(), mk_kw(intern("name", 4)), mk_sym(intern(bare, strlen(bare))));
    if (slash)
        m = map_assoc(m, mk_kw(intern("ns", 2)), mk_sym(intern(b->name, (size_t)(slash - b->name))));
    v->meta = m;
    return v;
}

/* (cljc/fn-arities f) -> [[fixed variadic?] ..] per arity. Lets the reflective
 * arity machinery (emmy.function uses .getDeclaredMethods) work without the JVM. */
static Cljc *prim_fn_arities(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *f = argv[0];
    if (f == NIL || f->tag != CLJC_FN) return mk_empty_vec();
    Cljc *out = mk_empty_vec();
    for (Cljc *ar = f->as.fn.arities; ar && ar->tag == CLJC_LIST; ar = ar->as.cons.tail) {
        Cljc *params = ar->as.cons.head->as.cons.head;
        size_t fixed; bool variadic; arity_info(params, &fixed, &variadic);
        Cljc *pair[2] = { mk_int((int64_t)fixed), mk_bool(variadic) };
        out = vec_conj1(out, mk_vector(pair, 2));
    }
    return out;
}

/* (cljc/set-var! var value) — set a Var's binding to value (backs alter-var-root). */
static Cljc *prim_set_var(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    Cljc *var = argv[0];
    if (var == NIL || var->tag != CLJC_VAR) cljc_error("set-var!: not a var");
    Binding *b = root_find(env_root(env), var->as.var.name, NULL);
    if (b) b->value = argv[1];
    return argv[1];
}

/* (cljc/eval-forms* src) — read and eval each top-level form in turn (NOT
 * read-all-then-eval), so a leading (ns ..) is active when later forms are
 * READ — their bare refs to ns-local defs then resolve. cur_reader_ns is
 * saved/restored so loading a file doesn't leak its ns to the caller. */
static Cljc *prim_eval_forms(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    const char *src = as_str(argv[0], "eval-forms");
    const char *saved_ns = cur_reader_ns;
    Cljc *last = NIL;
    while (*src) {
        skip_ws(&src);
        if (!*src) break;
        Cljc *form = read_form(&src);
        if (!form) break;
        last = eval(env, form);
    }
    cur_reader_ns = saved_ns;
    return last;
}

/* (macroexpand-1 form) — if form is a call to a macro, return its one-step
 * expansion (args unevaluated); otherwise return form unchanged. */
static Cljc *prim_macroexpand_1(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    Cljc *form = argv[0];
    if (form == NIL || form->tag != CLJC_LIST) return form;
    Cljc *head = form->as.cons.head;
    if (head == NIL || head->tag != CLJC_SYMBOL) return form;
    /* resolve head like resolve_symbol (locals, then root with home-ns), but
     * non-throwing — so a namespaced macro (loaded under (ns ..)) is found. */
    Cljc *fn = NULL;
    for (CljcEnv *e = env; e->parent; e = e->parent) {
        Cljc **p = env_local_find(e, head->as.symc.name);
        if (p) { fn = *p; break; }
    }
    if (!fn) {
        Binding *b = head->as.symc.root_cache;
        if (!b) b = root_find(env_root(env), head->as.symc.name, head->as.symc.home_ns);
        if (b) fn = b->value;
    }
    if (!fn || fn->tag != CLJC_FN || !fn->as.fn.is_macro) return form;
    size_t base = vsp;
    for (Cljc *a = form->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        vpush(a->as.cons.head);
    Cljc *expansion = apply(env, fn, &vstack[base], (int)(vsp - base));
    vsp = base;
    return expansion;
}

static Cljc *prim_with_meta(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    Cljc *m = argv[1];
    if (m != NIL && m->tag != CLJC_MAP) cljc_error("with-meta: meta must be a map");
    /* Lenient (Clojure throws on a non-IObj): metadata is advisory, and libraries
     * attach it to immutable scalars they treat as values (Emmy on a ratio
     * coefficient, notebook annotations on nil). Pass the value through. */
    switch (v == NIL ? CLJC_NIL : v->tag) {
        case CLJC_LIST: case CLJC_VECTOR: case CLJC_MAP: case CLJC_SET:
        case CLJC_FN: case CLJC_SYMBOL: case CLJC_STRING: break;
        default: return v;
    }
    Cljc *c = alloc(v->tag);
    c->as = v->as;
    if (v->tag == CLJC_VECTOR) {        /* tails are exclusively owned */
        c->as.vec.tail = tail_alloc(v->as.vec.taillen);
        memcpy(c->as.vec.tail, v->as.vec.tail, sizeof(Cljc *) * v->as.vec.taillen);
    }
    if (v->tag == CLJC_STRING) {        /* string buffers are owned: copy */
        size_t n = strlen(v->as.str);
        c->as.str = xmalloc(n + 1);
        memcpy(c->as.str, v->as.str, n + 1);
    }
    c->meta = m == NIL ? NULL : m;
    return c;
}

static Cljc *prim_meta(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    /* a deftype instance implementing IMeta keeps its metadata in a :meta field
     * (the convention SCI's Var/Namespace follow) — return that, deref'd if it
     * is a mutable (atom) field. */
    if (v != NIL && v->tag == CLJC_MAP) {
        Cljc *ty, *mf;
        if (map_find(v, mk_kw(intern("cljc/type", 9)), &ty) &&
            map_find(v, mk_kw(intern("meta", 4)), &mf)) {
            if (mf != NIL && mf->tag == CLJC_ATOM) return mf->as.atom.value;
            return mf;
        }
    }
    return (v != NIL && v->meta) ? v->meta : NIL;
}

static Cljc *prim_identical(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    return mk_bool(argv[0] == argv[1] ||
                   (argv[0]->tag == CLJC_INT && argv[1]->tag == CLJC_INT &&
                    argv[0]->as.i == argv[1]->as.i));  /* small-int cache parity */
}

static Cljc *prim_int(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_INT) return v;
    if (v != NIL && v->tag == CLJC_CHAR) return mk_int((int64_t)v->as.chr);  /* (int \a) => 97 */
    if (v != NIL && v->tag == CLJC_DOUBLE) return mk_int((int64_t)v->as.d);
    if (v != NIL && v->tag == CLJC_STRING && v->as.str[0])
        return mk_int((int64_t)(unsigned char)v->as.str[0]);  /* lenient: (int "a") */
    cljc_error("int: expected a number or character");
    return NIL;
}

static Cljc *prim_getenv_raw(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *v = getenv(as_str(argv[0], "cljc/env*"));
    return v ? mk_str(v, strlen(v)) : NIL;
}

static Cljc *prim_sharedir(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    return mk_str(CLJC_SHAREDIR, strlen(CLJC_SHAREDIR));
}

/* The share dir relative to THIS executable (PREFIX/bin/cljc -> PREFIX/share/cljc),
 * so a binary installed under any PREFIX finds its batteries regardless of the
 * compile-time CLJC_SHAREDIR. nil if the exe path can't be determined. */
static Cljc *prim_exe_sharedir(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
#ifdef __linux__
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NIL;
    buf[(size_t)n] = '\0';
    char *slash = strrchr(buf, '/');     /* drop the exe name -> .../bin */
    if (!slash) return NIL;
    *slash = '\0';
    slash = strrchr(buf, '/');           /* drop "bin" -> PREFIX */
    if (!slash) return NIL;
    *slash = '\0';
    char path[sizeof(buf) + 32];
    snprintf(path, sizeof(path), "%s/share/cljc", buf);
    return mk_str(path, strlen(path));
#else
    return NIL;
#endif
}

static Cljc *prim_hash(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int((int64_t)cljc_hash(argv[0]));
}

static Cljc *prim_type(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return NIL;
    if (v->tag == CLJC_MAP) {  /* records are maps tagged with :cljc/type */
        Cljc *t;
        if (map_find(v, mk_kw(intern("cljc/type", 9)), &t)) return t;
        return mk_kw(intern("map", 3));
    }
    const char *n;
    switch (v->tag) {
        case CLJC_BOOL: n = "boolean"; break;
        case CLJC_INT: n = "int"; break;
        case CLJC_BIGINT: n = "bigint"; break;
        case CLJC_RATIO: n = "ratio"; break;
        case CLJC_VAR: n = "var"; break;
        case CLJC_DOUBLE: n = "double"; break;
        case CLJC_STRING: n = "string"; break;
        case CLJC_CHAR: n = "char"; break;
        case CLJC_KEYWORD: n = "keyword"; break;
        case CLJC_SYMBOL: n = "symbol"; break;
        case CLJC_LIST: case CLJC_EMPTY: n = "list"; break;
        case CLJC_LAZY: n = "lazy-seq"; break;
        case CLJC_VECTOR: n = "vector"; break;
        case CLJC_SET: n = "set"; break;
        case CLJC_ATOM: n = "atom"; break;
        case CLJC_FN: case CLJC_NATIVE: n = "fn"; break;
        case CLJC_CORO: n = "coroutine"; break;
        default: n = "unknown"; break;
    }
    return mk_kw(intern(n, strlen(n)));
}

/* (cljc/chunk-map* f s n) => [strict-result-list rest-seq]; consumes at
 * most n elements but NEVER FORCES an unrealized lazy tail after the
 * first element. Already-realized list runs chunk at full speed, but
 * (map f (iterate g x)) advances one element per step — over-realizing
 * past a take-while boundary called user fns on values they were never
 * meant to see (JVM map doesn't chunk unchunked sources either). */
static Cljc *prim_chunk_map(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *f = argv[0];
    Cljc *s = seq1(argv[1]);
    int64_t n = as_int(argv[2], "chunk-map");
    Cljc *out = NIL, **t = &out;
    while (n-- > 0 && s != NIL) {
        *t = mk_cons(apply(env, f, &s->as.cons.head, 1), NIL);
        t = &(*t)->as.cons.tail;
        Cljc *tail = s->as.cons.tail;
        if (tail != NIL && tail->tag == CLJC_LAZY &&
            !tail->as.lazy.done) { s = tail; break; }
        s = seq1(tail);
    }
    Cljc *pair[2] = {out, s};
    return mk_vector(pair, 2);
}

static Cljc *prim_chunk_filter(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *f = argv[0];
    Cljc *s = seq1(argv[1]);
    int64_t n = as_int(argv[2], "chunk-filter");
    Cljc *out = NIL, **t = &out;
    while (n-- > 0 && s != NIL) {
        if (is_truthy(apply(env, f, &s->as.cons.head, 1))) {
            *t = mk_cons(s->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
        Cljc *tail = s->as.cons.tail;
        if (tail != NIL && tail->tag == CLJC_LAZY &&
            !tail->as.lazy.done) { s = tail; break; }
        s = seq1(tail);
    }
    Cljc *pair[2] = {out, s};
    return mk_vector(pair, 2);
}

/* (cljc/onto strict-list tail) — splice `tail` (possibly lazy) onto the end of
 * `lst`. lst is a fresh, unshared chunk list from the chunk-map / chunk-filter
 * natives, so we mutate its last cons in place instead of copying every element
 * (this is the hot map/filter chunk-splice — copying doubled conses per item). */
static Cljc *prim_onto(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *lst = argv[0];
    Cljc *tail = argv[1];
    if (lst == NIL || lst->tag != CLJC_LIST) return tail;
    Cljc *l = lst;
    while (l->as.cons.tail != NIL && l->as.cons.tail->tag == CLJC_LIST)
        l = l->as.cons.tail;
    l->as.cons.tail = tail;
    return lst;
}

static Cljc *prim_gc(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    gc_collect();
    return mk_int((int64_t)gc_freed_last);  /* cells freed by this collection */
}

/* ── Exceptions ── */

static Cljc *prim_throw(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    cljc_throw_value(argv[0]);
    return NIL;  /* unreachable */
}

static const char *kw_message(void) { return intern("message", 7); }
static const char *kw_data(void)    { return intern("data", 4); }

static Cljc *prim_ex_info(CljcEnv *env, Cljc **argv, int nargs) {
    /* (ex-info msg data) => {:message msg :data data} — exceptions are plain
     * maps here, so all map functions work on them. */
    (void)env;
    Cljc *m = mk_map();
    m = map_assoc(m, mk_kw(kw_message()), argv[0]);
    m = map_assoc(m, mk_kw(kw_data()), argv[1]);
    if (nargs >= 3 && argv[2] != NIL)           /* (ex-info msg data cause) */
        m = map_assoc(m, mk_kw(intern("cause", 5)), argv[2]);
    return m;
}

static Cljc *prim_ex_message(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *e = argv[0];
    if (e != NIL && e->tag == CLJC_STRING) return e;  /* interpreter errors are strings */
    if (e != NIL && e->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(e, mk_kw(kw_message()), &out)) return out;
    }
    return NIL;
}

static Cljc *prim_ex_data(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *e = argv[0];
    if (e != NIL && e->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(e, mk_kw(kw_data()), &out)) return out;
    }
    return NIL;
}

/* ── Atoms ── */

static Cljc *as_atom(Cljc *v, const char *what) {
    if (v == NIL || v->tag != CLJC_ATOM) cljc_error("%s: expected an atom, got %s", what, val_type_name(v));
    return v;
}

static Cljc *prim_atom(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *a = alloc(CLJC_ATOM);
    a->as.atom.value = argv[0];
    return a;
}

static Cljc *prim_deref(CljcEnv *env, Cljc **argv, int nargs) {
    (void)nargs;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_ATOM) return v->as.atom.value;
    if (v != NIL && v->tag == CLJC_VAR) {     /* deref a Var -> its current value */
        Binding *b = root_find(env_root(env), v->as.var.name, NULL);
        return b ? b->value : NIL;
    }
    if (v != NIL && v->tag == CLJC_MAP) {
        /* CljcDelay: force in C, independent of the multimethod table — a
         * library that redefines `deref` as a defmulti resets that table, so
         * delays must not depend on it. {:cljc/type :CljcDelay :f thunk :v val} */
        Cljc *ty;
        if (map_find(v, mk_kw(intern("cljc/type", 9)), &ty) &&
            ty == mk_kw(intern("CljcDelay", 9))) {
            Cljc *fa = NIL, *va = NIL;
            map_find(v, mk_kw(intern("f", 1)), &fa);
            map_find(v, mk_kw(intern("v", 1)), &va);
            if (fa != NIL && fa->tag == CLJC_ATOM && fa->as.atom.value != NIL) {
                Cljc *val = apply(env, fa->as.atom.value, NULL, 0);
                if (va != NIL && va->tag == CLJC_ATOM) va->as.atom.value = val;
                fa->as.atom.value = NIL;
                return val;
            }
            return (va != NIL && va->tag == CLJC_ATOM) ? va->as.atom.value : NIL;
        }
    }
    /* a deftype implementing deref/IDeref (e.g. SCI's Var) */
    Cljc *out;
    if (dispatch_deftype_method(env, v, "deref", NULL, 0, &out)) return out;
    /* a scalar is clearly not derefable: surface the mistake rather than
     * silently returning it (which becomes a cryptic error downstream). */
    if (v != NIL) switch (v->tag) {
        case CLJC_INT: case CLJC_BIGINT: case CLJC_DOUBLE: case CLJC_RATIO:
        case CLJC_STRING: case CLJC_CHAR: case CLJC_KEYWORD: case CLJC_SYMBOL:
        case CLJC_BOOL:
            cljc_error("deref: %s is not derefable (expected an atom, var, or delay)",
                       val_type_name(v));
            break;
        default: break;
    }
    return v;   /* otherwise identity: @#'fn (var -> value) derefs to itself */
}

static Cljc *prim_reset(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    /* a deftype with its own atom semantics (a `deref` + `cljc/cas` method, e.g.
     * DataScript's Conn via extend-clj/deftype-atom): reset! = cas(@x, v). */
    if (argv[0] != NIL && argv[0]->tag == CLJC_MAP) {
        Cljc *old, *casout;
        if (dispatch_deftype_method(env, argv[0], "deref", NULL, 0, &old)) {
            Cljc *cargs[2] = { old, argv[1] };
            if (dispatch_deftype_method(env, argv[0], "cljc/cas", cargs, 2, &casout))
                return argv[1];
        }
    }
    Cljc *a = as_atom(argv[0], "reset!");
    Cljc *v = argv[1];
    a->as.atom.value = v;
    return v;
}

static Cljc *prim_swap(CljcEnv *env, Cljc **argv, int nargs) {
    /* (swap! a f x y) => sets a to (f @a x y), returns the new value. */
    /* deftype atom (Conn): new = (apply f @x args); cas(@x, new). */
    if (argv[0] != NIL && argv[0]->tag == CLJC_MAP) {
        Cljc *old;
        if (dispatch_deftype_method(env, argv[0], "deref", NULL, 0, &old)) {
            size_t b = vsp;
            vpush(old);
            for (int i = 2; i < nargs; i++) vpush(argv[i]);
            Cljc *nv = apply(env, argv[1], &vstack[b], (int)(vsp - b));
            vsp = b;
            Cljc *cargs[2] = { old, nv }, *casout;
            if (dispatch_deftype_method(env, argv[0], "cljc/cas", cargs, 2, &casout))
                return nv;
        }
    }
    Cljc *a = as_atom(argv[0], "swap!");
    Cljc *f = argv[1];
    size_t base = vsp;
    vpush(a->as.atom.value);
    for (int i = 2; i < nargs; i++) vpush(argv[i]);
    Cljc *nv = apply(env, f, &vstack[base], (int)(vsp - base));
    vsp = base;
    a->as.atom.value = nv;
    return nv;
}

/* ── compare / sort ── */

static int cmp_values(Cljc *a, Cljc *b) {
    if (a == b) return 0;
    if (a == NIL) return -1;          /* nil sorts first */
    if (b == NIL) return 1;
    bool a_num = a->tag == CLJC_INT || a->tag == CLJC_DOUBLE || a->tag == CLJC_BIGINT || a->tag == CLJC_RATIO;
    bool b_num = b->tag == CLJC_INT || b->tag == CLJC_DOUBLE || b->tag == CLJC_BIGINT || b->tag == CLJC_RATIO;
    if (a_num && b_num) return num_cmp(a, b);   /* exact for bigints/ratios */
    if (a->tag != b->tag) cljc_error("compare: %s and %s are not comparable", val_type_name(a), val_type_name(b));
    switch (a->tag) {
        case CLJC_STRING:  return strcmp(a->as.str, b->as.str);
        case CLJC_CHAR:    return a->as.chr - b->as.chr;
        case CLJC_KEYWORD: return strcmp(a->as.kw, b->as.kw);
        case CLJC_SYMBOL:  return strcmp(a->as.sym, b->as.sym);
        case CLJC_BOOL:    return (int)a->as.b - (int)b->as.b;
        case CLJC_VECTOR: case CLJC_LIST: {
            Cljc *sa = to_seq(a), *sb = to_seq(b);
            while (sa != NIL && sb != NIL) {
                int c = cmp_values(sa->as.cons.head, sb->as.cons.head);
                if (c) return c;
                sa = sa->as.cons.tail; sb = sb->as.cons.tail;
            }
            return sa != NIL ? 1 : sb != NIL ? -1 : 0;  /* shorter sorts first */
        }
        default: cljc_error("compare: %s is not comparable", val_type_name(a));
    }
    return 0;
}

static Cljc *prim_compare(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(cmp_values(argv[0], argv[1]));
}

/* ───── Sorted set/map (CLJC_SORTED) ───────────────────────────────────── */

static Cljc *mk_sorted(Cljc *cmp, bool is_map) {
    Cljc *s = alloc(CLJC_SORTED);
    s->as.sorted.root = NIL;
    s->as.sorted.cmp = cmp;          /* NIL => default cmp_values */
    s->as.sorted.is_map = is_map;
    return s;
}

/* the comparison key of an iteration entry: a set's element is its own key; a
 * map entry is a [k v] vector keyed by element 0. */
static Cljc *sorted_entry_key(Cljc *coll, Cljc *entry) {
    return coll->as.sorted.is_map ? vec_nth(entry, 0) : entry;
}

static int sorted_cmp_call(CljcEnv *env, Cljc *coll, Cljc *a, Cljc *b) {
    Cljc *cmp = coll->as.sorted.cmp;
    if (cmp == NIL) return cmp_values(a, b);
    Cljc *args[2] = { a, b };
    Cljc *r = apply(env, cmp, args, 2);
    if (r == NIL) return 0;
    if (r->tag == CLJC_INT)    return r->as.i < 0 ? -1 : r->as.i > 0 ? 1 : 0;
    if (r->tag == CLJC_DOUBLE) return r->as.d < 0 ? -1 : r->as.d > 0 ? 1 : 0;
    if (is_truthy(r)) return -1;     /* boolean comparator: true => a before b */
    Cljc *args2[2] = { b, a };
    return is_truthy(apply(env, cmp, args2, 2)) ? 1 : 0;
}

static Cljc *mk_map_entry(Cljc *k, Cljc *v) {
    Cljc *kv[2] = { k, v };
    return mk_vector(kv, 2);
}

/* ── weight-balanced (Adams') tree backing a sorted collection ──
 * Persistent: every insert/delete copies only the path it touches. DELTA bounds
 * the size ratio between siblings; GAMMA picks single vs double rotation. */
#define WBT_DELTA 3u
#define WBT_GAMMA 2u

static uint32_t tree_size(Cljc *n) { return n == NIL ? 0 : n->as.tnode.size; }

static Cljc *mk_tnode(Cljc *l, Cljc *key, Cljc *val, Cljc *r) {
    Cljc *n = alloc(CLJC_TNODE);
    n->as.tnode.left = l; n->as.tnode.right = r;
    n->as.tnode.key = key; n->as.tnode.val = val;
    n->as.tnode.size = tree_size(l) + tree_size(r) + 1;
    return n;
}

static Cljc *tnode_balance(Cljc *l, Cljc *key, Cljc *val, Cljc *r) {
    uint32_t ln = tree_size(l), rn = tree_size(r);
    if (ln + rn <= 1) return mk_tnode(l, key, val, r);
    if (rn > WBT_DELTA * ln) {                  /* right heavy: rotate left */
        Cljc *rl = r->as.tnode.left, *rr = r->as.tnode.right;
        if (tree_size(rl) < WBT_GAMMA * tree_size(rr))            /* single */
            return mk_tnode(mk_tnode(l, key, val, rl),
                            r->as.tnode.key, r->as.tnode.val, rr);
        return mk_tnode(mk_tnode(l, key, val, rl->as.tnode.left), /* double */
                        rl->as.tnode.key, rl->as.tnode.val,
                        mk_tnode(rl->as.tnode.right, r->as.tnode.key, r->as.tnode.val, rr));
    }
    if (ln > WBT_DELTA * rn) {                   /* left heavy: rotate right */
        Cljc *ll = l->as.tnode.left, *lr = l->as.tnode.right;
        if (tree_size(lr) < WBT_GAMMA * tree_size(ll))            /* single */
            return mk_tnode(ll, l->as.tnode.key, l->as.tnode.val,
                            mk_tnode(lr, key, val, r));
        return mk_tnode(mk_tnode(ll, l->as.tnode.key, l->as.tnode.val, lr->as.tnode.left),
                        lr->as.tnode.key, lr->as.tnode.val,       /* double */
                        mk_tnode(lr->as.tnode.right, key, val, r));
    }
    return mk_tnode(l, key, val, r);
}

static Cljc *tnode_insert(CljcEnv *env, Cljc *coll, Cljc *n, Cljc *key, Cljc *val) {
    if (n == NIL) return mk_tnode(NIL, key, val, NIL);
    int c = sorted_cmp_call(env, coll, key, n->as.tnode.key);
    if (c < 0) return tnode_balance(tnode_insert(env, coll, n->as.tnode.left, key, val),
                                    n->as.tnode.key, n->as.tnode.val, n->as.tnode.right);
    if (c > 0) return tnode_balance(n->as.tnode.left, n->as.tnode.key, n->as.tnode.val,
                                    tnode_insert(env, coll, n->as.tnode.right, key, val));
    if (!coll->as.sorted.is_map) return n;       /* set: keep existing element */
    return mk_tnode(n->as.tnode.left, key, val, n->as.tnode.right);  /* map: update */
}

static Cljc *tnode_delete_min(Cljc *n, Cljc **ok, Cljc **ov) {
    if (n->as.tnode.left == NIL) { *ok = n->as.tnode.key; *ov = n->as.tnode.val; return n->as.tnode.right; }
    return tnode_balance(tnode_delete_min(n->as.tnode.left, ok, ov),
                         n->as.tnode.key, n->as.tnode.val, n->as.tnode.right);
}
static Cljc *tnode_delete_max(Cljc *n, Cljc **ok, Cljc **ov) {
    if (n->as.tnode.right == NIL) { *ok = n->as.tnode.key; *ov = n->as.tnode.val; return n->as.tnode.left; }
    return tnode_balance(n->as.tnode.left, n->as.tnode.key, n->as.tnode.val,
                         tnode_delete_max(n->as.tnode.right, ok, ov));
}
/* join two subtrees of a removed node (the larger donates its extreme element). */
static Cljc *tnode_glue(Cljc *l, Cljc *r) {
    if (l == NIL) return r;
    if (r == NIL) return l;
    Cljc *k, *v;
    if (tree_size(l) > tree_size(r)) { Cljc *nl = tnode_delete_max(l, &k, &v); return tnode_balance(nl, k, v, r); }
    Cljc *nr = tnode_delete_min(r, &k, &v); return tnode_balance(l, k, v, nr);
}
static Cljc *tnode_delete(CljcEnv *env, Cljc *coll, Cljc *n, Cljc *key, bool *removed) {
    if (n == NIL) { *removed = false; return NIL; }
    int c = sorted_cmp_call(env, coll, key, n->as.tnode.key);
    if (c < 0) return tnode_balance(tnode_delete(env, coll, n->as.tnode.left, key, removed),
                                    n->as.tnode.key, n->as.tnode.val, n->as.tnode.right);
    if (c > 0) return tnode_balance(n->as.tnode.left, n->as.tnode.key, n->as.tnode.val,
                                    tnode_delete(env, coll, n->as.tnode.right, key, removed));
    *removed = true;
    return tnode_glue(n->as.tnode.left, n->as.tnode.right);
}

static Cljc *sorted_clone(Cljc *coll, Cljc *root) {
    Cljc *s = alloc(CLJC_SORTED);
    s->as.sorted.root = root;
    s->as.sorted.cmp = coll->as.sorted.cmp;
    s->as.sorted.is_map = coll->as.sorted.is_map;
    s->meta = coll->meta;
    return s;
}

static uint32_t sorted_count(Cljc *coll) { return tree_size(coll->as.sorted.root); }

/* set conj / map assoc: `val` is the element (set) or the raw value (map). */
static Cljc *sorted_put(CljcEnv *env, Cljc *coll, Cljc *key, Cljc *val) {
    return sorted_clone(coll, tnode_insert(env, coll, coll->as.sorted.root, key, val));
}
static Cljc *sorted_remove(CljcEnv *env, Cljc *coll, Cljc *key) {
    bool removed = false;
    Cljc *root = tnode_delete(env, coll, coll->as.sorted.root, key, &removed);
    return removed ? sorted_clone(coll, root) : coll;
}
static bool sorted_get(CljcEnv *env, Cljc *coll, Cljc *key, Cljc **out) {
    for (Cljc *n = coll->as.sorted.root; n != NIL; ) {
        int c = sorted_cmp_call(env, coll, key, n->as.tnode.key);
        if (c < 0) n = n->as.tnode.left;
        else if (c > 0) n = n->as.tnode.right;
        else { *out = n->as.tnode.val; return true; }
    }
    return false;
}
static bool sorted_contains(CljcEnv *env, Cljc *coll, Cljc *key) {
    Cljc *out; return sorted_get(env, coll, key, &out);
}

/* in-order entries (set: elements; map: [k v] vectors) as a cons list. */
static void tnode_collect(Cljc *coll, Cljc *n, Cljc ***t) {
    if (n == NIL) return;
    tnode_collect(coll, n->as.tnode.left, t);
    Cljc *e = coll->as.sorted.is_map ? mk_map_entry(n->as.tnode.key, n->as.tnode.val) : n->as.tnode.key;
    **t = mk_cons(e, NIL);
    *t = &(**t)->as.cons.tail;
    tnode_collect(coll, n->as.tnode.right, t);
}
static Cljc *sorted_entry_list(Cljc *coll) {
    Cljc *out = NIL, **t = &out;
    tnode_collect(coll, coll->as.sorted.root, &t);
    return out;
}

static Cljc *prim_sorted_set(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *s = mk_sorted(NIL, false);
    for (int i = 0; i < nargs; i++) s = sorted_put(env, s, argv[i], argv[i]);
    return s;
}
static Cljc *prim_sorted_set_by(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *s = mk_sorted(argv[0], false);
    for (int i = 1; i < nargs; i++) s = sorted_put(env, s, argv[i], argv[i]);
    return s;
}
static Cljc *prim_sorted_map(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *m = mk_sorted(NIL, true);
    for (int i = 0; i + 1 < nargs; i += 2)
        m = sorted_put(env, m, argv[i], argv[i + 1]);
    return m;
}
static Cljc *prim_sorted_map_by(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *m = mk_sorted(argv[0], true);
    for (int i = 1; i + 1 < nargs; i += 2)
        m = sorted_put(env, m, argv[i], argv[i + 1]);
    return m;
}

/* (subseq sc test key) / (subseq sc t1 k1 t2 k2): entries whose key satisfies
 * (test (compare entry-key key) 0). The satisfying set is contiguous (the data
 * is ordered). rev => descending order (rsubseq). */
static Cljc *sorted_subseq(CljcEnv *env, Cljc **argv, int nargs, bool rev) {
    Cljc *coll = argv[0];
    if (coll == NIL) return NIL;
    if (coll->tag != CLJC_SORTED) cljc_error("subseq: not a sorted collection");
    bool two = nargs >= 5;
    Cljc *t1 = argv[1], *k1 = argv[2];
    Cljc *t2 = two ? argv[3] : NIL, *k2 = two ? argv[4] : NIL;
    Cljc *out = NIL, **t = &out;
    for (Cljc *e = sorted_entry_list(coll); e != NIL; e = e->as.cons.tail) {
        Cljc *entry = e->as.cons.head, *ek = sorted_entry_key(coll, entry);
        Cljc *a1[2] = { mk_int(sorted_cmp_call(env, coll, ek, k1)), mk_int(0) };
        if (!is_truthy(apply(env, t1, a1, 2))) continue;
        if (two) {
            Cljc *a2[2] = { mk_int(sorted_cmp_call(env, coll, ek, k2)), mk_int(0) };
            if (!is_truthy(apply(env, t2, a2, 2))) continue;
        }
        if (rev) { out = mk_cons(entry, out); }          /* prepend => descending */
        else { *t = mk_cons(entry, NIL); t = &(*t)->as.cons.tail; }
    }
    return out;
}
static Cljc *prim_subseq(CljcEnv *env, Cljc **argv, int nargs) {
    return sorted_subseq(env, argv, nargs, false);
}
static Cljc *prim_rsubseq(CljcEnv *env, Cljc **argv, int nargs) {
    return sorted_subseq(env, argv, nargs, true);
}

/* qsort has no context parameter; the interpreter is single-threaded. */
static CljcEnv *g_sort_env;
static Cljc *g_sort_fn;

static int sort_adapter(const void *pa, const void *pb) {
    Cljc *a = *(Cljc *const *)pa, *b = *(Cljc *const *)pb;
    if (!g_sort_fn) return cmp_values(a, b);
    Cljc *two[2] = {a, b};
    Cljc *r = apply(g_sort_env, g_sort_fn, two, 2);
    if (r != NIL && r->tag == CLJC_INT) return (int)r->as.i;
    /* Boolean comparator: (f a b) true => a first; tie-break with (f b a). */
    if (is_truthy(r)) return -1;
    Cljc *two2[2] = {b, a};
    Cljc *r2 = apply(g_sort_env, g_sort_fn, two2, 2);
    return is_truthy(r2) ? 1 : 0;
}

static Cljc *prim_sort(CljcEnv *env, Cljc **argv, int nargs) {
    /* (sort coll) or (sort comparator coll) — returns a list. */
    Cljc *fn = NULL, *coll;
    if (nargs > 1) {
        fn = argv[0];
        coll = argv[1];
    } else coll = argv[0];
    Cljc *s = to_seq(coll);
    size_t n = list_len(s);
    Cljc **arr = xmalloc(sizeof(Cljc *) * (n ? n : 1));
    size_t i = 0;
    for (Cljc *l = s; l && l->tag == CLJC_LIST; l = l->as.cons.tail) arr[i++] = l->as.cons.head;
    g_sort_env = env; g_sort_fn = fn;
    qsort(arr, n, sizeof(Cljc *), sort_adapter);  /* elements stay rooted via coll */
    g_sort_fn = NULL;
    Cljc *out = n == 0 ? EMPTY : NIL;   /* (sort []) => () */
    for (size_t j = n; j > 0; j--) out = mk_cons(arr[j - 1], out);
    free(arr);
    return out;
}

/* ── coercions ── */

static Cljc *prim_vec(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = to_seq(argv[0]);
    Cljc *v = mk_empty_vec();
    for (Cljc *l = s; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        v = vec_conj1(v, l->as.cons.head);
    return v;
}

static const char *as_named(Cljc *v, const char *what) {
    if (v != NIL) {
        if (v->tag == CLJC_KEYWORD) return v->as.kw;
        if (v->tag == CLJC_SYMBOL) return v->as.sym;
        if (v->tag == CLJC_STRING) return v->as.str;
    }
    cljc_error("%s: expected a string, keyword, or symbol", what);
    return NULL;
}

static Cljc *prim_name(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *n = as_named(argv[0], "name");
    /* a symbol/keyword ns/foo has name "foo"; a string and the lone "/" division
     * symbol are returned verbatim. */
    if (argv[0] != NIL && argv[0]->tag != CLJC_STRING) {
        const char *slash = strchr(n, '/');
        if (slash && slash != n && slash[1] != '\0')
            return mk_str(slash + 1, strlen(slash + 1));
    }
    return mk_str(n, strlen(n));
}

static Cljc *prim_keyword(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    /* (keyword \a) => nil, like Clojure — keyword rejects chars. */
    if (argv[0] != NIL && argv[0]->tag == CLJC_CHAR) return NIL;
    if (nargs >= 2) {                       /* (keyword ns name) => :ns/name */
        const char *ns = as_named(argv[0], "keyword");
        const char *nm = as_named(argv[1], "keyword");
        size_t ln = strlen(ns), lm = strlen(nm);
        char *buf = xmalloc(ln + lm + 2);
        memcpy(buf, ns, ln); buf[ln] = '/';
        memcpy(buf + ln + 1, nm, lm); buf[ln + lm + 1] = '\0';
        Cljc *r = mk_kw(intern(buf, ln + lm + 1));
        free(buf);
        return r;
    }
    const char *n = as_named(argv[0], "keyword");
    return mk_kw(intern(n, strlen(n)));
}

/* string/symbol/keyword -> its name; a non-named, non-nil value (e.g. what
 * cljc's var-less `resolve` returns where Clojure would yield a Var) is coerced
 * via its printed form, so (symbol (resolve s)) doesn't crash. nil still errors. */
static const char *symbol_name_of(Cljc *x) {
    if (x != NIL && (x->tag == CLJC_STRING || x->tag == CLJC_SYMBOL ||
                     x->tag == CLJC_KEYWORD))
        return as_named(x, "symbol");
    if (x == NIL) return as_named(x, "symbol");   /* keep the Clojure-like error */
    /* A non-named value where Clojure would have a Var (e.g. (symbol (resolve
     * s))). Give it a qualified, deliberately non-(clojure|cljs).core name so
     * callers that branch on (namespace it) treat it as a foreign var rather
     * than a core one — and it never looks like a bare resolvable symbol. */
    return intern("cljc.opaque/var", 15);
}

static Cljc *prim_symbol(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    /* (symbol name) or (symbol ns name) — a nil ns yields the bare name */
    if (nargs >= 2 && argv[0] != NIL) {
        const char *ns = as_named(argv[0], "symbol");
        const char *nm = symbol_name_of(argv[1]);
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s", ns, nm);
        return mk_sym(intern(buf, strlen(buf)));
    }
    const char *n = symbol_name_of(nargs >= 2 ? argv[1] : argv[0]);
    return mk_sym(intern(n, strlen(n)));
}

static Cljc *prim_quot(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (argv[0]->tag == CLJC_BIGINT || argv[1]->tag == CLJC_BIGINT) {
        Cljc *q; big_divmod(argv[0], argv[1], &q, NULL); return q;
    }
    int64_t a = as_int(argv[0], "quot");
    int64_t b = as_int(argv[1], "quot");
    if (b == 0) cljc_error("Divide by zero");
    return mk_int(a / b);
}

/* ── strings ── */

static char *as_str(Cljc *v, const char *what) {
    if (v == NIL || v->tag != CLJC_STRING) cljc_error("%s: expected a string", what);
    return v->as.str;
}

static Cljc *prim_subs(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "subs");
    size_t len = strlen(s);
    int64_t start = as_int(argv[1], "subs");
    int64_t end = nargs > 2
        ? as_int(argv[2], "subs") : (int64_t)len;
    if (start < 0 || end < start || (size_t)end > len)
        cljc_error("subs: start %lld, end %lld out of bounds for length %zu",
                   (long long)start, (long long)end, len);
    return mk_str(s + start, (size_t)(end - start));
}

#define STR_MAP_FN(NAME, XFORM) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        char *s = as_str(argv[0], #NAME); \
        Cljc *r = mk_str(s, strlen(s)); \
        for (char *c = r->as.str; *c; c++) *c = (char)XFORM((unsigned char)*c); \
        return r; \
    }

STR_MAP_FN(upper_case, toupper)
STR_MAP_FN(lower_case, tolower)

static Cljc *prim_trim(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "trim");
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    return mk_str(s, n);
}

static Cljc *prim_re_split(CljcEnv *env, Cljc **argv, int nargs);

static Cljc *prim_split(CljcEnv *env, Cljc **argv, int nargs) {
    /* (str/split s sep) — regex when sep is a #"..." literal or came
     * through re-pattern (meta-tagged); plain strings split literally. */
    if (argv[1] != NIL && argv[1]->tag == CLJC_STRING && argv[1]->meta)
        return prim_re_split(env, argv, nargs);
    (void)env;
    char *s = as_str(argv[0], "split");
    char *sep = as_str(argv[1], "split");
    size_t seplen = strlen(sep);
    if (seplen == 0) cljc_error("split: empty separator");
    Cljc *parts = NIL, **t = &parts;  /* build as list (stack-rooted), then vec */
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, sep);
        size_t n = hit ? (size_t)(hit - p) : strlen(p);
        *t = mk_cons(mk_str(p, n), NIL);
        t = &(*t)->as.cons.tail;
        if (!hit) break;
        p = hit + seplen;
    }
    Cljc *one[1] = {parts};
    return prim_vec(env, one, 1);
}

#define STR_PRED(NAME, EXPR) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        char *s = as_str(argv[0], #NAME); \
        char *sub = as_str(argv[1], #NAME); \
        size_t sl = strlen(s), bl = strlen(sub); \
        (void)sl; (void)bl; \
        return mk_bool(EXPR); \
    }

STR_PRED(starts_with, strncmp(s, sub, bl) == 0)
STR_PRED(ends_with,   bl <= sl && strcmp(s + sl - bl, sub) == 0)
STR_PRED(includes,    strstr(s, sub) != NULL)

/* (str/index-of s sub [from]) → first index at/after from, or nil. */
/* (str/index-of "..." \c) is valid in Clojure — a char search value. */
static char *strval_or_char(Cljc *v, char buf[5], const char *what) {
    if (v != NIL && v->tag == CLJC_CHAR) {
        int cp = v->as.chr, n = 0;
        if (cp < 0x80) buf[n++] = (char)cp;
        else if (cp < 0x800) { buf[n++] = (char)(0xC0|(cp>>6)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { buf[n++] = (char)(0xE0|(cp>>12)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        else { buf[n++] = (char)(0xF0|(cp>>18)); buf[n++] = (char)(0x80|((cp>>12)&0x3F)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
        buf[n] = '\0';
        return buf;
    }
    return as_str(v, what);
}

static Cljc *prim_index_of(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (argv[0] != NIL && argv[0]->tag != CLJC_STRING) cljc_error("str/index-of: expected a string, got %s", val_type_name(argv[0]));
    char *s = as_str(argv[0], "str/index-of");
    char cb[5];
    char *sub = strval_or_char(argv[1], cb, "str/index-of");
    size_t sl = strlen(s);
    size_t from = nargs > 2 ? (size_t)as_int(argv[2], "str/index-of") : 0;
    if (from > sl) return NIL;
    char *hit = strstr(s + from, sub);
    return hit ? mk_int((int64_t)(hit - s)) : NIL;
}

static Cljc *prim_blank_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return TRUE;
    char *s = as_str(v, "blank?");
    for (; *s; s++) if (!isspace((unsigned char)*s)) return FALSE;
    return TRUE;
}

/* ── file IO ── */

/* Embedded virtual files for self-contained bundles. A bundle registers its
 * script's transitive .clj dependencies here; slurp checks this table before
 * the filesystem, so require/load-file resolve embedded files even when no
 * vendor/ or share dir is present. Empty in a normal cljc → no effect. */
typedef struct { const char *name; const char *data; } CljcEmbeddedFile;
static const CljcEmbeddedFile *cljc_embedded;
static int cljc_n_embedded;

/* Match by exact name or by a '/'-bounded suffix, so a registered
 * "clojure/test.clj" satisfies a lookup of "./clojure/test.clj",
 * "vendor/clojure/test.clj", etc. (the paths require/ builds from *load-path*). */
static const char *embedded_lookup(const char *path) {
    size_t pl = strlen(path);
    for (int i = 0; i < cljc_n_embedded; i++) {
        const char *name = cljc_embedded[i].name;
        size_t nl = strlen(name);
        if (pl == nl && !strcmp(path, name)) return cljc_embedded[i].data;
        if (pl > nl && path[pl - nl - 1] == '/' && !strcmp(path + pl - nl, name))
            return cljc_embedded[i].data;
    }
    return NULL;
}

static Cljc *prim_slurp(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "slurp");
    const char *emb = embedded_lookup(path);
    if (emb) return mk_str(emb, strlen(emb));   /* bundled dependency */
    FILE *f = fopen(path, "rb");
    if (!f) cljc_error("slurp: cannot open %s", path);
    SBuf sb = {0};
    sb_grow(&sb, 4096);
    size_t n;
    char buf[4096];
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sb_grow(&sb, n);
        memcpy(sb.data + sb.len, buf, n);
        sb.len += n;
        sb.data[sb.len] = '\0';
    }
    fclose(f);
    Cljc *r = mk_str(sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return r;
}

static Cljc *prim_spit(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "spit");
    Cljc *content = argv[1];
    FILE *f = fopen(path, "wb");
    if (!f) cljc_error("spit: cannot open %s", path);
    SBuf sb = {0};
    print_to(&sb, content, false);
    if (sb.data) fwrite(sb.data, 1, sb.len, f);
    free(sb.data);
    fclose(f);
    return NIL;
}

/* (cljc/mtime* path) → file mtime in milliseconds, or nil if missing.
 * Millisecond resolution so the clerk file watcher sees rapid saves. */
static Cljc *prim_mtime(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "cljc/mtime*");
    struct stat st;
    if (stat(path, &st) != 0) return NIL;
#ifdef _WIN32
    return mk_int((int64_t)st.st_mtime * 1000);   /* second resolution only */
#else
    return mk_int((int64_t)st.st_mtim.tv_sec * 1000
                  + st.st_mtim.tv_nsec / 1000000);
#endif
}

/* ── MD5 (RFC 1321) — backs util/md5-style hashing puzzles ── */

static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
static const int md5_r[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

static void md5_block(uint32_t st[4], const unsigned char *p) {
    uint32_t m[16], a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8)
             | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16)      { f = (b & c) | (~b & d);  g = (uint32_t)i; }
        else if (i < 32) { f = (d & b) | (~d & c);  g = (5u*(uint32_t)i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3u*(uint32_t)i + 5) & 15; }
        else             { f = c ^ (b | ~d);        g = (7u*(uint32_t)i) & 15; }
        uint32_t t = d;
        d = c; c = b;
        uint32_t x = a + f + md5_k[i] + m[g];
        b += (x << md5_r[i]) | (x >> (32 - md5_r[i]));
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

/* (cljc/md5* s) → 32-char lowercase hex digest. */
static Cljc *prim_md5(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "cljc/md5*");
    size_t n = strlen(s);
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) md5_block(st, (const unsigned char *)s + i);
    unsigned char tail[128];
    size_t tn = n - i;
    memcpy(tail, s + i, tn);
    tail[tn++] = 0x80;
    size_t blocks = tn + 8 <= 64 ? 64 : 128;
    memset(tail + tn, 0, blocks - tn);
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) tail[blocks - 8 + k] = (unsigned char)(bits >> (8 * k));
    md5_block(st, tail);
    if (blocks == 128) md5_block(st, tail + 64);
    char hex[33];
    for (int k = 0; k < 16; k++)
        snprintf(hex + 2*k, 3, "%02x", (st[k/4] >> (8 * (k & 3))) & 0xff);
    return mk_str(hex, 32);
}

/* (flush) → flush the interpreter's stdout. */
static Cljc *prim_flush(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    fflush(COUT);
    return NIL;
}

/* (read-line) → next line from stdin (no trailing newline), nil at EOF. */
static Cljc *prim_read_line(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return NIL;
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') len--;
    return mk_str(buf, len);
}

/* (cljc/isatty*) → true when stdout is a terminal (color/prompt gating). */
static Cljc *prim_isatty(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    return mk_bool(isatty(1));
}

/* (cljc/now-ms*) → monotonic milliseconds as a double (for `time`). */
static Cljc *prim_now_ms(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
#ifdef _WIN32
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return mk_double((double)ctr.QuadPart * 1000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return mk_double((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
#endif
}

/* (cljc/sleep-ms* ms) → sleep this many milliseconds (csp timeouts). */
static Cljc *prim_sleep_ms(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 1) return NIL;
    double ms = as_num(argv[0]);
    if (ms <= 0) return NIL;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
#endif
    return NIL;
}

/* (cljc/poll-fds* fds events timeout-ms) → readiness vector for the csp event
 * loop. fds/events are parallel vectors; event bit 1 = wait-readable, bit 2 =
 * wait-writable. Returns a vector of ints with bit 1 set when readable (or the
 * peer hung up / errored, so a parked reader wakes to see EOF) and bit 2 when
 * writable. timeout-ms: 0 polls, -1 blocks until something is ready. */
static Cljc *prim_poll_fds(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 3) cljc_error("cljc/poll-fds*: (fds events timeout-ms)");
    if (argv[0]->tag != CLJC_VECTOR || argv[1]->tag != CLJC_VECTOR)
        cljc_error("cljc/poll-fds*: fds and events must be vectors");
    size_t n = vec_len(argv[0]);
    int timeout = (int)as_int(argv[2], "poll-fds*");
    if (n == 0) { (void)timeout; return mk_vector(NULL, 0); }
    struct pollfd *pfd = xmalloc(sizeof(struct pollfd) * n);
    for (size_t i = 0; i < n; i++) {
        int ev = (int)as_int(vec_nth(argv[1], i), "poll-fds*");
        pfd[i].fd = (int)as_int(vec_nth(argv[0], i), "poll-fds*");
        pfd[i].events = (short)(((ev & 1) ? POLLIN : 0) | ((ev & 2) ? POLLOUT : 0));
        pfd[i].revents = 0;
    }
    poll(pfd, (nfds_t)n, timeout);
    /* revents are 0..3 — all smallints (immortal), so building the result
     * vector triggers no GC hazard for the in-flight item array. */
    Cljc **items = xmalloc(sizeof(Cljc *) * n);
    for (size_t i = 0; i < n; i++) {
        int re = 0;
        if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) re |= 1;
        if (pfd[i].revents & POLLOUT) re |= 2;
        items[i] = mk_int(re);
    }
    Cljc *out = mk_vector(items, n);
    free(items);
    free(pfd);
    return out;
}

/* (cljc/epoch*) → unix seconds (libc.clj's now-epoch; the C time symbol
 * must not be FFI-bound by name — it would shadow the core time macro). */
static Cljc *prim_epoch(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs;
    return mk_int((int64_t)time(NULL));
}

/* (cljc/dir?* path) → true if path exists and is a directory. */
static Cljc *prim_dir_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "cljc/dir?*");
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? TRUE : FALSE;
}

/* (cljc/list-dir* path) → vector of entry names (sans . and ..), or nil
 * if path is unreadable / not a directory. */
static Cljc *prim_list_dir(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "cljc/list-dir*");
    DIR *d = opendir(path);
    if (!d) return NIL;
    Cljc *v = mk_empty_vec();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        v = vec_conj1(v, mk_str(e->d_name, strlen(e->d_name)));
    }
    closedir(d);
    return v;
}

/* ── TCP primitives ──
 * Generic loopback TCP for clj-land servers (the clerk notebook uses them;
 * the nREPL server predates them and keeps its own loop). File descriptors
 * travel as plain ints, mirroring the FFI's pointers-as-ints convention. */

static Cljc *prim_tcp_listen(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (cljc_wsa_init() != 0) cljc_error("tcp/listen: winsock init failed");
    int port = (int)as_int(argv[0], "tcp/listen");
    /* Optional host arg. Default 127.0.0.1 (loopback-only, the safe default
     * that nREPL relies on); pass "0.0.0.0" to listen on all interfaces, or a
     * specific dotted IP (e.g. a tailscale address) to bind just that one. */
    const char *host = nargs > 1 ? as_str(argv[1], "tcp/listen") : NULL;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) cljc_error("tcp/listen: cannot create socket");
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    if (!host || !*host) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (!strcmp(host, "0.0.0.0") || !strcmp(host, "*")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
        if (addr.sin_addr.s_addr == INADDR_NONE)
            cljc_error("tcp/listen: bad host address '%s'", host);
    }
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        sock_close(srv);
        cljc_error("tcp/listen: cannot bind %s:%d (port in use?)",
                   host && *host ? host : "127.0.0.1", port);
    }
    if (listen(srv, 16) < 0) { sock_close(srv); cljc_error("tcp/listen: listen failed"); }
    return mk_int(srv);
}

/* (tcp/accept srv timeout-ms) → client fd, or nil on timeout.
 * The timeout makes a single-threaded serve loop possible: each expiry is
 * a tick the caller can spend polling file mtimes. */
static Cljc *prim_tcp_accept(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int srv = (int)as_int(argv[0], "tcp/accept");
    int timeout = nargs > 1 ? (int)as_int(argv[1], "tcp/accept") : -1;
    struct pollfd p = { .fd = srv, .events = POLLIN };
    if (poll(&p, 1, timeout) <= 0) return NIL;
    int fd = accept(srv, NULL, NULL);
    return fd < 0 ? NIL : mk_int(fd);
}

/* (tcp/recv fd) → string from one read (≤64K), or nil on EOF/error. */
static Cljc *prim_tcp_recv(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int fd = (int)as_int(argv[0], "tcp/recv");
    char buf[65536];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return NIL;
    return mk_str(buf, (size_t)n);
}

/* (tcp/send fd s) → true if fully sent, false on a dead peer.
 * MSG_NOSIGNAL: a browser closing an SSE stream must not SIGPIPE-kill us. */
static Cljc *prim_tcp_send(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int fd = (int)as_int(argv[0], "tcp/send");
    Cljc *v = argv[1];
    SBuf sb = {0};
    print_to(&sb, v, false);
    const char *p = sb.data ? sb.data : "";
    size_t left = sb.len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n <= 0) { free(sb.data); return FALSE; }
        p += n;
        left -= (size_t)n;
    }
    free(sb.data);
    return TRUE;
}

static Cljc *prim_tcp_close(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    sock_close((int)as_int(argv[0], "tcp/close"));
    return NIL;
}

/* (tcp/send-some* fd str start) → ONE non-blocking send of str[start..]. Returns
 * bytes written (≥0), -1 if it would block (caller should park on POLLOUT and
 * retry), or -2 on a dead peer. The backpressure primitive for csp's send!. */
static Cljc *prim_tcp_send_some(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 2) cljc_error("tcp/send-some*: (fd str [start])");
    int fd = (int)as_int(argv[0], "tcp/send-some*");
    char *s = as_str(argv[1], "tcp/send-some*");
    size_t total = strlen(s);
    size_t start = nargs > 2 ? (size_t)as_int(argv[2], "tcp/send-some*") : 0;
    if (start >= total) return mk_int(0);
    int flags = MSG_NOSIGNAL;
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    ssize_t n = send(fd, s + start, total - start, flags);
    if (n >= 0) return mk_int((int64_t)n);
    if (errno == EAGAIN || errno == EWOULDBLOCK) return mk_int(-1);
    return mk_int(-2);
}

/* (tcp/connect host port) → a connected client fd. */
static Cljc *prim_tcp_connect(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (cljc_wsa_init() != 0) cljc_error("tcp/connect: winsock init failed");
    if (nargs < 2) cljc_error("tcp/connect: (host port)");
    const char *host = as_str(argv[0], "tcp/connect");
    int port = (int)as_int(argv[1], "tcp/connect");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) cljc_error("tcp/connect: cannot create socket");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = strcmp(host, "localhost") ? inet_addr(host)
                                                      : htonl(INADDR_LOOPBACK);
    if (addr.sin_addr.s_addr == INADDR_NONE)
        { sock_close(fd); cljc_error("tcp/connect: bad host '%s'", host); }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        { sock_close(fd); cljc_error("tcp/connect: cannot reach %s:%d", host, port); }
    return mk_int(fd);
}

/* ── printing variants ── */

static Cljc *print_args(CljcEnv *env, Cljc **argv, int nargs, bool readably, bool newline) {
    SBuf sb = {0};
    bool first = true;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, argv[ai_], readably);
    }
    if (newline) sb_putc(&sb, '\n');
    emit_out(env, sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return NIL;
}

/* (cljc/eprintln* ..) — like println but always to stderr (diagnostics). */
static Cljc *prim_eprintln(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    SBuf sb = {0};
    for (int i = 0; i < nargs; i++) { if (i) sb_putc(&sb, ' '); print_to(&sb, argv[i], false); }
    sb_putc(&sb, '\n');
    if (sb.data) { fwrite(sb.data, 1, sb.len, CERR); free(sb.data); }
    return NIL;
}

static Cljc *prim_pr(CljcEnv *env, Cljc **argv, int nargs)    { return print_args(env, argv, nargs, true, false); }
static Cljc *prim_prn(CljcEnv *env, Cljc **argv, int nargs)   { return print_args(env, argv, nargs, true, true); }
static Cljc *prim_print(CljcEnv *env, Cljc **argv, int nargs) { return print_args(env, argv, nargs, false, false); }


/* ── format / string-replace / regex string ops ── */

static Cljc *prim_format(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *fmt = as_str(argv[0], "format");
    int ai = 1;
    SBuf out = {0};
    sb_grow(&out, 1); out.data[0] = '\0';
    for (const char *f = fmt; *f; ) {
        if (*f != '%') { sb_putc(&out, *f++); continue; }
        f++;
        if (*f == '%') { sb_putc(&out, '%'); f++; continue; }
        char spec[20]; int si = 0;
        while (*f && strchr("-+ 0#.0123456789", *f) && si < 16) spec[si++] = *f++;
        spec[si] = '\0';
        char conv = *f++;
        if (!conv) cljc_error("format: dangling %%");
        if (ai >= nargs) cljc_error("format: not enough arguments");
        Cljc *v = argv[ai++];
        char cfmt[32], tmp[512];
        switch (conv) {
            case 's': {
                SBuf t = {0};
                print_to(&t, v, false);
                if (si == 0) {  /* no width/precision: append directly, any length */
                    if (t.data) sb_puts(&out, t.data);
                } else {
                    snprintf(cfmt, sizeof cfmt, "%%%ss", spec);
                    snprintf(tmp, sizeof tmp, cfmt, t.data ? t.data : "");
                    sb_puts(&out, tmp);
                }
                free(t.data);
                break;
            }
            case 'd': case 'x': case 'X': case 'o':
                /* %x with a pre-formatted string (e.g. an md5 hex digest
                 * standing in for BigInteger): zero-pad and pass through. */
                if (v != NIL && v->tag == CLJC_STRING && (conv == 'x' || conv == 'X')) {
                    size_t want = (size_t)atoi(*spec == '0' ? spec + 1 : spec);
                    for (size_t k = strlen(v->as.str); k < want; k++) sb_putc(&out, '0');
                    sb_puts(&out, v->as.str);
                    break;
                }
                snprintf(cfmt, sizeof cfmt, "%%%sll%c", spec, conv);
                snprintf(tmp, sizeof tmp, cfmt, (long long)as_int(v, "format"));
                sb_puts(&out, tmp);
                break;
            case 'f': case 'e': case 'g':
                snprintf(cfmt, sizeof cfmt, "%%%s%c", spec, conv);
                snprintf(tmp, sizeof tmp, cfmt, as_num(v));
                sb_puts(&out, tmp);
                break;
            default:
                cljc_error("format: unsupported conversion %%%c", conv);
        }
    }
    Cljc *r = mk_str(out.data, out.len);
    free(out.data);
    return r;
}

static Cljc *prim_re_replace(CljcEnv *env, Cljc **argv, int nargs);

static Cljc *prim_str_replace(CljcEnv *env, Cljc **argv, int nargs) {
    /* (str/replace s match replacement). A #"..." / re-pattern match (a
     * meta-tagged string) does regex replacement with $1..$9 group refs in the
     * replacement; a plain string matches literally. Mirrors Clojure's dispatch
     * and matches how str/split already treats its separator. */
    if (argv[1] != NIL && argv[1]->tag == CLJC_STRING && argv[1]->meta)
        return prim_re_replace(env, argv, nargs);
    (void)env;
    char *s = as_str(argv[0], "replace");
    char *m = as_str(argv[1], "replace");
    char *r = as_str(argv[2], "replace");
    size_t mlen = strlen(m);
    if (mlen == 0) cljc_error("replace: empty match string");
    SBuf out = {0};
    sb_grow(&out, 1); out.data[0] = '\0';
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, m);
        if (!hit) { sb_puts(&out, p); break; }
        while (p < hit) sb_putc(&out, *p++);
        sb_puts(&out, r);
        p += mlen;
    }
    Cljc *res = mk_str(out.data, out.len);
    free(out.data);
    return res;
}

/* Append replacement text, expanding $0..$9 to capture groups; $$ => $;
 * a backslash escapes the next char (\$ => literal $, \\ => literal \). */
static void rx_subst(SBuf *out, const char *repl) {
    for (const char *r = repl; *r; r++) {
        if (*r == '\\' && r[1]) { sb_putc(out, r[1]); r++; continue; }
        if (*r == '$' && r[1] == '$') { sb_putc(out, '$'); r++; continue; }
        if (*r == '$' && isdigit((unsigned char)r[1])) {
            int g = r[1] - '0';
            r++;
            if (rx_cap_s[g] && rx_cap_e[g] && rx_cap_e[g] >= rx_cap_s[g])
                for (const char *c = rx_cap_s[g]; c < rx_cap_e[g]; c++) sb_putc(out, *c);
            continue;
        }
        sb_putc(out, *r);
    }
}

/* The value passed to a replacement FUNCTION: the matched string when the
 * pattern has no groups, else [match g1 .. gn] (nil for non-participating). */
static Cljc *rx_match_value(int ngroups) {
    /* ngroups counts group 0; ==1 means no user groups (pass the match string) */
    if (ngroups == 1)
        return mk_str(rx_cap_s[0], (size_t)(rx_cap_e[0] - rx_cap_s[0]));
    Cljc *v = mk_empty_vec();
    for (int g = 0; g < ngroups; g++)
        v = vec_conj1(v, (rx_cap_s[g] && rx_cap_e[g] && rx_cap_e[g] >= rx_cap_s[g])
                         ? mk_str(rx_cap_s[g], (size_t)(rx_cap_e[g] - rx_cap_s[g])) : NIL);
    return v;
}

static Cljc *prim_re_replace(CljcEnv *env, Cljc **argv, int nargs) {
    /* (re-replace s pattern replacement) — all matches. replacement is a string
     * ($1..$9 group refs) or a function (called with the match / [match g1..]). */
    (void)nargs;
    char *s = as_str(argv[0], "re-replace");
    char *pat = as_str(argv[1], "re-replace");
    Cljc *replv = argv[2];
    bool is_fn = replv != NIL && (replv->tag == CLJC_FN || replv->tag == CLJC_NATIVE);
    char *repl = is_fn ? NULL : as_str(argv[2], "re-replace");
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    SBuf out = {0};
    sb_grow(&out, 1); out.data[0] = '\0';
    const char *p = s;
    while (*p) {
        rx_reset_caps();
        rx_cap_s[0] = p;
        if (rx_m(prog, p)) {
            const char *mend = rx_match_end;          /* save: a fn may clobber rx_* */
            rx_cap_e[0] = mend;
            if (is_fn) {
                Cljc *mv = rx_match_value(ngroups);
                sb_puts(&out, as_str(apply(env, replv, &mv, 1), "replace fn"));
                rx_str_begin = s;                     /* restore after the call */
            } else rx_subst(&out, repl);
            if (mend > p) { p = mend; continue; }
            /* empty match: emit one char and advance to avoid looping */
            sb_putc(&out, *p);
        } else {
            sb_putc(&out, *p);
        }
        p++;
    }
    /* A trailing empty match at end-of-string still substitutes
     * ((re-replace "ab" "x*" "-") => "-a-b-", as in Clojure). */
    rx_reset_caps();
    rx_cap_s[0] = p;
    if (rx_m(prog, p) && rx_match_end == p) {
        rx_cap_e[0] = p;
        if (is_fn) {
            Cljc *mv = rx_match_value(ngroups);
            sb_puts(&out, as_str(apply(env, replv, &mv, 1), "replace fn"));
        } else rx_subst(&out, repl);
    }
    free(pool);
    Cljc *res = mk_str(out.data, out.len);
    free(out.data);
    return res;
}

static Cljc *prim_re_split(CljcEnv *env, Cljc **argv, int nargs) {
    /* (split s pattern [limit]) — Java/Clojure semantics: zero-width matches
     * split between chars (empty pattern → chars); limit=0 (default) drops
     * trailing empties, limit>0 caps the count, limit<0 keeps trailing empties.
     * A no-match returns [s] (so (split "" #",") => [""]). */
    (void)env;
    char *s = as_str(argv[0], "re-split");
    char *pat = as_str(argv[1], "re-split");
    long limit = (nargs > 2) ? (long)as_int(argv[2], "split") : 0;
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    Cljc *segs = NIL, **t = &segs;
    size_t nsegs = 0;
    bool any_match = false;
    const char *seg = s, *p = s, *end = s + strlen(s);
    while (*p) {
        if (limit > 0 && (long)nsegs == limit - 1) break;  /* last seg keeps the rest */
        rx_reset_caps(); rx_cap_s[0] = p;
        if (rx_m(prog, p)) {
            any_match = true;
            const char *mend = rx_match_end;
            if (mend > p) {                                /* non-empty match */
                *t = mk_cons(mk_str(seg, (size_t)(p - seg)), NIL); t = &(*t)->as.cons.tail; nsegs++;
                p = mend; seg = p;
            } else if (p == s) { p++; }                    /* zero-width at index 0: skip */
            else {                                         /* zero-width between chars */
                *t = mk_cons(mk_str(seg, (size_t)(p - seg)), NIL); t = &(*t)->as.cons.tail; nsegs++;
                seg = p; p++;
            }
        } else p++;
    }
    *t = mk_cons(mk_str(seg, (size_t)(end - seg)), NIL); nsegs++;   /* final segment */
    free(pool);
    size_t keep = nsegs;
    if (limit == 0 && any_match) {                         /* drop trailing empties */
        keep = 0; size_t i = 0;
        for (Cljc *l = segs; l && l->tag == CLJC_LIST; l = l->as.cons.tail, i++)
            if (l->as.cons.head->as.str[0]) keep = i + 1;
    }
    Cljc *v = mk_empty_vec();
    size_t i = 0;
    for (Cljc *l = segs; l && l->tag == CLJC_LIST && i < keep; l = l->as.cons.tail, i++)
        v = vec_conj1(v, l->as.cons.head);
    return v;
}

/* ── math / random ── */

#define MATH1(NAME, FN) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        return mk_double(FN(as_num(argv[0]))); \
    }

/* Two-arg double->double, same shape as MATH1. */
#define MATH2(NAME, FN) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; (void)nargs; \
        return mk_double(FN(as_num(argv[0]), as_num(argv[1]))); \
    }

MATH1(sqrt, sqrt)
MATH1(cbrt, cbrt)
MATH1(floor, floor)
MATH1(ceil, ceil)
MATH1(trunc, trunc)

/* trigonometric */
MATH1(sin, sin)
MATH1(cos, cos)
MATH1(tan, tan)
MATH1(asin, asin)
MATH1(acos, acos)
MATH1(atan, atan)

/* hyperbolic */
MATH1(sinh, sinh)
MATH1(cosh, cosh)
MATH1(tanh, tanh)
MATH1(asinh, asinh)
MATH1(acosh, acosh)
MATH1(atanh, atanh)

/* exponential / logarithmic — expm1/log1p stay accurate near 0 */
MATH1(exp, exp)
MATH1(expm1, expm1)
MATH1(log, log)
MATH1(log10, log10)
MATH1(log2, log2)
MATH1(log1p, log1p)

/* statistical / special functions */
MATH1(erf, erf)         /* Gaussian error function */
MATH1(erfc, erfc)       /* complementary error function, 1 - erf(x) */
MATH1(gamma, tgamma)    /* Γ(x), the generalized factorial */
MATH1(loggamma, lgamma) /* log|Γ(x)|, stable for large x */

MATH2(pow, pow)
MATH2(atan2, atan2)     /* note arg order: (atan2 y x) */
MATH2(hypot, hypot)     /* sqrt(x*x + y*y) without overflow */

static Cljc *prim_round(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    /* Java's Math.round: floor(x + 0.5) — round half toward +infinity
     * (so -2.5 → -2), not C llround's round-half-away-from-zero. */
    return mk_int((int64_t)floor(as_num(argv[0]) + 0.5));
}

static Cljc *prim_math_abs(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_DOUBLE) return mk_double(fabs(v->as.d));
    /* unsigned negate avoids UB on INT64_MIN (returns INT64_MIN, as Java does) */
    return mk_int(v->as.i < 0 ? (int64_t)(0u - (uint64_t)v->as.i) : v->as.i);
}

static Cljc *prim_rand(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    double r = (double)rand() / ((double)RAND_MAX + 1.0);
    if (nargs > 0)
        return mk_double(r * as_num(argv[0]));
    return mk_double(r);
}

static Cljc *prim_rand_int(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t n = as_int(argv[0], "rand-int");
    if (n <= 0) return mk_int(0);
    return mk_int((int64_t)((double)rand() / ((double)RAND_MAX + 1.0) * (double)n));
}


/* ── bit operations (int64) ── */

#define BIT_FOLD(NAME, LABEL, OP) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        int64_t v = as_int(argv[0], LABEL); \
        for (int bi_ = 1; bi_ < nargs; bi_++) v = v OP as_int(argv[bi_], LABEL); \
        return mk_int(v); \
    }

BIT_FOLD(bit_and, "bit-and", &)
BIT_FOLD(bit_or,  "bit-or",  |)
BIT_FOLD(bit_xor, "bit-xor", ^)

static Cljc *prim_bit_not(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(~as_int(argv[0], "bit-not"));
}

static Cljc *prim_bit_and_not(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t v = as_int(argv[0], "bit-and-not");
    for (int bi_ = 1; bi_ < nargs; bi_++) v &= ~as_int(argv[bi_], "bit-and-not");
    return mk_int(v);
}

static Cljc *prim_bsl(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int((int64_t)((uint64_t)as_int(argv[0], "bit-shift-left")
                            << (as_int(argv[1], "bit-shift-left") & 63)));
}

static Cljc *prim_bsr(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(as_int(argv[0], "bit-shift-right")
                  >> (as_int(argv[1], "bit-shift-right") & 63));
}

static Cljc *prim_ubsr(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int((int64_t)((uint64_t)as_int(argv[0], "unsigned-bit-shift-right")
                            >> (as_int(argv[1], "unsigned-bit-shift-right") & 63)));
}

static Cljc *prim_bit_test(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_bool((as_int(argv[0], "bit-test") >> (as_int(argv[1], "bit-test") & 63)) & 1);
}

static Cljc *prim_bit_set(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(as_int(argv[0], "bit-set") | ((int64_t)1 << (as_int(argv[1], "bit-set") & 63)));
}

static Cljc *prim_bit_clear(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(as_int(argv[0], "bit-clear") & ~((int64_t)1 << (as_int(argv[1], "bit-clear") & 63)));
}

static Cljc *prim_bit_flip(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(as_int(argv[0], "bit-flip") ^ ((int64_t)1 << (as_int(argv[1], "bit-flip") & 63)));
}

/* (char 97) → \a — code point to char. A char passes through; a 1-char
 * string is accepted leniently (its first byte). */
static Cljc *prim_char(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_CHAR) return v;
    if (v != NIL && v->tag == CLJC_STRING && v->as.str[0])
        return mk_char((unsigned char)v->as.str[0]);
    return mk_char((int32_t)as_int(v, "char"));
}

/* (str/replace-first s match repl) — first occurrence only. A #"..."/re-pattern
 * match does a single regex replacement ($1..$9 in repl); a string is literal. */
static Cljc *prim_replace_first(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    if (argv[1] != NIL && argv[1]->tag == CLJC_STRING && argv[1]->meta) {
        char *s = as_str(argv[0], "str/replace-first");
        char *pat = as_str(argv[1], "str/replace-first");
        Cljc *replv = argv[2];
        bool is_fn = replv != NIL && (replv->tag == CLJC_FN || replv->tag == CLJC_NATIVE);
        char *repl = is_fn ? NULL : as_str(argv[2], "str/replace-first");
        Rx *pool; int ngroups;
        Rx *prog = rx_compile(pat, &pool, &ngroups);
        rx_str_begin = s;
        SBuf out = {0};
        sb_grow(&out, 1); out.data[0] = '\0';
        const char *p = s;
        bool done = false;
        while (!done) {                            /* note: tests at the EOS NUL too */
            rx_reset_caps();
            rx_cap_s[0] = p;
            if (rx_m(prog, p)) {
                const char *mend = rx_match_end;
                rx_cap_e[0] = mend;
                if (is_fn) {
                    Cljc *mv = rx_match_value(ngroups);
                    sb_puts(&out, as_str(apply(env, replv, &mv, 1), "replace fn"));
                    rx_str_begin = s;
                } else rx_subst(&out, repl);
                if (mend > p) p = mend;
                else if (*p) { sb_putc(&out, *p); p++; }  /* empty match mid-string */
                done = true;
            } else if (*p) { sb_putc(&out, *p); p++; }
            else done = true;                      /* end of string, no match */
        }
        sb_puts(&out, p);                          /* tail after the match */
        free(pool);
        Cljc *res = mk_str(out.data, out.len);
        free(out.data);
        return res;
    }
    char *s = as_str(argv[0], "str/replace-first");
    char *m = as_str(argv[1], "str/replace-first");
    char *r = as_str(argv[2], "str/replace-first");
    char *hit = *m ? strstr(s, m) : NULL;
    if (!hit) return argv[0];
    SBuf sb = {0};
    sb_grow(&sb, 1);
    sb.data[0] = '\0';
    size_t pre = (size_t)(hit - s), ml = strlen(m), rl = strlen(r);
    sb_grow(&sb, pre + rl + strlen(hit + ml));
    memcpy(sb.data + sb.len, s, pre); sb.len += pre;
    memcpy(sb.data + sb.len, r, rl); sb.len += rl;
    strcpy(sb.data + sb.len, hit + ml); sb.len += strlen(hit + ml);
    Cljc *out = mk_str(sb.data, sb.len);
    free(sb.data);
    return out;
}

/* ── read-string / eval / peek / pop / empty ── */

static Cljc *prim_parse_long(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "parse-long");
    if (isspace((unsigned char)s[0])) return NIL;  /* no leading whitespace (Clojure) */
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') return NIL;  /* whole string or nil */
    if (errno == ERANGE) return NIL;           /* overflow: nil, like Clojure */
    return mk_int((int64_t)v);
}

static Cljc *prim_parse_double(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "parse-double");
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return NIL;
    return mk_double(v);
}

static Cljc *prim_read_string(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    const char *p = as_str(argv[0], "read-string");
    /* Nested reads (load-file, require shims) must not disturb the host
     * file's line tracking — else later top-level forms inherit bogus
     * line meta and error carets point at the wrong line. */
    int save_line = rd_line;
    const char *save_start = rd_line_start;
    rd_line = 1;
    rd_line_start = p;
    Cljc *form = read_form(&p);
    rd_line = save_line;
    rd_line_start = save_start;
    return form ? form : NIL;
}

static Cljc *prim_eval(CljcEnv *env, Cljc **argv, int nargs) {
    /* Like Clojure: evaluates with no access to local lexical scope. */
    return eval(env_root(env), argv[0]);
}

static Cljc *prim_peek(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return NIL;
    if (v->tag == CLJC_LIST) return v->as.cons.head;          /* list: first */
    if (v->tag == CLJC_VECTOR)                                 /* vector: last */
        return vec_len(v) ? vec_nth(v, vec_len(v) - 1) : NIL;
    cljc_error("peek: %s is not a list or vector", val_type_name(v));
    return NIL;
}

static Cljc *prim_pop(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_LIST) return v->as.cons.tail;
    if (v != NIL && v->tag == CLJC_VECTOR) {
        uint32_t cnt = v->as.vec.count;
        if (cnt == 0) cljc_error("pop: empty vector");
        if (v->as.vec.taillen > 1)  /* fast path: shrink the tail */
            return vec_cell(v->as.vec.root, v->as.vec.shift, cnt - 1,
                            v->as.vec.tail, (uint8_t)(v->as.vec.taillen - 1), NULL);
        /* tail would empty: rebuild (rare — every 32nd pop) */
        Cljc *nv = mk_empty_vec();
        for (size_t i = 0; i + 1 < cnt; i++) nv = vec_conj1(nv, vec_nth(v, i));
        return nv;
    }
    cljc_error("pop: %s is not a list or vector", val_type_name(v));
    return NIL;
}

static Cljc *prim_empty(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return NIL;
    switch (v->tag) {
        case CLJC_LIST: case CLJC_LAZY: case CLJC_EMPTY: return EMPTY;  /* (empty '(1 2)) => () */
        case CLJC_VECTOR: return mk_empty_vec();
        case CLJC_MAP:    return mk_map();
        case CLJC_SET:    return mk_set();
        case CLJC_SORTED: return mk_sorted(v->as.sorted.cmp, v->as.sorted.is_map);
        default:          return NIL;
    }
}

/* ── coroutine engine (Lua-style: new / resume / yield / status) ──
 * The save/restore dance: a SWITCH-IN (resume) installs the target's vstack
 * segment + scalar state then swapcontexts; a SWITCH-OUT (yield / die) stashes
 * the leaving coro's segment, installs the resumer's state, then swapcontexts.
 * So each direction restores exactly the context it jumps to. */
#ifdef CLJC_HAVE_CORO
#define CORO_STACK_SIZE (1u << 20)   /* 1 MiB C stack per coro (eval recurses) */
#define CORO_VSTACK_CAP (1u << 14)   /* 16K operand slots per coro (128 KiB) */

/* Save the active execution state (the globals) into a coro's slot, or main's.
 * The vstack trio is part of this: switching coros swaps the whole value stack
 * so each coro keeps its own stable array (absolute argv pointers stay valid). */
static void state_save(Coro *c) {
    if (c) { c->s_err_top = err_top; c->s_cur_exc = cur_exc; c->s_eval_sp = eval_sp;
             c->s_vstack = vstack; c->s_vsp = vsp; c->s_vstack_cap = vstack_cap; }
    else   { main_s_err_top = err_top; main_s_cur_exc = cur_exc; main_s_eval_sp = eval_sp;
             main_s_vstack = vstack; main_s_vsp = vsp; main_s_vstack_cap = vstack_cap; }
}
static void state_load(Coro *c) {
    if (c) { err_top = c->s_err_top; cur_exc = c->s_cur_exc; eval_sp = c->s_eval_sp;
             vstack = c->s_vstack; vsp = c->s_vsp; vstack_cap = c->s_vstack_cap; }
    else   { err_top = main_s_err_top; cur_exc = main_s_cur_exc; eval_sp = main_s_eval_sp;
             vstack = main_s_vstack; vsp = main_s_vsp; vstack_cap = main_s_vstack_cap; }
}

/* Switch INTO target, passing val. Runs on the resumer's context. */
static Cljc *coro_resume(Coro *target, Cljc *val) {
    Coro *resumer = coro_current;
    char anchor;
    if (resumer) resumer->saved_sp = &anchor; else coro_main_saved_sp = &anchor;
    state_save(resumer);                 /* stash resumer's globals (incl. its vstack) */
    if (resumer) resumer->status = CORO_NORMAL;
    target->resumer = resumer;
    target->xfer = val;
    coro_current = target; target->status = CORO_RUNNING;
    state_load(target);                  /* install target's vstack + scalars */
    swapcontext(resumer ? &resumer->ctx : &coro_main_ctx, &target->ctx);
    /* ← target yielded or died back to us; it already restored our state. */
    return target->xfer;
}

/* Switch OUT to the resumer with the given exit status (SUSPENDED or DEAD). */
static void coro_switch_out(Coro *self, CoroStatus st, void *sp) {
    self->saved_sp = sp;
    state_save(self);                    /* keep self's vstack as-is for resume */
    self->status = st;
    Coro *resumer = self->resumer;
    coro_current = resumer;
    if (resumer) resumer->status = CORO_RUNNING;
    state_load(resumer);
}

static Cljc *coro_yield(Cljc *val) {
    Coro *self = coro_current;
    if (!self) cljc_error("coro/yield outside a coroutine");
    char anchor;
    self->xfer = val;
    coro_switch_out(self, CORO_SUSPENDED, &anchor);
    swapcontext(&self->ctx, self->resumer ? &self->resumer->ctx : &coro_main_ctx);
    /* ← resumed again; coro_resume restored our vstack + state. */
    return self->xfer;
}

static void coro_trampoline(void) {
    Coro *self = coro_current;
    ErrFrame base; base.prev = NULL; base.vsp_save = vsp; base.esp_save = eval_sp;
    err_top = &base;
    if (setjmp(base.jb) == 0) {
        self->xfer = apply(gc_root_envs[0], self->thunk, NULL, 0);
        self->error = NULL;
    } else {
        vsp = base.vsp_save; eval_sp = base.esp_save;
        self->error = cur_exc ? cur_exc : mk_str(err_msg, strlen(err_msg));
        self->xfer = self->error;
    }
    coro_switch_out(self, CORO_DEAD, NULL);
    setcontext(self->resumer ? &self->resumer->ctx : &coro_main_ctx);
    /* unreachable */
}

static Cljc *coro_new(Cljc *thunk) {
    Cljc *cell = alloc(CLJC_CORO);     /* as.coro zeroed → GC-safe before attach */
    Coro *co = xmalloc(sizeof(Coro));
    memset(co, 0, sizeof *co);
    co->self = cell; co->thunk = thunk; co->xfer = NIL; co->error = NULL;
    co->s_cur_exc = NIL; co->status = CORO_NEW;
    co->s_vstack = xmalloc(sizeof(Cljc *) * CORO_VSTACK_CAP);   /* this coro's own vstack */
    co->s_vsp = 0;
    co->s_vstack_cap = CORO_VSTACK_CAP;
    co->stack = xmalloc(CORO_STACK_SIZE);
    co->stack_size = CORO_STACK_SIZE;
    co->stack_top = co->stack + CORO_STACK_SIZE;
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp = co->stack;
    co->ctx.uc_stack.ss_size = CORO_STACK_SIZE;
    co->ctx.uc_link = NULL;
    makecontext(&co->ctx, coro_trampoline, 0);
    cell->as.coro = co;
    return cell;
}

static Cljc *prim_coro_new(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 1) cljc_error("coro/new: needs a thunk");
    Cljc *f = argv[0];
    if (!(f->tag == CLJC_FN || f->tag == CLJC_NATIVE))
        cljc_error("coro/new: argument must be a function");
    return coro_new(f);
}
static Cljc *prim_coro_resume(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 1 || argv[0]->tag != CLJC_CORO) cljc_error("coro/resume: not a coroutine");
    Coro *co = argv[0]->as.coro;
    if (co->status == CORO_DEAD) cljc_error("coro/resume: coroutine is dead");
    if (co->status == CORO_RUNNING || co->status == CORO_NORMAL)
        cljc_error("coro/resume: coroutine is already running");
    Cljc *r = coro_resume(co, nargs > 1 ? argv[1] : NIL);
    if (co->status == CORO_DEAD && co->error) cljc_throw_value(co->error);  /* propagate */
    return r;
}
static Cljc *prim_coro_yield(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return coro_yield(nargs > 0 ? argv[0] : NIL);
}
static Cljc *prim_coro_status(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 1 || argv[0]->tag != CLJC_CORO) cljc_error("coro/status: not a coroutine");
    const char *s;
    switch (argv[0]->as.coro->status) {
        case CORO_NEW: s = "new"; break;
        case CORO_SUSPENDED: s = "suspended"; break;
        case CORO_RUNNING: s = "running"; break;
        case CORO_NORMAL: s = "normal"; break;
        default: s = "dead"; break;
    }
    return mk_kw(intern(s, strlen(s)));
}
static Cljc *prim_coro_alive(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs < 1 || argv[0]->tag != CLJC_CORO) cljc_error("coro/alive?: not a coroutine");
    return argv[0]->as.coro->status == CORO_DEAD ? FALSE : TRUE;
}
#else  /* no coroutine support (e.g. Windows) */
static Cljc *prim_coro_new(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)argv; (void)nargs; cljc_error("coroutines unavailable on this platform");
}
static Cljc *prim_coro_resume(CljcEnv *env, Cljc **argv, int nargs) { return prim_coro_new(env, argv, nargs); }
static Cljc *prim_coro_yield(CljcEnv *env, Cljc **argv, int nargs) { return prim_coro_new(env, argv, nargs); }
static Cljc *prim_coro_status(CljcEnv *env, Cljc **argv, int nargs) { return prim_coro_new(env, argv, nargs); }
static Cljc *prim_coro_alive(CljcEnv *env, Cljc **argv, int nargs) { return prim_coro_new(env, argv, nargs); }
#endif  /* CLJC_HAVE_CORO */

/* ── sh / FFI (the s7 cload model: generate glue C, compile, dlopen) ── */

static Cljc *prim_sh(CljcEnv *env, Cljc **argv, int nargs) {
    /* (sh "cmd") => {:exit n :out "captured stdout+stderr"} */
    (void)env;
    char *cmd = as_str(argv[0], "sh");
    SBuf full = {0};
    sb_puts(&full, cmd);
    sb_puts(&full, " 2>&1");
    FILE *p = popen(full.data, "r");
    free(full.data);
    if (!p) cljc_error("sh: popen failed");
    SBuf out = {0};
    sb_grow(&out, 1); out.data[0] = '\0';
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        sb_grow(&out, n);
        memcpy(out.data + out.len, buf, n);
        out.len += n; out.data[out.len] = '\0';
    }
    int status = pclose(p);
    Cljc *m = mk_map();
    m = map_assoc(m, mk_kw(intern("exit", 4)), mk_int(status == -1 ? -1 : WEXITSTATUS(status)));
    Cljc *s = mk_str(out.data, out.len);
    free(out.data);
    m = map_assoc(m, mk_kw(intern("out", 3)), s);
    return m;
}

/* Vtable handed to FFI modules — generated glue marshals exclusively
 * through these, so modules need no cljc symbols or headers. Append-only:
 * reordering breaks every compiled module. */
typedef struct {
    void *(*mk_int)(long long);
    void *(*mk_double)(double);
    void *(*mk_str)(const char *);
    void *(*nil)(void);
    long long (*as_int)(void *);
    double (*as_double)(void *);
    const char *(*as_str)(void *);
    void *(*nth_arg)(void *args, int i);
    void (*def_native)(void *env, const char *name, void *(*fn)(void *, void *, int));
    void (*error)(const char *msg);
    /* Appended for struct-by-value marshalling (raylib et al.): build a cljc
     * vector from struct fields (returns), and read a field out of a passed
     * vector (args). Append-only — never reorder the slots above. */
    void *(*mk_vec)(void **items, int n);
    void *(*nth_elem)(void *vec, int i);
} CljcFfiApi;

static void *fa_mk_int(long long i) { return mk_int((int64_t)i); }
static void *fa_mk_double(double d) { return mk_double(d); }
static void *fa_mk_str(const char *s) { return s ? mk_str(s, strlen(s)) : NIL; }  /* NULL => nil */
static void *fa_nil(void) { return NIL; }
static long long fa_as_int(void *v) { return (long long)as_int((Cljc *)v, "ffi"); }
static double fa_as_double(void *v) { return as_num((Cljc *)v); }
static const char *fa_as_str(void *v) { return as_str((Cljc *)v, "ffi"); }
static void *fa_nth_arg(void *args, int i) {
    return ((Cljc **)args)[i];   /* glue guards its own arity */
}
static void fa_def_native(void *env, const char *name, void *(*fn)(void *, void *, int)) {
    cljc_define_native((CljcEnv *)env, name, (CljcNativeFn)fn);
}
static void fa_error(const char *msg) { cljc_error("%s", msg); }
static void *fa_mk_vec(void **items, int n) { return mk_vector((Cljc **)items, (size_t)n); }
static void *fa_nth_elem(void *vec, int i) { return vec_nth((Cljc *)vec, (size_t)i); }

static CljcFfiApi ffi_api = {
    fa_mk_int, fa_mk_double, fa_mk_str, fa_nil,
    fa_as_int, fa_as_double, fa_as_str, fa_nth_arg, fa_def_native, fa_error,
    fa_mk_vec, fa_nth_elem,
};

static Cljc *prim_ffi_load(CljcEnv *env, Cljc **argv, int nargs) {
    /* (ffi-load* "/path/mod.so") — dlopen + call cljc_module_init. */
    char *path = as_str(argv[0], "ffi-load*");
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) cljc_error("ffi-load*: %s", dlerror());
    void (*init)(void *, CljcFfiApi *) =
        (void (*)(void *, CljcFfiApi *))dlsym(h, "cljc_module_init");
    if (!init) cljc_error("ffi-load*: no cljc_module_init in %s", path);
    init(env_root(env), &ffi_api);
    return TRUE;
}

/* ───── Public C API ─────────────────────────────────────────────────── */

CljcEnv *cljc_new_env(void);
Cljc    *cljc_eval_string(CljcEnv *env, const char *src);
void     cljc_print(Cljc *v);
void     cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn);
void     cljc_set_stack_base(void *p);
void     cljc_set_embedded_files(const CljcEmbeddedFile *files, int n);

void cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn) {
    env_define_root(env_root(env), intern(name, strlen(name)), mk_native(fn));
}

/* Register bundled .clj dependencies so slurp/require/load-file find them
 * without a filesystem. Call before cljc_eval_string. */
void cljc_set_embedded_files(const CljcEmbeddedFile *files, int n) {
    cljc_embedded = files; cljc_n_embedded = n;
}

/* Record the high-water mark of the C stack for conservative root scanning.
 * Call with the address of a local near the top of the thread (e.g. &argc in
 * main) before evaluating anything. Safe to call repeatedly — keeps the
 * highest address seen (downward-growing stacks). */
void cljc_set_stack_base(void *p) {
    if (!gc_stack_base || p > gc_stack_base) {
        gc_stack_base = p;
        stack_floor_init((const char *)p);   /* recompute the C-stack overflow floor */
    }
}

/* Core functions and macros written in cljc itself. Evaluated once at env
 * creation — every line here is one we don't maintain as a C primitive. */
static const char *PRELUDE =
    "(defn identity [x] x)\n"
    "(defn some? [x] (not (nil? x)))\n"
    "(defn not= [& args] (not (apply = args)))\n"
    "(defn even? [n] (zero? (mod n 2)))\n"
    "(defn odd? [n] (not (even? n)))\n"
    "(defn abs [x] (if (neg? x) (- x) x))\n"
    "(defn max [x & xs] (reduce (fn [a b] (if (> a b) a b)) x xs))\n"
    "(defn min [x & xs] (reduce (fn [a b] (if (< a b) a b)) x xs))\n"
    "(defn complement [f] (fn [& args] (not (apply f args))))\n"
    "(defn constantly [x] (fn [& args] x))\n"
    "(defn comp [& fs]\n"
    "  (if (empty? fs)\n"
    "    identity\n"
    "    (reduce (fn [f g] (fn [& args] (f (apply g args)))) fs)))\n"
    "(defn partial [f & pre] (fn [& args] (apply f (concat pre args))))\n"
    "(defn into [to from]\n"
    "  (if (vector? to)\n"
    "    (persistent! (reduce conj! (transient to) (seq from)))\n"
    "    (reduce conj to from)))\n"
    "(defn mapv\n"
    "  ([f coll] (apply vector (map f coll)))\n"
    "  ([f c1 c2] (apply vector (map f c1 c2)))\n"
    "  ([f c1 c2 & more] (apply vector (apply map f c1 c2 more))))\n"
    "(defn filterv [f coll] (apply vector (filter f coll)))\n"
    "(defn repeat [n x] (map (constantly x) (range n)))\n"
    "(defn nthrest [coll n] (drop n coll))\n"
    "(defn split-at [n coll] [(take n coll) (drop n coll)])\n"
    "(defn str/split-lines [s] (re-split s \"\\r?\\n\"))\n"
    "(defmacro if-not [test then & else] `(if (not ~test) ~then ~@else))\n"
    "(defmacro fn* [& body] (cons 'fn body))\n"   /* fn* === fn (cljc fn is the special form) */
    /* (. target member ..) interop: (. Class (m a)) -> (Class/m a) when target is
       a class (dotted or Capitalized), else instance (.m target a). */
    "(defmacro . [target member & args]\n"
    "  (let [m (if (seq? member) member (cons member args))\n"
    "        s (str target)\n"
    "        c (when (pos? (count s)) (int (first s)))\n"
    "        static? (and (symbol? target) (or (str/includes? s \".\") (and c (>= c 65) (<= c 90))))]\n"
    "    (if static?\n"
    "      (cons (symbol (str s \"/\" (first m))) (rest m))\n"
    "      (cons (symbol (str \".\" (first m))) (cons target (rest m))))))\n"
    /* (.. x a (b c) ..) threads interop: (.. x a b) => (. (. x a) b) */
    "(defmacro .. [x & forms] (reduce (fn [acc f] (list '. acc f)) x forms))\n"
    "(defn .getClass [x] (type x))\n"
    "(defn .isInstance [k o] (isa? (type o) k))\n"
    "(defmacro let* [& body] (cons 'let body))\n"
    "(defmacro loop* [& body] (cons 'loop body))\n"
    "(defmacro when-not [test & body] `(when (not ~test) ~@body))\n"
    /* locking: cljc is single-threaded, so it just runs the body */
    "(defmacro locking [_ & body] `(do ~@body))\n"
    "(defmacro -> [x & forms]\n"
    "  (loop [x x forms forms]\n"
    "    (if (empty? forms)\n"
    "      x\n"
    "      (let [form (first forms)\n"
    "            threaded (if (list? form)\n"
    "                       (cons (first form) (cons x (rest form)))\n"
    "                       (list form x))]\n"
    "        (recur threaded (rest forms))))))\n"
    "(defmacro ->> [x & forms]\n"
    "  (loop [x x forms forms]\n"
    "    (if (empty? forms)\n"
    "      x\n"
    "      (let [form (first forms)\n"
    "            threaded (if (list? form)\n"
    "                       (concat form (list x))\n"
    "                       (list form x))]\n"
    "        (recur threaded (rest forms))))))\n"
    /* seq utilities */
    "(defn next [coll] (seq (rest coll)))\n"
    "(defn mapcat [f coll] (apply concat (map f coll)))\n"
    "(defn remove [pred coll] (filter (complement pred) coll))\n"
    "(defn keep [f coll] (filter some? (map f coll)))\n"
    "(defn butlast [coll] (reverse (rest (reverse coll))))\n"
    "(defn every? [pred coll]\n"
    "  (loop [s (seq coll)]\n"
    "    (cond (nil? s) true\n"
    "          (pred (first s)) (recur (next s))\n"
    "          :else false)))\n"
    "(defn some [pred coll]\n"
    "  (loop [s (seq coll)]\n"
    "    (if s\n"
    "      (let [r (pred (first s))]\n"
    "        (if r r (recur (next s))))\n"
    "      nil)))\n"
    "(defn take-while [pred coll]\n"
    "  (loop [s (seq coll) acc (list)]\n"
    "    (if (and s (pred (first s)))\n"
    "      (recur (next s) (cons (first s) acc))\n"
    "      (reverse acc))))\n"
    "(defn drop-while [pred coll]\n"
    "  (loop [s (seq coll)]\n"
    "    (if (and s (pred (first s))) (recur (next s)) s)))\n"
    "(defn interleave [c1 c2]\n"
    "  (loop [a (seq c1) b (seq c2) acc (list)]\n"
    "    (if (and a b)\n"
    "      (recur (next a) (next b) (cons (first b) (cons (first a) acc)))\n"
    "      (reverse acc))))\n"
    "(defn interpose [sep coll] (rest (mapcat (fn [x] (list sep x)) coll)))\n"
    "(defn partition\n"
    "  ([n coll] (partition n n coll))\n"
    "  ([n step coll]\n"
    "   (loop [s (seq coll) acc (list)]\n"
    "     (let [chunk (take n s)]\n"
    "       (if (< (count chunk) n)\n"
    "         (reverse acc)\n"
    "         (recur (seq (drop step s)) (cons chunk acc))))))\n"
    "  ([n step pad coll]\n"          /* pad the final short partition (Clojure's 4-arity) */
    "   (loop [s (seq coll) acc (list)]\n"
    "     (if s\n"
    "       (let [chunk (take n s)]\n"
    "         (if (= n (count chunk))\n"
    "           (recur (seq (drop step s)) (cons chunk acc))\n"
    "           (reverse (cons (take n (concat chunk pad)) acc))))\n"
    "       (reverse acc)))))\n"
    "(defn distinct [coll]\n"
    "  (reverse (loop [s (seq coll) acc (list)]\n"
    "    (if s\n"
    "      (let [x (first s)]\n"
    "        (recur (next s) (if (some (fn [y] (= x y)) acc) acc (cons x acc))))\n"
    "      acc))))\n"
    "(defn group-by [f coll]\n"
    "  (reduce (fn [m x] (let [k (f x)] (assoc m k (conj (get m k []) x)))) {} coll))\n"
    "(defn frequencies [coll]\n"
    "  (reduce (fn [m x] (assoc m x (inc (get m x 0)))) {} coll))\n"
    "(defn juxt [& fs] (fn [& args] (mapv (fn [f] (apply f args)) fs)))\n"
    "(defn sort-by\n"
    "  ([f coll] (sort (fn [a b] (< (compare (f a) (f b)) 0)) coll))\n"
    /* comparator arity: cmp may be a boolean pred (>) or 3-way (compare) */
    "  ([f cmp coll]\n"
    "   (sort (fn [a b] (let [r (cmp (f a) (f b))]\n"
    "                     (if (number? r) (neg? r) r))) coll)))\n"
    /* associative utilities */
    "(defn update [m k f & args] (assoc m k (apply f (get m k) args)))\n"
    "(defn get-in\n"
    "  ([m ks] (reduce get m ks))\n"
    "  ([m ks d] (let [r (reduce (fn [acc k] (if (nil? acc) nil (get acc k))) m ks)]\n"
    "              (if (nil? r) d r))))\n"
    "(defn assoc-in [m ks v]\n"
    "  (let [k (first ks) r (rest ks)]\n"
    "    (if (empty? r)\n"
    "      (assoc m k v)\n"
    "      (assoc m k (assoc-in (get m k {}) r v)))))\n"
    "(defn update-in [m ks f & args]\n"
    "  (assoc-in m ks (apply f (get-in m ks) args)))\n"
    "(defn select-keys [m ks]\n"
    "  (reduce (fn [acc k] (if (contains? m k) (assoc acc k (get m k)) acc)) {} ks))\n"
    /* string helpers */
    "(defn str/join\n"
    "  ([coll] (apply str coll))\n"
    "  ([sep coll] (if (empty? coll) \"\"\n"
    "                  (reduce (fn [a b] (str a sep b)) (str (first coll)) (rest coll)))))\n"
    /* control-flow macros */
    "(defmacro if-let [bindings then & else]\n"
    "  `(let [t# ~(nth bindings 1)]\n"
    "     (if t# (let [~(nth bindings 0) t#] ~then) ~@else)))\n"
    "(defmacro when-let [bindings & body]\n"
    "  `(let [t# ~(nth bindings 1)]\n"
    "     (when t# (let [~(nth bindings 0) t#] ~@body))))\n"
    "(defmacro when-first [bindings & body]\n"   /* (when-first [x coll] ..) */
    "  `(when-let [xs# (seq ~(nth bindings 1))]\n"
    "     (let [~(nth bindings 0) (first xs#)] ~@body)))\n"
    "(defmacro dotimes [bindings & body]\n"
    "  `(loop [~(nth bindings 0) 0]\n"
    "     (when (< ~(nth bindings 0) ~(nth bindings 1))\n"
    "       ~@body\n"
    "       (recur (inc ~(nth bindings 0))))))\n"
    /* multi-binding doseq nests: (doseq [a as b bs] ...) iterates b per a */
    "(defmacro doseq [bindings & body]\n"
    "  (let [inner (if (> (count bindings) 2)\n"
    "                (list (cons 'doseq (cons (vec (drop 2 bindings)) body)))\n"
    "                body)]\n"
    "    `(loop [s# (seq ~(nth bindings 1))]\n"
    "       (when s#\n"
    "         (let [~(nth bindings 0) (first s#)] ~@inner)\n"
    "         (recur (next s#))))))\n"
    "(defmacro while [test & body]\n"
    "  `(loop [] (when ~test ~@body (recur))))\n"
    /* case: a (v1 v2 ...) test matches any member (Clojure semantics);
     * no match and no default throws No matching clause. */
    "(defmacro case [e & clauses]\n"
    "  (let [g (gensym \"case\")\n"
    "        pred (fn [t]\n"
    "               (if (and (seq? t) (seq t))\n"   /* a LIST/seq clause key (incl. a lazy seq from a macro) is multi-constant; a vector is a single constant */
    "                 (cons 'or (map (fn [v] (list '= g (list 'quote v))) t))\n"
    "                 (list '= g (list 'quote t))))]\n"
    "    (list 'let [g e]\n"
    "      (cons 'cond\n"
    "        (loop [cs clauses acc (list)]\n"
    "          (if (empty? cs)\n"
    "            (reverse (cons (list 'throw (list 'ex-info\n"
    "                                             (list 'str \"No matching clause: \" (list 'pr-str g))\n"
    "                                             {}))\n"
    "                           (cons :else acc)))\n"
    "            (if (empty? (rest cs))\n"
    "              (reverse (cons (first cs) (cons :else acc)))\n"
    "              (recur (rest (rest cs))\n"
    "                     (cons (first (rest cs))\n"
    "                           (cons (pred (first cs)) acc))))))))))\n"
    "(defmacro for [bindings body]\n"
    "  (if (empty? bindings)\n"
    "    `(list ~body)\n"
    "    (let [k (nth bindings 0) v (nth bindings 1) more (vec (drop 2 bindings))]\n"
    "      (cond\n"
    "        (= k :when) `(if ~v (for ~more ~body) (list))\n"
    "        (= k :let)  `(let ~v (for ~more ~body))\n"
    "        :else       `(mapcat (fn [~k] (for ~more ~body)) ~v)))))\n"
    /* batch 5-lite */
    "(defmacro comment [& _] nil)\n"
    "(defmacro ->clerk-only [& _] nil)\n"   /* notebook-only code (mentat.clerk-utils): dropped */
    "(defmacro ->clerk [& _] nil)\n"
    /* taoensso.timbre logging — no-ops (Emmy warns during simplify) */
    "(defmacro taoensso.timbre/warn [& _] nil) (defmacro taoensso.timbre/info [& _] nil)\n"
    "(defmacro taoensso.timbre/debug [& _] nil) (defmacro taoensso.timbre/error [& _] nil)\n"
    "(defmacro taoensso.timbre/trace [& _] nil) (defmacro taoensso.timbre/spy [& body] (last body))\n"
    "(defmacro taoensso.timbre/warnf [& _] nil) (defmacro taoensso.timbre/errorf [& _] nil)\n"
    "(defmacro defn- [name & body] `(defn ~name ~@body))\n"
    "(defn vary-meta [x f & args] (with-meta x (apply f (meta x) args)))\n"
    /* c resolves to a type keyword (deftype name, or a host-class stub/keyword);
       compare against the value's (type x). Unknown host classes are keywords
       that simply won't match, so instance? on them is false, as before. */
    /* a protocol resolves to a vector of its method symbols (see defprotocol);
       instance? against one means "does x's type implement it" = satisfies?.
       A host/deftype class resolves to a type keyword: plain type equality. */
    /* protocol (a vector of method syms) -> satisfies?; a class keyword -> the
       type ISA the class, so a supertype like Number matches an int/double too. */
    "(defn instance? [c x] (if (vector? c) (satisfies? c x) (isa? (type x) c)))\n"
    "(defn nnext [s] (next (next s)))\n"
    "(defn find [m k] (when (contains? m k) [k (get m k)]))\n"
    "(defn reduced [x] (cons '**reduced** (cons x nil)))\n"
    "(defn reduced? [x]\n"
    "  (and (list? x) (= '**reduced** (first x))))\n"
    "(defn unreduced [x] (if (reduced? x) (second x) x))\n"
    "(defn ensure-reduced [x] (if (reduced? x) x (reduced x)))\n"
    "(defn key [e] (first e))\n"
    "(defn subvec\n"
    "  ([v s] (subvec v s (count v)))\n"
    "  ([v s e] (vec (take (- e s) (drop s v)))))\n"
    /* defrecord registers its type here so a deftype can be told apart from a
       record (both are tagged maps in cljc). map? stays native (records, plain
       maps, AND deftypes all read as maps — instaparse relies on this). */
    "(def cljc/record-types (atom #{}))\n"
    "(def cljc/map-raw? map?)\n"
    /* coll?, though, must be FALSE for a deftype that isn't a collection (Emmy's
       structures and core.logic's LVar are tagged maps but not collections): a
       tagged map counts as a coll only when it's a record or derives a collection
       interface (Sequential / IPersistentCollection). Plain maps stay colls. */
    "(defn coll? [x]\n"
    "  (or (list? x) (vector? x) (set? x) (sorted? x) (seq? x) (sequential? x)\n"
    "      (and (cljc/map-raw? x)\n"
    "           (let [t (get x :cljc/type)]\n"
    "             (or (nil? t) (contains? (deref cljc/record-types) t)\n"
    "                 (isa? t :clojure.lang.IPersistentCollection))))))\n"
    "(defn map-entry? [x] (and (vector? x) (= 2 (count x))))\n"
    "(defn list* [& args]\n"
    "  (let [r (reverse args)]\n"
    "    (reduce (fn [acc x] (cons x acc)) (seq (first r)) (rest r))))\n"
    "(defn val [e] (second e))\n"
    "(defn ffirst [s] (first (first s)))\n"
    "(defn nfirst [s] (next (first s)))\n"
    "(defn fnext [s] (first (next s)))\n"
    "(defn dorun' [s] (dorun s))\n"
    "(def volatile! atom)\n"
    "(def vreset! reset!)\n"
    "(defmacro vswap! [v f & args] `(reset! ~v (~f @~v ~@args)))\n"
    "(defn swap-vals! [a f & args] (let [old @a] [old (apply swap! a f args)]))\n"
    "(defn reset-vals! [a v] (let [old @a] (reset! a v) [old v]))\n"
    /* a deftype (no longer map?) is sequential? when it derives Sequential —
       check the type tag directly, not map?. */
    "(defn sequential? [x] (boolean (or (list? x) (vector? x) (seq? x)\n"
    "                          (let [t (get x :cljc/type)] (and t (isa? t :clojure.lang.Sequential))))))\n"
    "(defn class [x] (if (fn? x) x (type x)))\n"   /* fn reflects on itself (below) */
    /* synthesize java.lang.reflect.Method info from cljc fn arities so libraries
       that compute arity by reflection (emmy.function) work without the JVM */
    "(defn .getDeclaredMethods [c]\n"
    "  (if (fn? c)\n"
    "    (vec (mapcat (fn [a] (let [n (first a) v (second a)]\n"
    "                           (if v [{:nm \"doInvoke\" :pt (vec (repeat n nil))} {:nm \"getRequiredArity\"}]\n"
    "                               [{:nm \"invoke\" :pt (vec (repeat n nil))}]))) (cljc/fn-arities c)))\n"
    "    []))\n"
    "(defn .getParameterTypes [m] (:pt m))\n"
    "(defn .getRequiredArity [f] (or (some (fn [a] (when (second a) (first a))) (cljc/fn-arities f)) 0))\n"
    /* regexes are strings; the :regex meta makes str/split treat them so */
    "(defn re-pattern [s] (with-meta s {:regex true}))\n"
    "(defn boolean? [x] (or (true? x) (false? x)))\n"
    "(defn nat-int? [x] (and (int? x) (>= x 0)))\n"
    /* Global ad-hoc hierarchy for derive/isa?/defmulti, like clojure.core's
     * *global-hierarchy*: {child #{direct-parents}}. isa? closes it transitively
     * and is vector-aware ([::a ::b] isa [::c ::d] elementwise), which is what
     * multimethod dispatch on multiple args needs. */
    "(def cljc/global-hierarchy (atom {}))\n"
    "(defn cljc/tag-isa? [c p]\n"
    "  (or (= c p) (boolean (some (fn [x] (cljc/tag-isa? x p)) (get (deref cljc/global-hierarchy) c)))))\n"
    "(defn isa? [c p]\n"
    "  (or (= c p) (cljc/tag-isa? c p)\n"
    "      (and (vector? c) (vector? p) (= (count c) (count p))\n"
    "           (every? identity (map isa? c p)))))\n"
    "(defn parents [t] (get (deref cljc/global-hierarchy) t))\n"
    "(defn ancestors [t]\n"
    "  (let [ps (get (deref cljc/global-hierarchy) t)]\n"
    "    (reduce (fn [acc p] (into (conj acc p) (ancestors p))) #{} ps)))\n"
    "(defn descendants [t]\n"
    "  (set (filter (fn [k] (and (not= k t) (cljc/tag-isa? k t))) (keys (deref cljc/global-hierarchy)))))\n"
    "(defn derive\n"
    "  ([t parent] (swap! cljc/global-hierarchy update t (fn [s] (conj (or s #{}) parent))) nil)\n"
    "  ([h t parent] (derive t parent)))\n"
    "(defn underive\n"
    "  ([t parent] (swap! cljc/global-hierarchy update t disj parent) nil)\n"
    "  ([h t parent] (underive t parent)))\n"
    /* resolve a method by walking the hierarchy: among method keys that dv isa?,
     * pick the most specific (the one that isa? all the other matches). */
    "(defn cljc/multi-find [t dv]\n"
    "  (let [ms (filter (fn [e] (isa? dv (key e))) t)]\n"
    "    (when (seq ms)\n"
    "      (val (or (some (fn [e] (when (every? (fn [e2] (isa? (key e) (key e2))) ms) e)) ms)\n"
    "               (first ms))))))\n"
    /* cljc's collection types ARE the host's Sequential/collection interfaces, so
       derive them — lets a (defmethod f [.. Sequential]) match a vector/list/seq
       dispatch value (e.g. Emmy's partial-derivative on a vector of selectors). */
    "(doseq [c [:clojure.lang.Sequential :clojure.lang.IPersistentVector :clojure.lang.PersistentVector\n"
    "           :clojure.lang.IPersistentCollection :clojure.lang.Indexed :clojure.lang.Associative]]\n"
    "  (derive :vector c))\n"
    "(doseq [c [:clojure.lang.Sequential :clojure.lang.ISeq :clojure.lang.IPersistentList\n"
    "           :clojure.lang.PersistentList :clojure.lang.IPersistentCollection]] (derive :list c))\n"
    "(doseq [c [:clojure.lang.Sequential :clojure.lang.ISeq :clojure.lang.LazySeq\n"
    "           :clojure.lang.IPersistentCollection]] (derive :lazy-seq c))\n"
    "(doseq [c [:clojure.lang.IPersistentMap :clojure.lang.Associative :clojure.lang.PersistentArrayMap\n"
    "           :clojure.lang.PersistentHashMap :clojure.lang.IPersistentCollection]] (derive :map c))\n"
    "(doseq [c [:clojure.lang.IPersistentSet :clojure.lang.PersistentHashSet\n"
    "           :clojure.lang.IPersistentCollection]] (derive :set c))\n"
    /* Iterable / Seqable / java.util.Map — DataScript's seqable? checks these */
    "(def Iterable :java.lang.Iterable) (def java.lang.Iterable :java.lang.Iterable)\n"
    "(def clojure.lang.Seqable :clojure.lang.Seqable) (def java.util.Map :java.util.Map)\n"
    "(doseq [t [:vector :list :lazy-seq :set]]\n"
    "  (derive t :java.lang.Iterable) (derive t :clojure.lang.Seqable))\n"
    "(derive :map :java.util.Map) (derive :map :clojure.lang.Seqable)\n"
    /* cljc's numeric types are java.lang.Number subclasses, so (instance? Number n)
       holds — Emmy's v/number? checks exactly this */
    "(doseq [t [:int :double :bigint :ratio]] (derive t :number))\n"
    "(defn every-pred [& ps]\n"
    "  (fn [& args] (every? (fn [p] (every? p args)) ps)))\n"
    "(defn some-fn [& ps]\n"
    "  (fn [& args] (or (some (fn [p] (some p args)) ps) false)))\n"
    "(defn memoize [f]\n"
    "  (let [cache (atom {})]\n"
    "    (fn [& args]\n"
    "      (let [hit (get @cache args :cljc/memo-miss)]\n"
    "        (if (= hit :cljc/memo-miss)\n"
    "          (let [v (apply f args)] (swap! cache assoc args v) v)\n"
    "          hit)))))\n"
    "(defn take-last [n coll] (drop (max 0 (- (count coll) n)) coll))\n"
    "(defn drop-last\n"
    "  ([coll] (drop-last 1 coll))\n"
    "  ([n coll] (take (max 0 (- (count coll) n)) coll)))\n"
    "(defn update-keys [m f]\n"
    "  (into {} (map (fn [kv] [(f (first kv)) (second kv)]) (seq m))))\n"
    "(defn update-vals [m f]\n"
    "  (into {} (map (fn [kv] [(first kv) (f (second kv))]) (seq m))))\n"
    /* defonce: evaluating a bare unbound symbol throws — catch it and def.
     * Keeps atoms etc. alive across re-evaluation (notebook saves, reloads). */
    "(defmacro defonce [n e] `(try ~n (catch Exception ex# (def ~n ~e))))\n"
    "(defmacro time [expr]\n"
    "  `(let [t0# (cljc/now-ms*) v# ~expr]\n"
    "     (println (str \"Elapsed time: \" (- (cljc/now-ms*) t0#) \" msecs\"))\n"
    "     v#))\n"
    /* == (numeric value-equality across the tower, 1 == 1.0) is a native prim. */
    "(defn distinct? [& xs] (= (count xs) (count (set xs))))\n"
    /* +' -' *' are real auto-promoting natives now (see prim_*q); inc'/dec'
     * are defined just below in terms of them */
    /* deftype, tolerated: defines a Name. constructor returning a plain map
     * of fields; interface method bodies are ignored. Enough for files that
     * define a type they rarely use to still load. */
    /* Real deftype: an instance is a map tagged {:cljc/type :T} (so `type` and
       protocol multimethods dispatch on it) whose MUTABLE fields are atoms.
       Method bodies are rewritten so a field read becomes (:f this) — deref'd
       for mutable fields — and (set! f v) becomes (reset! (:f this) v), then
       registered as defmethods on :T. Field metadata (^:unsynchronized-mutable
       etc.) marks a field mutable; bare fields are immutable values. */
    "(defn cljc/field-sym [f]\n"
    "  (loop [f f] (if (and (list? f) (= 'with-meta (first f))) (recur (second f)) f)))\n"
    "(defn cljc/field-mut? [f]\n"
    "  (loop [f f]\n"
    "    (if (and (list? f) (= 'with-meta (first f)))\n"
    "      (let [m (nth f 2 nil)]\n"
    "        (if (and (map? m) (or (:unsynchronized-mutable m) (:volatile-mutable m) (:mutable m)))\n"
    "          true (recur (second f))))\n"
    "      false)))\n"
    /* symbols bound by a binding pattern (so deftype-walk can drop them from
       fset — a local shadows a same-named field). */
    "(defn cljc/binding-syms [pat]\n"
    "  (cond (symbol? pat) (if (= pat '&) #{} #{pat})\n"
    "        (vector? pat) (reduce into #{} (map cljc/binding-syms pat))\n"
    "        (map? pat) (into (set (concat (:keys pat) (when (symbol? (:as pat)) [(:as pat)])))\n"
    "                         (mapcat cljc/binding-syms (filter (complement keyword?) (keys pat))))\n"
    "        :else #{}))\n"
    "(defn cljc/deftype-walk [form fset mset this]\n"
    /* macroexpand this node so macros that GENERATE field reads or (set! field
       ..) (e.g. tools.reader's update!) are rewritten too — but STOP at a set!
       form so it stays a set! for the rewrite below (set! is itself a macro). */
    "  (let [form (loop [f form]\n"
    "               (if (and (list? f) (= 'set! (first f))) f\n"
    "                 (let [e (macroexpand-1 f)] (if (= e f) f (recur e)))))]\n"
    "  (cond\n"
    "    (and (symbol? form) (contains? fset form))\n"
    "      (let [a (list (keyword (str form)) this)]\n"
    "        (if (contains? mset form) (list 'deref a) a))\n"
    "    (and (list? form) (= 'set! (first form)) (symbol? (second form))\n"
    "         (contains? mset (second form)))\n"
    "      (list 'reset! (list (keyword (str (second form))) this)\n"
    "            (cljc/deftype-walk (nth form 2) fset mset this))\n"
    /* let, let-star and loop: binding NAMES shadow fields. Walk each value with
       the fields visible at that point, then walk the body with shadowed fields
       removed, so (loop [state state] ..) keeps the loop var a local. */
    "    (and (list? form) (contains? #{'let 'let* 'loop} (first form)) (vector? (second form)))\n"
    "      (let [acc (reduce (fn [a [b v]]\n"
    "                          {:fs (reduce disj (:fs a) (cljc/binding-syms b))\n"
    "                           :bs (conj (:bs a) b (cljc/deftype-walk v (:fs a) mset this))})\n"
    "                        {:fs fset :bs []} (partition 2 (second form)))]\n"
    "        (apply list (first form) (vec (:bs acc))\n"
    "               (map (fn [x] (cljc/deftype-walk x (:fs acc) mset this)) (drop 2 form))))\n"
    "    (list? form)   (apply list (map (fn [x] (cljc/deftype-walk x fset mset this)) form))\n"
    "    (vector? form) (mapv (fn [x] (cljc/deftype-walk x fset mset this)) form)\n"
    "    (map? form)    (into {} (map (fn [kv] [(cljc/deftype-walk (first kv) fset mset this)\n"
    "                                           (cljc/deftype-walk (second kv) fset mset this)]) form))\n"
    "    :else form)))\n"
    "(defmacro deftype [tname fields & specs]\n"
    "  (let [tname (cljc/field-sym tname)\n"   /* peel ^{:doc ..} off the name */
    "        tkw   (keyword (str tname))\n"
    "        fsyms (mapv cljc/field-sym fields)\n"
    "        fset  (set fsyms)\n"
    "        mset  (set (filter some? (map (fn [f] (when (cljc/field-mut? f) (cljc/field-sym f))) fields)))\n"
    "        ms    (filter list? specs)\n"
    /* every declared marker interface / protocol (definterface -> a keyword,
       imported class -> a keyword) becomes a parent of this type, so
       (instance? IBindable x) / (sequential? x) work via the hierarchy. */
    "        ifaces (filter symbol? specs)\n"
    "        kvs   (mapcat (fn [s] [(keyword (str s)) (if (contains? mset s) (list 'atom s) s)]) fsyms)]\n"
    "    `(do\n"
    "       (def ~tname ~tkw)\n"
    "       (defn ~(symbol (str tname \".\")) ~fsyms\n"
    "         (hash-map :cljc/type ~tkw ~@kvs))\n"
    "       (def ~(symbol (str \"->\" tname)) ~(symbol (str tname \".\")))\n"
    "       ~@(map (fn [i] `(let [k# (cljc/resolve-maybe '~i)]\n"
    "                         (when (and (keyword? k#) (not (contains? cljc/no-derive-kinds k#)))\n"
    "                           (derive ~tkw k#)))) ifaces)\n"
    /* group method forms by name -> one multi-arity fn per (type,name), so a
       protocol method with several arities (e.g. IFn invoke) doesn't collide */
    "       ~@(map (fn [g]\n"
    "                (let [mname (first g)\n"
    "                      arities (map (fn [m]\n"
    "                                     (let [oparams (vec (second m)) othis (first oparams)\n"
    "                                           this (if (= othis '_) (gensym \"this\") othis)\n"
    "                                           params (assoc oparams 0 this)\n"
    "                                           body (map (fn [x] (cljc/deftype-walk x fset mset this)) (drop 2 m))]\n"
    "                                       `(~params ~@body)))\n"
    "                                   (second g))]\n"
    "                  `(cljc/reg-method! '~mname ~tkw (fn ~@arities))))\n"
    "              (group-by first ms))\n"
    "       ~tkw)))\n"
    /* PersistentQueue: a vector tagged {:cljc/queue true} — conj at the
     * back (meta survives), peek/pop at the FRONT (true FIFO; pop is
     * O(n), fine at puzzle scale). */
    "(def clojure.lang.PersistentQueue/EMPTY (with-meta [] {:cljc/queue true}))\n"
    "(defn cljc/queue? [x] (and (vector? x) (get (meta x) :cljc/queue)))\n"
    /* Java-interop shims: enough for the canonical md5 idiom and common
     * static calls to run verbatim. Method symbols (.foo) are plain
     * globals here. */
    "(defn MessageDigest/getInstance [algo]\n"
    "  (if (= (str/upper-case algo) \"MD5\") :md5\n"
    "      (throw (ex-info (str \"MessageDigest: only MD5 (got \" algo \")\") {}))))\n"
    "(defn .getBytes [s] s)\n"
    "(defn .digest [algo s] (cljc/md5* s))\n"   /* → 32-char hex string */
    "(defn .indexOf [coll x]\n"
    "  (if (string? coll)\n"
    "    (or (str/index-of coll x) -1)\n"
    "    (loop [i 0 s (seq coll)]\n"
    "      (cond (nil? s) -1\n"
    "            (= (first s) x) i\n"
    "            :else (recur (inc i) (next s))))))\n"
    /* Math/{sqrt,cbrt,pow,floor,ceil,round,abs,trunc}, the trig/hyperbolic
     * family, exp/log{,10,2,1p}/expm1, erf/erfc/gamma/loggamma, and the
     * constants Math/PI and Math/E are all already natives */
    "(def cljc/digit-chars \"0123456789abcdefghijklmnopqrstuvwxyz\")\n"
    "(defn Character/digit [c radix]\n"
    "  (let [i (str/index-of cljc/digit-chars (str/lower-case (str c)))]\n"
    "    (if (and i (< i radix)) i -1)))\n"
    "(defn Character/isWhitespace [c]\n"
    "  (let [n (int c)] (or (= n 32) (= n 9) (= n 10) (= n 13) (= n 12) (= n 11))))\n"
    "(defn Character/isDigit [c] (let [n (int c)] (and (>= n 48) (<= n 57))))\n"
    "(defn Character/valueOf [c] c)\n"
    /* java.lang.System statics */
    "(defn System/getenv ([] {}) ([k] (cljc/env* k)))\n"
    "(defn System/getProperty ([k] nil) ([k d] d))\n"
    "(defn System/exit [n] nil)\n"
    "(defn System/currentTimeMillis [] (long (cljc/now-ms*)))\n"
    /* Apache Commons Math integer gcd, used by Emmy's rational-function simplifier */
    "(defn ArithmeticUtils/gcd [a b]\n"
    "  (let [a (if (neg? a) (- a) a) b (if (neg? b) (- b) b)]\n"
    "    (loop [a a b b] (if (zero? b) a (recur b (rem a b))))))\n"
    "(defn biginteger [x] x)\n"
    "(defn .gcd [a b] (ArithmeticUtils/gcd a b))\n"
    /* stopwatch.core/start returns an elapsed-nanos fn; cljc has no real clock,
       so report 0 (Emmy's simplifier stopwatch then never times out) */
    "(defn stopwatch.core/start [] (constantly 0))\n"
    "(def cljc/uuid-counter (atom 0))\n"
    "(defn UUID/randomUUID [] (symbol (str \"uuid-\" (swap! cljc/uuid-counter inc))))\n"
    "(def cljc/rt-id (atom 0))\n"   /* clojure.lang.RT/nextID: monotonic unique ids (core.logic lvars) */
    "(defn clojure.lang.RT/nextID [] (swap! cljc/rt-id inc))\n"
    /* RT array ops (persistent-sorted-set's arrays.cljc expands aget/aset to these) */
    "(defn clojure.lang.RT/aget [a i] (aget a i))\n"
    "(defn clojure.lang.RT/aset [a i v] (aset a i v))\n"
    "(defn clojure.lang.RT/alength [a] (alength a))\n"
    "(defn clojure.lang.RT/assoc [m k v] (assoc m k v))\n"
    "(defn clojure.lang.RT/get ([m k] (get m k)) ([m k d] (get m k d)))\n"
    "(defn clojure.lang.RT/conj [c x] (conj c x))\n"
    "(defn clojure.lang.RT/count [c] (count c))\n"
    "(defn clojure.lang.RT/nth ([c i] (nth c i)) ([c i d] (nth c i d)))\n"
    "(defn clojure.lang.RT/seq [c] (seq c))\n"
    "(defn clojure.lang.RT/iter [c] (clojure.lang.RT/seq c))\n"
    "(defn clojure.lang.Numbers/compare [x y] (cond (< x y) -1 (> x y) 1 :else 0))\n"
    "(defn clojure.lang.Numbers/toRatio [x] (rationalize x))\n"   /* cljc ratios already support numerator/denominator */
    "(defn clojure.lang.Numbers/divide [x y] (/ x y))\n"
    "(defn Integer/compare [x y] (cond (< x y) -1 (> x y) 1 :else 0))\n"
    "(defn Long/compare [x y] (cond (< x y) -1 (> x y) 1 :else 0))\n"
    "(defn .compareTo [x y] (compare x y))\n"
    /* mutable java.util collections as atoms over a persistent coll (DataScript's
       serialization buffers + query_v3). cljc's (pkg.Class. ..) resolves to the
       short ctor name. */
    "(defn ArrayList. ([] (atom [])) ([_] (atom [])))\n"
    "(defn HashMap. ([] (atom {})) ([_] (atom {})))\n"
    "(defn HashSet. ([] (atom #{})) ([_] (atom #{})))\n"
    "(defn .add [c x] (swap! c clojure.core/conj x) true)\n"
    "(defn .put [m k v] (swap! m assoc k v) v)\n"
    "(defn .toArray [c] (object-array (deref c)))\n"
    "(defn .contains [c x] (contains? (deref c) x))\n"
    "(defn .isArray [_] false)\n"   /* cljc arrays are vectors, not host array classes */
    /* java.io char readers over a string, for libs like clojure.data.csv: a
       reader is {:cljc/type <class-kw> :rdr (atom {:s :pos :pb})}; .read returns
       the next codepoint (or -1), .unread pushes one back. */
    "(defn StringReader. [s] {:cljc/type :java.io.Reader :rdr (atom {:s (str s) :pos 0 :pb nil})})\n"
    "(defn PushbackReader. ([r] (assoc r :cljc/type :java.io.PushbackReader))\n"
    "                      ([r _] (assoc r :cljc/type :java.io.PushbackReader)))\n"
    "(defn .read [r] (let [a (:rdr r) m (deref a)]\n"
    "                  (if-some [pb (:pb m)] (do (swap! a assoc :pb nil) pb)\n"
    "                    (let [s (:s m) pos (:pos m)]\n"
    "                      (if (< pos (count s)) (do (swap! a assoc :pos (inc pos)) (int (nth s pos))) -1)))))\n"
    "(defn .unread [r ch] (swap! (:rdr r) assoc :pb (long ch)) nil)\n"
    "(defn .close [_] nil)\n"
    /* java.io char writer: a StringBuilder prints as its content, so (str w)
       yields what was written. .write takes a string or an int codepoint. */
    "(defn StringWriter. [] (StringBuilder.))\n"
    "(defn .write [w x] (.append w (if (number? x) (char x) x)) nil)\n"
    "(defn .flush [_] nil)\n"
    "(defn .first [c] (first c))\n"
    "(defn random-uuid [] (UUID/randomUUID))\n"
    "(defn System/nanoTime [] (long (* (cljc/now-ms*) 1000000.0)))\n"
    "(defn System/lineSeparator [] \"\\n\")\n"
    /* *in* / with-in-str + a LispReader$StringReader shim: instaparse unescapes
     * grammar strings by reading them through the JVM reader. cljc has no such
     * machinery, but unescaping a string literal IS what read-string does, so
     * the StringReader collapses to (read-string (str "\"" *in*)). */
    "(def ^:dynamic *in* nil)\n"
    "(defmacro with-in-str [s & body] `(binding [*in* ~s] ~@body))\n"
    "(defn clojure.lang.LispReader$StringReader. [] (fn [& _] (read-string (str \"\\\"\" *in*))))\n"
    "(defn java.util.LinkedList. [] [])\n"
    /* java reflection: cljc has no classes — inert */
    "(defn Class/forName [& _] nil)\n"
    "(defn .getName [x] (if (and (map? x) (:nm x)) (:nm x) (str x)))\n"   /* :nm = synthetic Method */
    "(defn .getSimpleName [x] (str x))\n"
    "(defn .getClass [x] (type x))\n"
    /* threads: cljc is single-threaded, so one stable identity/id */
    "(defn Thread/currentThread [] :main-thread)\n"
    "(defn .getId [_] 0)\n"
    "(defn Thread/sleep [ms & _] (cljc/sleep-ms* ms))\n"
    /* primitive TYPE fields (reflection tables) -> cljc type keywords */
    "(def Character/TYPE :char) (def Integer/TYPE :int) (def Long/TYPE :int)\n"
    "(def Double/TYPE :double) (def Float/TYPE :double) (def Boolean/TYPE :bool)\n"
    "(def Byte/TYPE :int) (def Short/TYPE :int) (def Void/TYPE nil)\n"
    "(def Integer/MAX_VALUE 2147483647) (def Integer/MIN_VALUE -2147483648)\n"
    "(def Long/MAX_VALUE 9223372036854775807) (def Long/MIN_VALUE -9223372036854775808)\n"
    "(def Byte/MIN_VALUE -128) (def Byte/MAX_VALUE 127)\n"
    "(def Short/MIN_VALUE -32768) (def Short/MAX_VALUE 32767)\n"
    "(def Character/MIN_VALUE 0) (def Character/MAX_VALUE 65535)\n"
    "(def Character/MAX_CODE_POINT 1114111)\n"
    "(def Double/POSITIVE_INFINITY ##Inf) (def Double/NEGATIVE_INFINITY ##-Inf)\n"
    "(def Double/NaN ##NaN) (def Double/MAX_VALUE 1.7976931348623157E308) (def Double/MIN_VALUE 4.9E-324)\n"
    "(defn Double/isNaN [x] (not= x x)) (defn Double/isInfinite [x] (or (= x ##Inf) (= x ##-Inf)))\n"
    "(defn Long/bitCount [n]\n"
    "  (loop [n n c 0] (if (zero? n) c (recur (unsigned-bit-shift-right n 1) (+ c (bit-and n 1))))))\n"
    "(def bit-count Long/bitCount)\n"
    "(defn Long/numberOfLeadingZeros [n]\n"
    "  (if (zero? n) 64 (loop [n n c 0] (if (neg? n) c (recur (bit-shift-left n 1) (inc c))))))\n"
    "(defn Long/numberOfTrailingZeros [n]\n"
    "  (if (zero? n) 64 (loop [n n c 0] (if (odd? n) c (recur (unsigned-bit-shift-right n 1) (inc c))))))\n"
    "(defn Integer/numberOfLeadingZeros [n] (- (Long/numberOfLeadingZeros (bit-and n 0xFFFFFFFF)) 32))\n"
    "(def Integer/bitCount Long/bitCount)\n"
    /* StringBuilder: an atom holding a string; the .methods code uses to build
       tokens. .length/.charAt/.toString also accept a plain string. */
    "(defn StringBuilder. ([] {:cljc/type :StringBuilder :v (atom \"\")})\n"
    "  ([s] {:cljc/type :StringBuilder :v (atom (str s))}))\n"
    "(defn cljc/sb-str [o] (if (and (map? o) (= :StringBuilder (:cljc/type o))) (deref (:v o)) (str o)))\n"
    "(defn .append [sb x] (swap! (:v sb) str x) sb)\n"
    "(defn .toString [o] (cljc/sb-str o))\n"
    /* A deftype that implements an interface method whose name collides with a
       built-in string .method (e.g. CharSequence length/charAt/subSequence on
       instaparse's Segment): prefer the deftype's own method for instances. */
    "(defn cljc/dt-method [mname o]\n"
    "  (when (and (map? o) (get o :cljc/type))\n"
    "    (get-in (deref cljc/deftype-methods) [mname (get o :cljc/type)])))\n"
    /* coerce any CharSequence (a string, or a deftype like Segment with
       length/charAt) to a plain string — cljc's regex needs a real string */
    "(defn cljc/cs->str [s]\n"
    "  (if (and (map? s) (get s :cljc/type))\n"
    "    (apply str (map (fn [i] (.charAt s i)) (range (.length s))))\n"
    "    (str s)))\n"
    "(defn .length [o] (if-let [m (cljc/dt-method 'length o)] (m o) (count (cljc/sb-str o))))\n"
    "(defn .charAt [o i] (if-let [m (cljc/dt-method 'charAt o)] (m o i) (nth (cljc/sb-str o) i)))\n"
    "(defn .deleteCharAt [sb i]\n"
    "  (let [s (deref (:v sb))] (reset! (:v sb) (str (subs s 0 i) (subs s (inc i))))) sb)\n"
    "(defn .setLength [sb n] (reset! (:v sb) (subs (deref (:v sb)) 0 n)) sb)\n"
    /* java.util.regex Matcher over cljc's own regex: re-matches returns the
       whole match (no groups) or a [whole g1 g2 ..] vector. */
    "(defn .matcher [pat s] (atom {:pat pat :s (cljc/cs->str s) :groups nil}))\n"
    "(defn .matches [m]\n"
    "  (let [r (re-matches (:pat (deref m)) (:s (deref m)))]\n"
    "    (swap! m assoc :groups (cond (vector? r) r (string? r) [r] :else nil))\n"
    "    (boolean r)))\n"
    /* Matcher.lookingAt: match anchored at the front (a prefix, not the whole
       string). Stores groups like .matches so .group reads them. */
    "(defn .lookingAt [m]\n"
    "  (let [r (cljc/re-match-front (:pat (deref m)) (:s (deref m)))]\n"
    "    (swap! m assoc :groups (cond (vector? r) r (string? r) [r] :else nil))\n"
    "    (boolean r)))\n"
    "(defn .group ([m] (.group m 0))\n"
    "  ([m n] (when-let [g (:groups (deref m))] (nth g n nil))))\n"
    /* BigInteger / BigInt / Numbers: cljc integers are all int64, so collapse
       the bignum machinery (used by tools.reader's match-int) to plain ints. */
    /* two Java ctors: (String val, int radix) parses; (int signum, byte[] mag)
       is the MD5 idiom — pass the magnitude through for %0..x formatting. */
    "(defn BigInteger.\n"
    "  ([s] (parse-long (str s)))\n"
    "  ([a b] (if (string? a) (Integer/parseInt a (int b)) b)))\n"
    "(defn .bitLength [n] 1)\n"          /* always < 64 -> the long path */
    "(defn .negate [n] (- n))\n"
    "(defn .longValue [n] n)\n"
    "(defn .intValue [n] n)\n"
    "(def BigInt/fromBigInteger identity)\n"
    "(def Numbers/reduceBigInt identity)\n"
    /* java.lang.String instance methods over cljc strings */
    "(defn .endsWith [s suf] (str/ends-with? (str s) suf))\n"
    "(defn .startsWith [s pre] (str/starts-with? (str s) pre))\n"
    "(defn .substring ([s a] (subs (str s) a)) ([s a b] (subs (str s) a b)))\n"
    "(defn .replace [s old new] (clojure.string/replace (str s) old new))\n"  /* String.replace: literal */
    "(defn String/valueOf [x] (str x))\n"
    "(defn Integer/parseInt ([s] (parse-long s)) ([s _radix] (parse-long s)))\n"
    "(defn Long/parseLong ([s] (parse-long s)) ([s _radix] (parse-long s)))\n"
    "(defn Double/parseDouble [s] (parse-double s))\n"
    "(defn .toString [x] (str x))\n"
    "(defn .subSequence [s a b]\n"
    "  (if-let [m (cljc/dt-method 'subSequence s)] (m s a b) (subs (str s) a b)))\n"
    "(defn .contains [s x]\n"
    "  (cond (string? s) (str/includes? s (str x))\n"
    "        (or (set? s) (map? s)) (contains? s x)\n"
    "        :else (boolean (some (fn [e] (= e x)) s))))\n"
    "(defn .trim [s] (str/trim (str s)))\n"
    "(defn array-map [& kvs] (apply hash-map kvs))\n"
    "(defn Double/parseDouble [s] (parse-double (str s)))\n"
    "(defn Float/parseFloat [s] (parse-double (str s)))\n"
    /* STM refs: cljc is single-threaded, so atoms model refs exactly */
    "(defn ref [x & _] (atom x))\n"
    "(defmacro dosync [& body] `(do ~@body))\n"
    "(defn ref-set [r v] (reset! r v))\n"
    "(defn alter [r f & args] (apply swap! r f args))\n"
    "(defn commute [r f & args] (apply swap! r f args))\n"
    "(defn ensure [r] (deref r))\n"
    /* sorted colls: approximated as unsorted (cljc has no ordered colls) */
    /* sorted-set/-by, sorted-map/-by, sorted?, subseq, rsubseq are native */
    "(def clojure.edn/read-string read-string)\n"
    "(def clojure.edn/read read-string)\n"
    /* ── clojure.core completeness (fns SCI and other libs enumerate) ── */
    /* predicates */
    "(defn any? [_] true)\n"
    "(defn associative? [x] (or (map? x) (vector? x)))\n"
    "(defn counted? [x] (or (vector? x) (map? x) (set? x) (string? x)))\n"
    "(defn indexed? [x] (vector? x))\n"
    "(defn seqable? [x] (or (nil? x) (coll? x) (string? x) (seq? x)))\n"
    "(defn reversible? [x] (vector? x))\n"
    "(defn ratio? [_] false) (defn rational? [x] (number? x)) (defn decimal? [_] false)\n"
    "(defn float? [x] (double? x))\n"
    "(defn bigint? [x] (= (type x) :bigint))\n"
    "(defn integer? [x] (or (int? x) (bigint? x)))\n"
    "(defn ratio? [x] (= (type x) :ratio))\n"
    "(defn rational? [x] (or (integer? x) (ratio? x)))\n"
    "(defn volatile? [x] (= :atom (type x))) (defn realized? [_] true)\n"
    "(defn inst? [_] false) (defn uri? [_] false) (defn uuid? [_] false)\n"
    "(defn object? [x] (some? x)) (defn special-symbol? [_] false)\n"
    "(defn bytes? [_] false) (defn array? [_] false) (defn chunked-seq? [_] false)\n"
    "(defn reader-conditional? [_] false) (defn tagged-literal? [_] false)\n"
    "(defn undefined? [x] (nil? x)) (defn keyword-identical? [a b] (= a b))\n"
    "(defn neg-int? [x] (and (int? x) (neg? x))) (defn pos-int? [x] (and (int? x) (pos? x)))\n"
    /* numeric coercions (cljc ints are int64, no bignum/ratio) */
    "(defn byte [x] (int x)) (defn short [x] (int x)) (defn long [x] (int x))\n"
    "(defn num [x] x) (defn bigdec [x] x) (defn bigint [x] (int x)) (defn biginteger [x] (int x))\n"
    "(defn rationalize [x] x)\n"   /* numerator/denominator are real natives now */
    "(defn double [x] (* 1.0 x)) (defn float [x] (* 1.0 x))\n"
    /* unchecked math == checked in cljc */
    "(def unchecked-add +) (def unchecked-subtract -) (def unchecked-multiply *)\n"
    "(defn inc' [x] (+' x 1)) (defn dec' [x] (-' x 1))\n"   /* auto-promoting inc/dec */
    "(def unchecked-dec dec) (def unchecked-inc inc) (def unchecked-negate -)\n"
    "(def unchecked-add-int +) (def unchecked-subtract-int -) (def unchecked-multiply-int *)\n"
    "(def unchecked-dec-int dec) (def unchecked-inc-int inc) (def unchecked-negate-int -)\n"
    "(def unchecked-divide-int quot) (def unchecked-remainder-int rem)\n"
    "(def unchecked-int int) (def unchecked-long int) (def unchecked-byte int)\n"
    "(def unchecked-short int) (def unchecked-char int)\n"
    "(def unchecked-float float) (def unchecked-double double)\n"
    /* array coercions are identity; the *-array CONSTRUCTORS are defined later,
       after cljc's real (transient-backed, mutable) int-array exists */
    "(def booleans identity) (def chars identity) (def doubles identity) (def longs identity)\n"
    "(def ints identity) (def floats identity) (def shorts identity) (def bytes identity)\n"
    /* misc */
    "(defn bounded-count [n coll] (loop [i 0 s (seq coll)] (if (and s (< i n)) (recur (inc i) (next s)) i)))\n"
    "(defn comparator [f] (fn [a b] (cond (f a b) -1 (f b a) 1 :else 0)))\n"
    "(defn shuffle [coll] (vec coll))\n"
    "(defn replace\n"
    "  ([smap] (map (fn [x] (if (contains? smap x) (get smap x) x))))\n"
    "  ([smap coll] (mapv (fn [x] (if (contains? smap x) (get smap x) x)) coll)))\n"
    "(defn replicate [n x] (repeat n x))\n"
    "(defn nthnext [coll n] (loop [n n s (seq coll)] (if (and (pos? n) s) (recur (dec n) (next s)) s)))\n"
    "(defn rseq [v] (seq (reverse v))) (defn supers [_] #{})\n"
    "(defn munge [x] x) (defn namespace-munge [x] (str x))\n"
    "(defn add-watch [r _ _] r) (defn remove-watch [r _] r)\n"
    "(defn promise [] (atom nil)) (defn deliver [p v] (reset! p v) p)\n"
    "(defn line-seq [rdr] (seq (str/split-lines (str rdr))))\n"
    "(defn iterator-seq [x] (seq x)) (defn enumeration-seq [x] (seq x))\n"
    "(defn array-seq [x] (seq x)) (defn xml-seq [x] (seq x))\n"
    "(defn re-matcher [pat s] (.matcher pat s))\n"
    "(defn re-groups [m] (:groups (deref m)))\n"
    "(defn remove-all-methods [mm] (swap! cljc/multi-tables assoc (cljc/mmk mm) {}) nil)\n"
    "(defn prefers [_] {})\n"
    "(defn ex-cause [e] (when (map? e) (:cause e)))\n"
    "(defn tagged-literal [tag form] {:tag tag :form form})\n"
    "(defn compare-and-set! [a old new] (if (= (deref a) old) (do (reset! a new) true) false))\n"
    "(defn random-uuid [] \"00000000-0000-0000-0000-000000000000\")\n"
    "(defn hash-ordered-coll [c] (hash (vec c))) (defn hash-unordered-coll [c] (hash (set c)))\n"
    "(defn hash-combine [a b] (bit-xor (hash a) (hash b)))\n"
    "(defn pop! [coll] coll)\n"
    "(defn random-sample\n"
    "  ([prob] (filter (fn [_] (< (rand) prob))))\n"
    "  ([prob coll] (filter (fn [_] (< (rand) prob)) coll)))\n"
    /* chunked-seq stubs (cljc seqs are unchunked) */
    "(defn chunk-buffer [_] (atom [])) (defn chunk-append [b x] (swap! b conj x) nil)\n"
    "(defn chunk [b] (deref b)) (defn chunk-cons [c s] (concat c s))\n"
    "(defn chunk-first [s] (first s)) (defn chunk-rest [s] (rest s)) (defn chunk-next [s] (next s))\n"
    /* misc remaining */
    "(defn bean [_] {}) (defn demunge [x] (str x)) (defn seque ([coll] coll) ([_ coll] coll))\n"
    "(defn update-proxy [& _] nil) (defn proxy-mappings [_] {}) (defn proxy-call-with-super [f _ _] (f))\n"
    "(defn reader-conditional [form splicing?] {:form form :splicing? splicing?})\n"
    "(defn inst-ms [_] 0) (defn to-array-2d [coll] (mapv vec coll))\n"
    "(defn halt-when\n"
    "  ([pred] (halt-when pred nil))\n"
    "  ([pred retf]\n"
    "   (fn [rf]\n"
    "     (fn ([] (rf))\n"
    "       ([result] (if (and (map? result) (contains? result :cljc/halt))\n"
    "                   (:cljc/halt result) (rf result)))\n"
    "       ([result input]\n"
    "        (if (pred input)\n"
    "          (reduced {:cljc/halt (if retf (retf (rf result) input) input)})\n"
    "          (rf result input)))))))\n"
    "(defn print-str [& xs] (str/join \" \" (map str xs)))\n"
    "(defn prn-str [& xs] (str (apply pr-str xs) \"\\n\"))\n"
    "(defn println-str [& xs] (str (str/join \" \" (map str xs)) \"\\n\"))\n"
    "(defn test [_] :ok)\n"
    "(def char-name-string {}) (def char-escape-string {})\n"
    "(defn seq-to-map-for-destructuring [s] (if (map? s) s (apply hash-map s)))\n"
    "(defmacro when-some [bindings & body]\n"
    "  `(let [v# ~(second bindings)] (if (nil? v#) nil (let [~(first bindings) v#] ~@body))))\n"
    "(defmacro if-some\n"
    "  ([bindings then] `(if-some ~bindings ~then nil))\n"
    "  ([bindings then else]\n"
    "   `(let [v# ~(second bindings)] (if (nil? v#) ~else (let [~(first bindings) v#] ~then)))))\n"
    "(defn ->Eduction [xform coll] (sequence xform coll))\n"
    "(defn eduction [& args] (sequence (apply comp (butlast args)) (last args)))\n"
    "(defn str/triml [s]\n"
    "  (let [n (count s)] (loop [i 0] (if (and (< i n) (str/blank? (subs s i (inc i)))) (recur (inc i)) (subs s i)))))\n"
    "(defn str/trimr [s]\n"
    "  (loop [i (count s)] (if (and (pos? i) (str/blank? (subs s (dec i) i))) (recur (dec i)) (subs s 0 i))))\n"
    "(defn str/trim-newline [s]\n"
    "  (loop [i (count s)] (if (and (pos? i) (contains? #{\"\\n\" \"\\r\"} (subs s (dec i) i))) (recur (dec i)) (subs s 0 i))))\n"
    "(defn str/capitalize [s]\n"
    "  (if (< (count s) 1) s (str (str/upper-case (subs s 0 1)) (str/lower-case (subs s 1)))))\n"
    "(defn str/reverse [s] (apply str (reverse (seq s))))\n"
    "(defn str/last-index-of [s x]\n"
    "  (let [sub (str x)]\n"
    "    (loop [i (- (count s) (count sub))]\n"
    "      (cond (neg? i) nil\n"
    "            (= (subs s i (+ i (count sub))) sub) i\n"
    "            :else (recur (dec i))))))\n"
    "(defn str/escape [s cmap] (apply str (map (fn [c] (or (get cmap c) c)) (seq s))))\n"
    "(defn str/re-quote-replacement [s] (str/replace (str/replace s \"\\\\\" \"\\\\\\\\\") \"$\" \"\\\\$\"))\n"
    /* clojure.string/foo resolves to cljc's str/foo (same for set/walk) */
    "(cljc/alias* \"clojure.string\" \"str\")\n"
    "(defn Integer/parseInt\n"
    "  ([s] (parse-long s))\n"
    "  ([s radix]\n"
    "   (reduce (fn [acc c]\n"
    "             (let [d (Character/digit c radix)]\n"
    "               (when (neg? d) (throw (ex-info (str \"bad digit: \" c) {})))\n"
    "               (+ (* acc radix) d)))\n"
    "           0 (seq s))))\n"
    "(def Long/parseLong Integer/parseInt)\n"
    "(defn Integer/toString\n"
    "  ([n] (str n))\n"
    "  ([n radix]\n"
    "   (if (zero? n) \"0\"\n"
    "       (loop [n n acc \"\"]\n"
    "         (if (zero? n) acc\n"
    "             (recur (quot n radix)\n"
    "                    (str (get cljc/digit-chars (mod n radix)) acc)))))))\n"
    "(def Long/toString Integer/toString)\n"
    "(defn Integer/toBinaryString [n] (Integer/toString n 2))\n"
    "(defn AssertionError. [msg] (ex-info (str msg) {}))\n"
    "(defn RuntimeException. ([] (ex-info \"\" {})) ([msg] (ex-info (str msg) {})) ([msg c] (ex-info (str msg) {} c)))\n"
    "(defn Exception. ([] (ex-info \"\" {})) ([msg] (ex-info (str msg) {})) ([msg c] (ex-info (str msg) {} c)))\n"
    "(defn IllegalArgumentException. ([msg] (ex-info (str msg) {})) ([msg c] (ex-info (str msg) {} c)))\n"
    "(defn IllegalStateException. ([msg] (ex-info (str msg) {})) ([msg c] (ex-info (str msg) {} c)))\n"
    "(defn UnsupportedOperationException. [msg] (ex-info (str msg) {}))\n"
    /* mutable arrays, as transient vectors (assoc! mutates in place) */
    "(defn int-array [x] (transient (vec (if (int? x) (repeat x 0) x))))\n"
    "(def byte-array int-array)\n"
    "(def long-array int-array)\n"
    /* Object arrays default to nil (not 0 like int arrays) — DataScript relies on
       (when-some [x (aget arr i)] ..) skipping unfilled slots. */
    "(defn object-array [x] (transient (vec (if (int? x) (repeat x nil) x))))\n"
    "(defn LazilyPersistentVector/createOwning [oa] (vec oa))\n"
    "(defn PersistentArrayMap/createWithCheck [arr]\n"
    "  (let [v (vec arr) m (apply hash-map v)]\n"
    "    (when-not (= (* 2 (count m)) (count v)) (throw (ex-info \"Duplicate key\" {})))\n"
    "    m))\n"
    /* java.util.HashMap: a mutable map as an atom holding a persistent map */
    "(defn HashMap. ([] (atom {})) ([_] (atom {})) ([_ _] (atom {})))\n"
    "(defn .putAll [hm m] (swap! hm merge m) hm)\n"
    "(defn .put [hm k v] (swap! hm assoc k v) v)\n"
    "(defn aget [a i] (a i))\n"
    "(defn aset [a i v] (assoc! a i v) v)\n"
    "(defn alength [a] (count a))\n"
    /* remaining array constructors / ops as transient (mutable) arrays */
    "(def double-array int-array) (def float-array int-array) (def short-array int-array)\n"
    "(def char-array int-array) (def boolean-array int-array)\n"
    "(defn make-array [_ & dims] (object-array (first dims)))\n"
    "(defn to-array [coll] (int-array (vec coll)))\n"
    "(defn into-array ([coll] (int-array (vec coll))) ([_t coll] (int-array (vec coll))))\n"
    "(defn aclone [a] (int-array (vec a)))\n"
    "(defn aset-int [a i v] (aset a i v)) (defn aset-long [a i v] (aset a i v))\n"
    "(defn aset-double [a i v] (aset a i v)) (defn aset-float [a i v] (aset a i v))\n"
    "(defn aset-boolean [a i v] (aset a i v)) (defn aset-byte [a i v] (aset a i v))\n"
    "(defn aset-char [a i v] (aset a i v)) (defn aset-short [a i v] (aset a i v))\n"
    /* image-writing stubs: visualization code runs, no PNGs produced */
    "(def BufferedImage/TYPE_3BYTE_BGR 5)\n"
    "(def BufferedImage/TYPE_INT_RGB 1)\n"
    "(defn BufferedImage. [w h type] (atom {:w w :h h}))\n"
    "(defn .setRGB [img x y rgb] nil)\n"
    "(defn .getRGB [& _] 0)\n"          /* (.getRGB color) and (.getRGB img x y) */
    "(defn java.awt.Color. [& _] 0)\n"
    "(def java.awt.Color/WHITE 0) (def java.awt.Color/BLACK 0)\n"
    "(def java.awt.Color/RED 0) (def java.awt.Color/GREEN 0)\n"
    "(def java.awt.Color/BLUE 0) (def java.awt.Color/GRAY 0)\n"
    "(def java.awt.Color/YELLOW 0) (def java.awt.Color/ORANGE 0)\n"
    /* host-class stubs so libraries that extend protocols to / dispatch on JVM
       classes load (cljc never has instances of them; each resolves to a
       distinct keyword token). Mainly for tools.reader's java.io.* readers. */
    "(def java.io.PushbackReader :java.io.PushbackReader)\n"
    "(def java.io.Reader :java.io.Reader)\n"
    "(def java.io.Writer :java.io.Writer)\n"
    "(def java.io.InputStream :java.io.InputStream)\n"
    "(def java.io.BufferedReader :java.io.BufferedReader)\n"
    "(def clojure.lang.LineNumberingPushbackReader :clojure.lang.LineNumberingPushbackReader)\n"
    /* map host classes to cljc's actual type-keyword dispatch values so a
       protocol extension to e.g. String actually fires for cljc strings, and
       Object becomes the multimethod :default catch-all. */
    "(def Object :default) (def String :string) (def CharSequence :string)\n"
    "(def Character :char) (def Boolean :bool) (def Number :int)\n"
    "(def Long :int) (def Integer :int) (def Double :double)\n"
    "(def Float :double) (def Byte :int) (def Short :int) (def Number :number)\n"
    "(def Keyword :keyword) (def Symbol :symbol) (def String :string) (def Boolean :bool) (def Character :char)\n"
    "(def BigInt :bigint) (def BigInteger :bigint) (def BigDecimal :double) (def Ratio :ratio)\n"
    /* host exception/error classes (catch dispatch values, sci class tables) */
    "(def Exception :Exception) (def Throwable :Throwable) (def Error :Error)\n"
    "(def RuntimeException :RuntimeException) (def AssertionError :AssertionError)\n"
    "(def IllegalArgumentException :IllegalArgumentException)\n"
    "(def IllegalStateException :IllegalStateException)\n"
    "(def NullPointerException :NullPointerException)\n"
    "(def ArithmeticException :ArithmeticException)\n"
    "(def ClassCastException :ClassCastException)\n"
    "(def IndexOutOfBoundsException :IndexOutOfBoundsException)\n"
    "(def NumberFormatException :NumberFormatException)\n"
    "(def UnsupportedOperationException :UnsupportedOperationException)\n"
    "(def ClassNotFoundException :ClassNotFoundException)\n"
    "(def StackOverflowError :StackOverflowError)\n"
    "(defn File. [path] path)\n"
    /* date/uuid host classes (#inst/#uuid data-reader setup) — inert stubs */
    "(defn java.text.SimpleDateFormat. [& _] (atom nil))\n"
    "(defn java.util.Date. [& _] 0)\n"
    "(defn java.util.GregorianCalendar. [& _] (atom nil))\n"
    "(defn .setTimeZone [& _] nil)\n"
    "(defn .setCalendar [& _] nil)\n"
    "(defn java.util.TimeZone/getTimeZone [_] nil)\n"
    "(defn java.util.UUID/fromString [s] s)\n"
    "(defn java.util.UUID/randomUUID [] \"00000000-0000-0000-0000-000000000000\")\n"
    "(defn java.util.UUID. [& _] \"00000000-0000-0000-0000-000000000000\")\n"
    "(defn ImageIO/write [img fmt file] true)\n"
    /* :paths from a project's deps.edn / bb.edn feed *load-path*, so the SAME
       config that drives clj and bb also tells cljc where to find code. Walk
       UP from the cwd to the nearest dir holding deps.edn/bb.edn (like clj/bb
       find the project root), resolving its :paths relative to that dir. Only
       :paths is honoured — cljc has no Maven/git resolver, so :deps, :tasks,
       :aliases are ignored. Read errors degrade to nil (no paths). */
    /* &env compat shim: cljc doesn't pass a per-expansion macro environment,
       but portable libraries reference &env to detect cljs-vs-clj at expansion
       time (macrovich: (:ns &env) is truthy on cljs). A global empty map makes
       those macros take the clj branch — which is what cljc wants. Not a real
       lexical &env (it has no locals); enough for the common pattern. */
    "(def &env {})\n"
    /* runtime version vars some libraries (tools.reader) gate features on */
    "(def *clojure-version* {:major 1 :minor 11 :incremental 0 :qualifier nil})\n"
    "(defn clojure-version [] \"1.11.0\")\n"
    /* standard clojure.core dynamic vars libraries reference (cljc's printer /
       reader don't consult most of these; they exist so code that names them
       loads and can rebind them) */
    "(def *unchecked-math* false) (def *warn-on-reflection* false)\n"
    "(def *print-length* nil) (def *print-level* nil) (def *print-meta* false)\n"
    "(def *print-readably* true) (def *read-eval* true) (def *flush-on-newline* true)\n"
    "(def *assert* true) (def *data-readers* {}) (def *default-data-reader-fn* nil)\n"
    "(def *math-context* nil) (def *file* \"NO_SOURCE_PATH\") (def *command-line-args* nil)\n"
    "(def *ns* nil) (def *e nil) (def *1 nil) (def *2 nil) (def *3 nil)\n"
    /* print/println/pr/prn write to the current value of the *out* var. The
       default sentinels route to the real streams; bind it to a (StringWriter.)
       to capture (with-out-str), or to the *err* sentinel to reach stderr. */
    "(def ^:dynamic *out* :cljc/stdout) (def ^:dynamic *err* :cljc/stderr)\n"
    "(defmacro with-out-str [& body] `(let [s# (StringWriter.)] (binding [*out* s#] ~@body) (str s#)))\n"
    "(defmacro with-err-str [& body] `(let [s# (StringWriter.)] (binding [*err* s#] ~@body) (str s#)))\n"
    /* &form / &env: Clojure binds these in macro bodies (the call form + compile
       env). cljc doesn't yet thread them, so a nil global keeps macros that only
       read them for metadata/cljs-detection working — nil &env is the clj path. */
    "(def &form nil) (def &env nil)\n"
    /* ns-name: with cljc's flat namespaces, *ns* carries no Namespace object, so
       fall back to the namespace currently being loaded (meander's defsyntax). */
    "(defn ns-name [n] (symbol (let [s (str n)] (if (or (nil? n) (= \"\" s)) (cljc/current-ns*) s))))\n"
    "(def *allow-unresolved-vars* false) (def *suppress-read* nil) (def *verbose-defrecords* false)\n"
    "(def *print-dup* false) (def *print-namespace-maps* true) (def *fn-loader* nil)\n"
    "(def *source-path* \"NO_SOURCE_FILE\") (def *use-context-classloader* true)\n"
    /* proxy: no JVM classes to subclass — stub to nil so (def x (proxy ..))
       loads (the object is unused unless its host methods are called). */
    /* proxy: cljc has no host classes; model the instance as an atom. If the
       proxy defines an (initialValue [] BODY) method (the ThreadLocal idiom),
       seed the atom with BODY so .get returns it. .get/.set act on the atom. */
    "(defmacro proxy [_supers _ctor & methods]\n"
    "  (let [iv (some (fn [m] (when (= 'initialValue (first m)) m)) methods)]\n"
    "    (if iv `(atom ~(first (drop 2 iv))) `(atom nil))))\n"
    "(defn .get\n"
    "  ([x] (if (= :atom (type x)) (deref x) x))\n"   /* IDeref/future .get */
    "  ([m k] (get (if (= :atom (type m)) (deref m) m) k))\n"   /* incl. HashMap */
    "  ([m k d] (get (if (= :atom (type m)) (deref m) m) k d)))\n"
    /* clojure.lang.{Associative,Indexed,IDeref,Map} interop (sci.impl.faster) */
    "(defn .assoc [m k v] (assoc m k v))\n"
    "(defn .nth ([c i] (nth c i)) ([c i d] (nth c i d)))\n"
    "(defn .count [c] (count c))\n"
    "(defn .seq [c] (seq c))\n"
    "(defn .hashCode [x] (hash x))\n"
    "(defn .equals [a b] (= a b))\n"
    "(defn .valAt ([m k] (get m k)) ([m k d] (get m k d)))\n"
    "(defn .deref [r] (deref r))\n"
    "(defn .idx [x] (:idx x))\n"   /* BindingNode field via (.idx node) in :clj */
    "(defn .containsKey [m k] (contains? (if (= (type m) :atom) (deref m) m) k))\n"
    "(defn .entryAt [m k] (find m k))\n"
    "(defn .set [x v] (when (= :atom (type x)) (reset! x v)) nil)\n"
    "(defn .remove [& _] nil)\n"
    /* exception accessors (cljc errors carry :message/:data; ex-* read them) */
    "(defn .getMessage [e] (or (ex-message e) (when (map? e) (:message e)) (str e)))\n"
    "(defn .getLocalizedMessage [e] (.getMessage e))\n"
    "(defn .getCause [e] (when (map? e) (:cause e)))\n"
    "(defn .getData [e] (ex-data e))\n"
    "(defn .getStackTrace [_] [])\n"
    "(defn .printStackTrace [& _] nil)\n"
    /* set! on a var/symbol rebinds the global (cljc vars are mutable globals);
       set! on a deftype field is rewritten to reset! before this is reached. */
    "(defmacro set! [place val] (if (symbol? place) `(def ~place ~val) val))\n"
    /* (Object.) is used as a unique sentinel; a fresh atom is a clean identity
       object that never equals read data. */
    "(defn Object. [] (atom nil))\n"
    /* #'x => (var x) => a Var (IFn + IDeref, carries {:name :ns} metadata).
       resolve/ns-resolve/requiring-resolve return a Var too (Clojure semantics);
       a Var calls its value in call position and @-derefs to it, so most uses
       work unchanged while reflective code (import-def) can read :name. */
    "(defmacro var [s] `(cljc/resolve-var '~s))\n"
    "(defn resolve ([sym] (cljc/resolve-var sym)) ([_ns sym] (cljc/resolve-var sym)))\n"
    "(def ns-resolve resolve)\n"
    "(defn requiring-resolve [sym] (cljc/resolve-var sym))\n"
    "(defn var? [x] (= (type x) :var))\n"
    "(defn var-get [v] (deref v))\n"
    "(defn find-var [sym] (cljc/resolve-var sym))\n"
    "(defn alter-var-root [v f & args] (cljc/set-var! v (apply f (deref v) args)))\n"
    /* thread-binding primitives (hiccup's binding* / clojure.core internals):
       cljc is single-threaded, so a global save/restore stack suffices. */
    "(def cljc/tbind-stack (atom ()))\n"
    "(defn push-thread-bindings [m]\n"
    "  (swap! cljc/tbind-stack conj (into {} (map (fn [[v _]] [v (deref v)]) m)))\n"
    "  (doseq [[v val] m] (cljc/set-var! v val)))\n"
    "(defn pop-thread-bindings []\n"
    "  (let [saved (first (deref cljc/tbind-stack))]\n"
    "    (swap! cljc/tbind-stack rest)\n"
    "    (doseq [[v val] saved] (cljc/set-var! v val))))\n"
    "(defn get-thread-bindings [] (apply merge {} (reverse (deref cljc/tbind-stack))))\n"
    "(defn with-bindings* [m f & args]\n"
    "  (push-thread-bindings m) (try (apply f args) (finally (pop-thread-bindings))))\n"
    "(defmacro with-bindings [m & body] `(with-bindings* ~m (fn [] ~@body)))\n"
    "(defn bound-fn* [f] (let [b (get-thread-bindings)] (fn [& a] (with-bindings* b #(apply f a)))))\n"
    "(defmacro bound-fn [& fntail] `(bound-fn* (fn ~@fntail)))\n"
    /* alter-meta!/reset-meta!: a deftype with a mutable :meta field (e.g. SCI's
       Var/Namespace) keeps its metadata in a :meta atom — mutate that. */
    "(defn alter-meta! [r f & args]\n"
    "  (when (= :atom (type (:meta r))) (swap! (:meta r) (fn [m] (apply f m args))))\n"
    "  (when (= :atom (type (:meta r))) (deref (:meta r))))\n"
    "(defn reset-meta! [r m] (when (= :atom (type (:meta r))) (reset! (:meta r) m)) m)\n"
    /* ad-hoc hierarchies: cljc multimethods don't use them, so these are inert */
    /* derive/underive/isa?/parents/ancestors are real now — see above */
    "(defn make-hierarchy [] {})\n"
    "(defn ident? [x] (or (symbol? x) (keyword? x)))\n"
    "(defn namespace [x]\n"
    "  (let [s (if (keyword? x) (subs (str x) 1) (str x))\n"
    "        i (str/index-of s \"/\")]\n"
    "    (when (and i (pos? i)) (subs s 0 i))))\n"
    "(defn qualified-symbol? [x] (and (symbol? x) (str/includes? (str x) \"/\")))\n"
    "(defn simple-symbol? [x] (and (symbol? x) (not (str/includes? (str x) \"/\"))))\n"
    "(defn qualified-keyword? [x] (and (keyword? x) (str/includes? (str x) \"/\")))\n"
    "(defn simple-keyword? [x] (and (keyword? x) (not (str/includes? (str x) \"/\"))))\n"
    "(defn qualified-ident? [x] (and (ident? x) (str/includes? (str x) \"/\")))\n"
    "(defn simple-ident? [x] (and (ident? x) (not (str/includes? (str x) \"/\"))))\n"
    "(def global-hierarchy (atom {}))\n"
    "(defn cljc/edn-paths [f]\n"
    "  (try (let [m (read-string (slurp f))]\n"
    "         (when (and (map? m) (vector? (:paths m))) (:paths m)))\n"
    "       (catch Exception e nil)))\n"
    "(defn cljc/project-paths []\n"
    "  (loop [pre \"\"]\n"                       /* "" -> "../" -> "../../" ... */
    "    (let [ps (mapcat (fn [n] (cljc/edn-paths (str pre n))) [\"deps.edn\" \"bb.edn\"])]\n"
    "      (cond\n"
    "        (seq ps) (map (fn [p] (str pre p)) ps)\n"   /* resolve to the config's dir */
    "        (> (count pre) 60) nil\n"            /* stop after ~20 levels */
    "        :else (recur (str pre \"../\"))))))\n"
    "(def *load-path*\n"
    "  (vec (distinct (concat [\".\"]\n"
    "                         (cljc/project-paths)\n"
    "                         [\"vendor\"]\n"
    "                         (when-let [p (cljc/env* \"CLJC_PATH\")]\n"
    "                           (str/split p \":\"))\n"
    /* exe-relative share dir first (works under any install PREFIX), then the
       compile-time one as a fallback */
    "                         (when-let [d (cljc/exe-sharedir*)] [d (str d \"/vendor\")])\n"
    "                         [(cljc/sharedir*) (str (cljc/sharedir*) \"/vendor\")]))))\n"
    "(def cljc/loaded-namespaces (atom #{}))\n"
    "(defn cljc/spec-opt [spec k]\n"
    "  (loop [s (seq (rest spec))]\n"
    "    (cond (nil? s) nil\n"
    "          (= k (first s)) (second s)\n"
    "          :else (recur (nnext s)))))\n"
    /* (:import clojure.lang.Foo (java.io Bar Baz)) — bind each short class name
       to a keyword token so code that names the class loads. cljc has no real
       classes; the value just needs to be a stable, distinct placeholder. */
    /* well-known host scalar classes map to cljc's OWN type keywords, so a
       (defmethod f [Keyword] ..) dispatches on the same value (type x) yields —
       this is what makes Emmy's generic-op derivative dispatch (g/add :dfdx) hit. */
    "(def cljc/class->kind {\"Keyword\" :keyword \"Symbol\" :symbol \"String\" :string\n"
    "  \"Long\" :int \"Integer\" :int \"BigInt\" :bigint \"BigInteger\" :bigint \"Ratio\" :ratio\n"
    "  \"Double\" :double \"Float\" :double \"Number\" :number \"Boolean\" :bool \"Character\" :char\n"
    /* all the host fn classes collapse to cljc's single :fn type, so an
       (extend-type MultiFn ..) etc. applies to every cljc function */
    "  \"AFunction\" :fn \"Fn\" :fn \"RestFn\" :fn \"MultiFn\" :fn \"IFn\" :fn \"AFn\" :fn\n"
    "  \"Var\" :var})\n"
    /* a deftype must NOT derive from these — they're cljc's own native-value
       kinds (and the multimethod :default / nil). Object resolves to :default,
       so without this every deftype would become isa? :default. */
    "(def cljc/no-derive-kinds (conj (set (vals cljc/class->kind)) :default :nil))\n"
    /* bind the FULLY-QUALIFIED host class names too, so (extend-type
       clojure.lang.Fn ..) — written without an import — resolves like the short
       name does (core.logic extends clojure.lang.Fn for its delayed streams). */
    "(doseq [[c k] cljc/class->kind]\n"
    "  (eval (list 'def (symbol (str \"clojure.lang.\" c)) k))\n"
    "  (eval (list 'def (symbol (str \"java.lang.\" c)) k)))\n"
    "(defn cljc/import-one [spec]\n"
    "  (when spec\n"
    "  (if (or (list? spec) (vector? spec) (seq? spec))\n"
    /* a deftype/record imported from another cljc namespace resolves to its REAL
       type tag (datascript.parser/BindScalar -> :BindScalar), not a fabricated
       :pkg.Class keyword — otherwise extend-type/instance? key the wrong value. */
    "    (let [pkg (str (first spec))]\n"
    "      (doseq [c (rest spec)]\n"
    "        (eval (list 'def (symbol (str c)) (or (cljc/class->kind (str c))\n"
    "                                              (cljc/resolve-maybe (str pkg \"/\" c))\n"
    "                                              (keyword (str pkg \".\" c)))))))\n"
    "    (let [s (str spec) short (last (str/split s #\"\\.\"))]\n"
    "      (eval (list 'def (symbol short) (or (cljc/class->kind short)\n"
    "                                          (cljc/resolve-maybe s) (keyword s))))))))\n"
    "(defn cljc/require-one [spec]\n"
    "  (when spec\n"   /* a #?(:cljs ..)-only spec reads as nil under cljc */
    "  (let [nsname (if (vector? spec) (first spec) spec)]\n"
    "    (when-let [a (and (vector? spec) (cljc/spec-opt spec :as))]\n"
    "      (cljc/alias* (str a) (str nsname)))\n"
    "    (when-not (contains? @cljc/loaded-namespaces nsname)\n"
    "      (let [rel (str/replace (str/replace (str nsname) \"-\" \"_\") \".\" \"/\")\n"
    "            paths (mapcat (fn [d] [(str d \"/\" rel \".clj\")\n"
    "                                   (str d \"/\" rel \".cljc\")])\n"
    "                          *load-path*)\n"
    "            hit (some (fn [p] (try (do (slurp p) p) (catch Exception e nil)))\n"
    "                      paths)]\n"
    "        (if hit\n"
    "          (do (swap! cljc/loaded-namespaces conj nsname)\n"
    "              (let [old (cljc/in-ns* (str nsname))]\n"
    "                (try (load-file hit)\n"
    "                     (finally (cljc/in-ns* old)))))\n"
    /* no file found: warn (not throw) — clojure.core & already-interned
       namespaces are native, so only flag genuinely-unknown names */
    "          (when (and (not= (str nsname) \"clojure.core\")\n"
    "                     (empty? (cljc/ns-publics* (str nsname))))\n"
    "            (binding [*out* *err*]\n"
    "              (println (str \"WARNING: could not locate namespace \" nsname\n"
    "                            \" on the load path\")))))))\n"
    "    (when-let [refers (and (vector? spec) (cljc/spec-opt spec :refer))]\n"
    "      (let [cur (cljc/current-ns*)]\n"
    "        (doseq [r (if (= refers :all) (map symbol (cljc/ns-publics* (str nsname))) refers)]\n"
    /* inside a ns, alias to the source global (shared identity for dynamic
       vars); at top level (no ns), fall back to a value copy */
    "          (if (seq cur)\n"
    "            (cljc/refer* (str cur \"/\" r) (str nsname \"/\" r))\n"
    "            (eval (list 'def r (symbol (str nsname \"/\" r)))))))))))\n"
    "(defmacro require [& specs]\n"
    "  `(do ~@(map (fn [s] `(cljc/require-one ~s)) specs) nil))\n"
    "(defn cljc/use-one [spec]\n"
    "  (let [nsname (if (vector? spec) (first spec) spec)]\n"
    "    (cljc/require-one nsname)\n"
    "    (doseq [r (cljc/ns-publics* (str nsname))]\n"
    "      (eval (list 'def (symbol r) (symbol (str nsname \"/\" r)))))))\n"
    /* (use judge) / (use 'judge) / (use [judge ...]) — require then refer ALL
       public names. Strips a leading quote so bare and quoted specs both work. */
    "(defmacro use [& specs]\n"
    "  `(do ~@(map (fn [s]\n"
    "                (let [s (if (and (seq? s) (= (first s) (quote quote))) (second s) s)]\n"
    "                  `(cljc/use-one (quote ~s))))\n"
    "              specs)\n"
    "       nil))\n"
    "(defmacro declare [& names]\n"
    "  `(do ~@(map (fn [n] `(def ~n nil)) names)))\n"
    "(defn boolean [x] (if x true false))\n"
    "(defn true? [x] (= x true))\n"
    "(defn false? [x] (= x false))\n"
    "(defn map-indexed [f coll]\n"
    "  (loop [s (seq coll) i 0 acc (list)]\n"
    "    (if s\n"
    "      (recur (next s) (inc i) (cons (f i (first s)) acc))\n"
    "      (reverse acc))))\n"
    "(defn keep-indexed [f coll] (filter some? (map-indexed f coll)))\n"
    "(defn partition-all [n coll]\n"
    "  (loop [s (seq coll) acc (list)]\n"
    "    (if s\n"
    "      (recur (seq (drop n s)) (cons (take n s) acc))\n"
    "      (reverse acc))))\n"
    "(defn zipmap [ks vs]\n"
    "  (loop [k (seq ks) v (seq vs) m {}]\n"
    "    (if (and k v)\n"
    "      (recur (next k) (next v) (assoc m (first k) (first v)))\n"
    "      m)))\n"
    "(defn merge-with [f & ms]\n"
    "  (reduce (fn [acc m]\n"
    "            (reduce (fn [a [k v]]\n"
    "                      (if (contains? a k)\n"
    "                        (assoc a k (f (get a k) v))\n"
    "                        (assoc a k v)))\n"
    "                    acc (seq m)))\n"
    "          {} ms))\n"
    "(defn reduce-kv [f init m]\n"
    "  (if (map? m)\n"
    "    (reduce (fn [acc [k v]] (f acc k v)) init (seq m))\n"
    "    (let [n (count m)]\n"            /* vectors/indexed: keys are indices */
    "      (loop [i 0 acc init]\n"
    "        (cond (reduced? acc) (unreduced acc)\n"
    "              (< i n) (recur (inc i) (f acc i (nth m i)))\n"
    "              :else acc)))))\n"
    "(defn repeatedly [n f] (map (fn [_] (f)) (range n)))\n"
    "(defmacro doto [x & forms]\n"
    "  (let [g (gensym \"doto\")]\n"
    "    `(let [~g ~x]\n"
    "       ~@(map (fn [f]\n"
    "                (if (list? f)\n"
    "                  (concat (list (first f) g) (rest f))\n"
    "                  (list f g)))\n"
    "              forms)\n"
    "       ~g)))\n"
    "(defmacro letfn [fnspecs & body]\n"
    "  `(let [~@(mapcat (fn [spec] (list (first spec) (cons 'fn (rest spec))))\n"
    "                   fnspecs)]\n"
    "     ~@body))\n"
    "(defmacro condp [pred expr & clauses]\n"
    "  (let [p (gensym \"p\") e (gensym \"e\")]\n"
    "    (list 'let [p pred e expr]\n"
    "      (cons 'cond\n"
    "        (loop [cs clauses acc (list)]\n"
    "          (if (empty? cs)\n"
    "            (reverse (cons '(throw (ex-info \"condp: no matching clause\" {})) (cons :else acc)))\n"
    "            (if (empty? (rest cs))\n"
    "              (reverse (cons (first cs) (cons :else acc)))\n"
    "              (recur (rest (rest cs))\n"
    "                     (cons (first (rest cs)) (cons (list p (first cs) e) acc))))))))))\n"
    /* clojure.core.match subset: match a value against vector patterns. A symbol
       binds (a bare _ is a wildcard), a keyword/number/etc. compares by =, and a
       vector pattern recurses element-wise. Enough for Emmy's arity combination. */
    "(defn cljc/match-compile [v pat]\n"
    "  (cond\n"
    "    (= pat '_) {:test true :binds []}\n"
    "    (symbol? pat) {:test true :binds [pat v]}\n"
    "    (vector? pat)\n"
    "      (let [subs (map-indexed (fn [i p] (cljc/match-compile (list 'nth v i) p)) pat)]\n"
    "        {:test (apply list 'and (list 'sequential? v) (list '= (list 'count v) (count pat))\n"
    "                      (map :test subs))\n"
    "         :binds (vec (mapcat :binds subs))})\n"
    "    :else {:test (list '= v pat) :binds []}))\n"
    "(defmacro match [value & clauses]\n"
    "  (let [v (gensym \"mv\")]\n"
    "    (list 'let [v value]\n"
    "      (cons 'cond\n"
    "        (concat\n"
    "          (mapcat (fn [[pat res]]\n"
    "                    (let [c (cljc/match-compile v pat)]\n"
    "                      [(:test c) (list 'let (:binds c) res)]))\n"
    "                  (partition 2 clauses))\n"
    "          (list :else (list 'throw (list 'ex-info \"no matching clause\" {}))))))))\n"
    "(defn rand-nth [coll] (nth (vec coll) (rand-int (count coll))))\n"
    "(defn max-key [f x & xs] (reduce (fn [a b] (if (> (f a) (f b)) a b)) x xs))\n"
    "(defn min-key [f x & xs] (reduce (fn [a b] (if (< (f a) (f b)) a b)) x xs))\n"
    /* preserve the first set's type (e.g. a sorted-set stays sorted): seed from
       s1 and conj/disj, never rebuild a plain hash-set */
    "(defn set/union\n"
    "  ([] #{})\n"
    "  ([s1] s1)\n"
    "  ([s1 & ss] (reduce (fn [a s] (reduce conj a (seq s))) s1 ss)))\n"
    "(defn set/intersection [s1 & ss]\n"
    "  (reduce (fn [a s] (reduce (fn [r x] (if (contains? s x) r (disj r x))) a (seq a))) s1 ss))\n"
    "(defn set/difference [s1 & ss]\n"
    "  (reduce (fn [a s] (reduce disj a (seq s))) s1 ss))\n"
    "(defmacro some-> [expr & forms]\n"
    "  (if (empty? forms)\n"
    "    expr\n"
    "    (let [g (gensym \"t\")]\n"
    "      `(some-> (let [~g ~expr] (if (nil? ~g) nil (-> ~g ~(first forms))))\n"
    "               ~@(rest forms)))))\n"
    "(defmacro some->> [expr & forms]\n"
    "  (if (empty? forms)\n"
    "    expr\n"
    "    (let [g (gensym \"t\")]\n"
    "      `(some->> (let [~g ~expr] (if (nil? ~g) nil (->> ~g ~(first forms))))\n"
    "                ~@(rest forms)))))\n"
    "(defmacro cond-> [expr & clauses]\n"
    "  (if (empty? clauses)\n"
    "    expr\n"
    "    `(cond-> (if ~(first clauses) (-> ~expr ~(second clauses)) ~expr)\n"
    "             ~@(drop 2 clauses))))\n"
    "(defmacro cond->> [expr & clauses]\n"
    "  (if (empty? clauses)\n"
    "    expr\n"
    "    `(cond->> (if ~(first clauses) (->> ~expr ~(second clauses)) ~expr)\n"
    "              ~@(drop 2 clauses))))\n"
    "(defmacro as-> [expr name & forms]\n"
    "  `(let [~name ~expr ~@(mapcat (fn [f] (list name f)) forms)] ~name))\n"
    "(defn not-empty [coll] (if (empty? coll) nil coll))\n"
    "(defn doall [x] x)\n"
    "(defn dorun [x] nil)\n"
    "(defn flatten [coll]\n"  /* sequential?: lazy sub-seqs flatten too */
    "  (mapcat (fn [x] (if (sequential? x) (flatten x) (list x))) coll))\n"
    "(defn fnil [f d] (fn [x & args] (apply f (if (nil? x) d x) args)))\n"
    "(defn ifn? [x] (or (fn? x) (keyword? x) (symbol? x) (map? x) (set? x) (vector? x)))\n"
    /* var? is a real predicate now (see CLJC_VAR / resolve-var above) */
    /* java.util.Iterator over any seqable, as a seq cursor in an atom */
    "(defn .iterator [coll] (atom (seq coll)))\n"
    "(defn .hasNext [it] (boolean (seq (deref it))))\n"
    /* .next is overloaded: a mutable iterator (an atom cursor) advances and
       returns its head; on a seq/list it's clojure.lang.ISeq.next (DataScript's
       (.next xs)). Dispatch on whether the arg is the atom cursor. */
    "(defn .next [it] (if (identical? (type it) :atom)\n"
    "                   (let [s (seq (deref it))] (reset! it (rest s)) (first s))\n"
    "                   (next it)))\n"
    "(defn .cons [xs x] (cons x xs))\n"   /* clojure.lang.ISeq.cons */
    /* trampoline: call f; while it returns a fn, keep calling it (stack-safe
     * via recur). Clojure's idiom for mutual recursion without proper TCO. */
    "(defn trampoline\n"
    "  ([f] (let [r (f)] (if (fn? r) (recur r) r)))\n"
    "  ([f & args] (trampoline (fn [] (apply f args)))))\n"
    "(defmacro assert\n"
    "  ([x] `(when-not ~x\n"
    "          (throw (ex-info (str \"Assert failed: \" (pr-str '~x)) {}))))\n"
    "  ([x msg] `(when-not ~x\n"
    "              (throw (ex-info (str \"Assert failed: \" ~msg \"\\n\" (pr-str '~x)) {})))))\n"
    ;

CljcEnv *cljc_new_env(void) {
    if (!NIL) {
        gc_stress = getenv("CLJC_GC_STRESS") != NULL;
        srand((unsigned)time(NULL));
        vstack = xmalloc(sizeof(Cljc *) * VSTACK_CAP);
        for (int64_t i = SMALLINT_MIN; i <= SMALLINT_MAX; i++) {
            smallints[i - SMALLINT_MIN].tag = CLJC_INT;
            smallints[i - SMALLINT_MIN].gcmark = 1;  /* permanently marked */
            smallints[i - SMALLINT_MIN].as.i = i;
        }
        for (int i = 0; i < 256; i++) {
            smallchars[i].tag = CLJC_CHAR;
            smallchars[i].gcmark = 1;             /* permanently marked */
            smallchars[i].as.chr = i;
        }
        NIL = alloc(CLJC_NIL);
        /* Self-referential cons fields: walking off the end of any form
         * (e.g. (def) with no args) yields NIL instead of reading
         * uninitialized union bytes. Safety net, not license — special
         * forms still arity-check for decent error messages. */
        NIL->as.cons.head = NIL;
        NIL->as.cons.tail = NIL;
        TRUE = alloc(CLJC_BOOL);  TRUE->as.b = true;
        FALSE = alloc(CLJC_BOOL); FALSE->as.b = false;
        EMPTY = alloc(CLJC_EMPTY);
        EMPTY->as.cons.head = NIL;   /* (first ()) => nil */
        EMPTY->as.cons.tail = EMPTY; /* (rest ())  => ()   */
    }
    CljcEnv *e = env_new(NULL);
    /* Register as a GC root immediately — everything defined below
     * (natives, prelude) is kept alive through this env. */
    if (gc_n_root_envs >= (int)(sizeof gc_root_envs / sizeof *gc_root_envs))
        cljc_error("too many root environments");
    gc_root_envs[gc_n_root_envs++] = e;
    cljc_define_native(e, "+",       prim_add);
    cljc_define_native(e, "-",       prim_sub);
    cljc_define_native(e, "*",       prim_mul);
    cljc_define_native(e, "+'",      prim_addq);
    cljc_define_native(e, "-'",      prim_subq);
    cljc_define_native(e, "*'",      prim_mulq);
    cljc_define_native(e, "numerator",   prim_numerator);
    cljc_define_native(e, "denominator", prim_denominator);
    cljc_define_native(e, "/",       prim_div);
    cljc_define_native(e, "=",       prim_eq);
    cljc_define_native(e, "<",       prim_lt);
    cljc_define_native(e, ">",       prim_gt);
    cljc_define_native(e, "<=",      prim_le);
    cljc_define_native(e, ">=",      prim_ge);
    cljc_define_native(e, "==",      prim_num_eq);
    cljc_define_native(e, "println", prim_println);
    cljc_define_native(e, "cljc/eprintln*", prim_eprintln);
    cljc_define_native(e, "str",     prim_str);
    cljc_define_native(e, "pr-str",  prim_pr_str);
    cljc_define_native(e, "list",    prim_list);
    cljc_define_native(e, "first",   prim_first);
    cljc_define_native(e, "second",  prim_second);
    cljc_define_native(e, "rest",    prim_rest);
    cljc_define_native(e, "cons",    prim_cons);
    cljc_define_native(e, "conj",    prim_conj);
    cljc_define_native(e, "count",   prim_count);
    cljc_define_native(e, "nth",     prim_nth);
    cljc_define_native(e, "vector",  prim_vector);
    cljc_define_native(e, "apply",   prim_apply);
    cljc_define_native(e, "not",     prim_not);
    cljc_define_native(e, "inc",     prim_inc);
    cljc_define_native(e, "dec",     prim_dec);
    cljc_define_native(e, "mod",     prim_mod);
    cljc_define_native(e, "rem",     prim_rem);
    cljc_define_native(e, "map",     prim_map);
    cljc_define_native(e, "filter",  prim_filter);
    cljc_define_native(e, "reduce",  prim_reduce);
    cljc_define_native(e, "range",   prim_range);  /* eager; used during prelude
                                                    * load before the lazy prelude
                                                    * `range` defn shadows it */
    cljc_define_native(e, "cljc/range-eager",  prim_range);       /* small ranges */
    cljc_define_native(e, "cljc/range-chunk*", prim_range_chunk); /* lazy chunks */
    cljc_define_native(e, "take",    prim_take);
    cljc_define_native(e, "drop",    prim_drop);
    cljc_define_native(e, "reverse", prim_reverse);
    cljc_define_native(e, "last",    prim_last);
    cljc_define_native(e, "seq",     prim_seq);
    cljc_define_native(e, "seq?",    prim_seq_p);
    cljc_define_native(e, "type",    prim_type);
    cljc_define_native(e, "sh",        prim_sh);
    cljc_define_native(e, "coro/new",    prim_coro_new);
    cljc_define_native(e, "coro/resume", prim_coro_resume);
    cljc_define_native(e, "coro/yield",  prim_coro_yield);
    cljc_define_native(e, "coro/status", prim_coro_status);
    cljc_define_native(e, "coro/alive?", prim_coro_alive);
    cljc_define_native(e, "hash",      prim_hash);
    cljc_define_native(e, "cljc/env*",      prim_getenv_raw);
    cljc_define_native(e, "cljc/sharedir*", prim_sharedir);
    cljc_define_native(e, "cljc/exe-sharedir*", prim_exe_sharedir);
    cljc_define_native(e, "int",       prim_int);
    cljc_define_native(e, "identical?", prim_identical);
    cljc_define_native(e, "with-meta",  prim_with_meta);
    cljc_define_native(e, "meta",       prim_meta);
    cljc_define_native(e, "cljc/alias*", prim_alias);
    cljc_define_native(e, "cljc/refer*", prim_refer);
    cljc_define_native(e, "cljc/current-ns*", prim_current_ns);
    cljc_define_native(e, "cljc/in-ns*", prim_in_ns);
    cljc_define_native(e, "cljc/ns-publics*", prim_ns_publics);
    cljc_define_native(e, "cljc/resolve-maybe", prim_resolve_maybe);
    cljc_define_native(e, "cljc/resolve-var", prim_resolve_var);
    cljc_define_native(e, "cljc/set-var!", prim_set_var);
    cljc_define_native(e, "cljc/fn-arities", prim_fn_arities);
    cljc_define_native(e, "macroexpand-1", prim_macroexpand_1);
    cljc_define_native(e, "cljc/eval-forms*", prim_eval_forms);
    cljc_define_native(e, "cljc/chunk-map*",    prim_chunk_map);
    cljc_define_native(e, "cljc/chunk-filter*", prim_chunk_filter);
    cljc_define_native(e, "cljc/onto",          prim_onto);
    cljc_define_native(e, "ffi-load*", prim_ffi_load);
    cljc_define_native(e, "hash-map",  prim_hash_map);
    cljc_define_native(e, "get",       prim_get);
    cljc_define_native(e, "assoc",     prim_assoc);
    cljc_define_native(e, "dissoc",    prim_dissoc);
    cljc_define_native(e, "keys",      prim_keys);
    cljc_define_native(e, "vals",      prim_vals);
    cljc_define_native(e, "contains?", prim_contains_p);
    cljc_define_native(e, "merge",     prim_merge);
    cljc_define_native(e, "map?",      prim_map_p);
    cljc_define_native(e, "set",       prim_set);
    cljc_define_native(e, "hash-set",  prim_hash_set);
    cljc_define_native(e, "disj",      prim_disj);
    cljc_define_native(e, "set?",      prim_set_p);
    cljc_define_native(e, "sorted?",   prim_sorted_p);
    cljc_define_native(e, "re-find",    prim_re_find);
    cljc_define_native(e, "cljc/re-match-front", prim_re_match_front);
    cljc_define_native(e, "re-matches", prim_re_matches);
    cljc_define_native(e, "re-seq",     prim_re_seq);
    cljc_define_native(e, "re-replace",  prim_re_replace);
    cljc_define_native(e, "re-split",    prim_re_split);
    cljc_define_native(e, "format",      prim_format);
    cljc_define_native(e, "str/replace", prim_str_replace);
    cljc_define_native(e, "Math/sqrt",  prim_sqrt);
    cljc_define_native(e, "Math/cbrt",  prim_cbrt);
    cljc_define_native(e, "Math/pow",   prim_pow);
    cljc_define_native(e, "Math/floor", prim_floor);
    cljc_define_native(e, "Math/ceil",  prim_ceil);
    cljc_define_native(e, "Math/round", prim_round);
    cljc_define_native(e, "Math/abs",   prim_math_abs);
    cljc_define_native(e, "Math/trunc", prim_trunc);
    cljc_define_native(e, "Math/sin",   prim_sin);
    cljc_define_native(e, "Math/cos",   prim_cos);
    cljc_define_native(e, "Math/tan",   prim_tan);
    cljc_define_native(e, "Math/asin",  prim_asin);
    cljc_define_native(e, "Math/acos",  prim_acos);
    cljc_define_native(e, "Math/atan",  prim_atan);
    cljc_define_native(e, "Math/atan2", prim_atan2);
    cljc_define_native(e, "Math/hypot", prim_hypot);
    cljc_define_native(e, "Math/sinh",  prim_sinh);
    cljc_define_native(e, "Math/cosh",  prim_cosh);
    cljc_define_native(e, "Math/tanh",  prim_tanh);
    cljc_define_native(e, "Math/asinh", prim_asinh);
    cljc_define_native(e, "Math/acosh", prim_acosh);
    cljc_define_native(e, "Math/atanh", prim_atanh);
    cljc_define_native(e, "Math/exp",   prim_exp);
    cljc_define_native(e, "Math/expm1", prim_expm1);
    cljc_define_native(e, "Math/log",   prim_log);
    cljc_define_native(e, "Math/log10", prim_log10);
    cljc_define_native(e, "Math/log2",  prim_log2);
    cljc_define_native(e, "Math/log1p", prim_log1p);
    cljc_define_native(e, "Math/erf",      prim_erf);
    cljc_define_native(e, "Math/erfc",     prim_erfc);
    cljc_define_native(e, "Math/gamma",    prim_gamma);
    cljc_define_native(e, "Math/loggamma", prim_loggamma);
    /* constants: bound as plain double globals, like Clojure's Math/PI field */
    env_define_root(env_root(e), intern("Math/PI", 7), mk_double(3.14159265358979323846));
    env_define_root(env_root(e), intern("Math/E",  6), mk_double(2.71828182845904523536));
    cljc_define_native(e, "rand",       prim_rand);
    cljc_define_native(e, "rand-int",   prim_rand_int);
    cljc_define_native(e, "read-string", prim_read_string);
    cljc_define_native(e, "parse-long",   prim_parse_long);
    cljc_define_native(e, "parse-double", prim_parse_double);
    cljc_define_native(e, "eval",        prim_eval);
    cljc_define_native(e, "peek",        prim_peek);
    cljc_define_native(e, "pop",         prim_pop);
    cljc_define_native(e, "empty",       prim_empty);
    cljc_define_native(e, "nil?",    prim_nil_p);
    cljc_define_native(e, "list?",   prim_list_p);
    cljc_define_native(e, "vector?", prim_vector_p);
    cljc_define_native(e, "number?", prim_number_p);
    cljc_define_native(e, "int?",    prim_int_p);
    cljc_define_native(e, "double?", prim_double_p);
    cljc_define_native(e, "string?", prim_string_p);
    cljc_define_native(e, "char?",   prim_char_p);
    cljc_define_native(e, "keyword?", prim_keyword_p);
    cljc_define_native(e, "symbol?", prim_symbol_p);
    cljc_define_native(e, "fn?",     prim_fn_p);
    cljc_define_native(e, "zero?",   prim_zero_p);
    cljc_define_native(e, "pos?",    prim_pos_p);
    cljc_define_native(e, "neg?",    prim_neg_p);
    cljc_define_native(e, "empty?",  prim_empty_p);
    cljc_define_native(e, "concat",  prim_concat);
    cljc_define_native(e, "gensym",  prim_gensym);
    cljc_define_native(e, "gc",      prim_gc);
    cljc_define_native(e, "throw",      prim_throw);
    cljc_define_native(e, "ex-info",    prim_ex_info);
    cljc_define_native(e, "ex-message", prim_ex_message);
    cljc_define_native(e, "ex-data",    prim_ex_data);
    cljc_define_native(e, "atom",    prim_atom);
    cljc_define_native(e, "deref",   prim_deref);
    cljc_define_native(e, "reset!",  prim_reset);
    cljc_define_native(e, "swap!",   prim_swap);
    cljc_define_native(e, "compare", prim_compare);
    cljc_define_native(e, "sort",    prim_sort);
    cljc_define_native(e, "sorted-set",    prim_sorted_set);
    cljc_define_native(e, "sorted-set-by", prim_sorted_set_by);
    cljc_define_native(e, "sorted-map",    prim_sorted_map);
    cljc_define_native(e, "sorted-map-by", prim_sorted_map_by);
    cljc_define_native(e, "subseq",        prim_subseq);
    cljc_define_native(e, "rsubseq",       prim_rsubseq);
    cljc_define_native(e, "vec",     prim_vec);
    cljc_define_native(e, "transient",   prim_transient);
    cljc_define_native(e, "persistent!", prim_persistent_bang);
    cljc_define_native(e, "conj!",       prim_conj_bang);
    cljc_define_native(e, "assoc!",      prim_assoc_bang);
    cljc_define_native(e, "dissoc!",     prim_dissoc);  /* shim: persistent */
    cljc_define_native(e, "disj!",       prim_disj);    /* shim: persistent */
    cljc_define_native(e, "name",    prim_name);
    cljc_define_native(e, "keyword", prim_keyword);
    cljc_define_native(e, "symbol",  prim_symbol);
    cljc_define_native(e, "quot",    prim_quot);
    cljc_define_native(e, "subs",    prim_subs);
    cljc_define_native(e, "slurp",   prim_slurp);
    cljc_define_native(e, "spit",    prim_spit);
    cljc_define_native(e, "cljc/mtime*", prim_mtime);
    cljc_define_native(e, "cljc/now-ms*", prim_now_ms);
    cljc_define_native(e, "cljc/sleep-ms*", prim_sleep_ms);
    cljc_define_native(e, "cljc/poll-fds*", prim_poll_fds);
    cljc_define_native(e, "cljc/epoch*", prim_epoch);
    cljc_define_native(e, "cljc/md5*", prim_md5);
    cljc_define_native(e, "read-line", prim_read_line);
    cljc_define_native(e, "flush", prim_flush);
    cljc_define_native(e, "cljc/isatty*", prim_isatty);
    cljc_define_native(e, "cljc/dir?*",  prim_dir_p);
    cljc_define_native(e, "cljc/list-dir*", prim_list_dir);
    cljc_define_native(e, "tcp/listen", prim_tcp_listen);
    cljc_define_native(e, "tcp/accept", prim_tcp_accept);
    cljc_define_native(e, "tcp/recv",   prim_tcp_recv);
    cljc_define_native(e, "tcp/send",   prim_tcp_send);
    cljc_define_native(e, "tcp/close",  prim_tcp_close);
    cljc_define_native(e, "tcp/connect", prim_tcp_connect);
    cljc_define_native(e, "tcp/send-some*", prim_tcp_send_some);
    cljc_define_native(e, "pr",      prim_pr);
    cljc_define_native(e, "prn",     prim_prn);
    cljc_define_native(e, "print",   prim_print);
    cljc_define_native(e, "str/upper-case",   prim_upper_case);
    cljc_define_native(e, "str/lower-case",   prim_lower_case);
    cljc_define_native(e, "str/trim",         prim_trim);
    cljc_define_native(e, "str/split",        prim_split);
    cljc_define_native(e, "str/starts-with?", prim_starts_with);
    cljc_define_native(e, "str/ends-with?",   prim_ends_with);
    cljc_define_native(e, "str/includes?",    prim_includes);
    cljc_define_native(e, "str/index-of",     prim_index_of);
    cljc_define_native(e, "str/replace-first", prim_replace_first);
    cljc_define_native(e, "bit-and", prim_bit_and);
    cljc_define_native(e, "bit-or",  prim_bit_or);
    cljc_define_native(e, "bit-xor", prim_bit_xor);
    cljc_define_native(e, "bit-not", prim_bit_not);
    cljc_define_native(e, "bit-and-not", prim_bit_and_not);
    cljc_define_native(e, "bit-shift-left",  prim_bsl);
    cljc_define_native(e, "bit-shift-right", prim_bsr);
    cljc_define_native(e, "unsigned-bit-shift-right", prim_ubsr);
    cljc_define_native(e, "bit-test",  prim_bit_test);
    cljc_define_native(e, "bit-set",   prim_bit_set);
    cljc_define_native(e, "bit-clear", prim_bit_clear);
    cljc_define_native(e, "bit-flip",  prim_bit_flip);
    cljc_define_native(e, "char", prim_char);
    cljc_define_native(e, "str/blank?",       prim_blank_p);
    cljc_eval_string(e, PRELUDE);
    /* Lazy core: shadows the eager natives so pipelines compose lazily.
     * Eager consumers (reduce, count, into, vec, sort...) realize via
     * to_seq, so finite pipelines behave identically. */
    cljc_eval_string(e,
        "(def range* range)\n"
        "(defn range\n"
        "  ([] (iterate inc 0))\n"
        "  ([n] (range* n)) ([a b] (range* a b)) ([a b s] (range* a b s)))\n"
        /* (f x) must stay deferred: forcing cell N must not compute cell
         * N+1's value (take-while boundaries, effectful fns) */
        "(defn iterate [f x] (cons x (lazy-seq (iterate f (f x)))))\n"
        "(defn map\n"
        "  ([f c] (lazy-seq (when-let [s (seq c)]\n"
        "                     (cons (f (first s)) (map f (rest s))))))\n"
        "  ([f c1 c2] (lazy-seq (let [s1 (seq c1) s2 (seq c2)]\n"
        "                         (when (and s1 s2)\n"
        "                           (cons (f (first s1) (first s2))\n"
        "                                 (map f (rest s1) (rest s2))))))))\n"
        "(defn filter [pred c]\n"
        "  (lazy-seq (when-let [s (seq c)]\n"
        "              (if (pred (first s))\n"
        "                (cons (first s) (filter pred (rest s)))\n"
        "                (filter pred (rest s))))))\n"
        "(defn take [n c]\n"
        "  (lazy-seq (when (> n 0)\n"
        "              (when-let [s (seq c)]\n"
        "                (cons (first s) (take (dec n) (rest s)))))))\n"
        "(defn take-while [pred c]\n"
        "  (lazy-seq (when-let [s (seq c)]\n"
        "              (when (pred (first s))\n"
        "                (cons (first s) (take-while pred (rest s)))))))\n"
        "(defn repeat\n"
        "  ([x] (lazy-seq (cons x (repeat x))))\n"
        "  ([n x] (take n (repeat x))))\n"
        "(defn concat\n"
        "  ([] (list))\n"
        "  ([a] (lazy-seq (seq a)))\n"
        "  ([a b] (lazy-seq (if-let [s (seq a)]\n"
        "                     (cons (first s) (concat (rest s) b))\n"
        "                     (seq b))))\n"
        "  ([a b & more]\n"
        "   (let [cat (fn cat [xys zs]\n"   /* lazy: consume one seq, then move to the next */
        "               (lazy-seq\n"
        "                 (if-let [s (seq xys)]\n"
        "                   (cons (first s) (cat (rest s) zs))\n"
        "                   (when (seq zs) (cat (first zs) (rest zs))))))]\n"
        "     (cat (concat a b) more))))\n"
        "(defn cycle [c] (lazy-seq (concat (seq c) (cycle c))))\n"
        "(defn map-xf [f]\n"
        "  (fn [rf] (fn ([] (rf)) ([acc] (rf acc)) ([acc x] (rf acc (f x)))\n"
        "             ([acc x & xs] (rf acc (apply f x xs))))))\n"   /* multi-coll transduce */
        "(defn filter-xf [pred]\n"
        "  (fn [rf] (fn ([] (rf)) ([acc] (rf acc))\n"
        "             ([acc x] (if (pred x) (rf acc x) acc)))))\n"
        "(defn take-xf [n]\n"
        "  (fn [rf]\n"
        "    (let [left (volatile! n)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x]\n"
        "         (let [k @left]\n"
        "           (vreset! left (dec k))\n"
        "           (cond (pos? (dec k)) (rf acc x)\n"
        "                 (pos? k) (ensure-reduced (rf acc x))\n"
        "                 :else (ensure-reduced acc))))))))\n"
        "(defn drop-xf [n]\n"
        "  (fn [rf]\n"
        "    (let [left (volatile! n)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (if (pos? @left) (do (vswap! left dec) acc) (rf acc x)))))))\n"
        "(defn keep-xf [f]\n"
        "  (fn [rf] (fn ([] (rf)) ([acc] (rf acc))\n"
        "             ([acc x] (let [v (f x)] (if (nil? v) acc (rf acc v)))))))\n"
        "(defn mapcat-xf [f]\n"
        "  (fn [rf]\n"
        "    (fn ([] (rf)) ([acc] (rf acc))\n"
        "      ([acc x]\n"
        "       (loop [acc acc s (seq (f x))]\n"
        "         (if s\n"
        "           (let [r (rf acc (first s))]\n"
        "             (if (reduced? r) r (recur r (next s))))\n"
        "           acc))))))\n"
        "(defn distinct-xf []\n"
        "  (fn [rf]\n"
        "    (let [seen (volatile! #{})]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (if (contains? @seen x) acc\n"
        "                     (do (vswap! seen conj x) (rf acc x))))))))\n"
        "(defn transduce\n"
        "  ([xform f coll] (transduce xform f (f) coll))\n"
        "  ([xform f init coll]\n"
        "   (let [rf (xform f)] (rf (reduce rf init coll)))))\n"
        "(defn sequence*2 [xform coll] (seq (transduce xform conj [] coll)))\n"
        "(defn eduction [& args]\n"
        "  (sequence*2 (apply comp (butlast args)) (last args)))\n"
        /* cheap tier */
        "(defn dedupe [coll]\n"
        "  (lazy-seq (when-let [s (seq coll)]\n"
        "              (cons (first s)\n"
        "                    (dedupe (drop-while (fn [x] (= x (first s))) (rest s)))))))\n"
        "(defn partition-by [f coll]\n"
        "  (lazy-seq (when-let [s (seq coll)]\n"
        "              (let [v (f (first s))\n"
        "                    run (take-while (fn [x] (= v (f x))) s)]\n"
        "                (cons run (partition-by f (drop (count run) s)))))))\n"
        "(defn split-with [pred coll]\n"
        "  [(take-while pred coll) (drop-while pred coll)])\n"
        "(defn tree-seq [branch? children root]\n"
        "  (lazy-seq (cons root (when (branch? root)\n"
        "                         (mapcat (fn [c] (tree-seq branch? children c))\n"
        "                                 (children root))))))\n"
        "(defmacro lazy-cat [& colls] `(concat ~@(map (fn [c] `(lazy-seq ~c)) colls)))\n"
        "(defn run! [f coll] (doseq [x coll] (f x)) nil)\n"
        "(defn not-any? [pred coll] (not (some pred coll)))\n"
        "(defn not-every? [pred coll] (not (every? pred coll)))\n"
        "(defn edn/read-string [s] (read-string s))\n"
        "(defn pprint [x] (prn x))\n"
        "(defn repeatedly\n"
        "  ([f] (lazy-seq (cons (f) (repeatedly f))))\n"
        "  ([n f] (take n (repeatedly f))))\n"
        "(defn drop [n c]\n"
        "  (lazy-seq (loop [n n s (seq c)]\n"
        "              (if (and (pos? n) s) (recur (dec n) (seq (rest s))) s))))\n"
        "(defn mapcat [f c]\n"
        "  (lazy-seq (when-let [s (seq c)]\n"
        "              (concat (f (first s)) (mapcat f (rest s))))))\n"
        "(defn cljc/chunked [step f c]\n"
        "  (lazy-seq (when-let [s (seq c)]\n"
        "              (let [pair (step f s 32)]\n"
        "                (cljc/onto (nth pair 0)\n"
        "                           (cljc/chunked step f (nth pair 1)))))))\n"
        "(def cljc/map2 map)\n"
        "(defn map\n"
        "  ([f c] (cljc/chunked cljc/chunk-map* f c))\n"
        "  ([f c1 c2] (cljc/map2 f c1 c2)))\n"
        "(defn filter [pred c] (cljc/chunked cljc/chunk-filter* pred c))\n"
        /* range: lazy + chunked (4096/chunk) so streaming consumers keep only a
           chunk live instead of the whole range; small bounded INTEGER ranges
           stay eager (no lazy-seq overhead in hot loops). (range) is infinite. */
        "(defn cljc/range* [start end step]\n"
        "  (lazy-seq\n"
        "    (let [pair (cljc/range-chunk* start end step 4096)\n"
        "          chunk (nth pair 0)]\n"
        "      (when (seq chunk)\n"
        "        (cljc/onto chunk (cljc/range* (nth pair 1) end step))))))\n"
        "(defn cljc/range-small? [start end step]\n"
        "  (and (integer? start) (integer? end) (integer? step) (not (zero? step))\n"
        "       (let [n (quot (- end start) step)] (and (> n 0) (<= n 4096)))))\n"
        "(defn range\n"
        "  ([] (cljc/range* 0 nil 1))\n"
        "  ([end] (range 0 end 1))\n"
        "  ([start end] (range start end 1))\n"
        "  ([start end step]\n"
        "   (if (cljc/range-small? start end step)\n"
        "     (cljc/range-eager start end step)\n"
        "     (cljc/range* start end step))))\n"
        "(defn interleave [c1 c2]\n"
        "  (lazy-seq (let [s1 (seq c1) s2 (seq c2)]\n"
        "              (when (and s1 s2)\n"
        "                (cons (first s1)\n"
        "                      (cons (first s2)\n"
        "                            (interleave (rest s1) (rest s2))))))))\n"
        /* ── transducer arities ──
         * The xf builders above become the 1-arity of the seq functions,
         * Clojure-style: (map f) is a transducer. Current impls are
         * captured under cljc/ names first, then each fn is redefined
         * with the extra arity (root redefinition is late-bound). */
        "(defn remove-xf [pred] (filter-xf (complement pred)))\n"
        "(defn take-while-xf [pred]\n"
        "  (fn [rf] (fn ([] (rf)) ([acc] (rf acc))\n"
        "             ([acc x] (if (pred x) (rf acc x) (reduced acc))))))\n"
        "(defn drop-while-xf [pred]\n"
        "  (fn [rf]\n"
        "    (let [dropping (volatile! true)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (if (and @dropping (pred x)) acc\n"
        "                     (do (vreset! dropping false) (rf acc x))))))))\n"
        "(defn map-indexed-xf [f]\n"
        "  (fn [rf]\n"
        "    (let [i (volatile! -1)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (rf acc (f (vswap! i inc) x)))))))\n"
        "(defn keep-indexed-xf [f]\n"
        "  (fn [rf]\n"
        "    (let [i (volatile! -1)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (let [v (f (vswap! i inc) x)]\n"
        "                   (if (nil? v) acc (rf acc v))))))))\n"
        "(defn dedupe-xf []\n"
        "  (fn [rf]\n"
        "    (let [prev (volatile! :cljc/xf-none)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (let [p @prev]\n"
        "                   (vreset! prev x)\n"
        "                   (if (= p x) acc (rf acc x))))))))\n"
        "(defn interpose-xf [sep]\n"
        "  (fn [rf]\n"
        "    (let [started (volatile! false)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (if @started\n"
        "                   (let [r (rf acc sep)]\n"
        "                     (if (reduced? r) r (rf r x)))\n"
        "                   (do (vreset! started true) (rf acc x))))))))\n"
        "(defn partition-all-xf [n]\n"
        "  (fn [rf]\n"
        "    (let [buf (volatile! [])]\n"
        "      (fn ([] (rf))\n"
        "        ([acc] (let [b @buf\n"
        "                     acc (if (seq b) (unreduced (rf acc b)) acc)]\n"
        "                 (rf acc)))\n"
        "        ([acc x] (let [b (conj @buf x)]\n"
        "                   (if (= n (count b))\n"
        "                     (do (vreset! buf []) (rf acc b))\n"
        "                     (do (vreset! buf b) acc))))))))\n"
        "(defn partition-by-xf [f]\n"
        "  (fn [rf]\n"
        "    (let [buf (volatile! []) pv (volatile! :cljc/xf-none)]\n"
        "      (fn ([] (rf))\n"
        "        ([acc] (let [b @buf\n"
        "                     acc (if (seq b) (unreduced (rf acc b)) acc)]\n"
        "                 (rf acc)))\n"
        "        ([acc x]\n"
        "         (let [v (f x) p @pv]\n"
        "           (vreset! pv v)\n"
        "           (if (or (= p :cljc/xf-none) (= p v))\n"
        "             (do (vswap! buf conj x) acc)\n"
        "             (let [b @buf]\n"
        "               (vreset! buf [x])\n"
        "               (rf acc b)))))))))\n"
        "(def cat\n"
        "  (fn [rf]\n"
        "    (fn ([] (rf)) ([acc] (rf acc))\n"
        "      ([acc x] (loop [acc acc s (seq x)]\n"
        "                 (if s\n"
        "                   (let [r (rf acc (first s))]\n"
        "                     (if (reduced? r) r (recur r (next s))))\n"
        "                   acc))))))\n"
        "(def cljc/map-impl map)\n"
        "(defn map\n"
        "  ([f] (map-xf f))\n"
        "  ([f c] (cljc/map-impl f c))\n"
        "  ([f c1 c2] (cljc/map-impl f c1 c2))\n"
        "  ([f c1 c2 & more]\n"        /* n-coll: (apply map list rows) transpose */
        "   (let [cs (cons c1 (cons c2 more))]\n"
        "     (lazy-seq (when (every? seq cs)\n"
        "                 (cons (apply f (map first cs))\n"
        "                       (apply map f (map rest cs))))))))\n"
        "(def cljc/filter-impl filter)\n"
        "(defn filter ([pred] (filter-xf pred)) ([pred c] (cljc/filter-impl pred c)))\n"
        "(defn remove ([pred] (remove-xf pred)) ([pred c] (cljc/filter-impl (complement pred) c)))\n"
        "(def cljc/take-impl take)\n"
        "(defn take ([n] (take-xf n)) ([n c] (cljc/take-impl n c)))\n"
        "(def cljc/drop-impl drop)\n"
        "(defn drop ([n] (drop-xf n)) ([n c] (cljc/drop-impl n c)))\n"
        "(def cljc/take-while-impl take-while)\n"
        "(defn take-while ([pred] (take-while-xf pred)) ([pred c] (cljc/take-while-impl pred c)))\n"
        "(def cljc/drop-while-impl drop-while)\n"
        "(defn drop-while ([pred] (drop-while-xf pred)) ([pred c] (cljc/drop-while-impl pred c)))\n"
        "(def cljc/keep-impl keep)\n"
        "(defn keep ([f] (keep-xf f)) ([f c] (cljc/keep-impl f c)))\n"
        "(def cljc/mapcat-impl mapcat)\n"
        "(defn mapcat\n"
        "  ([f] (mapcat-xf f))\n"
        "  ([f c] (cljc/mapcat-impl f c))\n"
        "  ([f c1 c2 & more]\n"        /* lazy: works on infinite colls */
        "   (let [cs (cons c1 (cons c2 more))]\n"
        "     (lazy-seq (when (every? seq cs)\n"
        "                 (concat (apply f (map first cs))\n"
        "                         (apply mapcat f (map rest cs))))))))\n"
        "(def cljc/map-indexed-impl map-indexed)\n"
        "(defn map-indexed ([f] (map-indexed-xf f)) ([f c] (cljc/map-indexed-impl f c)))\n"
        "(def cljc/keep-indexed-impl keep-indexed)\n"
        "(defn keep-indexed ([f] (keep-indexed-xf f)) ([f c] (cljc/keep-indexed-impl f c)))\n"
        "(def cljc/distinct-impl distinct)\n"
        "(defn distinct ([] (distinct-xf)) ([c] (cljc/distinct-impl c)))\n"
        "(def cljc/dedupe-impl dedupe)\n"
        "(defn dedupe ([] (dedupe-xf)) ([c] (cljc/dedupe-impl c)))\n"
        "(def cljc/interpose-impl interpose)\n"
        "(defn interpose ([sep] (interpose-xf sep)) ([sep c] (cljc/interpose-impl sep c)))\n"
        "(def cljc/partition-all-impl partition-all)\n"
        "(defn partition-all ([n] (partition-all-xf n)) ([n c] (cljc/partition-all-impl n c)))\n"
        "(def cljc/partition-by-impl partition-by)\n"
        "(defn partition-by ([f] (partition-by-xf f)) ([f c] (cljc/partition-by-impl f c)))\n"
        /* multi-coll transduce: step every coll in parallel, feeding the xform's
           reducing fn multiple inputs ((sequence (map conj) c1 c2) — meander). */
        "(defn cljc/transduce-n [xform rf init colls]\n"
        "  (let [f (xform rf)]\n"
        "    (loop [acc init css colls]\n"
        "      (if (every? seq css)\n"
        "        (let [step (apply f acc (map first css))]\n"
        "          (if (reduced? step) (f (deref step)) (recur step (map rest css))))\n"
        "        (f acc)))))\n"
        "(defn sequence\n"
        "  ([c] (seq c))\n"
        "  ([xform c] (sequence*2 xform c))\n"
        "  ([xform c1 c2 & more] (seq (cljc/transduce-n xform conj [] (list* c1 c2 more)))))\n"
        "(defn completing\n"
        "  ([f] (completing f identity))\n"
        "  ([f cf] (fn ([] (f)) ([x] (cf x)) ([x y] (f x y)))))\n"
        "(def cljc/into-impl into)\n"
        "(defn into\n"
        "  ([] [])\n"
        "  ([to] to)\n"
        "  ([to from] (cljc/into-impl to from))\n"
        "  ([to xform from] (transduce xform conj to from)))\n"
        "(defn reductions\n"
        "  ([f coll] (lazy-seq (if-let [s (seq coll)]\n"
        "                        (reductions f (first s) (rest s))\n"
        "                        (list (f)))))\n"
        "  ([f init coll]\n"
        "   (if (reduced? init)\n"
        "     (list (unreduced init))\n"
        "     (cons init (lazy-seq (when-let [s (seq coll)]\n"
        "                            (reductions f (f init (first s)) (rest s))))))))\n"
        "(defn take-nth-xf [n]\n"
        "  (fn [rf]\n"
        "    (let [i (volatile! -1)]\n"
        "      (fn ([] (rf)) ([acc] (rf acc))\n"
        "        ([acc x] (if (zero? (mod (vswap! i inc) n)) (rf acc x) acc))))))\n"
        "(defn take-nth\n"
        "  ([n] (take-nth-xf n))\n"
        "  ([n coll] (lazy-seq (when-let [s (seq coll)]\n"
        "                        (cons (first s) (take-nth n (drop n s)))))))\n"
        "(def pmap map)\n"   /* single-threaded: same results, no parallelism */
        /* peek/pop on maps: priority-map semantics — the entry with the
         * smallest value. O(n) scan; backs clojure.data.priority-map. */
        "(def cljc/peek-impl peek)\n"
        "(defn peek [c]\n"
        "  (cond\n"
        "    (map? c) (when (seq c) (apply min-key second (seq c)))\n"
        "    (cljc/queue? c) (first c)\n"            /* queue: FIFO front */
        "    :else (cljc/peek-impl c)))\n"
        "(def cljc/pop-impl pop)\n"
        "(defn pop [c]\n"
        "  (cond\n"
        "    (map? c) (dissoc c (first (peek c)))\n"
        "    (cljc/queue? c) (with-meta (vec (rest c)) {:cljc/queue true})\n"
        "    :else (cljc/pop-impl c)))\n");
    /* Tier 3: multimethods, minimal protocols, records — pure prelude. */
    cljc_eval_string(e,
        "(def cljc/multi-tables (atom {}))\n"
        /* Multimethod tables are keyed by the BARE method name (no namespace) —
           the model deftype/extend-type/reify/protocol-vectors already use. This
           is what lets a (defmulti add ..) in one ns and a (defmethod g/add ..)
           in another (via the g alias, e.g. Emmy's emmy.generic vs emmy.numbers)
           share a table: both normalize to `add`. */
        "(defn cljc/mmk [s] (if (symbol? s) (symbol (name s)) s))\n"
        /* deftype protocol methods live in a SEPARATE registry the special
           invoke/deref dispatch reads, so a library redefining a method name as
           a defmulti (which resets multi-tables for that name) can't wipe them. */
        "(def cljc/deftype-methods (atom {}))\n"
        "(defn cljc/reg-method! [mname tkw f]\n"
        "  (swap! cljc/multi-tables update mname assoc tkw f)\n"
        "  (swap! cljc/deftype-methods update mname assoc tkw f))\n"
        /* Clojure signature: (defmulti name docstring? attr-map? dispatch-fn
           & options). Skip a leading docstring/attr-map; the next form is the
           dispatch fn; remaining key-vals are options (we honour :default). */
        "(defmacro defmulti [name & args]\n"
        /* peel reader metadata off the name (e.g. (defmulti ^:no-doc add ..) —
         * Emmy's defgeneric does this) so the table key matches defmethod's bare
         * name; otherwise methods register under `add` but dispatch reads the
         * (with-meta add ..) form's empty table. */
        "  (let [name (loop [n name] (if (and (seq? n) (= 'with-meta (first n))) (recur (second n)) n))\n"
        "        key (cljc/mmk name)\n"
        "        args (if (string? (first args)) (rest args) args)\n"
        "        args (if (map? (first args)) (rest args) args)\n"
        "        dispatch (first args)\n"
        "        opts (apply hash-map (rest args))\n"
        "        default (get opts :default :default)]\n"
        /* preserve any methods already registered under this name: cljc's flat
           globals share the table between deftype protocol methods and a later
           (defmulti same-name) a library may declare (e.g. SCI redefining deref);
           resetting would wipe deftype deref/invoke/etc. methods. */
        "  `(do (swap! cljc/multi-tables update '~key (fn [t#] (or t# {})))\n"
        "       (def ~name\n"
        "         (with-meta\n"   /* carry the table key so (defmethod multifn ..) on a
                                  * runtime multimethod value can find it (Emmy defunary) */
        "         (let [d# ~dispatch]\n"
        "           (fn [& args#]\n"
        "             (let [t# (get @cljc/multi-tables '~key)\n"
        "                   dv# (apply d# args#)\n"
        "                   m# (or (get t# dv#) (cljc/multi-find t# dv#) (get t# ~default))]\n"
        "               (if m#\n"
        "                 (apply m# args#)\n"
        "                 (throw (ex-info (str \"No method in \" '~name\n"
        "                                      \" for \" (pr-str dv#)) {}))))))\n"
        "         {:cljc/mm-key '~key})))))\n"
        "(defmacro defmethod [name dval params & body]\n"
        /* peel reader metadata off the multimethod name (e.g. defgeneric passes
           ^:no-doc add): otherwise ~name evaluates to (with-meta add ..), losing
           the :cljc/mm-key and keying the method under the wrong table entry */
        "  (let [name (loop [n name] (if (and (seq? n) (= 'with-meta (first n))) (recur (second n)) n))]\n"
        /* skip a host-class dispatch value (dotted / $-inner-class) that cljc
           can't resolve — it would never match anyway; bare typos still error */
        "  (if (and (symbol? dval)\n"
        "           (or (str/includes? (str dval) \".\") (str/includes? (str dval) \"$\"))\n"
        "           (not (cljc/resolve-maybe (str dval))))\n"
        "    nil\n"
        /* Clojure evaluates the multifn (so it can be a runtime value, e.g. Emmy's
       defunary passes the multimethod as a fn arg). Read its table key from the
       dispatch fn's metadata; fall back to the literal name for a plain symbol. */
    "    `(let [mm# ~name\n"
        "           key# (or (:cljc/mm-key (meta mm#)) (cljc/mmk '~name))]\n"
        "       (swap! cljc/multi-tables update key# assoc ~dval (fn ~params ~@body))\n"
        "       mm#))))\n"
        /* multimethod API. The table key lives in the multifn's :cljc/mm-key
           meta (set by defmulti/read by defmethod) — introspection must use the
           SAME key, not (cljc/mmk mm) on the fn value (which differs). */
        "(defn cljc/mm-key [mm] (or (:cljc/mm-key (meta mm)) (cljc/mmk mm)))\n"
        "(def cljc/multi-prefs (atom {}))\n"
        "(defn prefer-method [mm x y]\n"
        "  (swap! cljc/multi-prefs update (cljc/mm-key mm)\n"
        "         (fn [p] (update (or p {}) x (fn [s] (conj (or s #{}) y))))) mm)\n"
        "(defn prefers [mm] (get @cljc/multi-prefs (cljc/mm-key mm) {}))\n"
        "(defn remove-method [mm dval] (swap! cljc/multi-tables update (cljc/mm-key mm) dissoc dval) mm)\n"
        "(defn remove-all-methods [mm] (swap! cljc/multi-tables assoc (cljc/mm-key mm) {}) mm)\n"
        "(defn get-method [mm dval] (get (get @cljc/multi-tables (cljc/mm-key mm)) dval))\n"
        "(defn methods [mm] (get @cljc/multi-tables (cljc/mm-key mm)))\n"
        /* print-method/print-dup: Clojure printing multimethods libraries extend
           (cljc prints via print_to; these just need to exist + be defmethod-able) */
        "(defmulti print-method (fn [x & _] (type x)))\n"
        "(defmulti print-dup (fn [x & _] (type x)))\n"
        /* minimal clojure.pprint: simple-dispatch is a multimethod libraries
           (defmethod ..) on to customize printing; pprint just prints. */
        "(defmulti clojure.pprint/simple-dispatch type)\n"
        "(defmethod clojure.pprint/simple-dispatch :default [x] (pr x))\n"
        "(defn clojure.pprint/pprint [x & _] (prn x))\n"
        "(def clojure.pprint/*print-right-margin* 72)\n"
        /* protocols: each method dispatches on (type (first args)) */
        /* drop an optional protocol docstring (and any non-list sig); each method
           sig is (name [args].. doc?), so only `name` matters for the defmulti */
        "(defmacro defprotocol [pname & sigs]\n"
        "  (let [sigs (filter list? sigs)]\n"
        "  `(do ~@(map (fn [sig]\n"
        "                (let [m (first sig)]\n"
        "                  `(defmulti ~m (fn [& args#] (type (first args#))))))\n"
        "              sigs)\n"
        "       (def ~pname '~(mapv first sigs)))))\n"
        /* definterface: cljc has no host interfaces — bind the name to a type
           keyword so deftype/instance?/extend references to it resolve. */
        "(defmacro definterface [iname & sigs] `(def ~iname ~(keyword (str iname))))\n"
        "(defmacro extend-type [t & impls]\n"
        /* Skip interleaved protocol-name symbols (Clojure syntax: (extend-type
           T Proto (m [this] ..) ..)); cljc dispatches per method. A keyword type
           (cljc-native, e.g. :string) dispatches directly; a symbol type (host
           class / deftype name) is resolved to its dispatch value at runtime and
           SKIPPED if unresolved — so extensions to classes/protocol-interfaces
           cljc lacks are quietly ignored rather than erroring. */
        /* group arities by method name so a multi-arity protocol method
           (e.g. IFn invoke) becomes one multi-arity fn, not colliding entries */
        "  (let [ms (filter list? impls)\n"
        "        groups (vals (group-by first ms))]\n"
        /* nil is a real dispatch value ((type nil) => nil), so (extend-type nil ..)
           — core.logic extends nil for IBind/IMPlus/ITake — registers under it. */
        "    (if (or (keyword? t) (nil? t))\n"
        "      `(do ~@(map (fn [g]\n"
        "                    (let [m (first (first g))\n"
        "                          arities (mapcat (fn [form] (if (vector? (second form))\n"
        "                                                       [`(~(vec (second form)) ~@(drop 2 form))]\n"
        "                                                       (map (fn [a] `(~(vec (first a)) ~@(rest a))) (rest form)))) g)]\n"
        "                      `(cljc/reg-method! '~m ~t (fn ~@arities))))\n"
        "                  groups))\n"
        "      `(when-let [tv# (cljc/resolve-maybe '~t)]\n"
        "         ~@(map (fn [g]\n"
        "                  (let [m (first (first g))\n"
        "                        arities (mapcat (fn [form] (if (vector? (second form))\n"
        "                                                     [`(~(vec (second form)) ~@(drop 2 form))]\n"
        "                                                     (map (fn [a] `(~(vec (first a)) ~@(rest a))) (rest form)))) g)]\n"
        "                    `(cljc/reg-method! '~m tv# (fn ~@arities))))\n"
        "                groups)))))\n"
        /* (extend-protocol P T1 (m [x] ...) (m2 [x] ...) T2 (m [x] ...)) —
           grouped by TYPE; expands to one extend-type per type. The protocol
           name is ignored (cljc methods dispatch globally on type). */
        "(defmacro extend-protocol [p & specs]\n"
        "  (let [groups (loop [s specs acc [] cur nil]\n"
        "                 (cond\n"
        "                   (empty? s) (if cur (conj acc cur) acc)\n"
        "                   (list? (first s))\n"            /* method impl -> current group */
        "                     (recur (rest s) acc (conj cur (first s)))\n"
        "                   :else\n"                        /* a type symbol/keyword -> new group */
        "                     (recur (rest s) (if cur (conj acc cur) acc) [(first s)])))]\n"
        "    `(do ~@(map (fn [g] `(extend-type ~(first g) ~@(rest g))) groups))))\n"
        /* (extend AType AProtocol {:method (fn [this ..] ..) ..} ..) — the
           low-level extend fn. Registers each method-map entry as a method on
           AType (dispatch value); the protocol arg is ignored. */
        "(defn extend [t & proto+maps]\n"
        "  (doseq [pm (partition 2 proto+maps)]\n"
        "    (doseq [e (second pm)]\n"
        "      (swap! cljc/multi-tables update (symbol (name (first e))) assoc t (second e)))))\n"
        /* A type satisfies a protocol if it participates in it. cljc tracks
           per-method registration, so test whether x's type implements ANY of
           the protocol's methods — a reify/extend may legally omit some (they'd
           throw only if called), and `every?` would wrongly reject it. */
        "(defn satisfies? [proto x]\n"
        "  (boolean (some (fn [m] (contains? (get @cljc/multi-tables (cljc/mmk m)) (type x))) proto)))\n"
        /* records: maps tagged with :cljc/type */
        /* defrecord: like deftype but immutable (fields are direct values, no
           atoms) and with both ->R (positional) and map->R (from a map) ctors.
           Protocol method bodies are field-rewritten and registered like
           deftype's. */
        "(defmacro defrecord [rname fields & specs]\n"
        "  (let [rname (cljc/field-sym rname)\n"
        "        kw    (keyword (str rname))\n"
        "        fsyms (mapv cljc/field-sym fields)\n"
        "        fset  (set fsyms)\n"
        "        ms    (filter list? specs)\n"
        "        kvs   (mapcat (fn [s] [(keyword (str s)) s]) fsyms)]\n"
        "    `(do\n"
        "       (def ~rname ~kw)\n"
        "       (swap! cljc/record-types conj ~kw)\n"   /* records ARE maps; deftypes aren't */
        "       (defn ~(symbol (str \"->\" rname)) ~fsyms (hash-map :cljc/type ~kw ~@kvs))\n"
        "       (def ~(symbol (str rname \".\")) ~(symbol (str \"->\" rname)))\n"
        "       (defn ~(symbol (str \"map->\" rname)) [m] (assoc m :cljc/type ~kw))\n"
        "       ~@(map (fn [g]\n"
        "                (let [mn (first g)\n"
        "                      arities (map (fn [m]\n"
        "                                     (let [oparams (vec (second m)) othis (first oparams)\n"
        "                                           this (if (= othis '_) (gensym \"this\") othis)\n"
        "                                           params (assoc oparams 0 this)\n"
        "                                           body (map (fn [x] (cljc/deftype-walk x fset #{} this)) (drop 2 m))]\n"
        "                                       `(~params ~@body)))\n"
        "                                   (second g))]\n"
        "                  `(cljc/reg-method! '~mn ~kw (fn ~@arities))))\n"
        "              (group-by first ms))\n"
        "       ~kw)))\n"
        /* delay/force: a deftype whose IDeref forces the thunk once, memoized
           (cljc's deref dispatch handles @d). Defined here — after multi-tables
           and the deftype machinery exist. */
        "(deftype CljcDelay [^:unsynchronized-mutable f ^:unsynchronized-mutable v]\n"
        "  clojure.lang.IDeref\n"
        "  (deref [_] (when f (set! v (f)) (set! f nil)) v))\n"
        "(defmacro delay [& body] `(CljcDelay. (fn [] ~@body) nil))\n"
        "(defn delay? [x] (= :CljcDelay (type x)))\n"
        "(defn force [x] (if (= :CljcDelay (type x)) (deref x) x))\n"
        "(defn record? [x] (and (map? x) (contains? x :cljc/type)))\\n"
        /* reify: a form's type keyword t is fixed at macroexpand time, so EVERY
           instance of one (reify ..) form shares t. Methods must therefore live
           PER INSTANCE (in :cljc/impls), not in the global method table keyed by
           t — else a second instance's methods clobber the first's. We register
           one delegating dispatch method per (proto-method, t) that forwards to
           the receiver's own impl, and store each instance's closures in it. */
        "(defn cljc/reg-reify-method! [mname t]\n"
        "  (when-not (contains? (get (deref cljc/multi-tables) mname) t)\n"
        "    (swap! cljc/multi-tables update mname\n"
        "           (fn [tb] (assoc (or tb {}) t\n"
        "                       (fn [& args] (apply (get (:cljc/impls (first args)) mname) args)))))))\n"
        "(defmacro reify [& clauses]\n"
        "  (let [t (keyword (str (gensym)))\n"
        "        ms (filter list? clauses)\n"
        "        regs (map (fn [m] (list 'cljc/reg-reify-method! (list 'quote (first m)) t)) ms)\n"
        "        impls (mapcat (fn [m] (list (list 'quote (first m))\n"
        "                                    (cons 'fn (cons (vec (second m)) (drop 2 m))))) ms)\n"
        "        inst (list 'hash-map :cljc/type t :cljc/impls (cons 'hash-map impls))]\n"
        "    (concat (list 'do) regs (list inst))))\n");
    /* FFI glue generator — declare C signatures as data, compile, load:
     * (ffi/define [[:double cos [:double]] [:int getpid []]]
     *             {:headers ["math.h" "unistd.h"] :libs "-lm"}) */
    cljc_eval_string(e,
        "(def cljc/ffi-counter (atom 0))\n"
        "(def cljc/ffi-api-decl\n"
        "  (str \"typedef struct { void*(*mk_int)(long long);\"\n"
        "       \" void*(*mk_double)(double); void*(*mk_str)(const char*);\"\n"
        "       \" void*(*nil)(void); long long(*as_int)(void*);\"\n"
        "       \" double(*as_double)(void*); const char*(*as_str)(void*);\"\n"
        "       \" void*(*nth_arg)(void*,int);\"\n"
        "       \" void(*def_native)(void*,const char*,void*(*)(void*,void*,int));\"\n"
        "       \" void(*error)(const char*);\"\n"
        "       \" void*(*mk_vec)(void**,int); void*(*nth_elem)(void*,int);\"\n"
        "       \" } CljcFfiApi;\\n\"\n"
        "       \"static CljcFfiApi *api;\\n\"))\n"
        "(defn cljc/ffi-build [code libs]\n"
        "  (let [base (str \"/tmp/cljc-ffi4-\" (Math/abs (hash (str code libs))))]\n"
        "    (when-not (zero? (:exit (sh (str \"test -f \" base \".so\"))))\n"
        "      (spit (str base \".c\") code)\n"
        "      (let [r (sh (str \"cc -shared -fPIC -O2 -o \" base \".so \" base \".c \" libs))]\n"
        "        (when-not (zero? (:exit r))\n"
        "          (throw (ex-info (str \"ffi: compile failed:\\n\" (:out r)) {})))))\n"
        "    (ffi-load* (str base \".so\"))))\n"
        "(defn cljc/ffi-scalar-in [t expr]\n"
        "  (case t\n"
        "    :int (str \"api->as_int(\" expr \")\")\n"
        "    :double (str \"api->as_double(\" expr \")\")\n"
        "    :float (str \"(float)api->as_double(\" expr \")\")\n"
        "    :string (str \"api->as_str(\" expr \")\")\n"
        "    :pointer (str \"(void*)api->as_int(\" expr \")\")))\n"
        "(defn cljc/ffi-scalar-out [t expr]\n"
        "  (case t\n"
        "    :int (str \"api->mk_int(\" expr \")\")\n"
        "    :double (str \"api->mk_double(\" expr \")\")\n"
        "    :float (str \"api->mk_double((double)(\" expr \"))\")\n"
        "    :string (str \"api->mk_str(\" expr \")\")\n"
        "    :pointer (str \"api->mk_int((long long)(\" expr \"))\")))\n"
        /* A type is a scalar keyword (:int etc.) or a struct reference (a
         * string/symbol) that keys into the :structs registry. */
        "(defn cljc/ffi-ret [structs t expr]\n"
        "  (cond\n"
        "    (= t :void) (str expr \"; return api->nil();\")\n"
        "    (keyword? t) (str \"return \" (cljc/ffi-scalar-out t expr) \";\")\n"
        "    :else\n"
        "    (let [fields (get structs (str t)) n (count fields)]\n"
        "      (str \"{ \" t \" _r = \" expr \"; void *_i[\" n \"]; \"\n"
        "           (str/join \" \" (map-indexed (fn [j [ft fnm]]\n"
        "                            (str \"_i[\" j \"] = \" (cljc/ffi-scalar-out ft (str \"_r.\" fnm)) \";\"))\n"
        "                          fields))\n"
        "           \" return api->mk_vec(_i, \" n \"); }\"))))\n"
        "(defn cljc/ffi-arg [structs t i]\n"
        "  (if (keyword? t)\n"
        "    (cljc/ffi-scalar-in t (str \"api->nth_arg(args, \" i \")\"))\n"
        "    (let [fields (get structs (str t)) a (str \"api->nth_arg(args, \" i \")\")]\n"
        "      (str \"(\" t \"){\"\n"
        "           (str/join \", \" (map-indexed (fn [j [ft _]]\n"
        "                            (cljc/ffi-scalar-in ft (str \"api->nth_elem(\" a \", \" j \")\")))\n"
        "                          fields))\n"
        "           \"}\"))))\n"
        "(defn cljc/ffi-wrapper [structs sig]\n"
        "  (let [[ret cname argts] sig]\n"
        "    (str \"static void *w_\" cname \"(void *env, void *args, int nargs) { (void)env; \"\n"
        "         \"if (nargs < \" (count argts) \") api->error(\\\"\" cname \": too few args\\\"); \"\n"
        "         (cljc/ffi-ret structs ret (str cname \"(\"\n"
        "                              (str/join \", \" (map-indexed (fn [i t] (cljc/ffi-arg structs t i)) argts))\n"
        "                              \")\"))\n"
        "         \" }\\n\")))\n"
        "(defn ffi/define*\n"
        "  ([sigs] (ffi/define* sigs {}))\n"
        "  ([sigs {:keys [headers libs prefix structs] :or {headers [] libs \"\" prefix \"\" structs {}}}]\n"
        "   (let [code (str \"#define _GNU_SOURCE\\n\"\n"
        "                   (str/join \"\" (map (fn [h] (str \"#include <\" h \">\\n\")) headers))\n"
        "                   cljc/ffi-api-decl\n"
        "                   (str/join \"\" (map (fn [s] (cljc/ffi-wrapper structs s)) sigs))\n"
        "                   \"void cljc_module_init(void *env, CljcFfiApi *a) { api = a;\\n\"\n"
        "                   (str/join \"\" (map (fn [[_ cname _]]\n"
        "                                        (str \"  api->def_native(env, \\\"\" prefix cname \"\\\", w_\" cname \");\\n\"))\n"
        "                                      sigs))\n"
        "                   \"}\\n\")]\n"
        "     (cljc/ffi-build code libs))))\n"
        "(defmacro ffi/define [sigs & opts]\n"
        "  `(ffi/define* '~sigs ~@opts))\n"
        "(defn ffi/defstruct*\n"
        "  ([sname fields] (ffi/defstruct* sname fields {}))\n"
        "  ([sname fields {:keys [headers libs] :or {headers [] libs \"\"}}]\n"
        "   (let [sn (str sname)\n"
        "         pfx (str \"struct \" sn \" *p = (struct \" sn \"*)(long long)api->as_int(api->nth_arg(args,0)); \")\n"
        "         getter (fn [[t f]]\n"
        "                  (str \"static void *w_get_\" sn \"_\" f \"(void *env, void *args, int nargs) { (void)env; (void)nargs; \"\n"
        "                       pfx (cljc/ffi-ret {} t (str \"p->\" f)) \" }\\n\"))\n"
        "         setter (fn [[t f]]\n"
        "                  (str \"static void *w_set_\" sn \"_\" f \"(void *env, void *args, int nargs) { (void)env; (void)nargs; \"\n"
        "                       pfx \"p->\" f \" = \" (cljc/ffi-arg {} t 1) \"; return api->nil(); }\\n\"))\n"
        "         code (str \"#define _GNU_SOURCE\\n\"\n"
        "                   (str/join \"\" (map (fn [h] (str \"#include <\" h \">\\n\")) headers))\n"
        "                   \"#include <stdlib.h>\\n\"\n"
        "                   cljc/ffi-api-decl\n"
        "                   \"static void *w_make_\" sn \"(void *env, void *args, int nargs) { (void)env; (void)args; (void)nargs;\"\n"
        "                   \" return api->mk_int((long long)calloc(1, sizeof(struct \" sn \"))); }\\n\"\n"
        "                   (str/join \"\" (map getter fields))\n"
        "                   (str/join \"\" (map setter (remove (fn [[t _]] (= t :string)) fields)))\n"
        "                   \"void cljc_module_init(void *env, CljcFfiApi *a) { api = a;\\n\"\n"
        "                   \"  api->def_native(env, \\\"make-\" sn \"\\\", w_make_\" sn \");\\n\"\n"
        "                   (str/join \"\" (map (fn [[_ f]]\n"
        "                                        (str \"  api->def_native(env, \\\"\" sn \"-\" f \"\\\", w_get_\" sn \"_\" f \");\\n\"))\n"
        "                                      fields))\n"
        "                   (str/join \"\" (map (fn [[_ f]]\n"
        "                                        (str \"  api->def_native(env, \\\"set-\" sn \"-\" f \"!\\\", w_set_\" sn \"_\" f \");\\n\"))\n"
        "                                      (remove (fn [[t _]] (= t :string)) fields)))\n"
        "                   \"}\\n\")]\n"
        "     (cljc/ffi-build code libs))))\n"
        "(defmacro ffi/defstruct [sname fields & opts]\n"
        "  `(ffi/defstruct* '~sname '~fields ~@opts))\n"
        "(defn cljc/slurp-maybe [p] (try (slurp p) (catch Exception e nil)))\n"
    "(defn load-file [path]\n"
    "  (let [src (or (cljc/slurp-maybe path)\n"
    "                (some (fn [d] (cljc/slurp-maybe (str d \"/\" path))) *load-path*)\n"
    "                (throw (ex-info (str \"load-file: not found on *load-path*: \" path) {})))]\n"
    /* incremental: a leading (ns ..) is active when later forms are read */
    "    (cljc/eval-forms* src)))\n");
    /* Top-level user code runs in the `user` namespace (like Clojure's REPL), so
     * a top-level (:refer [reduce ..]) of a core-shadowing name registers a
     * "user/reduce" refer ALIAS instead of clobbering the global core binding.
     * The preamble above loaded with cur_reader_ns NULL, so core defs stay bare
     * and core code (NULL home-ns) never sees the user refers. `user` defs are
     * kept bare too (see env_define_root) so nothing else shifts. */
    cur_reader_ns = intern("user", 4);
    return e;
}

/* Set true when the most recent cljc_eval_string raised (vs returned nil).
 * The nREPL uses it to send eval-error instead of a bogus value=nil. */
bool cljc_eval_errored;

Cljc *cljc_eval_string(CljcEnv *env, const char *src) {
    char stack_anchor;
    cljc_set_stack_base(&stack_anchor);  /* ensure at least this frame is scanned */
    Cljc * volatile result = NIL;  /* survives the error longjmp */
    cljc_eval_errored = false;
    if (setjmp(err_jmp) != 0) { print_error(); vsp = 0; eval_sp = 0; cljc_eval_errored = true; return NIL; }
    while (*src) {
        skip_ws(&src);
        if (!*src) break;
        Cljc *form = read_form(&src);
        if (!form) break;
        result = eval(env, form);
    }
    return result;
}

void cljc_print(Cljc *v) { print(v); }

/* ───── REPL ─────────────────────────────────────────────────────────── */

#ifndef CLJC_NO_MAIN

/* Script mode: read entire stream, eval every form, print nothing but what
 * the script prints itself (babashka-style). Errors abort with status 1. */
static int run_stream(CljcEnv *env, FILE *f, const char *name) {
    /* Slurp — scripts are small; streams (stdin) can't be sized up front. */
    size_t cap = 1 << 16, len = 0;
    /* volatile: src must survive the longjmp from cljc_error intact. */
    char * volatile src = malloc(cap);
    if (!src) { fputs("out of memory\n", stderr); return 1; }
    size_t n;
    while ((n = fread(src + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *p = realloc(src, cap);
            if (!p) { free(src); fputs("out of memory\n", stderr); return 1; }
            src = p;
        }
    }
    src[len] = '\0';
    if (setjmp(err_jmp) != 0) {
        print_error();
        vsp = 0;
        eval_sp = 0;
        free(src);
        return 1;
    }
    rd_line = 1;   /* track source lines for error traces */
    err_src_text = src;            /* retained for error display */
    const char *p = src;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        Cljc *form = read_form(&p);
        if (!form) break;
        eval(env, form);
    }
    rd_line = 0;
    /* src intentionally retained: error rendering may need it later */
    return 0;
}



/* ───── Interactive REPL: line editor ────────────────────────────────── */

/* linenoise-style: raw termios, editing keys, persistent history, tab
 * completion against live root bindings, live syntax highlighting,
 * paren-balance multiline, *1 *2 *3 result history. Zero dependencies. */

#ifndef _WIN32
#include <termios.h>
#endif

#define RL_MAX 8192
#define HIST_MAX 512
static char *rl_hist[HIST_MAX];
static int rl_hist_n;

static void hist_load(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.cljc_history", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[RL_MAX];
    while (fgets(line, sizeof line, f) && rl_hist_n < HIST_MAX) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n) rl_hist[rl_hist_n++] = strdup(line);
    }
    fclose(f);
}

static void hist_add(const char *line) {
    if (!*line) return;
    if (rl_hist_n && !strcmp(rl_hist[rl_hist_n-1], line)) return;
    if (rl_hist_n == HIST_MAX) {
        free(rl_hist[0]);
        memmove(rl_hist, rl_hist + 1, sizeof(char *) * (HIST_MAX - 1));
        rl_hist_n--;
    }
    rl_hist[rl_hist_n++] = strdup(line);
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.cljc_history", home);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

/* Index of the bracket matching `target`; -1 = none, -2 = orphan closer.
 * *mismatch set when the pair's TYPES disagree: ( closed by ] etc. */
static bool rl_pair_ok(char open, char close) {
    return (open == '(' && close == ')') ||
           (open == '[' && close == ']') ||
           (open == '{' && close == '}');
}

static long rl_match(const char *buf, long target, bool *mismatch) {
    long stack[256];
    int sp = 0;
    bool in_str = false, in_com = false;
    *mismatch = false;
    for (long i = 0; buf[i]; i++) {
        char c = buf[i];
        if (in_com) { if (c == '\n') in_com = false; continue; }
        if (in_str) {
            if (c == '\\' && buf[i+1]) i++;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == ';') in_com = true;
        else if (strchr("([{", c)) { if (sp < 256) stack[sp++] = i; }
        else if (strchr(")]}", c)) {
            if (sp > 0) {
                long open = stack[--sp];
                if (i == target || open == target) {
                    *mismatch = !rl_pair_ok(buf[open], c);
                    return i == target ? open : i;
                }
            } else if (i == target) return -2;   /* closer with no opener */
        }
    }
    return -1;
}

/* Render the buffer with syntax highlighting into out; the bracket pair
 * (hl_a, hl_b) renders in reverse video — red when hl_bad (mismatched
 * types or an orphan closer). */
static void rl_highlight(const char *buf, SBuf *out, long hl_a, long hl_b,
                         bool hl_bad) {
    const char *p = buf;
    while (*p) {
        long off = (long)(p - buf);
        if ((off == hl_a || off == hl_b) && strchr("()[]{}", *p)) {
            sb_puts(out, hl_bad ? "\x1b[7;31;1m" : "\x1b[7;1m");
            sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
            continue;
        }
        if (*p == ';') {                                /* comment */
            sb_puts(out, "\x1b[2m");
            while (*p) sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
        } else if (*p == '"') {                          /* string */
            sb_puts(out, "\x1b[32m");
            sb_putc(out, *p++);
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) { sb_putc(out, *p++); }
                sb_putc(out, *p++);
            }
            if (*p) sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
        } else if (*p == ':' && is_sym_char((unsigned char)p[1])) {  /* keyword */
            sb_puts(out, "\x1b[36m");
            while (is_sym_char((unsigned char)*p)) sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
        } else if (isdigit((unsigned char)*p) ||
                   ((*p == '-' || *p == '+') && isdigit((unsigned char)p[1]))) {
            sb_puts(out, "\x1b[33m");                    /* number */
            sb_putc(out, *p++);
            while (is_sym_char((unsigned char)*p)) sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
        } else if (strchr("()[]{}", *p)) {               /* delimiters */
            sb_puts(out, "\x1b[2m");
            sb_putc(out, *p++);
            sb_puts(out, "\x1b[0m");
        } else sb_putc(out, *p++);
    }
}

static void rl_refresh_opt(const char *prompt, const char *buf, size_t pos,
                           bool show_match) {
    /* bracket match: closer just typed/behind cursor, or opener at cursor */
    long hl_a = -1, hl_b = -1;
    if (show_match && pos > 0 && strchr(")]}", buf[pos - 1])) hl_a = (long)pos - 1;
    else if (!show_match) { /* accepted line: no lingering highlight */ }
    else if (show_match && strchr("([{", buf[pos] ? buf[pos] : ' ')) hl_a = (long)pos;
    bool hl_bad = false;
    if (hl_a >= 0) {
        hl_b = rl_match(buf, hl_a, &hl_bad);
        if (hl_b == -2) { hl_b = -1; hl_bad = true; }  /* orphan: red alone */
        else if (hl_b < 0) hl_a = -1;
    }
    SBuf out = {0};
    sb_puts(&out, "\r\x1b[K");
    sb_puts(&out, prompt);
    rl_highlight(buf, &out, hl_a, hl_b, hl_bad);
    /* cursor: return to col 0, advance past prompt + pos */
    char mv[32];
    snprintf(mv, sizeof mv, "\r\x1b[%zuC", strlen(prompt) + pos);
    sb_puts(&out, mv);
    fwrite(out.data, 1, out.len, stdout);
    fflush(stdout);
    free(out.data);
}

static void rl_refresh(const char *prompt, const char *buf, size_t pos) {
    rl_refresh_opt(prompt, buf, pos, true);
}

/* Tab completion: the symbol fragment before the cursor, against root
 * bindings + special forms. Inserts the unique completion or lists. */
static const char *rl_specials[] = {"defn", "defmacro", "let", "loop", "recur",
    "lazy-seq", "binding", "when", "cond", "quote", NULL};

static void rl_complete(char *buf, size_t *len, size_t *pos, const char *prompt) {
    size_t start = *pos;
    while (start > 0 && is_sym_char((unsigned char)buf[start-1])) start--;
    size_t fraglen = *pos - start;
    if (!fraglen) return;
    const char *matches[64];
    int nm = 0;
    for (Binding *b = gc_root_envs[0]->bindings; b && nm < 64; b = b->next) {
        if (strstr(b->name, "**") || !strncmp(b->name, "cljc/", 5)) continue;
        if (!strncmp(b->name, buf + start, fraglen)) matches[nm++] = b->name;
    }
    for (int i = 0; rl_specials[i] && nm < 64; i++)
        if (!strncmp(rl_specials[i], buf + start, fraglen)) matches[nm++] = rl_specials[i];
    if (nm == 0) return;
    /* longest common prefix of all matches */
    size_t common = strlen(matches[0]);
    for (int i = 1; i < nm; i++) {
        size_t j = 0;
        while (j < common && matches[i][j] == matches[0][j]) j++;
        common = j;
    }
    if (common > fraglen) {       /* extend the fragment */
        size_t add = common - fraglen;
        if (*len + add < RL_MAX - 1) {
            memmove(buf + *pos + add, buf + *pos, *len - *pos + 1);
            memcpy(buf + *pos, matches[0] + fraglen, add);
            *len += add;
            *pos += add;
        }
    } else if (nm > 1) {          /* show candidates */
        printf("\r\n");
        for (int i = 0; i < nm && i < 24; i++)
            printf("%s%s", i ? "  " : "", matches[i]);
        if (nm > 24) printf("  ...(%d total)", nm);
        printf("\r\n");
    }
    rl_refresh(prompt, buf, *pos);
}

/* Read one edited line; returns false on EOF (ctrl-d on empty). */
static bool rl_edit(const char *prompt, char *buf, size_t bufcap) {
#ifdef _WIN32
    /* No termios on Windows: plain line input, no in-line editing/history. */
    fputs(prompt, stdout); fflush(stdout);
    if (!fgets(buf, (int)bufcap, stdin)) return false;
    buf[strcspn(buf, "\n")] = 0;
    return true;
#else
    struct termios orig, raw;
    if (tcgetattr(0, &orig) == -1) {            /* not a tty after all */
        if (!fgets(buf, (int)bufcap, stdin)) return false;
        buf[strcspn(buf, "\n")] = 0;
        return true;
    }
    raw = orig;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    size_t len = 0, pos = 0;
    int hidx = rl_hist_n;
    char saved[RL_MAX] = "";
    buf[0] = 0;
    rl_refresh(prompt, buf, pos);
    for (;;) {
        int c = getchar();
        if (c == EOF || (c == 4 && len == 0)) {           /* ctrl-d */
            tcsetattr(0, TCSAFLUSH, &orig);
            printf("\r\n");
            return false;
        }
        if (c == '\r' || c == '\n') {
            rl_refresh_opt(prompt, buf, len, false);  /* clear match highlight */
            tcsetattr(0, TCSAFLUSH, &orig);
            printf("\r\n");
            return true;
        }
        if (c == 3) { len = pos = 0; buf[0] = 0; }        /* ctrl-c: clear */
        else if (c == 127 || c == 8) {                    /* backspace */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos + 1);
                pos--; len--;
            }
        } else if (c == 1) pos = 0;                       /* ctrl-a */
        else if (c == 5) pos = len;                       /* ctrl-e */
        else if (c == 11) { buf[pos] = 0; len = pos; }    /* ctrl-k */
        else if (c == 21) {                               /* ctrl-u */
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos; pos = 0;
        } else if (c == 23) {                             /* ctrl-w */
            size_t s = pos;
            while (s > 0 && buf[s-1] == ' ') s--;
            while (s > 0 && buf[s-1] != ' ') s--;
            memmove(buf + s, buf + pos, len - pos + 1);
            len -= pos - s; pos = s;
        } else if (c == 12) { printf("\x1b[2J\x1b[H"); }  /* ctrl-l */
        else if (c == '\t') {
            rl_complete(buf, &len, &pos, prompt);
            continue;
        } else if (c == 27) {                             /* escape sequences */
            int c1 = getchar(), c2 = getchar();
            if (c1 == '[') {
                if (c2 == 'D' && pos > 0) pos--;          /* left */
                else if (c2 == 'C' && pos < len) pos++;   /* right */
                else if (c2 == 'H') pos = 0;
                else if (c2 == 'F') pos = len;
                else if (c2 == 'A') {                     /* up: history */
                    if (hidx > 0) {
                        if (hidx == rl_hist_n) snprintf(saved, sizeof saved, "%s", buf);
                        hidx--;
                        snprintf(buf, bufcap, "%s", rl_hist[hidx]);
                        len = pos = strlen(buf);
                    }
                } else if (c2 == 'B') {                   /* down */
                    if (hidx < rl_hist_n) {
                        hidx++;
                        snprintf(buf, bufcap, "%s",
                                 hidx == rl_hist_n ? saved : rl_hist[hidx]);
                        len = pos = strlen(buf);
                    }
                } else if (c2 == '3') { (void)getchar();  /* delete key */
                    if (pos < len) {
                        memmove(buf + pos, buf + pos + 1, len - pos);
                        len--;
                    }
                }
            }
        } else if (c >= 32 && c < 127 && len < bufcap - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos] = (char)c;
            pos++; len++;
        }
        rl_refresh(prompt, buf, pos);
    }
#endif  /* _WIN32 */
}

static bool balanced(const char *s) {
    int depth = 0;
    bool in_str = false, in_com = false;
    for (const char *c = s; *c; c++) {
        if (in_com) { if (*c == '\n') in_com = false; continue; }
        if (in_str) {
            if (*c == '\\' && c[1]) c++;
            else if (*c == '"') in_str = false;
            continue;
        }
        if (*c == '"') in_str = true;
        else if (*c == ';') in_com = true;
        else if (strchr("([{", *c)) depth++;
        else if (strchr(")]}", *c)) depth--;
    }
    return depth <= 0 && !in_str;
}

/* Record a result in the absolute history: *results* grows by one and
 * (*results* n) retrieves by prompt number because vectors are callable.
 * Index 0 is a nil spacer so numbers match the prompt. (*out* was
 * deliberately avoided — Clojure reserves it for the stdout stream.) */
static void repl_record(CljcEnv *env, Cljc *result) {
    Cljc *outv = env_lookup_maybe(env, "*results*");
    if (!outv || outv->tag != CLJC_VECTOR) {
        outv = mk_empty_vec();
        outv = vec_conj1(outv, NIL);
    }
    outv = vec_conj1(outv, result);
    env_define_root(env_root(env), intern("*results*", 9), outv);
    Cljc *star2 = env_lookup_maybe(env, "*1");
    Cljc *star3 = env_lookup_maybe(env, "*2");
    if (star3) env_define_root(env_root(env), intern("*3", 2), star3);
    if (star2) env_define_root(env_root(env), intern("*2", 2), star2);
    env_define_root(env_root(env), intern("*1", 2), result);
}

static int run_repl(CljcEnv *env) {
    hist_load();
    printf("cljc %s — tab completes, ↑ history, *1 *2 *3 / (*results* n) hold results, !cmd shells out\n",
           CLJC_VERSION);
    char form[RL_MAX * 4];
    char line[RL_MAX];
    int out_n = 1;
    char prompt[48];
    for (;;) {
        form[0] = 0;
        snprintf(prompt, sizeof prompt, "cljc[%d]> ", out_n);
        if (!rl_edit(prompt, line, sizeof line)) break;
        snprintf(form, sizeof form, "%s", line);
        while (form[0] != '!' && !balanced(form)) {
            if (!rl_edit("    ...> ", line, sizeof line)) break;
            size_t fl = strlen(form);
            snprintf(form + fl, sizeof form - fl, "\n%s", line);
        }
        if (!form[0]) continue;
        hist_add(form);
        if (setjmp(err_jmp) != 0) {
            print_error();
            vsp = 0;
            eval_sp = 0;
            continue;
        }
        /* the entered form is the error-excerpt source for this input */
        err_src_text = form;
        err_src_name = "<repl>";
        rd_line = 1;
        rd_line_start = form;
        if (form[0] == '!') {                  /* shell mode: !ls -la */
            const char *cmd = form + 1;
            while (*cmd == ' ') cmd++;
            if (!*cmd) continue;
            Cljc *carg[1] = {mk_str(cmd, strlen(cmd))};
            Cljc *r = prim_sh(env, carg, 1);   /* {:exit n :out s} */
            Cljc *out, *exitc;
            if (map_find(r, mk_kw(intern("out", 3)), &out) && out->tag == CLJC_STRING)
                fputs(out->as.str, stdout);
            if (map_find(r, mk_kw(intern("exit", 4)), &exitc) &&
                exitc->tag == CLJC_INT && exitc->as.i != 0)
                printf("\x1b[31m[exit %lld]\x1b[0m\n", (long long)exitc->as.i);
            repl_record(env, r);   /* shell results are numbered too */
            out_n++;
            continue;
        }
        const char *p = form;
        while (*p) {
            skip_ws(&p);
            if (!*p) break;
            Cljc *f = read_form(&p);
            if (!f) break;
            Cljc *result = eval(env, f);
            repl_record(env, result);
            printf("\x1b[2m[%d]\x1b[0m ", out_n);
            print(result);
            putchar('\n');
            out_n++;
        }
        rd_line = 0;
    }
    return 0;
}

/* ───── nREPL server ─────────────────────────────────────────────────── */

/* Minimal nREPL-over-bencode so editors (Conjure, CIDER, Calva) can talk
 * to cljc: ./cljc --nrepl [port]. Single client at a time, blocking IO.
 * Ops: clone, describe, eval, load-file, close, ls-sessions, interrupt. */

#ifndef _WIN32   /* the server loop relies on fdopen() over a socket fd */
#include <sys/socket.h>
#include <netinet/in.h>

/* ── bencode writing ── */
static void bw_str(FILE *f, const char *s, size_t n) { fprintf(f, "%zu:", n); fwrite(s, 1, n, f); }
static void bw_cstr(FILE *f, const char *s) { bw_str(f, s, strlen(s)); }
static void bw_kv(FILE *f, const char *k, const char *v) { bw_cstr(f, k); bw_cstr(f, v); }

/* ── bencode reading: pull out the string fields we care about ── */
typedef struct { char *op, *code, *id, *session, *file; } NreplMsg;

static char *br_string(FILE *f, int first) {
    size_t n = (size_t)(first - '0');
    int c;
    while ((c = fgetc(f)) != ':' && c != EOF) n = n * 10 + (size_t)(c - '0');
    char *s = malloc(n + 1);
    if (!s || fread(s, 1, n, f) != n) { free(s); return NULL; }
    s[n] = '\0';
    return s;
}

static bool br_skip(FILE *f, int c);  /* skip any bencode value */

static bool br_skip_body(FILE *f) {  /* skip until matching 'e' (list/dict) */
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) return false;
        if (c == 'e') return true;
        if (!br_skip(f, c)) return false;
    }
}

static bool br_skip(FILE *f, int c) {
    if (c == 'i') { while ((c = fgetc(f)) != 'e') if (c == EOF) return false; return true; }
    if (c == 'l' || c == 'd') return br_skip_body(f);
    if (c >= '0' && c <= '9') { char *s = br_string(f, c); free(s); return s != NULL; }
    return false;
}

/* Read one top-level dict message; returns false on EOF/garbage. */
static bool nrepl_read(FILE *f, NreplMsg *m) {
    memset(m, 0, sizeof *m);
    int c = fgetc(f);
    if (c != 'd') return false;
    for (;;) {
        c = fgetc(f);
        if (c == 'e') return true;
        if (c < '0' || c > '9') return false;
        char *key = br_string(f, c);
        if (!key) return false;
        c = fgetc(f);
        if (c >= '0' && c <= '9') {
            char *val = br_string(f, c);
            if (!val) { free(key); return false; }
            if      (!strcmp(key, "op"))      m->op = val;
            else if (!strcmp(key, "code"))    m->code = val;
            else if (!strcmp(key, "id"))      m->id = val;
            else if (!strcmp(key, "session")) m->session = val;
            else if (!strcmp(key, "file"))    m->file = val;
            else free(val);
        } else if (!br_skip(f, c)) { free(key); return false; }
        free(key);
    }
}

static void nrepl_free(NreplMsg *m) {
    free(m->op); free(m->code); free(m->id); free(m->session); free(m->file);
}

/* ── responses: every reply echoes id + session ── */
static void resp_head(FILE *f, NreplMsg *m) {
    fputc('d', f);
    if (m->id) bw_kv(f, "id", m->id);
    bw_kv(f, "session", m->session ? m->session : "none");
}

static void resp_status(FILE *f, NreplMsg *m, const char *status) {
    resp_head(f, m);
    bw_cstr(f, "status");
    fputc('l', f); bw_cstr(f, status); fputc('e', f);
    fputc('e', f);
    fflush(f);
}

static void resp_status2(FILE *f, NreplMsg *m, const char *s1, const char *s2) {
    resp_head(f, m);
    bw_cstr(f, "status");
    fputc('l', f); bw_cstr(f, s1); bw_cstr(f, s2); fputc('e', f);
    fputc('e', f);
    fflush(f);
}

static void resp_field(FILE *f, NreplMsg *m, const char *key, const char *val, size_t n) {
    resp_head(f, m);
    bw_cstr(f, key); bw_str(f, val, n);
    fputc('e', f);
    fflush(f);
}

static void nrepl_eval(FILE *out, NreplMsg *m, CljcEnv *env, const char *code) {
    /* Capture interpreter stdout/stderr into protocol messages. */
    char *obuf = NULL, *ebuf = NULL;
    size_t olen = 0, elen = 0;
    FILE *oms = open_memstream(&obuf, &olen);
    FILE *ems = open_memstream(&ebuf, &elen);
    cljc_out = oms; cljc_err = ems;
    Cljc *result = cljc_eval_string(env, code);
    cljc_out = NULL; cljc_err = NULL;
    fclose(oms); fclose(ems);
    if (olen) resp_field(out, m, "out", obuf, olen);
    if (elen) resp_field(out, m, "err", ebuf, elen);
    free(obuf); free(ebuf);
    if (cljc_eval_errored) {
        /* a throw/undefined-symbol: report eval-error, NOT a bogus value=nil */
        resp_status2(out, m, "eval-error", "done");
        return;
    }
    SBuf sb = {0};
    print_to(&sb, result, true);
    resp_field(out, m, "value", sb.data ? sb.data : "nil", sb.len);
    free(sb.data);
    resp_status(out, m, "done");
}

static void nrepl_serve_client(int fd, CljcEnv *env) {
    FILE *in = fdopen(fd, "r");
    FILE *out = fdopen(dup(fd), "w");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return; }
    static int session_counter = 0;
    NreplMsg m;
    while (nrepl_read(in, &m)) {
        const char *op = m.op ? m.op : "";
        if (!strcmp(op, "clone")) {
            char sid[32];
            snprintf(sid, sizeof sid, "cljc-session-%d", ++session_counter);
            resp_head(out, &m);
            bw_kv(out, "new-session", sid);
            bw_cstr(out, "status");
            fputc('l', out); bw_cstr(out, "done"); fputc('e', out);
            fputc('e', out);
            fflush(out);
        } else if (!strcmp(op, "describe")) {
            resp_head(out, &m);
            bw_cstr(out, "ops");
            fputc('d', out);
            const char *ops[] = {"clone","describe","eval","load-file","close",
                                 "ls-sessions","interrupt", NULL};
            for (int i = 0; ops[i]; i++) { bw_cstr(out, ops[i]); fputs("de", out); }
            fputc('e', out);
            bw_cstr(out, "versions");
            fputc('d', out);
            bw_cstr(out, "cljc"); fputc('d', out);
            bw_kv(out, "version-string", "0.1");
            fputc('e', out);
            fputc('e', out);
            bw_cstr(out, "status");
            fputc('l', out); bw_cstr(out, "done"); fputc('e', out);
            fputc('e', out);
            fflush(out);
        } else if (!strcmp(op, "eval") && m.code) {
            nrepl_eval(out, &m, env, m.code);
        } else if (!strcmp(op, "load-file") && m.file) {
            nrepl_eval(out, &m, env, m.file);
        } else if (!strcmp(op, "close") || !strcmp(op, "ls-sessions")
                   || !strcmp(op, "interrupt")) {
            resp_status(out, &m, "done");
        } else {
            resp_status(out, &m, "done");  /* unknown op: ack so clients move on */
        }
        nrepl_free(&m);
    }
    fclose(in);
    fclose(out);
}

#endif  /* _WIN32 (nREPL helpers) */

static int nrepl_server(CljcEnv *env, int port) {
#ifdef _WIN32
    /* nREPL wraps the client socket in a FILE* via fdopen(); Windows sockets
     * are not C file descriptors, so this loop can't run as-is. */
    (void)env; (void)port;
    fprintf(stderr, "nREPL server is not supported on Windows builds\n");
    return 1;
#else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 1) < 0) { perror("listen"); return 1; }
    FILE *pf = fopen(".nrepl-port", "w");   /* editors auto-discover this */
    if (pf) { fprintf(pf, "%d", port); fclose(pf); }
    fprintf(stderr, "cljc nREPL server on 127.0.0.1:%d\n", port);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        nrepl_serve_client(fd, env);   /* one client at a time */
    }
#endif  /* _WIN32 */
}

/* ───── subcommands ──────────────────────────────────────────────────── */

static void usage(FILE *f) {
    fputs(
        "cljc " CLJC_VERSION " — a small Clojure in C\n"
        "\n"
        "usage: cljc [subcommand] [args]\n"
        "\n"
        "  cljc                       interactive REPL (piped stdin: run it)\n"
        "  cljc <file.clj> [args]     run a script (args land in *args*)\n"
        "\n"
        "subcommands:\n"
        "  run <file> [args]          run a script (explicit form)\n"
        "  -m <namespace> [args]      require the ns, call its -main (like bb -m)\n"
        "  eval <expr...>             evaluate, print the last value (alias -e)\n"
        "  repl                       interactive REPL\n"
        "  nrepl [port]               nREPL server for editors (default 7888)\n"
        "  notebook <file|dir> [port] live literate notebook (default 7878);\n"
        "                             dir: any .clj saved in the tree is shown\n"
        "  notebook <file> -o <html>  static notebook build\n"
        "  test [files...]            load files, run deftests, exit 1 on failure\n"
        "  judge [-a|-i] <files...>   inline snapshot tests: fill in/verify\n"
        "                             (test expr) results; -a apply, -i review\n"
        "  lint [files...]            reader syntax check, full error rendering\n"
        "  fmt [check] <files...>     format with cljfmt (fix in place; check only)\n"
        "  bundle <file> <out>        script + runtime → one native binary\n"
        "  version                    print version\n"
        "  help                       this text\n",
        f);
}

static void set_args(CljcEnv *env, int argc, char **argv, int from) {
    Cljc *as = mk_empty_vec();
    for (int i = from; i < argc; i++)
        as = vec_conj1(as, mk_str(argv[i], strlen(argv[i])));
    env_define_root(env, intern("*args*", 6), as);
}

/* Run an internal clj program string. Errors render and exit 1; with
 * truthy_exit, a falsy final value also exits 1 (e.g. run-tests). */
static int run_subprogram(CljcEnv *env, const char *src, bool truthy_exit) {
    if (setjmp(err_jmp) != 0) { print_error(); vsp = 0; eval_sp = 0; return 1; }
    const char *p = src;
    Cljc *volatile last = TRUE;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        Cljc *form = read_form(&p);
        if (!form) break;
        last = eval(env, form);
    }
    return (truthy_exit && (last == NIL || last == FALSE)) ? 1 : 0;
}

/* `cljc lint`: reader-level syntax check, Elm-style errors with carets. */
static int lint_file(CljcEnv *env, const char *path) {
    (void)env;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "lint: cannot open %s\n", path); return 1; }
    size_t cap = 1 << 16, len = 0, n;
    char *src = malloc(cap);
    if (!src) { fclose(f); return 1; }
    while ((n = fread(src + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) { cap *= 2; src = realloc(src, cap); if (!src) { fclose(f); return 1; } }
    }
    src[len] = '\0';
    fclose(f);
    err_src_name = path;
    err_src_text = src;             /* retained: error rendering may use it */
    rd_line = 1;
    if (setjmp(err_jmp) != 0) { print_error(); vsp = 0; eval_sp = 0; rd_line = 0; return 1; }
    const char *p = src;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        if (!read_form(&p)) break;
    }
    rd_line = 0;
    return 0;
}

static int run_script(CljcEnv *env, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    err_src_name = path;
    int rc = run_stream(env, f, path);
    fclose(f);
    return rc;
}

int main(int argc, char **argv) {
    /* Deep lazy chains recurse eval on the C stack; give it room. The
     * kernel grows the main stack on demand up to the soft limit, and
     * the conservative GC only scans the used portion. (The iterative
     * eval that removes this need is future bytecode-VM work.) */
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        rlim_t want = 1024ul * 1024 * 1024;   /* 1 GB */
        if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
        if (want > rl.rlim_cur) { rl.rlim_cur = want; setrlimit(RLIMIT_STACK, &rl); }
    }
#endif
    cljc_set_stack_base(&argc);  /* top-of-stack anchor for conservative GC */
    CljcEnv *env = cljc_new_env();
    const char *cmd = argc > 1 ? argv[1] : NULL;

    if (!cmd) {
        set_args(env, argc, argv, 2);
        if (!isatty(0)) { err_src_name = "<stdin>"; return run_stream(env, stdin, "<stdin>"); }
        return run_repl(env);
    }

    /* Exact subcommand names win; `cljc run <file>` is the escape hatch
     * for a script file that happens to be named like one. */
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("cljc %s\n", CLJC_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "repl")) {
        set_args(env, argc, argv, 2);
        return run_repl(env);
    }
    if (!strcmp(cmd, "nrepl") || !strcmp(cmd, "--nrepl"))
        return nrepl_server(env, argc > 2 ? atoi(argv[2]) : 7888);
    if (!strcmp(cmd, "run")) {
        if (argc < 3) { fputs("usage: cljc run <file.clj> [args]\n", stderr); return 1; }
        set_args(env, argc, argv, 3);
        return run_script(env, argv[2]);
    }
    if (!strcmp(cmd, "-m") || !strcmp(cmd, "--main")) {
        /* cljc -m <ns> [args] — require the namespace, then call its -main
         * with the remaining args (as strings), like `bb -m` / `clojure -m`. */
        if (argc < 3) { fputs("usage: cljc -m <namespace> [args]\n", stderr); return 1; }
        const char *ns = argv[2];
        set_args(env, argc, argv, 3);   /* args after the ns land in *args* */
        char prog[1024];
        snprintf(prog, sizeof prog,
                 "(require '[%s])"
                 "(let [m (or (cljc/resolve-maybe \"%s/-main\")"
                 "            (cljc/resolve-maybe \"-main\"))]"
                 "  (if m (apply m *args*)"
                 "    (throw (ex-info \"no -main found in %s\" {}))))",
                 ns, ns, ns);
        return run_subprogram(env, prog, false);
    }
    if (!strcmp(cmd, "eval") || !strcmp(cmd, "-e")) {
        if (argc < 3) { fputs("usage: cljc eval <expr...>\n", stderr); return 1; }
        set_args(env, argc, argv, 3);
        if (setjmp(err_jmp) != 0) { print_error(); return 1; }
        Cljc *volatile last = NIL;
        for (int i = 2; i < argc; i++) {
            const char *p = argv[i];
            while (*p) {
                skip_ws(&p);
                if (!*p) break;
                Cljc *form = read_form(&p);
                if (!form) break;
                last = eval(env, form);
            }
        }
        if (last != NIL) { print(last); fputc('\n', stdout); }
        return 0;
    }
    if (!strcmp(cmd, "notebook") || !strcmp(cmd, "clerk")) {
        set_args(env, argc, argv, 2);
        return run_subprogram(env, "(load-file \"clerk.clj\") (clerk/main)", false);
    }
    if (!strcmp(cmd, "test")) {
        set_args(env, argc, argv, 2);
        return run_subprogram(env,
            "(load-file \"test.clj\")"
            "(doseq [f *args*] (load-file f))"
            "(run-tests)", true);
    }
    if (!strcmp(cmd, "lint")) {
        if (argc < 3) { fputs("usage: cljc lint <files...>\n", stderr); return 1; }
        int bad = 0;
        for (int i = 2; i < argc; i++) bad += lint_file(env, argv[i]);
        if (!bad) printf("%d file%s, no reader errors\n", argc - 2, argc == 3 ? "" : "s");
        return bad ? 1 : 0;
    }
    if (!strcmp(cmd, "fmt")) {
        /* thin wrapper over cljfmt (https://github.com/weavejester/cljfmt):
         *   cljc fmt <files>        -> cljfmt fix <files>   (format in place)
         *   cljc fmt check <files>  -> cljfmt check <files>  (verb passes through)
         * exec replaces this process, so cljfmt's streams and exit code are ours. */
        int base = 2;
        const char *verb = "fix";
        if (argc > 2 && (!strcmp(argv[2], "fix") || !strcmp(argv[2], "check"))) {
            verb = argv[2]; base = 3;
        }
        int n = argc - base;
        if (n > 4090) { fputs("cljc fmt: too many arguments\n", stderr); return 1; }
        char *av[4096];
        int k = 0;
        av[k++] = (char *)"cljfmt";
        av[k++] = (char *)verb;
        for (int i = base; i < argc; i++) av[k++] = argv[i];
        av[k] = NULL;
        execvp("cljfmt", av);
        fprintf(stderr,
            "cljc fmt: cljfmt not found on PATH.\n"
            "  cljc fmt wraps cljfmt; install the native binary from\n"
            "  https://github.com/weavejester/cljfmt (or `brew install cljfmt`).\n");
        return 127;
    }
    if (!strcmp(cmd, "judge")) {
        /* inline snapshot tests; judge/main returns the exit code (2 on
         * load errors so editors can tell them from test failures) */
        set_args(env, argc, argv, 2);
        if (setjmp(err_jmp) != 0) { print_error(); return 2; }
        /* require (not load-file) so the file's `(ns judge)` is active during
         * the READ — its bare defns get home-ns "judge", so the runner's
         * internal bare references resolve to judge/… . */
        const char *prog = "(require '[judge]) (apply judge/-main *args*)";
        Cljc *last = NIL;
        while (*prog) {
            skip_ws(&prog);
            if (!*prog) break;
            Cljc *f2 = read_form(&prog);
            if (!f2) break;
            last = eval(env, f2);
        }
        return (last != NIL && last->tag == CLJC_INT) ? (int)last->as.i : 0;
    }
    if (!strcmp(cmd, "bundle")) {
        if (argc != 4) { fputs("usage: cljc bundle <script.clj> <output>\n", stderr); return 1; }
        set_args(env, argc, argv, 2);
        return run_subprogram(env, "(load-file \"bundle.clj\")", false);
    }
    if (cmd[0] == '-') {            /* unknown flag: complain, show help */
        fprintf(stderr, "cljc: unknown option %s\n\n", cmd);
        usage(stderr);
        return 1;
    }
    /* default: treat as a script path */
    set_args(env, argc, argv, 2);
    return run_script(env, cmd);
}
#endif
