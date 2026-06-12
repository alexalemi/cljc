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
#include <setjmp.h>
#include <math.h>
#include <time.h>
#include <unistd.h>      /* isatty, close — REPL detection, tcp primitives */
#include <sys/stat.h>    /* stat — file mtimes (clerk file watching) */
#include <dirent.h>      /* opendir — directory walking (clerk dir mode) */
#include <sys/socket.h>  /* tcp primitives (clerk notebook server) */
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>  /* setrlimit — main() raises the stack ceiling */

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
    CLJC_FREE,      /* internal: swept cell on the free list — never user-visible */
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
        double d;
        const char *sym;
        /* Wider view of the same symbol cell: name aliases .sym; root_cache
         * memoizes the resolved ROOT binding (stable — root def mutates). */
        struct { const char *name; Binding *root_cache; const char *home_ns; } symc;
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
        /* HAMT node. kids interleaves [k1,v1,k2,v2...]; k==NULL → v is a
         * subnode. Collision nodes hold same-hash entries linearly. */
        struct { Cljc **kids; uint32_t bitmap; uint32_t chash; uint16_t nkids;
                 bool collision; uint32_t edit_id; } hnode;
        /* arities: list of (params-list . body-list) pairs; dispatch by argc */
        struct { Cljc *arities; CljcEnv *env; bool is_macro; } fn;
        CljcNativeFn native;
        struct { Cljc *value; } atom;
        struct { Cljc *thunk; Cljc *cached; bool done; } lazy;
        /* recur sentinel: up to 3 values inline (covers real loops);
         * wider recurs spill to a heap array stored in iv[0]. */
        struct { Cljc *iv[3]; uint8_t n; bool spill; } recur;
        struct { uint32_t *code; Cljc **consts;
                 uint32_t ncode; uint16_t nconst; } chunk;
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
static Cljc *to_seq(Cljc *v);
static Cljc *seq1(Cljc *v);
static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc **argv, int nargs);
static Cljc *prim_conj(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_assoc(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_dissoc(CljcEnv *env, Cljc **argv, int nargs);
static Cljc *prim_disj(CljcEnv *env, Cljc **argv, int nargs);
/* GC-rooted value stack: call arguments live here (heap argv buffers would
 * be invisible to the conservative stack scan). Frames push, call, restore. */
#define VSTACK_CAP (1u << 20)
static Cljc **vstack;
static size_t vsp;
static void vpush(Cljc *v);
void cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn);
/* Namespace aliases for the flat-global model: (require '[x.y :as m])
 * registers "m"; on lookup miss, m/foo retries as bare foo. */
#define MAX_ALIASES 64
static const char *alias_table[MAX_ALIASES];   /* alias prefix */
static const char *alias_ns[MAX_ALIASES];      /* full namespace it names */
static int n_aliases;

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
static Cljc *NIL, *TRUE, *FALSE;

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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void cljc_throw_value(Cljc *v) {
    cur_exc = v;
    snprintf(err_msg, sizeof err_msg, "uncaught exception");
    trace_snapshot();
    cljc_raise();
}

static void vpush(Cljc *v) {
    if (vsp >= VSTACK_CAP) cljc_error("value stack overflow");
    vstack[vsp++] = v;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) cljc_error("out of memory");
    return p;
}

/* ───── Constructors ─────────────────────────────────────────────────── */

static Cljc *alloc(CljcTag t) {
    Cljc *v = cell_alloc(t != CLJC_INT && t != CLJC_DOUBLE);
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
    /* While a library loads, its defs land under "ns/name" — isolation
     * from the flat globals (and from each other). */
    if (cur_reader_ns) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s", cur_reader_ns, name);
        name = intern(buf, strlen(buf));
    }
    for (Binding *b = root->bindings; b; b = b->next)
        if (b->name == name) { b->value = value; return; }
    env_define(root, name, value);
}

static Cljc *env_lookup(CljcEnv *env, const char *name) {
    for (CljcEnv *e = env; e; e = e->parent) {
        Cljc **p = env_local_find(e, name);
        if (p) return *p;
    }
    cljc_error("unable to resolve symbol: %s", name);
    return NIL;
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
static size_t gc_allocs, gc_threshold = GC_MIN_THRESHOLD;
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
            case CLJC_VECTOR:
            case CLJC_TVEC:
                for (size_t i = 0; i < v->as.vec.taillen; i++) mark_push(v->as.vec.tail[i]);
                mark_push(v->as.vec.root);          /* NULL-safe */
                break;
            case CLJC_MAP:
            case CLJC_SET:
                mark_push(v->as.map.root);          /* NULL-safe */
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

    gc_mark(NIL); gc_mark(TRUE); gc_mark(FALSE);
    gc_mark(cur_exc);  /* exception value may be in flight between throw and catch */
    for (size_t vi = 0; vi < vsp; vi++) gc_mark(vstack[vi]);
    for (int i = 0; i < gc_n_root_envs; i++) gc_mark_env(gc_root_envs[i]);

    if (gc_stack_base) {
        void *sp = &regs;
        void *lo = sp < gc_stack_base ? sp : gc_stack_base;
        void *hi = sp < gc_stack_base ? gc_stack_base : sp;
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
     * collecting half as often nearly halves GC time for 2x peak waste. */
    gc_threshold = live * 4 > GC_MIN_THRESHOLD ? live * 4 : GC_MIN_THRESHOLD;
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
        case CLJC_DOUBLE: {
            double d = v->as.d;
            if (d == (double)(int64_t)d) return mix64((uint64_t)(int64_t)d);  /* = int cross-equality */
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            return mix64(bits);
        }
        case CLJC_STRING:  return fnv1a(v->as.str, strlen(v->as.str));
        case CLJC_KEYWORD: return fnv1a(v->as.kw, strlen(v->as.kw)) ^ 0x517cc1b7u;
        case CLJC_SYMBOL:  return fnv1a(v->as.sym, strlen(v->as.sym)) ^ 0x2545f491u;
        case CLJC_LAZY:
            return cljc_hash(to_seq(v));
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
    if (i >= v->as.vec.count) cljc_error("vector index out of bounds: %zu", i);
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
    if (i > cnt) cljc_error("assoc on vector: index out of bounds");
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
    if (i > cnt) cljc_error("assoc!: index out of bounds");
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

static int64_t as_int(Cljc *v, const char *what) {
    if (v == NULL || v->tag != CLJC_INT) cljc_error("%s: expected an integer", what);
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
        bool is_float = false;
        for (size_t i = 0; i < n; i++) if (start[i] == '.' || start[i] == 'e') is_float = true;
        char buf[64]; if (n >= sizeof buf) cljc_error("number too long");
        memcpy(buf, start, n); buf[n] = '\0';
        if (is_float) return mk_double(strtod(buf, NULL));
        return mk_int(strtoll(buf, NULL, 10));
    }

    /* Keyword? */
    if (start[0] == ':') {
        if (n == 1) cljc_error("invalid token: :");
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
    if (c == '(') return read_list(p, ')');
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
    if (c == '\\') {  /* char literal => 1-char string (no char type) */
        (*p)++;
        const char *start = *p;
        while (is_sym_char((unsigned char)**p)) (*p)++;
        size_t n = (size_t)(*p - start);
        if (n == 0) { (*p)++; return mk_str(*p - 1, 1); }  /* \( etc */
        if (n == 1) return mk_str(start, 1);
        if (n == 5 && !memcmp(start, "space", 5)) return mk_str(" ", 1);
        if (n == 7 && !memcmp(start, "newline", 7)) return mk_str("\n", 1);
        if (n == 3 && !memcmp(start, "tab", 3)) return mk_str("\t", 1);
        if (n == 6 && !memcmp(start, "return", 6)) return mk_str("\r", 1);
        if (n == 8 && !memcmp(start, "formfeed", 8)) return mk_str("\f", 1);
        if (n == 9 && !memcmp(start, "backspace", 9)) return mk_str("\b", 1);
        if (start[0] == 'o' && n >= 2 && n <= 4) {   /* \oNNN octal */
            char ch = 0;
            for (size_t i = 1; i < n; i++) ch = (char)(ch * 8 + (start[i] - '0'));
            return mk_str(&ch, 1);
        }
        cljc_error("unsupported char literal");
    }
    if (c == '#' && (*p)[1] == '_') {
        *p += 2;
        read_form(p);   /* read and discard */
        return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                       mk_cons(NIL, NIL));   /* splices zero elements */
    }
    if (c == '#' && (*p)[1] == '#') {  /* ##Inf ##-Inf ##NaN */
        *p += 2;
        if (!strncmp(*p, "Inf", 3))  { *p += 3; return mk_double(INFINITY); }
        if (!strncmp(*p, "-Inf", 4)) { *p += 4; return mk_double(-INFINITY); }
        if (!strncmp(*p, "NaN", 3))  { *p += 3; return mk_double(NAN); }
        cljc_error("unknown ## literal (expected ##Inf, ##-Inf, ##NaN)");
    }
    if (c == '#' && (*p)[1] == '?') {
        /* reader conditional: keep the :cljc or :default branch.
         * #?@(...) splices the branch into the enclosing list — returned
         * as a marker cons that read_list unpacks. */
        *p += 2;
        bool splicing = **p == '@';
        if (splicing) (*p)++;
        skip_ws(p);
        if (**p != '(') cljc_error("#? expects a list");
        Cljc *clauses = read_list(p, ')');
        static const char *KW_CLJC, *KW_DEFAULT;
        if (!KW_CLJC) { KW_CLJC = intern("cljc", 4); KW_DEFAULT = intern("default", 7); }
        for (Cljc *l = clauses; l && l->tag == CLJC_LIST && l->as.cons.tail != NIL;
             l = l->as.cons.tail->as.cons.tail) {
            Cljc *k = l->as.cons.head;
            if (k->tag == CLJC_KEYWORD && (k->as.kw == KW_CLJC || k->as.kw == KW_DEFAULT)) {
                Cljc *branch = l->as.cons.tail->as.cons.head;
                if (!splicing) return branch;
                return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                               mk_cons(branch, NIL));
            }
            if (l->as.cons.tail == NIL) break;
        }
        if (splicing)  /* no branch: splice nothing */
            return mk_cons(mk_sym(intern("**reader-splice**", 17)),
                           mk_cons(NIL, NIL));
        return NIL;  /* no matching branch (divergence: nil, not nothing) */
    }
    if (c == '#' && (*p)[1] == '(') {
        /* #(...) => (fn [%1 ...] (...)); % aliases %1, %& is the rest arg */
        (*p)++;
        Cljc *body = read_form(p);
        int maxn = 0;
        bool pct = false, pctn = false, variadic = false;
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
                    pctn = true;
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
        if (pct && pctn) cljc_error("#(): use %% or %%1, not both");
        Cljc *items[11];
        size_t ni = 0;
        for (int i = 1; i <= maxn; i++) {
            char nm[4] = {'%', (char)('0' + i), 0, 0};
            items[ni++] = mk_sym(intern(i == 1 && pct ? "%" : nm, i == 1 && pct ? 1 : 2));
        }
        if (variadic) {
            items[ni++] = mk_sym(intern("&", 1));
            items[ni++] = mk_sym(intern("%&", 2));
        }
        Cljc *params = mk_vector(items, ni);
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
        const char *tag = "unquote";
        size_t taglen = 7;
        if (**p == '@') { (*p)++; tag = "unquote-splicing"; taglen = 16; }
        Cljc *form = read_form(p);
        return mk_cons(mk_sym(intern(tag, taglen)), mk_cons(form, NIL));
    }
    if (c == '@') {
        (*p)++;
        Cljc *form = read_form(p);
        return mk_cons(mk_sym(intern("deref", 5)), mk_cons(form, NIL));
    }
    return read_atom(p);
}

/* ───── Evaluator ────────────────────────────────────────────────────── */

