// Host test of the pipelined frame end (kl_vulkan.c, KL-TEST fence blocks): the fence ring, the serial,
// the in-flight allowance and the pending-serial hand-off, against a fake GPU. Linux/macOS, no Vulkan driver.
// Build: tools/ci/host-tests.sh (ASan + UBSan). Needs the Vulkan headers (-I<headers>).
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define VKI(...) fprintf(stderr, "  [vk] " __VA_ARGS__)
#define KLVK_EYE_STAGES 4

// ---- fake GPU: frames are "submitted" by the test, "completed" by the test (or by a wait) ----
static unsigned g_submitted, g_completed;      // frames the guest submitted / the GPU finished
static unsigned g_waits, g_polls;
static int g_lost;
typedef struct { unsigned target; int used; } fake_fence;
static fake_fence g_ff[16];

typedef struct {
    VkDevice dev; VkQueue queue; int nused; VkQueue only;   // nused: queues the guest used this frame, only: the first
    PFN_vkCreateFence CreateFence; PFN_vkGetFenceStatus GetFenceStatus;
    PFN_vkWaitForFences WaitForFences; PFN_vkResetFences ResetFences;
} klvk_device;

static VkResult VKAPI_CALL f_create(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void)d; (void)ci; (void)a;
    for (int i = 0; i < 16; i++) if (!g_ff[i].used) { g_ff[i].used = 1; g_ff[i].target = 0; *out = (VkFence)(uintptr_t)&g_ff[i]; return VK_SUCCESS; }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static VkResult VKAPI_CALL f_status(VkDevice d, VkFence f) {
    (void)d; g_polls++;
    if (g_lost) return VK_ERROR_DEVICE_LOST;
    return __atomic_load_n(&g_completed, __ATOMIC_ACQUIRE) >= ((fake_fence *)(uintptr_t)f)->target ? VK_SUCCESS : VK_NOT_READY;
}
static VkResult VKAPI_CALL f_wait(VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t t) {
    (void)d; (void)n; (void)all; (void)t; g_waits++;
    if (g_lost) return VK_ERROR_DEVICE_LOST;
    unsigned tg = ((fake_fence *)(uintptr_t)fs[0])->target;
    if (__atomic_load_n(&g_completed, __ATOMIC_ACQUIRE) < tg) __atomic_store_n(&g_completed, tg, __ATOMIC_RELEASE);   // the GPU catches up
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL f_reset(VkDevice d, uint32_t n, const VkFence *fs) {
    (void)d; for (uint32_t i = 0; i < n; i++) ((fake_fence *)(uintptr_t)fs[i])->target = 0xffffffffu;
    return VK_SUCCESS;
}

static klvk_device g_dev0;
static klvk_device *g_devs[8] = { &g_dev0 };
static struct { unsigned (*gpu_backlog_ms)(void); } g_vpvk;
static struct { int img; } g_eye[KLVK_EYE_STAGES][2];
static uint64_t g_frame_serial;
static int g_marked_lost;

static double klvk_now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int klvk_gpu_poll_lost(void) { return g_lost; }
static void klvk_slow_wait_note(const char *w, double t0) { (void)w; (void)t0; }
static int kl_env_on(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) != 0 : d; }
static int kl_env_int(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
// The empty fence submit: the fence is ordered after everything submitted so far.
static VkQueue g_marker_q, g_fenced_q;
static VkResult klvk_qsubmit(klvk_device *d, VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)d; assert(n == 0 && !si);
    g_marker_q = q;
    ((fake_fence *)(uintptr_t)f)->target = g_submitted;
    return VK_SUCCESS;
}
static VkQueue klvk_wq(klvk_device *d) { return d->queue; }
static int klvk_used_queues(klvk_device *d, VkQueue *only) { *only = d->only; return d->nused; }
static void klvk_queue_fenced(klvk_device *d, VkQueue q) { g_fenced_q = q; d->nused = 0; d->only = NULL; }
static void klvk_gpu_mark_lost(const char *why);

