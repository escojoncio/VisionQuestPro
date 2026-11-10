// Guest swap: the guest's large anonymous mappings backed by unlinked temporary files instead of anonymous memory.
//
// Why. visionOS ends the process at ~8190 MiB of phys_footprint, and phys_footprint counts anonymous pages whether
// they are resident or compressed. A Quest has no such line: its cold pages go to zram and the title keeps running.
// Measured (Mobile Suit Gundam, logs 26): a level load took the footprint from 4.1 to 8.05 GiB in 6 s (GPU textures
// +1.6 GiB, guest heap +1.9 GiB at the peak, 1 GiB of it compressed) and jetsam killed it; the low-memory notification
// cannot help because the guest is inside a synchronous load. Pages of a file mapping are not charged to
// phys_footprint, and the kernel can write them to the file and drop them under pressure, which is what swap is.
//
// What. klb_mmap (kl_libc.c) asks kl_gswap_map for a private anonymous non-executable mapping of at least
// KL_GSWAP_MIN_BYTES once the policy is on; every such mapping gets its own unlinked file (so closing it after the
// last munmap discards its pages without writing them). The guest sees ordinary zero-filled read/write memory:
//   munmap            kl_gswap_unmap forgets the range (hole punched, file closed when nothing of it is mapped)
//   MADV_DONTNEED     kl_gswap_fresh maps a never-used offset of the same file over the range: zero on next touch,
//                     exactly Linux's contract, whatever F_PUNCHHOLE does to cached pages
//   mmap(MAP_FIXED)   over a range of ours: kl_gswap_fresh again (it stays file-backed), else forgotten
// Any failure (no temp dir, no disk space, no descriptor, mmap refused) answers MAP_FAILED / 0 and the caller falls
// back to anonymous memory, so the feature can only fail towards the old behaviour.
//
// Header-only and POSIX so tests/t_gswap.c runs the shipping code on Linux. One translation unit owns the state.
#ifndef KL_GSWAP_H
#define KL_GSWAP_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define KL_GSWAP_MIN_BYTES   ((size_t)4 << 20)      // smaller mappings stay anonymous
#define KL_GSWAP_MAX_FILES   384                    // live mappings served at once
#define KL_GSWAP_DISK_SPARE  ((uint64_t)1 << 30)    // free space that must remain after a mapping is fully written

typedef struct { uintptr_t va; size_t len; int file; uint64_t off; } klgs_piece;     // [va, va+len) -> file @ off
typedef struct { int fd; uint64_t live; uint64_t size; } klgs_file;                  // fd < 0: slot free
typedef struct { int file; uint64_t off; size_t len; int lost; } klgs_rel;           // a range cut out of the table; lost = still mapped, only untracked

static struct {
    pthread_mutex_t mu;
    klgs_piece     *p;
    unsigned        n, cap;
    klgs_file       f[KL_GSWAP_MAX_FILES];
    int             inited;        // 0 untried, 1 ready, -1 unavailable
    char            dir[512];
    size_t          pg;
} klgs = { .mu = PTHREAD_MUTEX_INITIALIZER };

// Counters for the [mem] line. live = bytes mapped from files now.
static _Atomic uint64_t klgs_live, klgs_total, klgs_fresh_bytes, klgs_refused;
static _Atomic unsigned klgs_files, klgs_ever;

static void klgs_punch(int fd, uint64_t off, uint64_t len) {
#if defined(__APPLE__)
    struct { unsigned int fp_flags, reserved; off_t fp_offset, fp_length; } a = { 0, 0, (off_t)off, (off_t)len };
    (void)fcntl(fd, 99 /* F_PUNCHHOLE */, &a);
#elif defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    (void)fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len);
#else
    (void)fd; (void)off; (void)len;
#endif
}

// Under klgs.mu. Temp directory, page size, descriptor limit: once.
static int klgs_init(void) {
    if (klgs.inited) return klgs.inited > 0;
    klgs.inited = -1;
    for (unsigned i = 0; i < KL_GSWAP_MAX_FILES; i++) klgs.f[i].fd = -1;
    klgs.pg = (size_t)sysconf(_SC_PAGESIZE);
    const char *t = getenv("KL_GSWAP_DIR");
    if (!t || !*t) t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(klgs.dir, sizeof klgs.dir, "%s", t);
    size_t l = strlen(klgs.dir);
    while (l > 1 && klgs.dir[l - 1] == '/') klgs.dir[--l] = 0;
    if (access(klgs.dir, W_OK) != 0) {
        fprintf(stderr, "  [gswap] off: %s is not writable (%s)\n", klgs.dir, strerror(errno));
        return 0;
    }
    // One descriptor per live mapping: make room (the default soft limit is 256).
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rlim_t want = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (want > rl.rlim_cur) { rl.rlim_cur = want; (void)setrlimit(RLIMIT_NOFILE, &rl); }
    }
    klgs.inited = 1;
    return 1;
}

