// The part of bionic's libc / libm export list that kl_bionic_extra.c and kl_bionic_extra2.c
// still left unserved (docs/android-ndk-inventario.md, "missing"), written under the same three
// rules: forward what Darwin does identically, translate what differs (struct layouts, flag
// numbers, 128-bit long double), and answer honestly for what an app cannot have.
//
// This file is the portable half: everything here runs on a Linux host too, so the tests in
// libc/test/t_bionic_extra3.c exercise the real code (cc -DKL_EXTRA3_TEST ... -lm). The
// Darwin-only half (kqueue-backed timerfd/signalfd/inotify, termios, sendmmsg...) is
// kl_bionic_extra4.c, whose rows are in the same table below.
//
// Names bionic does not export are NOT served, so a guest that imports them fails by name as it
// would on Android: pthread_cancel / pthread_setcancelstate / pthread_testcancel (bionic has no
// thread cancellation), wordexp, strfmon, the <fts.h> family is the exception that is simply
// not done (see bitacora.md, Paso 3 Android completo).
#define _GNU_SOURCE 1
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <complex.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <regex.h>
#include <wchar.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include "kl_ld128.h"

void *kl_shim_lookup(const char *name);                 // kl_shim.c
#ifdef KL_EXTRA3_TEST
#define XE3(guest, sym)
#define DECL3(sym)
#else
#include "klepton.h"
#define XE3(guest, sym) { guest, (void *)sym },
#define DECL3(sym) extern void sym(void);
#endif

typedef struct { const char *name; void *fn; } kl3_entry;
#define X3(n) { #n, (void *)(n) }
#define K3(n) { #n, (void *)kl3_##n }

#define LX_ENOSYS 38
#define LX_ERANGE 34
#define LX_EINVAL 22
#define LX_ENOENT 2

// One lookup of another shim entry, cached by the caller (the table is searched linearly).
#define LAZY(var, name) do { if (!(var)) (var) = kl_shim_lookup(name); } while (0)

// h_errno of the calling thread lives in kl_bionic_extra.c (__get_h_errno).
void kl_h_errno_set(int e);

// ======================================================================
// POSIX regex: bionic's regex is OpenBSD's (Henry Spencer), whose regex_t, regmatch_t (64-bit
// regoff_t), REG_* error codes and the flags below are the BSD set Darwin uses. Darwin adds
// REG_ENHANCED (0400) and REG_LARGE/REG_BACKR in the eflags; a bionic flag word never carries
// them, so it is masked to the shared bits instead of letting a stray bit change the dialect.
// ======================================================================
static int kl3_regcomp(regex_t *re, const char *pat, int cflags) { return regcomp(re, pat, cflags & 0x7F & ~0x40); }
static int kl3_regexec(const regex_t *re, const char *s, size_t n, regmatch_t *m, int eflags) {
    return regexec(re, s, n, m, eflags & 7);
}
static size_t kl3_regerror(int code, const regex_t *re, char *buf, size_t n) { return regerror(code, re, buf, n); }
static void kl3_regfree(regex_t *re) { regfree(re); }

// ======================================================================
// <search.h>: tsearch & co. An AVL tree of {key, left, right, height}; the node's first member is
// the key pointer, which is the contract tsearch's return value has (*(void **)ret == key).
// ======================================================================
typedef struct tnode { const void *key; struct tnode *l, *r; int h; } tnode;
typedef int (*tcmp_t)(const void *, const void *);
static int t_h(const tnode *n) { return n ? n->h : 0; }
static void t_fix(tnode *n) { int a = t_h(n->l), b = t_h(n->r); n->h = 1 + (a > b ? a : b); }
static tnode *t_rotr(tnode *y) { tnode *x = y->l; y->l = x->r; x->r = y; t_fix(y); t_fix(x); return x; }
static tnode *t_rotl(tnode *x) { tnode *y = x->r; x->r = y->l; y->l = x; t_fix(x); t_fix(y); return y; }
static tnode *t_bal(tnode *n) {
    t_fix(n);
    int b = t_h(n->l) - t_h(n->r);
    if (b > 1) { if (t_h(n->l->l) < t_h(n->l->r)) n->l = t_rotl(n->l); return t_rotr(n); }
    if (b < -1) { if (t_h(n->r->r) < t_h(n->r->l)) n->r = t_rotr(n->r); return t_rotl(n); }
    return n;
}
static tnode *t_ins(tnode *n, const void *key, tcmp_t cmp, tnode **at, int *oom) {
    if (!n) {
        tnode *m = calloc(1, sizeof *m);
        if (!m) { *oom = 1; return NULL; }
        m->key = key; m->h = 1; *at = m;
        return m;
    }
    int c = cmp(key, n->key);
    if (c == 0) { *at = n; return n; }
    if (c < 0) n->l = t_ins(n->l, key, cmp, at, oom); else n->r = t_ins(n->r, key, cmp, at, oom);
    return *oom ? n : t_bal(n);
}
static void *kl3_tsearch(const void *key, void **rootp, tcmp_t cmp) {
    if (!rootp) return NULL;
    tnode *at = NULL; int oom = 0;
    *rootp = t_ins(*rootp, key, cmp, &at, &oom);
    return oom ? NULL : at;
}
static void *kl3_tfind(const void *key, void *const *rootp, tcmp_t cmp) {
    if (!rootp) return NULL;
    for (tnode *n = *rootp; n; ) {
        int c = cmp(key, n->key);
        if (c == 0) return n;
        n = c < 0 ? n->l : n->r;
    }
    return NULL;
}
static tnode *t_popmin(tnode *n, tnode **min) {
    if (!n->l) { *min = n; return n->r; }
    n->l = t_popmin(n->l, min);
    return t_bal(n);
}
static tnode *t_del(tnode *n, tnode *par, const void *key, tcmp_t cmp, tnode **parent_out, int *found) {
    if (!n) return NULL;
    int c = cmp(key, n->key);
    if (c < 0) n->l = t_del(n->l, n, key, cmp, parent_out, found);
    else if (c > 0) n->r = t_del(n->r, n, key, cmp, parent_out, found);
    else {
        *found = 1; *parent_out = par;
        tnode *repl;
        if (!n->l) repl = n->r;
        else if (!n->r) repl = n->l;
        else { tnode *m; tnode *rr = t_popmin(n->r, &m); m->l = n->l; m->r = rr; repl = t_bal(m); }
        free(n);
        return repl;
    }
    return t_bal(n);
}
static char t_dummy;
// Returns the parent of the deleted node. A deleted root has no parent: the new root is returned,
// or (for the last node) a non-NULL placeholder, so "!= NULL" still means "it was there".
static void *kl3_tdelete(const void *key, void **rootp, tcmp_t cmp) {
    if (!rootp || !*rootp) return NULL;
    tnode *par = NULL; int found = 0;
    *rootp = t_del(*rootp, NULL, key, cmp, &par, &found);
    if (!found) return NULL;
    if (par) return par;
    return *rootp ? *rootp : (void *)&t_dummy;
}
// VISIT: preorder 0, postorder 1, endorder 2, leaf 3 (the same numbers on Darwin).
typedef void (*twalk_fn)(const void *, int, int);
static void t_walk(const tnode *n, twalk_fn f, int depth) {
    if (!n) return;
    if (!n->l && !n->r) { f(n, 3, depth); return; }
    f(n, 0, depth); t_walk(n->l, f, depth + 1);
    f(n, 1, depth); t_walk(n->r, f, depth + 1);
    f(n, 2, depth);
}
static void kl3_twalk(const void *root, twalk_fn f) { if (root && f) t_walk(root, f, 0); }
static void t_destroy(tnode *n, void (*fr)(void *)) {
    if (!n) return;
    t_destroy(n->l, fr); t_destroy(n->r, fr);
    if (fr) fr((void *)n->key);
    free(n);
}
static void kl3_tdestroy(void *root, void (*fr)(void *)) { t_destroy(root, fr); }

// lsearch / lfind / insque / remque
static void *kl3_lfind(const void *key, const void *base, size_t *nelp, size_t width, tcmp_t cmp) {
    const char *p = base;
    for (size_t i = 0; i < *nelp; i++, p += width) if (cmp(key, p) == 0) return (void *)p;
    return NULL;
}
static void *kl3_lsearch(const void *key, void *base, size_t *nelp, size_t width, tcmp_t cmp) {
    void *f = kl3_lfind(key, base, nelp, width, cmp);
    if (f) return f;
    void *dst = (char *)base + *nelp * width;
    memcpy(dst, key, width);
    (*nelp)++;
    return dst;
}
struct kl_qelem { struct kl_qelem *next, *prev; };
static void kl3_insque(void *e, void *p) {
    struct kl_qelem *el = e, *pred = p;
    if (!pred) { el->next = el->prev = NULL; return; }
    el->next = pred->next; el->prev = pred;
    if (pred->next) pred->next->prev = el;
    pred->next = el;
}
static void kl3_remque(void *e) {
    struct kl_qelem *el = e;
    if (el->next) el->next->prev = el->prev;
    if (el->prev) el->prev->next = el->next;
}

// hsearch: open addressing with linear probing over a power-of-two table (at least 2x the
// requested size, so ENTER stays cheap); keys are NOT copied, as the interface says.
typedef struct { char *key; void *data; } hs_entry;
typedef struct { hs_entry *tab; size_t cap, used; } hs_table;
struct kl_hsdata { hs_table *tbl; };
static size_t hs_hash(const char *s) { size_t h = 1469598103934665603ull; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; } return h; }
static int hs_create(hs_table **out, size_t nel) {
    if (*out) return 0;
    size_t cap = 16;
    while (cap < nel * 2 && cap < ((size_t)1 << 40)) cap <<= 1;
    hs_table *t = calloc(1, sizeof *t);
    if (!t) { errno = ENOMEM; return 0; }
    t->tab = calloc(cap, sizeof *t->tab);
    if (!t->tab) { free(t); errno = ENOMEM; return 0; }
    t->cap = cap;
    *out = t;
    return 1;
}
static void hs_destroy(hs_table **t) { if (*t) { free((*t)->tab); free(*t); *t = NULL; } }
static int hs_search(hs_table *t, hs_entry item, int enter, hs_entry **ret) {
    if (!t || !item.key) { errno = ESRCH; return 0; }
    size_t i = hs_hash(item.key) & (t->cap - 1);
    for (size_t n = 0; n < t->cap; n++, i = (i + 1) & (t->cap - 1)) {
        hs_entry *e = &t->tab[i];
        if (!e->key) {
            if (!enter) { errno = ESRCH; return 0; }
            if (t->used + 1 >= t->cap) { errno = ENOMEM; return 0; }
            *e = item; t->used++; *ret = e;
            return 1;
        }
        if (!strcmp(e->key, item.key)) { *ret = e; return 1; }
    }
    errno = enter ? ENOMEM : ESRCH;
    return 0;
}
static hs_table *g_hs;
static pthread_mutex_t g_hs_mu = PTHREAD_MUTEX_INITIALIZER;
static int kl3_hcreate(size_t nel) { pthread_mutex_lock(&g_hs_mu); int r = hs_create(&g_hs, nel); pthread_mutex_unlock(&g_hs_mu); return r; }
static void kl3_hdestroy(void) { pthread_mutex_lock(&g_hs_mu); hs_destroy(&g_hs); pthread_mutex_unlock(&g_hs_mu); }
static hs_entry *kl3_hsearch(hs_entry item, int action) {   // ACTION: FIND 0, ENTER 1
    hs_entry *r = NULL;
    pthread_mutex_lock(&g_hs_mu);
    int ok = hs_search(g_hs, item, action == 1, &r);
    pthread_mutex_unlock(&g_hs_mu);
    return ok ? r : NULL;
}
static int kl3_hcreate_r(size_t nel, struct kl_hsdata *h) { return h ? hs_create(&h->tbl, nel) : 0; }
static void kl3_hdestroy_r(struct kl_hsdata *h) { if (h) hs_destroy(&h->tbl); }
static int kl3_hsearch_r(hs_entry item, int action, hs_entry **ret, struct kl_hsdata *h) {
    hs_entry *r = NULL;
    if (!h) { errno = EINVAL; return 0; }
    int ok = hs_search(h->tbl, item, action == 1, &r);
    if (ok && ret) *ret = r;
    return ok;
}

