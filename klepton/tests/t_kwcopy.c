// Host test of the logcat keyword copy (kl_va_handlers.c, KL-TEST kwcopy block, cut out at build time): the run-state
// words keep their behaviour, the shader-cache / MR words have a sub-cap each and a budget of 1000 lines of their own, so they
// cannot push the run-state words out (nor the other way round). Exception / MR-scene words (Exception, NullReference, MRUK,
// OVRPlugin, Scene, Room, Boundary, Anchor) have a third budget of 400 lines (200 bytes each), fed by the lines the two others
// did not take (MRUK / anchor continue there once their sub-cap is spent). Inside it Scene / Room have 40 lines each and the
// Exception / NullReference group and the MRUK / OVRPlugin / Boundary / Anchor group keep 100 lines reserved.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include "kwcopy.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int capfd, savedfd;
static void cap_reset(void) { if (ftruncate(capfd, 0) != 0) abort(); }
static size_t count(const char *needle) {
    char *b = calloc(1, 1 << 20);
    ssize_t n = pread(capfd, b, (1 << 20) - 1, 0);
    size_t c = 0;
    if (n > 0) for (const char *p = b; (p = strstr(p, needle)); p += strlen(needle)) c++;
    free(b);
    return c;
}
static int has(const char *needle) { return count(needle) > 0; }
static void reset(int cap) {
    g_kw_cap = cap; g_kw_n = g_kw_new_n = g_kw_mr_n = 0; memset(g_kw_sub_n, 0, sizeof g_kw_sub_n);
    memset(g_kw_mr_grp, 0, sizeof g_kw_mr_grp); memset(g_kw_mr_word, 0, sizeof g_kw_mr_word); cap_reset();
}
static void feed(int prio, const char *tag, const char *msg, int times) {
    for (int i = 0; i < times; i++) kl_log_keyword_copy(prio, tag, msg);
}

