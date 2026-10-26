// Host test of the guest-error ring (kl_va_handlers.c, KL-TEST lring block, cut out at build time): which log lines are
// kept, repeats counted, the 32-line wrap, and the "[fatal] last guest errors:" text the crash dump and the ForceQuit
// dump emit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lring.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static char out[16384];
static size_t outn, emits;
static void emit(const char *b, size_t n) { memcpy(out + outn, b, n); outn += n; out[outn] = 0; emits++; }
static const char *dump(void) { outn = emits = 0; out[0] = 0; kl_log_ring_dump(emit); return out; }

int main(void) {
    // Nothing logged yet.
    CHECK(!strcmp(dump(), "[fatal] last guest errors: (none logged)\n"), "empty ring");

    // The cheap pre-check.
    CHECK(kl_log_ring_wants(6, "Foo") && kl_log_ring_wants(7, NULL), "ERROR and FATAL of any tag");
    CHECK(kl_log_ring_wants(3, "UE4") && kl_log_ring_wants(3, "UE") && kl_log_ring_wants(4, "Unity") &&
          kl_log_ring_wants(3, "UnityMain"), "UE4 / UE / Unity tags at any priority");
    CHECK(!kl_log_ring_wants(5, "Foo") && !kl_log_ring_wants(3, NULL) && !kl_log_ring_wants(3, "UE45"), "other lines");

    // What is kept: a quiet line of the engine tag is not, a fatal one is.
    kl_log_ring_note(3, "UE4", "LogInit: Display: hello");
    kl_log_ring_note(3, "Foo", "Fatal in a tag we do not watch");
    kl_log_ring_note(5, "Foo", "warning");
    kl_log_ring_note(3, "UE4", NULL);
    CHECK(strstr(dump(), "(none logged)"), "filtered lines are not stored");
    kl_log_ring_note(3, "UE4", "LogWindows: Fatal error: 0xc0000005");
    kl_log_ring_note(4, "UE", "Assertion failed: Ptr != nullptr");
    kl_log_ring_note(3, "Unity", "Error: shader compile");
    kl_log_ring_note(6, "Foo", "plain error\n");
    kl_log_ring_note(7, "abort", "multi\nline\r\n\n");
    CHECK(!strcmp(dump(), "[fatal] last guest errors:\n"
                          "  D/UE4: LogWindows: Fatal error: 0xc0000005\n"
                          "  I/UE: Assertion failed: Ptr != nullptr\n"
                          "  D/Unity: Error: shader compile\n"
                          "  E/Foo: plain error\n"
                          "  F/abort: multi line\n"), "kept lines in order, newlines flattened, trailing ones dropped");
    CHECK(emits == 6, "one emit per line plus the header");

    // Consecutive identical lines are one line with a counter; a different line in between starts a new one.
    for (int i = 0; i < 1000; i++) kl_log_ring_note(6, "Spam", "GL_INVALID_OPERATION\n");
    CHECK(strstr(dump(), "  E/Spam: GL_INVALID_OPERATION (x1000)\n") && strstr(out, "  F/abort: multi line\n"),
          "repeat counter, earlier lines kept");
    kl_log_ring_note(6, "Spam", "GL_INVALID_OPERATION");
    kl_log_ring_note(6, "Other", "x");
    kl_log_ring_note(6, "Spam", "GL_INVALID_OPERATION");
    dump();
    CHECK(strstr(out, "(x1001)\n") && strstr(out, "  E/Other: x\n  E/Spam: GL_INVALID_OPERATION\n"), "a line in between breaks the run");

    // A line longer than a slot is cut so that the counter still fits.
    char big[1000];
    memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
    kl_log_ring_note(6, "T", big);
    kl_log_ring_note(6, "T", big);
    dump();
    const char *e = strstr(out, "  E/T: ");
    const char *nl = e ? strchr(e, '\n') : NULL;
    CHECK(e && nl && (size_t)(nl - (e + 2)) == KL_LRING_BASE + 5 /* " (x2)" */, "long line cut to the base size, counter kept");
    CHECK(nl && nl[-1] == ')', "counter at the end");

    // Wrap: 40 more lines leave exactly the last 32, oldest first.
    for (int i = 0; i < 40; i++) {
        char m[32];
        snprintf(m, sizeof m, "line %d", i);
        kl_log_ring_note(6, "W", m);
    }
    dump();
    int lines = 0;
    for (const char *p = out; (p = strchr(p, '\n')); p++) lines++;
    CHECK(lines == 1 + KL_LRING_N, "header + 32 lines");
    CHECK(strstr(out, "E/W: line 8\n") && !strstr(out, "E/W: line 7\n") && strstr(out, "E/W: line 39\n"), "oldest 8, newest 39");
    CHECK(strstr(out, "line 8\n") < strstr(out, "line 39\n"), "oldest first");

    if (fails) return 1;
    printf("t_lring ok\n");
    return 0;
}
