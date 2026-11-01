// Permission request queue of kl_jni_window.c (KL-TEST permq block, cut out at build time):
// the results are the checkSelfPermission ones (0 / -1), taken at request time, delivered
// later in order, bounded, and nothing is lost or duplicated.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Stand-in for the manifest model: only these are "granted".
static int klj_permission_state(const char *p, const char **why) {
    *why = "";
    return strcmp(p, "android.permission.RECORD_AUDIO") == 0 || strcmp(p, "android.permission.INTERNET") == 0;
}

// Pin accounting stubs: every pin must be matched by one unpin.
static int g_pins, g_unpins, g_pin_obj_a, g_pin_obj_b;
static void kl_jni_pin_object(void *o) { g_pins++; if (o == &g_pin_obj_a) g_pin_obj_a++; if (o == &g_pin_obj_b) g_pin_obj_b++; }
static void kl_jni_unpin_object(void *o) { g_unpins++; if (o == &g_pin_obj_a) g_pin_obj_a--; if (o == &g_pin_obj_b) g_pin_obj_b--; }

#include "permq.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    const char *two[] = {"android.permission.RECORD_AUDIO", "android.permission.CAMERA"};
    klj_perm_req r;
    CHECK(!klj_perm_pop(&r), "empty queue pops nothing");
    CHECK(klj_perm_push((void *)1, NULL, 7, two, 2) == 1, "push");
    CHECK(g_perm_qn == 1, "queued, not delivered inside the request");
    CHECK(klj_perm_pop(&r) == 1, "pop");
    CHECK(r.code == 7 && r.n == 2 && r.target == (void *)1 && r.cb == NULL, "request fields kept");
    CHECK(r.res[0] == 0 && r.res[1] == -1, "0 GRANTED / -1 DENIED like checkSelfPermission");
    CHECK(strcmp(r.perm[1], "android.permission.CAMERA") == 0, "permission names copied");
    for (int i = 0; i < r.n; i++) free(r.perm[i]);
    CHECK(!klj_perm_pop(&r), "popped once only");

    // FIFO across wrap-around, bounded at KLJ_PERM_MAX_REQ.
    int pushed = 0;
    for (int i = 0; i < 20; i++) pushed += klj_perm_push(NULL, NULL, i, two, 1);
    CHECK(pushed == KLJ_PERM_MAX_REQ, "queue is bounded");
    for (int i = 0; i < 3; i++) { klj_perm_pop(&r); CHECK(r.code == i, "FIFO order"); free(r.perm[0]); }
    CHECK(klj_perm_push(NULL, NULL, 100, two, 1) == 1, "room again after pops");
    int want[] = {3, 4, 5, 6, 7, 100};
    for (int i = 0; i < 6; i++) { CHECK(klj_perm_pop(&r) == 1 && r.code == want[i], "order across the wrap"); free(r.perm[0]); }
    CHECK(!klj_perm_pop(&r), "drained");

    // Too many permissions are clamped, never overrun.
    const char *many[KLJ_PERM_MAX_N + 5];
    for (int i = 0; i < KLJ_PERM_MAX_N + 5; i++) many[i] = "android.permission.INTERNET";
    CHECK(klj_perm_push(NULL, NULL, 1, many, KLJ_PERM_MAX_N + 5) == 1, "oversized push");
    klj_perm_pop(&r);
    CHECK(r.n == KLJ_PERM_MAX_N, "clamped to KLJ_PERM_MAX_N");
    for (int i = 0; i < r.n; i++) free(r.perm[i]);
    // Empty request: still answered (n = 0), like Android.
    CHECK(klj_perm_push(NULL, NULL, 9, two, 0) == 1 && klj_perm_pop(&r) && r.n == 0 && r.code == 9, "empty request");

    // Pins: target and cb are pinned while queued and unpinned by klj_perm_release; a refused push leaves none.
    g_pins = g_unpins = 0; g_pin_obj_a = g_pin_obj_b = 0;
    CHECK(klj_perm_push(&g_pin_obj_a, &g_pin_obj_b, 1, two, 2) == 1, "push with target and cb");
    CHECK(g_pin_obj_a == 1 && g_pin_obj_b == 1 && g_pins == 2, "target and cb pinned while queued");
    CHECK(klj_perm_push(NULL, &g_pin_obj_b, 2, two, 1) == 1, "push with cb only");
    CHECK(g_pin_obj_b == 2 && g_pins == 3, "same cb pinned twice (counted pins)");
    CHECK(klj_perm_pop(&r) == 1, "pop first");
    CHECK(g_pins == 3 && g_unpins == 0, "popping alone does not unpin");
    klj_perm_release(&r);
    CHECK(g_pin_obj_a == 0 && g_pin_obj_b == 1 && g_unpins == 2, "release unpins target and cb");
    CHECK(klj_perm_pop(&r) == 1, "pop second");
    klj_perm_release(&r);
    CHECK(g_pin_obj_b == 0 && g_pins == g_unpins, "all pins paired");
    // Overflow path: a push that does not fit leaves its pins balanced (pinned, then unpinned).
    for (int i = 0; i < KLJ_PERM_MAX_REQ; i++) klj_perm_push(NULL, NULL, i, two, 1);
    CHECK(klj_perm_push(&g_pin_obj_a, &g_pin_obj_b, 99, two, 1) == 0, "overflow refused");
    CHECK(g_pin_obj_a == 0 && g_pin_obj_b == 0 && g_pins == g_unpins, "refused push leaves no pin behind");
    while (klj_perm_pop(&r)) klj_perm_release(&r);
    CHECK(g_pins == g_unpins, "pins balanced at the end");
    if (!fails) puts("t_perm_queue: ok");
    return fails ? 1 : 0;
}
