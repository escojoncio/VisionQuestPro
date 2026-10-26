// The background gate; see kl_bg.h.
#include "kl_bg.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

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

// ---- frame boundary for the capture ------------------------------------------------------------------------------
// Looked up once (dlsym, like vpgpu_call): a miss costs one atomic load per frame afterwards.
#define KL_FB_STALE_NS 500000000ull   // a route that stopped calling this long ago hands the boundary over
static _Atomic int       g_fb_state;  // 0 not looked up, 1 absent, 2 present
static void (*_Atomic g_fb_fn)(void);
static _Atomic int       g_fb_owner;  // KL_FRAME_* of the route that calls
static _Atomic uint64_t  g_fb_last;   // its last call, CLOCK_MONOTONIC ns

void kl_frame_boundary(int route) {
    int st = atomic_load(&g_fb_state);
    if (st == 0) {
        void (*f)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "vpvk_frame_boundary");
        atomic_store(&g_fb_fn, f);
        st = f ? 2 : 1;
        atomic_store(&g_fb_state, st);
    }
    if (st != 2) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    int owner = atomic_load(&g_fb_owner);
    if (owner != route) {
        if (owner && now - atomic_load(&g_fb_last) < KL_FB_STALE_NS) return;
        atomic_store(&g_fb_owner, route);
    }
    atomic_store(&g_fb_last, now);
    void (*f)(void) = atomic_load(&g_fb_fn);
    if (f) f();
}