// ======================================================================
// drand48 family, with POSIX's exact recurrence X(n+1) = (a*X(n) + c) mod 2^48, a = 0x5DEECE66D,
// c = 0xB. One shared state serves srand48/lrand48/mrand48/drand48 (Darwin's lrand48 would be a
// second, unrelated stream: srand48(s) followed by drand48() must continue the same sequence).
// ======================================================================
#define R48_MASK 0xFFFFFFFFFFFFull
static uint64_t g_r48_x = 0x1234ABCD330Eull, g_r48_a = 0x5DEECE66Dull, g_r48_c = 0xB;
static pthread_mutex_t g_r48_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t r48_step_global(void) {
    pthread_mutex_lock(&g_r48_mu);
    g_r48_x = (g_r48_a * g_r48_x + g_r48_c) & R48_MASK;
    uint64_t x = g_r48_x;
    pthread_mutex_unlock(&g_r48_mu);
    return x;
}
static uint64_t r48_step_user(unsigned short xs[3]) {
    uint64_t x = (uint64_t)xs[0] | ((uint64_t)xs[1] << 16) | ((uint64_t)xs[2] << 32);
    pthread_mutex_lock(&g_r48_mu);
    uint64_t a = g_r48_a, c = g_r48_c;
    pthread_mutex_unlock(&g_r48_mu);
    x = (a * x + c) & R48_MASK;
    xs[0] = (unsigned short)x; xs[1] = (unsigned short)(x >> 16); xs[2] = (unsigned short)(x >> 32);
    return x;
}
static double r48_d(uint64_t x) { return (double)x / 281474976710656.0; }      // x / 2^48, exact
static void kl3_srand48(long seed) {
    pthread_mutex_lock(&g_r48_mu);
    g_r48_x = (((uint64_t)seed & 0xFFFFFFFFull) << 16) | 0x330E;
    g_r48_a = 0x5DEECE66Dull; g_r48_c = 0xB;
    pthread_mutex_unlock(&g_r48_mu);
}
static unsigned short *kl3_seed48(unsigned short s[3]) {
    static unsigned short old[3];
    pthread_mutex_lock(&g_r48_mu);
    old[0] = (unsigned short)g_r48_x; old[1] = (unsigned short)(g_r48_x >> 16); old[2] = (unsigned short)(g_r48_x >> 32);
    g_r48_x = (uint64_t)s[0] | ((uint64_t)s[1] << 16) | ((uint64_t)s[2] << 32);
    g_r48_a = 0x5DEECE66Dull; g_r48_c = 0xB;
    pthread_mutex_unlock(&g_r48_mu);
    return old;
}
static void kl3_lcong48(unsigned short p[7]) {
    pthread_mutex_lock(&g_r48_mu);
    g_r48_x = (uint64_t)p[0] | ((uint64_t)p[1] << 16) | ((uint64_t)p[2] << 32);
    g_r48_a = (uint64_t)p[3] | ((uint64_t)p[4] << 16) | ((uint64_t)p[5] << 32);
    g_r48_c = p[6];
    pthread_mutex_unlock(&g_r48_mu);
}
static double kl3_drand48(void) { return r48_d(r48_step_global()); }
static long kl3_lrand48(void) { return (long)(r48_step_global() >> 17); }
static long kl3_mrand48(void) { return (long)(int32_t)(r48_step_global() >> 16); }
static double kl3_erand48(unsigned short xs[3]) { return r48_d(r48_step_user(xs)); }
static long kl3_nrand48(unsigned short xs[3]) { return (long)(r48_step_user(xs) >> 17); }
static long kl3_jrand48(unsigned short xs[3]) { return (long)(int32_t)(r48_step_user(xs) >> 16); }
// OpenBSD's rand_r (bionic's), not the Park-Miller one FreeBSD/Darwin ship.
static int kl3_rand_r(unsigned *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return (int)(*seed % 0x80000000u);
}

// ======================================================================
// getopt / getopt_long / getopt_long_only and their globals. The globals have to be OURS: a
// guest binds `optind` & co. to the address we hand out, and Darwin's getopt would update
// Darwin's copies. Permutes like bionic (GNU order) unless the optstring starts with '+' or
// POSIXLY_CORRECT is set; a leading '-' returns non-options as option 1.
// ======================================================================
struct kl3_option { const char *name; int has_arg; int *flag; int val; };
char *kl3_optarg;
int kl3_optind = 1, kl3_opterr = 1, kl3_optopt = '?', kl3_optreset;
static char *g_nextc;
static int g_first_nonopt = 1, g_last_nonopt = 1;
static int opt_isopt(const char *a) { return a && a[0] == '-' && a[1]; }
static void opt_reverse(char **v, int a, int b) { for (b--; a < b; a++, b--) { char *t = v[a]; v[a] = v[b]; v[b] = t; } }
// Swap the blocks [bottom,middle) and [middle,top).
static void opt_exchange(char **v, int bottom, int middle, int top) {
    opt_reverse(v, bottom, middle); opt_reverse(v, middle, top); opt_reverse(v, bottom, top);
    g_first_nonopt += kl3_optind - g_last_nonopt;     // the non-options now sit just before optind
    g_last_nonopt = kl3_optind;
}
static void opt_err(const char *prog, const char *fmt, const char *a) {
    if (!kl3_opterr) return;
    fprintf(stderr, "%s: ", prog ? prog : "?");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}
static int getopt_core(int argc, char *const argvc[], const char *optstring,
                       const struct kl3_option *lo, int *lidx, int long_only) {
    char **argv = (char **)argvc;
    if (!optstring) return -1;
    int posix = getenv("POSIXLY_CORRECT") != NULL, inorder = 0;
    if (*optstring == '+') { posix = 1; optstring++; } else if (*optstring == '-') { inorder = 1; optstring++; }
    int silent = (*optstring == ':');
    if (silent) optstring++;
    const char *prog = argc > 0 ? argv[0] : "?";
    kl3_optarg = NULL;
    if (kl3_optreset || kl3_optind == 0) { kl3_optreset = 0; kl3_optind = 1; g_nextc = NULL; g_first_nonopt = g_last_nonopt = 1; }
    int mode = 0;                    // 0 short option, 1 "--name", 2 "-name" in getopt_long_only
    if (!g_nextc || !*g_nextc) {
        g_nextc = NULL;
        if (g_last_nonopt > kl3_optind) g_last_nonopt = kl3_optind;
        if (g_first_nonopt > kl3_optind) g_first_nonopt = kl3_optind;
        if (!posix && !inorder) {
            if (g_first_nonopt != g_last_nonopt && g_last_nonopt != kl3_optind) opt_exchange(argv, g_first_nonopt, g_last_nonopt, kl3_optind);
            else if (g_last_nonopt != kl3_optind) g_first_nonopt = kl3_optind;
            while (kl3_optind < argc && !opt_isopt(argv[kl3_optind])) kl3_optind++;
            g_last_nonopt = kl3_optind;
        }
        if (kl3_optind != argc && !strcmp(argv[kl3_optind], "--")) {
            kl3_optind++;
            if (g_first_nonopt != g_last_nonopt && g_last_nonopt != kl3_optind) opt_exchange(argv, g_first_nonopt, g_last_nonopt, kl3_optind);
            else if (g_first_nonopt == g_last_nonopt) g_first_nonopt = kl3_optind;
            g_last_nonopt = argc; kl3_optind = argc;
        }
        if (kl3_optind == argc) {
            if (g_first_nonopt != g_last_nonopt) kl3_optind = g_first_nonopt;
            return -1;
        }
        if (!opt_isopt(argv[kl3_optind])) {
            if (posix) return -1;
            kl3_optarg = argv[kl3_optind++];           // '-' prefix: a non-option is "option 1"
            return 1;
        }
        const char *a = argv[kl3_optind];
        if (lo && a[1] == '-') { mode = 1; g_nextc = (char *)a + 2; }
        else if (lo && long_only) { mode = 2; g_nextc = (char *)a + 1; }
        else g_nextc = (char *)a + 1;
    }
    if (mode) {
        const char *name = g_nextc, *eq = strchr(name, '=');
        size_t nl = eq ? (size_t)(eq - name) : strlen(name);
        int match = -1, ambiguous = 0;
        for (int i = 0; lo[i].name; i++) {
            if (strncmp(lo[i].name, name, nl)) continue;
            if (strlen(lo[i].name) == nl) { match = i; ambiguous = 0; break; }
            if (match >= 0 && (lo[match].has_arg != lo[i].has_arg || lo[match].flag != lo[i].flag || lo[match].val != lo[i].val)) ambiguous = 1;
            if (match < 0) match = i;
        }
        // "-x" with no long name of that spelling falls back to the short option x
        if (match >= 0 || mode == 1) {
            const char *whole = argv[kl3_optind];
            g_nextc = NULL; kl3_optind++;
            if (ambiguous) { opt_err(prog, "option '%s' is ambiguous", whole); kl3_optopt = 0; return '?'; }
            if (match < 0) { opt_err(prog, "unrecognized option '%s'", whole); kl3_optopt = 0; return '?'; }
            const struct kl3_option *o = &lo[match];
            if (eq) {
                if (o->has_arg) kl3_optarg = (char *)eq + 1;
                else { opt_err(prog, "option '--%s' doesn't allow an argument", o->name); kl3_optopt = o->flag ? 0 : o->val; return '?'; }
            } else if (o->has_arg == 1) {
                if (kl3_optind < argc) kl3_optarg = argv[kl3_optind++];
                else { opt_err(prog, "option '--%s' requires an argument", o->name); kl3_optopt = o->flag ? 0 : o->val; return silent ? ':' : '?'; }
            }
            if (lidx) *lidx = match;
            if (o->flag) { *o->flag = o->val; return 0; }
            return o->val;
        }
    }
    // short option
    int c = (unsigned char)*g_nextc++;
    const char *p = (c == ':' || c == ';') ? NULL : strchr(optstring, c);
    if (!*g_nextc) kl3_optind++;
    if (!p) {
        kl3_optopt = c;
        opt_err(prog, "invalid option -- '%s'", (char[]){ (char)c, 0 });
        if (!*g_nextc) g_nextc = NULL;
        return '?';
    }
    if (p[1] == ':') {
        if (p[2] == ':') {                              // optional argument: only if attached
            if (*g_nextc) { kl3_optarg = g_nextc; kl3_optind++; }
            g_nextc = NULL;
        } else if (*g_nextc) {
            kl3_optarg = g_nextc; kl3_optind++; g_nextc = NULL;
        } else {
            g_nextc = NULL;
            if (kl3_optind >= argc) {
                kl3_optopt = c;
                opt_err(prog, "option requires an argument -- '%s'", (char[]){ (char)c, 0 });
                return silent ? ':' : '?';
            }
            kl3_optarg = argv[kl3_optind++];
        }
    }
    if (g_nextc && !*g_nextc) g_nextc = NULL;
    return c;
}
static int kl3_getopt(int argc, char *const argv[], const char *optstring) { return getopt_core(argc, argv, optstring, NULL, NULL, 0); }
static int kl3_getopt_long(int argc, char *const argv[], const char *s, const struct kl3_option *lo, int *idx) {
    return getopt_core(argc, argv, s, lo, idx, 0);
}
static int kl3_getopt_long_only(int argc, char *const argv[], const char *s, const struct kl3_option *lo, int *idx) {
    return getopt_core(argc, argv, s, lo, idx, 1);
}

