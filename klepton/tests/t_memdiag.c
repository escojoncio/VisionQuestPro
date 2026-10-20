// Host test (Linux or macOS) of the memory-jump diagnostics. The functions are cut out of the shipping sources by
// their KL-TEST markers (kl_libc.c: rate trigger, range lookup, per-file statistics; kl_app.c: entitlement parsing),
// so the test runs the real code:
//   make memdiag        (or tools/ci/host-tests.sh)
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/loader.h>
#else
struct load_command { uint32_t cmd, cmdsize; };
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; };
#define LC_CODE_SIGNATURE 0x1d
#define LC_SEGMENT_64 0x19
#endif

int kl_obbmap_is_vfd(int fd) { return fd == 99999; }

#include "libc_blocks.inc"
#include "app_blocks.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void test_rate(void) {
    uint64_t slope;
    // +1.4 GiB in 1 s with 2 GiB left: 1.43 s to go.
    double ttl = kl_rate_ttl(5000ull << 20, (5000ull + 1434) << 20, 1000000000ull, 2048ull << 20, &slope);
    CHECK(ttl > 1.4 && ttl < 1.5 && (slope >> 20) == 1434, "rate: 1.4 GB/s, 2 GB left");
    // The same slope with 20 GiB left is 14 s away: not under the 6 s threshold.
    ttl = kl_rate_ttl(0, 1434ull << 20, 1000000000ull, 20480ull << 20, &slope);
    CHECK(ttl > 14.0 && ttl < 14.5, "rate: far from the ceiling");
    // Not rising / no time: -1 and a zero slope.
    ttl = kl_rate_ttl(900, 900, 1000, 100, &slope);
    CHECK(ttl < 0 && slope == 0, "rate: flat");
    ttl = kl_rate_ttl(900, 100, 1000, 100, &slope);
    CHECK(ttl < 0 && slope == 0, "rate: falling");
    ttl = kl_rate_ttl(0, 100, 0, 100, &slope);
    CHECK(ttl < 0, "rate: dt 0");
    // A rise too small for one byte per second is "never".
    ttl = kl_rate_ttl(0, 1, 4000000000ull, 100, &slope);
    CHECK(slope == 0 && ttl > 1e8, "rate: slope rounds to zero");
}

static void test_ranges(void) {
    uint64_t r[2][2] = { {100, 100}, {300, 100} };      // [100,200) [300,400)
    CHECK(kl_range_hit(r, 2, 150, 10) == 1, "range: inside the first");
    CHECK(kl_range_hit(r, 2, 200, 100) == 0, "range: the gap");
    CHECK(kl_range_hit(r, 2, 199, 2) == 1, "range: straddles the end of the first");
    CHECK(kl_range_hit(r, 2, 390, 500) == 1, "range: starts inside the last");
    CHECK(kl_range_hit(r, 2, 0, 100) == 0, "range: ends where the first starts");
    CHECK(kl_range_hit(r, 2, 400, 10) == 0, "range: starts where the last ends");
    CHECK(kl_range_hit(r, 2, 150, 200) == 1, "range: spans both");
    CHECK(kl_range_hit(r, 2, 50, 300) == 1, "range: covers the first and more");
    CHECK(kl_range_hit(r, 0, 150, 10) == 0, "range: no ranges");
    uint64_t big[1][2] = { {0, 1000} };
    CHECK(kl_range_hit(big, 1, 500, 1) == 1, "range: one range around the region");
}

static char *dump_to_string(const char *why) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    kl_fstat_dump(f, why);
    fclose(f);
    return buf;
}

