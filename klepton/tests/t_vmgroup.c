// Host test of the [hb] vm grouping (kl_libc.c, KL-TEST vmgroup block, cut out at build time): regions are summed by
// (user_tag, file-backed vs share mode) and formatted top-N + rest. The Mach walk itself is not tested; it only feeds
// kl_vmg_add.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *kl_vm_tag_name(unsigned t) { return t == 3 ? "MALLOC_LARGE" : t == 240 ? "guest_mmap" : t == 0 ? "untagged" : NULL; }
#include "vmgroup.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define M(x) ((uint64_t)(x) << 20)

int main(void) {
    static kl_vmg g[KL_VMG_N];
    char buf[1024];

    // Same (tag, kind) sums; file-backed beats the share mode; a share mode out of range is "other"; tag is one byte.
    kl_vmg_add(g, 3, 0, 2, M(100), M(90), M(5));
    kl_vmg_add(g, 3, 0, 2, M(50), M(40), M(1));
    kl_vmg_add(g, 3, 1, 2, M(7), 0, 0);
    kl_vmg_add(g, 3, 0, 0, M(1), 0, 0);
    kl_vmg_add(g, 3, 0, 99, M(1), 0, 0);
    kl_vmg_add(g, 0x100, 0, 2, M(1), 0, 0);
    CHECK(g[3 * KL_VMG_KINDS + 2].res == M(150) && g[3 * KL_VMG_KINDS + 2].dirty == M(130) &&
          g[3 * KL_VMG_KINDS + 2].swap == M(6), "same tag and kind are summed");
    CHECK(g[3 * KL_VMG_KINDS + 0].res == M(7), "external pager is the file kind whatever the share mode");
    CHECK(g[3 * KL_VMG_KINDS + 9].res == M(2), "share mode 0 and 99 are other");
    CHECK(g[0 * KL_VMG_KINDS + 2].res == M(1), "tag is masked to one byte");

    // Four groups, top 2: ranked by resident + swapped (the swapped-out one wins), the rest is summed.
    memset(g, 0, sizeof g);
    kl_vmg_add(g, 3, 0, 2, M(100), M(100), M(0));        // 100
    kl_vmg_add(g, 240, 0, 2, M(10), M(10), M(500));      // 510: compressed memory ranks first
    kl_vmg_add(g, 77, 0, 4, M(60), M(1), M(0));          // 60, unnamed tag
    kl_vmg_add(g, 0, 1, 1, M(30), M(2), M(3));           // 33 (cow; external=1 makes it "file")
    kl_vmg_format(g, 42, 2, 0, buf, sizeof buf);
    CHECK(strstr(buf, "regions=42 groups=4 total res=200M dirty=113M swap=503M;"), "header and totals");
    char *a = strstr(buf, "guest_mmap/priv=10/10/500");
    char *b = strstr(buf, "MALLOC_LARGE/priv=100/100/0");
    CHECK(a && b && a < b, "top 2 in rank order");
    CHECK(strstr(buf, " rest(2)=90/3/3"), "rest sums the other two groups");
    CHECK(!strstr(buf, "tag77") && !strstr(buf, "untagged"), "groups past the top are not listed");

    // Unnamed tag prints tagN; file kind; no rest when everything fits.
    memset(g, 0, sizeof g);
    kl_vmg_add(g, 77, 1, 2, M(60), M(1), M(0));
    kl_vmg_format(g, 1, 12, 0, buf, sizeof buf);
    CHECK(strstr(buf, "tag77/file=60/1/0") && !strstr(buf, "rest("), "tagN, file kind, no rest");

    // A cut walk: no totals, no rest, only the orientative top.
    memset(g, 0, sizeof g);
    kl_vmg_add(g, 3, 0, 2, M(100), M(100), 0);
    kl_vmg_add(g, 240, 0, 2, M(10), M(10), 0);
    kl_vmg_add(g, 77, 0, 4, M(60), M(1), 0);
    kl_vmg_format(g, 20000, 1, 1, buf, sizeof buf);
    CHECK(strstr(buf, "scanned=20000 regions, NOT totals") && strstr(buf, "MALLOC_LARGE/priv=100/100/0"), "partial: top only");
    CHECK(!strstr(buf, "total res") && !strstr(buf, "rest(") && !strstr(buf, "groups="), "partial: no totals, no rest");

    // Empty map.
    memset(g, 0, sizeof g);
    kl_vmg_format(g, 0, 12, 0, buf, sizeof buf);
    CHECK(strstr(buf, "regions=0 groups=0 total res=0M") && !strstr(buf, "rest("), "empty map");

    // A buffer too small for the line is cut, never overrun (guard bytes).
    memset(g, 0, sizeof g);
    for (unsigned t = 1; t < 40; t++) kl_vmg_add(g, t + 100, 0, 2, M(t), M(t), M(t));
    char small[200 + 16];
    memset(small, 'x', sizeof small);
    kl_vmg_format(g, 39, 12, 0, small, 200);
    CHECK(memchr(small, 0, 200) != NULL, "small buffer is terminated");
    CHECK(small[200] == 'x' && small[215] == 'x', "small buffer is not overrun");

    if (fails) return 1;
    printf("t_vmgroup ok\n");
    return 0;
}
