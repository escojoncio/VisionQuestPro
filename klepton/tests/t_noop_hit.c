// `[jni] noop_hit` dedupe of kl_jni.c (KL-TEST noophit block, cut out at build time): each method is claimed once,
// and no more than 128 are ever claimed.
#include <pthread.h>
#include <stdio.h>

#include "noophit.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    static char keys[200];
    int fresh = 0;
    CHECK(klj_noop_claim(&keys[0]) == 1, "first sight is claimed");
    CHECK(klj_noop_claim(&keys[0]) == 0, "a repeat is not");
    fresh = 1;
    for (int i = 1; i < 200; i++) fresh += klj_noop_claim(&keys[i]);
    CHECK(fresh == 128, "exactly 128 distinct methods are ever claimed");
    CHECK(klj_noop_claim(&keys[0]) == 0, "a claimed one stays refused");
    CHECK(klj_noop_claim(&keys[199]) == 0, "past the limit nothing is claimed");
    if (!fails) puts("t_noop_hit: ok");
    return fails ? 1 : 0;
}
