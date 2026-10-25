// Host test (Linux or macOS) of the Linux-semantics fixes in the libc/pthread shims. The code under test is cut out of
// the shipping sources by KL-TEST markers, so the test runs the real functions:
//   kl_libc.c    affinity (sched_getaffinity mask), madvclip (MADV_DONTNEED head/interior/tail split)
//   kl_pthread.c pthmap (mutex/cond address maps, tombstones, destroy that frees the host object)
//   kl_shim.c    mallocx (jemalloc alignment/zero flags)
//   make libcsem        (or tools/ci/host-tests.sh)
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#define malloc_size(p) malloc_usable_size((void *)(p))
#define malloc_good_size(n) ((n) <= 24 ? (size_t)24 : (((n) + 8 + 15) & ~(size_t)15) - 8)   /* glibc chunk rounding */
#endif

#include "libc_blocks.inc"
#include "shim_blocks.inc"

// Count live host mutex/cond blocks: a destroy that frees nothing is the leak under test.
static _Atomic long g_live;
static void *t_malloc(size_t n) { void *p = malloc(n); if (p) atomic_fetch_add(&g_live, 1); return p; }
static void t_free(void *p) { if (p) atomic_fetch_sub(&g_live, 1); free(p); }
#define malloc t_malloc
#define free t_free
#define mtx_log(...) ((void)0)
#include "pthread_blocks.inc"
#undef malloc
#undef free

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int popcount_bytes(const unsigned char *b, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n; i++) c += __builtin_popcount(b[i]);
    return c;
}

static void test_affinity(void) {
    static const long cpus[] = { 1, 7, 10, 64, 65, 130 };
    for (unsigned k = 0; k < sizeof cpus / sizeof cpus[0]; k++) {
        unsigned char m[128];
        memset(m, 0xFF, sizeof m);
        size_t need = kl_affinity_fill(m, sizeof m, cpus[k]);
        CHECK(popcount_bytes(m, sizeof m) == cpus[k], "affinity: popcount over 128 bytes is the cpu count");
        int ok = 1;
        for (long i = 0; i < 1024; i++) ok &= (((m[i / 8] >> (i % 8)) & 1) == (i < cpus[k]));
        CHECK(ok, "affinity: bits [0,n) set, the rest clear");
        CHECK(need == (size_t)((cpus[k] + 63) / 64) * 8, "affinity: kernel mask size");
    }
    unsigned char s[8];
    memset(s, 0xFF, sizeof s);
    kl_affinity_fill(s, sizeof s, 100);
    CHECK(popcount_bytes(s, sizeof s) == 64, "affinity: capped by the buffer");
    CHECK(kl_affinity_fill(NULL, 0, 10) == 8, "affinity: NULL buffer only reports the size");
}

static void test_madvclip(void) {
    int ok = 1;
    for (uintptr_t s = 0x100000; s < 0x100000 + 3 * 16384; s += 4)
        for (size_t n = 1; n < 4 * 16384 + 8; n += (n < 64 ? 1 : 997)) {
            uintptr_t lo, hi, e = s + n;
            kl_madv_clip(s, n, &lo, &hi);
            // head [s,lo), interior [lo,hi), tail [hi,e) partition [s,e) exactly
            ok &= lo >= s && lo <= hi && hi <= e;
            ok &= (lo & 16383) == 0 || lo == e;          // interior starts on a host page (or there is none)
            ok &= (hi & 16383) == 0 || hi == e;
            ok &= lo - s < 16384 || lo == hi;            // head is less than a page unless it is everything
            if (hi > lo) ok &= (lo & 16383) == 0 && (hi & 16383) == 0 && e - hi < 16384;
            else ok &= lo == e && hi == e;               // no whole page: the head is the whole range
        }
    CHECK(ok, "madvclip: head + whole pages + tail partition the range");
    uintptr_t lo, hi;
    kl_madv_clip(0x104000, 0x4000, &lo, &hi);
    CHECK(lo == 0x104000 && hi == 0x108000, "madvclip: aligned range is all interior");
    kl_madv_clip(0x101000, 0x1000, &lo, &hi);
    CHECK(lo == 0x102000 && hi == 0x102000, "madvclip: 4 KiB inside a page has no interior");
    kl_madv_clip(0x103000, 0x6000, &lo, &hi);
    CHECK(lo == 0x104000 && hi == 0x108000, "madvclip: unaligned both ends keeps the inner pages");
}

