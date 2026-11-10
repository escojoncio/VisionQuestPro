// Host test (Linux or macOS) of the guest swap (runtime/libc/kl_gswap.h): file-backed guest mappings behave like
// zero-filled anonymous memory through munmap, MADV_DONTNEED (fresh pages) and partial cuts.
//   make gswap        (or tools/ci/host-tests.sh)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libc/kl_gswap.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int all(const unsigned char *p, size_t n, unsigned char v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

int main(void) {
    char dir[] = "/tmp/t_gswap_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    setenv("KL_GSWAP_DIR", dir, 1);
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), M = (size_t)1 << 20;
    if (M % pg) return 2;

    // A mapping reads zero, holds what is written, and is ours.
    unsigned char *a = kl_gswap_map(NULL, 8 * M, PROT_READ | PROT_WRITE);
    CHECK(a != MAP_FAILED, "map 8 MiB");
    if (a == MAP_FAILED) { printf("t_gswap: cannot map in %s\n", dir); return 1; }
    CHECK(all(a, 8 * M, 0), "fresh mapping is zero");
    memset(a, 0xAB, 8 * M);
    CHECK(kl_gswap_owns(a) && kl_gswap_owns(a + 8 * M - 1) && !kl_gswap_owns(a + 8 * M), "owns: bounds");
    CHECK(atomic_load(&klgs_live) == 8 * M && atomic_load(&klgs_files) == 1, "counters after map");

    // MADV_DONTNEED over the middle: zero there, data kept around it.
    CHECK(kl_gswap_fresh(a + 2 * M, M, PROT_READ | PROT_WRITE) == 1, "fresh middle");
    CHECK(all(a + 2 * M, M, 0), "decommitted range reads zero");
    CHECK(all(a, 2 * M, 0xAB) && all(a + 3 * M, 5 * M, 0xAB), "neighbours kept");
    memset(a + 2 * M, 0xCD, M);
    CHECK(all(a + 2 * M, M, 0xCD), "decommitted range writable");
    CHECK(atomic_load(&klgs_live) == 8 * M, "fresh does not change live");
    // ...again over a range that spans the three pieces it left.
    CHECK(kl_gswap_fresh(a + M, 3 * M, PROT_READ | PROT_WRITE) == 1, "fresh across pieces");
    CHECK(all(a + M, 3 * M, 0) && all(a, M, 0xAB) && all(a + 4 * M, 4 * M, 0xAB), "fresh across pieces: contents");

    // Not ours / not whole pages / partly ours: refused, nothing changed.
    unsigned char *anon = mmap(NULL, 4 * M, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(anon, 0x11, 4 * M);
    CHECK(kl_gswap_fresh(anon, M, PROT_READ | PROT_WRITE) == 0 && all(anon, M, 0x11), "fresh: not ours");
    CHECK(kl_gswap_fresh(a + 1, pg, PROT_READ | PROT_WRITE) == 0 && all(a, M, 0xAB), "fresh: unaligned");
    CHECK(kl_gswap_fresh(a + 7 * M, 2 * M, PROT_READ | PROT_WRITE) == 0 && all(a + 7 * M, M, 0xAB), "fresh: runs past ours");

    // Partial munmap: head, then a hole in the middle; the rest keeps its data.
    kl_gswap_unmap(a, M); munmap(a, M);
    CHECK(!kl_gswap_owns(a) && kl_gswap_owns(a + M), "unmap head");
    kl_gswap_unmap(a + 5 * M, M); munmap(a + 5 * M, M);
    CHECK(!kl_gswap_owns(a + 5 * M) && kl_gswap_owns(a + 6 * M) && kl_gswap_owns(a + 4 * M), "unmap middle");
    CHECK(all(a + 4 * M, M, 0xAB) && all(a + 6 * M, 2 * M, 0xAB), "data around the holes");
    CHECK(atomic_load(&klgs_live) == 6 * M && atomic_load(&klgs_files) == 1, "counters after partial unmaps");

    // A second mapping; one unmap over both and the anonymous gap between/around them.
    unsigned char *b = kl_gswap_map(NULL, 4 * M, PROT_READ | PROT_WRITE);
    CHECK(b != MAP_FAILED && atomic_load(&klgs_files) == 2, "second mapping");
    unsigned char *lo = a < b ? a : b, *hi = a < b ? b + 4 * M : a + 8 * M;
    if (b > a) hi = b + 4 * M; else hi = a + 8 * M;
    kl_gswap_unmap(lo, (size_t)(hi - lo));
    CHECK(atomic_load(&klgs_live) == 0 && atomic_load(&klgs_files) == 0, "everything released, files closed");
    CHECK(!kl_gswap_owns(a + 6 * M) && !kl_gswap_owns(b), "nothing owned");
    munmap(a + M, 4 * M); munmap(a + 6 * M, 2 * M); munmap(b, 4 * M);

    // PROT_NONE reserve, then made usable: the reserve/commit shape.
    unsigned char *r = kl_gswap_map(NULL, 16 * M, PROT_NONE);
    CHECK(r != MAP_FAILED, "PROT_NONE reserve");
    CHECK(mprotect(r + 4 * M, 4 * M, PROT_READ | PROT_WRITE) == 0, "commit by mprotect");
    r[4 * M] = 7;
    CHECK(r[4 * M] == 7 && all(r + 4 * M + 1, 4 * M - 1, 0), "committed range usable");
    CHECK(kl_gswap_fresh(r + 4 * M, 4 * M, PROT_NONE) == 1, "decommit to PROT_NONE");
    CHECK(mprotect(r + 4 * M, 4 * M, PROT_READ | PROT_WRITE) == 0 && r[4 * M] == 0, "recommit reads zero");
    kl_gswap_unmap(r, 16 * M); munmap(r, 16 * M);
    CHECK(atomic_load(&klgs_live) == 0 && atomic_load(&klgs_files) == 0, "reserve released");

    // Many small pieces: the table grows and unmap drains more than one batch.
    unsigned char *m = kl_gswap_map(NULL, 64 * M, PROT_READ | PROT_WRITE);
    CHECK(m != MAP_FAILED, "64 MiB");
    for (unsigned i = 0; i < 400; i++)
        CHECK(kl_gswap_fresh(m + (size_t)i * 2 * pg, pg, PROT_READ | PROT_WRITE) == 1, "fresh page");
    m[0] = 1; m[pg] = 2;
    kl_gswap_unmap(m, 64 * M); munmap(m, 64 * M);
    CHECK(atomic_load(&klgs_live) == 0 && atomic_load(&klgs_files) == 0 && klgs.n == 0, "fragmented mapping released");

    // Blocks of the malloc family: size, in-place shrink, growth by move, free; not-ours pointers are left alone.
    {
        unsigned char *b1 = kl_gswap_big(5 * M + 123);
        CHECK(b1 && all(b1, 5 * M + 123, 0), "block: zero filled");
        if (!b1) { printf("t_gswap: cannot allocate a block\n"); return 1; }
        CHECK(kl_gswap_alloc_size(b1) == 5 * M + 123 && kl_gswap_alloc_size(b1 + 16) == 0, "block: size only at its base");
        memset(b1, 0x5A, 5 * M + 123);
        unsigned char *same = kl_gswap_alloc_realloc(b1, 3 * M);
        CHECK(same == b1 && kl_gswap_alloc_size(b1) == 3 * M && all(b1, 3 * M, 0x5A), "block: shrink in place");
        unsigned char *b2 = kl_gswap_alloc_realloc(b1, 9 * M);
        CHECK(b2 && b2 != b1 && kl_gswap_alloc_size(b2) == 9 * M && kl_gswap_alloc_size(b1) == 0, "block: growth moves it");
        CHECK(b2 && all(b2, 3 * M, 0x5A) && all(b2 + 3 * M, 6 * M, 0), "block: contents moved, rest zero");
        int local = 0;
        CHECK(kl_gswap_alloc_free(&local) == 0 && kl_gswap_alloc_size(&local) == 0, "block: a pointer that is not ours");
        CHECK(kl_gswap_alloc_realloc(&local, M) == NULL, "block: realloc of a pointer that is not ours");
        CHECK(kl_gswap_alloc_free(b2) == 1 && kl_gswap_alloc_free(b2) == 0, "block: free once");
        CHECK(atomic_load(&klgs_live) == 0 && atomic_load(&klgs_files) == 0 && atomic_load(&klgs_blk_n) == 0, "block: all released");
#if defined(__APPLE__)
        // libmalloc finds the blocks through the registered zone (what free / realloc / malloc_size of any caller use).
        // The zone's own entries are called here rather than free()/realloc(): this test runs under ASan, whose
        // interposed free() rejects every pointer that is not ASan's.
        unsigned char *z = kl_gswap_big(6 * M);
        CHECK(z && malloc_zone_from_ptr(z) == &klgs_zone, "zone: libmalloc resolves the block to our zone");
        CHECK(z && klgs_zone.size(&klgs_zone, z) == 6 * M && klgs_zone.size(&klgs_zone, z + 8) == 0, "zone: size");
        memset(z, 0x77, 6 * M);
        unsigned char *z2 = klgs_zone.realloc(&klgs_zone, z, 12 * M);
        CHECK(z2 && klgs_zone.size(&klgs_zone, z2) == 12 * M && all(z2, 6 * M, 0x77), "zone: realloc");
        klgs_zone.free_definite_size(&klgs_zone, z2, 12 * M);
        CHECK(atomic_load(&klgs_blk_n) == 0 && atomic_load(&klgs_live) == 0, "zone: free");
        void *plain = malloc(100);                                   // ordinary pointers are not claimed
        CHECK(plain && klgs_zone.size(&klgs_zone, plain) == 0 && malloc_zone_from_ptr(plain) != &klgs_zone, "zone: ordinary malloc is not ours");
        free(plain);
        malloc_statistics_t st;
        malloc_zone_statistics(NULL, &st);                           // walks every registered zone, ours included
        malloc_zone_statistics(&klgs_zone, &st);
        CHECK(st.blocks_in_use == 0, "zone: statistics");
#endif
    }

    rmdir(dir);
    if (fails) { printf("t_gswap: %d FAILED\n", fails); return 1; }
    printf("t_gswap: ok\n");
    return 0;
}
