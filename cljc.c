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
#ifndef CLJC_NO_MAIN
#include <unistd.h>  /* isatty — REPL vs script-mode detection */
#endif

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
    CLJC_SET,       /* persistent set: a HAMT keyed by its elements (uses as.map) */
    CLJC_HNODE,     /* internal: HAMT tree node — never user-visible */
    CLJC_RECUR,     /* sentinel: (recur args...) — bubbles to enclosing loop */
    CLJC_FREE,      /* internal: swept cell on the free list — never user-visible */
} CljcTag;

typedef struct Cljc Cljc;
typedef struct CljcEnv CljcEnv;
typedef Cljc *(*CljcNativeFn)(CljcEnv *env, Cljc *args);

struct Cljc {
    CljcTag tag;
    uint8_t gcmark;
    union {
        bool b;
        int64_t i;
        double d;
        const char *sym;
        const char *kw;
        char *str;
        struct { Cljc *head; Cljc *tail; } cons;
        /* Persistent vector: 32-way position trie + tail of the last ≤32
         * elements. tail is owned by THIS cell (copied per derived vector);
         * tree nodes are shared CLJC_HNODE cells. root NULL → all in tail. */
        struct { Cljc *root; Cljc **tail; uint32_t count; uint8_t shift; uint8_t taillen; } vec;
        /* HAMT persistent map: root tree node (NULL when empty) + entry count */
        struct { Cljc *root; size_t count; } map;
        /* HAMT node. kids interleaves [k1,v1,k2,v2...]; k==NULL → v is a
         * subnode. Collision nodes hold same-hash entries linearly. */
        struct { Cljc **kids; uint32_t bitmap; uint32_t chash; uint16_t nkids; bool collision; } hnode;
        /* arities: list of (params-list . body-list) pairs; dispatch by argc */
        struct { Cljc *arities; CljcEnv *env; bool is_macro; } fn;
        CljcNativeFn native;
        struct { Cljc *value; } atom;
        struct { Cljc **vals; size_t n; } recur;
    } as;
};

/* ───── Environment (lexical scope, linked frames) ───────────────────── */

typedef struct Binding {
    const char *name;   /* interned symbol pointer — compare by == */
    Cljc *value;
    struct Binding *next;
} Binding;

struct CljcEnv {
    Binding *bindings;
    CljcEnv *parent;    /* doubles as the free-list next pointer when swept */
    uint8_t gcmark;
    uint8_t gcfree;
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
static Cljc *cell_alloc(void);
static CljcEnv *env_alloc(void);
static size_t vec_len(Cljc *v);
static Cljc *vec_nth(Cljc *v, size_t i);
static Cljc *NIL, *TRUE, *FALSE;

/* ───── Error handling ───────────────────────────────────────────────── */

/* Errors unwind to the innermost handler frame — pushed by the `try`
 * special form — or, with no try in flight, to the top-level err_jmp set by
 * the REPL/script/eval-string entry points. The exception itself is a value:
 * cur_exc when (throw x) raised it, or NULL meaning "use err_msg" for
 * interpreter-raised errors. */

typedef struct ErrFrame { jmp_buf jb; struct ErrFrame *prev; } ErrFrame;
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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
static void cljc_error(const char *fmt, ...) {
    cur_exc = NULL;  /* message-style error */
    va_list ap; va_start(ap, fmt);
    vsnprintf(err_msg, sizeof err_msg, fmt, ap);
    va_end(ap);
    cljc_raise();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void cljc_throw_value(Cljc *v) {
    cur_exc = v;
    snprintf(err_msg, sizeof err_msg, "uncaught exception");
    cljc_raise();
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) cljc_error("out of memory");
    return p;
}

/* ───── Constructors ─────────────────────────────────────────────────── */

static Cljc *alloc(CljcTag t) {
    Cljc *v = cell_alloc();  /* pooled; may trigger a GC before carving */
    v->tag = t;
    return v;
}

static Cljc *mk_int(int64_t i)       { Cljc *v = alloc(CLJC_INT);    v->as.i = i; return v; }
static Cljc *mk_double(double d)     { Cljc *v = alloc(CLJC_DOUBLE); v->as.d = d; return v; }
static Cljc *mk_bool(bool b)         { return b ? TRUE : FALSE; }
static Cljc *mk_sym(const char *s)   { Cljc *v = alloc(CLJC_SYMBOL); v->as.sym = s; return v; }
static Cljc *mk_kw(const char *s)    { Cljc *v = alloc(CLJC_KEYWORD); v->as.kw = s; return v; }
static Cljc *mk_str(const char *s, size_t n) {
    Cljc *v = alloc(CLJC_STRING);
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
    return e;
}

static void env_define(CljcEnv *env, const char *name, Cljc *value) {
    Binding *b = xmalloc(sizeof *b);
    b->name = name; b->value = value; b->next = env->bindings;
    env->bindings = b;
}

static Cljc *env_lookup(CljcEnv *env, const char *name) {
    for (CljcEnv *e = env; e; e = e->parent)
        for (Binding *b = e->bindings; b; b = b->next)
            if (b->name == name) return b->value;
    cljc_error("unable to resolve symbol: %s", name);
    return NIL;
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

#define GC_MIN_THRESHOLD 65536
static size_t gc_allocs, gc_threshold = GC_MIN_THRESHOLD;
static size_t gc_freed_last;
static bool gc_stress;             /* CLJC_GC_STRESS=1: collect every 512 allocs */
static void *gc_stack_base;
static CljcEnv *gc_root_envs[8];
static int gc_n_root_envs;

static void gc_mark_env(CljcEnv *e);

static void gc_mark(Cljc *v) {
    while (v && !v->gcmark) {
        if (v->tag == CLJC_FREE) return;
        v->gcmark = 1;
        switch (v->tag) {
            case CLJC_LIST:
                gc_mark(v->as.cons.head);
                v = v->as.cons.tail;      /* iterate tails: lists can be huge */
                break;
            case CLJC_VECTOR:
                for (size_t i = 0; i < v->as.vec.taillen; i++) gc_mark(v->as.vec.tail[i]);
                v = v->as.vec.root;  /* NULL-safe */
                break;
            case CLJC_MAP:
            case CLJC_SET:
                v = v->as.map.root;  /* NULL-safe: loop condition handles it */
                break;
            case CLJC_HNODE:
                for (size_t i = 0; i < v->as.hnode.nkids; i++)
                    gc_mark(v->as.hnode.kids[i]);  /* NULL slots skip in gc_mark */
                return;
            case CLJC_RECUR:
                for (size_t i = 0; i < v->as.recur.n; i++) gc_mark(v->as.recur.vals[i]);
                return;
            case CLJC_FN:
                gc_mark_env(v->as.fn.env);
                v = v->as.fn.arities;  /* a list of cons pairs — generic list marking */
                break;
            case CLJC_ATOM:
                v = v->as.atom.value;
                break;
            default:
                return;
        }
    }
}

static void gc_mark_env(CljcEnv *e) {
    while (e && !e->gcmark && !e->gcfree) {
        e->gcmark = 1;
        for (Binding *b = e->bindings; b; b = b->next) gc_mark(b->value);
        e = e->parent;
    }
}

/* If w points into a pool block (interior pointers included), mark the
 * containing object. */
static void gc_mark_conservative(uintptr_t w) {
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

    /* Flush callee-saved registers onto the stack so the scan sees them. */
    jmp_buf regs;
    setjmp(regs);

    gc_mark(NIL); gc_mark(TRUE); gc_mark(FALSE);
    gc_mark(cur_exc);  /* exception value may be in flight between throw and catch */
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
                    case CLJC_VECTOR: free(c->as.vec.tail); break;
                    case CLJC_HNODE:  free(c->as.hnode.kids); break;
                    case CLJC_RECUR:  free(c->as.recur.vals); break;
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
                    free(bn);
                    bn = next;
                }
                e->gcfree = 1;
            }
            e->bindings = NULL;
            e->parent = env_freelist;
            env_freelist = e;
        }
    }

    gc_freed_last = freed;
    gc_threshold = live * 2 > GC_MIN_THRESHOLD ? live * 2 : GC_MIN_THRESHOLD;
}

