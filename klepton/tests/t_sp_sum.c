// "[sp-sum]" line builder of kl_ovrp_spatial.c (KL-TEST spsum block, cut out at build time): token format,
// the 900-character cap with the line split repeating the prefix, and no token ever cut.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spsum.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static char g_lines[64][1100];
static int g_n;
static void collect(const char *line, void *ctx) {
    (void)ctx;
    if (g_n < 64) snprintf(g_lines[g_n], sizeof g_lines[0], "%s", line);
    g_n++;
}

int main(void) {
    char t[64];
    sp_sum_tok_fn(t, sizeof t, "ovrp_CreateSpatialAnchor", 12, 0);
    CHECK(strcmp(t, "CreateSpatialAnchor=12/0") == 0, "token drops ovrp_ and shows calls/lastret");
    sp_sum_tok_fn(t, sizeof t, "ovrp_QuerySpaces", 3, -1004);
    CHECK(strcmp(t, "QuerySpaces=3/-1004") == 0, "negative last return");
    sp_sum_tok_fn(t, sizeof t, "Foo", 1, 7);
    CHECK(strcmp(t, "Foo=1/7") == 0, "name without the prefix is kept");

    const char *a[] = {"CreateSpatialAnchor=12/0", "Poll=300", "ev49=2"};
    sp_sum_build(65.4, a, 3, 900, collect, NULL);
    CHECK(g_n == 1, "one line when it fits");
    CHECK(strcmp(g_lines[0], "[sp-sum] t=65s CreateSpatialAnchor=12/0 Poll=300 ev49=2") == 0, "line format");

    g_n = 0;
    sp_sum_build(5, a, 0, 900, collect, NULL);
    CHECK(g_n == 0, "no tokens, no line");

    // 60 tokens of ~30 chars: must split under the cap, every line with the prefix, every token intact, in order.
    static char tokb[60][40];
    const char *tk[60];
    for (int i = 0; i < 60; i++) { snprintf(tokb[i], sizeof tokb[i], "SomeLongFunctionName%02d=%d/-1", i, i * 7); tk[i] = tokb[i]; }
    g_n = 0;
    sp_sum_build(120, tk, 60, 900, collect, NULL);
    CHECK(g_n >= 2, "split in several lines");
    int seen = 0, ok = 1;
    for (int l = 0; l < g_n && l < 64; l++) {
        CHECK(strlen(g_lines[l]) <= 900, "line within the cap");
        CHECK(strncmp(g_lines[l], "[sp-sum] t=120s ", 16) == 0, "every line repeats the prefix");
        char *p = g_lines[l] + 16, *save = NULL;
        for (char *w = strtok_r(p, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            if (seen >= 60 || strcmp(w, tk[seen]) != 0) ok = 0;
            seen++;
        }
    }
    CHECK(ok && seen == 60, "all 60 tokens present once, intact, in order");

    // A token that alone exceeds the cap still gets its own line (never cut) as long as the buffer holds it.
    char big[300];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    const char *b[] = {"a=1/0", big, "b=2/0"};
    g_n = 0;
    sp_sum_build(1, b, 3, 100, collect, NULL);
    CHECK(g_n == 3, "oversized token gets a line of its own");
    if (!fails) puts("t_sp_sum: ok");
    return fails ? 1 : 0;
}