static Cljc *eval_list(CljcEnv *env, Cljc *list) {
    /* Evaluate each element, return a fresh list. */
    Cljc *out = NIL, **tail = &out;
    for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *tail = mk_cons(eval(env, l->as.cons.head), NIL);
        tail = &(*tail)->as.cons.tail;
    }
    return out;
}

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
                destructure(scope, vec_nth(pattern, ++i), s);
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
        static const char *KW_KEYS, *KW_AS, *KW_OR;
        if (!KW_KEYS) {
            KW_KEYS = intern("keys", 4);
            KW_AS = intern("as", 2);
            KW_OR = intern("or", 2);
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
                        const char *nm = sym_name(vec_nth(spec, j), ":keys");
                        Cljc *kw = mk_kw(nm);
                        Cljc *v = NIL;
                        bool found = value != NIL && value->tag == CLJC_MAP &&
                                     map_find(value, kw, &v);
                        if (!found && defaults && defaults->tag == CLJC_MAP) {
                            Cljc *d;
                            if (map_find(defaults, vec_nth(spec, j), &d))
                                v = eval(scope, d);
                        }
                        env_define(scope, nm, v);
                    }
                    continue;
                }
                cljc_error("destructure: unsupported map directive :%s", k->as.kw);
            }
            /* {binding-form lookup-key} */
            Cljc *v = NIL;
            bool found = value != NIL && value->tag == CLJC_MAP && map_find(value, spec, &v);
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
            destructure(call, p->as.cons.tail->as.cons.head, restl);
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
static Cljc *resolve_symbol(CljcEnv *env, Cljc *form) {
    const char *name = form->as.symc.name;
    CljcEnv *e = env;
    for (; e->parent; e = e->parent) {
        Cljc **p = env_local_find(e, name);
        if (p) return *p;
    }
    Binding *cb = form->as.symc.root_cache;
    if (cb) return cb->value;
    if (form->as.symc.home_ns) {   /* library code: own ns wins */
        char buf[256];
        snprintf(buf, sizeof buf, "%s/%s", form->as.symc.home_ns, name);
        const char *qual = intern(buf, strlen(buf));
        for (Binding *b = e->bindings; b; b = b->next)
            if (b->name == qual) { form->as.symc.root_cache = b; return b->value; }
    }
    for (Binding *b = e->bindings; b; b = b->next)
        if (b->name == name) { form->as.symc.root_cache = b; return b->value; }
    /* alias fallback: m/foo -> foo when m is a registered alias */
    {
        const char *slash = strchr(name, '/');
        if (slash && slash != name) {
            const char *pre = intern(name, (size_t)(slash - name));
            for (int i = 0; i < n_aliases; i++) {
                if (alias_table[i] == pre) {
                    /* m/foo => <full-ns>/foo, falling back to bare foo
                     * (pre-isolation libs and core shims) */
                    char buf[256];
                    snprintf(buf, sizeof buf, "%s/%s", alias_ns[i], slash + 1);
                    const char *qual = intern(buf, strlen(buf));
                    const char *bare = intern(slash + 1, strlen(slash + 1));
                    /* the ns-qualified def must win over any bare global
                     * (two passes — || takes whichever was defined last) */
                    for (Binding *b = e->bindings; b; b = b->next)
                        if (b->name == qual) {
                            form->as.symc.root_cache = b;
                            return b->value;
                        }
                    for (Binding *b = e->bindings; b; b = b->next)
                        if (b->name == bare) {
                            form->as.symc.root_cache = b;
                            return b->value;
                        }
                    break;
                }
            }
        }
    }
    err_token = name;
    cljc_error("I don't know what `%s` refers to.", name);
    return NIL;
}

/* ───── Bytecode VM ──────────────────────────────────────────────────────
 *
 * Each interpreted fn arity compiles its body to a chunk on first call
 * (cached in the arity cell's meta slot). The compiler covers the hot
 * forms — if/do/let/loop/recur/and/or/when/cond, calls, literals,
 * closures, lazy-seq — and emits VOP_EVAL (tree-walk this sub-form) for
 * everything else, so the VM is always correct without being complete.
 * Macros expand at compile time through the same apply machinery and the
 * expansion is spliced into the source form, exactly like eval does.
 *
 * The operand stack is the GC-rooted vstack; locals stay in slot-envs
 * (closures capture them unchanged); VOP_SYM resolves through the same
 * symbol cells, so root_cache caching carries over. Error traces inside
 * compiled bodies are coarser (no per-subform eval_stack frames). */

static Cljc *make_fn(CljcEnv *env, Cljc *forms, bool is_macro);

enum {
    VOP_NIL, VOP_TRUE, VOP_FALSE, VOP_CONST, VOP_SYM,
    VOP_BIND, VOP_DESTRUCT, VOP_NEWENV, VOP_POPENV, VOP_POP,
    VOP_CALL, VOP_JMP, VOP_JMPF, VOP_JMPF_KEEP, VOP_JMPT_KEEP,
    VOP_EVAL, VOP_CLOSURE, VOP_LAZY, VOP_VEC,
    VOP_REBIND, VOP_RECURFN, VOP_RET
};

typedef struct {
    uint32_t *code; uint32_t ncode, ccap;
    Cljc **consts; uint32_t nconst, kcap;
    const char *locals[256]; int nlocals;      /* compile-time scope names */
    int loop_pc;                               /* innermost loop target    */
    Cljc *loop_names;                          /* its binding symbols      */
    uint32_t loop_nbind;
    int loop_depth;                            /* NEWENVs since loop entry */
    bool ok;
} VmC;

static void vmc_emit(VmC *c, uint8_t op, uint32_t arg) {
    if (c->ncode >= c->ccap) {
        c->ccap = c->ccap ? c->ccap * 2 : 64;
        c->code = realloc(c->code, sizeof(uint32_t) * c->ccap);
        if (!c->code) cljc_error("out of memory");
    }
    c->code[c->ncode++] = (uint32_t)op | (arg << 8);
}

static uint32_t vmc_const(VmC *c, Cljc *v) {
    for (uint32_t i = 0; i < c->nconst; i++)      /* dedup (small pools) */
        if (c->consts[i] == v) return i;
    if (c->nconst >= c->kcap) {
        c->kcap = c->kcap ? c->kcap * 2 : 16;
        c->consts = realloc(c->consts, sizeof(Cljc *) * c->kcap);
        if (!c->consts) cljc_error("out of memory");
    }
    vpush(v);                  /* GC root until the chunk cell owns them */
    c->consts[c->nconst] = v;
    return c->nconst++;
}

static bool vmc_local_p(VmC *c, const char *name) {
    for (int i = c->nlocals - 1; i >= 0; i--)
        if (c->locals[i] == name) return true;
    return false;
}

static void vmc_local_push(VmC *c, const char *name) {
    if (c->nlocals < 256) c->locals[c->nlocals] = name;
    c->nlocals++;              /* may exceed 256: only tracking matters */
}