static void maybe_gc(void) {
    if (++gc_allocs >= (gc_stress ? 512 : gc_threshold)) gc_collect();
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

/* Allocate a node with zeroed kid slots. Callers fill the slots immediately
 * (no allocation in between), so a mid-fill GC can never trace garbage. */
static Cljc *mk_hnode(uint32_t bitmap, uint16_t nkids, bool collision, uint32_t chash) {
    Cljc *n = alloc(CLJC_HNODE);
    n->as.hnode.kids = xmalloc(sizeof(Cljc *) * (nkids ? nkids : 1));
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
    return nm;
}

static Cljc *map_dissoc_one(Cljc *m, Cljc *k) {
    bool removed = false;
    Cljc *root = hamt_dissoc(m->as.map.root, 0, cljc_hash(k), k, &removed);
    if (!removed) return m;
    Cljc *nm = alloc(CLJC_MAP);
    nm->as.map.root = root;
    nm->as.map.count = m->as.map.count - 1;
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
    return ns;
}

static Cljc *set_disj(Cljc *s, Cljc *x) {
    bool removed = false;
    Cljc *root = hamt_dissoc(s->as.map.root, 0, cljc_hash(x), x, &removed);
    if (!removed) return s;
    Cljc *ns = alloc(CLJC_SET);
    ns->as.map.root = root;
    ns->as.map.count = s->as.map.count - 1;
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
    nv->as.vec.tail = xmalloc(sizeof(Cljc *) * (n ? n : 1));
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
    if (cnt - vec_tailoff(v) < 32)  /* room in tail: the cheap path */
        return vec_cell(v->as.vec.root, v->as.vec.shift, cnt + 1,
                        v->as.vec.tail, v->as.vec.taillen, x);
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
    return vec_cell(newroot, newshift, cnt + 1, &x, 0, x) /* tail = [x] */;
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
        return nv;
    }
    Cljc *newroot = vec_doassoc(v->as.vec.shift, v->as.vec.root, i, x);
    return vec_cell(newroot, v->as.vec.shift, cnt,
                    v->as.vec.tail, v->as.vec.taillen, NULL);
}

/* Build a vector from a C array (small n — entry pairs, literals). */
static Cljc *mk_vector(Cljc **items, size_t n) {
    Cljc *v = mk_empty_vec();
    for (size_t i = 0; i < n; i++) v = vec_conj1(v, items[i]);
    return v;
}

static Cljc *cell_alloc(void) {
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
    memset(&c->as, 0, sizeof c->as);  /* half-built cells are always safe to mark/sweep */
    c->gcmark = 0;
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

static void skip_ws(const char **p) {
    while (**p) {
        if (isspace((unsigned char)**p) || **p == ',') { (*p)++; }
        else if (**p == ';') { while (**p && **p != '\n') (*p)++; }
        else break;
    }
}

static bool is_sym_char(int c) {
    if (c == '\0' || isspace(c)) return false;
    return !strchr("()[]{}\";'`,~@", c);
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
                case '\\': sb_putc(&sb, '\\'); break;
                case '"':  sb_putc(&sb, '"');  break;
                case '\0': cljc_error("unterminated string");
                default:   cljc_error("unsupported escape: \\%c", **p);
            }
        } else sb_putc(&sb, c);
        (*p)++;
    }
    if (**p != '"') cljc_error("unterminated string");
    (*p)++; /* consume closing " */
    Cljc *r = mk_str(sb.data ? sb.data : "", sb.len);
    free(sb.data);
    return r;
}