// ======================================================================
// iconv, over a code-point pivot. Encodings: UTF-8, UTF-16 (BOM-detecting on input, LE with BOM
// on output), UTF-16LE/BE, UCS-2LE/BE, UTF-32 / UTF-32LE/BE, UCS-4, WCHAR_T (UTF-32LE, which is
// what bionic's wchar_t is), US-ASCII and ISO-8859-1. That is bionic's own set. "//TRANSLIT"
// replaces a character the target cannot hold with '?', "//IGNORE" drops it; otherwise EILSEQ.
// ======================================================================
enum { E_UTF8, E_UTF16, E_UTF16LE, E_UTF16BE, E_UTF32, E_UTF32LE, E_UTF32BE, E_ASCII, E_LATIN1 };
typedef struct { int from, to, translit, ignore, in_bom_done, in_be, out_bom_done; } kl_iconv;
static int enc_id(const char *name, int *translit, int *ignore) {
    char n[48]; size_t k = 0;
    for (const char *p = name; *p && k < sizeof n - 1; p++) {
        if (p[0] == '/' && p[1] == '/') {
            if (strcasestr(p, "TRANSLIT")) *translit = 1;
            if (strcasestr(p, "IGNORE")) *ignore = 1;
            break;
        }
        if (*p == '-' || *p == '_' || *p == ' ') continue;
        n[k++] = (char)toupper((unsigned char)*p);
    }
    n[k] = 0;
    static const struct { const char *n; int id; } t[] = {
        {"UTF8", E_UTF8}, {"UTF16", E_UTF16}, {"UTF16LE", E_UTF16LE}, {"UTF16BE", E_UTF16BE},
        {"UCS2", E_UTF16}, {"UCS2LE", E_UTF16LE}, {"UCS2BE", E_UTF16BE},
        {"UTF32", E_UTF32}, {"UTF32LE", E_UTF32LE}, {"UTF32BE", E_UTF32BE},
        {"UCS4", E_UTF32BE}, {"UCS4LE", E_UTF32LE}, {"UCS4BE", E_UTF32BE}, {"WCHART", E_UTF32LE},
        {"ASCII", E_ASCII}, {"USASCII", E_ASCII}, {"ANSIX3.41968", E_ASCII},
        {"ISO88591", E_LATIN1}, {"LATIN1", E_LATIN1}, {"L1", E_LATIN1}, {"ISO885911987", E_LATIN1},
        {"", -2},
    };
    for (size_t i = 0; t[i].id != -2; i++) if (!strcmp(t[i].n, n)) return t[i].id;
    return -1;
}
// bionic returns (iconv_t)-1 with EINVAL when either name is unknown.
static void *kl3_iconv_open(const char *to, const char *from) {
    kl_iconv *cd = calloc(1, sizeof *cd);
    if (!cd) return (void *)-1;
    int tt = 0, ti = 0, ft = 0, fi = 0;
    cd->to = enc_id(to ? to : "", &tt, &ti);
    cd->from = enc_id(from ? from : "", &ft, &fi);
    cd->translit = tt; cd->ignore = ti;
    if (cd->to < 0 || cd->from < 0) { free(cd); errno = EINVAL; return (void *)-1; }
    return cd;
}
static int kl3_iconv_close(void *cd) { if (cd && cd != (void *)-1) free(cd); return 0; }
// Decodes one code point at s[0..n). Returns bytes used, 0 if input is truncated (-> EINVAL),
// -1 if invalid (-> EILSEQ).
static int ic_decode(kl_iconv *c, const unsigned char *s, size_t n, uint32_t *cp, int *skip) {
    *skip = 1;
    if (n == 0) return 0;
    switch (c->from) {
    case E_ASCII: if (s[0] > 0x7F) return -1; *cp = s[0]; return 1;
    case E_LATIN1: *cp = s[0]; return 1;
    case E_UTF8: {
        unsigned l = s[0], len;
        if (l < 0x80) { *cp = l; return 1; }
        if (l >= 0xC2 && l <= 0xDF) len = 2; else if (l >= 0xE0 && l <= 0xEF) len = 3;
        else if (l >= 0xF0 && l <= 0xF4) len = 4; else return -1;
        uint32_t v = len == 2 ? (l & 0x1F) : len == 3 ? (l & 0x0F) : (l & 0x07);
        for (unsigned i = 1; i < len; i++) {
            if (i >= n) return 0;
            if ((s[i] & 0xC0) != 0x80) { *skip = (int)i; return -1; }
            if (i == 1 && ((l == 0xE0 && s[1] < 0xA0) || (l == 0xED && s[1] > 0x9F) ||
                           (l == 0xF0 && s[1] < 0x90) || (l == 0xF4 && s[1] > 0x8F))) return -1;
            v = (v << 6) | (s[i] & 0x3F);
        }
        *cp = v; return (int)len;
    }
    case E_UTF16: case E_UTF16LE: case E_UTF16BE: {
        int be = c->from == E_UTF16BE || (c->from == E_UTF16 && c->in_be);
        if (n < 2) return 0;
        uint32_t u = be ? (s[0] << 8 | s[1]) : (s[1] << 8 | s[0]);
        if (c->from == E_UTF16 && !c->in_bom_done) {
            c->in_bom_done = 1;
            if (u == 0xFEFF) { *cp = 0xFEFF; c->in_be = 0; return -2; }                 // BOM, consumed silently
            if (u == 0xFFFE) { c->in_be = 1; return -2; }
        }
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (n < 4) return 0;
            uint32_t w = be ? (s[2] << 8 | s[3]) : (s[3] << 8 | s[2]);
            if (w < 0xDC00 || w > 0xDFFF) { *skip = 2; return -1; }
            *cp = 0x10000 + ((u - 0xD800) << 10) + (w - 0xDC00); return 4;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) { *skip = 2; return -1; }
        *cp = u; return 2;
    }
    default: {                                                                          // UTF-32 flavours
        int be = c->from == E_UTF32BE || (c->from == E_UTF32 && c->in_be);
        if (n < 4) return 0;
        uint32_t u = be ? ((uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3])
                        : ((uint32_t)s[3] << 24 | s[2] << 16 | s[1] << 8 | s[0]);
        if (c->from == E_UTF32 && !c->in_bom_done) {
            c->in_bom_done = 1;
            if (u == 0xFEFF) return -2;
            if (u == 0xFFFE0000u) { c->in_be = 1; return -2; }
        }
        if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) { *skip = 4; return -1; }
        *cp = u; return 4;
    }
    }
}
// Encodes into out[8]; returns the length, 0 when the character cannot be represented.
static int ic_encode(kl_iconv *c, uint32_t cp, unsigned char *o) {
    int k = 0;
    switch (c->to) {
    case E_ASCII: if (cp > 0x7F) return 0; o[0] = (unsigned char)cp; return 1;
    case E_LATIN1: if (cp > 0xFF) return 0; o[0] = (unsigned char)cp; return 1;
    case E_UTF8:
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
        if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
        if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
        if (cp <= 0x10FFFF) { o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 0x3F); o[2] = 0x80 | (cp >> 6 & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4; }
        return 0;
    case E_UTF16: case E_UTF16LE: case E_UTF16BE: {
        int be = c->to == E_UTF16BE;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        uint16_t u[3]; int nu = 0;
        if (c->to == E_UTF16 && !c->out_bom_done) { u[nu++] = 0xFEFF; c->out_bom_done = 1; }
        if (cp >= 0x10000) { cp -= 0x10000; u[nu++] = (uint16_t)(0xD800 + (cp >> 10)); u[nu++] = (uint16_t)(0xDC00 + (cp & 0x3FF)); }
        else u[nu++] = (uint16_t)cp;
        for (int i = 0; i < nu; i++) { if (be) { o[k++] = u[i] >> 8; o[k++] = u[i] & 0xFF; } else { o[k++] = u[i] & 0xFF; o[k++] = u[i] >> 8; } }
        return k;
    }
    default: {
        int be = c->to == E_UTF32BE;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        if (c->to == E_UTF32 && !c->out_bom_done) { c->out_bom_done = 1; o[k++] = 0xFF; o[k++] = 0xFE; o[k++] = 0; o[k++] = 0; }
        if (be) { o[k++] = cp >> 24; o[k++] = cp >> 16; o[k++] = cp >> 8; o[k++] = cp; }
        else { o[k++] = cp; o[k++] = cp >> 8; o[k++] = cp >> 16; o[k++] = cp >> 24; }
        return k;
    }
    }
}
static size_t kl3_iconv(void *cdp, char **inp, size_t *inleft, char **outp, size_t *outleft) {
    kl_iconv *c = cdp;
    if (!c || c == (void *)-1) { errno = EBADF; return (size_t)-1; }
    if (!inp || !*inp) {                                   // reset the shift state
        c->in_bom_done = c->in_be = c->out_bom_done = 0;
        return 0;
    }
    size_t irrev = 0;
    while (*inleft > 0) {
        uint32_t cp = 0; int skip;
        int used = ic_decode(c, (const unsigned char *)*inp, *inleft, &cp, &skip);
        if (used == -2) { *inp += (c->from == E_UTF16 || c->from == E_UTF16LE || c->from == E_UTF16BE) ? 2 : 4; *inleft -= (c->from == E_UTF16 || c->from == E_UTF16LE || c->from == E_UTF16BE) ? 2 : 4; continue; }
        if (used == 0) { errno = EINVAL; return (size_t)-1; }
        if (used < 0) {
            if (!c->ignore) { errno = EILSEQ; return (size_t)-1; }
            *inp += skip; *inleft -= (size_t)skip; continue;
        }
        unsigned char o[8];
        int in_bom = c->out_bom_done;
        int k = ic_encode(c, cp, o);
        if (k == 0) {
            if (c->translit) { k = ic_encode(c, '?', o); irrev++; }
            else if (c->ignore) { *inp += used; *inleft -= (size_t)used; continue; }
            if (k == 0) { errno = EILSEQ; return (size_t)-1; }
        }
        if ((size_t)k > *outleft) { c->out_bom_done = in_bom; errno = E2BIG; return (size_t)-1; }
        memcpy(*outp, o, (size_t)k);
        *outp += k; *outleft -= (size_t)k;
        *inp += used; *inleft -= (size_t)used;
    }
    return irrev;
}

// ======================================================================
// C11 <threads.h> and call_once over the pthread entries the shim already serves (their
// guest-side layouts: thrd_t = pthread_t, mtx_t = pthread_mutex_t, cnd_t = pthread_cond_t,
// tss_t = pthread_key_t, once_flag = pthread_once_t in bionic).
// ======================================================================
#define TH_SUCCESS 0
#define TH_BUSY 1
#define TH_ERROR 2
#define TH_NOMEM 3
#define TH_TIMEDOUT 4
typedef int (*pfn_i1)(void *);
#define PF(name) static void *pf_##name
PF(mutexattr_init); PF(mutexattr_settype); PF(mutexattr_destroy); PF(mutex_init); PF(mutex_destroy);
PF(mutex_lock); PF(mutex_trylock); PF(mutex_unlock); PF(mutex_timedlock);
PF(cond_init); PF(cond_destroy); PF(cond_signal); PF(cond_broadcast); PF(cond_wait); PF(cond_timedwait);
PF(create); PF(join); PF(detach); PF(self); PF(equal); PF(exit_); PF(key_create); PF(key_delete);
PF(getspecific); PF(setspecific); PF(once); PF(nanosleep_); PF(sched_yield_);
static void th_bind(void) {
    static int done;
    if (done) return;
    LAZY(pf_mutexattr_init, "pthread_mutexattr_init"); LAZY(pf_mutexattr_settype, "pthread_mutexattr_settype");
    LAZY(pf_mutexattr_destroy, "pthread_mutexattr_destroy"); LAZY(pf_mutex_init, "pthread_mutex_init");
    LAZY(pf_mutex_destroy, "pthread_mutex_destroy"); LAZY(pf_mutex_lock, "pthread_mutex_lock");
    LAZY(pf_mutex_trylock, "pthread_mutex_trylock"); LAZY(pf_mutex_unlock, "pthread_mutex_unlock");
    LAZY(pf_mutex_timedlock, "pthread_mutex_timedlock");
    LAZY(pf_cond_init, "pthread_cond_init"); LAZY(pf_cond_destroy, "pthread_cond_destroy");
    LAZY(pf_cond_signal, "pthread_cond_signal"); LAZY(pf_cond_broadcast, "pthread_cond_broadcast");
    LAZY(pf_cond_wait, "pthread_cond_wait"); LAZY(pf_cond_timedwait, "pthread_cond_timedwait");
    LAZY(pf_create, "pthread_create"); LAZY(pf_join, "pthread_join"); LAZY(pf_detach, "pthread_detach");
    LAZY(pf_self, "pthread_self"); LAZY(pf_equal, "pthread_equal"); LAZY(pf_exit_, "pthread_exit");
    LAZY(pf_key_create, "pthread_key_create"); LAZY(pf_key_delete, "pthread_key_delete");
    LAZY(pf_getspecific, "pthread_getspecific"); LAZY(pf_setspecific, "pthread_setspecific");
    LAZY(pf_once, "pthread_once"); LAZY(pf_nanosleep_, "nanosleep"); LAZY(pf_sched_yield_, "sched_yield");
    done = 1;
}
// rc values come back in the guest's errno numbering: EBUSY 16, ENOMEM 12, ETIMEDOUT 110.
static int th_rc_lx(int e) { return e == 0 ? TH_SUCCESS : e == 16 ? TH_BUSY : e == 110 ? TH_TIMEDOUT : e == 12 ? TH_NOMEM : TH_ERROR; }
static int kl3_mtx_init(void *m, int type) {
    th_bind();
    if (!pf_mutex_init) return TH_ERROR;
    if (type & 1) {                                        // mtx_recursive
        char attr[16] = {0};                               // pthread_mutexattr_t is 4 bytes in bionic
        ((int (*)(void *))pf_mutexattr_init)(attr);
        ((int (*)(void *, int))pf_mutexattr_settype)(attr, 1);      // PTHREAD_MUTEX_RECURSIVE
        int r = ((int (*)(void *, const void *))pf_mutex_init)(m, attr);
        ((int (*)(void *))pf_mutexattr_destroy)(attr);
        return th_rc_lx(r);
    }
    return th_rc_lx(((int (*)(void *, const void *))pf_mutex_init)(m, NULL));
}
static void kl3_mtx_destroy(void *m) { th_bind(); if (pf_mutex_destroy) ((int (*)(void *))pf_mutex_destroy)(m); }
static int kl3_mtx_lock(void *m) { th_bind(); return th_rc_lx(((int (*)(void *))pf_mutex_lock)(m)); }
static int kl3_mtx_trylock(void *m) { th_bind(); return th_rc_lx(((int (*)(void *))pf_mutex_trylock)(m)); }
static int kl3_mtx_unlock(void *m) { th_bind(); return th_rc_lx(((int (*)(void *))pf_mutex_unlock)(m)); }
static int kl3_mtx_timedlock(void *m, const struct timespec *ts) { th_bind(); return th_rc_lx(((int (*)(void *, const void *))pf_mutex_timedlock)(m, ts)); }
static int kl3_cnd_init(void *c) { th_bind(); return th_rc_lx(((int (*)(void *, const void *))pf_cond_init)(c, NULL)); }
static void kl3_cnd_destroy(void *c) { th_bind(); ((int (*)(void *))pf_cond_destroy)(c); }
static int kl3_cnd_signal(void *c) { th_bind(); return th_rc_lx(((int (*)(void *))pf_cond_signal)(c)); }
static int kl3_cnd_broadcast(void *c) { th_bind(); return th_rc_lx(((int (*)(void *))pf_cond_broadcast)(c)); }
static int kl3_cnd_wait(void *c, void *m) { th_bind(); return th_rc_lx(((int (*)(void *, void *))pf_cond_wait)(c, m)); }
static int kl3_cnd_timedwait(void *c, void *m, const struct timespec *ts) { th_bind(); return th_rc_lx(((int (*)(void *, void *, const void *))pf_cond_timedwait)(c, m, ts)); }
typedef struct { pfn_i1 fn; void *arg; } th_start;
static void *th_tramp(void *p) {
    th_start s = *(th_start *)p; free(p);
    return (void *)(intptr_t)s.fn(s.arg);
}
static int kl3_thrd_create(void *t, pfn_i1 fn, void *arg) {
    th_bind();
    th_start *s = malloc(sizeof *s);
    if (!s) return TH_NOMEM;
    s->fn = fn; s->arg = arg;
    int r = ((int (*)(void *, const void *, void *(*)(void *), void *))pf_create)(t, NULL, th_tramp, s);
    if (r) free(s);
    return th_rc_lx(r);
}
static int kl3_thrd_join(unsigned long t, int *res) {
    th_bind();
    void *rv = NULL;
    int r = ((int (*)(unsigned long, void **))pf_join)(t, &rv);
    if (!r && res) *res = (int)(intptr_t)rv;
    return r ? TH_ERROR : TH_SUCCESS;
}
static int kl3_thrd_detach(unsigned long t) { th_bind(); return ((int (*)(unsigned long))pf_detach)(t) ? TH_ERROR : TH_SUCCESS; }
static unsigned long kl3_thrd_current(void) { th_bind(); return ((unsigned long (*)(void))pf_self)(); }
static int kl3_thrd_equal(unsigned long a, unsigned long b) { th_bind(); return ((int (*)(unsigned long, unsigned long))pf_equal)(a, b); }
static void kl3_thrd_exit(int res) { th_bind(); ((void (*)(void *))pf_exit_)((void *)(intptr_t)res); for (;;) pause(); }
static void kl3_thrd_yield(void) { th_bind(); if (pf_sched_yield_) ((int (*)(void))pf_sched_yield_)(); }
// thrd_sleep: 0 slept the whole time, -1 interrupted by a signal, other negative on error.
static int kl3_thrd_sleep(const struct timespec *d, struct timespec *rem) {
    th_bind();
    if (!d) return -2;
    if (((int (*)(const void *, void *))pf_nanosleep_)(d, rem) == 0) return 0;
    return errno == EINTR ? -1 : -2;
}
static int kl3_tss_create(unsigned *k, void (*d)(void *)) { th_bind(); return ((int (*)(unsigned *, void (*)(void *)))pf_key_create)(k, d) ? TH_ERROR : TH_SUCCESS; }
static void kl3_tss_delete(unsigned k) { th_bind(); ((int (*)(unsigned))pf_key_delete)(k); }
static void *kl3_tss_get(unsigned k) { th_bind(); return ((void *(*)(unsigned))pf_getspecific)(k); }
static int kl3_tss_set(unsigned k, void *v) { th_bind(); return ((int (*)(unsigned, const void *))pf_setspecific)(k, v) ? TH_ERROR : TH_SUCCESS; }
static void kl3_call_once(void *flag, void (*fn)(void)) { th_bind(); ((int (*)(void *, void (*)(void)))pf_once)(flag, fn); }

