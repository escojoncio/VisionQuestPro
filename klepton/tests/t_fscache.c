// Host test of the [fs-cache] log (kl_libc.c, KL-TEST fscache block, cut out at build time): which paths match,
// first success per (op, path) + failures no. 1, 2, 4, 8 of each (op, path), the 64-line cap, errno untouched, size from the open fd.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "fscache.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int capfd, savedfd;
static size_t count(const char *needle) {
    char *b = calloc(1, 1 << 20);
    ssize_t n = pread(capfd, b, (1 << 20) - 1, 0);
    size_t c = 0;
    if (n > 0) for (const char *p = b; (p = strstr(p, needle)); p += strlen(needle)) c++;
    free(b);
    return c;
}
static void reset(void) {
    if (ftruncate(capfd, 0) != 0) abort();
    g_fsc_lines = g_fsc_nseen = g_fsc_nfail = 0;
    memset(g_fsc_seen, 0, sizeof g_fsc_seen);
    memset(g_fsc_fkey, 0, sizeof g_fsc_fkey);
    memset(g_fsc_fn, 0, sizeof g_fsc_fn);
    memset(g_fsc_fl, 0, sizeof g_fsc_fl);
}

int main(void) {
    char tmpl[] = "/tmp/t_fscache.XXXXXX";
    capfd = mkstemp(tmpl);
    CHECK(capfd >= 0, "mkstemp");
    unlink(tmpl);
    fcntl(capfd, F_SETFL, O_APPEND);   // after a truncate the next write goes to offset 0
    savedfd = dup(2);
    dup2(capfd, 2);

    // Path filter: case-insensitive substring, six words.
    static const char *const yes[] = { "/data/g/Saved/PipelineCache/a.bin", "/x/UPIPELINECACHE", "/x/y.METALAR",
        "/x/ShaderCache/z", "/x/psocache.bin", "/x/Pipelines/z", "/x/pipelines", "/x/PsOCaChE" };
    static const char *const no[] = { "/data/g/Saved/Config/Engine.ini", "/x/Pipeline/z", "Pipelines", "/x/shader_cache",
        "/x/metal", "", "/x/pso" };
    for (unsigned i = 0; i < sizeof yes / sizeof *yes; i++) { reset(); CHECK(kl_fscache_wanted(yes[i]), yes[i]); }
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++) { reset(); CHECK(!kl_fscache_wanted(no[i]), no[i]); }
    CHECK(!kl_fscache_wanted(NULL), "NULL path");
    kl_fscache_note("open", "/x/Config/a.ini", 5, 10);
    kl_fscache_note("open", "/x/Config/a.ini", -1, -1);
    kl_fscache_note("open", NULL, 5, 10);
    CHECK(count("[fs-cache]") == 0, "a path outside the filter logs nothing");

    // First success per (op, path); stat and open of the same path are separate; every failure.
    reset();
    kl_fscache_note("open", "/x/PipelineCache/a.bin", 7, 123);
    CHECK(count("[fs-cache] open path=/x/PipelineCache/a.bin ret=7 size=123\n") == 1, "success line format");
    kl_fscache_note("open", "/x/PipelineCache/a.bin", 8, 123);
    kl_fscache_note("open", "/x/PipelineCache/a.bin", 9, 123);
    CHECK(count("[fs-cache]") == 1, "repeat successful open of a path: not logged");
    kl_fscache_note("stat", "/x/PipelineCache/a.bin", 0, 123);
    kl_fscache_note("stat", "/x/PipelineCache/a.bin", 0, 123);
    CHECK(count("[fs-cache] stat path=/x/PipelineCache/a.bin ret=0 size=123\n") == 1 && count("[fs-cache]") == 2,
          "stat is its own first-time entry");
    kl_fscache_note("open", "/x/PipelineCache/b.bin", 9, 5);
    CHECK(count("[fs-cache]") == 3, "another path");
    kl_fscache_note("open", "/x/PipelineCache/c.bin", -1, -1);
    kl_fscache_note("open", "/x/PipelineCache/c.bin", -1, -1);
    kl_fscache_note("stat", "/x/PipelineCache/a.bin", -1, -1);
    CHECK(count("ret=-1 size=-1\n") == 3, "failures 1 and 2 of a path are logged, repeats included");
    kl_fscache_note("open", "/x/PipelineCache/c.bin", 4, 77);
    CHECK(count("/x/PipelineCache/c.bin ret=4 size=77") == 1, "a success after failures is logged once");
    kl_fscache_note("open", "/x/PipelineCache/c.bin", 4, 77);
    CHECK(count("[fs-cache]") == 7, "no more after the repeat");

    // errno survives the log.
    errno = ENOENT;
    kl_fscache_note("open", "/x/PipelineCache/zz.bin", -1, -1);
    CHECK(errno == ENOENT, "errno kept (failure line)");
    errno = EACCES;
    kl_fscache_note("stat", "/x/PipelineCache/zz.bin", 0, 1);
    CHECK(errno == EACCES, "errno kept (success line)");

    // 64 lines at most, whatever mix; the last one says so; afterwards the filter is closed.
    reset();
    for (int i = 0; i < 100; i++) {
        char p[64];
        snprintf(p, sizeof p, "/x/PipelineCache/f%d", i);
        kl_fscache_note("open", p, i % 2 ? -1 : 3, -1);
    }
    CHECK(count("[fs-cache]") == 64, "cap of 64 lines");
    CHECK(count("(fs-cache cap reached)") == 1, "cap marker once");
    CHECK(!kl_fscache_wanted("/x/PipelineCache/new"), "filter closed at the cap");
    for (int i = 0; i < 50; i++) kl_fscache_note("open", "/x/PipelineCache/f0", -1, -1);   // repeated failures after the cap
    CHECK(count("[fs-cache]") == 64, "nothing after the cap");

    // Repeated failures of one (op, path): no. 1, 2, 4, 8 are logged, 4 lines at most (not 64: the path fails every frame).
    {
        static const int upto[] = { 1, 2, 3, 4, 7, 8, 9, 16, 100 }, lines[] = { 1, 2, 2, 3, 3, 4, 4, 4, 4 };
        for (unsigned t = 0; t < sizeof upto / sizeof *upto; t++) {
            reset();
            for (int i = 0; i < upto[t]; i++) kl_fscache_note("open", "/x/PipelineCache/missing", -1, -1);
            char what[64];
            snprintf(what, sizeof what, "%d failures of one path log %d lines", upto[t], lines[t]);
            CHECK(count("[fs-cache]") == (size_t)lines[t], what);
        }
    }
    reset();
    for (int i = 0; i < 100; i++) kl_fscache_note("open", "/x/PipelineCache/missing", -1, -1);
    CHECK(count("[fs-cache]") == 4 && count("path=/x/PipelineCache/missing ret=-1 size=-1\n") == 4, "repeated failure of one path: 4 lines");
    // The counters are per (op, path), and interleaved failures of two paths do not disturb each other.
    for (int i = 0; i < 100; i++) {
        kl_fscache_note("stat", "/x/PipelineCache/missing", -1, -1);
        kl_fscache_note("open", "/x/PipelineCache/other", -1, -1);
    }
    CHECK(count("[fs-cache] stat path=/x/PipelineCache/missing") == 4 && count("[fs-cache] open path=/x/PipelineCache/other") == 4 &&
          count("[fs-cache]") == 12, "stat and open, and each path, have their own failure counter");
    kl_fscache_note("open", "/x/PipelineCache/missing", 5, 9);
    kl_fscache_note("open", "/x/PipelineCache/missing", 5, 9);
    CHECK(count("path=/x/PipelineCache/missing ret=5 size=9") == 1 && count("[fs-cache]") == 13, "a success after the failure lines is logged once");
    // Many failing paths still hit the global cap of 64 (4 lines each: 16 paths fill it), the last line says so once.
    reset();
    for (int r = 0; r < 20; r++)
        for (int i = 0; i < 20; i++) {
            char p[64];
            snprintf(p, sizeof p, "/x/PipelineCache/m%d", i);
            kl_fscache_note("open", p, -1, -1);
        }
    CHECK(count("[fs-cache]") == 64 && count("(fs-cache cap reached)") == 1, "failing paths: global cap of 64 kept");
    CHECK(!kl_fscache_wanted("/x/PipelineCache/new"), "filter closed at the cap");

    // kl_fscache_open: size comes from the fd; failure: ret -1 size -1.
    reset();
    char dir[] = "/tmp/t_fscache_d.XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char path[128], other[128];
    snprintf(path, sizeof path, "%s/PipelineCache.bin", dir);
    snprintf(other, sizeof other, "%s/other.bin", dir);
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && write(fd, "0123456789", 10) == 10, "create file");
    close(fd);
    fd = open(path, O_RDONLY);
    kl_fscache_open(path, fd);
    char want[256];
    snprintf(want, sizeof want, "[fs-cache] open path=%s ret=%d size=10\n", path, fd);
    CHECK(count(want) == 1, "open: ret = fd, size from fstat");
    close(fd);
    int bad = open("/nonexistent/PipelineCache/x", O_RDONLY);
    errno = ENOENT;
    kl_fscache_open("/nonexistent/PipelineCache/x", bad);
    CHECK(bad < 0 && count("path=/nonexistent/PipelineCache/x ret=-1 size=-1\n") == 1 && errno == ENOENT, "open failure");
    fd = open(other, O_CREAT | O_WRONLY, 0600);
    kl_fscache_open(other, fd);
    CHECK(count("other.bin") == 0, "unmatched file: no line");
    close(fd);
    unlink(path); unlink(other); rmdir(dir);

    dup2(savedfd, 2);
    if (fails) { printf("t_fscache: %d FAILED\n", fails); return 1; }
    printf("t_fscache: ok\n");
    return 0;
}