static Cljc *read_list(const char **p, char close) {
    (*p)++; /* consume open */
    Cljc *head = NIL, **tail = &head;
    for (;;) {
        skip_ws(p);
        if (**p == '\0') cljc_error("unterminated list");
        if (**p == close) { (*p)++; return head; }
        Cljc *item = read_form(p);
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

static const char *sym_amp(void) { return intern("&", 1); }

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
        Cljc *s = value == NIL ? NIL : to_seq(value);
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
            if (s != NIL) s = s->as.cons.tail;
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

/* Bind an arity's params to args. Each param may be any destructuring
 * pattern; '&' collects the remaining args as a list. */
static void bind_params(CljcEnv *call, Cljc *params, Cljc *args) {
    Cljc *p = params, *a = args;
    while (p && p->tag == CLJC_LIST) {
        Cljc *pat = p->as.cons.head;
        if (pat->tag == CLJC_SYMBOL && pat->as.sym == sym_amp()) {
            destructure(call, p->as.cons.tail->as.cons.head, a);
            return;
        }
        if (a == NIL || a->tag != CLJC_LIST) cljc_error("not enough arguments");
        destructure(call, pat, a->as.cons.head);
        p = p->as.cons.tail; a = a->as.cons.tail;
    }
    if (a != NIL) cljc_error("too many arguments");
}

static void arity_info(Cljc *params, size_t *fixed, bool *variadic) {
    *fixed = 0; *variadic = false;
    for (Cljc *p = params; p && p->tag == CLJC_LIST; p = p->as.cons.tail) {
        Cljc *h = p->as.cons.head;
        if (h->tag == CLJC_SYMBOL && h->as.sym == sym_amp()) { *variadic = true; return; }
        (*fixed)++;
    }
}

static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc *args) {
    if (fn->tag == CLJC_NATIVE) return fn->as.native(env, args);
    if (fn->tag == CLJC_FN) {
        for (;;) {
            /* Dispatch: exact param-count match wins; variadic is fallback. */
            size_t nargs = list_len(args);
            Cljc *chosen = NULL, *fallback = NULL;
            for (Cljc *ar = fn->as.fn.arities; ar && ar->tag == CLJC_LIST; ar = ar->as.cons.tail) {
                Cljc *arity = ar->as.cons.head;
                size_t fixed; bool variadic;
                arity_info(arity->as.cons.head, &fixed, &variadic);
                if (!variadic && nargs == fixed) { chosen = arity; break; }
                if (variadic && nargs >= fixed && !fallback) fallback = arity;
            }
            if (!chosen) chosen = fallback;
            if (!chosen) cljc_error("no matching arity for %zu args", nargs);
            CljcEnv *call = env_new(fn->as.fn.env);
            bind_params(call, chosen->as.cons.head, args);
            Cljc *result = eval_body(call, chosen->as.cons.tail);
            if (!(result && result->tag == CLJC_RECUR)) return result;
            /* (recur ...) in fn tail position: rebuild the arg list and loop. */
            Cljc *newargs = NIL, **t = &newargs;
            for (size_t i = 0; i < result->as.recur.n; i++) {
                *t = mk_cons(result->as.recur.vals[i], NIL);
                t = &(*t)->as.cons.tail;
            }
            args = newargs;
        }
    }
    /* Keywords as functions: (:key m) and (:key m default). */
    if (fn->tag == CLJC_KEYWORD) {
        Cljc *m = args->as.cons.head;
        Cljc *dflt = args->as.cons.tail != NIL ? args->as.cons.tail->as.cons.head : NIL;
        Cljc *out;
        if (m != NIL && m->tag == CLJC_MAP && map_find(m, fn, &out)) return out;
        return dflt;
    }
    /* Maps as functions: (m key) and (m key default). */
    if (fn->tag == CLJC_MAP) {
        Cljc *k = args->as.cons.head;
        Cljc *dflt = args->as.cons.tail != NIL ? args->as.cons.tail->as.cons.head : NIL;
        Cljc *out;
        if (map_find(fn, k, &out)) return out;
        return dflt;
    }
    /* Sets as functions: (#{:a} :a) => :a, else nil/default. */
    if (fn->tag == CLJC_SET) {
        Cljc *x = args->as.cons.head;
        Cljc *dflt = args->as.cons.tail != NIL ? args->as.cons.tail->as.cons.head : NIL;
        Cljc *out;
        if (set_contains(fn, x, &out)) return out;
        return dflt;
    }
    /* Vectors as functions: ([10 20 30] 1) => 20. */
    if (fn->tag == CLJC_VECTOR) {
        Cljc *k = args->as.cons.head;
        if (k->tag != CLJC_INT) cljc_error("vector lookup needs an integer index");
        if (k->as.i < 0 || (size_t)k->as.i >= vec_len(fn))
            cljc_error("vector index out of bounds: %lld", (long long)k->as.i);
        return vec_nth(fn, (size_t)k->as.i);
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
    /* Atoms are template literals. */
    return form;
}

static Cljc *eval(CljcEnv *env, Cljc *form) {
    if (form == NULL || form == NIL) return NIL;
    switch (form->tag) {
        case CLJC_INT: case CLJC_DOUBLE: case CLJC_BOOL: case CLJC_NIL:
        case CLJC_STRING: case CLJC_KEYWORD: case CLJC_FN: case CLJC_NATIVE:
        case CLJC_ATOM:
        case CLJC_RECUR:   /* not produced by the reader; appears only inside loop */
            return form;
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
            return env_lookup(env, form->as.sym);
        case CLJC_LIST: {
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
                    err_top = &frame;
                    if (setjmp(frame.jb) == 0) {
                        result = eval_body(env, body_v);
                        err_top = frame.prev;
                    } else {
                        err_top = frame.prev;
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
                            err_top = &hframe;
                            if (setjmp(hframe.jb) == 0) {
                                result = eval_body(scope, cc->as.cons.tail->as.cons.tail);
                                err_top = hframe.prev;
                            } else {
                                err_top = hframe.prev;
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
                if (s == SYM_QUASIQUOTE) return qq_expand(env, rest->as.cons.head);
                if (s == SYM_DEFMACRO) {
                    /* (defmacro name [params] body...) — a fn flagged so that
                     * eval calls it on unevaluated forms and re-evals the result. */
                    need_args(rest, 2, "defmacro");
                    const char *name = sym_name(rest->as.cons.head, "defmacro");
                    Cljc *m = make_fn(env, rest->as.cons.tail, true);
                    env_define(env_root(env), name, m);
                    return m;
                }
                if (s == SYM_DEFN) {
                    /* (defn name [params] body...) ≡ (def name (fn [params] body...)) */
                    need_args(rest, 2, "defn");
                    Cljc *name = rest->as.cons.head;
                    Cljc *fn_form = mk_cons(mk_sym(SYM_FN), rest->as.cons.tail);
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
                    const char *name = sym_name(rest->as.cons.head, "def");
                    Cljc *val = eval(env, rest->as.cons.tail->as.cons.head);
                    env_define(env_root(env), name, val);  /* def is always global */
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
                    Cljc *r = alloc(CLJC_RECUR);
                    r->as.recur.vals = xmalloc(sizeof(Cljc *) * (n ? n : 1));
                    r->as.recur.n = 0;  /* grows as slots fill — GC safety */
                    size_t i = 0;
                    for (Cljc *a = rest; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
                        r->as.recur.vals[i++] = eval(env, a->as.cons.head);
                        r->as.recur.n = i;
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
                            cljc_error("recur arity mismatch: expected %zu, got %zu",
                                       nparams, r->as.recur.n);
                        for (size_t i = 0; i < nparams; i++) {
                            for (Binding *b = scope->bindings; b; b = b->next) {
                                if (b->name == names[i]) { b->value = r->as.recur.vals[i]; break; }
                            }
                        }
                    }
                }
                if (s == SYM_FN)
                    /* (fn [x y] body...) or (fn ([x] ...) ([x y] ...)) */
                    return make_fn(env, rest, false);
            }

            /* Application. Macros get the unevaluated forms; the expansion
             * is then evaluated in the caller's environment. */
            Cljc *fn = eval(env, head);
            if (fn->tag == CLJC_FN && fn->as.fn.is_macro)
                return eval(env, apply(env, fn, rest));
            Cljc *args = eval_list(env, rest);
            return apply(env, fn, args);
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
            for (Cljc *l = v; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
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
        case CLJC_HNODE: sb_puts(sb, "#<hamt-node>"); break;  /* never user-visible */
        case CLJC_FN:     sb_puts(sb, "#<fn>"); break;
        case CLJC_NATIVE: sb_puts(sb, "#<native>"); break;
        case CLJC_ATOM:
            sb_puts(sb, "#atom[");
            print_to(sb, v->as.atom.value, readably);
            sb_putc(sb, ']');
            break;
        case CLJC_RECUR:  sb_puts(sb, "#<recur>"); break;
        case CLJC_FREE:   sb_puts(sb, "#<freed!>"); break;  /* seeing this is a GC bug */
    }
}

static void print(Cljc *v) {
    SBuf sb = {0};
    print_to(&sb, v, true);
    if (sb.data) { fwrite(sb.data, 1, sb.len, stdout); free(sb.data); }
}

/* Top-level (uncaught) error report. Resets the in-flight exception. */
static void print_error(void) {
    fputs("error: ", stderr);
    if (cur_exc) {
        SBuf sb = {0};
        print_to(&sb, cur_exc, true);
        if (sb.data) { fwrite(sb.data, 1, sb.len, stderr); free(sb.data); }
        cur_exc = NULL;
    } else {
        fputs(err_msg, stderr);
    }
    fputc('\n', stderr);
    err_top = NULL;  /* hygiene: no handler frames survive a top-level unwind */
}

/* ───── Primitives ───────────────────────────────────────────────────── */

/* Arithmetic fold with Clojure semantics:
 *   - ints stay ints; any double promotes the whole result
 *   - unary: (- x) negates, (/ x) reciprocates
 *   - integer / that doesn't divide evenly promotes to double
 *     (real Clojure makes a Ratio — a deliberate v0 divergence) */
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV } ArithOp;

static Cljc *arith(ArithOp op, Cljc *args) {
    size_t n = 0;
    bool is_float = false;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *v = a->as.cons.head;
        if (v->tag == CLJC_DOUBLE) is_float = true;
        else if (v->tag != CLJC_INT) cljc_error("expected number");
        n++;
    }
    if (n == 0) {
        if (op == OP_ADD) return mk_int(0);
        if (op == OP_MUL) return mk_int(1);
        cljc_error("wrong number of args (0)");
    }

    if (!is_float) {
        int64_t acc = args->as.cons.head->as.i;
        if (n == 1) {
            if (op == OP_SUB) return mk_int(-acc);
            if (op == OP_DIV) {
                if (acc == 0) cljc_error("division by zero");
                return acc == 1 || acc == -1 ? mk_int(acc) : mk_double(1.0 / (double)acc);
            }
            return mk_int(acc);
        }
        for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
            int64_t x = a->as.cons.head->as.i;
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
    double facc = as_num(args->as.cons.head);
    if (n == 1) {
        if (op == OP_SUB) return mk_double(-facc);
        if (op == OP_DIV) return mk_double(1.0 / facc);
        return mk_double(facc);
    }
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        double x = as_num(a->as.cons.head);
        switch (op) {
            case OP_ADD: facc += x; break;
            case OP_SUB: facc -= x; break;
            case OP_MUL: facc *= x; break;
            case OP_DIV: facc /= x; break;  /* /0.0 → ±Infinity, like Clojure doubles */
        }
    }
    return mk_double(facc);
}

static Cljc *prim_add(CljcEnv *env, Cljc *args) { (void)env; return arith(OP_ADD, args); }
static Cljc *prim_sub(CljcEnv *env, Cljc *args) { (void)env; return arith(OP_SUB, args); }
static Cljc *prim_mul(CljcEnv *env, Cljc *args) { (void)env; return arith(OP_MUL, args); }
static Cljc *prim_div(CljcEnv *env, Cljc *args) { (void)env; return arith(OP_DIV, args); }

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
    bool a_seq = a->tag == CLJC_LIST || a->tag == CLJC_VECTOR;
    bool b_seq = b->tag == CLJC_LIST || b->tag == CLJC_VECTOR;
    if (a_seq && b_seq) return seq_eq(a, b);
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

static Cljc *prim_eq(CljcEnv *env, Cljc *args) {
    (void)env;
    if (args == NIL) return TRUE;
    Cljc *first = args->as.cons.head;
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        if (!cljc_eq(first, a->as.cons.head)) return FALSE;
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
    static Cljc *prim_##NAME(CljcEnv *env, Cljc *args) { \
        (void)env; \
        Cljc *prev = NULL; \
        for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) { \
            Cljc *v = a->as.cons.head; \
            if (prev && !(as_num(prev) OP as_num(v))) return FALSE; \
            prev = v; \
        } \
        return TRUE; \
    }

COMPARISON(lt, <)
COMPARISON(gt, >)
COMPARISON(le, <=)
COMPARISON(ge, >=)

static Cljc *prim_println(CljcEnv *env, Cljc *args) {
    (void)env;
    SBuf sb = {0};
    bool first = true;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, a->as.cons.head, false);
    }
    sb_putc(&sb, '\n');
    fwrite(sb.data, 1, sb.len, stdout);
    free(sb.data);
    return NIL;
}