/* names bound by a destructuring pattern (for shadow tracking) */
static void vmc_pattern_locals(VmC *c, Cljc *pat) {
    if (pat == NIL) return;
    if (pat->tag == CLJC_SYMBOL) { vmc_local_push(c, pat->as.sym); return; }
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

static bool vm_special_name(const char *s) {
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

/* resolve without erroring (compile-time macro lookup) */
static Cljc *vm_resolve_maybe(CljcEnv *env, Cljc *sym) {
    Binding *cb = sym->as.symc.root_cache;
    if (cb) return cb->value;
    CljcEnv *root = env;
    while (root->parent) root = root->parent;
    for (Binding *b = root->bindings; b; b = b->next)
        if (b->name == sym->as.symc.name) return b->value;
    return NULL;
}

static void vmc_form(VmC *c, CljcEnv *cenv, Cljc *form);

static void vmc_body(VmC *c, CljcEnv *cenv, Cljc *body) {  /* do semantics */
    if (body == NIL || body->tag != CLJC_LIST) { vmc_emit(c, VOP_NIL, 0); return; }
    for (Cljc *b = body; b && b->tag == CLJC_LIST && c->ok; b = b->as.cons.tail) {
        vmc_form(c, cenv, b->as.cons.head);
        if (b->as.cons.tail != NIL && b->as.cons.tail->tag == CLJC_LIST)
            vmc_emit(c, VOP_POP, 0);
    }
}

static void vmc_form(VmC *c, CljcEnv *cenv, Cljc *form) {
    if (!c->ok) return;
    if (form == NIL) { vmc_emit(c, VOP_NIL, 0); return; }
    switch (form->tag) {
        case CLJC_BOOL:
            vmc_emit(c, form->as.b ? VOP_TRUE : VOP_FALSE, 0);
            return;
        case CLJC_INT: case CLJC_DOUBLE: case CLJC_STRING: case CLJC_KEYWORD:
        case CLJC_FN: case CLJC_NATIVE: case CLJC_MAP: case CLJC_SET:
            /* map/set literals with computed elements are rare in hot
             * bodies; constant ones are common — non-constant fall back */
            if (form->tag == CLJC_MAP || form->tag == CLJC_SET) {
                vmc_emit(c, VOP_EVAL, vmc_const(c, form));
                return;
            }
            vmc_emit(c, VOP_CONST, vmc_const(c, form));
            return;
        case CLJC_SYMBOL:
            vmc_emit(c, VOP_SYM, vmc_const(c, form));
            return;
        case CLJC_VECTOR: {
            size_t n = vec_len(form);
            if (n > 0xff) { vmc_emit(c, VOP_EVAL, vmc_const(c, form)); return; }
            for (size_t i = 0; i < n; i++) vmc_form(c, cenv, vec_nth(form, i));
            vmc_emit(c, VOP_VEC, (uint32_t)n);
            return;
        }
        case CLJC_LAZY:
            form = to_seq(form);
            if (form == NIL || form->tag != CLJC_LIST) { vmc_form(c, cenv, form); return; }
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
    if (head != NIL && head->tag == CLJC_SYMBOL && !vmc_local_p(c, head->as.sym)) {
        const char *s = head->as.sym;
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
            Cljc *cond = rest->as.cons.head;
            Cljc *then = rest->as.cons.tail != NIL ? rest->as.cons.tail->as.cons.head : NIL;
            Cljc *els = rest->as.cons.tail != NIL && rest->as.cons.tail->as.cons.tail != NIL
                        ? rest->as.cons.tail->as.cons.tail->as.cons.head : NIL;
            vmc_form(c, cenv, cond);
            uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
            vmc_form(c, cenv, then);
            uint32_t je = c->ncode; vmc_emit(c, VOP_JMP, 0);
            c->code[jf] = VOP_JMPF | (c->ncode << 8);
            vmc_form(c, cenv, els);
            c->code[je] = VOP_JMP | (c->ncode << 8);
            return;
        }
        if (s == S_DO) { vmc_body(c, cenv, rest); return; }
        if (s == S_WHEN) {
            vmc_form(c, cenv, rest->as.cons.head);
            uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
            vmc_body(c, cenv, rest->as.cons.tail);
            uint32_t je = c->ncode; vmc_emit(c, VOP_JMP, 0);
            c->code[jf] = VOP_JMPF | (c->ncode << 8);
            vmc_emit(c, VOP_NIL, 0);
            c->code[je] = VOP_JMP | (c->ncode << 8);
            return;
        }
        if (s == S_AND || s == S_OR) {
            if (rest == NIL || rest->tag != CLJC_LIST) {
                vmc_emit(c, s == S_AND ? VOP_TRUE : VOP_NIL, 0);
                return;
            }
            uint32_t jumps[128]; int nj = 0;
            for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                vmc_form(c, cenv, a->as.cons.head);
                if (a->as.cons.tail != NIL && a->as.cons.tail->tag == CLJC_LIST) {
                    if (nj >= 128) { c->ok = false; return; }
                    jumps[nj++] = c->ncode;
                    vmc_emit(c, s == S_AND ? VOP_JMPF_KEEP : VOP_JMPT_KEEP, 0);
                    vmc_emit(c, VOP_POP, 0);
                }
            }
            for (int i = 0; i < nj; i++)
                c->code[jumps[i]] = (c->code[jumps[i]] & 0xff) | (c->ncode << 8);
            return;
        }
        if (s == S_COND) {
            uint32_t ends[128]; int ne = 0;
            Cljc *a = rest;
            while (a && a->tag == CLJC_LIST && a->as.cons.tail != NIL &&
                   a->as.cons.tail->tag == CLJC_LIST) {
                vmc_form(c, cenv, a->as.cons.head);
                uint32_t jf = c->ncode; vmc_emit(c, VOP_JMPF, 0);
                vmc_form(c, cenv, a->as.cons.tail->as.cons.head);
                if (ne >= 128) { c->ok = false; return; }
                ends[ne++] = c->ncode; vmc_emit(c, VOP_JMP, 0);
                c->code[jf] = VOP_JMPF | (c->ncode << 8);
                a = a->as.cons.tail->as.cons.tail;
            }
            vmc_emit(c, VOP_NIL, 0);
            for (int i = 0; i < ne; i++)
                c->code[ends[i]] = VOP_JMP | (c->ncode << 8);
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
            for (size_t i = 0; i < vec_len(bv); i += 2) {
                Cljc *pat = vec_nth(bv, i);
                vmc_form(c, cenv, vec_nth(bv, i + 1));
                if (pat != NIL && pat->tag == CLJC_SYMBOL)
                    vmc_emit(c, VOP_BIND, vmc_const(c, pat));
                else
                    vmc_emit(c, VOP_DESTRUCT, vmc_const(c, pat));
                vmc_pattern_locals(c, pat);
            }
            vmc_body(c, cenv, rest->as.cons.tail);
            vmc_emit(c, VOP_POPENV, 0);
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
            Cljc *names = NIL, **t = &names;
            for (size_t i = 0; i < vec_len(bv); i += 2) {
                Cljc *sym = vec_nth(bv, i);
                vmc_form(c, cenv, vec_nth(bv, i + 1));
                vmc_emit(c, VOP_BIND, vmc_const(c, sym));
                vmc_local_push(c, sym->as.sym);
                *t = mk_cons(sym, NIL);
                t = &(*t)->as.cons.tail;
            }
            c->loop_names = names;
            vmc_const(c, names);              /* root it on the vstack */
            c->loop_nbind = (uint32_t)(vec_len(bv) / 2);
            c->loop_pc = (int)c->ncode;
            c->loop_depth = 0;
            vmc_body(c, cenv, rest->as.cons.tail);
            vmc_emit(c, VOP_POPENV, 0);
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
                vmc_form(c, cenv, a->as.cons.head);
                n++;
            }
            if (c->loop_pc >= 0) {
                if (n != c->loop_nbind) { c->ok = false; return; }
                if (c->loop_depth > 0xff) { c->ok = false; return; }
                vmc_emit(c, VOP_REBIND,
                         vmc_const(c, c->loop_names) | ((uint32_t)c->loop_depth << 16));
                c->code[c->ncode - 1] |= 0;   /* arg layout: k | depth<<16 */
                vmc_emit(c, VOP_JMP, (uint32_t)c->loop_pc);
            } else {
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
            vmc_emit(c, VOP_EVAL, vmc_const(c, form));
            return;
        }
        /* compile-time macro expansion, spliced like eval does */
        Cljc *mfn = vm_resolve_maybe(cenv, head);
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
            vmc_form(c, cenv, expansion);
            return;
        }
    }
    /* ordinary call: head expr, args, CALL n */
    {
        uint32_t n = 0;
        vmc_form(c, cenv, head);
        for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
            vmc_form(c, cenv, a->as.cons.head);
            n++;
        }
        vmc_emit(c, VOP_CALL, n);
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
        if (pe->tag == CLJC_SYMBOL && pe->as.sym == sym_amp()) continue;
        vmc_pattern_locals(&c, pe);
    }
    vmc_body(&c, cenv, body);
    vmc_emit(&c, VOP_RET, 0);
    if (!c.ok || c.nlocals > 256 || c.ncode > 0xffffff || c.nconst > 0xffff) {
        free(c.code); free(c.consts);
        vsp = base;
        return NULL;
    }
    Cljc *chunk = alloc(CLJC_CHUNK);   /* consts still vstack-rooted here */
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
    /* volatile: env must stay visible to the conservative GC scan */
    CljcEnv * volatile env_keep = env_in;
    CljcEnv *env = env_in;
    for (;;) {
        uint32_t ins = code[pc++];
        uint32_t a = ins >> 8;
        switch (ins & 0xff) {
            case VOP_NIL:   vpush(NIL); break;
            case VOP_TRUE:  vpush(TRUE); break;
            case VOP_FALSE: vpush(FALSE); break;
            case VOP_CONST: vpush(K[a]); break;
            case VOP_SYM:   vpush(resolve_symbol(env, K[a])); break;
            case VOP_BIND:
                env_define(env, K[a]->as.sym, vstack[vsp - 1]);
                vsp--;
                break;
            case VOP_DESTRUCT:
                destructure(env, K[a], vstack[vsp - 1]);
                vsp--;
                break;
            case VOP_NEWENV: env = env_new(env); env_keep = env; break;
            case VOP_POPENV: env = env->parent; env_keep = env; break;
            case VOP_POP:    vsp--; break;
            case VOP_CALL: {
                Cljc *f = vstack[vsp - a - 1];
                Cljc *r = apply(env, f, &vstack[vsp - a], (int)a);
                vsp -= a + 1;
                vpush(r);
                break;
            }
            case VOP_JMP:  pc = a; break;
            case VOP_JMPF: if (!is_truthy(vstack[--vsp])) pc = a; break;
            case VOP_JMPF_KEEP: if (!is_truthy(vstack[vsp - 1])) pc = a; break;
            case VOP_JMPT_KEEP: if (is_truthy(vstack[vsp - 1])) pc = a; break;
            case VOP_EVAL: vpush(eval(env, K[a])); break;
            case VOP_CLOSURE: vpush(make_fn(env, K[a], false)); break;
            case VOP_LAZY: {
                Cljc *t = make_fn(env, K[a], false);
                Cljc *l = alloc(CLJC_LAZY);
                l->as.lazy.thunk = t;
                vpush(l);
                break;
            }
            case VOP_VEC: {
                Cljc *v = mk_vector(&vstack[vsp - a], a);
                vsp -= a;
                vpush(v);
                break;
            }
            case VOP_REBIND: {
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
                break;
            }
            case VOP_RECURFN: {
                Cljc *r = alloc(CLJC_RECUR);
                Cljc **vals = r->as.recur.iv;
                if (a > 3) {
                    vals = xmalloc(sizeof(Cljc *) * a);
                    r->as.recur.iv[0] = (Cljc *)vals;
                    r->as.recur.spill = true;
                }
                for (uint32_t i = 0; i < a; i++) {
                    vals[i] = vstack[vsp - a + i];
                    r->as.recur.n = (uint8_t)(i + 1);
                }
                vsp -= a;
                vpush(r);
                break;
            }
            case VOP_RET: {
                Cljc *r = vstack[vsp - 1];
                vsp = base;
                (void)env_keep;
                return r;
            }
        }
    }
}