// ======================================================================
// <complex.h> and the long double leftovers. float/double complex travel in the same registers on
// bionic and Darwin (HFA of two), so those forward. long double complex is two binary128 values:
// a short-vector aggregate of two 128-bit vectors on AArch64, converted through double like the
// scalar *l functions in kl_bionic_extra.c.
// ======================================================================
#if defined(__aarch64__) && !defined(KL_EXTRA3_TEST)
#include <arm_neon.h>
typedef uint64x2_t kld;
static inline kl_q ld3_in(kld v) { kl_q q = { vgetq_lane_u64(v, 0), vgetq_lane_u64(v, 1) }; return q; }
static inline kld ld3_out(kl_q q) { uint64x2_t v = { q.lo, q.hi }; return v; }
#else
typedef kl_q kld;
static inline kl_q ld3_in(kld v) { return v; }
static inline kld ld3_out(kl_q q) { return q; }
#endif
static inline double D3(kld v) { return kl_q_to_double(ld3_in(v)); }
static inline kld Q3(double d) { return ld3_out(kl_q_from_double(d)); }
typedef struct { kld re, im; } kcl;
static inline double _Complex CIN(kcl z) { return D3(z.re) + D3(z.im) * I; }
static inline kcl COUT(double _Complex c) { kcl r = { Q3(creal(c)), Q3(cimag(c)) }; return r; }
#define CL1(n, f) static kcl kl3_##n(kcl z) { return COUT(f(CIN(z))); }
CL1(cacosl, cacos) CL1(cacoshl, cacosh) CL1(casinl, casin) CL1(casinhl, casinh) CL1(catanl, catan)
CL1(catanhl, catanh) CL1(ccosl, ccos) CL1(ccoshl, ccosh) CL1(cexpl, cexp) CL1(clogl, clog)
CL1(conjl, conj) CL1(cprojl, cproj) CL1(csinl, csin) CL1(csinhl, csinh) CL1(csqrtl, csqrt)
CL1(ctanl, ctan) CL1(ctanhl, ctanh)
static kcl kl3_cpowl(kcl a, kcl b) { return COUT(cpow(CIN(a), CIN(b))); }
static kld kl3_cabsl(kcl z) { return Q3(cabs(CIN(z))); }
static kld kl3_cargl(kcl z) { return Q3(carg(CIN(z))); }
static kld kl3_creall(kcl z) { return z.re; }
static kld kl3_cimagl(kcl z) { return z.im; }

static kld kl3_fabsl(kld x) { return Q3(fabs(D3(x))); }
static kld kl3_fdiml(kld x, kld y) { return Q3(fdim(D3(x), D3(y))); }
static kld kl3_modfl(kld x, kld *ip) { double i; double f = modf(D3(x), &i); *ip = Q3(i); return Q3(f); }
static kld kl3_significandl(kld x) {
    double v = D3(x);
    if (v == 0 || !isfinite(v)) return x;
    int e; double m = frexp(v, &e);
    return Q3(m * 2);
}
// isfinite & friends are macros in the headers; bionic also exports them as functions. The *l forms
// read the binary128 exponent directly (converting to double first would call 1e5000 infinite).
static int kl3_isfinite(double d) { return isfinite(d) != 0; }
static int kl3_isfinitef(float f) { return isfinite(f) != 0; }
static int kl3_isnormal(double d) { return isnormal(d) != 0; }
static int kl3_isnormalf(float f) { return isnormal(f) != 0; }
static int kl3_isfinitel(kld x) { return ((ld3_in(x).hi >> 48) & 0x7FFF) != 0x7FFF; }
static int kl3_isnormall(kld x) { unsigned e = (unsigned)((ld3_in(x).hi >> 48) & 0x7FFF); return e != 0 && e != 0x7FFF; }
static int g_signgam;                                   // lgamma's sign; the *_r forms are the reliable ones

// ======================================================================
// <utmp.h>, <mntent.h>
// ======================================================================
// An app has no utmp database: bionic's calls are present and find nothing.
static void kl3_setutent(void) {}
static void kl3_endutent(void) {}
static void *kl3_getutent(void) { return NULL; }
static void *kl3_pututline(const void *u) { (void)u; errno = EPERM; return NULL; }
static int kl3_utmpname(const char *f) { (void)f; return 0; }

typedef struct { char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts; int mnt_freq, mnt_passno; } kl_mntent;
static void *kl3_setmntent(const char *path, const char *mode) {
    static void *fo; LAZY(fo, "fopen");
    return fo ? ((void *(*)(const char *, const char *))fo)(path, mode) : NULL;
}
static int kl3_endmntent(void *f) {
    static void *fc; LAZY(fc, "fclose");
    if (f && fc) ((int (*)(void *))fc)(f);
    return 1;
}
static char *mnt_field(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return NULL; }
    char *start = s, *w = s;
    while (*s && *s != ' ' && *s != '\t') {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *w++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0')); s += 4;
        } else *w++ = *s++;
    }
    int more = *s != 0;
    *w = 0;
    *p = more ? s + 1 : s;
    return start;
}
static kl_mntent *kl3_getmntent_r(void *f, kl_mntent *m, char *buf, int len) {
    static void *fg; LAZY(fg, "fgets");
    if (!fg || !f || len < 2) return NULL;
    for (;;) {
        if (!((char *(*)(char *, int, void *))fg)(buf, len, f)) return NULL;
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\n') p[l - 1] = 0;
        char *a = mnt_field(&p), *b = mnt_field(&p), *c = mnt_field(&p), *d = mnt_field(&p);
        if (!a || !b || !c) continue;
        m->mnt_fsname = a; m->mnt_dir = b; m->mnt_type = c; m->mnt_opts = d ? d : (char *)"rw";
        char *e = mnt_field(&p), *g = mnt_field(&p);
        m->mnt_freq = e ? atoi(e) : 0; m->mnt_passno = g ? atoi(g) : 0;
        return m;
    }
}
static kl_mntent *kl3_getmntent(void *f) {
    static _Thread_local kl_mntent m; static _Thread_local char buf[1024];
    return kl3_getmntent_r(f, &m, buf, sizeof buf);
}
static char *kl3_hasmntopt(const kl_mntent *m, const char *opt) {
    size_t n = strlen(opt);
    for (char *p = m->mnt_opts; p && *p; ) {
        if (!strncmp(p, opt, n) && (p[n] == 0 || p[n] == ',' || p[n] == '=')) return p;
        p = strchr(p, ',');
        if (p) p++;
    }
    return NULL;
}