static void test_fstat(void) {
    char path[] = "/tmp/t_memdiag_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "fstat: temp file");
    if (fd < 0) return;
    char chunk[4096];
    memset(chunk, 7, sizeof chunk);
    for (int i = 0; i < 25; i++) CHECK(write(fd, chunk, sizeof chunk) == (ssize_t)sizeof chunk, "write");   // 100 KiB
    lseek(fd, 0, SEEK_SET);
    int fd1 = open(path, O_RDONLY);
    kl_fstat_open(path, fd1);
    char b[4096];
    CHECK(pread(fd1, b, sizeof b, 8192) == (ssize_t)sizeof b, "pread");
    kl_fstat_read(fd1, sizeof b);
    kl_fstat_read(fd1, 0);                                  // ignored
    kl_fstat_read(-1, 100);                                 // ignored
    kl_fstat_read(100000, 100);                             // out of range: ignored
    void *m = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd1, 0);
    CHECK(m != MAP_FAILED, "mmap");
    kl_fstat_mmap(fd1, 8192);
    char *dump = dump_to_string("live");                    // still open: the position counts, nothing read by read() yet
    CHECK(strstr(dump, "1 files, 1 opens") != NULL, "fstat: one file, one open");
    CHECK(strstr(dump, "size=100K opens=1 read=4K mmap=8K(x1)") != NULL, "fstat: pread and mmap before close");
    free(dump);
    CHECK(read(fd1, b, 1000) == 1000, "read");               // sequential: only the position knows
    kl_fstat_close(fd1);
    close(fd1);
    dump = dump_to_string("after close");
    CHECK(strstr(dump, "read=4K") != NULL, "fstat: pread 4096 + read 1000 = 5096 B (4K) after close");
    free(dump);
    // The same path again reuses the entry; a second descriptor counts too.
    int fd2 = open(path, O_RDONLY);
    kl_fstat_open(path, fd2);
    dump = dump_to_string("reopened");
    CHECK(strstr(dump, "1 files, 2 opens") != NULL && strstr(dump, "opens=2") != NULL, "fstat: same path, one entry");
    free(dump);
    kl_fstat_close(fd2);
    close(fd2);
    munmap(m, 8192);
    // A descriptor that is not a regular file is not tracked and clears a stale slot.
    int dn = open("/dev/null", O_RDONLY);
    kl_fstat_open("/dev/null", dn);
    kl_fstat_read(dn, 50);
    kl_fstat_close(dn);
    close(dn);
    dump = dump_to_string("dv0");
    CHECK(strstr(dump, "/dev/null") == NULL && strstr(dump, "1 files") != NULL, "fstat: /dev/null not tracked");
    free(dump);
    // Table overflow: 500 distinct names against a table of KL_FS_ENTRIES; nothing breaks, the rest is counted apart.
    int ofd = open(path, O_RDONLY);
    for (int i = 0; i < 500; i++) {
        char name[64];
        snprintf(name, sizeof name, "/virtual/file_%d.bin", i);
        kl_fstat_open(name, ofd);
        kl_fstat_read(ofd, 10);
        kl_fstat_close(ofd);
    }
    close(ofd);
    dump = dump_to_string("overflow");
    CHECK(strstr(dump, "opens of files past the table") != NULL, "fstat: overflow reported");
    CHECK(strstr(dump, "384 files") != NULL, "fstat: table capped");
    free(dump);
    kl_fstat_open(path, -1);                                // negative / huge fd: ignored
    kl_fstat_open(NULL, 3);
    kl_fstat_close(1 << 20);
    close(fd);
    unlink(path);
}