#include "fence_blocks.inc"

static void klvk_gpu_mark_lost(const char *why) { (void)why; g_marked_lost++; }

static void reset_world(int stages) {
    memset(g_ff, 0, sizeof g_ff);
    g_submitted = g_completed = g_waits = g_polls = 0; g_lost = 0; g_marked_lost = 0;
    g_frame_serial = 0; g_fe_dev = NULL; g_fe_n = 0; g_fe_head = 0; g_fe_active = 0;
    g_fe_lat_sum = g_fe_lat_max = 0; g_fe_lat_n = 0; g_native_n = 0; g_fe_native_seen = 0; g_fe_cfg_n = -1;
    memset(g_eye, 0, sizeof g_eye);
    for (int i = 0; i < stages; i++) g_eye[i][0].img = 1;
    g_dev0.dev = (VkDevice)(uintptr_t)0x1000; g_dev0.queue = (VkQueue)(uintptr_t)0x2000; g_dev0.nused = 0; g_dev0.only = NULL;
    g_marker_q = g_fenced_q = NULL;
    g_dev0.CreateFence = f_create; g_dev0.GetFenceStatus = f_status; g_dev0.WaitForFences = f_wait; g_dev0.ResetFences = f_reset;
}

// One guest frame: it submits work (the GPU takes `gpu_per_frame` frames' worth of progress afterwards).
static int g_stop, g_bad;
static void *poller(void *a) {
    (void)a;
    uint64_t last = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        uint64_t s = kl_vulkan_frame_serial();
        unsigned c = __atomic_load_n(&g_completed, __ATOMIC_ACQUIRE);   // read after: it only grows
        if (s < last || s > c) g_bad++;
        last = s;
    }
    return NULL;
}
static int one_frame(uint64_t issued, double *waited, unsigned *infl, unsigned *fmax) {
    g_submitted++;
    return klvk_frame_fence(&g_dev0, issued, waited, infl, fmax);
}