// ======================================================================
// Name resolution, offline. KL_NET_OFFLINE (the app sets it from the launcher; unset means
// offline, since games here never reach a network) makes every lookup answer what Android
// answers on a device with no connectivity: numeric addresses and "localhost" resolve, anything
// else is HOST_NOT_FOUND, and the raw resolver (res_*) has no server to talk to.
// ======================================================================
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define H_HOST_NOT_FOUND 1
#define H_NO_RECOVERY 3
__attribute__((unused)) static int net_offline(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("KL_NET_OFFLINE"); v = e && *e ? (e[0] != '0') : 1; }
    return v;
}
typedef struct { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; } kl_hostent;
// Lays a hostent out inside the caller's buffer; returns 0, or the Linux ERANGE.
static int host_fill(kl_hostent *h, char *buf, size_t len, const char *name, int af, const void *addr) {
    size_t alen = af == LX_AF_INET6 ? 16 : 4, nl = strlen(name) + 1;
    uintptr_t b = ((uintptr_t)buf + 7) & ~(uintptr_t)7;
    size_t pad = b - (uintptr_t)buf;
    size_t need = pad + 3 * sizeof(char *) + alen + nl;
    if (len < need) return LX_ERANGE;
    char **vec = (char **)b;
    char *ad = (char *)(vec + 3), *nm = ad + alen;
    memcpy(ad, addr, alen); memcpy(nm, name, nl);
    vec[0] = ad; vec[1] = NULL; vec[2] = NULL;               // addr_list[2], aliases[0] share the tail
    h->h_name = nm; h->h_aliases = &vec[2]; h->h_addrtype = af; h->h_length = (int)alen; h->h_addr_list = &vec[0];
    return 0;
}
static int host_lookup(const char *name, int af, unsigned char out[16]) {
    if (!name) return 0;
    if (af == LX_AF_INET) {
        struct in_addr a;
        if (inet_pton(AF_INET, name, &a) == 1) { memcpy(out, &a, 4); return 1; }
    } else if (af == LX_AF_INET6) {
        struct in6_addr a;
        if (inet_pton(AF_INET6, name, &a) == 1) { memcpy(out, &a, 16); return 1; }
    }
    if (!strcasecmp(name, "localhost")) {
        if (af == LX_AF_INET) { out[0] = 127; out[1] = 0; out[2] = 0; out[3] = 1; }
        else { memset(out, 0, 16); out[15] = 1; }
        return 1;
    }
    return 0;
}
static int gethost_common(const char *name, int af, kl_hostent *ret, char *buf, size_t len, kl_hostent **result, int *herr) {
    unsigned char a[16];
    *result = NULL;
    if (af != LX_AF_INET && af != LX_AF_INET6) { if (herr) *herr = H_NO_RECOVERY; return LX_EINVAL; }
    int found = host_lookup(name, af, a);
#ifndef KL_EXTRA3_TEST
    if (!found && !net_offline() && name) {
        struct hostent *d = gethostbyname2(name, af == LX_AF_INET6 ? AF_INET6 : AF_INET);
        if (d && d->h_addr_list && d->h_addr_list[0]) { memcpy(a, d->h_addr_list[0], af == LX_AF_INET6 ? 16 : 4); found = 1; }
    }
#endif
    if (!found) { if (herr) *herr = H_HOST_NOT_FOUND; return LX_ENOENT; }
    int r = host_fill(ret, buf, len, name, af, a);
    if (r) { if (herr) *herr = H_NO_RECOVERY; return r; }
    *result = ret;
    return 0;
}
static int kl3_gethostbyname_r(const char *name, kl_hostent *ret, char *buf, size_t len, kl_hostent **res, int *herr) {
    return gethost_common(name, LX_AF_INET, ret, buf, len, res, herr);
}
static int kl3_gethostbyname2_r(const char *name, int af, kl_hostent *ret, char *buf, size_t len, kl_hostent **res, int *herr) {
    return gethost_common(name, af, ret, buf, len, res, herr);
}
static int kl3_gethostbyaddr_r(const void *addr, unsigned alen, int type, kl_hostent *ret, char *buf, size_t len, kl_hostent **res, int *herr) {
    *res = NULL;
    const unsigned char *a = addr;
    int lo4 = type == LX_AF_INET && alen == 4 && a[0] == 127 && !a[1] && !a[2] && a[3] == 1;
    int lo6 = type == LX_AF_INET6 && alen == 16 && a[15] == 1 && !memcmp(a, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 15);
    if (!lo4 && !lo6) { if (herr) *herr = H_HOST_NOT_FOUND; return LX_ENOENT; }
    int r = host_fill(ret, buf, len, lo4 ? "localhost" : "ip6-localhost", type, a);
    if (r) { if (herr) *herr = H_NO_RECOVERY; return r; }
    *res = ret;
    return 0;
}
// The /etc/hosts-style enumerators, networks and services databases: bionic has none.
static void *kl3_gethostent(void) { return NULL; }
static void *kl3_getnetent(void) { return NULL; }
static void *kl3_getnetbyname(const char *n) { (void)n; return NULL; }
static void *kl3_getnetbyaddr(uint32_t n, int t) { (void)n; (void)t; return NULL; }
static void *kl3_getservent(void) { return NULL; }
static void kl3_sethostent(int s) { (void)s; }
static void kl3_endhostent(void) {}
static void kl3_setnetent(int s) { (void)s; }
static void kl3_endnetent(void) {}
static void kl3_setservent(int s) { (void)s; }
static void kl3_endservent(void) {}
static void kl3_setprotoent(int s) { (void)s; }
static void kl3_endprotoent(void) {}

// protocols: bionic carries the IANA list built in (no /etc/protocols).
typedef struct { char *p_name; char **p_aliases; int p_proto; } kl_protoent;
static const struct { const char *name, *alias; int num; } g_protos[] = {
    {"ip", "IP", 0}, {"icmp", "ICMP", 1}, {"igmp", "IGMP", 2}, {"ggp", "GGP", 3}, {"ipencap", "IP-ENCAP", 4},
    {"st", "ST", 5}, {"tcp", "TCP", 6}, {"egp", "EGP", 8}, {"igp", "IGP", 9}, {"pup", "PUP", 12},
    {"udp", "UDP", 17}, {"hmp", "HMP", 20}, {"xns-idp", "XNS-IDP", 22}, {"rdp", "RDP", 27},
    {"iso-tp4", "ISO-TP4", 29}, {"dccp", "DCCP", 33}, {"xtp", "XTP", 36}, {"ddp", "DDP", 37},
    {"idpr-cmtp", "IDPR-CMTP", 38}, {"ipv6", "IPv6", 41}, {"ipv6-route", "IPv6-Route", 43},
    {"ipv6-frag", "IPv6-Frag", 44}, {"idrp", "IDRP", 45}, {"rsvp", "RSVP", 46}, {"gre", "GRE", 47},
    {"esp", "IPSEC-ESP", 50}, {"ah", "IPSEC-AH", 51}, {"skip", "SKIP", 57}, {"ipv6-icmp", "IPv6-ICMP", 58},
    {"ipv6-nonxt", "IPv6-NoNxt", 59}, {"ipv6-opts", "IPv6-Opts", 60}, {"rspf", "RSPF", 73},
    {"vmtp", "VMTP", 81}, {"eigrp", "EIGRP", 88}, {"ospf", "OSPFIGP", 89}, {"ax.25", "AX.25", 93},
    {"ipip", "IPIP", 94}, {"etherip", "ETHERIP", 97}, {"encap", "ENCAP", 98}, {"pim", "PIM", 103},
    {"ipcomp", "IPCOMP", 108}, {"vrrp", "VRRP", 112}, {"l2tp", "L2TP", 115}, {"isis", "ISIS", 124},
    {"sctp", "SCTP", 132}, {"fc", "FC", 133}, {"udplite", "UDPLite", 136}, {"mpls-in-ip", "MPLS-in-IP", 137},
    {"hip", "HIP", 139}, {"shim6", "Shim6", 140}, {"wesp", "WESP", 141}, {"rohc", "ROHC", 142},
};
#define N_PROTOS (sizeof g_protos / sizeof g_protos[0])
static _Thread_local kl_protoent t_pe; static _Thread_local char *t_pe_alias[2];
static kl_protoent *proto_at(size_t i) {
    if (i >= N_PROTOS) return NULL;
    t_pe.p_name = (char *)g_protos[i].name; t_pe_alias[0] = (char *)g_protos[i].alias; t_pe_alias[1] = NULL;
    t_pe.p_aliases = t_pe_alias; t_pe.p_proto = g_protos[i].num;
    return &t_pe;
}
static size_t g_proto_cur;
static kl_protoent *kl3_getprotoent(void) { return proto_at(g_proto_cur++); }
static kl_protoent *kl3_getprotobyname(const char *n) {
    if (!n) return NULL;
    for (size_t i = 0; i < N_PROTOS; i++)
        if (!strcasecmp(n, g_protos[i].name) || !strcasecmp(n, g_protos[i].alias)) return proto_at(i);
    return NULL;
}
static kl_protoent *kl3_getprotobynumber(int n) {
    for (size_t i = 0; i < N_PROTOS; i++) if (g_protos[i].num == n) return proto_at(i);
    return NULL;
}
static void kl3_herror(const char *s) {
    static const char *const msg[] = { "Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure",
                                       "Unknown server error", "No address associated with name" };
    extern int *kl_h_errno_ptr(void);
    int e = *kl_h_errno_ptr();
    fprintf(stderr, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", (e >= 0 && e <= 4) ? msg[e] : "Unknown resolver error");
}

// byte order and the BSD inet_* leftovers
static uint32_t kl3_htonl(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t kl3_htons(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t kl3_inet_lnaof(struct in_addr in) {
    uint32_t a = ntohl(in.s_addr);
    return (a & 0x80000000u) == 0 ? (a & 0xFFFFFFu) : (a & 0xC0000000u) == 0x80000000u ? (a & 0xFFFFu) : (a & 0xFFu);
}
static uint32_t kl3_inet_netof(struct in_addr in) {
    uint32_t a = ntohl(in.s_addr);
    return (a & 0x80000000u) == 0 ? (a >> 24) : (a & 0xC0000000u) == 0x80000000u ? (a >> 16) : (a >> 8);
}
static struct in_addr kl3_inet_makeaddr(uint32_t net, uint32_t host) {
    uint32_t a = net < 256 ? (net << 24) | (host & 0xFFFFFFu) : net < 65536 ? (net << 16) | (host & 0xFFFFu) : (net << 8) | (host & 0xFFu);
    struct in_addr r; r.s_addr = htonl(a);
    return r;
}
// inet_network: like inet_addr but the result is in host order and a short form is a network number.
static uint32_t kl3_inet_network(const char *s) {
    uint32_t parts[4]; int n = 0;
    while (*s) {
        if (n == 4) return (uint32_t)-1;
        char *end; unsigned long v = strtoul(s, &end, 0);
        if (end == s) return (uint32_t)-1;
        if (v > 0xFFFFFFFFul) return (uint32_t)-1;
        parts[n++] = (uint32_t)v;
        s = end;
        if (*s == '.') { s++; if (!*s) return (uint32_t)-1; } else if (*s) return (uint32_t)-1;
    }
    if (!n) return (uint32_t)-1;
    if (n == 1) return parts[0];
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { if (parts[i] > 255) return (uint32_t)-1; r = (r << 8) | parts[i]; }
    return r;
}
struct kl_ether_addr { uint8_t o[6]; };
static struct kl_ether_addr *kl3_ether_aton_r(const char *s, struct kl_ether_addr *a) {
    if (!s) return NULL;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)*s)) return NULL;
        unsigned v = 0, d = 0;
        while (isxdigit((unsigned char)*s) && d < 3) { v = v * 16 + (isdigit((unsigned char)*s) ? (unsigned)(*s - '0') : (unsigned)(tolower((unsigned char)*s) - 'a' + 10)); s++; d++; }
        if (v > 255 || d > 2) return NULL;
        a->o[i] = (uint8_t)v;
        if (i < 5) { if (*s != ':') return NULL; s++; }
    }
    return *s ? NULL : a;
}
static struct kl_ether_addr *kl3_ether_aton(const char *s) { static _Thread_local struct kl_ether_addr a; return kl3_ether_aton_r(s, &a); }
static char *kl3_ether_ntoa_r(const struct kl_ether_addr *a, char *buf) {
    sprintf(buf, "%x:%x:%x:%x:%x:%x", a->o[0], a->o[1], a->o[2], a->o[3], a->o[4], a->o[5]);
    return buf;
}
static char *kl3_ether_ntoa(const struct kl_ether_addr *a) { static _Thread_local char b[18]; return kl3_ether_ntoa_r(a, b); }

// ---- the raw resolver: no server to ask offline ----
static int res_fail(void) { kl_h_errno_set(H_HOST_NOT_FOUND); return -1; }
static int kl3_res_init(void) { return 0; }
static int kl3_res_query(const char *d, int c, int t, unsigned char *a, int l) { (void)d; (void)c; (void)t; (void)a; (void)l; return res_fail(); }
static int kl3_res_querydomain(const char *n, const char *d, int c, int t, unsigned char *a, int l) { (void)n; (void)d; (void)c; (void)t; (void)a; (void)l; return res_fail(); }
static int kl3_res_send(const unsigned char *m, int ml, unsigned char *a, int al) {
    (void)m; (void)ml; (void)a; (void)al;
    errno = ENETUNREACH; kl_h_errno_set(H_HOST_NOT_FOUND); return -1;
}
static int kl3_res_nquery(void *st, const char *d, int c, int t, unsigned char *a, int l) { (void)st; return kl3_res_query(d, c, t, a, l); }
static int kl3_res_nsearch(void *st, const char *d, int c, int t, unsigned char *a, int l) { (void)st; return kl3_res_query(d, c, t, a, l); }
static int kl3_res_nquerydomain(void *st, const char *n, const char *d, int c, int t, unsigned char *a, int l) { (void)st; return kl3_res_querydomain(n, d, c, t, a, l); }
static int kl3_res_nsend(void *st, const unsigned char *m, int ml, unsigned char *a, int al) { (void)st; return kl3_res_send(m, ml, a, al); }
static int kl3_res_ninit(void *st) { (void)st; return 0; }
static void kl3_res_nclose(void *st) { (void)st; }
static void kl3_res_close(void) {}
static unsigned kl3_res_randomid(void) { static unsigned s = 0x2545; s = s * 1103515245u + 12345u; return (s >> 8) & 0xFFFF; }
static int kl3_res_isourserver(const void *a) { (void)a; return 0; }
static char *kl3_res_hostalias(const void *st, const char *n, char *buf, size_t l) { (void)st; (void)n; (void)buf; (void)l; return NULL; }
static int res_label_ok(const char *dn, int strict) {
    size_t n = strlen(dn);
    if (n == 0 || n > 253) return 0;
    const char *lab = dn; size_t ll = 0;
    for (const char *p = dn; ; p++) {
        if (*p == '.' || !*p) {
            if (ll == 0 && !(*p == 0 && p > dn)) return 0;
            if (ll > 63) return 0;
            if (strict && ll && (!isalnum((unsigned char)lab[0]) || !isalnum((unsigned char)p[-1]))) return 0;
            if (!*p) break;
            lab = p + 1; ll = 0; continue;
        }
        unsigned char c = (unsigned char)*p;
        if (strict ? !(isalnum(c) || c == '-' || c == '_') : (c < 0x21 || c > 0x7E)) return 0;
        ll++;
    }
    return 1;
}
static int kl3_res_hnok(const char *dn) { return res_label_ok(dn, 1); }
static int kl3_res_dnok(const char *dn) { return res_label_ok(dn, 0); }
static int kl3_res_ownok(const char *dn) { return res_label_ok(dn, 0); }
static int kl3_res_mailok(const char *dn) { return res_label_ok(dn, 0); }

