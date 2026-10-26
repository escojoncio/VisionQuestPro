// The background gate; see kl_bg.h.
#include "kl_bg.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_bg;       // background && !shutdown: the lock-free fast path of kl_bg_wait
static int      g_want_bg;     // last phase from Swift
static int      g_down;        // shutdown: permanent
static unsigned g_blocked;     // kl_bg_wait calls that blocked since the pause began

// g_mu held. One log line per transition of the effective state.
static void apply_locked(void) {
    int eff = g_want_bg && !g_down;
    if (eff == atomic_load(&g_bg)) return;
    atomic_store(&g_bg, eff);
    if (eff) {
        g_blocked = 0;
        fprintf(stderr, "[bg] gpu paused (app backgrounded; guest submits block)\n");
    } else {
        fprintf(stderr, "[bg] gpu resumed (%u submit(s) were blocked)\n", g_blocked);
        pthread_cond_broadcast(&g_cv);
    }
}

// VPKEngine's own gate (vpgpu/core/background.h) parks its Metal commits. VPVulkan is not linked: the app dlopens
// it RTLD_GLOBAL, so the C API is looked up per call and is a no-op when absent (host runs, host tests).
static void vpgpu_call(const char *name, int arg, int has_arg) {
    void *f = dlsym(RTLD_DEFAULT, name);
    if (!f) return;
    if (has_arg) ((void (*)(int))f)(arg);
    else ((void (*)(void))f)();
}

void kl_bg_set(int background) {
    pthread_mutex_lock(&g_mu);
    g_want_bg = background != 0;
    apply_locked();
    pthread_mutex_unlock(&g_mu);
    vpgpu_call("vpgpu_set_background", background != 0, 1);   // the caller (Swift main thread) is the gate's "main"
}

void kl_bg_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    g_down = 1;
    apply_locked();
    pthread_mutex_unlock(&g_mu);
    vpgpu_call("vpgpu_shutdown_background", 0, 0);
}

int kl_bg_active(void) { return atomic_load(&g_bg); }

void kl_bg_wait(void) {
    if (!atomic_load(&g_bg)) return;
    pthread_mutex_lock(&g_mu);
    if (atomic_load(&g_bg)) {
        g_blocked++;
        while (atomic_load(&g_bg)) pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}
