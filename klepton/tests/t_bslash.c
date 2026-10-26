// Host test of kl_libc.c's KL-TEST bslash block (cut out at build time): a guest path holding '\'
// (Unity/Gundam opens "<obb>\gvr_sequences_assets_*.bundle") is resolved as given when that file
// exists (Android allows '\' in names), else with '\' -> '/'. Paths without '\' are untouched.
#include <stdio.h>
#include <string.h>
#include "bslash.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static const char *g_exist[4];
static int exists(const char *p) { for (int i = 0; g_exist[i]; i++) if (!strcmp(g_exist[i], p)) return 1; return 0; }
// Stand-in for kl_guest_path_inner: maps /sdcard -> /host and, unless literal, '\' -> '/'.
static const char *map(const char *p, char *buf, size_t cap, int literal) {
    size_t n = 0;
    const char *pre = !strncmp(p, "/sdcard", 7) ? "/host" : "";
    if (*pre) { snprintf(buf, cap, "%s", pre); n = strlen(buf); p += 7; }
    for (; *p && n + 1 < cap; p++) buf[n++] = (!literal && *p == '\\') ? '/' : *p;
    buf[n] = 0;
    return buf;
}

int main(void) {
    char buf[1024]; int lit, ok; const char *r;

    // Only the '/' spelling exists: fallback finds it.
    g_exist[0] = "/host/obb/pkg/g.bundle"; g_exist[1] = 0;
    r = kl_bs_resolve("/sdcard/obb/pkg\\g.bundle", buf, sizeof buf, map, exists, &lit, &ok);
    CHECK(!strcmp(r, "/host/obb/pkg/g.bundle") && !lit && ok, "slash spelling is the fallback");

    // The file really has '\' in its name: used as given.
    g_exist[0] = "/host/obb/pkg\\g.bundle"; g_exist[1] = 0;
    r = kl_bs_resolve("/sdcard/obb/pkg\\g.bundle", buf, sizeof buf, map, exists, &lit, &ok);
    CHECK(!strcmp(r, "/host/obb/pkg\\g.bundle") && lit && ok, "literal spelling wins when it exists");

    // Neither exists: the '/' spelling comes back (what creation of a new file needs), ok=0.
    g_exist[0] = 0;
    r = kl_bs_resolve("/sdcard/a\\b", buf, sizeof buf, map, exists, &lit, &ok);
    CHECK(!strcmp(r, "/host/a/b") && !lit && !ok, "neither exists -> '/' spelling, ok=0");

    // No '\': mapped as before, no existence probe, flags clear.
    g_exist[0] = 0;
    r = kl_bs_resolve("/sdcard/a/b", buf, sizeof buf, map, exists, &lit, &ok);
    CHECK(!strcmp(r, "/host/a/b") && !lit && !ok, "no backslash untouched");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
