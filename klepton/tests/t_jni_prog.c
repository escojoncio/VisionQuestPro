// Host test of the [jni-prog] claim (kl_jni.c, KL-TEST jniprog block, cut out at build time): 3 printed calls per
// class.name, 50 in all, a call over its name's limit does not use one of the 50.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "jniprog.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    static const char *const cls = "com/epicgames/ue4/GameActivity", *const cls2 = "other/Cls";
    // A name called all the time: 3 printed, the rest not.
    int printed = 0, first = -1, last = -1;
    for (int i = 0; i < 1000; i++) {
        int idx = klj_prog_claim(cls, "getCacheDir");
        if (idx >= 0) { if (!printed) first = idx; last = idx; printed++; }
    }
    CHECK(printed == KLJ_PROG_PER_NAME && first == 0 && last == 2, "getCacheDir: 3 printed, indexes 0..2");
    CHECK(klj_prog_total == 3, "refused calls do not use the 50");
    // Another name, and the same name of another class, have their own 3.
    CHECK(klj_prog_claim(cls, "ShowProgressDialog") == 3, "other name: next index");
    CHECK(klj_prog_claim(cls2, "getCacheDir") == 4, "same name, other class: own counter");
    CHECK(klj_prog_claim(cls, "ShowProgressDialog") == 5 && klj_prog_claim(cls, "ShowProgressDialog") == 6, "up to 3");
    CHECK(klj_prog_claim(cls, "ShowProgressDialog") == -1, "4th of a name refused");
    // Distinct pointers with equal text are one name.
    static char copy[32], names[200][32];   // the claim keeps the pointers (kl_jni.c: g_wanted entries, never freed)
    snprintf(copy, sizeof copy, "%s", "UpdateProgressDialog");
    CHECK(klj_prog_claim(cls, copy) == 7, "UpdateProgressDialog");
    snprintf(copy, sizeof copy, "%s", "UpdateProgressDialog");
    CHECK(klj_prog_claim(cls, copy) == 8, "equal text, another pointer: same counter");
    // 50 in all: many different names fill the table; the 50th index is 49, then nothing, and no table overflow.
    int got = 9, ok = 1;
    for (int i = 0; i < 200; i++) {
        snprintf(names[i], sizeof names[i], "name%d", i);
        int idx = klj_prog_claim(cls, names[i]);
        if (idx >= 0) { if (idx != got) ok = 0; got++; }
    }
    CHECK(ok && got == KLJ_PROG_MAX && klj_prog_total == KLJ_PROG_MAX, "50 printed in all");
    CHECK(klj_prog_claim(cls, "getCacheDir") == -1 && klj_prog_claim(cls, "late") == -1, "nothing after the 50");
    if (fails) { printf("t_jni_prog: %d FAILED\n", fails); return 1; }
    printf("t_jni_prog: ok\n");
    return 0;
}
