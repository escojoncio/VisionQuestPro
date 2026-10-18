// Per-thread "what am I blocked on" registry, read by the [snap] dumps
// (kl_sample.c). The blocking shims (mutex, semaphore, condvar, futex,
// clock_nanosleep) call kl_wait_enter() before they park and kl_wait_leave()
// when they return, so a dump can say "thread X has waited on Y for T seconds"
// for threads whose stacks all look alike. Cost: one TLS read, a clock read and
// a few relaxed atomic stores per blocking call; nothing is ever locked.
#ifndef KL_WAIT_H
#define KL_WAIT_H

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

enum {
    KLW_NONE = 0,
    KLW_MUTEX,        // addr = guest mutex
    KLW_SEM,          // addr = guest sem
    KLW_SEM_TIMED,    // addr = guest sem
    KLW_COND,         // addr = guest cond, aux = guest mutex
    KLW_COND_TIMED,   // addr = guest cond, aux = guest mutex
    KLW_FUTEX,        // addr = futex word, aux = value it was waiting on
    KLW_NANOSLEEP,    // aux = requested nanoseconds
};

typedef struct {
    _Atomic uint32_t  act;        // mach thread port of the owner (0 = slot free)
    _Atomic uint32_t  kind;       // KLW_*; KLW_NONE while running
    _Atomic uintptr_t addr, aux;
    _Atomic uintptr_t ra;         // guest call site
    _Atomic uint64_t  since_ns;   // CLOCK_MONOTONIC_RAW when the wait began
    // GL/EGL side (kl_gles.c / kl_egl.c): the EGL context this thread made
    // current, and the GL/EGL call it is inside right now (NULL = none).
    _Atomic uintptr_t egl_ctx;
    _Atomic(const char *) gl_call;
    _Atomic uint64_t  gl_since_ns;
} kl_wait_slot;

#define KL_WAIT_SLOTS 512
extern kl_wait_slot  g_kl_wait[KL_WAIT_SLOTS];
extern _Atomic unsigned g_kl_wait_n;
extern __thread kl_wait_slot *kl_wait_tls;
kl_wait_slot *kl_wait_slot_get(void);   // allocates this thread's slot (NULL when full)

static inline void kl_wait_enter(unsigned kind, const void *addr, uintptr_t aux, const void *ra) {
    kl_wait_slot *s = kl_wait_tls ? kl_wait_tls : kl_wait_slot_get();
    if (!s) return;
    atomic_store_explicit(&s->addr, (uintptr_t)addr, memory_order_relaxed);
    atomic_store_explicit(&s->aux, aux, memory_order_relaxed);
    atomic_store_explicit(&s->ra, (uintptr_t)ra, memory_order_relaxed);
    atomic_store_explicit(&s->since_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW),
                          memory_order_relaxed);
    atomic_store_explicit(&s->kind, kind, memory_order_release);
}
static inline void kl_wait_leave(void) {
    if (kl_wait_tls) atomic_store_explicit(&kl_wait_tls->kind, KLW_NONE, memory_order_release);
}

static inline void kl_wait_set_ctx(const void *ctx) {
    kl_wait_slot *s = kl_wait_tls ? kl_wait_tls : kl_wait_slot_get();
    if (s) atomic_store_explicit(&s->egl_ctx, (uintptr_t)ctx, memory_order_relaxed);
}
static inline void kl_gl_enter(const char *name) {
    kl_wait_slot *s = kl_wait_tls ? kl_wait_tls : kl_wait_slot_get();
    if (!s) return;
    atomic_store_explicit(&s->gl_since_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW),
                          memory_order_relaxed);
    atomic_store_explicit(&s->gl_call, name, memory_order_release);
}
static inline void kl_gl_leave(void) {
    if (kl_wait_tls) atomic_store_explicit(&kl_wait_tls->gl_call, NULL, memory_order_release);
}

// Game-thread heartbeat (kl_sample.c): called by the per-frame OVRPlugin /
// OpenXR entry points. The first caller becomes "the game thread"; the watcher
// takes a full-thread snapshot when it stops calling while the process lives.
// src: 0 = OVRPlugin (wins over a thread registered from OpenXR), 1 = OpenXR.
void kl_game_beat(int src);

#endif