int main(void) {
    char tmpl[] = "/tmp/t_kwcopy.XXXXXX";
    capfd = mkstemp(tmpl);
    CHECK(capfd >= 0, "mkstemp");
    unlink(tmpl);
    fcntl(capfd, F_SETFL, O_APPEND);   // after a truncate the next write goes to offset 0
    savedfd = dup(2);
    dup2(capfd, 2);   // stderr (unbuffered) now lands in the capture file
    setenv("KL_LOG_KW_MAX", "", 1);
    reset(-1);

    // The default cap is read from the environment on first use: unset / empty = 1500.
    CHECK(kl_log_keyword_open() && g_kw_cap == 1500, "default cap 1500");
    setenv("KL_LOG_KW_MAX", "0", 1); reset(-1);
    CHECK(!kl_log_keyword_open(), "KL_LOG_KW_MAX=0 is off");
    feed(4, "Foo", "checkpoint", 1); feed(4, "UE4", "PipelineCache", 1);
    CHECK(count("[unity-kw]") == 0, "off prints nothing");
    unsetenv("KL_LOG_KW_MAX");

    // Current words: same behaviour (word start, INFO and above or Unity tag).
    reset(-1);
    kl_log_keyword_copy(4, "Foo", "Reached checkpoint 3");
    CHECK(has("I/Foo: Reached checkpoint 3"), "old word at INFO");
    kl_log_keyword_copy(3, "Foo", "checkpoint");
    kl_log_keyword_copy(3, "UE4", "checkpoint");
    CHECK(count("[unity-kw]") == 1, "old word at DEBUG: dropped for other tags and for UE4");
    kl_log_keyword_copy(3, "UnityMain", "Player died");
    CHECK(has("D/UnityMain: Player died"), "old word at DEBUG with a Unity tag");
    kl_log_keyword_copy(5, "Foo", "predicted");     // "die" inside a word: not at a word start
    kl_log_keyword_copy(5, "Foo", "abc1fail");
    CHECK(count("[unity-kw]") == 2, "mid-word / after a digit is not a hit");
    kl_log_keyword_copy(5, "Foo", "3fail");         // preceded by a digit: skipped like before
    kl_log_keyword_copy(5, "Foo", "no match here");
    kl_log_keyword_copy(5, "Foo", NULL);
    CHECK(count("[unity-kw]") == 2, "digit prefix, no keyword, NULL msg");
    for (unsigned i = 0; i < 11; i++) {             // each current word still hits
        reset(-1);
        kl_log_keyword_copy(4, "Foo", g_kw[i].w);
        CHECK(count("[unity-kw]") == 1 && g_kw[i].sub == 0, "current word hits");
    }

    // New words, each (case-insensitive, word start), at INFO with an unrelated tag.
    static const char *const nw[] = { "PipelineCache", "upipelinecache", "ShaderPipelineCache", "PSO compile", "precompiling",
        "Shader Cache saved", "passthrough on", "OVRManager init", "MRUK ready", "anchors found" };
    for (unsigned i = 0; i < sizeof nw / sizeof *nw; i++) {
        reset(-1);
        kl_log_keyword_copy(4, "Foo", nw[i]);
        CHECK(count("[unity-kw]") == 1, nw[i]);
    }
    reset(-1);
    kl_log_keyword_copy(4, "Foo", "Found the Room");
    kl_log_keyword_copy(4, "Foo", "LogRoom: SpatialAnchor");   // "Room" is not a word; Anchor is mid-word
    CHECK(count("[unity-kw]") == 1 && has("Found the Room"), "Room is a mr word at its start; mid-word (LogRoom, SpatialAnchor) does not hit");
    reset(-1);
    // New words are allowed at any priority for tags UE4 / UE, and for Unity; not for other tags.
    kl_log_keyword_copy(3, "UE4", "LogRHI: PipelineCache opened");
    kl_log_keyword_copy(3, "UE", "PSO precompile");
    kl_log_keyword_copy(3, "Unity", "anchor");
    CHECK(count("[unity-kw]") == 3, "new words at DEBUG for UE4 / UE / Unity");
    kl_log_keyword_copy(3, "Foo", "anchor");
    kl_log_keyword_copy(3, "UE45", "anchor");
    kl_log_keyword_copy(3, NULL, "anchor");
    CHECK(count("[unity-kw]") == 3, "new words at DEBUG for other tags: dropped");

    // Sub-cap: 150 anchor lines -> 100 printed, the 100th says so.
    reset(-1);
    feed(4, "Foo", "anchor created", 150);
    CHECK(g_kw_new_n == 100 && g_kw_mr_n == 50 && count("[unity-kw]") == 150, "sub-cap 100, the other 50 go to the mr budget");
    CHECK(count("(kw word cap reached)") == 1 && !has("(kw line cap reached)"), "marker on the last line only");
    feed(4, "Foo", "MRUK ready", 150);
    CHECK(count("MRUK ready") == 150, "another word has its own sub-cap (+ mr budget)");
    CHECK(g_kw_new_n == 200 && g_kw_n == 0 && g_kw_mr_n == 100, "new words count only printed lines (200) in their own counter; the run-state budget is untouched");

    // A line that also holds a current word still counts against that word once the new word is used up.
    feed(4, "Foo", "anchor died", 5);
    CHECK(count("anchor died") == 5, "exhausted new word falls through to a current word");

    // The new words cannot displace the current ones: all ten words at their sub-cap = 1000 lines, a budget of their own.
    reset(-1);
    for (unsigned i = 0; i < sizeof nw / sizeof *nw; i++) feed(4, "Foo", nw[i], 150);
    CHECK(g_kw_new_n >= 1000 && g_kw_n == 0 && g_kw_mr_n == 100 && count("[unity-kw]") == 1100,
          "ten new words use 1000 lines at most (+100 mr overflow of MRUK / anchors), g_kw_n untouched");
    CHECK(count("(kw new-word line cap reached)") == 1 && !has("(kw line cap reached)"), "new-word budget marker once");
    feed(4, "Foo", "anchor created", 20);
    CHECK(count("[unity-kw]") == 1120 && g_kw_new_n >= 1000, "new words: after their budget only the mr budget takes anchor lines");
    feed(4, "Foo", "mission started", 2000);
    CHECK(count("mission started") == 1500, "current words still get the whole 1500 after the new words are exhausted");
    CHECK(count("(kw line cap reached)") == 1 && g_kw_n >= 1500, "global cap marker once");
    // A line with an exhausted new word and a current word counts against the current word.
    reset(-1);
    for (unsigned i = 0; i < sizeof nw / sizeof *nw; i++) feed(4, "Foo", nw[i], 150);
    feed(4, "Foo", "anchor died", 3);
    CHECK(count("anchor died") == 3 && g_kw_n == 3, "exhausted new-word budget falls through to a current word");
    // ... and the other way round: current words exhausted, new words still print.
    reset(-1);
    feed(4, "Foo", "mission started", 2000);
    CHECK(count("[unity-kw]") == 1500 && kl_log_keyword_open(), "run-state budget used up: the logger is still open for new words");
    feed(4, "Foo", "anchor created", 150);
    CHECK(count("anchor created") == 150 && g_kw_new_n == 100 && g_kw_mr_n == 50, "new words print after the run-state budget is used up");
    feed(4, "Foo", "anchor died", 5);
    CHECK(count("anchor died") == 5 && g_kw_mr_n == 55, "anchor sub-cap and die used: the mr budget takes the line");
    feed(4, "Foo", "MRUK ready", 150);
    CHECK(count("MRUK ready") == 150, "another new word still has its sub-cap (+ mr budget)");
    // ... and current words alone still reach the whole global cap, as before.
    reset(-1);
    feed(4, "Foo", "mission started", 2000);
    CHECK(count("[unity-kw]") == 1500, "current words alone: full 1500 as before");
    // A custom global cap bounds each budget: new words obey min(1000, cap), current words cap.
    reset(50);
    feed(4, "Foo", "anchor", 200);
    CHECK(count("[unity-kw]") == 100 && has("(kw new-word line cap reached)") && has("(kw mr line cap reached)"),
          "custom cap below the sub-cap: 50 new-word + 50 mr");
    feed(4, "Foo", "mission", 200);
    CHECK(count("[unity-kw]") == 150 && has("(kw line cap reached)"), "custom cap: current words have their own 50");

    // Exception / MR-scene words: own budget of 400 lines, 200 bytes per line, not taken by the other two budgets.
    static const char *const mw[] = { "System.NullReferenceException: x", "ArgumentException at Foo", "NullReference in Update",
        "OVRPlugin init", "Scene loaded", "Room created", "Boundary visible", "Anchor saved", "MRUK ready" };
    for (unsigned i = 0; i < sizeof mw / sizeof *mw; i++) {
        reset(-1);
        kl_log_keyword_copy(4, "Foo", mw[i]);
        CHECK(count("[unity-kw]") == 1 && g_kw_mr_n == (i >= 7 ? 0 : 1), mw[i]);   // MRUK / anchor are sub-cap words first
    }
    reset(-1);
    kl_log_keyword_copy(4, "Foo", "SceneManager"); kl_log_keyword_copy(4, "Foo", "Unobscene"); kl_log_keyword_copy(4, "Foo", "Bedroom");
    CHECK(count("[unity-kw]") == 1 && g_kw_mr_n == 1, "mr words: Scene at a word start only (Exception is the only mid-word one)");
    reset(-1);
    kl_log_keyword_copy(3, "Foo", "Exception"); kl_log_keyword_copy(3, NULL, "Scene");
    CHECK(count("[unity-kw]") == 0, "mr words at DEBUG with other tags: dropped");
    kl_log_keyword_copy(3, "Unity", "Exception"); kl_log_keyword_copy(3, "UE4", "Room");
    CHECK(count("[unity-kw]") == 2, "mr words at DEBUG with Unity / UE4 tags");
    reset(-1);
    feed(4, "Foo", "NullReferenceException", 600);
    CHECK(count("[unity-kw]") == 300 && g_kw_n == 0 && g_kw_new_n == 0, "one group alone stops at 300: the other keeps 100 reserved");
    feed(4, "Foo", "Boundary visible", 200);
    CHECK(count("[unity-kw]") == 400 && count("Boundary visible") == 100 && count("(kw mr line cap reached)") == 1, "mr budget 400, marker once");
    feed(4, "Foo", "mission started", 20);
    CHECK(count("mission started") == 20, "run-state words unaffected by an exhausted mr budget");
    reset(-1);
    { char big[600]; memset(big, 'x', sizeof big); memcpy(big, "Exception ", 10); big[599] = 0;
      kl_log_keyword_copy(4, "Foo", big);
      char exp[300]; memcpy(exp, big, 200); exp[200] = '\n'; exp[201] = 0;
      CHECK(has(exp) && count("[unity-kw]") == 1, "line cut at 200 bytes"); }
    // Scene / Room are capped at 40 each and cannot displace the other two groups, in any order.
    reset(-1);
    feed(4, "Foo", "Scene loaded", 1000); feed(4, "Foo", "Room created", 1000);
    CHECK(count("Scene loaded") == 40 && count("Room created") == 40 && count("(kw mr word cap reached)") == 2, "Scene / Room: 40 lines each, marker on the last");
    feed(4, "Foo", "System.NullReferenceException: x", 1000);
    feed(4, "Foo", "Boundary visible", 1000);
    feed(4, "Foo", "MRUK ready", 1000);
    CHECK(count("NullReferenceException") >= 100 && count("Boundary visible") >= 100 && count("MRUK ready") >= 100,
          "saturating with Scene / Room displaces neither NullReference nor MRUK / Boundary");
    CHECK(g_kw_mr_n == 400 && g_kw_mr_grp[1] >= 100 && g_kw_mr_grp[2] >= 100 && g_kw_mr_grp[1] + g_kw_mr_grp[2] + 80 == 400, "total 400");
    reset(-1);   // the other way round: the two groups first, Scene / Room still get their 40
    feed(4, "Foo", "System.NullReferenceException: x", 1000); feed(4, "Foo", "Boundary visible", 1000);
    feed(4, "Foo", "Scene loaded", 1000);
    CHECK(count("Scene loaded") == 0 && g_kw_mr_n == 400, "budget used up by the two groups: Scene finds nothing left");
    reset(-1);
    feed(4, "Foo", "Scene loaded", 5); feed(4, "Foo", "Anchor saved", 1);
    CHECK(count("Scene loaded") == 5 && count("Anchor saved") == 1 && g_kw_mr_grp[0] == 5, "below the caps nothing is dropped");
    reset(100);
    feed(4, "Foo", "Exception", 300);
    CHECK(count("[unity-kw]") == 100, "mr budget bounded by a custom global cap");

    dup2(savedfd, 2);
    if (fails) { printf("t_kwcopy: %d FAILED\n", fails); return 1; }
    printf("t_kwcopy: ok\n");
    return 0;
}