static Cljc *prim_str(CljcEnv *env, Cljc *args) {
    (void)env;
    SBuf sb = {0};
    sb_grow(&sb, 1); sb.data[0] = '\0';
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *v = a->as.cons.head;
        if (v != NIL) print_to(&sb, v, false);  /* (str nil) => "" */
    }
    Cljc *r = mk_str(sb.data, sb.len);
    free(sb.data);
    return r;
}

static Cljc *prim_pr_str(CljcEnv *env, Cljc *args) {
    (void)env;
    SBuf sb = {0};
    sb_grow(&sb, 1); sb.data[0] = '\0';
    bool first = true;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, a->as.cons.head, true);
    }
    Cljc *r = mk_str(sb.data, sb.len);
    free(sb.data);
    return r;
}

static Cljc *prim_not(CljcEnv *env, Cljc *args) {
    (void)env;
    return mk_bool(!is_truthy(args->as.cons.head));
}

static Cljc *prim_count(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v == NIL) return mk_int(0);
    if (v->tag == CLJC_LIST) return mk_int((int64_t)list_len(v));
    if (v->tag == CLJC_VECTOR) return mk_int((int64_t)vec_len(v));
    if (v->tag == CLJC_MAP || v->tag == CLJC_SET)
        return mk_int((int64_t)v->as.map.count);
    if (v->tag == CLJC_STRING) return mk_int((int64_t)strlen(v->as.str));
    cljc_error("count: not countable");
    return NIL;
}

static Cljc *prim_nth(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *coll = args->as.cons.head;
    int64_t n = as_int(args->as.cons.tail->as.cons.head, "nth");
    Cljc *not_found = args->as.cons.tail->as.cons.tail != NIL
        ? args->as.cons.tail->as.cons.tail->as.cons.head : NULL;
    if (coll && coll->tag == CLJC_VECTOR) {
        if (n >= 0 && (size_t)n < vec_len(coll)) return vec_nth(coll, (size_t)n);
    } else if (coll && coll->tag == CLJC_LIST) {
        for (Cljc *l = coll; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
            if (n-- == 0) return l->as.cons.head;
    }
    if (not_found) return not_found;
    cljc_error("nth: index out of bounds");
    return NIL;
}

static Cljc *prim_conj(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *r = args->as.cons.head;  /* nil works: conj onto nil yields a list */
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *x = a->as.cons.head;
        if (r == NIL || r->tag == CLJC_LIST) {
            r = mk_cons(x, r);                      /* lists grow at the front */
        } else if (r->tag == CLJC_VECTOR) {
            r = vec_conj1(r, x);                    /* vectors grow at the back */
        } else if (r->tag == CLJC_SET) {
            r = set_conj(r, x);
        } else cljc_error("conj: not a collection");
    }
    return r;
}

static Cljc *prim_vector(CljcEnv *env, Cljc *args) {
    (void)env;
    size_t n = list_len(args);
    Cljc **items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
    size_t i = 0;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        items[i++] = a->as.cons.head;
    Cljc *v = mk_vector(items, n);
    free(items);
    return v;
}