// ---- DNS wire format helpers (ns_* / dn_*, from the BIND resolver's public contract) ----
#define NS_MAXDNAME 1025
#define NS_MAXCDNAME 255
static uint16_t kl3_ns_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t kl3_ns_get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void kl3_ns_put16(uint32_t v, uint8_t *p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void kl3_ns_put32(uint32_t v, uint8_t *p) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t kl3_getshort(const uint8_t *p) { return kl3_ns_get16(p); }
static uint32_t kl3_getlong(const uint8_t *p) { return kl3_ns_get32(p); }
static void kl3_putshort(uint16_t v, uint8_t *p) { kl3_ns_put16(v, p); }
static void kl3_putlong(uint32_t v, uint8_t *p) { kl3_ns_put32(v, p); }
// Uncompressed wire name at src -> text (escapes ". ; \ $ ( ) @" and non-printables as BIND does).
static int kl3_ns_name_ntop(const uint8_t *src, char *dst, size_t dstsiz) {
    const uint8_t *cp = src; char *dn = dst, *eom = dst + dstsiz;
    unsigned n;
    while ((n = *cp++) != 0) {
        if (n & 0xC0) { errno = EMSGSIZE; return -1; }
        if (dn != dst) { if (dn >= eom) { errno = EMSGSIZE; return -1; } *dn++ = '.'; }
        if (dn + n >= eom) { errno = EMSGSIZE; return -1; }
        for (; n > 0; n--) {
            unsigned char c = *cp++;
            if (strchr(".;\\$()@\"", c) && c) { if (dn + 1 >= eom) { errno = EMSGSIZE; return -1; } *dn++ = '\\'; *dn++ = (char)c; }
            else if (c <= 0x20 || c >= 0x7F) {
                if (dn + 3 >= eom) { errno = EMSGSIZE; return -1; }
                *dn++ = '\\'; *dn++ = (char)('0' + c / 100); *dn++ = (char)('0' + (c / 10) % 10); *dn++ = (char)('0' + c % 10);
            } else { if (dn >= eom) { errno = EMSGSIZE; return -1; } *dn++ = (char)c; }
        }
    }
    if (dn == dst) { if (dn >= eom) { errno = EMSGSIZE; return -1; } *dn++ = '.'; }
    if (dn >= eom) { errno = EMSGSIZE; return -1; }
    *dn = 0;
    return (int)(dn - dst);
}
// Text -> uncompressed wire name; 1 if the name was absolute (ended in '.'), 0 if not, -1 on error.
static int kl3_ns_name_pton(const char *src, uint8_t *dst, size_t dstsiz) {
    uint8_t *label = dst, *bp = dst + 1, *eom = dst + dstsiz;
    if (dstsiz < 1) { errno = EMSGSIZE; return -1; }
    if (src[0] == '.' && src[1] == 0) { *dst = 0; return 1; }
    if (!src[0]) { *dst = 0; return 0; }
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            size_t l = (size_t)(bp - label - 1);
            if (l == 0 || l > 63) { errno = EMSGSIZE; return -1; }
            *label = (uint8_t)l;
            if (bp >= eom) { errno = EMSGSIZE; return -1; }
            if (!p[1]) { *bp++ = 0; return 1; }
            label = bp++;
            continue;
        }
        if (c == '\\') {
            c = (unsigned char)*++p;
            if (isdigit(c) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2])) {
                int v = (c - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
                if (v > 255) { errno = EMSGSIZE; return -1; }
                c = (unsigned char)v; p += 2;
            } else if (!c) { errno = EMSGSIZE; return -1; }
        }
        if (bp >= eom) { errno = EMSGSIZE; return -1; }
        *bp++ = c;
    }
    size_t l = (size_t)(bp - label - 1);
    if (l > 63) { errno = EMSGSIZE; return -1; }
    *label = (uint8_t)l;
    if (bp >= eom) { errno = EMSGSIZE; return -1; }
    *bp++ = 0;
    return 0;
}
// Expand a possibly compressed name at src (inside msg..eom) into uncompressed wire form at dst.
// Returns the number of bytes the name occupies at src, or -1.
static int kl3_ns_name_unpack(const uint8_t *msg, const uint8_t *eom, const uint8_t *src, uint8_t *dst, size_t dstsiz) {
    const uint8_t *srcp = src; uint8_t *dstp = dst, *dsteom = dst + dstsiz;
    int len = -1, checked = 0;
    unsigned n;
    if (srcp < msg || srcp >= eom) { errno = EMSGSIZE; return -1; }
    while (srcp < eom && (n = *srcp++) != 0) {
        switch (n & 0xC0) {
        case 0:
            if (dstp + n + 1 >= dsteom || srcp + n >= eom) { errno = EMSGSIZE; return -1; }
            checked += (int)n + 1;
            *dstp++ = (uint8_t)n; memcpy(dstp, srcp, n); dstp += n; srcp += n;
            break;
        case 0xC0: {
            if (srcp >= eom) { errno = EMSGSIZE; return -1; }
            if (len < 0) len = (int)(srcp - src) + 1;
            const uint8_t *np = msg + ((n & 0x3F) << 8 | *srcp);
            if (np < msg || np >= eom) { errno = EMSGSIZE; return -1; }
            srcp = np;
            checked += 2;
            if (checked >= (int)(eom - msg)) { errno = EMSGSIZE; return -1; }       // pointer loop
            break;
        }
        default: errno = EMSGSIZE; return -1;
        }
    }
    if (dstp >= dsteom) { errno = EMSGSIZE; return -1; }
    *dstp = 0;
    if (len < 0) len = (int)(srcp - src);
    return len;
}
static int kl3_ns_name_uncompress(const uint8_t *msg, const uint8_t *eom, const uint8_t *src, char *dst, size_t dstsiz) {
    uint8_t tmp[NS_MAXCDNAME + 2];
    int n = kl3_ns_name_unpack(msg, eom, src, tmp, sizeof tmp);
    if (n < 0) return -1;
    if (kl3_ns_name_ntop(tmp, dst, dstsiz) < 0) return -1;
    return n;
}
static int kl3_ns_name_skip(const uint8_t **ptrptr, const uint8_t *eom) {
    const uint8_t *cp = *ptrptr; unsigned n;
    while (cp < eom && (n = *cp++) != 0) {
        switch (n & 0xC0) {
        case 0: cp += n; continue;
        case 0xC0: cp++; break;
        default: errno = EMSGSIZE; return -1;
        }
        break;
    }
    if (cp > eom) { errno = EMSGSIZE; return -1; }
    *ptrptr = cp;
    return 0;
}
// Without compression targets the packed name is the uncompressed one.
static int kl3_ns_name_pack(const uint8_t *src, uint8_t *dst, int dstsiz, const uint8_t **dnptrs, const uint8_t **lastdnptr) {
    (void)dnptrs; (void)lastdnptr;
    size_t n = 0;
    while (src[n]) n += src[n] + 1;
    n++;
    if ((int)n > dstsiz) { errno = EMSGSIZE; return -1; }
    memcpy(dst, src, n);
    return (int)n;
}
static int kl3_ns_name_compress(const char *src, uint8_t *dst, size_t dstsiz, const uint8_t **dnptrs, const uint8_t **lastdnptr) {
    uint8_t tmp[NS_MAXCDNAME + 2];
    if (kl3_ns_name_pton(src, tmp, sizeof tmp) == -1) return -1;
    return kl3_ns_name_pack(tmp, dst, (int)dstsiz, dnptrs, lastdnptr);
}
static int kl3_dn_expand(const uint8_t *msg, const uint8_t *eom, const uint8_t *src, char *dst, int dstsiz) {
    int n = kl3_ns_name_uncompress(msg, eom, src, dst, (size_t)dstsiz);
    if (n > 0 && dst[0] == '.') dst[0] = 0;
    return n;
}
static int kl3_dn_skipname(const uint8_t *ptr, const uint8_t *eom) {
    const uint8_t *saved = ptr;
    if (kl3_ns_name_skip(&ptr, eom) == -1) return -1;
    return (int)(ptr - saved);
}
static int kl3_dn_comp(const char *src, uint8_t *dst, int dstsiz, uint8_t **dnptrs, uint8_t **lastdnptr) {
    return kl3_ns_name_compress(src, dst, (size_t)dstsiz, (const uint8_t **)dnptrs, (const uint8_t **)lastdnptr);
}
static int kl3_dn_count_labels(const char *name) {
    int count = 0; size_t i, l = strlen(name);
    for (i = 0; i < l; i++) if (name[i] == '.') count++;
    if (l > 0 && name[l - 1] != '.') count++;
    if (name[0] == '*' && name[1] == '.' ) count--;
    return count;
}
static int kl3_ns_samename(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '.') la--;
    if (lb && b[lb - 1] == '.') lb--;
    return la == lb && strncasecmp(a, b, la) == 0 ? 1 : 0;
}
// res_mkquery(QUERY): header + one question; other opcodes are not built offline.
static int kl3_res_mkquery(int op, const char *dname, int cls, int type, const uint8_t *data, int datalen,
                           const uint8_t *newrr, uint8_t *buf, int buflen) {
    (void)data; (void)datalen; (void)newrr;
    if (op != 0 || !dname || buflen < 12 + 1 + 4) { errno = EINVAL; return -1; }
    memset(buf, 0, 12);
    kl3_ns_put16(kl3_res_randomid(), buf);
    buf[2] = 0x01;                                              // RD
    kl3_ns_put16(1, buf + 4);                                   // QDCOUNT
    int n = kl3_ns_name_compress(dname, buf + 12, (size_t)buflen - 12, NULL, NULL);
    if (n < 0 || 12 + n + 4 > buflen) { errno = EMSGSIZE; return -1; }
    kl3_ns_put16((uint32_t)type, buf + 12 + n);
    kl3_ns_put16((uint32_t)cls, buf + 12 + n + 2);
    return 12 + n + 4;
}
static int kl3_res_nmkquery(void *st, int op, const char *d, int c, int t, const uint8_t *dt, int dl, const uint8_t *nr, uint8_t *b, int bl) {
    (void)st; return kl3_res_mkquery(op, d, c, t, dt, dl, nr, b, bl);
}
// ns_msg / ns_rr as BIND defines them (bionic <arpa/nameser.h>).
typedef struct { const uint8_t *_msg, *_eom; uint16_t _id, _flags, _counts[4]; const uint8_t *_sections[4]; int _sect; int _rrnum; const uint8_t *_msg_ptr; } kl_ns_msg;
typedef struct { char name[NS_MAXDNAME]; uint16_t type, rr_class; uint32_t ttl; uint16_t rdlength; const uint8_t *rdata; } kl_ns_rr;
static int kl3_ns_initparse(const uint8_t *msg, int msglen, kl_ns_msg *h) {
    const uint8_t *eom = msg + msglen;
    memset(h, 0, sizeof *h);
    h->_msg = msg; h->_eom = eom;
    if (msg + 12 > eom) { errno = EMSGSIZE; return -1; }
    h->_id = kl3_ns_get16(msg); h->_flags = kl3_ns_get16(msg + 2);
    for (int i = 0; i < 4; i++) h->_counts[i] = kl3_ns_get16(msg + 4 + 2 * i);
    const uint8_t *p = msg + 12;
    for (int i = 0; i < 4; i++) {
        if (h->_counts[i] == 0) { h->_sections[i] = NULL; continue; }
        h->_sections[i] = p;
        for (int r = 0; r < h->_counts[i]; r++) {
            if (kl3_ns_name_skip(&p, eom) < 0) { errno = EMSGSIZE; return -1; }
            if (p + 4 > eom) { errno = EMSGSIZE; return -1; }
            p += 4;
            if (i != 0) {
                if (p + 6 > eom) { errno = EMSGSIZE; return -1; }
                p += 6;
                uint16_t rdl = kl3_ns_get16(p - 2);
                if (p + rdl > eom) { errno = EMSGSIZE; return -1; }
                p += rdl;
            }
        }
    }
    h->_sect = -1; h->_rrnum = -1; h->_msg_ptr = NULL;
    return 0;
}
static int kl3_ns_parserr(kl_ns_msg *h, int section, int rrnum, kl_ns_rr *rr) {
    if (section < 0 || section >= 4) { errno = ENODEV; return -1; }
    if (section != h->_sect) { h->_sect = section; h->_rrnum = 0; h->_msg_ptr = h->_sections[section]; }
    if (rrnum == -1) rrnum = h->_rrnum;
    if (rrnum < 0 || rrnum >= h->_counts[section]) { errno = ENODEV; return -1; }
    if (rrnum < h->_rrnum) { h->_rrnum = 0; h->_msg_ptr = h->_sections[section]; }
    while (h->_rrnum < rrnum) {
        const uint8_t *p = h->_msg_ptr;
        if (kl3_ns_name_skip(&p, h->_eom) < 0 || p + 4 > h->_eom) { errno = EMSGSIZE; return -1; }
        p += 4;
        if (section != 0) { if (p + 6 > h->_eom) { errno = EMSGSIZE; return -1; } p += 6; p += kl3_ns_get16(p - 2); }
        h->_msg_ptr = p; h->_rrnum++;
    }
    const uint8_t *p = h->_msg_ptr;
    int n = kl3_ns_name_uncompress(h->_msg, h->_eom, p, rr->name, sizeof rr->name);
    if (n < 0) return -1;
    p += n;
    if (p + 4 > h->_eom) { errno = EMSGSIZE; return -1; }
    rr->type = kl3_ns_get16(p); rr->rr_class = kl3_ns_get16(p + 2); p += 4;
    if (section == 0) { rr->ttl = 0; rr->rdlength = 0; rr->rdata = NULL; }
    else {
        if (p + 6 > h->_eom) { errno = EMSGSIZE; return -1; }
        rr->ttl = kl3_ns_get32(p); rr->rdlength = kl3_ns_get16(p + 4); p += 6;
        if (p + rr->rdlength > h->_eom) { errno = EMSGSIZE; return -1; }
        rr->rdata = p; p += rr->rdlength;
    }
    h->_msg_ptr = p; h->_rrnum = rrnum + 1;
    return 0;
}
static int kl3_ns_skiprr(const uint8_t *ptr, const uint8_t *eom, int section, int count) {
    const uint8_t *p = ptr;
    while (count--) {
        if (kl3_ns_name_skip(&p, eom) < 0 || p + 4 > eom) { errno = EMSGSIZE; return -1; }
        p += 4;
        if (section != 0) { if (p + 6 > eom) { errno = EMSGSIZE; return -1; } p += 6; p += kl3_ns_get16(p - 2); if (p > eom) { errno = EMSGSIZE; return -1; } }
    }
    return (int)(p - ptr);
}
// ns_msg_getflag(handle, flag): `handle` is the struct by value (passed by reference in AAPCS64).
static int kl3_ns_msg_getflag(const kl_ns_msg *h, int flag) {
    static const struct { uint16_t mask; int shift; } f[] = { {0x8000, 15}, {0x7800, 11}, {0x0400, 10}, {0x0200, 9}, {0x0100, 8}, {0x0080, 7}, {0x0040, 6}, {0x0020, 5}, {0x0010, 4}, {0x000F, 0} };
    return flag >= 0 && flag < 10 ? (h->_flags & f[flag].mask) >> f[flag].shift : 0;
}
// base64, the resolver's copy
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int kl3_b64_ntop(const uint8_t *src, size_t n, char *dst, size_t tgt) {
    size_t o = 0;
    while (n >= 3) {
        if (o + 4 >= tgt) return -1;
        dst[o++] = b64c[src[0] >> 2]; dst[o++] = b64c[(src[0] & 3) << 4 | src[1] >> 4];
        dst[o++] = b64c[(src[1] & 15) << 2 | src[2] >> 6]; dst[o++] = b64c[src[2] & 63];
        src += 3; n -= 3;
    }
    if (n) {
        if (o + 4 >= tgt) return -1;
        dst[o++] = b64c[src[0] >> 2];
        if (n == 1) { dst[o++] = b64c[(src[0] & 3) << 4]; dst[o++] = '='; }
        else { dst[o++] = b64c[(src[0] & 3) << 4 | src[1] >> 4]; dst[o++] = b64c[(src[1] & 15) << 2]; }
        dst[o++] = '=';
    }
    if (o >= tgt) return -1;
    dst[o] = 0;
    return (int)o;
}
static int kl3_b64_pton(const char *src, uint8_t *dst, size_t tgt) {
    size_t o = 0; unsigned acc = 0; int bits = 0, pad = 0;
    for (; *src; src++) {
        if (isspace((unsigned char)*src)) continue;
        if (*src == '=') { pad++; continue; }
        const char *p = strchr(b64c, *src);
        if (!p || pad) return -1;
        acc = acc << 6 | (unsigned)(p - b64c); bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= tgt) return -1; dst[o++] = (uint8_t)(acc >> bits); }
    }
    return (int)o;
}