// Under klgs.mu. An unlinked file of `size` bytes, all hole; -1 with the reason in *why.
static int klgs_open(uint64_t size, const char **why) {
    struct statvfs sv;
    if (statvfs(klgs.dir, &sv) == 0 && (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize < size + KL_GSWAP_DISK_SPARE) {
        *why = "disk space";
        return -1;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/klgswap.XXXXXX", klgs.dir);
    int fd = mkstemp(path);
    if (fd < 0) { *why = "mkstemp"; return -1; }
    unlink(path);
    // Out of the low numbers a guest may close or put in an fd_set.
    int hi = fcntl(fd, F_DUPFD_CLOEXEC, 1024);
    if (hi >= 0) { close(fd); fd = hi; } else (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
    (void)fcntl(fd, 64 /* F_SETPROTECTIONCLASS */, 4 /* class D: readable while the device is locked */);
#endif
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); *why = "ftruncate"; return -1; }
    return fd;
}

// Under klgs.mu. Index of the first piece that ends after `va` (n when none).
static unsigned klgs_find(uintptr_t va) {
    unsigned lo = 0, hi = klgs.n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (klgs.p[mid].va + klgs.p[mid].len <= va) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// Under klgs.mu. Insert keeping the table sorted; 0 when it cannot grow.
static int klgs_insert(klgs_piece pc) {
    if (klgs.n == klgs.cap) {
        unsigned nc = klgs.cap ? klgs.cap * 2 : 128;
        klgs_piece *np = (klgs_piece *)realloc(klgs.p, (size_t)nc * sizeof *np);
        if (!np) return 0;
        klgs.p = np; klgs.cap = nc;
    }
    unsigned i = klgs_find(pc.va);
    memmove(&klgs.p[i + 1], &klgs.p[i], (size_t)(klgs.n - i) * sizeof *klgs.p);
    klgs.p[i] = pc;
    klgs.n++;
    return 1;
}

// Under klgs.mu. Remove [a, a+len) from the table, at most `cap` ranges per call (written to rel[]). Returns how
// many; a caller that gets `cap` calls again. A piece cut in the middle needs one more slot: when the table cannot
// grow, the tail of that piece leaves the table as a `lost` entry (it is still mapped and in use: never punched).
static unsigned klgs_cut(uintptr_t a, size_t len, klgs_rel *rel, unsigned cap) {
    uintptr_t b = a + len;
    unsigned got = 0;
    unsigned i = klgs_find(a);
    while (i < klgs.n && klgs.p[i].va < b && got + 1 < cap) {      // +1: room for a lost tail
        klgs_piece pc = klgs.p[i];
        uintptr_t pe = pc.va + pc.len;
        uintptr_t lo = pc.va > a ? pc.va : a, hi = pe < b ? pe : b;
        rel[got++] = (klgs_rel){ pc.file, pc.off + (lo - pc.va), hi - lo, 0 };
        if (lo == pc.va && hi == pe) {                       // whole piece
            memmove(&klgs.p[i], &klgs.p[i + 1], (size_t)(klgs.n - i - 1) * sizeof *klgs.p);
            klgs.n--;
            continue;
        }
        if (lo == pc.va) {                                   // head cut
            klgs.p[i].va = hi; klgs.p[i].len = pe - hi; klgs.p[i].off = pc.off + (hi - pc.va);
        } else if (hi == pe) {                               // tail cut
            klgs.p[i].len = lo - pc.va;
        } else {                                             // middle: [va,lo) stays, [hi,pe) is a new piece
            klgs.p[i].len = lo - pc.va;
            klgs_piece tail = { hi, pe - hi, pc.file, pc.off + (hi - pc.va) };
            if (!klgs_insert(tail)) rel[got++] = (klgs_rel){ tail.file, tail.off, tail.len, 1 };
        }
        i++;
    }
    return got;
}

// Under klgs.mu. A range left the address space: give the blocks back, close the file with its last byte.
static void klgs_release(const klgs_rel *r) {
    klgs_file *f = &klgs.f[r->file];
    f->live -= r->len;
    atomic_fetch_sub(&klgs_live, r->len);
    if (r->lost) return;                                     // still mapped: the descriptor and the blocks stay
    if (f->live == 0) {
        close(f->fd);
        f->fd = -1;
        atomic_fetch_sub(&klgs_files, 1);
    } else {
        klgs_punch(f->fd, r->off, r->len);
    }
}

// A zero-filled mapping of `len` bytes at `prot`, file-backed. MAP_FAILED when it cannot be served.
static void *kl_gswap_map(void *hint, size_t len, int prot) {
    void *out = MAP_FAILED;
    const char *why = NULL;
    pthread_mutex_lock(&klgs.mu);
    if (!klgs_init()) { pthread_mutex_unlock(&klgs.mu); return MAP_FAILED; }
    len = (len + klgs.pg - 1) & ~(klgs.pg - 1);
    unsigned slot = 0;
    while (slot < KL_GSWAP_MAX_FILES && klgs.f[slot].fd >= 0) slot++;
    if (slot == KL_GSWAP_MAX_FILES) why = "file slots";
    int fd = why ? -1 : klgs_open(len, &why);
    if (fd >= 0) {
        out = mmap(hint, len, prot, MAP_SHARED, fd, 0);
        if (out == MAP_FAILED) why = "mmap";
        else if (!klgs_insert((klgs_piece){ (uintptr_t)out, len, (int)slot, 0 })) {
            munmap(out, len);
            out = MAP_FAILED;
            why = "table";
        }
        if (out == MAP_FAILED) close(fd);
    }
    if (out != MAP_FAILED) {
        klgs.f[slot] = (klgs_file){ fd, len, len };
        atomic_fetch_add(&klgs_live, len);
        atomic_fetch_add(&klgs_total, len);
        atomic_fetch_add(&klgs_files, 1);
        atomic_store(&klgs_ever, 1);
    }
    pthread_mutex_unlock(&klgs.mu);
    if (out == MAP_FAILED) {
        static _Atomic int said;
        if (atomic_fetch_add(&klgs_refused, 1) < 1000000 && atomic_fetch_add(&said, 1) < 4)
            fprintf(stderr, "  [gswap] %zu MiB stays anonymous: %s (%s)\n", len >> 20, why ? why : "?", strerror(errno));
    }
    return out;
}

// 1 when `addr` is inside a mapping of ours.
static int kl_gswap_owns(const void *addr) {
    if (!atomic_load(&klgs_ever)) return 0;
    pthread_mutex_lock(&klgs.mu);
    unsigned i = klgs_find((uintptr_t)addr);
    int own = i < klgs.n && klgs.p[i].va <= (uintptr_t)addr;
    pthread_mutex_unlock(&klgs.mu);
    return own;
}

// [addr, addr+len) is about to leave the address space or be replaced by the caller (munmap, an anonymous
// MAP_FIXED): forget what is ours in it. Call BEFORE the munmap/mmap, so the table never names reused addresses.
static void kl_gswap_unmap(void *addr, size_t len) {
    if (!atomic_load(&klgs_ever) || !len) return;
    pthread_mutex_lock(&klgs.mu);
    klgs_rel rel[16];
    unsigned got;
    do {
        got = klgs_cut((uintptr_t)addr, len, rel, 16);
        for (unsigned i = 0; i < got; i++) klgs_release(&rel[i]);
    } while (got >= 15);
    pthread_mutex_unlock(&klgs.mu);
}

// Replace [addr, addr+len) with never-used (zero) pages of the same file at `prot`, when the WHOLE range is ours.
// Returns 1 when done; 0 when any byte of it is not ours or a step failed (nothing was changed in the address space
// beyond sub-ranges already replaced, which read zero). The old blocks are punched out.
static int kl_gswap_fresh(void *addr, size_t len, int prot) {
    if (!atomic_load(&klgs_ever) || !len) return 0;
    uintptr_t a = (uintptr_t)addr, b = a + len;
    int ok = 1;
    pthread_mutex_lock(&klgs.mu);
    // Whole range covered by our pieces, contiguously?
    uintptr_t want = a;
    for (unsigned i = klgs_find(a); want < b; i++) {
        if (i >= klgs.n || klgs.p[i].va > want) { ok = 0; break; }
        want = klgs.p[i].va + klgs.p[i].len;
    }
    if (ok && ((a | b) & (klgs.pg - 1))) ok = 0;             // whole host pages only
    while (ok && a < b) {
        unsigned i = klgs_find(a);
        klgs_piece pc = klgs.p[i];
        uintptr_t hi = pc.va + pc.len < b ? pc.va + pc.len : b;
        size_t n = hi - a;
        klgs_file *f = &klgs.f[pc.file];
        uint64_t noff = f->size;
        if (ftruncate(f->fd, (off_t)(noff + n)) != 0) { ok = 0; break; }
        f->size = noff + n;
        if (mmap((void *)a, n, prot, MAP_FIXED | MAP_SHARED, f->fd, (off_t)noff) == MAP_FAILED) { ok = 0; break; }
        klgs_rel rel[3];
        unsigned got = klgs_cut(a, n, rel, 3);               // one piece: the cut range, maybe a lost tail
        for (unsigned k = 0; k < got; k++) {
            if (!rel[k].lost) { klgs_punch(klgs.f[rel[k].file].fd, rel[k].off, rel[k].len); continue; }
            f->live -= rel[k].len;                           // lost tail: still mapped, only untracked
            atomic_fetch_sub(&klgs_live, rel[k].len);
        }
        if (!klgs_insert((klgs_piece){ a, n, pc.file, noff })) {   // mapped but untracked: only bookkeeping is lost
            f->live -= n;
            atomic_fetch_sub(&klgs_live, n);
        }
        atomic_fetch_add(&klgs_fresh_bytes, n);
        a = hi;
    }
    pthread_mutex_unlock(&klgs.mu);
    return ok;
}

// ---- big blocks of the guest's malloc family ---------------------------------------------------------------------
// Measured (Mobile Suit Gundam, logs 27): the level's texture data is malloc'ed, not mmap'ed (MALLOC_LARGE 1357 MiB
// resident + 417 MiB compressed at the kill, against 650 MiB of guest mmap), so the mappings above alone moved 512 MiB.
// A block is one mapping of its own; the table below knows each block's base and size, which is what free(),
// realloc() and malloc_size() need. On Apple the blocks belong to a registered malloc zone, so those three find them
// from ANY caller (the guest's forwarded free, or the runtime freeing a pointer the guest allocated) with no wrapper.
typedef struct { uintptr_t base; size_t size, maplen; } klgs_blk;
static struct { klgs_blk *b; unsigned n, cap; } klgs_blks;        // under klgs.mu, sorted by base
static _Atomic unsigned klgs_blk_n;                                // blocks alive (fast "none of ours" answer)
static _Atomic uint64_t klgs_blk_total;                            // bytes ever handed out as blocks

// Under klgs.mu. Index of the first block whose base is >= `base`.
static unsigned klgs_blk_find(uintptr_t base) {
    unsigned lo = 0, hi = klgs_blks.n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (klgs_blks.b[mid].base < base) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// A zero-filled block of `size` bytes (page aligned), or NULL when it cannot be served from a file.
static void *kl_gswap_alloc(size_t size) {
    if (!size) size = 1;
    void *p = kl_gswap_map(NULL, size, PROT_READ | PROT_WRITE);
    if (p == MAP_FAILED) return NULL;
    pthread_mutex_lock(&klgs.mu);
    size_t maplen = (size + klgs.pg - 1) & ~(klgs.pg - 1);
    int ok = 1;
    if (klgs_blks.n == klgs_blks.cap) {
        unsigned nc = klgs_blks.cap ? klgs_blks.cap * 2 : 64;
        klgs_blk *nb = (klgs_blk *)realloc(klgs_blks.b, (size_t)nc * sizeof *nb);
        if (nb) { klgs_blks.b = nb; klgs_blks.cap = nc; } else ok = 0;
    }
    if (ok) {
        unsigned i = klgs_blk_find((uintptr_t)p);
        memmove(&klgs_blks.b[i + 1], &klgs_blks.b[i], (size_t)(klgs_blks.n - i) * sizeof *klgs_blks.b);
        klgs_blks.b[i] = (klgs_blk){ (uintptr_t)p, size, maplen };
        klgs_blks.n++;
        atomic_fetch_add(&klgs_blk_n, 1);
        atomic_fetch_add(&klgs_blk_total, size);
    }
    pthread_mutex_unlock(&klgs.mu);
    if (!ok) { kl_gswap_unmap(p, maplen); munmap(p, maplen); return NULL; }
    return p;
}

// Size of the block that starts at `p`; 0 when `p` is not the start of one.
static size_t kl_gswap_alloc_size(const void *p) {
    if (!atomic_load(&klgs_blk_n)) return 0;
    size_t size = 0;
    pthread_mutex_lock(&klgs.mu);
    unsigned i = klgs_blk_find((uintptr_t)p);
    if (i < klgs_blks.n && klgs_blks.b[i].base == (uintptr_t)p) size = klgs_blks.b[i].size;
    pthread_mutex_unlock(&klgs.mu);
    return size;
}

// Frees the block that starts at `p`. 1 when it was one of ours.
static int kl_gswap_alloc_free(void *p) {
    if (!atomic_load(&klgs_blk_n)) return 0;
    pthread_mutex_lock(&klgs.mu);
    unsigned i = klgs_blk_find((uintptr_t)p);
    if (i >= klgs_blks.n || klgs_blks.b[i].base != (uintptr_t)p) { pthread_mutex_unlock(&klgs.mu); return 0; }
    size_t maplen = klgs_blks.b[i].maplen;
    memmove(&klgs_blks.b[i], &klgs_blks.b[i + 1], (size_t)(klgs_blks.n - i - 1) * sizeof *klgs_blks.b);
    klgs_blks.n--;
    atomic_fetch_sub(&klgs_blk_n, 1);
    pthread_mutex_unlock(&klgs.mu);
    kl_gswap_unmap(p, maplen);
    munmap(p, maplen);
    return 1;
}

// Resizes the block at `p` (one of ours): in place while it fits its mapping, else a new block with the contents
// moved. NULL when a new block cannot be served; `p` is then untouched.
static void *kl_gswap_alloc_realloc(void *p, size_t size) {
    if (!size) size = 1;
    size_t old = 0;
    pthread_mutex_lock(&klgs.mu);
    unsigned i = klgs_blk_find((uintptr_t)p);
    int ours = i < klgs_blks.n && klgs_blks.b[i].base == (uintptr_t)p;
    if (ours) {
        old = klgs_blks.b[i].size;
        if (size <= klgs_blks.b[i].maplen) {
            klgs_blks.b[i].size = size;
            pthread_mutex_unlock(&klgs.mu);
            return p;
        }
    }
    pthread_mutex_unlock(&klgs.mu);
    if (!ours) return NULL;
    void *q = kl_gswap_alloc(size);
    if (!q) return NULL;
    memcpy(q, p, old < size ? old : size);
    kl_gswap_alloc_free(p);
    return q;
}

#if defined(__APPLE__)
#include <malloc/malloc.h>
// The malloc zone that owns the blocks. libmalloc asks every registered zone "is this yours?" (size) for a pointer its
// default zone does not know, so free / realloc / malloc_size reach these from any thread of the process.
static size_t klgs_z_size(malloc_zone_t *z, const void *p) { (void)z; return kl_gswap_alloc_size(p); }
static void *klgs_z_malloc(malloc_zone_t *z, size_t n) { (void)z; return kl_gswap_alloc(n); }
static void *klgs_z_calloc(malloc_zone_t *z, size_t c, size_t n) {
    (void)z;
    size_t t;
    return __builtin_mul_overflow(c, n, &t) ? NULL : kl_gswap_alloc(t);          // a fresh mapping is already zero
}
static void klgs_z_free(malloc_zone_t *z, void *p) { (void)z; kl_gswap_alloc_free(p); }
static void klgs_z_free_sized(malloc_zone_t *z, void *p, size_t n) { (void)z; (void)n; kl_gswap_alloc_free(p); }
static void *klgs_z_realloc(malloc_zone_t *z, void *p, size_t n) {
    (void)z;
    if (!p) return kl_gswap_alloc(n);
    void *q = kl_gswap_alloc_realloc(p, n);
    if (q) return q;
    size_t old = kl_gswap_alloc_size(p);                                         // no file for it: ordinary memory
    if (!old || !(q = malloc(n ? n : 1))) return NULL;
    memcpy(q, p, old < n ? old : n);
    kl_gswap_alloc_free(p);
    return q;
}
static void *klgs_z_memalign(malloc_zone_t *z, size_t align, size_t n) {
    (void)z;
    return align <= (size_t)getpagesize() ? kl_gswap_alloc(n) : NULL;            // blocks are page aligned
}
static void klgs_z_destroy(malloc_zone_t *z) { (void)z; }
static unsigned klgs_z_batch_malloc(malloc_zone_t *z, size_t n, void **out, unsigned c) { (void)z; (void)n; (void)out; (void)c; return 0; }
static void klgs_z_batch_free(malloc_zone_t *z, void **p, unsigned c) { (void)z; for (unsigned i = 0; i < c; i++) kl_gswap_alloc_free(p[i]); }
static kern_return_t klgs_zi_enumerator(task_t t, void *c, unsigned m, vm_address_t a, memory_reader_t r, vm_range_recorder_t rec) {
    (void)t; (void)c; (void)m; (void)a; (void)r; (void)rec;
    return KERN_SUCCESS;
}
static size_t klgs_zi_good_size(malloc_zone_t *z, size_t n) { (void)z; size_t pg = (size_t)getpagesize(); return (n + pg - 1) & ~(pg - 1); }
static boolean_t klgs_zi_check(malloc_zone_t *z) { (void)z; return 1; }
static void klgs_zi_print(malloc_zone_t *z, boolean_t v) { (void)z; (void)v; }
static void klgs_zi_log(malloc_zone_t *z, void *a) { (void)z; (void)a; }
static void klgs_zi_lock(malloc_zone_t *z) { (void)z; }
static void klgs_zi_stats(malloc_zone_t *z, malloc_statistics_t *st) {
    (void)z;
    st->blocks_in_use = atomic_load(&klgs_blk_n);
    st->size_in_use = st->max_size_in_use = st->size_allocated = (size_t)atomic_load(&klgs_live);
}
static boolean_t klgs_zi_locked(malloc_zone_t *z) { (void)z; return 0; }
static boolean_t klgs_zi_discharge_on(malloc_zone_t *z) { (void)z; return 0; }
static void klgs_zi_discharge(malloc_zone_t *z, void *m) { (void)z; (void)m; }

static malloc_introspection_t klgs_zone_intro;
static malloc_zone_t klgs_zone;
static void klgs_zone_register(void) {
    klgs_zone_intro.enumerator = klgs_zi_enumerator;
    klgs_zone_intro.good_size = klgs_zi_good_size;
    klgs_zone_intro.check = klgs_zi_check;
    klgs_zone_intro.print = klgs_zi_print;
    klgs_zone_intro.log = klgs_zi_log;
    klgs_zone_intro.force_lock = klgs_zi_lock;
    klgs_zone_intro.force_unlock = klgs_zi_lock;
    klgs_zone_intro.statistics = klgs_zi_stats;
    klgs_zone_intro.zone_locked = klgs_zi_locked;
    klgs_zone_intro.enable_discharge_checking = klgs_zi_discharge_on;
    klgs_zone_intro.disable_discharge_checking = klgs_zi_lock;
    klgs_zone_intro.discharge = klgs_zi_discharge;
    klgs_zone_intro.reinit_lock = klgs_zi_lock;
    klgs_zone.size = klgs_z_size;
    klgs_zone.malloc = klgs_z_malloc;
    klgs_zone.calloc = klgs_z_calloc;
    klgs_zone.valloc = klgs_z_malloc;
    klgs_zone.free = klgs_z_free;
    klgs_zone.realloc = klgs_z_realloc;
    klgs_zone.destroy = klgs_z_destroy;
    klgs_zone.zone_name = "klepton guest swap";
    klgs_zone.batch_malloc = klgs_z_batch_malloc;
    klgs_zone.batch_free = klgs_z_batch_free;
    klgs_zone.introspect = &klgs_zone_intro;
    klgs_zone.version = 9;                       // memalign (5), free_definite_size (6), reinit_lock (9); nothing newer
    klgs_zone.memalign = klgs_z_memalign;
    klgs_zone.free_definite_size = klgs_z_free_sized;
    malloc_zone_register(&klgs_zone);
}
// Before the first block is handed out.
static void kl_gswap_zone_ready(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, klgs_zone_register);
}
#else
static void kl_gswap_zone_ready(void) {}
#endif

// A block for the guest's malloc family: free() / realloc() / malloc_size() take it back like any other pointer.
static void *kl_gswap_big(size_t size) {
    kl_gswap_zone_ready();
    return kl_gswap_alloc(size);
}

#endif