static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc *args);

static Cljc *prim_apply(CljcEnv *env, Cljc *args) {
    /* (apply f a b [c d]) => (f a b c d) — last arg is spliced. */
    Cljc *fn = args->as.cons.head;
    Cljc *out = NIL, **t = &out;
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        if (a->as.cons.tail == NIL) {
            /* Splice the final collection. */
            Cljc *last = a->as.cons.head;
            if (last == NIL) break;
            if (last->tag == CLJC_LIST) { *t = last; }
            else if (last->tag == CLJC_VECTOR) {
                for (size_t i = 0; i < vec_len(last); i++) {
                    *t = mk_cons(vec_nth(last, i), NIL);
                    t = &(*t)->as.cons.tail;
                }
            } else cljc_error("apply: last argument must be a collection");
            break;
        }
        *t = mk_cons(a->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    return apply(env, fn, out);
}

#define TYPE_PRED(NAME, EXPR) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc *args) { \
        (void)env; Cljc *v = args->as.cons.head; (void)v; \
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

static Cljc *prim_empty_p(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v == NIL) return TRUE;
    if (v->tag == CLJC_LIST) return FALSE;  /* a cons is never empty */
    if (v->tag == CLJC_VECTOR) return mk_bool(vec_len(v) == 0);
    if (v->tag == CLJC_MAP || v->tag == CLJC_SET)
        return mk_bool(v->as.map.count == 0);
    if (v->tag == CLJC_STRING) return mk_bool(v->as.str[0] == '\0');
    cljc_error("empty?: not a collection");
    return NIL;
}

static Cljc *prim_inc(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v->tag == CLJC_DOUBLE) return mk_double(v->as.d + 1);
    return mk_int(as_int(v, "inc") + 1);
}

static Cljc *prim_dec(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v->tag == CLJC_DOUBLE) return mk_double(v->as.d - 1);
    return mk_int(as_int(v, "dec") - 1);
}

static Cljc *prim_mod(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t a = as_int(args->as.cons.head, "mod");
    int64_t b = as_int(args->as.cons.tail->as.cons.head, "mod");
    if (b == 0) cljc_error("mod: division by zero");
    int64_t m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;  /* Clojure mod follows divisor's sign */
    return mk_int(m);
}

/* ── Map primitives (HAMT engine — see the HAMT section above) ── */

static Cljc *prim_hash_map(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = mk_map();
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail->as.cons.tail) {
        if (a->as.cons.tail == NIL) cljc_error("hash-map needs an even number of arguments");
        m = map_assoc(m, a->as.cons.head, a->as.cons.tail->as.cons.head);
    }
    return m;
}

static Cljc *prim_get(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *coll = args->as.cons.head;
    Cljc *k = args->as.cons.tail->as.cons.head;
    Cljc *dflt = args->as.cons.tail->as.cons.tail != NIL
        ? args->as.cons.tail->as.cons.tail->as.cons.head : NIL;
    if (coll != NIL && coll->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(coll, k, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_SET) {
        Cljc *out;
        if (set_contains(coll, k, &out)) return out;
    } else if (coll != NIL && coll->tag == CLJC_VECTOR && k->tag == CLJC_INT) {
        if (k->as.i >= 0 && (size_t)k->as.i < vec_len(coll))
            return vec_nth(coll, (size_t)k->as.i);
    }
    return dflt;
}

static Cljc *prim_assoc(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *coll = args->as.cons.head;
    if (coll == NIL) coll = mk_map();
    Cljc *r = coll;
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail->as.cons.tail) {
        if (a->as.cons.tail == NIL) cljc_error("assoc needs key-value pairs");
        Cljc *k = a->as.cons.head, *v = a->as.cons.tail->as.cons.head;
        if (r->tag == CLJC_MAP) r = map_assoc(r, k, v);
        else if (r->tag == CLJC_VECTOR) {
            if (k->tag != CLJC_INT || k->as.i < 0)
                cljc_error("assoc on vector: index out of bounds");
            r = vec_assoc_idx(r, (size_t)k->as.i, v);  /* assoc at len appends */
        } else cljc_error("assoc: not associative");
    }
    return r;
}

static Cljc *prim_dissoc(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = args->as.cons.head;
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("dissoc: not a map");
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        m = map_dissoc_one(m, a->as.cons.head);
    return m;
}

