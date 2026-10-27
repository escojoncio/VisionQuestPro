// Background gate of gfx/kl_bg.c: while the app is backgrounded the guest's submit paths block
// (Metal refuses every command buffer then); foreground or shutdown releases them.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "kl_bg.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

// Stand-ins for VPKEngine's C API, which kl_bg.c finds with dlsym(RTLD_DEFAULT) (exported by the executable).
static int vp_last = -1, vp_sets, vp_down;
void vpgpu_set_background(int on) { vp_last = on; vp_sets++; }
void vpgpu_shutdown_background(void) { vp_down++; }

static _Atomic int in, out;
static void *submitter(void *a) {
    (void)a;
    atomic_fetch_add(&in, 1);
    kl_bg_wait();
    atomic_fetch_add(&out, 1);
    return NULL;
}
static int wait_out(int n) {
    for (int i = 0; i < 200 && atomic_load(&out) < n; i++) usleep(10000);
    return atomic_load(&out) >= n;
}

int main(void) {
    pthread_t t[3];
    // Foreground: never blocks.
    kl_bg_wait();
    CHECK(!kl_bg_active(), "starts in the foreground");

    // Background: submitters block, foreground releases them all.
    kl_bg_set(1);
    kl_bg_set(1);                      // repeated phase callback: no second transition
    CHECK(kl_bg_active(), "background flag set");
    CHECK(vp_last == 1 && vp_sets == 2, "VPKEngine gate told: background");
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, submitter, NULL);
    usleep(150000);
    CHECK(atomic_load(&in) == 2 && atomic_load(&out) == 0, "submitters blocked while backgrounded");
    kl_bg_set(0);
    CHECK(wait_out(2), "foreground releases the waiters");
    CHECK(vp_last == 0 && vp_sets == 3, "VPKEngine gate told: foreground");
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    CHECK(!kl_bg_active(), "foreground flag");
    kl_bg_wait();                      // returns at once again

    // Shutdown releases waiters and is permanent (a late background callback must not re-block).
    atomic_store(&in, 0); atomic_store(&out, 0);
    kl_bg_set(1);
    pthread_create(&t[2], NULL, submitter, NULL);
    usleep(150000);
    CHECK(atomic_load(&out) == 0, "blocked again after a second background");
    kl_bg_shutdown();
    CHECK(wait_out(1), "shutdown releases the waiter");
    // VPKEngine's gate is released (foreground), not shut down for good: it can be closed again.
    CHECK(vp_down == 0 && vp_last == 0, "VPKEngine gate released, not shut down");
    pthread_join(t[2], NULL);
    int sets = vp_sets;
    kl_bg_set(1);
    CHECK(!kl_bg_active(), "no background after shutdown");
    CHECK(vp_sets == sets && vp_last == 0, "late background callback not forwarded to VPKEngine");
    kl_bg_wait();                      // must not block
    // The run is over and the process is about to exit while backgrounded: whatever still commits parks.
    kl_bg_engine_park();
    CHECK(vp_last == 1, "VPKEngine gate closed again after shutdown");
    CHECK(!kl_bg_active(), "our own waiters stay released");

    if (fails) return 1;
    printf("t_bg ok\n");
    return 0;
}