static void test_mallocx(void) {
    for (int lg = 0; lg <= 16; lg++)
        for (size_t n = 1; n < 5000; n += 313) {
            void *p = kl_mallocx(n, lg | 0x40);
            CHECK(p != NULL, "mallocx: allocates");
            if (lg) CHECK(((uintptr_t)p & (((uintptr_t)1 << lg) - 1)) == 0, "mallocx: MALLOCX_LG_ALIGN honoured");
            int z = 1;
            for (size_t i = 0; i < n; i++) z &= ((unsigned char *)p)[i] == 0;
            CHECK(z, "mallocx: MALLOCX_ZERO honoured");
            if (lg) CHECK(kl_nallocx(n, lg) <= kl_sallocx(p, lg), "mallocx: aligned nallocx never exceeds the usable size");
            memset(p, 0xAB, n);
            size_t old = kl_sallocx(p, lg), nsz = n * 3 + 7;
            unsigned char *q = kl_rallocx(p, nsz, lg | 0x40);
            CHECK(q != NULL, "rallocx: allocates");
            if (lg) CHECK(((uintptr_t)q & (((uintptr_t)1 << lg) - 1)) == 0, "rallocx: alignment kept");
            int keep = 1, tail = 1;
            for (size_t i = 0; i < n; i++) keep &= q[i] == 0xAB;
            for (size_t i = old; i < nsz; i++) tail &= q[i] == 0;
            CHECK(keep, "rallocx: contents kept");
            CHECK(tail, "rallocx: MALLOCX_ZERO zeroes the grown tail");
            kl_dallocx(q, 0);
        }
    // rallocx ZERO: bytes past the old usable size read zero.
    unsigned char *p = kl_mallocx(10, 0);
    size_t old = kl_sallocx(p, 0);
    memset(p, 0xCD, old);
    unsigned char *q = kl_rallocx(p, old + 100, 0x40);
    int z = 1;
    for (size_t i = old; i < old + 100; i++) z &= q[i] == 0;
    CHECK(z, "rallocx: MALLOCX_ZERO zeroes the grown tail");
    kl_dallocx(q, 0);
}

// Drain the graveyard: every entry older than 100 ms is freed by the next put.
static void grave_drain(void) {
    usleep(150000);
    void *g = (void *)0x7000000u;
    cnd_entry_for(g);
    klb_pthread_cond_destroy(g);
}

// Distinct guest addresses, one cond each, created and destroyed: more of them than the table has slots.
// At full speed the 4096-entry graveyard fills and the rest leaks (never freed early); the table must stay usable.
static void test_cond_churn(void) {
    for (uintptr_t i = 0; i < 3 * (uintptr_t)MTX_MAP_SIZE; i++) {
        void *g = (void *)(0x40000000u + i * 64);
        cnd_entry *e = cnd_entry_for(g);
        CHECK(e && cnd_host(e), "cond churn: entry has a host cond");
        klb_pthread_cond_destroy(g);
    }
    int live = 0;
    for (unsigned i = 0; i < MTX_MAP_SIZE; i++) live += atomic_load(&g_cnd_map[i].key) > MTX_TOMB;
    CHECK(live == 0, "cond churn: no live entry left in the table");
    grave_drain();
    CHECK(g_grave_cnt == 1, "cond churn: graveyard reaped after 100 ms");
    // Below the ring size: nothing is freed early, everything is freed after 100 ms.
    long base = atomic_load(&g_live);
    for (uintptr_t i = 0; i < 1000; i++) {
        void *g = (void *)(0x48000000u + i * 64);
        cnd_entry_for(g);
        klb_pthread_cond_destroy(g);
    }
    CHECK(atomic_load(&g_live) - base == 1000, "cond grave: destroyed host conds are not freed early");
    grave_drain();
    CHECK(atomic_load(&g_live) - base <= 1, "cond grave: freed once 100 ms old");
}

