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
        struct { Cljc **items; size_t len; size_t cap; } vec;
        struct { Cljc **keys; Cljc **vals; size_t len; } map;
        struct { Cljc *params; Cljc *body; CljcEnv *env; bool is_macro; } fn;
        CljcNativeFn native;
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
static Cljc *NIL, *TRUE, *FALSE;

/* ───── Error handling: longjmp out of the evaluator ─────────────────── */

static jmp_buf err_jmp;
static char err_msg[256];

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
static void cljc_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(err_msg, sizeof err_msg, fmt, ap);
    va_end(ap);
    longjmp(err_jmp, 1);
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
                for (size_t i = 0; i < v->as.vec.len; i++) gc_mark(v->as.vec.items[i]);
                return;
            case CLJC_MAP:
                for (size_t i = 0; i < v->as.map.len; i++) {
                    gc_mark(v->as.map.keys[i]);
                    gc_mark(v->as.map.vals[i]);
                }
                return;
            case CLJC_RECUR:
                for (size_t i = 0; i < v->as.recur.n; i++) gc_mark(v->as.recur.vals[i]);
                return;
            case CLJC_FN:
                gc_mark(v->as.fn.params);
                gc_mark_env(v->as.fn.env);
                v = v->as.fn.body;
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
                    case CLJC_VECTOR: free(c->as.vec.items); break;
                    case CLJC_MAP:    free(c->as.map.keys); free(c->as.map.vals); break;
                    case CLJC_RECUR:  free(c->as.recur.vals); break;
                    default: break;
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
    return !strchr("()[]{}\";'`,~", c);
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
        Cljc *v = alloc(CLJC_VECTOR);
        size_t n = list_len(list);
        v->as.vec.items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
        v->as.vec.len = n; v->as.vec.cap = n;
        size_t i = 0;
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
            v->as.vec.items[i++] = l->as.cons.head;
        return v;
    }
    if (c == '{') {
        Cljc *list = read_list(p, '}');
        size_t n = list_len(list);
        if (n % 2 != 0) cljc_error("map literal must contain an even number of forms");
        /* Store as alternating k/v in a CLJC_MAP; values are still unevaluated
         * forms here — eval() builds the live map. */
        Cljc *m = alloc(CLJC_MAP);
        m->as.map.keys = xmalloc(sizeof(Cljc *) * (n / 2 ? n / 2 : 1));
        m->as.map.vals = xmalloc(sizeof(Cljc *) * (n / 2 ? n / 2 : 1));
        m->as.map.len = n / 2;
        size_t i = 0;
        for (Cljc *l = list; l && l->tag == CLJC_LIST; l = l->as.cons.tail->as.cons.tail) {
            m->as.map.keys[i] = l->as.cons.head;
            m->as.map.vals[i] = l->as.cons.tail->as.cons.head;
            i++;
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

/* Bind params to args in `call`. Supports (fn [x y & rest] ...) — after '&',
 * one name collects remaining args as a list. */
static void bind_params(CljcEnv *call, Cljc *params, Cljc *args) {
    static const char *SYM_AMP;
    if (!SYM_AMP) SYM_AMP = intern("&", 1);
    Cljc *p = params, *a = args;
    while (p && p->tag == CLJC_LIST) {
        const char *name = p->as.cons.head->as.sym;
        if (name == SYM_AMP) {
            Cljc *rest_name = p->as.cons.tail->as.cons.head;
            env_define(call, rest_name->as.sym, a == NIL ? NIL : a);
            return;
        }
        if (a == NIL || a->tag != CLJC_LIST) cljc_error("not enough arguments");
        env_define(call, name, a->as.cons.head);
        p = p->as.cons.tail; a = a->as.cons.tail;
    }
    if (a != NIL) cljc_error("too many arguments");
}

static Cljc *apply(CljcEnv *env, Cljc *fn, Cljc *args) {
    if (fn->tag == CLJC_NATIVE) return fn->as.native(env, args);
    if (fn->tag == CLJC_FN) {
        for (;;) {
            CljcEnv *call = env_new(fn->as.fn.env);
            bind_params(call, fn->as.fn.params, args);
            Cljc *result = eval_body(call, fn->as.fn.body);
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
    /* Vectors as functions: ([10 20 30] 1) => 20. */
    if (fn->tag == CLJC_VECTOR) {
        Cljc *k = args->as.cons.head;
        if (k->tag != CLJC_INT) cljc_error("vector lookup needs an integer index");
        if (k->as.i < 0 || (size_t)k->as.i >= fn->as.vec.len)
            cljc_error("vector index out of bounds: %lld", (long long)k->as.i);
        return fn->as.vec.items[k->as.i];
    }
    cljc_error("not callable");
    return NIL;
}

/* Build an interpreted fn from a [params] vector and a body form-list. */
static Cljc *make_fn(CljcEnv *env, Cljc *params_vec, Cljc *body, bool is_macro) {
    static const char *SYM_AMP;
    if (!SYM_AMP) SYM_AMP = intern("&", 1);
    if (params_vec == NIL || params_vec->tag != CLJC_VECTOR)
        cljc_error("fn params must be a vector");
    Cljc *params = NIL, **t = &params;
    for (size_t i = 0; i < params_vec->as.vec.len; i++) {
        const char *name = sym_name(params_vec->as.vec.items[i], "fn params");
        /* '&' must be followed by exactly one rest-arg name. */
        if (name == SYM_AMP && i + 2 != params_vec->as.vec.len)
            cljc_error("fn params: & must be followed by exactly one symbol");
        *t = mk_cons(params_vec->as.vec.items[i], NIL);
        t = &(*t)->as.cons.tail;
    }
    Cljc *f = alloc(CLJC_FN);
    f->as.fn.params = params;
    f->as.fn.body = body;
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
        /* Incremental len: mid-expansion GC marks only the filled slots. */
        Cljc *v = alloc(CLJC_VECTOR);
        size_t n = form->as.vec.len;
        v->as.vec.items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
        v->as.vec.len = 0;
        v->as.vec.cap = n;
        for (size_t i = 0; i < n; i++) {
            v->as.vec.items[i] = qq_expand(env, form->as.vec.items[i]);
            v->as.vec.len = i + 1;
        }
        return v;
    }
    if (form->tag == CLJC_MAP) {
        Cljc *m = alloc(CLJC_MAP);
        size_t n = form->as.map.len;
        m->as.map.keys = xmalloc(sizeof(Cljc *) * (n ? n : 1));
        m->as.map.vals = xmalloc(sizeof(Cljc *) * (n ? n : 1));
        m->as.map.len = 0;
        for (size_t i = 0; i < n; i++) {
            Cljc *k = qq_expand(env, form->as.map.keys[i]);   /* stack-rooted while */
            Cljc *val = qq_expand(env, form->as.map.vals[i]); /* the pair completes */
            m->as.map.keys[i] = k;
            m->as.map.vals[i] = val;
            m->as.map.len = i + 1;
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
        case CLJC_RECUR:   /* not produced by the reader; appears only inside loop */
            return form;
        case CLJC_FREE:
            cljc_error("internal: evaluated a freed value (GC bug)");
        case CLJC_VECTOR: {
            /* Vector literals evaluate each element: [(+ 1 2)] => [3].
             * len grows as slots fill so a mid-eval GC marks exactly the
             * elements written so far. */
            Cljc *v = alloc(CLJC_VECTOR);
            size_t n = form->as.vec.len;
            v->as.vec.items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
            v->as.vec.len = 0;
            v->as.vec.cap = n;
            for (size_t i = 0; i < n; i++) {
                v->as.vec.items[i] = eval(env, form->as.vec.items[i]);
                v->as.vec.len = i + 1;
            }
            return v;
        }
        case CLJC_MAP: {
            /* Map literals evaluate keys and values; later duplicates win. */
            Cljc *m = alloc(CLJC_MAP);
            size_t n = form->as.map.len;
            m->as.map.keys = xmalloc(sizeof(Cljc *) * (n ? n : 1));
            m->as.map.vals = xmalloc(sizeof(Cljc *) * (n ? n : 1));
            m->as.map.len = 0;
            for (size_t i = 0; i < n; i++) {
                Cljc *k = eval(env, form->as.map.keys[i]);
                Cljc *val = eval(env, form->as.map.vals[i]);
                bool replaced = false;
                for (size_t j = 0; j < m->as.map.len; j++)
                    if (cljc_eq(m->as.map.keys[j], k)) { m->as.map.vals[j] = val; replaced = true; break; }
                if (!replaced) {
                    m->as.map.keys[m->as.map.len] = k;
                    m->as.map.vals[m->as.map.len] = val;
                    m->as.map.len++;
                }
            }
            return m;
        }
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
                                  *SYM_DEFMACRO, *SYM_QUASIQUOTE;
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
                }
                if (s == SYM_QUASIQUOTE) return qq_expand(env, rest->as.cons.head);
                if (s == SYM_DEFMACRO) {
                    /* (defmacro name [params] body...) — a fn flagged so that
                     * eval calls it on unevaluated forms and re-evals the result. */
                    need_args(rest, 2, "defmacro");
                    const char *name = sym_name(rest->as.cons.head, "defmacro");
                    Cljc *m = make_fn(env, rest->as.cons.tail->as.cons.head,
                                      rest->as.cons.tail->as.cons.tail, true);
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
                        binds_vec->as.vec.len % 2 != 0)
                        cljc_error("let needs an even-sized binding vector");
                    CljcEnv *scope = env_new(env);
                    for (size_t i = 0; i < binds_vec->as.vec.len; i += 2) {
                        const char *name = sym_name(binds_vec->as.vec.items[i], "let binding");
                        Cljc *val = eval(scope, binds_vec->as.vec.items[i + 1]);
                        env_define(scope, name, val);
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
                        binds_vec->as.vec.len % 2 != 0)
                        cljc_error("loop needs an even-sized binding vector");
                    size_t nparams = binds_vec->as.vec.len / 2;
                    const char **names = xmalloc(sizeof(char *) * (nparams ? nparams : 1));
                    CljcEnv *scope = env_new(env);
                    for (size_t i = 0; i < nparams; i++) {
                        names[i] = sym_name(binds_vec->as.vec.items[i * 2], "loop binding");
                        Cljc *val = eval(scope, binds_vec->as.vec.items[i * 2 + 1]);
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
                    /* (fn [x y] body...) — params as a vector, n-ary body. */
                    return make_fn(env, rest->as.cons.head, rest->as.cons.tail, false);
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
            for (size_t i = 0; i < v->as.vec.len; i++) {
                if (i) sb_putc(sb, ' ');
                print_to(sb, v->as.vec.items[i], readably);
            }
            sb_putc(sb, ']');
            break;
        }
        case CLJC_MAP: {
            sb_putc(sb, '{');
            for (size_t i = 0; i < v->as.map.len; i++) {
                if (i) sb_puts(sb, ", ");
                print_to(sb, v->as.map.keys[i], readably);
                sb_putc(sb, ' ');
                print_to(sb, v->as.map.vals[i], readably);
            }
            sb_putc(sb, '}');
            break;
        }
        case CLJC_FN:     sb_puts(sb, "#<fn>"); break;
        case CLJC_NATIVE: sb_puts(sb, "#<native>"); break;
        case CLJC_RECUR:  sb_puts(sb, "#<recur>"); break;
        case CLJC_FREE:   sb_puts(sb, "#<freed!>"); break;  /* seeing this is a GC bug */
    }
}

static void print(Cljc *v) {
    SBuf sb = {0};
    print_to(&sb, v, true);
    if (sb.data) { fwrite(sb.data, 1, sb.len, stdout); free(sb.data); }
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
        bool done_a = la ? (la == NIL) : (ia >= a->as.vec.len);
        bool done_b = lb ? (lb == NIL) : (ib >= b->as.vec.len);
        if (done_a || done_b) return done_a && done_b;
        Cljc *xa = la ? la->as.cons.head : a->as.vec.items[ia];
        Cljc *xb = lb ? lb->as.cons.head : b->as.vec.items[ib];
        if (!cljc_eq(xa, xb)) return false;
        if (la) la = la->as.cons.tail; else ia++;
        if (lb) lb = lb->as.cons.tail; else ib++;
    }
}

static bool map_find(Cljc *m, Cljc *key, Cljc **out) {
    for (size_t i = 0; i < m->as.map.len; i++) {
        if (cljc_eq(m->as.map.keys[i], key)) {
            if (out) *out = m->as.map.vals[i];
            return true;
        }
    }
    return false;
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
            if (a->as.map.len != b->as.map.len) return false;
            for (size_t i = 0; i < a->as.map.len; i++) {
                Cljc *bv;
                if (!map_find(b, a->as.map.keys[i], &bv)) return false;
                if (!cljc_eq(a->as.map.vals[i], bv)) return false;
            }
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
    if (v->tag == CLJC_VECTOR) return mk_int((int64_t)v->as.vec.len);
    if (v->tag == CLJC_MAP) return mk_int((int64_t)v->as.map.len);
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
        if (n >= 0 && (size_t)n < coll->as.vec.len) return coll->as.vec.items[n];
    } else if (coll && coll->tag == CLJC_LIST) {
        for (Cljc *l = coll; l && l->tag == CLJC_LIST; l = l->as.cons.tail)
            if (n-- == 0) return l->as.cons.head;
    }
    if (not_found) return not_found;
    cljc_error("nth: index out of bounds");
    return NIL;
}

static Cljc *mk_vector(Cljc **items, size_t n) {
    Cljc *v = alloc(CLJC_VECTOR);
    v->as.vec.items = xmalloc(sizeof(Cljc *) * (n ? n : 1));
    if (n) memcpy(v->as.vec.items, items, sizeof(Cljc *) * n);
    v->as.vec.len = n; v->as.vec.cap = n;
    return v;
}

static Cljc *prim_conj(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *r = args->as.cons.head;  /* nil works: conj onto nil yields a list */
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *x = a->as.cons.head;
        if (r == NIL || r->tag == CLJC_LIST) {
            r = mk_cons(x, r);                      /* lists grow at the front */
        } else if (r->tag == CLJC_VECTOR) {
            Cljc *nv = alloc(CLJC_VECTOR);          /* vectors grow at the back */
            size_t n = r->as.vec.len;
            nv->as.vec.items = xmalloc(sizeof(Cljc *) * (n + 1));
            memcpy(nv->as.vec.items, r->as.vec.items, sizeof(Cljc *) * n);
            nv->as.vec.items[n] = x;
            nv->as.vec.len = nv->as.vec.cap = n + 1;
            r = nv;
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
                for (size_t i = 0; i < last->as.vec.len; i++) {
                    *t = mk_cons(last->as.vec.items[i], NIL);
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
TYPE_PRED(list_p,    v != NIL && v->tag == CLJC_LIST)
TYPE_PRED(vector_p,  v != NIL && v->tag == CLJC_VECTOR)
TYPE_PRED(number_p,  v != NIL && (v->tag == CLJC_INT || v->tag == CLJC_DOUBLE))
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
    if (v->tag == CLJC_VECTOR) return mk_bool(v->as.vec.len == 0);
    if (v->tag == CLJC_MAP) return mk_bool(v->as.map.len == 0);
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

/* ── Map primitives (v0 assoc-array engine) ── */

static Cljc *mk_map(size_t cap) {
    Cljc *m = alloc(CLJC_MAP);
    m->as.map.keys = xmalloc(sizeof(Cljc *) * (cap ? cap : 1));
    m->as.map.vals = xmalloc(sizeof(Cljc *) * (cap ? cap : 1));
    m->as.map.len = 0;
    return m;
}

static Cljc *map_assoc(Cljc *m, Cljc *k, Cljc *v) {
    /* Copy-on-write: callers keep their original map unchanged. */
    Cljc *nm = mk_map(m->as.map.len + 1);
    bool replaced = false;
    for (size_t i = 0; i < m->as.map.len; i++) {
        nm->as.map.keys[i] = m->as.map.keys[i];
        if (cljc_eq(m->as.map.keys[i], k)) { nm->as.map.vals[i] = v; replaced = true; }
        else nm->as.map.vals[i] = m->as.map.vals[i];
    }
    nm->as.map.len = m->as.map.len;
    if (!replaced) {
        nm->as.map.keys[nm->as.map.len] = k;
        nm->as.map.vals[nm->as.map.len] = v;
        nm->as.map.len++;
    }
    return nm;
}

static Cljc *prim_hash_map(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = mk_map(list_len(args) / 2);
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
    } else if (coll != NIL && coll->tag == CLJC_VECTOR && k->tag == CLJC_INT) {
        if (k->as.i >= 0 && (size_t)k->as.i < coll->as.vec.len)
            return coll->as.vec.items[k->as.i];
    }
    return dflt;
}

static Cljc *prim_assoc(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *coll = args->as.cons.head;
    if (coll == NIL) coll = mk_map(0);
    Cljc *r = coll;
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail->as.cons.tail) {
        if (a->as.cons.tail == NIL) cljc_error("assoc needs key-value pairs");
        Cljc *k = a->as.cons.head, *v = a->as.cons.tail->as.cons.head;
        if (r->tag == CLJC_MAP) r = map_assoc(r, k, v);
        else if (r->tag == CLJC_VECTOR) {
            if (k->tag != CLJC_INT || k->as.i < 0 || (size_t)k->as.i > r->as.vec.len)
                cljc_error("assoc on vector: index out of bounds");
            size_t n = r->as.vec.len, idx = (size_t)k->as.i;
            Cljc *nv = alloc(CLJC_VECTOR);
            size_t newlen = idx == n ? n + 1 : n;  /* assoc at len appends */
            nv->as.vec.items = xmalloc(sizeof(Cljc *) * newlen);
            memcpy(nv->as.vec.items, r->as.vec.items, sizeof(Cljc *) * n);
            nv->as.vec.items[idx] = v;
            nv->as.vec.len = nv->as.vec.cap = newlen;
            r = nv;
        } else cljc_error("assoc: not associative");
    }
    return r;
}

static Cljc *prim_dissoc(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = args->as.cons.head;
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("dissoc: not a map");
    for (Cljc *a = args->as.cons.tail; a && a->tag == CLJC_LIST; a = a->as.cons.tail) {
        Cljc *k = a->as.cons.head;
        Cljc *nm = mk_map(m->as.map.len);
        for (size_t i = 0; i < m->as.map.len; i++) {
            if (cljc_eq(m->as.map.keys[i], k)) continue;
            nm->as.map.keys[nm->as.map.len] = m->as.map.keys[i];
            nm->as.map.vals[nm->as.map.len] = m->as.map.vals[i];
            nm->as.map.len++;
        }
        m = nm;
    }
    return m;
}

static Cljc *prim_keys(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *m = args->as.cons.head;
    if (m == NIL) return NIL;
    if (m->tag != CLJC_MAP) cljc_error("keys: not a map");
    Cljc *out = NIL, **t = &out;
    for (size_t i = 0; i < m->as.map.len; i++) {
        *t = mk_cons(m->as.map.keys[i], NIL);
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
    for (size_t i = 0; i < m->as.map.len; i++) {
        *t = mk_cons(m->as.map.vals[i], NIL);
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
    if (coll->tag == CLJC_VECTOR)  /* contains? checks INDEX presence on vectors */
        return mk_bool(k->tag == CLJC_INT && k->as.i >= 0 && (size_t)k->as.i < coll->as.vec.len);
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
        for (size_t i = 0; i < m->as.map.len; i++)
            r = map_assoc(r, m->as.map.keys[i], m->as.map.vals[i]);
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
    Cljc *v = args->as.cons.head;
    if (v == NIL) return NIL;
    if (v->tag == CLJC_LIST) return v->as.cons.head;
    if (v->tag == CLJC_VECTOR) return v->as.vec.len ? v->as.vec.items[0] : NIL;
    cljc_error("first: not a sequence");
    return NIL;
}

static Cljc *prim_rest(CljcEnv *env, Cljc *args) {
    (void)env;
    Cljc *v = args->as.cons.head;
    if (v == NIL) return NIL;
    if (v->tag == CLJC_LIST) return v->as.cons.tail;
    if (v->tag == CLJC_VECTOR) {
        /* Vectors seq into lists, matching Clojure's (rest [1 2 3]) => (2 3). */
        Cljc *out = NIL, **t = &out;
        for (size_t i = 1; i < v->as.vec.len; i++) {
            *t = mk_cons(v->as.vec.items[i], NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    cljc_error("rest: not a sequence");
    return NIL;
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
        for (size_t i = 0; i < v->as.vec.len; i++) {
            *t = mk_cons(v->as.vec.items[i], NIL);
            t = &(*t)->as.cons.tail;
        }
        return out;
    }
    if (v->tag == CLJC_MAP) {
        /* Maps seq into [k v] entry vectors. */
        Cljc *out = NIL, **t = &out;
        for (size_t i = 0; i < v->as.map.len; i++) {
            Cljc *entry_items[2] = { v->as.map.keys[i], v->as.map.vals[i] };
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

static Cljc *prim_gc(CljcEnv *env, Cljc *args) {
    (void)env; (void)args;
    gc_collect();
    return mk_int((int64_t)gc_freed_last);  /* cells freed by this collection */
}

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
    cljc_define_native(e, "nil?",    prim_nil_p);
    cljc_define_native(e, "list?",   prim_list_p);
    cljc_define_native(e, "vector?", prim_vector_p);
    cljc_define_native(e, "number?", prim_number_p);
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
    cljc_eval_string(e, PRELUDE);
    return e;
}

Cljc *cljc_eval_string(CljcEnv *env, const char *src) {
    char stack_anchor;
    cljc_set_stack_base(&stack_anchor);  /* ensure at least this frame is scanned */
    Cljc * volatile result = NIL;  /* survives the error longjmp */
    if (setjmp(err_jmp) != 0) { fprintf(stderr, "error: %s\n", err_msg); return NIL; }
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
        fprintf(stderr, "error: %s\n", err_msg);
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

        if (setjmp(err_jmp) != 0) { fprintf(stderr, "error: %s\n", err_msg); buflen = 0; continue; }
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