int main(void) {
    setenv("KL_VK_FRAME_ASYNC", "1", 1);   // the pipelined path is opt-in; the test exercises it
    // 1. S = 3 stages, N = 2 (defaults): at most ONE frame submitted-and-incomplete when the frame end returns,
    //    and the serial never runs ahead of the GPU.
    reset_world(3);
    assert(klvk_frame_fmax() == 1);
    for (uint64_t k = 1; k <= 200; k++) {
        double w = 0; unsigned infl = 0, fmax = 0;
        // The GPU finishes the frame before last by the time the guest is done recording this one, but never this one.
        if (k >= 2 && g_completed < g_submitted - 1 && (k % 3)) g_completed = g_submitted - 1;
        assert(one_frame(k, &w, &infl, &fmax) == 1);
        assert(fmax == 1 && infl <= 1);
        assert(kl_vulkan_frame_serial() <= g_completed);            // never published before the GPU is done
        assert(g_submitted - g_completed <= 1);                      // the allowance held
        assert(g_frame_serial + 2 >= k);                             // and the serial keeps up
    }
    // The newest frame is still running (one in flight): the serial reaches it only when the GPU does, seen by a poller.
    assert(g_frame_serial <= 200);
    g_completed = g_submitted;
    assert(kl_vulkan_frame_serial() == 200);                          // the compositor's poll finishes it
    assert(g_fe_lat_n > 0);

    // 2. The poll is lock-free for the compositor when idle, and skipped when a guest thread holds the lock.
    pthread_mutex_lock(&g_fe_mu);
    uint64_t s0 = kl_vulkan_frame_serial();                           // trylock fails: just the current value
    pthread_mutex_unlock(&g_fe_mu);
    assert(s0 == 200);

    // 3. The pending serial is consumed once.
    t_pub_serial = 77;
    assert(kl_vulkan_frame_pending_serial() == 77);
    assert(kl_vulkan_frame_pending_serial() == 0);

    // 4. Allowance caps: 2 stages -> 0 (the frame waits for its own fence), 4 stages with N = 3 -> 2.
    reset_world(2);
    assert(klvk_frame_fmax() == 0);
    { double w = 0; unsigned infl = 9, fmax = 9;
      assert(one_frame(1, &w, &infl, &fmax) == 1);
      assert(infl == 0 && g_completed == g_submitted && g_frame_serial == 1); }
    reset_world(0);
    assert(klvk_frame_fmax() == 0);

    // 5. Native command buffers since the last frame: this one is waited for in full by the caller (returns 0 once).
    reset_world(3);
    { double w = 0; unsigned infl = 0, fmax = 0;
      assert(one_frame(1, &w, &infl, &fmax) == 1);
      g_native_n++;
      assert(one_frame(2, &w, &infl, &fmax) == 0);                    // caller does the queue wait
      assert(one_frame(3, &w, &infl, &fmax) == 1); }

    // 5b. Guest work on more than one VkQueue this frame: an empty submit orders after one queue only, so the caller waits
    //     (returns 0). It is not sticky: the next frame on a single queue is fenced again.
    reset_world(3);
    g_dev0.nused = 2; g_dev0.only = (VkQueue)(uintptr_t)0x3000;
    { double w = 0; unsigned infl = 0, fmax = 0;
      assert(one_frame(1, &w, &infl, &fmax) == 0);
      assert(g_fe_n == 0 && g_marker_q == NULL);
      g_dev0.nused = 0; g_dev0.only = NULL;                               // the caller's wait took the flags
      assert(one_frame(2, &w, &infl, &fmax) == 1);
      assert(g_marker_q == g_dev0.queue); }
    // 5c. Guest work on one queue that is not klepton's own: the marker goes on that queue (the fence is device-wide).
    reset_world(3);
    g_dev0.nused = 1; g_dev0.only = (VkQueue)(uintptr_t)0x3000;
    { double w = 0; unsigned infl = 0, fmax = 0;
      assert(one_frame(1, &w, &infl, &fmax) == 1);
      assert(g_marker_q == (VkQueue)(uintptr_t)0x3000 && g_fenced_q == g_marker_q); }

    // 6. The ring never overruns and recycles fences (many frames, GPU always far behind).
    reset_world(4);
    setenv("KL_VK_FRAME_INFLIGHT", "3", 1);
    for (uint64_t k = 1; k <= 100; k++) {
        double w = 0; unsigned infl = 0, fmax = 0;
        assert(one_frame(k, &w, &infl, &fmax) == 1);
        assert(fmax <= 2 && g_fe_n <= KLVK_FE_RING);
    }
    unsetenv("KL_VK_FRAME_INFLIGHT");

    // 7. Device lost: reported, nothing published.
    reset_world(3);
    { double w = 0; unsigned infl = 0, fmax = 0;
      assert(one_frame(1, &w, &infl, &fmax) == 1);
      g_lost = 1;
      one_frame(2, &w, &infl, &fmax);
      assert(g_marked_lost > 0);
      assert(g_frame_serial <= 1); }

    // 9. A compositor thread polling the serial while the guest ends frames and the GPU finishes them.
    reset_world(3);
    {
        pthread_t th;
        pthread_create(&th, NULL, poller, NULL);
        for (uint64_t k = 1; k <= 3000; k++) {
            double w = 0; unsigned infl = 0, fmax = 0;
            if (k % 2) __atomic_store_n(&g_completed, g_submitted, __ATOMIC_RELEASE);   // the GPU is fast every other frame
            assert(one_frame(k, &w, &infl, &fmax) == 1);
        }
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE); pthread_join(th, NULL);
        assert(g_bad == 0);
    }

    // 8. Mode: on here because main() set KL_VK_FRAME_ASYNC=1 (the default is off).
    assert(klvk_frame_pipelined_on() == 1);

    printf("t_frame_fence: ok\n");
    return 0;
}