static void test_cond_tombstone(void) {
    // three addresses sharing one hash slot: destroying the middle one must not hide the third.
    uintptr_t a[3]; int n = 0;
    unsigned want = mtx_hash(0x50000000u) & (MTX_MAP_SIZE - 1);
    for (uintptr_t x = 0x50000000u; n < 3; x += 16)
        if ((mtx_hash(x) & (MTX_MAP_SIZE - 1)) == want) a[n++] = x;
    cnd_entry *e0 = cnd_entry_for((void *)a[0]), *e1 = cnd_entry_for((void *)a[1]), *e2 = cnd_entry_for((void *)a[2]);
    CHECK(e0 != e1 && e1 != e2 && e0 != e2, "cond tombstone: three entries");
    klb_pthread_cond_destroy((void *)a[1]);
    CHECK(cnd_entry_for((void *)a[2]) == e2, "cond tombstone: lookup passes the tombstone");
    CHECK(atomic_load(&e1->key) == MTX_TOMB, "cond tombstone: destroyed slot is a tombstone");
    CHECK(cnd_entry_for((void *)a[1]) == e1, "cond tombstone: the freed slot is reused");
    // a cond with a sleeper is not freed under it.
    atomic_store(&e0->waiters, 1);
    pthread_cond_t *c0 = cnd_host(e0);
    klb_pthread_cond_destroy((void *)a[0]);
    CHECK(cnd_entry_for((void *)a[0]) == e0 && cnd_host(e0) == c0, "cond destroy: a cond with waiters stays");
    atomic_store(&e0->waiters, 0);
    // destroy of an address never seen is a no-op
    CHECK(klb_pthread_cond_destroy((void *)0x7777000) == 0, "cond destroy: unknown address");
}

// More live conds than slots: degrades to the shared cond, does not loop forever.
static void test_cond_full(void) {
    uintptr_t i = 0;
    for (; i < (uintptr_t)MTX_MAP_SIZE + 4; i++) cnd_entry_for((void *)(0x60000000u + i * 64));
    cnd_entry *e = cnd_entry_for((void *)(0x60000000u + i * 64));
    CHECK(e == &g_cnd_overflow && cnd_host(e), "cond full: shared fallback instead of a hang");
}

static void test_mutex_churn(void) {
    for (uintptr_t i = 0; i < 3 * (uintptr_t)MTX_MAP_SIZE; i++) {
        void *g = (void *)(0x80000000u + i * 64);
        mtx_entry *e = mtx_entry_for(g);
        pthread_mutex_t *h = mtx_host(e);
        pthread_mutex_lock(h); pthread_mutex_unlock(h);
        klb_pthread_mutex_destroy(g);
    }
    int live = 0;
    for (unsigned i = 0; i < MTX_MAP_SIZE; i++) live += atomic_load(&g_mtx_map[i].key) > MTX_TOMB;
    CHECK(live == 0, "mutex churn: no live entry left in the table");
    grave_drain();
    long base = atomic_load(&g_live);
    for (uintptr_t i = 0; i < 1000; i++) {
        void *g = (void *)(0x88000000u + i * 64);
        mtx_entry_for(g);
        klb_pthread_mutex_destroy(g);
    }
    CHECK(atomic_load(&g_live) - base == 1000, "mutex grave: destroyed host mutexes are not freed early");
    grave_drain();
    CHECK(atomic_load(&g_live) - base <= 1, "mutex grave: freed once 100 ms old");
    // a mutex the guest still holds is not freed (it leaks, as before), and destroy does not crash.
    void *g = (void *)0x90000000u;
    mtx_entry *e = mtx_entry_for(g);
    long before = atomic_load(&g_live);
    atomic_store(&e->depth, 1);
    pthread_mutex_t *hm = mtx_host(e);
    pthread_mutex_lock(hm);
    klb_pthread_mutex_destroy(g);
    pthread_mutex_unlock(hm); pthread_mutex_destroy(hm); t_free(hm);     // the intentional leak, released here
    CHECK(atomic_load(&g_live) == before - 1, "mutex destroy: a held mutex is kept (not freed by destroy)");
    CHECK(mtx_find(g) == NULL, "mutex destroy: the entry is gone");
}

// The churn tests leak by design when the graveyard ring is full.
const char *__asan_default_options(void) { return "detect_leaks=0"; }

int main(void) {
    alarm(240);
    test_affinity();
    test_madvclip();
    test_mallocx();
    test_cond_churn();
    test_cond_tombstone();
    test_mutex_churn();
    test_cond_full();
    if (fails) return 1;
    printf("t_libc_sem ok\n");
    return 0;
}
