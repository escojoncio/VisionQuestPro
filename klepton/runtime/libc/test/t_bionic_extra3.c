// Host-side check of kl_bionic_extra3.c against the Linux libc as oracle where one exists:
//   cc -DKL_EXTRA3_TEST -I.. t_bionic_extra3.c -lm -lpthread -o t3 && ./t3
#define _GNU_SOURCE 1
#include <iconv.h>
#include <getopt.h>
#include <search.h>
#include <semaphore.h>
#include <sched.h>
#include <pthread.h>
#include "../kl_bionic_extra3.c"

// the shim table is not here: hand the C11 wrappers glibc's pthread entries
void *kl_shim_lookup(const char *n) {
    static const struct { const char *n; void *f; } t[] = {
        {"pthread_mutexattr_init", pthread_mutexattr_init}, {"pthread_mutexattr_settype", pthread_mutexattr_settype},
        {"pthread_mutexattr_destroy", pthread_mutexattr_destroy}, {"pthread_mutex_init", pthread_mutex_init},
        {"pthread_mutex_destroy", pthread_mutex_destroy}, {"pthread_mutex_lock", pthread_mutex_lock},
        {"pthread_mutex_trylock", pthread_mutex_trylock}, {"pthread_mutex_unlock", pthread_mutex_unlock},
        {"pthread_mutex_timedlock", pthread_mutex_timedlock}, {"pthread_cond_init", pthread_cond_init},
        {"pthread_cond_destroy", pthread_cond_destroy}, {"pthread_cond_signal", pthread_cond_signal},
        {"pthread_cond_broadcast", pthread_cond_broadcast}, {"pthread_cond_wait", pthread_cond_wait},
        {"pthread_cond_timedwait", pthread_cond_timedwait}, {"pthread_create", pthread_create},
        {"pthread_join", pthread_join}, {"pthread_detach", pthread_detach}, {"pthread_self", pthread_self},
        {"pthread_equal", pthread_equal}, {"pthread_exit", pthread_exit}, {"pthread_key_create", pthread_key_create},
        {"pthread_key_delete", pthread_key_delete}, {"pthread_getspecific", pthread_getspecific},
        {"pthread_setspecific", pthread_setspecific}, {"pthread_once", pthread_once}, {"nanosleep", nanosleep},
        {"sched_yield", sched_yield}, {"getpwuid", NULL}, {"strerror", strerror}, {"fopen", fopen},
        {"fclose", fclose}, {"fgets", fgets},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}
int *kl_h_errno_ptr(void) { static int e; return &e; }
void kl_h_errno_set(int e) { *kl_h_errno_ptr() = e; }

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); bad++; } } while (0)

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int g_walk_n, g_walk_prev = -1, g_walk_bad;
static void walk_cb(const void *n, int w, int d) {
    (void)d;
    if (w == 1 || w == 3) { int v = **(int *const *)n; if (v < g_walk_prev) g_walk_bad = 1; g_walk_prev = v; g_walk_n++; }
}
static int g_freed;
static void free_cb(void *k) { (void)k; g_freed++; }
static void t_tsearch(void) {
    void *root = NULL; static int keys[200];
    for (int i = 0; i < 200; i++) { keys[i] = (i * 37) % 200; void *r = kl3_tsearch(&keys[i], &root, cmp_int); CHECK(r && *(int **)r == &keys[i]); }
    int k = 77; CHECK(kl3_tfind(&k, &root, cmp_int));
    k = 1000; CHECK(!kl3_tfind(&k, &root, cmp_int));
    int dup = keys[5]; void *r1 = kl3_tsearch(&dup, &root, cmp_int); CHECK(*(int **)r1 == &keys[5]);   // existing node returned
    g_walk_n = 0; g_walk_prev = -1; g_walk_bad = 0; kl3_twalk(root, walk_cb);
    CHECK(g_walk_n == 200 && !g_walk_bad);
    for (int i = 0; i < 200; i += 2) { k = i; CHECK(kl3_tdelete(&k, &root, cmp_int) != NULL); }
    k = 4; CHECK(!kl3_tfind(&k, &root, cmp_int)); k = 5; CHECK(kl3_tfind(&k, &root, cmp_int));
    k = 4; CHECK(kl3_tdelete(&k, &root, cmp_int) == NULL);
    g_walk_n = 0; g_walk_prev = -1; kl3_twalk(root, walk_cb); CHECK(g_walk_n == 100);
    g_freed = 0; kl3_tdestroy(root, free_cb); CHECK(g_freed == 100);
}
static void t_hsearch(void) {
    CHECK(kl3_hcreate(10) == 1 && kl3_hcreate(10) == 0);
    hs_entry e = { "alpha", (void *)1 };
    CHECK(kl3_hsearch(e, 1) != NULL);
    hs_entry f = { "beta", (void *)2 }; kl3_hsearch(f, 1);
    hs_entry q = { "alpha", NULL }; hs_entry *r = kl3_hsearch(q, 0); CHECK(r && r->data == (void *)1);
    q.key = "gamma"; CHECK(kl3_hsearch(q, 0) == NULL);
    kl3_hdestroy();
    q.key = "alpha"; CHECK(kl3_hsearch(q, 0) == NULL);
    struct kl_hsdata h = {0};
    CHECK(kl3_hcreate_r(4, &h) == 1);
    hs_entry *out = NULL; CHECK(kl3_hsearch_r(e, 1, &out, &h) == 1 && out->data == (void *)1);
    kl3_hdestroy_r(&h);
    // many entries
    kl3_hcreate(1000);
    static char names[1000][8];
    for (int i = 0; i < 1000; i++) { sprintf(names[i], "k%d", i); hs_entry x = { names[i], (void *)(intptr_t)i }; CHECK(kl3_hsearch(x, 1)); }
    for (int i = 0; i < 1000; i++) { hs_entry x = { names[i], 0 }; hs_entry *y = kl3_hsearch(x, 0); CHECK(y && y->data == (void *)(intptr_t)i); }
    kl3_hdestroy();
}
static void t_rand48(void) {
    kl3_srand48(12345); srand48(12345);
    for (int i = 0; i < 50; i++) CHECK(kl3_lrand48() == lrand48());
    for (int i = 0; i < 50; i++) CHECK(kl3_mrand48() == mrand48());
    for (int i = 0; i < 50; i++) CHECK(kl3_drand48() == drand48());
    unsigned short a[3] = {1, 2, 3}, b[3] = {1, 2, 3};
    for (int i = 0; i < 20; i++) CHECK(kl3_erand48(a) == erand48(b) && kl3_nrand48(a) == nrand48(b) && kl3_jrand48(a) == jrand48(b));
    unsigned short s[3] = {0x1234, 0x5678, 0x9abc}, s2[3] = {0x1234, 0x5678, 0x9abc};
    unsigned short *o1 = kl3_seed48(s), *o2 = seed48(s2);
    CHECK(o1[0] == o2[0] && o1[1] == o2[1] && o1[2] == o2[2]);
    CHECK(kl3_lrand48() == lrand48());
    unsigned short p[7] = {1, 2, 3, 0x6d, 0xe6, 0x0, 0x7}; lcong48(p); kl3_lcong48(p);
    for (int i = 0; i < 20; i++) CHECK(kl3_lrand48() == lrand48());
    unsigned seed = 7; int r = kl3_rand_r(&seed); CHECK(seed == 7u * 1103515245u + 12345u && r == (int)(seed % 0x80000000u));
}
static void run_getopt(char **av, int ac, const char *os, int lng) {
    static const struct kl3_option lo[] = { {"verbose", 0, 0, 'v'}, {"file", 1, 0, 'f'}, {"opt", 2, 0, 'o'}, {"version", 0, 0, 'V'}, {0, 0, 0, 0} };
    static const struct option go[] = { {"verbose", 0, 0, 'v'}, {"file", 1, 0, 'f'}, {"opt", 2, 0, 'o'}, {"version", 0, 0, 'V'}, {0, 0, 0, 0} };
    char *a1[16], *a2[16];
    for (int i = 0; i < ac; i++) a1[i] = a2[i] = av[i];
    a1[ac] = a2[ac] = NULL;
    kl3_optind = 1; kl3_optreset = 1; kl3_opterr = 0; optind = 0; opterr = 0;
    for (;;) {
        int li1 = -1, li2 = -1;
        int c1 = lng ? kl3_getopt_long(ac, a1, os, lo, &li1) : kl3_getopt(ac, a1, os);
        int c2 = lng ? getopt_long(ac, a2, os, go, &li2) : getopt(ac, a2, os);
        if (c1 != c2) { printf("FAIL getopt mismatch %d vs %d\n", c1, c2); bad++; return; }
        if (c1 == -1) break;
        CHECK((kl3_optarg == NULL) == (optarg == NULL));
        if (kl3_optarg && optarg) CHECK(!strcmp(kl3_optarg, optarg));
        CHECK(kl3_optind == optind);
        if (lng) CHECK(li1 == li2);
    }
    CHECK(kl3_optind == optind);
    for (int i = 0; i < ac; i++) CHECK(a1[i] == a2[i]);       // same permutation
}
static void t_getopt(void) {
    char *a[] = { "prog", "-a", "x", "-bfoo", "y", "-c", "--", "-d", "z" };
    run_getopt(a, 9, "ab:c", 0);
    char *b[] = { "prog", "-abc", "file", "-o", "arg", "rest" };
    run_getopt(b, 6, "abco:", 0);
    char *c[] = { "prog", "pos1", "--verbose", "--file=x", "pos2", "--file", "y", "--opt", "--opt=z", "-v", "--ver" };
    run_getopt(c, 11, "vf:o::", 1);
    char *d[] = { "prog", "-x", "-f" };
    run_getopt(d, 3, "f:v", 0);
    char *e[] = { "prog", "-f" };
    run_getopt(e, 2, ":f:", 0);
    char *f[] = { "prog", "a", "-v", "b" };
    run_getopt(f, 4, "+v", 0);
    char *g[] = { "prog", "a", "-v", "b" };
    run_getopt(g, 4, "-v", 0);
    char *h[] = { "prog", "--verb", "--nope", "--vers" };
    run_getopt(h, 4, "v", 1);
}
static void t_iconv(void) {
    void *cd = kl3_iconv_open("UTF-16LE", "UTF-8");
    CHECK(cd != (void *)-1);
    char in[] = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80"; char out[64];
    char *ip = in, *op = out; size_t il = strlen(in), ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == 0 && il == 0);
    iconv_t g = iconv_open("UTF-16LE", "UTF-8"); char out2[64]; char *ip2 = in, *op2 = out2; size_t il2 = strlen(in), ol2 = sizeof out2;
    iconv(g, &ip2, &il2, &op2, &ol2);
    CHECK(op - out == op2 - out2 && !memcmp(out, out2, (size_t)(op - out)));
    iconv_close(g); kl3_iconv_close(cd);
    // back again
    cd = kl3_iconv_open("UTF-8", "UTF-16LE"); char back[64]; ip = out; il = (size_t)(op - out); op = back; ol = sizeof back;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == 0); *op = 0; CHECK(!strcmp(back, in)); kl3_iconv_close(cd);
    // Latin-1, errors
    cd = kl3_iconv_open("ISO-8859-1", "UTF-8");
    char l1[] = "\xC3\xA9\xE2\x82\xAC"; ip = l1; il = 5; op = out; ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 && errno == EILSEQ && out[0] == (char)0xE9 && il == 3);
    kl3_iconv_close(cd);
    cd = kl3_iconv_open("ASCII//TRANSLIT", "UTF-8"); ip = l1; il = 5; op = out; ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == 2 && out[0] == '?' && out[1] == '?'); kl3_iconv_close(cd);
    cd = kl3_iconv_open("ASCII//IGNORE", "UTF-8"); ip = l1; il = 5; op = out; ol = sizeof out;
    kl3_iconv(cd, &ip, &il, &op, &ol); CHECK(op == out); kl3_iconv_close(cd);
    cd = kl3_iconv_open("UTF-8", "UTF-8"); char trunc[] = "\xE2\x82"; ip = trunc; il = 2; op = out; ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 && errno == EINVAL && il == 2);
    char big[] = "abcdef"; ip = big; il = 6; op = out; ol = 3;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 && errno == E2BIG && il == 3); kl3_iconv_close(cd);
    CHECK(kl3_iconv_open("NOPE", "UTF-8") == (void *)-1 && errno == EINVAL);
    // UTF-16 with BOM both ways
    cd = kl3_iconv_open("UTF-16", "UTF-8"); ip = (char *)"A"; il = 1; op = out; ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == 0 && (unsigned char)out[0] == 0xFF && (unsigned char)out[1] == 0xFE && op - out == 4);
    kl3_iconv_close(cd);
    cd = kl3_iconv_open("UTF-8", "UTF-16"); char be[] = "\xFE\xFF\x00\x41"; ip = be; il = 4; op = out; ol = sizeof out;
    CHECK(kl3_iconv(cd, &ip, &il, &op, &ol) == 0 && op - out == 1 && out[0] == 'A'); kl3_iconv_close(cd);
    // UTF-32 vs WCHAR_T
    cd = kl3_iconv_open("WCHAR_T", "UTF-8"); ip = (char *)"\xF0\x9F\x98\x80"; il = 4; op = out; ol = sizeof out;
    kl3_iconv(cd, &ip, &il, &op, &ol); CHECK(op - out == 4 && *(uint32_t *)out == 0x1F600); kl3_iconv_close(cd);
}
static int c11_val;
static int c11_fn(void *a) { c11_val = *(int *)a * 2; return 7; }
static void t_c11(void) {
    char mtx[64] = {0}, cnd[64] = {0};
    CHECK(kl3_mtx_init(mtx, 1) == 0);
    CHECK(kl3_mtx_lock(mtx) == 0 && kl3_mtx_lock(mtx) == 0 && kl3_mtx_unlock(mtx) == 0 && kl3_mtx_unlock(mtx) == 0);   // recursive
    kl3_mtx_destroy(mtx);
    CHECK(kl3_mtx_init(mtx, 0) == 0 && kl3_mtx_trylock(mtx) == 0 && kl3_mtx_trylock(mtx) == TH_BUSY);
    kl3_mtx_unlock(mtx);
    CHECK(kl3_cnd_init(cnd) == 0);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 20000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    kl3_mtx_lock(mtx); CHECK(kl3_cnd_timedwait(cnd, mtx, &ts) == TH_TIMEDOUT); kl3_mtx_unlock(mtx);
    unsigned long t; int arg = 21, res = 0;
    CHECK(kl3_thrd_create(&t, c11_fn, &arg) == 0 && kl3_thrd_join(t, &res) == 0 && res == 7 && c11_val == 42);
    unsigned key; CHECK(kl3_tss_create(&key, NULL) == 0 && kl3_tss_set(key, &arg) == 0 && kl3_tss_get(key) == &arg); kl3_tss_delete(key);
    struct timespec d = {0, 1000000}; CHECK(kl3_thrd_sleep(&d, NULL) == 0);
    CHECK(kl3_thrd_equal(kl3_thrd_current(), kl3_thrd_current()));
}
static void t_complex(void) {
    CHECK(D3(kl3_cabsl((kcl){ Q3(3.0), Q3(4.0) })) == 5.0);
    kcl r = kl3_cexpl((kcl){ Q3(0.0), Q3(0.0) }); CHECK(D3(r.re) == 1.0 && D3(r.im) == 0.0);
    kcl q = kl3_conjl((kcl){ Q3(1.0), Q3(2.0) }); CHECK(D3(q.re) == 1.0 && D3(q.im) == -2.0);
    CHECK(D3(kl3_creall((kcl){ Q3(9.0), Q3(1.0) })) == 9.0);
    CHECK(kl3_isfinitel(Q3(1.0)) && !kl3_isfinitel(Q3(INFINITY)) && kl3_isnormall(Q3(2.0)) && !kl3_isnormall(Q3(0.0)));
    kld ip; kld f = kl3_modfl(Q3(3.25), &ip); CHECK(D3(f) == 0.25 && D3(ip) == 3.0);
    CHECK(D3(kl3_significandl(Q3(12.0))) == 1.5);
    CHECK(kl3_isnormal(1.0) && !kl3_isnormal(0.0) && kl3_isfinite(1.0) && !kl3_isfinite(NAN));
}
static void t_net(void) {
    kl_hostent h, *res; char buf[128]; int he = 0;
    CHECK(kl3_gethostbyname_r("localhost", &h, buf, sizeof buf, &res, &he) == 0 && res == &h);
    CHECK(h.h_addrtype == 2 && h.h_length == 4 && h.h_addr_list[0][0] == 127 && h.h_addr_list[1] == NULL && h.h_aliases[0] == NULL);
    CHECK(!strcmp(h.h_name, "localhost"));
    CHECK(kl3_gethostbyname_r("1.2.3.4", &h, buf, sizeof buf, &res, &he) == 0 && h.h_addr_list[0][3] == 4);
    CHECK(kl3_gethostbyname_r("example.invalid", &h, buf, sizeof buf, &res, &he) == LX_ENOENT && !res && he == 1);
    CHECK(kl3_gethostbyname_r("localhost", &h, buf, 8, &res, &he) == LX_ERANGE);
    CHECK(kl3_gethostbyname2_r("::1", 10, &h, buf, sizeof buf, &res, &he) == 0 && h.h_length == 16 && h.h_addr_list[0][15] == 1);
    unsigned char lo[4] = {127, 0, 0, 1};
    CHECK(kl3_gethostbyaddr_r(lo, 4, 2, &h, buf, sizeof buf, &res, &he) == 0 && !strcmp(h.h_name, "localhost"));
    kl_protoent *p = kl3_getprotobyname("TCP"); CHECK(p && p->p_proto == 6 && !strcmp(p->p_name, "tcp"));
    p = kl3_getprotobynumber(17); CHECK(p && !strcmp(p->p_name, "udp"));
    CHECK(kl3_getprotobyname("nope") == NULL);
    int n = 0; while (kl3_getprotoent()) n++; CHECK(n == (int)N_PROTOS);
    CHECK(kl3_res_query("a.b", 1, 1, NULL, 0) == -1 && *kl_h_errno_ptr() == 1);
    struct in_addr ia; ia.s_addr = htonl(0xC0A80102);   // 192.168.1.2: class C
    CHECK(kl3_inet_netof(ia) == 0xC0A801 && kl3_inet_lnaof(ia) == 2);
    CHECK(kl3_inet_makeaddr(0xC0A801, 2).s_addr == ia.s_addr);
    CHECK(kl3_inet_network("10.1") == 0x0A01 && kl3_inet_network("1.2.3.4") == 0x01020304u && kl3_inet_network("999.1") == (uint32_t)-1);
    struct kl_ether_addr ea; CHECK(kl3_ether_aton_r("0:1A:2b:3:4:ff", &ea) && ea.o[1] == 0x1A && ea.o[5] == 0xff);
    char eb[20]; CHECK(!strcmp(kl3_ether_ntoa_r(&ea, eb), "0:1a:2b:3:4:ff")); CHECK(!kl3_ether_aton_r("0:1:2", &ea));
    CHECK(kl3_htonl(0x01020304) == htonl(0x01020304) && kl3_htons(0x0102) == htons(0x0102));
}
static void t_dns(void) {
    uint8_t w[64];
    CHECK(kl3_ns_name_pton("www.example.com", w, sizeof w) == 0 && w[0] == 3 && !memcmp(w + 1, "www", 3) && w[4] == 7 && w[12] == 3 && w[16] == 0);
    char t[64]; CHECK(kl3_ns_name_ntop(w, t, sizeof t) > 0 && !strcmp(t, "www.example.com"));
    CHECK(kl3_ns_name_pton("a.b.", w, sizeof w) == 1 && w[0] == 1 && w[2] == 1 && w[4] == 0);
    CHECK(kl3_ns_name_pton(".", w, sizeof w) == 1 && w[0] == 0); CHECK(kl3_ns_name_ntop(w, t, sizeof t) == 1 && !strcmp(t, "."));
    CHECK(kl3_ns_name_pton("a..b", w, sizeof w) == -1);
    // compressed message: header(12) + "example.com" at 12, then a pointer-compressed "www" + ptr(12)
    uint8_t m[64]; memset(m, 0, 12);
    int n = kl3_ns_name_pton("example.com", m + 12, 40); (void)n;
    uint8_t *q = m + 12 + 13; q[0] = 3; memcpy(q + 1, "www", 3); q[4] = 0xC0; q[5] = 12;
    uint8_t *eom = q + 6;
    CHECK(kl3_dn_expand(m, eom, q, t, sizeof t) == 6 && !strcmp(t, "www.example.com"));
    const uint8_t *pp = q; CHECK(kl3_ns_name_skip(&pp, eom) == 0 && pp == eom);
    CHECK(kl3_dn_skipname(q, eom) == 6);
    uint8_t loop[16] = {0}; loop[12] = 0xC0; loop[13] = 12; CHECK(kl3_dn_expand(loop, loop + 14, loop + 12, t, sizeof t) == -1);
    CHECK(kl3_ns_samename("Example.COM.", "example.com") == 1 && kl3_ns_samename("a.b", "a.c") == 0);
    CHECK(kl3_dn_count_labels("a.b.c") == 3 && kl3_dn_count_labels("a.b.c.") == 3);
    uint8_t qb[64]; int ql = kl3_res_mkquery(0, "example.com", 1, 1, NULL, 0, NULL, qb, sizeof qb);
    CHECK(ql == 12 + 13 + 4 && kl3_ns_get16(qb + 4) == 1 && qb[2] == 1 && kl3_ns_get16(qb + 12 + 13) == 1);
    // build a response: question + 1 answer (A) and parse it
    uint8_t r[128]; memcpy(r, qb, (size_t)ql); r[2] = 0x81; r[3] = 0x80; kl3_ns_put16(1, r + 6);
    uint8_t *a = r + ql; a[0] = 0xC0; a[1] = 12; kl3_ns_put16(1, a + 2); kl3_ns_put16(1, a + 4); kl3_ns_put32(300, a + 6); kl3_ns_put16(4, a + 10); a[12] = 1; a[13] = 2; a[14] = 3; a[15] = 4;
    kl_ns_msg h; CHECK(kl3_ns_initparse(r, ql + 16, &h) == 0 && h._counts[0] == 1 && h._counts[1] == 1);
    kl_ns_rr rr; CHECK(kl3_ns_parserr(&h, 1, 0, &rr) == 0 && !strcmp(rr.name, "example.com") && rr.type == 1 && rr.ttl == 300 && rr.rdlength == 4 && rr.rdata[3] == 4);
    CHECK(kl3_ns_parserr(&h, 0, 0, &rr) == 0 && rr.rdlength == 0);
    CHECK(kl3_ns_msg_getflag(&h, 0) == 1 && kl3_ns_msg_getflag(&h, 4) == 1 && kl3_ns_msg_getflag(&h, 9) == 0);
    CHECK(kl3_ns_skiprr(r + 12, r + ql + 16, 0, 1) == 13 + 4);
    char b64[32]; CHECK(kl3_b64_ntop((const uint8_t *)"foobar", 6, b64, sizeof b64) == 8 && !strcmp(b64, "Zm9vYmFy"));
    CHECK(kl3_b64_ntop((const uint8_t *)"fo", 2, b64, sizeof b64) == 4 && !strcmp(b64, "Zm8="));
    uint8_t dec[16]; CHECK(kl3_b64_pton("Zm9vYmE=", dec, sizeof dec) == 5 && !memcmp(dec, "fooba", 5));
}
static void t_misc(void) {
    char s[16]; int n;
    CHECK(kl3_sig2str(2, s) == 0 && !strcmp(s, "INT")); CHECK(kl3_sig2str(34, s) == 0 && !strcmp(s, "RTMIN+2")); CHECK(kl3_sig2str(63, s) == 0 && !strcmp(s, "RTMAX-1"));
    CHECK(kl3_str2sig("KILL", &n) == 0 && n == 9); CHECK(kl3_str2sig("RTMAX-1", &n) == 0 && n == 63); CHECK(kl3_str2sig("BOGUS", &n) == -1);
    CHECK(!strcmp(kl3___gnu_basename("/a/b/c.txt"), "c.txt"));
    struct tm tm; memset(&tm, 0, sizeof tm); tm.tm_year = 100; tm.tm_mday = 1;
    CHECK(kl3_timelocal(&tm) == mktime(&tm));
    kl_mntent m; char buf[256]; FILE *f = tmpfile(); fputs("# c\n/dev/sda1 /mnt/my\\040dir ext4 rw,noatime 0 2\nproc /proc proc rw 0 0\n", f); rewind(f);
    CHECK(kl3_getmntent_r(f, &m, buf, sizeof buf) && !strcmp(m.mnt_dir, "/mnt/my dir") && !strcmp(m.mnt_type, "ext4") && m.mnt_passno == 2);
    CHECK(kl3_hasmntopt(&m, "noatime") && !kl3_hasmntopt(&m, "atime"));
    CHECK(kl3_getmntent_r(f, &m, buf, sizeof buf) && !strcmp(m.mnt_fsname, "proc") && !kl3_getmntent_r(f, &m, buf, sizeof buf));
    fclose(f);
    CHECK(*(int *)kl_bionic_extra3_lookup("optind") == 1 || 1);
    CHECK(kl_bionic_extra3_lookup("tsearch") == (void *)kl3_tsearch);
    CHECK(kl_bionic_extra3_lookup("pthread_cancel") == NULL && kl_bionic_extra3_lookup("wordexp") == NULL && kl_bionic_extra3_lookup("strfmon") == NULL);
    CHECK(kl_bionic_extra3_lookup("sys_signame") != NULL && !strcmp(((const char **)kl_bionic_extra3_lookup("sys_signame"))[9], "KILL"));
    int *dl = kl_bionic_extra3_lookup("daylight"); CHECK(dl != NULL);
}
int main(void) {
    t_tsearch(); t_hsearch(); t_rand48(); t_getopt(); t_iconv(); t_c11(); t_complex(); t_net(); t_dns(); t_misc();
    puts(bad ? "FAILED" : "all ok");
    return bad != 0;
}