static void test_entitlements(void) {
    const char *xml =
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>\n"
        "<key>application-identifier</key><string>ABCDE.app</string>\n"
        "<key>com.apple.developer.kernel.increased-memory-limit</key>\n<true/>\n"
        "<key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>\n"
        "<key>get-task-allow</key><true/>\n"
        "<key>com.apple.developer.foo</key><integer>42</integer>\n"
        "<key>aps-environment</key><false/>\n"
        "</dict></plist>";
    size_t xl = strlen(xml);
    // A SuperBlob: header, two index entries (a CodeDirectory slot, then the entitlements slot), their blobs.
    size_t cd_off = 12 + 16, ent_off = cd_off + 16, total = ent_off + 8 + xl;
    uint8_t *sb = calloc(1, total);
    be32(sb, 0xfade0cc0u); be32(sb + 4, (uint32_t)total); be32(sb + 8, 2);
    be32(sb + 12, 0); be32(sb + 16, (uint32_t)cd_off);
    be32(sb + 20, 5); be32(sb + 24, (uint32_t)ent_off);
    be32(sb + cd_off, 0xfade0c02u); be32(sb + cd_off + 4, 16);
    be32(sb + ent_off, 0xfade7171u); be32(sb + ent_off + 4, (uint32_t)(8 + xl));
    memcpy(sb + ent_off + 8, xml, xl);
    const char *x = NULL;
    size_t xn = 0;
    CHECK(kl_superblob_xml(sb, total, &x, &xn) == 1 && xn == xl && x == (const char *)sb + ent_off + 8, "ent: blob found");
    char line[900];
    kl_ent_format("signed", x, xn, line, sizeof line);
    CHECK(strstr(line, "[ent] signed:") == line, "ent: tag");
    CHECK(strstr(line, " *kernel.increased-memory-limit=true") != NULL, "ent: memory key flagged with its value");
    CHECK(strstr(line, " *kernel.extended-virtual-addressing=true") != NULL, "ent: virtual addressing");
    CHECK(strstr(line, " *get-task-allow=true") != NULL, "ent: debug flag");
    CHECK(strstr(line, " developer.foo=42") == NULL && strstr(line, " foo=42") != NULL, "ent: integer value, prefix shortened");
    CHECK(strstr(line, " aps-environment=false") != NULL, "ent: false value");
    CHECK(strstr(line, "[6 keys, 3 memory/kernel/jit/debug]") != NULL, "ent: counts");
    // Cut short in every possible place: never read past `size`, never found.
    for (size_t n = 0; n < total; n++) {
        const char *xx = NULL; size_t xl2 = 0;
        int r = kl_superblob_xml(sb, n, &xx, &xl2);
        if (n < total) CHECK(!r || (xx + xl2 <= (const char *)sb + n), "ent: truncated superblob stays inside");
    }
    be32(sb + ent_off + 4, 0xffffffffu);                 // a length that wraps
    CHECK(kl_superblob_xml(sb, total, &x, &xn) == 0, "ent: absurd blob length rejected");
    be32(sb + ent_off + 4, 4);                           // shorter than its own header
    CHECK(kl_superblob_xml(sb, total, &x, &xn) == 0, "ent: blob shorter than its header rejected");
    be32(sb, 0xdeadbeefu);
    CHECK(kl_superblob_xml(sb, total, &x, &xn) == 0, "ent: wrong magic");
    free(sb);
    // Key list far longer than the line: truncated, still terminated.
    char big[20000];
    size_t w = 0;
    w += (size_t)snprintf(big + w, sizeof big - w, "<dict>");
    for (int i = 0; i < 300; i++) w += (size_t)snprintf(big + w, sizeof big - w, "<key>com.apple.developer.k%d</key><true/>", i);
    kl_ent_format("big", big, w, line, sizeof line);
    CHECK(strlen(line) < sizeof line && strstr(line, "[") != NULL, "ent: long key list bounded");
    kl_ent_format("empty", "", 0, line, sizeof line);
    CHECK(strstr(line, "[0 keys, 0 memory") != NULL, "ent: no keys");

    // Load commands: the code signature is found past other commands; bad sizes are refused.
    uint8_t cmds[64];
    memset(cmds, 0, sizeof cmds);
    struct load_command seg = { LC_SEGMENT_64, 24 };
    memcpy(cmds, &seg, sizeof seg);
    struct linkedit_data_command cs = { LC_CODE_SIGNATURE, 16, 0x1000, 0x200 };
    memcpy(cmds + 24, &cs, sizeof cs);
    uint32_t off = 0, size = 0;
    CHECK(kl_macho_codesig(cmds, 2, 40, &off, &size) == 1 && off == 0x1000 && size == 0x200, "macho: signature found");
    CHECK(kl_macho_codesig(cmds, 2, 30, &off, &size) == 0, "macho: command runs past sizeofcmds");
    CHECK(kl_macho_codesig(cmds, 1, 24, &off, &size) == 0, "macho: no signature command");
    struct load_command bad = { LC_SEGMENT_64, 4 };
    memcpy(cmds, &bad, sizeof bad);
    CHECK(kl_macho_codesig(cmds, 2, 40, &off, &size) == 0, "macho: cmdsize smaller than a command");
}

int main(void) {
    test_rate();
    test_ranges();
    test_fstat();
    test_entitlements();
    if (fails) { printf("t_memdiag: %d FAILED\n", fails); return 1; }
    printf("t_memdiag: ok\n");
    return 0;
}