// ======================================================================
// signals by name, tty leftovers, small libc odds and ends
// ======================================================================
static const char *const g_sigtxt[32] = {
    "Unknown signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trap", "Aborted", "Bus error",
    "Floating point exception", "Killed", "User signal 1", "Segmentation fault", "User signal 2", "Broken pipe",
    "Alarm clock", "Terminated", "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped",
    "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition", "CPU time limit exceeded",
    "File size limit exceeded", "Virtual timer expired", "Profiling timer expired", "Window size changed",
    "I/O possible", "Power failure", "Bad system call",
};
static const char *const g_signame[32] = {
    "Unknown", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE",
    "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM",
    "PROF", "WINCH", "IO", "PWR", "SYS",
};
static const char *g_sys_signame[65];
static void signame_init(void) {
    static int done;
    if (done) return;
    for (int i = 0; i < 32; i++) g_sys_signame[i] = g_signame[i];
    g_sys_signame[32] = "RTMIN"; g_sys_signame[64] = "RTMAX";
    done = 1;
}
static int kl3_sig2str(int sig, char *str) {
    if (sig >= 1 && sig < 32) { strcpy(str, g_signame[sig]); return 0; }
    if (sig == 32) { strcpy(str, "RTMIN"); return 0; }
    if (sig == 64) { strcpy(str, "RTMAX"); return 0; }
    if (sig > 32 && sig < 64) {
        if (sig - 32 <= 16) sprintf(str, "RTMIN+%d", sig - 32); else sprintf(str, "RTMAX-%d", 64 - sig);
        return 0;
    }
    return -1;
}
static int kl3_str2sig(const char *str, int *num) {
    if (!str || !num) return -1;
    for (int i = 1; i < 32; i++) if (!strcmp(str, g_signame[i])) { *num = i; return 0; }
    if (!strcmp(str, "RTMIN")) { *num = 32; return 0; }
    if (!strcmp(str, "RTMAX")) { *num = 64; return 0; }
    int n;
    if (sscanf(str, "RTMIN+%d", &n) == 1 && n >= 1 && n <= 31) { *num = 32 + n; return 0; }
    if (sscanf(str, "RTMAX-%d", &n) == 1 && n >= 1 && n <= 31) { *num = 64 - n; return 0; }
    char *end; long v = strtol(str, &end, 10);
    if (*str && !*end && v >= 1 && v <= 64) { *num = (int)v; return 0; }
    return -1;
}
static void kl3_psignal(int sig, const char *msg) {
    char tmp[32];
    const char *t;
    if (sig >= 0 && sig < 32) t = g_sigtxt[sig];
    else if (sig >= 32 && sig <= 64) { snprintf(tmp, sizeof tmp, "Real-time signal %d", sig - 32); t = tmp; }
    else { snprintf(tmp, sizeof tmp, "Unknown signal %d", sig); t = tmp; }
    if (msg && *msg) fprintf(stderr, "%s: %s\n", msg, t); else fprintf(stderr, "%s\n", t);
}
// siginfo_t (Linux): { int si_signo, si_errno, si_code; ... }
static void kl3_psiginfo(const void *info, const char *msg) {
    if (!info) return;
    kl3_psignal(*(const int *)info, msg);
}