static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc **argv, int nargs) {
    if (fn->tag == CLJC_NATIVE) return fn->as.native(env, argv, nargs);
    if (fn->tag == CLJC_FN) {
        /* volatile: when recur swaps argv to the sentinel's (possibly heap-
         * spilled) value array, this slot is the cell's only GC root — the
         * optimizer must not elide it. */
        Cljc * volatile recur_keep = NIL;
        (void)recur_keep;
        for (;;) {
            /* Dispatch: exact param-count match wins; variadic is fallback. */
            Cljc *chosen = NULL, *fallback = NULL;
            for (Cljc *ar = fn->as.fn.arities; ar && ar->tag == CLJC_LIST; ar = ar->as.cons.tail) {
                Cljc *arity = ar->as.cons.head;
                size_t fixed; bool variadic;
                arity_info(arity->as.cons.head, &fixed, &variadic);
                if (!variadic && (size_t)nargs == fixed) { chosen = arity; break; }
                if (variadic && (size_t)nargs >= fixed && !fallback) fallback = arity;
            }
            if (!chosen) chosen = fallback;
            if (!chosen) cljc_error("no matching arity for %d args", nargs);
            CljcEnv *call = env_new(fn->as.fn.env);
            bind_params(call, chosen->as.cons.head, argv, nargs);
            /* first call compiles the body; failures pin TRUE = tree-walk */
            if (chosen->meta == NULL) {
                Cljc *ch = vm_compile(call, chosen->as.cons.head,
                                      chosen->as.cons.tail);
                chosen->meta = ch ? ch : TRUE;
            }
            Cljc *result = chosen->meta->tag == CLJC_CHUNK
                ? vm_run(call, chosen->meta)
                : eval_body(call, chosen->as.cons.tail);
            if (!(result && result->tag == CLJC_RECUR)) return result;
            /* recur: the sentinel's value array IS the next argv. */
            recur_keep = result;   /* root the cell across the next iteration */
            argv = result->as.recur.spill
                ? (Cljc **)result->as.recur.iv[0] : result->as.recur.iv;
            nargs = (int)result->as.recur.n;
        }
    }
    Cljc *a0 = nargs > 0 ? argv[0] : NIL;
    Cljc *a1 = nargs > 1 ? argv[1] : NIL;
    /* Keywords as functions: (:key m) and (:key m default). */
    if (fn->tag == CLJC_KEYWORD) {
        Cljc *out;
        if (a0 != NIL && a0->tag == CLJC_MAP && map_find(a0, fn, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_MAP) {  /* (m key default) */
        Cljc *out;
        if (map_find(fn, a0, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_SET) {
        Cljc *out;
        if (set_contains(fn, a0, &out)) return out;
        return a1;
    }
    if (fn->tag == CLJC_VECTOR || fn->tag == CLJC_TVEC) {  /* transients too */
        if (a0->tag != CLJC_INT) cljc_error("vector lookup needs an integer index");
        if (a0->as.i < 0 || (size_t)a0->as.i >= vec_len(fn))
            cljc_error("vector index out of bounds: %lld", (long long)a0->as.i);
        return vec_nth(fn, (size_t)a0->as.i);
    }
    cljc_error("not callable");
    return NIL;
}

/* One arity: validate a [params] vector and pair it with its body. */
static Cljc *build_arity(Cljc *params_vec, Cljc *body) {
    if (params_vec == NIL || params_vec->tag != CLJC_VECTOR)
        cljc_error("fn params must be a vector");
    Cljc *params = NIL, **t = &params;
    for (size_t i = 0; i < vec_len(params_vec); i++) {
        Cljc *p = vec_nth(params_vec, i);
        bool is_amp = p->tag == CLJC_SYMBOL && p->as.sym == sym_amp();
        if (is_amp && i + 2 != vec_len(params_vec))
            cljc_error("fn params: & must be followed by exactly one binding");
        if (!is_amp && p->tag != CLJC_SYMBOL && p->tag != CLJC_VECTOR && p->tag != CLJC_MAP)
            cljc_error("fn params: unsupported binding form");
        *t = mk_cons(p, NIL);
        t = &(*t)->as.cons.tail;
    }
    return mk_cons(params, body);
}

/* Build an interpreted fn from the forms after `fn` (or after a defn name):
 * single arity ([params] body...) or multi-arity (([p] b...) ([p q] b...)). */
static Cljc *make_fn(CljcEnv *env, Cljc *forms, bool is_macro) {
    /* Arities (and the chunks cached on them) are pure structure: share
     * them across every closure built from the same source forms. A hot
     * loop creating a closure per iteration otherwise RECOMPILES its
     * body each call (5M+ vm_compiles in one profile). */
    static const char *SYM_ARITIES;
    if (!SYM_ARITIES) SYM_ARITIES = intern("**arities**", 11);
    if (forms != NIL && forms->tag == CLJC_LIST && forms->meta != NULL &&
        forms->meta->tag == CLJC_LIST &&
        forms->meta->as.cons.head->tag == CLJC_SYMBOL &&
        forms->meta->as.cons.head->as.sym == SYM_ARITIES) {
        Cljc *f = alloc(CLJC_FN);
        f->as.fn.arities = forms->meta->as.cons.tail;
        f->as.fn.env = env;
        f->as.fn.is_macro = is_macro;
        return f;
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
    if (forms != NIL && forms->tag == CLJC_LIST && forms->meta == NULL)
        forms->meta = mk_cons(mk_sym(SYM_ARITIES), arities);
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
    if (!SYM_UQ) { SYM_UQ = intern("unquote", 7); SYM_UQS = intern("unquote-splicing", 16); }

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
        case CLJC_INT: case CLJC_DOUBLE: case CLJC_BOOL: case CLJC_NIL:
        case CLJC_STRING: case CLJC_KEYWORD: case CLJC_FN: case CLJC_NATIVE:
        case CLJC_ATOM: case CLJC_TVEC:
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

            /* Special forms — dispatch by interned symbol pointer. */
            if (head->tag == CLJC_SYMBOL) {
                const char *s = head->as.sym;
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
                            if (catch_clause) cljc_error("try: only one catch clause is supported");
                            if (finally_clause) cljc_error("try: catch must precede finally");
                            catch_clause = f;
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
                    /* compat no-ops: flat globals already match the universal
                     * (:require [clojure.string :as str]) alias convention */
                    if (s == SYM_USE || s == SYM_IMPORT) return NIL;
                    if (s == SYM_NS) {
                        /* (ns name (:require [a.b :as x] ...)) — load the
                         * :require clauses; everything else is tolerated. */
                        static const char *KW_REQ;
                        if (!KW_REQ) KW_REQ = intern("require", 7);
                        for (Cljc *c = rest; c && c->tag == CLJC_LIST; c = c->as.cons.tail) {
                            Cljc *cl = c->as.cons.head;
                            /* a clause is (:require ...) or [:require ...] */
                            if (cl != NIL && cl->tag == CLJC_LIST &&
                                cl->as.cons.head->tag == CLJC_KEYWORD &&
                                cl->as.cons.head->as.kw == KW_REQ) {
                                for (Cljc *sp = cl->as.cons.tail; sp && sp->tag == CLJC_LIST;
                                     sp = sp->as.cons.tail) {
                                    Cljc *callee = env_lookup_maybe(env, "cljc/require-one");
                                    if (callee) {
                                        Cljc *one[1] = {sp->as.cons.head};
                                        apply(env, callee, one, 1);
                                    }
                                }
                            } else if (cl != NIL && cl->tag == CLJC_VECTOR &&
                                       vec_len(cl) > 0 &&
                                       vec_nth(cl, 0)->tag == CLJC_KEYWORD &&
                                       vec_nth(cl, 0)->as.kw == KW_REQ) {
                                for (uint32_t vi = 1; vi < vec_len(cl); vi++) {
                                    Cljc *callee = env_lookup_maybe(env, "cljc/require-one");
                                    if (callee) {
                                        Cljc *one[1] = {vec_nth(cl, vi)};
                                        apply(env, callee, one, 1);
                                    }
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
                    (void)SYM_REQUIRE;
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
                            }
                            if (!b)
                                for (Binding *x = root->bindings; x; x = x->next)
                                    if (x->name == nm) { b = x; break; }
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
                    const char *name = sym_name(rest->as.cons.head, "defmacro");
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
                    need_args(rest, 2, "def");
                    Cljc *namef = rest->as.cons.head;
                    /* (def ^:dynamic x v): the reader wrapped the name as
                     * (with-meta x m) — unwrap; def meta is not retained. */
                    if (namef != NIL && namef->tag == CLJC_LIST &&
                        namef->as.cons.head->tag == CLJC_SYMBOL &&
                        !strcmp(namef->as.cons.head->as.sym, "with-meta") &&
                        namef->as.cons.tail != NIL)
                        namef = namef->as.cons.tail->as.cons.head;
                    const char *name = sym_name(namef, "def");
                    Cljc *valf = rest->as.cons.tail;
                    /* (def name "docstring" value): skip the docstring */
                    if (valf->as.cons.tail != NIL &&
                        valf->as.cons.tail->tag == CLJC_LIST &&
                        valf->as.cons.head != NIL &&
                        valf->as.cons.head->tag == CLJC_STRING)
                        valf = valf->as.cons.tail;
                    Cljc *val = eval(env, valf->as.cons.head);
                    env_define_root(env_root(env), name, val);  /* def is always global */
                    return val;
                }
                if (s == SYM_LET) {
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
                if (s == SYM_LOOP) {
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

                        /* Rebind in place: loop slots are mutable locals that get
                         * fresh values each pass (Clojure semantics). */
                        if (r->as.recur.n != nparams)
                            cljc_error("recur arity mismatch: expected %zu, got %d",
                                       nparams, (int)r->as.recur.n);
                        Cljc **rvals = r->as.recur.spill
                            ? (Cljc **)r->as.recur.iv[0] : r->as.recur.iv;
                        for (size_t i = 0; i < nparams; i++) {
                            Cljc **p = env_local_find(scope, names[i]);
                            if (p) *p = rvals[i];
                        }
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
                if (s == SYM_FN) {
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

/* readably=true  → pr semantics: strings get quotes (read-back form)
 * readably=false → str/print semantics: strings render raw */
static void print_to(SBuf *sb, Cljc *v, bool readably) {
    if (v == NULL || v == NIL) { sb_puts(sb, "nil"); return; }
    switch (v->tag) {
        case CLJC_NIL: sb_puts(sb, "nil"); break;
        case CLJC_BOOL: sb_puts(sb, v->as.b ? "true" : "false"); break;
        case CLJC_INT: sb_printf(sb, "%lld", (long long)v->as.i); break;
        case CLJC_DOUBLE: {
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%g", v->as.d);
            sb_puts(sb, tmp);
            /* %g prints 1.0 as "1" — append ".0" so doubles stay visually
             * distinct from ints ("inf"/"nan" pass the strpbrk too). */
            if (!strpbrk(tmp, ".ein")) sb_puts(sb, ".0");
            break;
        }
        case CLJC_SYMBOL: sb_puts(sb, v->as.sym); break;
        case CLJC_KEYWORD: sb_putc(sb, ':'); sb_puts(sb, v->as.kw); break;
        case CLJC_STRING:
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
        case CLJC_LIST: {
            sb_putc(sb, '(');
            bool first = true;
            /* seq1 step: a lazy tail (cons onto lazy-seq) keeps printing */
            for (Cljc *l = v; l && l->tag == CLJC_LIST; l = seq1(l->as.cons.tail)) {
                if (!first) sb_putc(sb, ' ');
                first = false;
                print_to(sb, l->as.cons.head, readably);
            }
            sb_putc(sb, ')');
            break;
        }
        case CLJC_VECTOR: {
            sb_putc(sb, '[');
            for (size_t i = 0; i < vec_len(v); i++) {
                if (i) sb_putc(sb, ' ');
                print_to(sb, vec_nth(v, i), readably);
            }
            sb_putc(sb, ']');
            break;
        }
        case CLJC_MAP: {
            sb_putc(sb, '{');
            bool first = true;
            for (Cljc *e = map_entry_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                if (!first) sb_puts(sb, ", ");
                first = false;
                print_to(sb, e->as.cons.head->as.cons.head, readably);
                sb_putc(sb, ' ');
                print_to(sb, e->as.cons.head->as.cons.tail, readably);
            }
            sb_putc(sb, '}');
            break;
        }
        case CLJC_SET: {
            sb_puts(sb, "#{");
            bool sfirst = true;
            for (Cljc *e = set_element_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
                if (!sfirst) sb_putc(sb, ' ');
                sfirst = false;
                print_to(sb, e->as.cons.head, readably);
            }
            sb_putc(sb, '}');
            break;
        }
        case CLJC_TVEC:  sb_puts(sb, "#<transient-vector>"); break;
        case CLJC_HNODE: sb_puts(sb, "#<hamt-node>"); break;  /* never user-visible */
        case CLJC_FN:     sb_puts(sb, "#<fn>"); break;
        case CLJC_NATIVE: sb_puts(sb, "#<native>"); break;
        case CLJC_ATOM:
            sb_puts(sb, "#atom[");
            print_to(sb, v->as.atom.value, readably);
            sb_putc(sb, ']');
            break;
        case CLJC_LAZY:   print_to(sb, to_seq(v), readably); break;  /* realizes! */
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
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV } ArithOp;

static Cljc *arith(ArithOp op, Cljc **argv, int nargs) {
    size_t n = (size_t)nargs;
    bool is_float = false;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        Cljc *v = argv[ai_];
        if (v->tag == CLJC_DOUBLE) is_float = true;
        else if (v->tag != CLJC_INT) cljc_error("expected number");
    }
    if (n == 0) {
        if (op == OP_ADD) return mk_int(0);
        if (op == OP_MUL) return mk_int(1);
        cljc_error("wrong number of args (0)");
    }

    if (!is_float) {
        int64_t acc = argv[0]->as.i;
        int ai_;
        if (n == 1) {
            if (op == OP_SUB) return mk_int(-acc);
            if (op == OP_DIV) {
                if (acc == 0) cljc_error("division by zero");
                return acc == 1 || acc == -1 ? mk_int(acc) : mk_double(1.0 / (double)acc);
            }
            return mk_int(acc);
        }
        for (ai_ = 1; ai_ < nargs; ai_++) {
            int64_t x = argv[ai_]->as.i;
            switch (op) {
                case OP_ADD: acc += x; break;
                case OP_SUB: acc -= x; break;
                case OP_MUL: acc *= x; break;
                case OP_DIV:
                    if (x == 0) cljc_error("division by zero");
                    if (acc % x != 0) { is_float = true; goto float_path; }
                    acc /= x;
                    break;
            }
        }
        return mk_int(acc);
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

static Cljc *prim_add(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_ADD, argv, nargs); }
static Cljc *prim_sub(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_SUB, argv, nargs); }
static Cljc *prim_mul(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_MUL, argv, nargs); }
static Cljc *prim_div(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return arith(OP_DIV, argv, nargs); }

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

static bool cljc_eq(Cljc *a, Cljc *b) {
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    bool a_seq = a->tag == CLJC_LIST || a->tag == CLJC_VECTOR || a->tag == CLJC_LAZY;
    bool b_seq = b->tag == CLJC_LIST || b->tag == CLJC_VECTOR || b->tag == CLJC_LAZY;
    if (a_seq && b_seq)  /* to_seq also surfaces lazy tails hidden in lists */
        return seq_eq(a->tag == CLJC_VECTOR ? a : to_seq(a),
                      b->tag == CLJC_VECTOR ? b : to_seq(b));
    if (a->tag != b->tag) {
        /* Numeric cross-equality: int vs double. */
        if ((a->tag == CLJC_INT && b->tag == CLJC_DOUBLE) ||
            (a->tag == CLJC_DOUBLE && b->tag == CLJC_INT)) {
            double da = a->tag == CLJC_INT ? (double)a->as.i : a->as.d;
            double db = b->tag == CLJC_INT ? (double)b->as.i : b->as.d;
            return da == db;
        }
        return false;
    }
    switch (a->tag) {
        case CLJC_NIL: return true;
        case CLJC_BOOL: return a->as.b == b->as.b;
        case CLJC_INT: return a->as.i == b->as.i;
        case CLJC_DOUBLE: return a->as.d == b->as.d;
        case CLJC_SYMBOL: return a->as.sym == b->as.sym;
        case CLJC_KEYWORD: return a->as.kw == b->as.kw;
        case CLJC_STRING: return strcmp(a->as.str, b->as.str) == 0;
        case CLJC_MAP: {
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
    cljc_error("expected number");
    return 0;
}

/* Chained comparisons: (< 1 2 3) is true iff each adjacent pair satisfies OP. */
#define COMPARISON(NAME, OP) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        Cljc *prev = NULL; \
        for (int ai_ = 0; ai_ < nargs; ai_++) { \
            Cljc *v = argv[ai_]; \
            if (prev && !(as_num(prev) OP as_num(v))) return FALSE; \
            prev = v; \
        } \
        return TRUE; \
    }

COMPARISON(lt, <)
COMPARISON(gt, >)
COMPARISON(le, <=)
COMPARISON(ge, >=)

static Cljc *prim_println(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    SBuf sb = {0};
    bool first = true;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, argv[ai_], false);
    }
    sb_putc(&sb, '\n');
    fwrite(sb.data, 1, sb.len, COUT);
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
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return mk_int(0);
    if (v->tag == CLJC_LIST || v->tag == CLJC_LAZY)
        return mk_int((int64_t)list_len(to_seq(v)));
    if (v->tag == CLJC_VECTOR || v->tag == CLJC_TVEC)
        return mk_int((int64_t)vec_len(v));
    if (v->tag == CLJC_MAP || v->tag == CLJC_SET)
        return mk_int((int64_t)v->as.map.count);
    if (v->tag == CLJC_STRING) return mk_int((int64_t)strlen(v->as.str));
    cljc_error("count: not countable");
    return NIL;
}

static Cljc *prim_nth(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *coll = argv[0];
    int64_t n = as_int(argv[1], "nth");
    Cljc *not_found = nargs > 2
        ? argv[2] : NULL;
    if (coll && (coll->tag == CLJC_VECTOR || coll->tag == CLJC_TVEC)) {
        if (n >= 0 && (size_t)n < vec_len(coll)) return vec_nth(coll, (size_t)n);
    } else if (coll && (coll->tag == CLJC_LIST || coll->tag == CLJC_LAZY)) {
        for (Cljc *l = seq1(coll); l && l->tag == CLJC_LIST; l = seq1(l->as.cons.tail))
            if (n-- == 0) return l->as.cons.head;
    }
    if (not_found) return not_found;
    cljc_error("nth: index out of bounds");
    return NIL;
}

static Cljc *prim_conj(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (nargs == 0) return mk_empty_vec();   /* (conj) => [] */
    Cljc *r = argv[0];  /* nil works: conj onto nil yields a list */
    for (int i = 1; i < nargs; i++) {
        Cljc *x = argv[i];
        if (r == NIL || r->tag == CLJC_LIST || r->tag == CLJC_LAZY) {
            Cljc *prev = r;                         /* lazy: cons keeps it lazy */
            r = mk_cons(x, r);                      /* lists grow at the front */
            if (prev != NIL && prev->tag == CLJC_LIST) r->meta = prev->meta;
        } else if (r->tag == CLJC_VECTOR) {
            Cljc *prev = r;
            r = vec_conj1(r, x);                    /* vectors grow at the back */
            if (prev->meta) r->meta = prev->meta;   /* queue tag etc. survive */
        } else if (r->tag == CLJC_SET) {
            r = set_conj(r, x);
        } else if (r->tag == CLJC_MAP) {
            /* (conj m [k v]) and (conj m {k v ...}) — Clojure semantics */
            if (x != NIL && x->tag == CLJC_VECTOR && vec_len(x) == 2) {
                r = map_assoc(r, vec_nth(x, 0), vec_nth(x, 1));
            } else if (x != NIL && x->tag == CLJC_MAP) {
                for (Cljc *e = map_entry_list(x); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
                    r = map_assoc(r, e->as.cons.head->as.cons.head,
                                  e->as.cons.head->as.cons.tail);
            } else cljc_error("conj on map: expected a [k v] entry or a map");
        } else cljc_error("conj: not a collection");
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
    for (int i = 1; i < nargs - 1; i++) vpush(argv[i]);
    if (nargs > 1)  /* splice the final collection — any seqable */
        for (Cljc *s = seq1(argv[nargs - 1]); s != NIL; s = seq1(s->as.cons.tail))
            vpush(s->as.cons.head);
    Cljc *r = apply(env, fn, &vstack[base], (int)(vsp - base));
    vsp = base;
    return r;
}

#define TYPE_PRED(NAME, EXPR) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; Cljc *v = argv[0]; (void)v; \
        return mk_bool(EXPR); \
    }

TYPE_PRED(nil_p,     v == NIL)
TYPE_PRED(map_p,     v != NIL && v->tag == CLJC_MAP)
TYPE_PRED(set_p,     v != NIL && v->tag == CLJC_SET)
TYPE_PRED(list_p,    v != NIL && v->tag == CLJC_LIST)
TYPE_PRED(vector_p,  v != NIL && v->tag == CLJC_VECTOR)
TYPE_PRED(number_p,  v != NIL && (v->tag == CLJC_INT || v->tag == CLJC_DOUBLE))
TYPE_PRED(int_p,     v != NIL && v->tag == CLJC_INT)
TYPE_PRED(double_p,  v != NIL && v->tag == CLJC_DOUBLE)
TYPE_PRED(string_p,  v != NIL && v->tag == CLJC_STRING)
TYPE_PRED(keyword_p, v != NIL && v->tag == CLJC_KEYWORD)
TYPE_PRED(symbol_p,  v != NIL && v->tag == CLJC_SYMBOL)
TYPE_PRED(fn_p,      v != NIL && (v->tag == CLJC_FN || v->tag == CLJC_NATIVE))
TYPE_PRED(zero_p,    as_num(v) == 0)
TYPE_PRED(pos_p,     as_num(v) > 0)
TYPE_PRED(neg_p,     as_num(v) < 0)

static Cljc *prim_empty_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return TRUE;
    if (v->tag == CLJC_LIST) return FALSE;  /* a cons is never empty */
    if (v->tag == CLJC_LAZY) return mk_bool(seq1(v) == NIL);
    if (v->tag == CLJC_VECTOR) return mk_bool(vec_len(v) == 0);
    if (v->tag == CLJC_MAP || v->tag == CLJC_SET)
        return mk_bool(v->as.map.count == 0);
    if (v->tag == CLJC_STRING) return mk_bool(v->as.str[0] == '\0');
    cljc_error("empty?: not a collection");
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
    int64_t a = as_int(argv[0], "mod");
    int64_t b = as_int(argv[1], "mod");
    if (b == 0) cljc_error("mod: division by zero");
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
    (void)env;
    Cljc *coll = argv[0];
    Cljc *k = argv[1];
    Cljc *dflt = nargs > 2
        ? argv[2] : NIL;
    if (coll != NIL && coll->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(coll, k, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_SET) {
        Cljc *out;
        if (set_contains(coll, k, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_VECTOR && k->tag == CLJC_INT) {
        if (k->as.i >= 0 && (size_t)k->as.i < vec_len(coll))
            return vec_nth(coll, (size_t)k->as.i);
    } else if (coll != NIL && coll->tag == CLJC_STRING && k->tag == CLJC_INT) {
        size_t len = strlen(coll->as.str);   /* (get s i) => 1-char string */
        if (k->as.i >= 0 && (size_t)k->as.i < len)
            return mk_str(coll->as.str + k->as.i, 1);
    }
    return dflt;
}

static Cljc *prim_assoc(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *coll = argv[0];
    if (coll == NIL) coll = mk_map();
    Cljc *r = coll;
    if ((nargs - 1) % 2 != 0) cljc_error("assoc needs key-value pairs");
    for (int i = 1; i < nargs; i += 2) {
        Cljc *k = argv[i], *v = argv[i + 1];
        if (r->tag == CLJC_MAP) r = map_assoc(r, k, v);
        else if (r->tag == CLJC_VECTOR) {
            if (k->tag != CLJC_INT || k->as.i < 0)
                cljc_error("assoc on vector: index out of bounds");
            r = vec_assoc_idx(r, (size_t)k->as.i, v);  /* assoc at len appends */
        } else cljc_error("assoc: not associative");
    }
    return r;
}

static Cljc *prim_dissoc(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("dissoc: not a map");
    for (int i = 1; i < nargs; i++)
        m = map_dissoc_one(m, argv[i]);
    return m;
}

static Cljc *prim_keys(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("keys: not a map");
    Cljc *out = NIL, **t = &out;
    for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
        *t = mk_cons(e->as.cons.head->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_vals(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *m = argv[0];
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("vals: not a map");
    Cljc *out = NIL, **t = &out;
    for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
        *t = mk_cons(e->as.cons.head->as.cons.tail, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_contains_p(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *coll = argv[0];
    Cljc *k = argv[1];
    if (coll == NIL) return FALSE;
    if (coll->tag == CLJC_MAP) return mk_bool(map_find(coll, k, NULL));
    if (coll->tag == CLJC_SET) return mk_bool(set_contains(coll, k, NULL));
    if (coll->tag == CLJC_VECTOR)  /* contains? checks INDEX presence on vectors */
        return mk_bool(k->tag == CLJC_INT && k->as.i >= 0 && (size_t)k->as.i < vec_len(coll));
    cljc_error("contains?: not associative");
    return NIL;
}

static Cljc *prim_merge(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *r = NIL;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        Cljc *m = argv[ai_];
        if (m == NIL) continue;
        if (m->tag != CLJC_MAP) cljc_error("merge: not a map");
        if (r == NIL) { r = m; continue; }
        for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
            r = map_assoc(r, e->as.cons.head->as.cons.head, e->as.cons.head->as.cons.tail);
    }
    return r;
}

static Cljc *prim_rem(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t a = as_int(argv[0], "rem");
    int64_t b = as_int(argv[1], "rem");
    if (b == 0) cljc_error("rem: division by zero");
    return mk_int(a % b);
}

static Cljc *prim_list(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *out = NIL;
    for (int i = nargs - 1; i >= 0; i--) out = mk_cons(argv[i], out);
    return out;
}

static Cljc *prim_first(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = seq1(argv[0]);  /* forces ONE cell at most */
    return s == NIL ? NIL : s->as.cons.head;
}

static Cljc *prim_rest(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *s = seq1(argv[0]);
    return s == NIL ? NIL : s->as.cons.tail;  /* tail may be lazy */
}

static Cljc *prim_second(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *r = prim_rest(env, argv, nargs);
    if (r == NIL) return NIL;
    return r->as.cons.head;
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

/* Force a lazy cell once; thunk dropped after so its closure can be GC'd. */
static Cljc *lazy_force(Cljc *l) {
    if (!l->as.lazy.done) {
        l->as.lazy.cached = apply(gc_root_envs[0], l->as.lazy.thunk, NULL, 0);
        l->as.lazy.done = true;
        l->as.lazy.thunk = NIL;
    }
    return l->as.lazy.cached;
}

/* Single-step seq: force AT MOST the head cell. Returns NIL or a cons whose
 * tail may itself be lazy. This is what keeps pipelines lazy. */
static Cljc *seq1(Cljc *v) {
    for (;;) {
        if (v == NULL || v == NIL) return NIL;
        if (v->tag == CLJC_LIST) return v;
        if (v->tag == CLJC_LAZY) { v = lazy_force(v); continue; }
        return to_seq(v);  /* finite collections materialize */
    }
}

/* Normalize any seqable to a FULLY REALIZED plain list (eager consumers).
 * Plain lists pass through untouched unless a lazy tail hides inside. */
static Cljc *to_seq(Cljc *v) {
    if (v == NIL) return NIL;
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
        /* Strings seq into 1-char strings — no char type (divergence). */
        Cljc *out = NIL, **t = &out;
        for (const char *c = v->as.str; *c; c++) {
            *t = mk_cons(mk_str(c, 1), NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    if (v->tag == CLJC_SET) return set_element_list(v);
    if (v->tag == CLJC_MAP) {
        /* Maps seq into [k v] entry vectors. */
        Cljc *out = NIL, **t = &out;
        for (Cljc *e = map_entry_list(v); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
            Cljc *entry_items[2] = { e->as.cons.head->as.cons.head, e->as.cons.head->as.cons.tail };
            *t = mk_cons(mk_vector(entry_items, 2), NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    cljc_error("not seqable");
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
    Cljc *acc, *seq;
    if (nargs < 3) {
        seq = seq1(argv[1]);               /* lazy cursor: no realization */
        if (seq == NIL) return apply(env, f, NULL, 0);  /* (reduce f []) => (f) */
        acc = seq->as.cons.head;
        seq = seq1(seq->as.cons.tail);
    } else {
        acc = argv[1];
        seq = seq1(argv[2]);
    }
    for (Cljc *l = seq; l != NIL; l = seq1(l->as.cons.tail)) {
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
    int64_t start = 0, end = 0, step = 1;
    size_t n = (size_t)nargs;
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
    for (Cljc *l = to_seq(argv[0]); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        out = mk_cons(l->as.cons.head, out);
    return out;
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
    return mk_bool(v != NIL && (v->tag == CLJC_LIST || v->tag == CLJC_LAZY));
}

/* ───── Regex engine ─────────────────────────────────────────────────── */

/* Tiny backtracking matcher. Supported: literals . ^ $ [abc] [a-z] [^...]
 * \d \D \w \W \s \S \n \t \r, ( ) capture groups, (?: ) non-capturing,
 * (?=X) (?!X) lookahead, | alternation, * + ? quantifiers with lazy
 * variants (*? +? ??), X{n} X{n,} X{n,m} bounded repeats (n,m ≤ 64,
 * desugared to copies).
 * Not supported: backreferences, lookbehind. Patterns are plain
 * strings; the #"..." reader literal passes backslashes through raw.
 * Parse-time desugaring: X+ => X X* (atom re-parsed), X? => ALT(X, empty);
 * only star is a real loop, guarded against empty-match cycles. */

enum { RX_CHAR, RX_ANY, RX_CLASS, RX_BOL, RX_EOL, RX_STAR, RX_LOOP,
       RX_ALT, RX_JOIN, RX_GS, RX_GE, RX_LA };

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
        RxChain atom = rx_parse_atom(c);
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
            if (hi == -1) hi = lo;
            if (lo > 64 || (hi != -2 && (hi > 64 || hi < lo)))
                cljc_error("regex: {n,m} out of range");
            const char *after = c->p;
            RxChain seq = lo > 0 ? atom : (RxChain){NULL, NULL};
            for (int k = 1; k < lo; k++) {       /* required copies 2..n */
                c->p = atom_src;
                int sg = c->ngroups;
                RxChain copy = rx_parse_atom(c);
                c->ngroups = sg;                 /* copies share group numbers */
                if (!seq.h) seq = copy;
                else if (copy.h) { seq.t->next = copy.h; seq.t = copy.t; }
            }
            if (hi == -2) {                      /* {n,}: append X* */
                c->p = atom_src;
                int sg = c->ngroups;
                RxChain copy = rx_parse_atom(c);
                c->ngroups = sg;
                RxChain star = rx_star(c, copy, false);
                if (!seq.h) seq = star;
                else { seq.t->next = star.h; seq.t = star.t; }
            } else {
                for (int k = lo; k < hi; k++) {  /* optional copies */
                    c->p = atom_src;
                    int sg = c->ngroups;
                    RxChain copy = rx_parse_atom(c);
                    c->ngroups = sg;
                    Rx *a = rx_node(c, RX_ALT);
                    Rx *j1 = rx_node(c, RX_JOIN); j1->owner = a;
                    Rx *j2 = rx_node(c, RX_JOIN); j2->owner = a;
                    if (copy.t) copy.t->next = j1;
                    a->child = copy.h ? copy.h : j1;
                    a->alt = j2;
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
            if (q == '*') {
                unit = rx_star(c, atom, lazy);
            } else if (q == '+') {
                /* X+ => X X*: re-parse the atom for the star's copy. */
                const char *save = c->p;
                int save_groups = c->ngroups;
                c->p = atom_src;
                RxChain atom2 = rx_parse_atom(c);
                c->ngroups = save_groups;  /* copies share group numbers */
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
        case RX_ANY:   return *s && *s != '\n' && rx_m(r->next, s + 1);
        case RX_CLASS: return *s && rx_bit_test(r, (unsigned char)*s) && rx_m(r->next, s + 1);
        case RX_BOL:   return s == rx_str_begin && rx_m(r->next, s);
        case RX_EOL:   return *s == '\0' && rx_m(r->next, s);
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
static Rx *rx_compile(const char *pattern, Rx **pool_out, int *ngroups_out) {
    RxC c;
    c.p = pattern;
    c.pool = xmalloc(sizeof(Rx) * RX_MAX_NODES);
    c.npool = 0;
    c.ngroups = 1;  /* group 0 = whole match */
    RxChain top = rx_parse_alt(&c);
    if (*c.p) { free(c.pool); cljc_error("regex: unexpected )"); }
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
    c.p = pattern;
    c.pool = xmalloc(sizeof(Rx) * RX_MAX_NODES);
    c.npool = 0;
    c.ngroups = 1;
    RxChain top = rx_parse_alt(&c);
    if (*c.p) { free(c.pool); cljc_error("regex: unexpected )"); }
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
    (void)env;
    Cljc *s = argv[0];
    if (s == NIL) return NIL;
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
    for (int i = 0; i < n_aliases; i++)
        if (alias_table[i] == in) { alias_ns[i] = nsin; return NIL; }
    if (n_aliases >= MAX_ALIASES) cljc_error("too many aliases");
    alias_table[n_aliases] = in;
    alias_ns[n_aliases] = nsin;
    n_aliases++;
    return NIL;
}

/* (cljc/in-ns* name-or-nil) — set the reader/def namespace, return the old. */
static Cljc *prim_in_ns(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    const char *old = cur_reader_ns;
    Cljc *v = argv[0];
    cur_reader_ns = (v == NIL) ? NULL
        : intern(v->as.str, strlen(v->as.str));
    return old ? mk_str(old, strlen(old)) : NIL;
}

static Cljc *prim_with_meta(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env; (void)nargs;
    Cljc *v = argv[0];
    Cljc *m = argv[1];
    if (m != NIL && m->tag != CLJC_MAP) cljc_error("with-meta: meta must be a map");
    if (v == NIL) cljc_error("with-meta: nil cannot carry metadata");
    switch (v->tag) {
        case CLJC_LIST: case CLJC_VECTOR: case CLJC_MAP: case CLJC_SET:
        case CLJC_FN: case CLJC_SYMBOL: case CLJC_STRING: break;
        default: cljc_error("with-meta: this type cannot carry metadata");
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
    if (v != NIL && v->tag == CLJC_DOUBLE) return mk_int((int64_t)v->as.d);
    if (v != NIL && v->tag == CLJC_STRING && v->as.str[0])
        return mk_int((int64_t)(unsigned char)v->as.str[0]);  /* (int \a) => 97 */
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
        case CLJC_DOUBLE: n = "double"; break;
        case CLJC_STRING: n = "string"; break;
        case CLJC_KEYWORD: n = "keyword"; break;
        case CLJC_SYMBOL: n = "symbol"; break;
        case CLJC_LIST: n = "list"; break;
        case CLJC_LAZY: n = "lazy-seq"; break;
        case CLJC_VECTOR: n = "vector"; break;
        case CLJC_SET: n = "set"; break;
        case CLJC_ATOM: n = "atom"; break;
        case CLJC_FN: case CLJC_NATIVE: n = "fn"; break;
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

/* (cljc/onto strict-list tail) — copy the list's conses onto tail (which
 * may be lazy), without per-element lazy cells. */
static Cljc *prim_onto(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *lst = argv[0];
    Cljc *tail = argv[1];
    if (lst == NIL) return tail;
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = lst; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *t = mk_cons(l->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    *t = tail;
    return out;
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
    Cljc *msg = argv[0];
    Cljc *data = argv[1];
    Cljc *m = mk_map();
    m = map_assoc(m, mk_kw(kw_message()), msg);
    m = map_assoc(m, mk_kw(kw_data()), data);
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
    if (v == NIL || v->tag != CLJC_ATOM) cljc_error("%s: expected an atom", what);
    return v;
}

static Cljc *prim_atom(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *a = alloc(CLJC_ATOM);
    a->as.atom.value = argv[0];
    return a;
}

static Cljc *prim_deref(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return as_atom(argv[0], "deref")->as.atom.value;
}

static Cljc *prim_reset(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *a = as_atom(argv[0], "reset!");
    Cljc *v = argv[1];
    a->as.atom.value = v;
    return v;
}

static Cljc *prim_swap(CljcEnv *env, Cljc **argv, int nargs) {
    /* (swap! a f x y) => sets a to (f @a x y), returns the new value. */
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
    bool a_num = a->tag == CLJC_INT || a->tag == CLJC_DOUBLE;
    bool b_num = b->tag == CLJC_INT || b->tag == CLJC_DOUBLE;
    if (a_num && b_num) {
        double d = as_num(a) - as_num(b);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    if (a->tag != b->tag) cljc_error("compare: incomparable types");
    switch (a->tag) {
        case CLJC_STRING:  return strcmp(a->as.str, b->as.str);
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
        default: cljc_error("compare: incomparable type");
    }
    return 0;
}

static Cljc *prim_compare(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int(cmp_values(argv[0], argv[1]));
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
    Cljc *out = NIL;
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
    (void)env;
    const char *n = as_named(argv[0], "name");
    return mk_str(n, strlen(n));
}

static Cljc *prim_keyword(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    const char *n = as_named(argv[0], "keyword");
    return mk_kw(intern(n, strlen(n)));
}

static Cljc *prim_symbol(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    const char *n = as_named(argv[0], "symbol");
    return mk_sym(intern(n, strlen(n)));
}

static Cljc *prim_quot(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    int64_t a = as_int(argv[0], "quot");
    int64_t b = as_int(argv[1], "quot");
    if (b == 0) cljc_error("quot: division by zero");
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
        cljc_error("subs: index out of bounds");
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
static Cljc *prim_index_of(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *s = as_str(argv[0], "str/index-of");
    char *sub = as_str(argv[1], "str/index-of");
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

static Cljc *prim_slurp(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    char *path = as_str(argv[0], "slurp");
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
    return mk_int((int64_t)st.st_mtim.tv_sec * 1000
                  + st.st_mtim.tv_nsec / 1000000);
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return mk_double((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
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
    int port = (int)as_int(argv[0], "tcp/listen");
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) cljc_error("tcp/listen: cannot create socket");
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(srv);
        cljc_error("tcp/listen: cannot bind 127.0.0.1:%d (port in use?)", port);
    }
    if (listen(srv, 16) < 0) { close(srv); cljc_error("tcp/listen: listen failed"); }
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
    close((int)as_int(argv[0], "tcp/close"));
    return NIL;
}

/* ── with-out-str ──
 * (cljc/with-out-str* thunk) → everything the thunk printed, as a string.
 * Swaps the interpreter output stream for a memstream around the call; an
 * ErrFrame restores the stream before re-raising if the thunk throws. */
static Cljc *prim_with_out_str(CljcEnv *env, Cljc **argv, int nargs) {
    Cljc *f = argv[0];
    /* volatile: the memstream machinery updates buf on every write, and we
     * read it back after a longjmp into this frame. */
    char *volatile buf = NULL;
    size_t blen = 0;
    FILE *ms = open_memstream((char **)&buf, &blen);
    if (!ms) cljc_error("with-out-str: out of memory");
    FILE *saved = cljc_out;
    cljc_out = ms;
    ErrFrame frame;
    frame.prev = err_top;
    frame.vsp_save = vsp;
    frame.esp_save = eval_sp;
    err_top = &frame;
    if (setjmp(frame.jb) == 0) {
        apply(env, f, vstack + vsp, 0);
        err_top = frame.prev;
    } else {                       /* thunk threw: restore stream, re-raise */
        err_top = frame.prev;
        vsp = frame.vsp_save;
        eval_sp = frame.esp_save;
        cljc_out = saved;
        fclose(ms);
        free((char *)buf);
        cljc_raise();
    }
    cljc_out = saved;
    fclose(ms);                    /* flush: buf/blen now final */
    Cljc *r = mk_str(buf ? (char *)buf : "", blen);
    free((char *)buf);
    return r;
}

/* ── printing variants ── */

static Cljc *print_args(Cljc **argv, int nargs, bool readably, bool newline) {
    SBuf sb = {0};
    bool first = true;
    for (int ai_ = 0; ai_ < nargs; ai_++) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, argv[ai_], readably);
    }
    if (newline) sb_putc(&sb, '\n');
    if (sb.data) { fwrite(sb.data, 1, sb.len, COUT); free(sb.data); }
    return NIL;
}

static Cljc *prim_pr(CljcEnv *env, Cljc **argv, int nargs)    { (void)env; return print_args(argv, nargs, true, false); }
static Cljc *prim_prn(CljcEnv *env, Cljc **argv, int nargs)   { (void)env; return print_args(argv, nargs, true, true); }
static Cljc *prim_print(CljcEnv *env, Cljc **argv, int nargs) { (void)env; return print_args(argv, nargs, false, false); }


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

static Cljc *prim_str_replace(CljcEnv *env, Cljc **argv, int nargs) {
    /* (str/replace s match replacement) — match is a LITERAL substring.
     * For regex replacement with $1 refs, use re-replace. */
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

/* Append replacement text, expanding $0..$9 to capture groups; $$ => $. */
static void rx_subst(SBuf *out, const char *repl) {
    for (const char *r = repl; *r; r++) {
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

static Cljc *prim_re_replace(CljcEnv *env, Cljc **argv, int nargs) {
    /* (re-replace s pattern replacement) — all matches; $1..$9 in repl. */
    (void)env;
    char *s = as_str(argv[0], "re-replace");
    char *pat = as_str(argv[1], "re-replace");
    char *repl = as_str(argv[2], "re-replace");
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
            rx_cap_e[0] = rx_match_end;
            rx_subst(&out, repl);
            if (rx_match_end > p) { p = rx_match_end; continue; }
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
        rx_subst(&out, repl);
    }
    free(pool);
    Cljc *res = mk_str(out.data, out.len);
    free(out.data);
    return res;
}

static Cljc *prim_re_split(CljcEnv *env, Cljc **argv, int nargs) {
    /* (re-split s pattern) => vector of segments; trailing empties dropped. */
    (void)env;
    char *s = as_str(argv[0], "re-split");
    char *pat = as_str(argv[1], "re-split");
    Rx *pool; int ngroups;
    Rx *prog = rx_compile(pat, &pool, &ngroups);
    rx_str_begin = s;
    Cljc *segs = NIL, **t = &segs;  /* list of segment strings, in order */
    size_t nsegs = 0, last_nonempty = 0;
    const char *seg = s, *p = s;
    while (*p) {
        rx_reset_caps();
        if (rx_m(prog, p) && rx_match_end > p) {
            *t = mk_cons(mk_str(seg, (size_t)(p - seg)), NIL);
            t = &(*t)->as.cons.tail;
            nsegs++;
            if (p > seg) last_nonempty = nsegs;
            p = rx_match_end;
            seg = p;
        } else p++;
    }
    *t = mk_cons(mk_str(seg, (size_t)(p - seg)), NIL);
    nsegs++;
    if (p > seg) last_nonempty = nsegs;
    free(pool);
    Cljc *v = mk_empty_vec();
    size_t i = 0;
    for (Cljc *l = segs; l && l->tag == CLJC_LIST && i < last_nonempty; l = l->as.cons.tail, i++)
        v = vec_conj1(v, l->as.cons.head);
    return v;
}

/* ── math / random ── */

#define MATH1(NAME, FN) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        return mk_double(FN(as_num(argv[0]))); \
    }

MATH1(sqrt, sqrt)
MATH1(floor, floor)
MATH1(ceil, ceil)

static Cljc *prim_pow(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_double(pow(as_num(argv[0]),
                         as_num(argv[1])));
}

static Cljc *prim_round(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    return mk_int((int64_t)llround(as_num(argv[0])));
}

static Cljc *prim_math_abs(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v != NIL && v->tag == CLJC_DOUBLE) return mk_double(fabs(v->as.d));
    return mk_int(v->as.i < 0 ? -v->as.i : v->as.i);
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

#define BIT_FOLD(NAME, OP) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc **argv, int nargs) { \
        (void)env; \
        int64_t v = as_int(argv[0], #NAME); \
        for (int bi_ = 1; bi_ < nargs; bi_++) v = v OP as_int(argv[bi_], #NAME); \
        return mk_int(v); \
    }

BIT_FOLD(bit_and, &)
BIT_FOLD(bit_or,  |)
BIT_FOLD(bit_xor, ^)

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

/* (char 97) → "a" — code point to (UTF-8) one-char string; strings pass
 * through, so (char (first "abc")) works in char-as-string cljc. */
static Cljc *prim_char(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    if (argv[0] != NIL && argv[0]->tag == CLJC_STRING) return argv[0];
    int64_t c = as_int(argv[0], "char");
    char b[4];
    int n = 0;
    if (c < 0x80) b[n++] = (char)c;
    else if (c < 0x800) {
        b[n++] = (char)(0xC0 | (c >> 6));
        b[n++] = (char)(0x80 | (c & 0x3F));
    } else {
        b[n++] = (char)(0xE0 | (c >> 12));
        b[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (c & 0x3F));
    }
    return mk_str(b, (size_t)n);
}

/* (str/replace-first s match repl) — first occurrence only, literal match. */
static Cljc *prim_replace_first(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
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
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') return NIL;  /* whole string or nil */
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
    cljc_error("peek: not a list or vector");
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
    cljc_error("pop: not a list or vector");
    return NIL;
}

static Cljc *prim_empty(CljcEnv *env, Cljc **argv, int nargs) {
    (void)env;
    Cljc *v = argv[0];
    if (v == NIL) return NIL;
    switch (v->tag) {
        case CLJC_LIST:   return NIL;
        case CLJC_VECTOR: return mk_empty_vec();
        case CLJC_MAP:    return mk_map();
        case CLJC_SET:    return mk_set();
        default:          return NIL;
    }
}

/* ── sh / FFI (the s7 cload model: generate glue C, compile, dlopen) ── */

#include <dlfcn.h>

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

static CljcFfiApi ffi_api = {
    fa_mk_int, fa_mk_double, fa_mk_str, fa_nil,
    fa_as_int, fa_as_double, fa_as_str, fa_nth_arg, fa_def_native, fa_error,
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

void cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn) {
    env_define_root(env_root(env), intern(name, strlen(name)), mk_native(fn));
}

/* Record the high-water mark of the C stack for conservative root scanning.
 * Call with the address of a local near the top of the thread (e.g. &argc in
 * main) before evaluating anything. Safe to call repeatedly — keeps the
 * highest address seen (downward-growing stacks). */
void cljc_set_stack_base(void *p) {
    if (!gc_stack_base || p > gc_stack_base) gc_stack_base = p;
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
    "(defmacro when-not [test & body] `(when (not ~test) ~@body))\n"
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
    "         (recur (seq (drop step s)) (cons chunk acc)))))))\n"
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
    "  ([sep coll] (if (empty? coll) \"\" (reduce (fn [a b] (str a sep b)) coll))))\n"
    /* control-flow macros */
    "(defmacro if-let [bindings then & else]\n"
    "  `(let [t# ~(nth bindings 1)]\n"
    "     (if t# (let [~(nth bindings 0) t#] ~then) ~@else)))\n"
    "(defmacro when-let [bindings & body]\n"
    "  `(let [t# ~(nth bindings 1)]\n"
    "     (when t# (let [~(nth bindings 0) t#] ~@body))))\n"
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
    "               (if (and (list? t) (seq t))\n"
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
    "(defmacro defn- [name & body] `(defn ~name ~@body))\n"
    "(defn vary-meta [x f & args] (with-meta x (apply f (meta x) args)))\n"
    "(defmacro instance? [c x] `(do ~x false))\n"
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
    "(defn coll? [x] (or (list? x) (vector? x) (map? x) (set? x) (seq? x)))\n"
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
    "(defmacro with-out-str [& body] `(cljc/with-out-str* (fn [] ~@body)))\n"
    "(defn sequential? [x] (or (list? x) (vector? x) (seq? x)))\n"
    "(def class type)\n"
    /* regexes are strings; the :regex meta makes str/split treat them so */
    "(defn re-pattern [s] (with-meta s {:regex true}))\n"
    "(defn boolean? [x] (or (true? x) (false? x)))\n"
    "(defn nat-int? [x] (and (int? x) (>= x 0)))\n"
    "(defn isa? [c p] (= c p))\n"          /* no hierarchies (v0) */
    "(defn every-pred [& ps]\n"
    "  (fn [& args] (every? (fn [p] (every? p args)) ps)))\n"
    "(defn some-fn [& ps]\n"
    "  (fn [& args] (some (fn [p] (some p args)) ps)))\n"
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
    "(def == =)\n"                       /* = already numeric cross-type */
    "(defn distinct? [& xs] (= (count xs) (count (set xs))))\n"
    /* arbitrary-precision variants: int64 here (overflow wraps, v0) */
    "(def *' *) (def +' +) (def -' -) (def inc' inc) (def dec' dec)\n"
    "(defn char? [x] (and (string? x) (= 1 (count x))))\n"
    /* deftype, tolerated: defines a Name. constructor returning a plain map
     * of fields; interface method bodies are ignored. Enough for files that
     * define a type they rarely use to still load. */
    "(defmacro deftype [tname fields & _]\n"
    "  `(defn ~(symbol (str tname \".\")) [~@fields]\n"
    "     ~(zipmap (map keyword fields) fields)))\n"
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
    "(defn BigInteger. [signum x] x)\n"         /* hex passes through */
    "(defn .indexOf [coll x]\n"
    "  (if (string? coll)\n"
    "    (or (str/index-of coll x) -1)\n"
    "    (loop [i 0 s (seq coll)]\n"
    "      (cond (nil? s) -1\n"
    "            (= (first s) x) i\n"
    "            :else (recur (inc i) (next s))))))\n"
    /* Math/sqrt|pow|floor|ceil|round|abs are already natives */
    "(def cljc/digit-chars \"0123456789abcdefghijklmnopqrstuvwxyz\")\n"
    "(defn Character/digit [c radix]\n"
    "  (let [i (str/index-of cljc/digit-chars (str/lower-case (str c)))]\n"
    "    (if (and i (< i radix)) i -1)))\n"
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
    /* mutable arrays, as transient vectors (assoc! mutates in place) */
    "(defn int-array [x] (transient (vec (if (int? x) (repeat x 0) x))))\n"
    "(def byte-array int-array)\n"
    "(def long-array int-array)\n"
    "(def object-array int-array)\n"
    "(defn aget [a i] (a i))\n"
    "(defn aset [a i v] (assoc! a i v) v)\n"
    "(defn alength [a] (count a))\n"
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
    "(defn File. [path] path)\n"
    "(defn ImageIO/write [img fmt file] true)\n"
    "(def *load-path*\n"
    "  (vec (concat [\".\" \"vendor\"]\n"
    "               (when-let [p (cljc/env* \"CLJC_PATH\")]\n"
    "                 (str/split p \":\"))\n"
    "               [(cljc/sharedir*) (str (cljc/sharedir*) \"/vendor\")])))\n"
    "(def cljc/loaded-namespaces (atom #{}))\n"
    "(defn cljc/spec-opt [spec k]\n"
    "  (loop [s (seq (rest spec))]\n"
    "    (cond (nil? s) nil\n"
    "          (= k (first s)) (second s)\n"
    "          :else (recur (nnext s)))))\n"
    "(defn cljc/require-one [spec]\n"
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
    "        (when hit\n"
    "          (swap! cljc/loaded-namespaces conj nsname)\n"
    "          (let [old (cljc/in-ns* (str nsname))]\n"
    "            (try (load-file hit)\n"
    "                 (finally (cljc/in-ns* old)))))))\n"
    "    (when-let [refers (and (vector? spec) (cljc/spec-opt spec :refer))]\n"
    "      (doseq [r refers]\n"
    "        (eval (list 'def r (symbol (str nsname \"/\" r))))))))\n"
    "(defmacro require [& specs]\n"
    "  `(do ~@(map (fn [s] `(cljc/require-one ~s)) specs) nil))\n"
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
    "  (reduce (fn [acc [k v]] (f acc k v)) init (seq m)))\n"
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
    "(defn rand-nth [coll] (nth (vec coll) (rand-int (count coll))))\n"
    "(defn max-key [f x & xs] (reduce (fn [a b] (if (> (f a) (f b)) a b)) x xs))\n"
    "(defn min-key [f x & xs] (reduce (fn [a b] (if (< (f a) (f b)) a b)) x xs))\n"
    "(defn set/union [& sets] (reduce (fn [a s] (reduce conj a (seq s))) #{} sets))\n"
    "(defn set/intersection [s1 & ss]\n"
    "  (reduce (fn [a s] (set (filter (fn [x] (contains? s x)) (seq a)))) s1 ss))\n"
    "(defn set/difference [s1 & ss]\n"
    "  (reduce (fn [a s] (set (remove (fn [x] (contains? s x)) (seq a)))) s1 ss))\n"
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
        NIL = alloc(CLJC_NIL);
        /* Self-referential cons fields: walking off the end of any form
         * (e.g. (def) with no args) yields NIL instead of reading
         * uninitialized union bytes. Safety net, not license — special
         * forms still arity-check for decent error messages. */
        NIL->as.cons.head = NIL;
        NIL->as.cons.tail = NIL;
        TRUE = alloc(CLJC_BOOL);  TRUE->as.b = true;
        FALSE = alloc(CLJC_BOOL); FALSE->as.b = false;
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
    cljc_define_native(e, "/",       prim_div);
    cljc_define_native(e, "=",       prim_eq);
    cljc_define_native(e, "<",       prim_lt);
    cljc_define_native(e, ">",       prim_gt);
    cljc_define_native(e, "<=",      prim_le);
    cljc_define_native(e, ">=",      prim_ge);
    cljc_define_native(e, "println", prim_println);
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
    cljc_define_native(e, "range",   prim_range);
    cljc_define_native(e, "take",    prim_take);
    cljc_define_native(e, "drop",    prim_drop);
    cljc_define_native(e, "reverse", prim_reverse);
    cljc_define_native(e, "last",    prim_last);
    cljc_define_native(e, "seq",     prim_seq);
    cljc_define_native(e, "seq?",    prim_seq_p);
    cljc_define_native(e, "type",    prim_type);
    cljc_define_native(e, "sh",        prim_sh);
    cljc_define_native(e, "hash",      prim_hash);
    cljc_define_native(e, "cljc/env*",      prim_getenv_raw);
    cljc_define_native(e, "cljc/sharedir*", prim_sharedir);
    cljc_define_native(e, "int",       prim_int);
    cljc_define_native(e, "identical?", prim_identical);
    cljc_define_native(e, "with-meta",  prim_with_meta);
    cljc_define_native(e, "meta",       prim_meta);
    cljc_define_native(e, "cljc/alias*", prim_alias);
    cljc_define_native(e, "cljc/in-ns*", prim_in_ns);
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
    cljc_define_native(e, "re-find",    prim_re_find);
    cljc_define_native(e, "re-matches", prim_re_matches);
    cljc_define_native(e, "re-seq",     prim_re_seq);
    cljc_define_native(e, "re-replace",  prim_re_replace);
    cljc_define_native(e, "re-split",    prim_re_split);
    cljc_define_native(e, "format",      prim_format);
    cljc_define_native(e, "str/replace", prim_str_replace);
    cljc_define_native(e, "Math/sqrt",  prim_sqrt);
    cljc_define_native(e, "Math/pow",   prim_pow);
    cljc_define_native(e, "Math/floor", prim_floor);
    cljc_define_native(e, "Math/ceil",  prim_ceil);
    cljc_define_native(e, "Math/round", prim_round);
    cljc_define_native(e, "Math/abs",   prim_math_abs);
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
    cljc_define_native(e, "cljc/with-out-str*", prim_with_out_str);
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
        "  ([a b & more] (concat (concat a b) (apply concat more))))\n"
        "(defn cycle [c] (lazy-seq (concat (seq c) (cycle c))))\n"
        "(defn map-xf [f]\n"
        "  (fn [rf] (fn ([] (rf)) ([acc] (rf acc)) ([acc x] (rf acc (f x))))))\n"
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
        "(defn sequence ([c] (seq c)) ([xform c] (sequence*2 xform c)))\n"
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
        "   (cons init (lazy-seq (when-let [s (seq coll)]\n"
        "                          (reductions f (f init (first s)) (rest s)))))))\n"
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
        "(defmacro defmulti [name dispatch]\n"
        "  `(do (swap! cljc/multi-tables assoc '~name {})\n"
        "       (def ~name\n"
        "         (let [d# ~dispatch]\n"
        "           (fn [& args#]\n"
        "             (let [t# (get @cljc/multi-tables '~name)\n"
        "                   dv# (apply d# args#)\n"
        "                   m# (get t# dv# (get t# :default))]\n"
        "               (if m#\n"
        "                 (apply m# args#)\n"
        "                 (throw (ex-info (str \"No method in \" '~name\n"
        "                                      \" for \" (pr-str dv#)) {})))))))))\n"
        "(defmacro defmethod [name dval params & body]\n"
        "  `(do (swap! cljc/multi-tables update '~name assoc ~dval\n"
        "              (fn ~params ~@body))\n"
        "       '~name))\n"
        /* protocols: each method dispatches on (type (first args)) */
        "(defmacro defprotocol [pname & sigs]\n"
        "  `(do ~@(map (fn [sig]\n"
        "                (let [m (first sig)]\n"
        "                  `(defmulti ~m (fn [& args#] (type (first args#))))))\n"
        "              sigs)\n"
        "       (def ~pname '~(mapv first sigs))))\n"
        "(defmacro extend-type [t & impls]\n"
        "  `(do ~@(map (fn [[m params & body]]\n"
        "                `(defmethod ~m ~t ~(vec params) ~@body))\n"
        "              impls)))\n"
        "(defn satisfies? [proto x]\n"
        "  (every? (fn [m] (contains? (get @cljc/multi-tables m) (type x))) proto))\n"
        /* records: maps tagged with :cljc/type */
        "(defmacro defrecord [rname fields]\n"
        "  (let [kw (keyword (str rname))]\n"
        "    `(defn ~(symbol (str \"->\" rname)) ~fields\n"
        "       (assoc (zipmap ~(mapv keyword (map str fields))\n"
        "                      ~fields)\n"
        "              :cljc/type ~kw))))\n"
        "(defn record? [x] (and (map? x) (contains? x :cljc/type)))\\n"
        "(defmacro reify [& clauses]\n"
        "  (let [t (keyword (str (gensym)))\n"
        "        impls (loop [cs clauses acc (list)]\n"
        "                (cond (empty? cs) (reverse acc)\n"
        "                      (list? (first cs))\n"
        "                      (recur (rest cs)\n"
        "                             (cons (let [[m params & body] (first cs)]\n"
        "                                     (concat (list (quote defmethod) m t (vec params)) body))\n"
        "                                   acc))\n"
        "                      :else (recur (rest cs) acc)))]\n"
        "    (concat (list (quote do)) impls (list {:cljc/type t}))))\n");
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
        "       \" void(*error)(const char*); } CljcFfiApi;\\n\"\n"
        "       \"static CljcFfiApi *api;\\n\"))\n"
        "(defn cljc/ffi-build [code libs]\n"
        "  (let [base (str \"/tmp/cljc-ffi3-\" (Math/abs (hash (str code libs))))]\n"
        "    (when-not (zero? (:exit (sh (str \"test -f \" base \".so\"))))\n"
        "      (spit (str base \".c\") code)\n"
        "      (let [r (sh (str \"cc -shared -fPIC -O2 -o \" base \".so \" base \".c \" libs))]\n"
        "        (when-not (zero? (:exit r))\n"
        "          (throw (ex-info (str \"ffi: compile failed:\\n\" (:out r)) {})))))\n"
        "    (ffi-load* (str base \".so\"))))\n"
        "(defn cljc/ffi-ret [t expr]\n"
        "  (case t\n"
        "    :int (str \"return api->mk_int(\" expr \");\")\n"
        "    :double (str \"return api->mk_double(\" expr \");\")\n"
        "    :string (str \"return api->mk_str(\" expr \");\")\n"
        "    :pointer (str \"return api->mk_int((long long)(\" expr \"));\")\n"
        "    :void (str expr \"; return api->nil();\")))\n"
        "(defn cljc/ffi-arg [t i]\n"
        "  (case t\n"
        "    :int (str \"api->as_int(api->nth_arg(args, \" i \"))\")\n"
        "    :double (str \"api->as_double(api->nth_arg(args, \" i \"))\")\n"
        "    :string (str \"api->as_str(api->nth_arg(args, \" i \"))\")\n"
        "    :pointer (str \"(void*)api->as_int(api->nth_arg(args, \" i \"))\")))\n"
        "(defn cljc/ffi-wrapper [[ret cname argts]]\n"
        "  (str \"static void *w_\" cname \"(void *env, void *args, int nargs) { (void)env; \"\n"
        "       \"if (nargs < \" (count argts) \") api->error(\\\"\" cname \": too few args\\\"); \"\n"
        "       (cljc/ffi-ret ret (str cname \"(\"\n"
        "                              (str/join \", \" (map-indexed (fn [i t] (cljc/ffi-arg t i)) argts))\n"
        "                              \")\"))\n"
        "       \" }\\n\"))\n"
        "(defn ffi/define*\n"
        "  ([sigs] (ffi/define* sigs {}))\n"
        "  ([sigs {:keys [headers libs prefix] :or {headers [] libs \"\" prefix \"\"}}]\n"
        "   (let [base (atom nil)\n"
        "         api-decl cljc/ffi-api-decl\n"
        "         code (str \"#define _GNU_SOURCE\\n\"\n"
        "                   (str/join \"\" (map (fn [h] (str \"#include <\" h \">\\n\")) headers))\n"
        "                   api-decl\n"
        "                   (str/join \"\" (map cljc/ffi-wrapper sigs))\n"
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
        "                       pfx (cljc/ffi-ret t (str \"p->\" f)) \" }\\n\"))\n"
        "         setter (fn [[t f]]\n"
        "                  (str \"static void *w_set_\" sn \"_\" f \"(void *env, void *args, int nargs) { (void)env; (void)nargs; \"\n"
        "                       pfx \"p->\" f \" = \" (cljc/ffi-arg t 1) \"; return api->nil(); }\\n\"))\n"
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
    "    (eval (read-string (str \"(do \" src \")\")))))\n");
    return e;
}

Cljc *cljc_eval_string(CljcEnv *env, const char *src) {
    char stack_anchor;
    cljc_set_stack_base(&stack_anchor);  /* ensure at least this frame is scanned */
    Cljc * volatile result = NIL;  /* survives the error longjmp */
    if (setjmp(err_jmp) != 0) { print_error(); vsp = 0; eval_sp = 0; return NIL; }
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

#include <termios.h>

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

static int nrepl_server(CljcEnv *env, int port) {
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
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        rlim_t want = 1024ul * 1024 * 1024;   /* 1 GB */
        if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
        if (want > rl.rlim_cur) { rl.rlim_cur = want; setrlimit(RLIMIT_STACK, &rl); }
    }
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
    if (!strcmp(cmd, "judge")) {
        /* inline snapshot tests; judge/main returns the exit code (2 on
         * load errors so editors can tell them from test failures) */
        set_args(env, argc, argv, 2);
        if (setjmp(err_jmp) != 0) { print_error(); return 2; }
        const char *prog = "(load-file \"judge.clj\") (judge/main)";
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