static Cljc *prim_keys(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = args->as.cons.head;
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("keys: not a map");
    Cljc *out = NIL, **t = &out;
    for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
        *t = mk_cons(e->as.cons.head->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_vals(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = args->as.cons.head;
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("vals: not a map");
    Cljc *out = NIL, **t = &out;
    for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail) {
        *t = mk_cons(e->as.cons.head->as.cons.tail, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_contains_p(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *coll = args->as.cons.head;
    Cljc *k = args->as.cons.tail->as.cons.head;
    if (coll == NIL) return FALSE;
    if (coll->tag == CLJC_MAP) return mk_bool(map_find(coll, k, NULL));
    if (coll->tag == CLJC_SET) return mk_bool(set_contains(coll, k, NULL));
    if (coll->tag == CLJC_VECTOR)  /* contains? checks INDEX presence on vectors */
        return mk_bool(k->tag == CLJC_INT && k->as.i >= 0 && (size_t)k->as.i < vec_len(coll));
    cljc_error("contains?: not associative");
    return NIL;
}

static Cljc *prim_merge(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *r = NIL;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *m = a->as.cons.head;
        if (m == NIL) continue;
        if (m->tag != CLJC_MAP) cljc_error("merge: not a map");
        if (r == NIL) { r = m; continue; }
        for (Cljc *e = map_entry_list(m); e && e->tag == CLJC_LIST; e = e->as.cons.tail)
            r = map_assoc(r, e->as.cons.head->as.cons.head, e->as.cons.head->as.cons.tail);
    }
    return r;
}

static Cljc *prim_rem(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t a = as_int(args->as.cons.head, "rem");
    int64_t b = as_int(args->as.cons.tail->as.cons.head, "rem");
    if (b == 0) cljc_error("rem: division by zero");
    return mk_int(a % b);
}

static Cljc *prim_list(CljcEnv *env, Cljc *args) { (void)env; return args; }

static Cljc *prim_first(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = to_seq(args->as.cons.head);  /* lists/vectors/maps uniformly */
    return s == NIL ? NIL : s->as.cons.head;
}

static Cljc *prim_rest(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = to_seq(args->as.cons.head);
    return s == NIL ? NIL : s->as.cons.tail;
}

static Cljc *prim_second(CljcEnv *env, Cljc *args) {
    Cljc *r = prim_rest(env, args);
    if (r == NIL) return NIL;
    return r->as.cons.head;
}

static Cljc *prim_cons(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *h = args->as.cons.head;
    /* to_seq keeps lists proper: (cons 1 [2 3]) => (1 2 3), (cons 1 2) errors.
     * Improper dotted pairs aren't a Clojure concept. */
    Cljc *t = to_seq(args->as.cons.tail->as.cons.head);
    return mk_cons(h, t);
}

/* ── Seq library ── */

/* Normalize any seqable to a list cursor (lists pass through, vectors copy).
 * The HAMT/lazy-seq milestone replaces this with a real ISeq protocol. */
static Cljc *to_seq(Cljc *v) {
    if (v == NIL) return NIL;
    if (v->tag == CLJC_LIST) return v;
    if (v->tag == CLJC_VECTOR) {
        Cljc *out = NIL, **t = &out;
        for (size_t i = 0; i < vec_len(v); i++) {
            *t = mk_cons(vec_nth(v, i), NIL);
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

static Cljc *prim_map(CljcEnv *env, Cljc *args) {
    Cljc *f = args->as.cons.head;
    Cljc *seq = to_seq(args->as.cons.tail->as.cons.head);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *t = mk_cons(apply(env, f, mk_cons(l->as.cons.head, NIL)), NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_filter(CljcEnv *env, Cljc *args) {
    Cljc *f = args->as.cons.head;
    Cljc *seq = to_seq(args->as.cons.tail->as.cons.head);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        if (is_truthy(apply(env, f, mk_cons(l->as.cons.head, NIL)))) {
            *t = mk_cons(l->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
    }
    return out;
}

static Cljc *prim_reduce(CljcEnv *env, Cljc *args) {
    /* (reduce f coll) or (reduce f init coll) */
    Cljc *f = args->as.cons.head;
    Cljc *acc, *seq;
    if (args->as.cons.tail->as.cons.tail == NIL) {
        seq = to_seq(args->as.cons.tail->as.cons.head);
        if (seq == NIL) return apply(env, f, NIL);  /* (reduce f []) => (f) */
        acc = seq->as.cons.head;
        seq = seq->as.cons.tail;
    } else {
        acc = args->as.cons.tail->as.cons.head;
        seq = to_seq(args->as.cons.tail->as.cons.tail->as.cons.head);
    }
    for (Cljc *l = seq; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        acc = apply(env, f, mk_cons(acc, mk_cons(l->as.cons.head, NIL)));
    return acc;
}

static Cljc *prim_range(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t start = 0, end = 0, step = 1;
    size_t n = list_len(args);
    if (n == 1) end = as_int(args->as.cons.head, "range");
    else if (n >= 2) {
        start = as_int(args->as.cons.head, "range");
        end = as_int(args->as.cons.tail->as.cons.head, "range");
        if (n >= 3) step = as_int(args->as.cons.tail->as.cons.tail->as.cons.head, "range");
    }
    if (step == 0) cljc_error("range: step must be nonzero");
    Cljc *out = NIL, **t = &out;
    for (int64_t i = start; step > 0 ? i < end : i > end; i += step) {
        *t = mk_cons(mk_int(i), NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_take(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t n = as_int(args->as.cons.head, "take");
    Cljc *seq = to_seq(args->as.cons.tail->as.cons.head);
    Cljc *out = NIL, **t = &out;
    for (Cljc *l = seq; n-- > 0 && l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
        *t = mk_cons(l->as.cons.head, NIL);
        t = &(*t)->as.cons.tail;
    }
    return out;
}

static Cljc *prim_drop(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t n = as_int(args->as.cons.head, "drop");
    Cljc *seq = to_seq(args->as.cons.tail->as.cons.head);
    while (n-- > 0 && seq != NIL && seq->tag == CLJC_LIST) seq = seq->as.cons.tail;
    return seq;
}

static Cljc *prim_reverse(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *out = NIL;
    for (Cljc *l = to_seq(args->as.cons.head); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        out = mk_cons(l->as.cons.head, out);
    return out;
}

static Cljc *prim_last(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *r = NIL;
    for (Cljc *l = to_seq(args->as.cons.head); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        r = l->as.cons.head;
    return r;
}

static Cljc *prim_concat(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *out = NIL, **t = &out;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        for (Cljc *l = to_seq(a->as.cons.head); l && l->tag == CLJC_LIST; l = l->as.cons.tail) {
            *t = mk_cons(l->as.cons.head, NIL);
            t = &(*t)->as.cons.tail;
        }
    }
    return out;
}

static Cljc *prim_gensym(CljcEnv *env, Cljc *args) {
    (void)env;
    static int counter = 0;
    char buf[64];
    const char *prefix = "G__";
    if (args != NIL && args->as.cons.head->tag == CLJC_STRING)
        prefix = args->as.cons.head->as.str;
    snprintf(buf, sizeof buf, "%s%d", prefix, counter++);
    return mk_sym(intern(buf, strlen(buf)));
}

static Cljc *prim_seq(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = to_seq(args->as.cons.head);
    return s == NIL ? NIL : s;  /* (seq []) => nil, matching Clojure */
}

static Cljc *prim_set(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = mk_set();
    for (Cljc *l = to_seq(args->as.cons.head); l && l->tag == CLJC_LIST; l = l->as.cons.tail)
        s = set_conj(s, l->as.cons.head);
    return s;
}

static Cljc *prim_hash_set(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = mk_set();
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        s = set_conj(s, a->as.cons.head);
    return s;
}

static Cljc *prim_disj(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = args->as.cons.head;
    if (s == NIL) return NIL;
    if (s->tag != CLJC_SET) cljc_error("disj: not a set");
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail)
        s = set_disj(s, a->as.cons.head);
    return s;
}

static Cljc *prim_gc(CljcEnv *env, Cljc *args) {
    (void)env; (void)args;
    gc_collect();
    return mk_int((int64_t)gc_freed_last);  /* cells freed by this collection */
}

/* ── Exceptions ── */

static Cljc *prim_throw(CljcEnv *env, Cljc *args) {
    (void)env;
    cljc_throw_value(args->as.cons.head);
    return NIL;  /* unreachable */
}

static const char *kw_message(void) { return intern("message", 7); }
static const char *kw_data(void)    { return intern("data", 4); }

static Cljc *prim_ex_info(CljcEnv *env, Cljc *args) {
    /* (ex-info msg data) => {:message msg :data data} — exceptions are plain
     * maps here, so all map functions work on them. */
    (void)env;
    Cljc *msg = args->as.cons.head;
    Cljc *data = args->as.cons.tail->as.cons.head;
    Cljc *m = mk_map();
    m = map_assoc(m, mk_kw(kw_message()), msg);
    m = map_assoc(m, mk_kw(kw_data()), data);
    return m;
}

static Cljc *prim_ex_message(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *e = args->as.cons.head;
    if (e != NIL && e->tag == CLJC_STRING) return e;  /* interpreter errors are strings */
    if (e != NIL && e->tag == CLJC_MAP) {
        Cljc *out;
        if (map_find(e, mk_kw(kw_message()), &out)) return out;
    }
    return NIL;
}

static Cljc *prim_ex_data(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *e = args->as.cons.head;
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

static Cljc *prim_atom(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *a = alloc(CLJC_ATOM);
    a->as.atom.value = args->as.cons.head;
    return a;
}

static Cljc *prim_deref(CljcEnv *env, Cljc *args) {
    (void)env;
    return as_atom(args->as.cons.head, "deref")->as.atom.value;
}

static Cljc *prim_reset(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *a = as_atom(args->as.cons.head, "reset!");
    Cljc *v = args->as.cons.tail->as.cons.head;
    a->as.atom.value = v;
    return v;
}

static Cljc *prim_swap(CljcEnv *env, Cljc *args) {
    /* (swap! a f x y) => sets a to (f @a x y), returns the new value. */
    Cljc *a = as_atom(args->as.cons.head, "swap!");
    Cljc *f = args->as.cons.tail->as.cons.head;
    Cljc *extra = args->as.cons.tail->as.cons.tail;
    Cljc *nv = apply(env, f, mk_cons(a->as.atom.value, extra));
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

static Cljc *prim_compare(CljcEnv *env, Cljc *args) {
    (void)env;
    return mk_int(cmp_values(args->as.cons.head, args->as.cons.tail->as.cons.head));
}

/* qsort has no context parameter; the interpreter is single-threaded. */
static CljcEnv *g_sort_env;
static Cljc *g_sort_fn;

static int sort_adapter(const void *pa, const void *pb) {
    Cljc *a = *(Cljc *const *)pa, *b = *(Cljc *const *)pb;
    if (!g_sort_fn) return cmp_values(a, b);
    Cljc *r = apply(g_sort_env, g_sort_fn, mk_cons(a, mk_cons(b, NIL)));
    if (r != NIL && r->tag == CLJC_INT) return (int)r->as.i;
    /* Boolean comparator: (f a b) true => a first; tie-break with (f b a). */
    if (is_truthy(r)) return -1;
    Cljc *r2 = apply(g_sort_env, g_sort_fn, mk_cons(b, mk_cons(a, NIL)));
    return is_truthy(r2) ? 1 : 0;
}

static Cljc *prim_sort(CljcEnv *env, Cljc *args) {
    /* (sort coll) or (sort comparator coll) — returns a list. */
    Cljc *fn = NULL, *coll;
    if (args->as.cons.tail != NIL) {
        fn = args->as.cons.head;
        coll = args->as.cons.tail->as.cons.head;
    } else coll = args->as.cons.head;
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

static Cljc *prim_vec(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *s = to_seq(args->as.cons.head);
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

static Cljc *prim_name(CljcEnv *env, Cljc *args) {
    (void)env;
    const char *n = as_named(args->as.cons.head, "name");
    return mk_str(n, strlen(n));
}

static Cljc *prim_keyword(CljcEnv *env, Cljc *args) {
    (void)env;
    const char *n = as_named(args->as.cons.head, "keyword");
    return mk_kw(intern(n, strlen(n)));
}

static Cljc *prim_symbol(CljcEnv *env, Cljc *args) {
    (void)env;
    const char *n = as_named(args->as.cons.head, "symbol");
    return mk_sym(intern(n, strlen(n)));
}

static Cljc *prim_quot(CljcEnv *env, Cljc *args) {
    (void)env;
    int64_t a = as_int(args->as.cons.head, "quot");
    int64_t b = as_int(args->as.cons.tail->as.cons.head, "quot");
    if (b == 0) cljc_error("quot: division by zero");
    return mk_int(a / b);
}

/* ── strings ── */

static char *as_str(Cljc *v, const char *what) {
    if (v == NIL || v->tag != CLJC_STRING) cljc_error("%s: expected a string", what);
    return v->as.str;
}

static Cljc *prim_subs(CljcEnv *env, Cljc *args) {
    (void)env;
    char *s = as_str(args->as.cons.head, "subs");
    size_t len = strlen(s);
    int64_t start = as_int(args->as.cons.tail->as.cons.head, "subs");
    int64_t end = args->as.cons.tail->as.cons.tail != NIL
        ? as_int(args->as.cons.tail->as.cons.tail->as.cons.head, "subs") : (int64_t)len;
    if (start < 0 || end < start || (size_t)end > len)
        cljc_error("subs: index out of bounds");
    return mk_str(s + start, (size_t)(end - start));
}

#define STR_MAP_FN(NAME, XFORM) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc *args) { \
        (void)env; \
        char *s = as_str(args->as.cons.head, #NAME); \
        Cljc *r = mk_str(s, strlen(s)); \
        for (char *c = r->as.str; *c; c++) *c = (char)XFORM((unsigned char)*c); \
        return r; \
    }

STR_MAP_FN(upper_case, toupper)
STR_MAP_FN(lower_case, tolower)

static Cljc *prim_trim(CljcEnv *env, Cljc *args) {
    (void)env;
    char *s = as_str(args->as.cons.head, "trim");
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    return mk_str(s, n);
}

static Cljc *prim_split(CljcEnv *env, Cljc *args) {
    /* (str/split s sep) — sep is a plain string, not a regex (divergence). */
    (void)env;
    char *s = as_str(args->as.cons.head, "split");
    char *sep = as_str(args->as.cons.tail->as.cons.head, "split");
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
    Cljc *one = mk_cons(parts, NIL);
    return prim_vec(env, one);
}

#define STR_PRED(NAME, EXPR) \
    static Cljc *prim_##NAME(CljcEnv *env, Cljc *args) { \
        (void)env; \
        char *s = as_str(args->as.cons.head, #NAME); \
        char *sub = as_str(args->as.cons.tail->as.cons.head, #NAME); \
        size_t sl = strlen(s), bl = strlen(sub); \
        (void)sl; (void)bl; \
        return mk_bool(EXPR); \
    }

STR_PRED(starts_with, strncmp(s, sub, bl) == 0)
STR_PRED(ends_with,   bl <= sl && strcmp(s + sl - bl, sub) == 0)
STR_PRED(includes,    strstr(s, sub) != NULL)

static Cljc *prim_blank_p(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v == NIL) return TRUE;
    char *s = as_str(v, "blank?");
    for (; *s; s++) if (!isspace((unsigned char)*s)) return FALSE;
    return TRUE;
}

/* ── file IO ── */

static Cljc *prim_slurp(CljcEnv *env, Cljc *args) {
    (void)env;
    char *path = as_str(args->as.cons.head, "slurp");
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

static Cljc *prim_spit(CljcEnv *env, Cljc *args) {
    (void)env;
    char *path = as_str(args->as.cons.head, "spit");
    Cljc *content = args->as.cons.tail->as.cons.head;
    FILE *f = fopen(path, "wb");
    if (!f) cljc_error("spit: cannot open %s", path);
    SBuf sb = {0};
    print_to(&sb, content, false);
    if (sb.data) fwrite(sb.data, 1, sb.len, f);
    free(sb.data);
    fclose(f);
    return NIL;
}

/* ── printing variants ── */

static Cljc *print_args(Cljc *args, bool readably, bool newline) {
    SBuf sb = {0};
    bool first = true;
    for (Cljc *a = args; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        print_to(&sb, a->as.cons.head, readably);
    }
    if (newline) sb_putc(&sb, '\n');
    if (sb.data) { fwrite(sb.data, 1, sb.len, stdout); free(sb.data); }
    return NIL;
}

static Cljc *prim_pr(CljcEnv *env, Cljc *args)    { (void)env; return print_args(args, true, false); }
static Cljc *prim_prn(CljcEnv *env, Cljc *args)   { (void)env; return print_args(args, true, true); }
static Cljc *prim_print(CljcEnv *env, Cljc *args) { (void)env; return print_args(args, false, false); }

/* ───── Public C API ─────────────────────────────────────────────────── */

CljcEnv *cljc_new_env(void);
Cljc    *cljc_eval_string(CljcEnv *env, const char *src);
void     cljc_print(Cljc *v);
void     cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn);
void     cljc_set_stack_base(void *p);

void cljc_define_native(CljcEnv *env, const char *name, CljcNativeFn fn) {
    env_define(env_root(env), intern(name, strlen(name)), mk_native(fn));
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
    "(defn into [to from] (reduce conj to from))\n"
    "(defn mapv [f coll] (apply vector (map f coll)))\n"
    "(defn filterv [f coll] (apply vector (filter f coll)))\n"
    "(defn repeat [n x] (map (constantly x) (range n)))\n"
    "(defn nthrest [coll n] (drop n coll))\n"
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
    "(defn partition [n coll]\n"
    "  (loop [s (seq coll) acc (list)]\n"
    "    (let [chunk (take n s)]\n"
    "      (if (< (count chunk) n)\n"
    "        (reverse acc)\n"
    "        (recur (seq (drop n s)) (cons chunk acc))))))\n"
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
    "  ([f coll] (sort (fn [a b] (< (compare (f a) (f b)) 0)) coll)))\n"
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
    "(defmacro doseq [bindings & body]\n"
    "  `(loop [s# (seq ~(nth bindings 1))]\n"
    "     (when s#\n"
    "       (let [~(nth bindings 0) (first s#)] ~@body)\n"
    "       (recur (next s#)))))\n"
    "(defmacro while [test & body]\n"
    "  `(loop [] (when ~test ~@body (recur))))\n"
    "(defmacro case [e & clauses]\n"
    "  (let [g (gensym \"case\")]\n"
    "    (list 'let [g e]\n"
    "      (cons 'cond\n"
    "        (loop [cs clauses acc (list)]\n"
    "          (if (empty? cs)\n"
    "            (reverse acc)\n"
    "            (if (empty? (rest cs))\n"
    "              (reverse (cons (first cs) (cons :else acc)))\n"
    "              (recur (rest (rest cs))\n"
    "                     (cons (first (rest cs))\n"
    "                           (cons (list '= g (list 'quote (first cs))) acc))))))))))\n"
    "(defmacro for [bindings body]\n"
    "  (if (= 2 (count bindings))\n"
    "    `(map (fn [~(nth bindings 0)] ~body) ~(nth bindings 1))\n"
    "    `(mapcat (fn [~(nth bindings 0)] (for ~(vec (drop 2 bindings)) ~body))\n"
    "             ~(nth bindings 1))))\n"
    /* batch 5-lite */
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
    "(defn set/union [& sets] (reduce (fn [a s] (reduce conj a (seq s))) #{} sets))\n"
    "(defn set/intersection [s1 s2] (set (filter (fn [x] (contains? s2 x)) (seq s1))))\n"
    "(defn set/difference [s1 s2] (set (remove (fn [x] (contains? s2 x)) (seq s1))))\n"
    "(defmacro assert [x]\n"
    "  `(when-not ~x\n"
    "     (throw (ex-info (str \"Assert failed: \" (pr-str '~x)) {}))))\n"
    ;

CljcEnv *cljc_new_env(void) {
    if (!NIL) {
        gc_stress = getenv("CLJC_GC_STRESS") != NULL;
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
    cljc_define_native(e, "name",    prim_name);
    cljc_define_native(e, "keyword", prim_keyword);
    cljc_define_native(e, "symbol",  prim_symbol);
    cljc_define_native(e, "quot",    prim_quot);
    cljc_define_native(e, "subs",    prim_subs);
    cljc_define_native(e, "slurp",   prim_slurp);
    cljc_define_native(e, "spit",    prim_spit);
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
    cljc_define_native(e, "str/blank?",       prim_blank_p);
    cljc_eval_string(e, PRELUDE);
    return e;
}

Cljc *cljc_eval_string(CljcEnv *env, const char *src) {
    char stack_anchor;
    cljc_set_stack_base(&stack_anchor);  /* ensure at least this frame is scanned */
    Cljc * volatile result = NIL;  /* survives the error longjmp */
    if (setjmp(err_jmp) != 0) { print_error(); return NIL; }
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
static int run_stream(CljcEnv *env, FILE *f) {
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
        free(src);
        return 1;
    }
    const char *p = src;
    while (*p) {
        skip_ws(&p);
        if (!*p) break;
        Cljc *form = read_form(&p);
        if (!form) break;
        eval(env, form);
    }
    free(src);
    return 0;
}

/* Interactive REPL. Accumulates lines until parens balance, so multi-line
 * forms work. A separate function so its locals are below the GC stack base
 * recorded in main. */
static int run_repl(CljcEnv *env) {
    char buf[65536];
    size_t buflen = 0;
    fputs("cljc v0 — Ctrl-D to quit\n", stdout);
    for (;;) {
        fputs(buflen ? "  ... " : "cljc> ", stdout); fflush(stdout);
        if (!fgets(buf + buflen, (int)(sizeof buf - buflen), stdin)) { putchar('\n'); break; }
        buflen = strlen(buf);
        if (buflen >= sizeof buf - 2) {  /* full buffer would loop forever */
            fprintf(stderr, "error: input too long\n");
            buflen = 0;
            continue;
        }
        /* Balance check: count delimiters outside strings/comments. */
        int depth = 0; bool in_str = false, in_comment = false;
        for (const char *c = buf; *c; c++) {
            if (in_comment) { if (*c == '\n') in_comment = false; continue; }
            if (in_str) {
                if (*c == '\\' && c[1]) c++;
                else if (*c == '"') in_str = false;
                continue;
            }
            if (*c == '"') in_str = true;
            else if (*c == ';') in_comment = true;
            else if (*c == '(' || *c == '[' || *c == '{') depth++;
            else if (*c == ')' || *c == ']' || *c == '}') depth--;
        }
        if (depth > 0 || in_str) continue;  /* keep reading lines */

        if (setjmp(err_jmp) != 0) { print_error(); buflen = 0; continue; }
        const char *p = buf;
        while (*p) {
            skip_ws(&p);
            if (!*p) break;
            Cljc *form = read_form(&p);
            if (!form) break;
            Cljc *result = eval(env, form);
            print(result); putchar('\n');
        }
        buflen = 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    cljc_set_stack_base(&argc);  /* top-of-stack anchor for conservative GC */
    CljcEnv *env = cljc_new_env();

    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        int rc = run_stream(env, f);
        fclose(f);
        return rc;
    }
    if (!isatty(0)) return run_stream(env, stdin);
    return run_repl(env);
}
#endif