static char *kl3_ctermid(char *s) { static char b[] = "/dev/tty"; if (!s) return b; strcpy(s, b); return s; }
static char *kl3_ttyname(int fd) { (void)fd; errno = ENOTTY; return NULL; }
// getlogin_r returns an errno NUMBER (Linux): the name is the passwd entry of our uid.
static int kl3_getlogin_r(char *name, size_t size) {
    void *gp = kl_shim_lookup("getpwuid");
    if (!gp) return 6;                                               // ENXIO
    char **pw = ((char **(*)(unsigned))gp)((unsigned)getuid());
    if (!pw || !pw[0]) return 6;
    if (strlen(pw[0]) + 1 > size) return LX_ERANGE;
    strcpy(name, pw[0]);
    return 0;
}
static int kl3__tolower(int c) { return c - 'A' + 'a'; }
static int kl3__toupper(int c) { return c - 'a' + 'A'; }
static const char *kl3___gnu_basename(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void *kl3___mempcpy_chk(void *d, const void *s, size_t n, size_t dl) {
    if (n > dl) { fprintf(stderr, "FORTIFY: mempcpy: prevented write past end of buffer\n"); abort(); }
    return (char *)memcpy(d, s, n) + n;
}
static void kl3_android_reset_stack_guards(void) {}
static char *kl3_strerror_l(int e, void *loc) {
    (void)loc; static void *f; LAZY(f, "strerror");
    return f ? ((char *(*)(int))f)(e) : (char *)"Unknown error";
}
static char *kl3_strptime_l(const char *buf, const char *fmt, void *tm, void *loc) {
    (void)loc; static void *f; LAZY(f, "strptime");
    return f ? ((char *(*)(const char *, const char *, void *))f)(buf, fmt, tm) : NULL;
}
static size_t kl3_wcsftime_l(wchar_t *s, size_t n, const wchar_t *fmt, const void *tm, void *loc) {
    (void)loc; static void *f; LAZY(f, "wcsftime");
    return f ? ((size_t (*)(wchar_t *, size_t, const wchar_t *, const void *))f)(s, n, fmt, tm) : 0;
}
static kld kl3_wcstold_l(const wchar_t *s, wchar_t **end, void *loc) { (void)loc; return Q3(wcstod(s, end)); }
static time_t kl3_timelocal(struct tm *t) { return mktime(t); }

// ---- data symbols: the guest binds the ADDRESS ----
static long g_timezone_v; static int g_daylight_v;
static void tz_init(void) {
    static int done;
    if (done) return;
    done = 1;
    time_t now = time(NULL);
    struct tm a, b;
    struct tm *jan, *jul;
    time_t y = now - (now % 31536000);
    jan = localtime_r(&y, &a); jul = localtime_r(&(time_t){ y + 15552000 }, &b);
    if (!jan || !jul) return;
    long off_jan = a.tm_gmtoff, off_jul = b.tm_gmtoff;
    g_daylight_v = (a.tm_isdst > 0 || b.tm_isdst > 0) ? 1 : 0;
    g_timezone_v = -(off_jan < off_jul ? off_jan : off_jul);        // standard time is the smaller offset
}
static const char *g_progname_v = "app_process64";
static const struct in6_addr g_in6addr_loopback_v = IN6ADDR_LOOPBACK_INIT;
static void *g_hook_null;                                           // __malloc_hook & co. (never consulted)
static void *data_symbol(const char *name) {
    if (!strcmp(name, "optarg")) return &kl3_optarg;
    if (!strcmp(name, "optind")) return &kl3_optind;
    if (!strcmp(name, "opterr")) return &kl3_opterr;
    if (!strcmp(name, "optopt")) return &kl3_optopt;
    if (!strcmp(name, "optreset")) return &kl3_optreset;
    if (!strcmp(name, "signgam")) return &g_signgam;
    if (!strcmp(name, "timezone")) { tz_init(); return &g_timezone_v; }
    if (!strcmp(name, "daylight")) { tz_init(); return &g_daylight_v; }
    if (!strcmp(name, "__progname")) {
#ifndef KL_EXTRA3_TEST
        extern const char *klb_getprogname_for_err(void);
        g_progname_v = klb_getprogname_for_err();
#endif
        return &g_progname_v;
    }
    if (!strcmp(name, "in6addr_loopback")) return (void *)&g_in6addr_loopback_v;
    if (!strcmp(name, "sys_signame")) { signame_init(); return g_sys_signame; }
    if (!strcmp(name, "__malloc_hook") || !strcmp(name, "__realloc_hook") || !strcmp(name, "__free_hook") ||
        !strcmp(name, "__memalign_hook")) return &g_hook_null;
    return NULL;
}

// ======================================================================
// aliases: bionic's *64 spellings are the same call on LP64 (sigset64_t == sigset_t, off64_t ==
// off_t), and a few other names are another name for something already served.
// ======================================================================
static const struct { const char *name, *target; } g_alias3[] = {
    {"sigaction64", "sigaction"}, {"sigaddset64", "sigaddset"}, {"sigdelset64", "sigdelset"},
    {"sigemptyset64", "sigemptyset"}, {"sigfillset64", "sigfillset"}, {"sigismember64", "sigismember"},
    {"sigpending64", "sigpending"}, {"sigprocmask64", "sigprocmask"}, {"sigsuspend64", "sigsuspend"},
    {"sigtimedwait64", "sigtimedwait"}, {"sigwait64", "sigwait"}, {"sigwaitinfo64", "sigwaitinfo"},
    {"pthread_sigmask64", "pthread_sigmask"}, {"pselect64", "pselect"}, {"ppoll64", "ppoll"},
    {"signalfd64", "signalfd"}, {"preadv64", "preadv"}, {"pwritev64", "pwritev"},
    {"preadv64v2", "preadv2"}, {"pwritev64v2", "pwritev2"},
    {"mkstemp64", "mkstemp"}, {"mkostemp64", "mkostemp"}, {"mkostemps64", "mkostemps"}, {"mkstemps64", "mkstemps"},
    {"lockf64", "lockf"}, {"funopen64", "funopen"}, {"sendfile64", "sendfile"},
    {"readdir64_r", "readdir_r"}, {"scandirat64", "scandirat"},
    {"epoll_pwait64", "epoll_pwait"},
    {"posix_spawnattr_getsigdefault64", "posix_spawnattr_getsigdefault"}, {"posix_spawnattr_setsigdefault64", "posix_spawnattr_setsigdefault"},
    {"posix_spawnattr_getsigmask64", "posix_spawnattr_getsigmask"}, {"posix_spawnattr_setsigmask64", "posix_spawnattr_setsigmask"},
};

// ======================================================================
// termios helpers that touch only the struct (bionic's kernel-shaped termios: c_cflag holds the
// baud rate in CBAUD; there is no separate input speed), message catalogs, qsort_r, bindresvport
// ======================================================================
typedef struct { uint32_t c_iflag, c_oflag, c_cflag, c_lflag; uint8_t c_line; uint8_t c_cc[19]; } kl3_termios;
#define LXT_CBAUD 0x100Fu
static unsigned kl3_cfgetispeed(const kl3_termios *t) { return t->c_cflag & LXT_CBAUD; }
static unsigned kl3_cfgetospeed(const kl3_termios *t) { return t->c_cflag & LXT_CBAUD; }
static int kl3_cfsetspeed(kl3_termios *t, unsigned sp) { t->c_cflag = (t->c_cflag & ~LXT_CBAUD) | (sp & LXT_CBAUD); return 0; }
static int kl3_cfsetispeed(kl3_termios *t, unsigned sp) { return kl3_cfsetspeed(t, sp); }
static int kl3_cfsetospeed(kl3_termios *t, unsigned sp) { return kl3_cfsetspeed(t, sp); }
static void kl3_cfmakeraw(kl3_termios *t) {
    t->c_iflag &= ~(0x1u | 0x2u | 0x8u | 0x20u | 0x40u | 0x80u | 0x100u | 0x400u);      // IGNBRK BRKINT PARMRK ISTRIP INLCR IGNCR ICRNL IXON
    t->c_oflag &= ~0x1u;                                                                // OPOST
    t->c_lflag &= ~(0x8u | 0x40u | 0x2u | 0x1u | 0x8000u);                              // ECHO ECHONL ICANON ISIG IEXTEN
    t->c_cflag &= ~(0x30u | 0x100u);                                                    // CSIZE PARENB
    t->c_cflag |= 0x30u;                                                                // CS8
}
// <nl_types.h>: bionic's catalogs open a file; none exist for an app, and catgets then answers its default.
static void *kl3_catopen(const char *name, int flag) { (void)name; (void)flag; errno = ENOENT; return (void *)-1; }
static char *kl3_catgets(void *cat, int set, int msg, const char *dflt) { (void)cat; (void)set; (void)msg; return (char *)dflt; }
static int kl3_catclose(void *cat) { (void)cat; errno = EBADF; return -1; }
// qsort_r takes the context LAST in glibc/bionic; Darwin's takes it first.
struct kl3_qr { int (*cmp)(const void *, const void *, void *); void *arg; };
#ifdef __APPLE__
static int kl3_qr_tramp(void *t, const void *a, const void *b) { struct kl3_qr *q = t; return q->cmp(a, b, q->arg); }
static void kl3_qsort_r(void *base, size_t n, size_t w, int (*cmp)(const void *, const void *, void *), void *arg) {
    struct kl3_qr q = { cmp, arg };
    qsort_r(base, n, w, &q, kl3_qr_tramp);
}
#else
static void kl3_qsort_r(void *base, size_t n, size_t w, int (*cmp)(const void *, const void *, void *), void *arg) { qsort_r(base, n, w, cmp, arg); }
#endif
// Binding a reserved port needs privilege an app does not have.
static int kl3_bindresvport(int sd, void *sin) { (void)sd; (void)sin; errno = EACCES; return -1; }

// ======================================================================
// the table
// ======================================================================
#include <execinfo.h>
#include <setjmp.h>
#include <sys/mman.h>

static const kl3_entry g_extra3[] = {
    // ---- forwarded: Darwin's routine has bionic's signature and results ----
    X3(asctime), X3(asctime_r), X3(copysignf), X3(fabsf), X3(truncf), X3(fdim), X3(fdimf),
    X3(getloadavg), X3(getsubopt), X3(initstate), X3(setstate), X3(posix_madvise),
    X3(backtrace), X3(backtrace_symbols), X3(backtrace_symbols_fd), X3(_setjmp), X3(_longjmp),
    X3(cabs), X3(cabsf), X3(cacos), X3(cacosf), X3(cacosh), X3(cacoshf), X3(carg), X3(cargf),
    X3(casin), X3(casinf), X3(casinh), X3(casinhf), X3(catan), X3(catanf), X3(catanh), X3(catanhf),
    X3(ccos), X3(ccosf), X3(ccosh), X3(ccoshf), X3(cexp), X3(cexpf), X3(cimag), X3(cimagf),
    X3(clog), X3(clogf), X3(conj), X3(conjf), X3(cpow), X3(cpowf), X3(cproj), X3(cprojf),
    X3(creal), X3(crealf), X3(csin), X3(csinf), X3(csinh), X3(csinhf), X3(csqrt), X3(csqrtf),
    X3(ctan), X3(ctanf), X3(ctanh), X3(ctanhf),
    // ---- regex ----
    K3(regcomp), K3(regexec), K3(regerror), K3(regfree),
    // ---- search.h ----
    K3(tsearch), K3(tfind), K3(tdelete), K3(twalk), K3(tdestroy), K3(lsearch), K3(lfind), K3(insque), K3(remque),
    K3(hcreate), K3(hdestroy), K3(hsearch), K3(hcreate_r), K3(hdestroy_r), K3(hsearch_r),
    // ---- rand48 (srand48/lrand48 included: one shared state) ----
    K3(srand48), K3(seed48), K3(lcong48), K3(drand48), K3(lrand48), K3(mrand48), K3(erand48), K3(nrand48),
    K3(jrand48), K3(rand_r),
    // ---- getopt ----
    K3(getopt), K3(getopt_long), K3(getopt_long_only),
    // ---- iconv ----
    K3(iconv_open), K3(iconv_close), K3(iconv),
    // ---- C11 threads ----
    K3(mtx_init), K3(mtx_destroy), K3(mtx_lock), K3(mtx_trylock), K3(mtx_unlock), K3(mtx_timedlock),
    K3(cnd_init), K3(cnd_destroy), K3(cnd_signal), K3(cnd_broadcast), K3(cnd_wait), K3(cnd_timedwait),
    K3(thrd_create), K3(thrd_join), K3(thrd_detach), K3(thrd_current), K3(thrd_equal), K3(thrd_exit),
    K3(thrd_yield), K3(thrd_sleep), K3(tss_create), K3(tss_delete), K3(tss_get), K3(tss_set), K3(call_once),
    // ---- long double complex and leftovers ----
    K3(cacosl), K3(cacoshl), K3(casinl), K3(casinhl), K3(catanl), K3(catanhl), K3(ccosl), K3(ccoshl),
    K3(cexpl), K3(clogl), K3(conjl), K3(cprojl), K3(csinl), K3(csinhl), K3(csqrtl), K3(ctanl), K3(ctanhl),
    K3(cpowl), K3(cabsl), K3(cargl), K3(creall), K3(cimagl),
    K3(fabsl), K3(fdiml), K3(modfl), K3(significandl),
    K3(isfinite), K3(isfinitef), K3(isnormal), K3(isnormalf), K3(isfinitel), K3(isnormall),
    // ---- utmp / mntent ----
    K3(setutent), K3(endutent), K3(getutent), K3(pututline), K3(utmpname),
    K3(setmntent), K3(endmntent), K3(getmntent), K3(getmntent_r), K3(hasmntopt),
    // ---- netdb, offline ----
    K3(gethostbyname_r), K3(gethostbyname2_r), K3(gethostbyaddr_r), K3(gethostent), K3(getnetent),
    K3(getnetbyname), K3(getnetbyaddr), K3(getservent), K3(sethostent), K3(endhostent), K3(setnetent),
    K3(endnetent), K3(setservent), K3(endservent), K3(setprotoent), K3(endprotoent), K3(getprotoent),
    K3(getprotobyname), K3(getprotobynumber), K3(herror),
    K3(htonl), K3(htons), { "ntohl", (void *)kl3_htonl }, { "ntohs", (void *)kl3_htons },
    K3(inet_lnaof), K3(inet_netof), K3(inet_makeaddr), K3(inet_network),
    K3(ether_aton), K3(ether_aton_r), K3(ether_ntoa), K3(ether_ntoa_r),
    // ---- resolver ----
    K3(res_init), K3(res_query), K3(res_querydomain), K3(res_send), K3(res_mkquery),
    { "res_search", (void *)kl3_res_query }, { "__res_nsearch", (void *)kl3_res_nsearch },
    { "__res_ninit", (void *)kl3_res_ninit }, { "__res_nclose", (void *)kl3_res_nclose },
    { "__res_nquery", (void *)kl3_res_nquery }, { "__res_nquerydomain", (void *)kl3_res_nquerydomain },
    { "__res_nsend", (void *)kl3_res_nsend }, { "__res_nmkquery", (void *)kl3_res_nmkquery },
    { "__res_send", (void *)kl3_res_send }, { "__res_querydomain", (void *)kl3_res_querydomain },
    { "__res_close", (void *)kl3_res_close }, { "__res_randomid", (void *)kl3_res_randomid },
    { "__res_isourserver", (void *)kl3_res_isourserver }, { "__res_hostalias", (void *)kl3_res_hostalias },
    { "__res_hnok", (void *)kl3_res_hnok }, { "__res_dnok", (void *)kl3_res_dnok },
    { "__res_ownok", (void *)kl3_res_ownok }, { "__res_mailok", (void *)kl3_res_mailok },
    K3(ns_get16), K3(ns_get32), K3(ns_put16), K3(ns_put32), K3(ns_name_ntop), K3(ns_name_pton),
    K3(ns_name_unpack), K3(ns_name_uncompress), K3(ns_name_skip), K3(ns_name_pack), K3(ns_name_compress),
    K3(ns_samename), K3(ns_initparse), K3(ns_parserr), K3(ns_skiprr), K3(ns_msg_getflag),
    K3(dn_expand), { "__dn_skipname", (void *)kl3_dn_skipname }, { "__dn_comp", (void *)kl3_dn_comp },
    { "__dn_count_labels", (void *)kl3_dn_count_labels },
    { "_getshort", (void *)kl3_getshort }, { "_getlong", (void *)kl3_getlong },
    { "__putshort", (void *)kl3_putshort }, { "__putlong", (void *)kl3_putlong },
    { "__b64_ntop", (void *)kl3_b64_ntop }, { "__b64_pton", (void *)kl3_b64_pton },
    // ---- signals by name, tty, odds and ends ----
    K3(sig2str), K3(str2sig), K3(psignal), K3(psiginfo), K3(ctermid), K3(ttyname), K3(getlogin_r),
    K3(_tolower), K3(_toupper), K3(__gnu_basename), K3(__mempcpy_chk), K3(android_reset_stack_guards),
    K3(strerror_l), K3(strptime_l), K3(wcsftime_l), K3(wcstold_l), K3(timelocal),
    K3(cfgetispeed), K3(cfgetospeed), K3(cfsetispeed), K3(cfsetospeed), K3(cfsetspeed), K3(cfmakeraw),
    K3(catopen), K3(catgets), K3(catclose), K3(qsort_r), K3(bindresvport), X3(duplocale),
};

void *kl_bionic_extra4_lookup(const char *name);            // kl_bionic_extra4.c (Darwin only)

void *kl_bionic_extra3_lookup(const char *name) {
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < sizeof g_extra3 / sizeof g_extra3[0]; i++)
        if (g_extra3[i].name[0] == name[0] && !strcmp(g_extra3[i].name, name)) return g_extra3[i].fn;
    void *d = data_symbol(name);
    if (d) return d;
#ifndef KL_EXTRA3_TEST
    void *f = kl_bionic_extra4_lookup(name);
    if (f) return f;
#endif
    for (size_t i = 0; i < sizeof g_alias3 / sizeof g_alias3[0]; i++)
        if (!strcmp(g_alias3[i].name, name)) {
            void *t = kl_bionic_extra3_lookup(g_alias3[i].target);          // the target may live here...
            return t ? t : kl_shim_lookup(g_alias3[i].target);              // ...or in the generated table
        }
    return NULL;
}
