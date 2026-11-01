// Android NDK shim. See kl_ndk.h for the measured scope.
//
// Three independent pieces:
//   ALooper       — real implementation over poll() and a self-pipe. Unity waits
//                   on this, so a stub that returns immediately would spin.
//   ANativeWindow — a synthetic window carrying geometry. No pixels until EGL binds one.
//   ASensor       — accelerometer/gyroscope/gravity/rotation vector derived from the head pose.
//   AInputQueue   — a real queue (keys/motion pushed by the host; Touch controllers as a gamepad).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kl_ndk.h"
#include "kl_android.h"
#include "kl_obbmap.h"
#include "klepton.h"      // kl_guest_path, for assets named by absolute path
// For kl_jni_locale_parts — AConfiguration and java.util.Locale are two doors
// onto one fact and must not answer differently.
#include "kl_jni.h"
#include "kl_ovrp.h"
#include "kl_openxr.h"   // the XR surface-swapchain seam, for ANativeWindow_fromSurface
#include "kl_mediandk.h"  // kl_mediandk_lookup: the AHardwareBuffer core the JNI-facing half builds on

// ================================================================== ALooper
enum {
    ALOOPER_POLL_WAKE     = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT  = -3,
    ALOOPER_POLL_ERROR    = -4,
};
enum { ALOOPER_EVENT_INPUT = 1 << 0 };

#define KL_LOOPER_MAX_FDS 16

typedef int (*kl_looper_cb)(int fd, int events, void *data);

typedef struct kl_looper {
    int             refs;
    int             wake_r, wake_w;
    struct {
        int          fd, ident, events;
        kl_looper_cb cb;
        void        *data;
    } fds[KL_LOOPER_MAX_FDS];
    int             nfds;
    pthread_mutex_t lock;
} kl_looper;

static pthread_key_t  g_looper_key;
static pthread_once_t g_looper_once = PTHREAD_ONCE_INIT;
static void kl_looper_key_init(void) { pthread_key_create(&g_looper_key, NULL); }

static kl_looper *kl_ALooper_forThread(void) {
    pthread_once(&g_looper_once, kl_looper_key_init);
    return pthread_getspecific(g_looper_key);
}

static kl_looper *kl_ALooper_prepare(int opts) {
    kl_looper *l = kl_ALooper_forThread();
    if (l) return l;

    l = calloc(1, sizeof *l);
    if (!l) return NULL;
    int p[2];
    if (pipe(p) != 0) { free(l); return NULL; }
    // Non-blocking so draining the wake pipe never stalls the loop.
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);
    l->wake_r = p[0];
    l->wake_w = p[1];
    l->refs   = 1;               // the reference held by the thread itself
    (void)opts;
    pthread_mutex_init(&l->lock, NULL);
    pthread_setspecific(g_looper_key, l);
    return l;
}

// Android's main thread ALWAYS has a looper: ActivityThread calls
// Looper.prepareMainLooper() long before any activity's onCreate, so
// ALooper_forThread() on that thread never returns NULL and native code is
// entitled to assume it. We author that side, so a host driving a
// NativeActivity has to have done the same thing first.
//
// libvrlink_scene is what forced this and it fails a long way from the cause:
// UIThreadCallbackHandler's constructor pipe()s, takes ALooper_forThread(),
// and calls ALooper_addFd() on it — and on -1 it throws std::bad_alloc, which
// is the *wrong* exception for "there was no looper" and sends the search
// straight to memory. The abort is 8 frames of libc++ terminate machinery with
// no mention of a looper anywhere.
void kl_ndk_prepare_looper(void) { (void)kl_ALooper_prepare(0); }

int kl_ndk_thread_has_looper(void) { return kl_ALooper_forThread() != NULL; }

// The calling thread's looper as an opaque handle, and a wake for one — what an
// AChoreographer needs to deliver a frame callback to the thread that asked.
void *kl_ndk_looper_current(void) { return kl_ALooper_forThread(); }
static void kl_ALooper_wake(kl_looper *l);
void kl_ndk_looper_wake(void *l) { kl_ALooper_wake(l); }

static int kl_looper_poll(int timeoutMillis, int *outFd, int *outEvents, void **outData);

// ...and running it, which is the other half and is just as much ours to do.
// Android's main thread does not merely HAVE a looper, it is sitting in
// Looper.loop() whenever it is not inside a callback — that loop is what makes
// a posted message run. A host that prepares a looper and then sleeps has built
// the queue and left nothing draining it, which is the Choreographer lesson from
// The same rule in a different subsystem: the guest is not stuck, it is waiting for a pump
// we declined to run.
int kl_ndk_pump_looper(int timeout_ms) {
    return kl_looper_poll(timeout_ms, NULL, NULL, NULL);
}

static void kl_ALooper_acquire(kl_looper *l) {
    if (l) __atomic_fetch_add(&l->refs, 1, __ATOMIC_RELAXED);
}

static void kl_ALooper_release(kl_looper *l) {
    // Deliberately never frees. A looper is thread-owned and long-lived, and a
    // use-after-free here would surface as an unrelated poll() failure much
    // later. One leaked looper per thread is the cheaper trade.
    if (l) __atomic_fetch_sub(&l->refs, 1, __ATOMIC_RELAXED);
}

// 'w' = a real ALooper_wake (pollOnce returns ALOOPER_POLL_WAKE); 'i' = the internal wake of
// addFd/removeFd, which only makes the poller rebuild its fd set and keep waiting (as on Android,
// where rebuildEpoll is invisible to the caller).
static void kl_ALooper_wake_byte(kl_looper *l, char c) {
    if (!l) return;
    ssize_t n = write(l->wake_w, &c, 1);
    (void)n;   // EAGAIN just means a wake is already pending, which is the point
}
static void kl_ALooper_wake(kl_looper *l) { kl_ALooper_wake_byte(l, 'w'); }

static int kl_ALooper_addFd(kl_looper *l, int fd, int ident, int events,
                            kl_looper_cb cb, void *data) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {          // re-registering replaces
            l->fds[i].ident = ident; l->fds[i].events = events;
            l->fds[i].cb = cb; l->fds[i].data = data;
            pthread_mutex_unlock(&l->lock);
            kl_ALooper_wake_byte(l, 'i');
            return 1;
        }
    if (l->nfds == KL_LOOPER_MAX_FDS) { pthread_mutex_unlock(&l->lock); return -1; }
    l->fds[l->nfds++] = (typeof(l->fds[0])){fd, ident, events, cb, data};
    pthread_mutex_unlock(&l->lock);
    // Android's Looper::addFd/removeFd wake the looper (rebuildEpoll). Another thread may
    // register an fd on a looper whose owner is already blocked in poll() with the OLD fd set
    // (kl_looper_poll snapshots it before poll): without this wake the new fd is not watched
    // until something else wakes the loop. Unity's VideoDecoderLooper is exactly that: the
    // thread prepares + blocks in ALooper_pollOnce(-1), and the creating thread then addFd()s
    // the request pipe, so every surface-mode video request stayed unanswered.
    kl_ALooper_wake_byte(l, 'i');
    return 1;
}

static int kl_ALooper_removeFd(kl_looper *l, int fd) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {
            l->fds[i] = l->fds[--l->nfds];
            pthread_mutex_unlock(&l->lock);
            kl_ALooper_wake_byte(l, 'i');   // the poller may hold the removed fd in its snapshot
            return 1;
        }
    pthread_mutex_unlock(&l->lock);
    return 0;
}


static int64_t kl_now_ms(void);

static int kl_looper_poll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    kl_looper *l = kl_ALooper_forThread();
    if (!l) return ALOOPER_POLL_ERROR;

    // Frame callbacks the vsync clock has made due for this thread run first, as
    // the Choreographer's own fd callback would: they are "a callback ran".
    if (kl_android_choreo_dispatch()) return ALOOPER_POLL_CALLBACK;

    // Deadline for the internal-wake retries below: an addFd/removeFd wake must not shorten the wait.
    const int64_t deadline = timeoutMillis > 0 ? kl_now_ms() + timeoutMillis : 0;
    int remaining = timeoutMillis;

    for (;;) {
    struct pollfd pfd[KL_LOOPER_MAX_FDS + 1];
    int           ident[KL_LOOPER_MAX_FDS + 1];
    kl_looper_cb  cb[KL_LOOPER_MAX_FDS + 1];
    void         *data[KL_LOOPER_MAX_FDS + 1];

    pthread_mutex_lock(&l->lock);
    pfd[0] = (struct pollfd){l->wake_r, POLLIN, 0};
    int n = 1;
    for (int i = 0; i < l->nfds; i++, n++) {
        pfd[n]   = (struct pollfd){l->fds[i].fd,
                                   (short)(l->fds[i].events & ALOOPER_EVENT_INPUT ? POLLIN : 0), 0};
        ident[n] = l->fds[i].ident;
        cb[n]    = l->fds[i].cb;
        data[n]  = l->fds[i].data;
    }
    pthread_mutex_unlock(&l->lock);

    int r = poll(pfd, (nfds_t)n, remaining);
    if (r < 0)  return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;

    if (pfd[0].revents & POLLIN) {         // drain the wake pipe
        char buf[64];
        int real = 0;
        ssize_t got;
        while ((got = read(l->wake_r, buf, sizeof buf)) > 0)
            for (ssize_t k = 0; k < got; k++) if (buf[k] == 'w') real = 1;
        if (kl_android_choreo_dispatch()) return ALOOPER_POLL_CALLBACK;   // the wake was a vsync
        if (real) return ALOOPER_POLL_WAKE;
        // Only the internal wake of addFd/removeFd: rebuild the fd set and wait out the rest of the
        // timeout. A zero timeout re-polls once more without blocking (the new set may already be
        // ready; the wake bytes are drained, so this ends with TIMEOUT when nothing is).
        if (timeoutMillis > 0) {
            int64_t left = deadline - kl_now_ms();
            if (left <= 0) return ALOOPER_POLL_TIMEOUT;
            remaining = (int)left;
        }
        continue;
    }
    for (int i = 1; i < n; i++) {
        if (!pfd[i].revents) continue;
        if (cb[i]) { cb[i](pfd[i].fd, pfd[i].revents, data[i]); return ALOOPER_POLL_CALLBACK; }
        if (outFd)     *outFd     = pfd[i].fd;
        if (outEvents) *outEvents = pfd[i].revents;
        if (outData)   *outData   = data[i];
        return ident[i];
    }
    return ALOOPER_POLL_WAKE;
    }
}

static int64_t kl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// pollAll differs from pollOnce only in that it does not return
// ALOOPER_POLL_CALLBACK to the caller — it keeps going until something the
// caller cares about happens.
//
// The timeout is a deadline, not a per-iteration budget: re-polling with the
// full timeout after each callback would let this overshoot without bound.
// Nothing triggers a callback today (the sensor queue never produces events),
// but frame pacing is this project's hardest open risk and a looper
// that sleeps longer than asked is exactly the kind of thing that shows up
// later as judder and is miserable to trace back.
static int kl_ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    int64_t deadline = timeoutMillis > 0 ? kl_now_ms() + timeoutMillis : 0;
    int     remaining = timeoutMillis;
    for (;;) {
        int r = kl_looper_poll(remaining, outFd, outEvents, outData);
        if (r != ALOOPER_POLL_CALLBACK) return r;
        if (timeoutMillis == 0) return ALOOPER_POLL_CALLBACK;
        if (timeoutMillis > 0) {
            int64_t left = deadline - kl_now_ms();
            if (left <= 0) return ALOOPER_POLL_TIMEOUT;
            remaining = (int)left;
        }
        // timeoutMillis < 0 means block indefinitely; remaining stays negative.
    }
}
static int kl_ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    return kl_looper_poll(timeoutMillis, outFd, outEvents, outData);
}

// ============================================================ ANativeWindow
typedef struct kl_native_window {
    int     refs;
    int32_t width, height, format;
    // Non-NULL for a window somebody else made and owns — today that is an
    // AImageReader's output surface (kl_mediandk.c). Opaque here deliberately;
    // see kl_ndk.h.
    void   *owner;
    // Recorded for the getters; the surface the guest renders into is ANGLE's
    // and takes its transform/dataspace from the EGL config, as for the format.
    int32_t transform, dataspace;
} kl_native_window;

// Quest 2 per-eye geometry, which is what this title was built against. Only a
// placeholder: in VR the eye buffers come from the XR runtime, and this
// surface exists mainly to give Unity a non-zero Screen size at startup. The
// host overrides it via kl_ndk_set_window once a real drawable exists.
static kl_native_window g_window = {1, 1832, 1920, 1 /* RGBA_8888 */, NULL, 0, 0};

void kl_ndk_set_window(int32_t w, int32_t h, int32_t format) {
    g_window.width = w; g_window.height = h; g_window.format = format;
}
void *kl_ndk_window(void) { return &g_window; }

void kl_ndk_window_size(const void *win, int32_t *w, int32_t *h) {
    const kl_native_window *nw = win ? (const kl_native_window *)win : &g_window;
    if (w) *w = nw->width;
    if (h) *h = nw->height;
}

// Windows that are not the activity's. See kl_ndk.h for why these exist.
void *kl_ndk_window_new(int32_t w, int32_t h, int32_t format, void *owner) {
    kl_native_window *nw = calloc(1, sizeof *nw);
    if (!nw) return NULL;
    nw->refs = 1;
    nw->width = w; nw->height = h; nw->format = format;
    nw->owner = owner;
    return nw;
}

void kl_ndk_window_free(void *win) {
    // g_window is static storage and is the activity's for the life of the
    // process; freeing it would be a wild free, and a guest that releases a
    // surface it did not create is not doing anything unusual.
    if (win && win != (void *)&g_window) free(win);
}

void *kl_ndk_window_owner(const void *win) {
    return win ? ((const kl_native_window *)win)->owner : NULL;
}

void kl_ndk_window_set_size(void *win, int32_t w, int32_t h) {
    kl_native_window *nw = win ? (kl_native_window *)win : &g_window;
    nw->width = w; nw->height = h;
}

static void    kl_ANativeWindow_acquire(kl_native_window *w) { if (w) __atomic_fetch_add(&w->refs, 1, __ATOMIC_RELAXED); }
static void    kl_ANativeWindow_release(kl_native_window *w) { if (w) __atomic_fetch_sub(&w->refs, 1, __ATOMIC_RELAXED); }
static int32_t kl_ANativeWindow_getWidth(kl_native_window *w)  { return w ? w->width  : 0; }
static int32_t kl_ANativeWindow_getHeight(kl_native_window *w) { return w ? w->height : 0; }
static int32_t kl_ANativeWindow_getFormat(kl_native_window *w) { return w ? w->format : 0; }

void *kl_jni_surface_window(void *surface);   // kl_jni_hostvideo.c; NULL unless Surface(SurfaceTexture)

static void *kl_ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env;
    // XR_KHR_android_surface_swapchain: a Surface handed back by
    // xrCreateSwapchainAndroidSurfaceKHR is NOT the activity's window — it is the
    // producer end of a specific XR swapchain (a guest's game screen). Resolving
    // it to g_window would send the guest's GL rendering to the main window
    // instead of the swapchain, which is exactly why nothing reached the display.
    // Give it its own ANativeWindow, sized to the swapchain and carrying the
    // Surface as its owner so kl_egl can find the swapchain from the window.
    if (surface) {
        void *sc = kl_xr_swapchain_for_surface(surface);
        if (sc) {
            int32_t w = 0, h = 0;
            kl_xr_android_surface_size(sc, &w, &h);
            void *win = kl_ndk_window_new(w, h, 1 /*RGBA_8888*/, surface);
            fprintf(stderr, "  [ndk] ANativeWindow_fromSurface(%p) -> XR game "
                            "surface window %p (%dx%d)\n", surface, win, w, h);
            return win;
        }
    }
    // Surface(SurfaceTexture): the window is the reader behind that SurfaceTexture
    // (kl_jni_hostvideo.c), so AMediaCodec_configure decodes into it for real.
    if (surface) {
        void *w = kl_jni_surface_window(surface);
        if (w) { kl_ANativeWindow_acquire(w); return w; }
    }
    kl_ANativeWindow_acquire(&g_window);
    return &g_window;
}

// Android treats 0 for any parameter as "keep the default". Honour that:
// Unity calls this with 0,0 to reset geometry after a format-only change, and
// clamping to zero there would make Screen.width report 0.
static int32_t kl_ANativeWindow_setBuffersGeometry(kl_native_window *w,
                                                   int32_t width, int32_t height, int32_t format) {
    if (!w) return -EINVAL;
    if (width > 0)  w->width  = width;
    if (height > 0) w->height = height;
    if (format > 0) w->format = format;
    return 0;
}

static int32_t kl_ANativeWindow_setBuffersTransform(kl_native_window *w, int32_t t) {
    if (!w) return -EINVAL;
    if (t < 0 || t > 7) return -EINVAL;           // ANATIVEWINDOW_TRANSFORM_* are the low 3 bits
    w->transform = t;
    return 0;
}
static int32_t kl_ANativeWindow_setBuffersDataSpace(kl_native_window *w, int32_t ds) {
    if (!w) return -EINVAL;
    w->dataspace = ds;
    return 0;
}
static int32_t kl_ANativeWindow_getBuffersDataSpace(kl_native_window *w) {
    return w ? w->dataspace : -EINVAL;
}
// setFrameRate is a hint to the display; compatibility is DEFAULT(0) or
// FIXED_SOURCE(1) and a change strategy is ONLY_IF_SEAMLESS(0) or ALWAYS(1).
// The headset's refresh rate is the compositor's, so the hint is validated and
// recorded as accepted, which is the "no preference honoured" Android allows.
static int32_t kl_ANativeWindow_setFrameRate(kl_native_window *w, float fps, int8_t compat) {
    if (!w || !(fps >= 0.0f) || fps > 1e6f || (compat != 0 && compat != 1)) return -EINVAL;
    return 0;
}
static int32_t kl_ANativeWindow_setFrameRateWithChangeStrategy(kl_native_window *w, float fps,
                                                               int8_t compat, int8_t strategy) {
    if (strategy != 0 && strategy != 1) return -EINVAL;
    return kl_ANativeWindow_setFrameRate(w, fps, compat);
}
static void kl_ANativeWindow_clearFrameRate(kl_native_window *w) { (void)w; }
static void kl_ANativeWindow_tryAllocateBuffers(kl_native_window *w) { (void)w; }

// The software path: SDL3's renderer is GLES2, but the lock/unlockAndPost pair
// is what a canvas renderer would paint through, and libmain imports both.
// The bits buffer is real memory so the guest can draw without faulting;
// nothing displays it — display goes through EGL.
typedef struct { int32_t width, height, stride, format; void *bits; uint32_t rsvd[6]; }
    kl_window_buffer;
static void *g_lock_bits;
static size_t g_lock_bytes;

static int32_t kl_ANativeWindow_lock(kl_native_window *w, kl_window_buffer *buf, void *dirty) {
    (void)dirty;
    kl_native_window *nw = w ? w : &g_window;
    size_t need = (size_t)nw->width * (size_t)nw->height * 4;
    if (need > g_lock_bytes) {
        free(g_lock_bits);
        g_lock_bits = calloc(1, need);
        if (!g_lock_bits) return -ENOMEM;
        g_lock_bytes = need;
    }
    buf->width  = nw->width;
    buf->height = nw->height;
    buf->stride = nw->width;
    buf->format = nw->format;
    buf->bits   = g_lock_bits;
    return 0;
}
static int32_t kl_ANativeWindow_unlockAndPost(kl_native_window *w) { (void)w; return 0; }

// ==================================================================== AAsset
// Steam Link reads its assets through the NDK C API where Unity reads them
// over JNI (see kl_ndk.h). An AAsset is a plain file under the assets root —
// the unpacked APK tree, which the JNI side (kl_jni.c) also serves from.
typedef struct { FILE *f; int64_t len; void *buf; char path[1200]; } kl_asset;

static char g_asset_root[1024] = "assets";
void kl_ndk_set_assets_dir(const char *dir) {
    if (dir) snprintf(g_asset_root, sizeof g_asset_root, "%s", dir);
}

static char g_asset_manager;         // one manager; it is a directory, not state

static void *kl_AAssetManager_fromJava(void *env, void *jmgr) {
    (void)env; (void)jmgr;
    return &g_asset_manager;
}

// ...and the same one without a JNI round trip, for ANativeActivity. Android
// fills that struct's assetManager field itself, and it must be the pointer
// AAssetManager_fromJava would return or the guest would hold two managers for
// one directory.
void *kl_ndk_asset_manager(void) { return &g_asset_manager; }

// An asset name is relative to the APK's assets/ on Android. Middleware that
// ports one file layer to every platform (Wwise's Android low-level IO, AC
// Nexus) also hands this door an ABSOLUTE path — "/sdcard/Android/obb/<pkg>/
// Init.bnk" — and Android's AssetManager2 resolves such a name against the
// filesystem rather than the APK. Gluing it under the assets root made
// "<assets>//sdcard/..." and every bank was a MISS. An absolute name goes
// through kl_guest_path (the same mapping fopen/open/stat use) and is served
// as a plain file; the handle is the same kl_asset, so read/seek/length/
// getBuffer/openFileDescriptor behave identically for both kinds.
static const char *kl_asset_resolve(const char *fname, char *path, size_t cap) {
    if (!fname) return NULL;
    if (fname[0] == '/') {
        const char *q = kl_guest_path(fname, path, cap);
        if (q != path) snprintf(path, cap, "%s", q);
        return path;
    }
    snprintf(path, cap, "%s/%s", g_asset_root, fname);
    return path;
}

static void *kl_AAssetManager_open(void *mgr, const char *fname, int mode) {
    (void)mgr; (void)mode;
    char path[1200];
    if (!kl_asset_resolve(fname, path, sizeof path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) f = kl_obbmap_fopen(path, "rb");   // audio banks live only in the OBB
    if (!f && fname[0] == '/') {
        // A leading '/' that is NOT a real path: some engines prefix asset names
        // with it. Fall back to the APK's assets as before.
        char ap[1200];
        snprintf(ap, sizeof ap, "%s%s", g_asset_root, fname);
        f = fopen(ap, "rb");
        if (!f) f = kl_obbmap_fopen(ap, "rb");
        if (f) snprintf(path, sizeof path, "%s", ap);
    }
    if (!f) {
        // Always on, capped: an asset the guest asks for and the APK lacks is how
        // "file not found" from a middleware (Wwise, FMOD) looks from here.
        static int n;
        if (n < 32) { n++;
            if (fname && fname[0] == '/')
                fprintf(stderr, "[asset] AAssetManager_open(\"%s\") MISS -> %s (%d/32)\n", fname, path, n);
            else
                fprintf(stderr, "[asset] AAssetManager_open(\"%s\") MISS (%d/32)\n", fname ? fname : "(null)", n);
        }
        return NULL;
    }
    kl_asset *a = calloc(1, sizeof *a);
    if (!a) { fclose(f); return NULL; }
    fseeko(f, 0, SEEK_END);
    a->len = ftello(f);
    fseeko(f, 0, SEEK_SET);
    a->f = f;
    snprintf(a->path, sizeof a->path, "%s", path);
    return a;
}

// The whole asset as one pointer. Android can hand back a pointer into the
// mapped APK because an uncompressed asset is contiguous there; an asset here is
// a plain file, so this reads it once and caches the buffer on the handle.
//
// The lifetime is the ASSET's, in both — the buffer dies with AAsset_close and
// the caller is not allowed to free it — so matching that exactly is what keeps
// a leak the guest's rather than ours. UE4 reads UE4CommandLine.txt this way.
// KL-TEST-BEGIN aasset
// Two threads may ask for the buffer of the same asset: the lazy creation is under a lock. A short read
// (file truncated, I/O error) is not cached: the caller gets NULL, never a half-filled buffer.
static pthread_mutex_t g_asset_buf_mu = PTHREAD_MUTEX_INITIALIZER;
static const void *kl_AAsset_getBuffer(kl_asset *a) {
    if (!a || !a->f) return NULL;
    void *have = __atomic_load_n(&a->buf, __ATOMIC_ACQUIRE);
    if (have) return have;
    if (a->len < 0) return NULL;
    pthread_mutex_lock(&g_asset_buf_mu);
    if (a->buf) { pthread_mutex_unlock(&g_asset_buf_mu); return a->buf; }
    void *buf = malloc((size_t)a->len + 1);
    if (!buf) { pthread_mutex_unlock(&g_asset_buf_mu); return NULL; }
    off_t was = ftello(a->f);
    fseeko(a->f, 0, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)a->len, a->f);
    fseeko(a->f, was, SEEK_SET);
    if (got < (size_t)a->len) { free(buf); pthread_mutex_unlock(&g_asset_buf_mu); return NULL; }
    ((char *)buf)[got] = '\0';
    __atomic_store_n(&a->buf, buf, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_asset_buf_mu);
    return buf;
}
// KL-TEST-END aasset

// ...and the same asset as a file descriptor plus a window into it. Android
// answers with a descriptor onto the APK and a non-zero offset, which is why the
// out-parameters exist at all; here every asset is its own file, so the window
// is the whole of it. Refused for a compressed asset on Android — nothing here
// is compressed, so this never takes that arm.
//
// Both out-parameters are WRITTEN on success and left alone on failure, which is
// the rule everywhere here: a caller that reads an out-parameter we never wrote
// is reading its own uninitialised stack.
static int kl_AAsset_openFileDescriptor(kl_asset *a, int64_t *out_start, int64_t *out_len) {
    if (!a || !a->path[0]) return -1;
    int fd = open(a->path, O_RDONLY | O_CLOEXEC);
    // An asset served from the OBB (kl_obbmap_fopen above) has no file at its
    // path: hand out the OBB read-through descriptor instead, whose guest
    // read/pread/lseek are translated into the entry's byte window, so the
    // window seen through it starts at 0 exactly like a loose file's.
    if (fd < 0) fd = kl_obbmap_open(a->path, O_RDONLY);
    if (fd < 0) return -1;
    if (out_start) *out_start = 0;
    if (out_len)   *out_len   = a->len;
    return fd;
}

// KL-TEST-BEGIN aasset
// 0 is end of asset; -1 is a read error (what AAsset_read documents), not a silent 0.
static int kl_AAsset_read(kl_asset *a, void *buf, size_t count) {
    if (!a || !a->f) return -1;
    size_t got = fread(buf, 1, count, a->f);
    if (got == 0 && ferror(a->f)) { clearerr(a->f); return -1; }
    return (int)got;
}
// KL-TEST-END aasset
static int64_t kl_AAsset_getLength64(kl_asset *a) { return a ? a->len : -1; }
static int64_t kl_AAsset_seek64(kl_asset *a, int64_t off, int whence) {
    if (!a || !a->f || fseeko(a->f, off, whence) != 0) return -1;
    return ftello(a->f);
}
static void kl_AAsset_close(kl_asset *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    free(a->buf);
    free(a);
}
// The 32-bit spelling. Both exist in the NDK and the newer SDL3 in the VR build
// imports this one; truncating is what Android does too, and no asset here is
// anywhere near 2 GB.
static int64_t kl_AAsset_getLength(kl_asset *a) { return a ? (int64_t)a->len : -1; }  // off_t

// The rest of the NDK's AAsset surface (CRI ADX2 imports AAsset_seek and
// AAsset_openFileDescriptor64). off_t and off64_t are both 64-bit on arm64
// bionic, so the plain spellings ARE the 64-bit ones.
static int64_t kl_AAsset_seek(kl_asset *a, int64_t off, int whence) {
    return kl_AAsset_seek64(a, off, whence);
}
static int kl_AAsset_openFileDescriptor64(kl_asset *a, int64_t *out_start, int64_t *out_len) {
    return kl_AAsset_openFileDescriptor(a, out_start, out_len);
}
static int64_t kl_AAsset_getRemainingLength64(kl_asset *a) {
    if (!a || !a->f) return -1;
    int64_t at = (int64_t)ftello(a->f);
    return at < 0 ? -1 : (a->len > at ? a->len - at : 0);
}

// "Is the buffer a heap copy rather than a mapping of the APK": ours always is
// once getBuffer has run (Android answers per-asset; nothing branches on it
// beyond choosing whether to cache).
static int kl_AAsset_isAllocated(kl_asset *a) { return a && a->buf ? 1 : 0; }

// Directory enumeration. Android returns FILE names only — no subdirectories,
// no "." or ".." — and returns NULL to end iteration, which is also how the
// caller learns the directory was empty. The name belongs to the AAssetDir and
// is invalidated by the next call, so it is stored in the handle rather than
// strdup'd: a caller that keeps it is relying on Android's lifetime, and
// matching that exactly is what keeps a bug ours rather than theirs.
typedef struct { DIR *d; char name[256]; } kl_assetdir;

static void *kl_AAssetManager_openDir(void *mgr, const char *dirname) {
    (void)mgr;
    char path[1200];
    // Same rule as AAssetManager_open: an absolute directory is a filesystem
    // path (mapped like opendir's), a relative one lives under assets/.
    if (!kl_asset_resolve(dirname && *dirname ? dirname : ".", path, sizeof path)) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    kl_assetdir *ad = calloc(1, sizeof *ad);
    if (!ad) { closedir(d); return NULL; }
    ad->d = d;
    return ad;
}

static const char *kl_AAssetDir_getNextFileName(kl_assetdir *ad) {
    if (!ad || !ad->d) return NULL;
    for (struct dirent *e; (e = readdir(ad->d)) != NULL; ) {
        if (e->d_name[0] == '.') continue;           // skip . .. and dotfiles
        if (e->d_type == DT_DIR) continue;           // files only, as Android does
        snprintf(ad->name, sizeof ad->name, "%s", e->d_name);
        return ad->name;
    }
    return NULL;
}

static void kl_AAssetDir_rewind(kl_assetdir *ad) {
    if (ad && ad->d) rewinddir(ad->d);
}

static void kl_AAssetDir_close(kl_assetdir *ad) {
    if (!ad) return;
    if (ad->d) closedir(ad->d);
    free(ad);
}

// =================================================================== ASensor
// The head's own motion, made into Android's motion sensors. visionOS gives no
// IMU for the head (CoreMotion has none for it); what the runtime HAS is ARKit's
// head pose, published through kl_ovrp, and every sensor below is derived from
// that pose by the physics that relates it to the sensor -- nothing is invented:
//
//   gravity / accelerometer  the world's "up" expressed in the head frame, 9.80665 m/s^2.
//                            An accelerometer at rest reads exactly this; the user's own
//                            linear acceleration is NOT added (a twice-differentiated pose
//                            is noise), so a head accelerating reads as a head at rest.
//   gyroscope                the head's angular velocity in the head frame (rad/s), from
//                            successive poses (kl_ovrp_head_motion says when it has no basis,
//                            and then there is simply no event rather than a zero).
//   rotation vector /        the head orientation as a unit quaternion in Android's
//   game rotation vector     East-North-Up world: tracking space is Y-up with -Z forward, so
//                            ENU = (x, -z, y), a +90 degree turn about X applied on the left.
//                            Heading is relative to the tracking origin's forward (there is no
//                            magnetometer), which is what the GAME rotation vector means; the
//                            rotation vector reports it too with heading accuracy -1 (unknown).
// Not listed: magnetometer, light, proximity, linear acceleration, pressure (no source).
// The device frame is Android's: +X right, +Y up, +Z out of the display toward the user, which
// is exactly the head frame of an HMD (X right, Y up, Z backward).
#include <math.h>
#define KL_ASENSOR_TYPE_ACCELEROMETER        1
#define KL_ASENSOR_TYPE_GYROSCOPE            4
#define KL_ASENSOR_TYPE_GRAVITY              9
#define KL_ASENSOR_TYPE_ROTATION_VECTOR     11
#define KL_ASENSOR_TYPE_GAME_ROTATION_VECTOR 15
#define KL_GRAVITY_EARTH 9.80665f

typedef struct kl_sensor {
    const char *name, *vendor;
    int         type, min_delay_us;
    float       resolution;
    const char *string_type;
    int         fifo_max, fifo_reserved, reporting_mode, wake_up, handle;
} kl_sensor;

static const kl_sensor g_sensor_tab[] = {
    {"Head Accelerometer",         "VisionQuestPro", KL_ASENSOR_TYPE_ACCELEROMETER,        5000, 0.0024f,
     "android.sensor.accelerometer",         0, 0, 0 /* continuous */, 0, 1},
    {"Head Gyroscope",             "VisionQuestPro", KL_ASENSOR_TYPE_GYROSCOPE,            5000, 0.001f,
     "android.sensor.gyroscope",             0, 0, 0, 0, 2},
    {"Head Gravity",               "VisionQuestPro", KL_ASENSOR_TYPE_GRAVITY,              5000, 0.0024f,
     "android.sensor.gravity",               0, 0, 0, 0, 3},
    {"Head Rotation Vector",       "VisionQuestPro", KL_ASENSOR_TYPE_ROTATION_VECTOR,      5000, 5.96e-8f,
     "android.sensor.rotation_vector",       0, 0, 0, 0, 4},
    {"Head Game Rotation Vector",  "VisionQuestPro", KL_ASENSOR_TYPE_GAME_ROTATION_VECTOR, 5000, 5.96e-8f,
     "android.sensor.game_rotation_vector",  0, 0, 0, 0, 5},
};
#define KL_NSENSORS ((unsigned)(sizeof g_sensor_tab / sizeof g_sensor_tab[0]))
static const kl_sensor *g_sensors[KL_NSENSORS];     // the NULL-terminated-free list getSensorList hands out

static int kl_sensor_index(const void *s) {
    for (unsigned i = 0; i < KL_NSENSORS; i++)
        if (s == &g_sensor_tab[i]) return (int)i;
    return -1;
}

// v rotated by the CONJUGATE of q (world -> head): t = 2 * cross(-q.xyz, v); v' = v + w*t + cross(-q.xyz, t).
static void kl_q_unrotate(const float q[4], const float v[3], float out[3]) {
    float ux = -q[0], uy = -q[1], uz = -q[2];
    float tx = 2 * (uy * v[2] - uz * v[1]);
    float ty = 2 * (uz * v[0] - ux * v[2]);
    float tz = 2 * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + q[3] * tx + (uy * tz - uz * ty);
    out[1] = v[1] + q[3] * ty + (uz * tx - ux * tz);
    out[2] = v[2] + q[3] * tz + (ux * ty - uy * tx);
}

// One consistent sample of everything above, from the pose the FRONTEND last published (the live
// head, not the per-frame latch: a sensor stream faster than the frame would otherwise repeat one
// value per frame). 0 when there is no usable orientation.
int kl_ndk_motion_sample(kl_ndk_motion *m) {
    if (!m) return 0;
    float px, py, pz, q[4];
    kl_ovrp_get_head_pose(&px, &py, &pz, &q[0], &q[1], &q[2], &q[3]);
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.5f && n < 1.5f)) return 0;
    for (int i = 0; i < 4; i++) q[i] /= n;

    const float up[3] = { 0.0f, KL_GRAVITY_EARTH, 0.0f };          // tracking space is Y-up
    kl_q_unrotate(q, up, m->gravity);

    // ENU = C * tracking, C = +90 degrees about X: q_enu = qC (x) q_head (Hamilton product).
    const float c = 0.70710678f;                                   // qC = (c, 0, 0, c)
    float e[4] = {
        c * q[3] + c * q[0],
        c * q[1] - c * q[2],
        c * q[2] + c * q[1],
        c * q[3] - c * q[0],
    };
    if (e[3] < 0) for (int i = 0; i < 4; i++) e[i] = -e[i];        // Android reports w >= 0
    memcpy(m->quat, e, sizeof e);

    // Tilt for a consumer that wants angles (UE4's `tilt`): SensorManager.getOrientation() of an
    // upright device sits at pitch -90 degrees -- gimbal lock, where azimuth and roll are noise --
    // which is what a headset always is, so the angles are taken the way remapCoordinateSystem for
    // an upright/camera device gives them: azimuth = compass heading of where the head LOOKS
    // (0 = tracking-origin forward, clockwise), pitch = elevation of the gaze (+ up), roll = head
    // roll about the gaze (+ when the right ear rises). All well-conditioned for an upright head.
    float x = e[0], y = e[1], z = e[2], w = e[3];
    float fwd_e = -2 * (x * z + y * w), fwd_n = -2 * (y * z - x * w), fwd_u = -(1 - 2 * (x * x + y * y));
    float right_u = 2 * (x * z - y * w), up_u = 2 * (y * z + x * w);
    float su = fwd_u; if (su > 1) su = 1; if (su < -1) su = -1;
    m->tilt[0] = atan2f(fwd_e, fwd_n);
    m->tilt[1] = asinf(su);
    m->tilt[2] = atan2f(right_u, up_u);
    (void)w;

    // Head-frame angular velocity: the world rate rotated into the head frame.
    float lin[3], ang[3];
    m->rate_valid = kl_ovrp_head_motion(lin, ang);
    if (m->rate_valid) kl_q_unrotate(q, ang, m->rate);
    else m->rate[0] = m->rate[1] = m->rate[2] = 0;
    return 1;
}

// Bionic's ASensorEvent: 104 bytes. The vector sensors report x,y,z in data[0..2] and the accuracy
// STATUS as the int8 right after them (sensor_vec_t.status = data[3]'s first byte); the rotation
// vector is x,y,z,w,heading-accuracy in data[0..4].
typedef struct kl_sensor_event {
    int32_t version, sensor, type, reserved0;
    int64_t timestamp;
    float   data[16];
    uint32_t flags;
    int32_t reserved1[3];
} kl_sensor_event;

typedef struct kl_sensor_queue {
    kl_looper   *looper;
    int          ident;
    kl_looper_cb cb;
    void        *data;
    int          pipe_r, pipe_w;   // one byte per due tick, written by the ticker
    pthread_t    ticker;
    volatile int on_mask;          // bit i: sensor i enabled on this queue
    volatile int period_us, quit, started;
    int          rate_us[KL_NSENSORS];
} kl_sensor_queue;

static char g_sensor_manager;      // opaque, non-NULL handle

// The queue ticks at the fastest rate any enabled sensor asked for.
static int kl_sensor_period(const kl_sensor_queue *q) {
    int best = 0;
    for (unsigned i = 0; i < KL_NSENSORS; i++)
        if ((q->on_mask >> i) & 1) {
            int p = q->rate_us[i] > 0 ? q->rate_us[i] : 20000;
            if (p < g_sensor_tab[i].min_delay_us) p = g_sensor_tab[i].min_delay_us;
            if (!best || p < best) best = p;
        }
    return best ? best : 20000;
}

static void *kl_sensor_ticker(void *arg) {
    kl_sensor_queue *q = arg;
    while (!q->quit) {
        usleep((useconds_t)q->period_us);
        if (q->on_mask && !q->quit) { char c = 1; (void)!write(q->pipe_w, &c, 1); }
    }
    return NULL;
}

static void *kl_ASensorManager_getInstance(void) { return &g_sensor_manager; }
static void *kl_ASensorManager_getInstanceForPackage(const char *pkg) { (void)pkg; return &g_sensor_manager; }

static int kl_ASensorManager_getSensorList(void *mgr, const kl_sensor ***list) {
    (void)mgr;
    for (unsigned i = 0; i < KL_NSENSORS; i++) g_sensors[i] = &g_sensor_tab[i];
    if (list) *list = g_sensors;
    return (int)KL_NSENSORS;
}

static const kl_sensor *kl_ASensorManager_getDefaultSensor(void *mgr, int type) {
    (void)mgr;
    for (unsigned i = 0; i < KL_NSENSORS; i++)
        if (g_sensor_tab[i].type == type) return &g_sensor_tab[i];
    return NULL;
}

static void *kl_ASensorManager_createEventQueue(void *mgr, kl_looper *looper, int ident,
                                                kl_looper_cb cb, void *data) {
    (void)mgr;
    kl_sensor_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    int p[2];
    if (pipe(p) != 0) { free(q); return NULL; }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);
    q->looper = looper; q->ident = ident; q->cb = cb; q->data = data;
    q->pipe_r = p[0]; q->pipe_w = p[1];
    q->period_us = 20000;
    if (looper) kl_ALooper_addFd(looper, q->pipe_r, ident, ALOOPER_EVENT_INPUT, cb, data);
    return q;
}

static int kl_ASensorManager_destroyEventQueue(void *mgr, kl_sensor_queue *q) {
    (void)mgr;
    if (!q) return -EINVAL;
    if (q->started) { q->quit = 1; pthread_join(q->ticker, NULL); }
    if (q->looper) kl_ALooper_removeFd(q->looper, q->pipe_r);
    close(q->pipe_r); close(q->pipe_w);
    free(q);
    return 0;
}

static int kl_sensor_enable(kl_sensor_queue *q, const void *s, int32_t period_us) {
    int i = kl_sensor_index(s);
    if (!q || i < 0) return -EINVAL;
    if (period_us > 0) q->rate_us[i] = period_us;
    q->on_mask |= 1 << i;
    q->period_us = kl_sensor_period(q);
    if (!q->started) {
        if (pthread_create(&q->ticker, NULL, kl_sensor_ticker, q) != 0) { q->on_mask &= ~(1 << i); return -EAGAIN; }
        q->started = 1;
    }
    return 0;
}
static int kl_ASensorEventQueue_enableSensor(void *qp, const void *s) { return kl_sensor_enable(qp, s, 0); }
static int kl_ASensorEventQueue_disableSensor(void *qp, const void *s) {
    kl_sensor_queue *q = qp;
    int i = kl_sensor_index(s);
    if (!q || i < 0) return -EINVAL;
    q->on_mask &= ~(1 << i);
    q->period_us = kl_sensor_period(q);
    return 0;
}
static int kl_ASensorEventQueue_setEventRate(void *qp, const void *s, int32_t us) {
    kl_sensor_queue *q = qp;
    int i = kl_sensor_index(s);
    if (!q || i < 0) return -EINVAL;
    q->rate_us[i] = us;
    q->period_us = kl_sensor_period(q);
    return 0;
}
static int kl_ASensorEventQueue_hasEvents(void *qp) {
    kl_sensor_queue *q = qp;
    if (!q) return -EINVAL;
    struct pollfd pf = { q->pipe_r, POLLIN, 0 };
    return poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN) ? 1 : 0;
}

// ssize_t: 0 means "no events pending", which is not an error. A tick yields one event per enabled
// sensor (as many as `count` holds, in sensor order); the pipe is drained and the sample taken now,
// and a tick with no measurement behind it produces nothing rather than a zero.
static ssize_t kl_ASensorEventQueue_getEvents(void *qp, void *events, size_t count) {
    kl_sensor_queue *q = qp;
    if (!q || !events) return -EINVAL;
    char buf[64];
    ssize_t n = read(q->pipe_r, buf, sizeof buf);
    if (n <= 0 || !q->on_mask || count < 1) return 0;
    kl_ndk_motion m;
    if (!kl_ndk_motion_sample(&m)) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    kl_sensor_event *out = events;
    size_t k = 0;
    for (unsigned i = 0; i < KL_NSENSORS && k < count; i++) {
        if (!((q->on_mask >> i) & 1)) continue;
        const kl_sensor *s = &g_sensor_tab[i];
        kl_sensor_event *e = &out[k];
        memset(e, 0, sizeof *e);
        e->version = (int32_t)sizeof *e; e->sensor = s->handle; e->type = s->type;
        e->timestamp = now;
        switch (s->type) {
        case KL_ASENSOR_TYPE_ACCELEROMETER:
        case KL_ASENSOR_TYPE_GRAVITY:
            memcpy(e->data, m.gravity, sizeof m.gravity);
            ((int8_t *)&e->data[3])[0] = 3;               // SENSOR_STATUS_ACCURACY_HIGH
            break;
        case KL_ASENSOR_TYPE_GYROSCOPE:
            if (!m.rate_valid) continue;                   // no basis for a rate: no event, not a zero
            memcpy(e->data, m.rate, sizeof m.rate);
            ((int8_t *)&e->data[3])[0] = 3;
            break;
        case KL_ASENSOR_TYPE_ROTATION_VECTOR:
        case KL_ASENSOR_TYPE_GAME_ROTATION_VECTOR:
            memcpy(e->data, m.quat, sizeof m.quat);
            e->data[4] = s->type == KL_ASENSOR_TYPE_ROTATION_VECTOR ? -1.0f : 0.0f;   // heading accuracy unknown
            break;
        }
        k++;
    }
    return (ssize_t)k;
}

static const char *kl_ASensor_getStringType(const kl_sensor *s) { return s && s->string_type ? s->string_type : ""; }
static int kl_ASensor_getFifoMaxEventCount(const kl_sensor *s)      { return s ? s->fifo_max : 0; }
static int kl_ASensor_getFifoReservedEventCount(const kl_sensor *s) { return s ? s->fifo_reserved : 0; }
static int kl_ASensor_getReportingMode(const kl_sensor *s)          { return s ? s->reporting_mode : -1; }
static int kl_ASensor_isWakeUpSensor(const kl_sensor *s)            { return s ? s->wake_up : 0; }
static int kl_ASensor_getHandle(const kl_sensor *s)                 { return s ? s->handle : -1; }
// Direct channels (ashmem/hardware-buffer sensor streams) need a sensor that advertises them;
// none does here.
static int kl_ASensor_isDirectChannelTypeSupported(const kl_sensor *s, int type) { (void)s; (void)type; return 0; }
static int kl_ASensor_getHighestDirectReportRateLevel(const kl_sensor *s) { (void)s; return 0; /* ASENSOR_DIRECT_RATE_STOP */ }
static const kl_sensor *kl_ASensorManager_getDefaultSensorEx(void *mgr, int type, int wakeUp) {
    (void)mgr;
    for (unsigned i = 0; i < KL_NSENSORS; i++)
        if (g_sensor_tab[i].type == type && (!!g_sensor_tab[i].wake_up == !!wakeUp)) return &g_sensor_tab[i];
    return NULL;
}
static int kl_ASensorManager_createSharedMemoryDirectChannel(void *mgr, int fd, size_t size) {
    (void)mgr; (void)fd; (void)size; return -EINVAL;
}
static int kl_ASensorManager_createHardwareBufferDirectChannel(void *mgr, const void *hb, size_t size) {
    (void)mgr; (void)hb; (void)size; return -EINVAL;
}
static void kl_ASensorManager_destroyDirectChannel(void *mgr, int channel) { (void)mgr; (void)channel; }
static int kl_ASensorManager_configureDirectReport(void *mgr, const kl_sensor *s, int channel, int rate) {
    (void)mgr; (void)s; (void)channel; (void)rate; return -EINVAL;
}
// registerSensor: enable with a sampling period (latency is accepted: there is no FIFO to batch in).
static int kl_ASensorEventQueue_registerSensor(void *q, const void *s, int32_t period_us, int64_t latency_us) {
    (void)latency_us;
    return kl_sensor_enable(q, s, period_us);
}
static int kl_ASensorEventQueue_requestAdditionalInfoEvents(void *q, int enable) {
    (void)enable; return q ? 0 : -EINVAL;
}

static const char *kl_ASensor_getName(const kl_sensor *s)   { return s ? s->name   : ""; }
static const char *kl_ASensor_getVendor(const kl_sensor *s) { return s ? s->vendor : ""; }
static int   kl_ASensor_getType(const kl_sensor *s)         { return s ? s->type : -1 /* ASENSOR_TYPE_INVALID */; }
static float kl_ASensor_getResolution(const kl_sensor *s)   { return s ? s->resolution : 0.0f; }
static int   kl_ASensor_getMinDelay(const kl_sensor *s)     { return s ? s->min_delay_us : 0; }

// ============================================================ AConfiguration
// Moved to kl_android.c with the rest of the family: the headset's configuration
// (xhdpi, landscape, SDK 32, the system locale) and every getter/setter. A new
// AConfiguration is still all-ANY; fromAssetManager fills it.

// ========================================================== ANativeActivity
// The two calls the guest makes back INTO the activity. Both are requests to
// the framework, not queries, so there is nothing to answer — but neither is a
// no-op in meaning, and a silent one invents an answer.
static void kl_ANativeActivity_finish(void *act) {
    (void)act;
    fprintf(stderr, "  [ndk] ANativeActivity_finish() — the guest is asking to "
                    "close the activity\n");
}
// The window's pixel format, asked for before the window exists. UE4 calls this
// from its glue as the first thing after onStart, with one of the
// WINDOW_FORMAT_* constants (RGBA_8888 = 1, RGBX_8888 = 2, RGB_565 = 4).
//
// Recorded rather than applied, and that is not a shrug: the surface the guest
// actually renders into here is ANGLE's, whose format is settled by the EGL
// config the guest itself chooses through eglChooseConfig — so honouring this
// would mean overriding a later, more specific request with an earlier, vaguer
// one. Android's own behaviour is close to that: setWindowFormat is a HINT to
// the window manager and the EGL config still wins for what gets drawn.
//
// Named on the way through, because a format we ignore is exactly the thing to
// know about if a guest's colours ever come out wrong.
static void kl_ANativeActivity_setWindowFormat(void *act, int format) {
    (void)act;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ndk] ANativeActivity_setWindowFormat(%d) — recorded, "
                        "not applied: the EGL config the guest chooses decides "
                        "the surface format here\n", format);
    }
}
static void kl_ANativeActivity_showSoftInput(void *act, unsigned flags) {
    (void)act;
    fprintf(stderr, "  [ndk] ANativeActivity_showSoftInput(0x%x) — no soft "
                    "keyboard here\n", flags);
}

// ================================================================ AInputQueue
// A real input queue, for a NativeActivity guest (native_app_glue and everything built on it).
// The host that creates the ANativeActivity hands the guest kl_ndk_input_queue() in
// onInputQueueCreated; the guest attaches it to its looper and drains it with getEvent/finishEvent.
// Events arrive two ways: kl_ndk_input_push_key / kl_ndk_input_push_motion (a frontend or the text
// keyboard), and -- once a looper is attached -- the Touch controllers, presented as the gamepad
// Android shows a NativeActivity app for them (A/B/X/Y, sticks, triggers, bumpers, start/select).
// Events are owned by the queue until finishEvent, as on Android.
enum { KL_AINPUT_EVENT_KEY = 1, KL_AINPUT_EVENT_MOTION = 2 };
enum { KL_AKEY_ACTION_DOWN = 0, KL_AKEY_ACTION_UP = 1, KL_AMOTION_ACTION_MOVE = 2 };
enum { KL_AINPUT_SOURCE_GAMEPAD = 0x00000401, KL_AINPUT_SOURCE_JOYSTICK = 0x01000010 };
enum { KL_AXIS_X = 0, KL_AXIS_Y = 1, KL_AXIS_Z = 11, KL_AXIS_RZ = 14,
       KL_AXIS_LTRIGGER = 17, KL_AXIS_RTRIGGER = 18, KL_AXIS_MAX = 48 };
enum { KL_AKEYCODE_BUTTON_A = 96, KL_AKEYCODE_BUTTON_B = 97, KL_AKEYCODE_BUTTON_X = 99,
       KL_AKEYCODE_BUTTON_Y = 100, KL_AKEYCODE_BUTTON_L1 = 102, KL_AKEYCODE_BUTTON_R1 = 103,
       KL_AKEYCODE_BUTTON_L2 = 104, KL_AKEYCODE_BUTTON_R2 = 105, KL_AKEYCODE_BUTTON_THUMBL = 106,
       KL_AKEYCODE_BUTTON_THUMBR = 107, KL_AKEYCODE_BUTTON_START = 108, KL_AKEYCODE_BUTTON_SELECT = 109 };

#define KL_INPUT_MAGIC 0x4b4c4945u      /* 'KLIE' */
#define KL_INPUT_RING 128
typedef struct kl_input_event {
    uint32_t magic;
    int32_t  type, source, device_id, action, flags, meta, keycode, scancode, repeat, button_state;
    int64_t  down_time, event_time;
    float    axes[KL_AXIS_MAX];
} kl_input_event;

typedef struct kl_input_queue {
    pthread_mutex_t lock;
    kl_input_event *ring[KL_INPUT_RING];
    unsigned        head, count;
    int             pipe_r, pipe_w;
    kl_looper      *looper;
    pthread_t       poller;
    volatile int    poller_on;
    int             poller_started;   // thread exists and has not been joined (under g_poller_mu)
} kl_input_queue;
static pthread_mutex_t g_poller_mu = PTHREAD_MUTEX_INITIALIZER;
static kl_input_queue  g_inq;
static pthread_once_t  g_inq_once = PTHREAD_ONCE_INIT;

static int64_t kl_input_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void kl_inq_init(void) {
    pthread_mutex_init(&g_inq.lock, NULL);
    int p[2];
    if (pipe(p) != 0) { g_inq.pipe_r = g_inq.pipe_w = -1; return; }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);
    g_inq.pipe_r = p[0]; g_inq.pipe_w = p[1];
}

void *kl_ndk_input_queue(void) { pthread_once(&g_inq_once, kl_inq_init); return &g_inq; }

static void kl_inq_push(kl_input_event *e) {
    pthread_once(&g_inq_once, kl_inq_init);
    pthread_mutex_lock(&g_inq.lock);
    if (g_inq.count == KL_INPUT_RING) {                // a guest that never drains: drop the oldest
        free(g_inq.ring[g_inq.head]);
        g_inq.head = (g_inq.head + 1) % KL_INPUT_RING; g_inq.count--;
        char c; (void)!read(g_inq.pipe_r, &c, 1);
    }
    g_inq.ring[(g_inq.head + g_inq.count++) % KL_INPUT_RING] = e;
    pthread_mutex_unlock(&g_inq.lock);
    char c = 1; (void)!write(g_inq.pipe_w, &c, 1);
}

void kl_ndk_input_push_key(int source, int device_id, int action, int keycode, int meta) {
    kl_input_event *e = calloc(1, sizeof *e);
    if (!e) return;
    e->magic = KL_INPUT_MAGIC; e->type = KL_AINPUT_EVENT_KEY;
    e->source = source; e->device_id = device_id; e->action = action; e->keycode = keycode; e->meta = meta;
    e->down_time = e->event_time = kl_input_now_ns();
    kl_inq_push(e);
}

void kl_ndk_input_push_motion(int source, int device_id, int action, const float *axes, int naxes) {
    kl_input_event *e = calloc(1, sizeof *e);
    if (!e) return;
    e->magic = KL_INPUT_MAGIC; e->type = KL_AINPUT_EVENT_MOTION;
    e->source = source; e->device_id = device_id; e->action = action;
    if (axes) memcpy(e->axes, axes, (size_t)(naxes < KL_AXIS_MAX ? naxes : KL_AXIS_MAX) * sizeof(float));
    e->down_time = e->event_time = kl_input_now_ns();
    kl_inq_push(e);
}

// The controllers as a gamepad. Left = X/Y/left stick/L1/L2, right = A/B/right stick/R1/R2; the
// menu buttons ride the left hand's START/BACK words. Sticks go out as one MOVE event whenever
// either changes by more than the dead band.
static const struct { uint32_t bit; int key; } kl_pad_map[2][6] = {
    { {KL_OVRP_RAW_X, KL_AKEYCODE_BUTTON_X}, {KL_OVRP_RAW_Y, KL_AKEYCODE_BUTTON_Y},
      {KL_OVRP_RAW_LTHUMBSTICK, KL_AKEYCODE_BUTTON_THUMBL}, {KL_OVRP_RAW_START, KL_AKEYCODE_BUTTON_START},
      {KL_OVRP_RAW_BACK, KL_AKEYCODE_BUTTON_SELECT}, {0, 0} },
    { {KL_OVRP_RAW_A, KL_AKEYCODE_BUTTON_A}, {KL_OVRP_RAW_B, KL_AKEYCODE_BUTTON_B},
      {KL_OVRP_RAW_RTHUMBSTICK, KL_AKEYCODE_BUTTON_THUMBR}, {0, 0}, {0, 0}, {0, 0} },
};

static void *kl_input_poller(void *arg) {
    (void)arg;
    uint32_t prev_btn[2] = {0, 0};
    int      prev_trig[2][2] = {{0, 0}, {0, 0}};      // [hand][0 index, 1 grip]
    float    prev_axes[KL_AXIS_MAX] = {0};
    while (g_inq.poller_on) {
        usleep(8000);
        float axes[KL_AXIS_MAX] = {0};
        for (int hand = 0; hand < 2; hand++) {
            uint32_t b = 0, t = 0; float it = 0, gt = 0, sx = 0, sy = 0;
            if (!kl_ovrp_controller_input(hand, &b, &t, &it, &gt, &sx, &sy)) { b = 0; it = gt = sx = sy = 0; }
            for (int k = 0; k < 6; k++) {
                uint32_t bit = kl_pad_map[hand][k].bit;
                if (!bit) continue;
                if ((b ^ prev_btn[hand]) & bit)
                    kl_ndk_input_push_key(KL_AINPUT_SOURCE_GAMEPAD, 1, (b & bit) ? KL_AKEY_ACTION_DOWN : KL_AKEY_ACTION_UP,
                                          kl_pad_map[hand][k].key, 0);
            }
            prev_btn[hand] = b;
            int trig[2] = { it > 0.5f, gt > 0.5f };
            static const int tkey[2][2] = { {KL_AKEYCODE_BUTTON_L2, KL_AKEYCODE_BUTTON_L1},
                                            {KL_AKEYCODE_BUTTON_R2, KL_AKEYCODE_BUTTON_R1} };
            for (int k = 0; k < 2; k++) {
                if (trig[k] != prev_trig[hand][k])
                    kl_ndk_input_push_key(KL_AINPUT_SOURCE_GAMEPAD, 1, trig[k] ? KL_AKEY_ACTION_DOWN : KL_AKEY_ACTION_UP,
                                          tkey[hand][k], 0);
                prev_trig[hand][k] = trig[k];
            }
            if (hand == 0) { axes[KL_AXIS_X] = sx;  axes[KL_AXIS_Y] = -sy; axes[KL_AXIS_LTRIGGER] = it; }
            else           { axes[KL_AXIS_Z] = sx;  axes[KL_AXIS_RZ] = -sy; axes[KL_AXIS_RTRIGGER] = it; }
        }
        int changed = 0;
        for (int a = 0; a < KL_AXIS_MAX; a++) if (fabsf(axes[a] - prev_axes[a]) > 0.02f) changed = 1;
        if (changed) {
            kl_ndk_input_push_motion(KL_AINPUT_SOURCE_GAMEPAD | KL_AINPUT_SOURCE_JOYSTICK, 1, KL_AMOTION_ACTION_MOVE,
                                     axes, KL_AXIS_MAX);
            memcpy(prev_axes, axes, sizeof axes);
        }
    }
    return NULL;
}

static void kl_AInputQueue_attachLooper(void *qp, void *looper, int ident, void *cb, void *data) {
    kl_input_queue *q = qp;
    if (q != &g_inq || !looper) return;
    q->looper = looper;
    kl_ALooper_addFd(looper, q->pipe_r, ident, ALOOPER_EVENT_INPUT, (kl_looper_cb)cb, data);
    // One poller at most: detachLooper joins it, so a second attach never overlaps a dying one.
    pthread_mutex_lock(&g_poller_mu);
    if (!q->poller_started) {
        q->poller_on = 1;
        if (pthread_create(&q->poller, NULL, kl_input_poller, NULL) != 0) q->poller_on = 0;
        else q->poller_started = 1;
    }
    pthread_mutex_unlock(&g_poller_mu);
}
static void kl_AInputQueue_detachLooper(void *qp) {
    kl_input_queue *q = qp;
    if (q != &g_inq || !q->looper) return;
    kl_ALooper_removeFd(q->looper, q->pipe_r);
    q->looper = NULL;
    pthread_mutex_lock(&g_poller_mu);
    q->poller_on = 0;
    if (q->poller_started) { pthread_join(q->poller, NULL); q->poller_started = 0; }   // exits within one 8 ms tick
    pthread_mutex_unlock(&g_poller_mu);
}
static int32_t kl_AInputQueue_hasEvents(void *qp) {
    kl_input_queue *q = qp;
    if (q != &g_inq) return -EINVAL;
    pthread_mutex_lock(&q->lock);
    int n = q->count > 0;
    pthread_mutex_unlock(&q->lock);
    return n;
}
// 0 and the event, or a negative value for "no event" (-EAGAIN) / a foreign queue (-EINVAL).
static int32_t kl_AInputQueue_getEvent(void *qp, void **out) {
    kl_input_queue *q = qp;
    if (out) *out = NULL;
    if (q != &g_inq || !out) return -EINVAL;
    pthread_mutex_lock(&q->lock);
    if (!q->count) { pthread_mutex_unlock(&q->lock); return -EAGAIN; }
    kl_input_event *e = q->ring[q->head];
    q->head = (q->head + 1) % KL_INPUT_RING; q->count--;
    pthread_mutex_unlock(&q->lock);
    char c; (void)!read(q->pipe_r, &c, 1);
    *out = e;
    return 0;
}
// An IME would get the first look at a key; there is none, so nothing is pre-dispatched.
static int32_t kl_AInputQueue_preDispatchEvent(void *q, void *ev) { (void)q; (void)ev; return 0; }
static void    kl_AInputQueue_finishEvent(void *q, void *ev, int handled) {
    (void)q; (void)handled;
    kl_input_event *e = ev;
    if (e && e->magic == KL_INPUT_MAGIC) { e->magic = 0; free(e); }
}

static const kl_input_event *kl_ie(const void *ev) {
    const kl_input_event *e = ev;
    return e && e->magic == KL_INPUT_MAGIC ? e : NULL;
}
#define KL_IE_GET(rettype, sym, field) \
    static rettype kl_##sym(const void *ev) { const kl_input_event *e = kl_ie(ev); return e ? (rettype)e->field : 0; }
KL_IE_GET(int32_t, AInputEvent_getType,       type)
KL_IE_GET(int32_t, AInputEvent_getDeviceId,   device_id)
KL_IE_GET(int32_t, AInputEvent_getSource,     source)
KL_IE_GET(int32_t, AKeyEvent_getAction,       action)
KL_IE_GET(int32_t, AKeyEvent_getFlags,        flags)
KL_IE_GET(int32_t, AKeyEvent_getKeyCode,      keycode)
KL_IE_GET(int32_t, AKeyEvent_getScanCode,     scancode)
KL_IE_GET(int32_t, AKeyEvent_getMetaState,    meta)
KL_IE_GET(int32_t, AKeyEvent_getRepeatCount,  repeat)
KL_IE_GET(int64_t, AKeyEvent_getDownTime,     down_time)
KL_IE_GET(int64_t, AKeyEvent_getEventTime,    event_time)
KL_IE_GET(int32_t, AMotionEvent_getAction,    action)
KL_IE_GET(int32_t, AMotionEvent_getFlags,     flags)
KL_IE_GET(int32_t, AMotionEvent_getMetaState, meta)
KL_IE_GET(int32_t, AMotionEvent_getButtonState, button_state)
KL_IE_GET(int64_t, AMotionEvent_getDownTime,  down_time)
KL_IE_GET(int64_t, AMotionEvent_getEventTime, event_time)
static size_t  kl_AMotionEvent_getPointerCount(const void *ev) { return kl_ie(ev) ? 1 : 0; }
static int32_t kl_AMotionEvent_getPointerId(const void *ev, size_t i) { (void)i; (void)ev; return 0; }
static int32_t kl_AMotionEvent_getToolType(const void *ev, size_t i) { (void)i; (void)ev; return 0; /* UNKNOWN */ }
static float   kl_AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t i) {
    const kl_input_event *e = kl_ie(ev);
    return e && i == 0 && axis >= 0 && axis < KL_AXIS_MAX ? e->axes[axis] : 0.0f;
}
static float kl_AMotionEvent_getX(const void *ev, size_t i)    { return kl_AMotionEvent_getAxisValue(ev, KL_AXIS_X, i); }
static float kl_AMotionEvent_getY(const void *ev, size_t i)    { return kl_AMotionEvent_getAxisValue(ev, KL_AXIS_Y, i); }
static float kl_AMotionEvent_getRawX(const void *ev, size_t i) { return kl_AMotionEvent_getAxisValue(ev, KL_AXIS_X, i); }
static float kl_AMotionEvent_getRawY(const void *ev, size_t i) { return kl_AMotionEvent_getAxisValue(ev, KL_AXIS_Y, i); }
static float kl_AMotionEvent_getPressure(const void *ev, size_t i) { return kl_AMotionEvent_getAxisValue(ev, 2, i); }

// ========================================================= AHardwareBuffer (rest)
// libmediandk (kl_mediandk.c) serves allocate/describe/lock/unlock/acquire/release/getId; what it
// cannot is the part that needs the JNI object model, and the inter-process pair.
//
//   AHardwareBuffer_toHardwareBuffer(env, ahb)   a Java android.hardware.HardwareBuffer wrapping
//                                                ahb (takes its own reference, as Android does).
//   AHardwareBuffer_fromHardwareBuffer(env, obj) the ahb behind such an object, NO new reference
//                                                (documented); NULL for an object we did not make.
//   AHardwareBuffer_sendHandleToUnixSocket / recvHandleFromUnixSocket
//                                                the handle crosses a Unix socket between PROCESSES
//                                                on Android. Here there is one process: the socket
//                                                carries an opaque ticket, and the receiver gets
//                                                the same buffer back (with a reference of its own).
typedef void (*kl_ahb_ref_fn)(void *);
typedef int  (*kl_ahb_id_fn)(const void *, uint64_t *);
static kl_ahb_ref_fn kl_ahb_acquire_fn(void) {
    static kl_ahb_ref_fn f; if (!f) f = (kl_ahb_ref_fn)kl_mediandk_lookup("AHardwareBuffer_acquire"); return f;
}
static kl_ahb_ref_fn kl_ahb_release_fn(void) {
    static kl_ahb_ref_fn f; if (!f) f = (kl_ahb_ref_fn)kl_mediandk_lookup("AHardwareBuffer_release"); return f;
}
static kl_ahb_id_fn kl_ahb_id(void) {
    static kl_ahb_id_fn f; if (!f) f = (kl_ahb_id_fn)kl_mediandk_lookup("AHardwareBuffer_getId"); return f;
}

#define KL_AHB_JAVA_MAX 64
static struct { void *obj, *ahb; } g_ahb_java[KL_AHB_JAVA_MAX];
static pthread_mutex_t g_ahb_mu = PTHREAD_MUTEX_INITIALIZER;

static void *kl_AHardwareBuffer_toHardwareBuffer(void *env, void *ahb) {
    (void)env;
    if (!ahb) return NULL;
    pthread_mutex_lock(&g_ahb_mu);
    for (int i = 0; i < KL_AHB_JAVA_MAX; i++)                    // one Java object per buffer, as identity-stable as Java's
        if (g_ahb_java[i].ahb == ahb) { void *o = g_ahb_java[i].obj; pthread_mutex_unlock(&g_ahb_mu); return o; }
    int slot = -1;
    for (int i = 0; i < KL_AHB_JAVA_MAX; i++) if (!g_ahb_java[i].obj) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_ahb_mu); return NULL; }
    void *obj = kl_jni_new_object("android/hardware/HardwareBuffer");
    if (!obj) { pthread_mutex_unlock(&g_ahb_mu); return NULL; }
    kl_jni_pin_object(obj);
    kl_ahb_ref_fn acq = kl_ahb_acquire_fn();
    if (acq) acq(ahb);                                           // the Java object's own reference
    g_ahb_java[slot].obj = obj; g_ahb_java[slot].ahb = ahb;
    pthread_mutex_unlock(&g_ahb_mu);
    return obj;
}
static void *kl_AHardwareBuffer_fromHardwareBuffer(void *env, void *obj) {
    (void)env;
    if (!obj) return NULL;
    void *r = NULL;
    pthread_mutex_lock(&g_ahb_mu);
    for (int i = 0; i < KL_AHB_JAVA_MAX; i++) if (g_ahb_java[i].obj == obj) { r = g_ahb_java[i].ahb; break; }
    pthread_mutex_unlock(&g_ahb_mu);
    return r;
}

// Ticket on the wire: magic + the buffer's id. The sent table keeps a reference until received.
#define KL_AHB_TICKET 0x4b4c4148u            /* 'KLAH' */
#define KL_AHB_SENT_MAX 32
static struct { uint64_t id; void *ahb; } g_ahb_sent[KL_AHB_SENT_MAX];

static int kl_AHardwareBuffer_sendHandleToUnixSocket(const void *ahb, int sockfd) {
    kl_ahb_id_fn idf = kl_ahb_id();
    uint64_t id = 0;
    if (!ahb || sockfd < 0 || !idf || idf(ahb, &id) != 0) return -EINVAL;
    pthread_mutex_lock(&g_ahb_mu);
    int slot = -1;
    for (int i = 0; i < KL_AHB_SENT_MAX; i++) if (!g_ahb_sent[i].ahb) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_ahb_mu); return -ENOMEM; }
    kl_ahb_ref_fn acq = kl_ahb_acquire_fn();
    if (acq) acq((void *)ahb);
    g_ahb_sent[slot].id = id; g_ahb_sent[slot].ahb = (void *)ahb;
    pthread_mutex_unlock(&g_ahb_mu);
    struct { uint32_t magic; uint64_t id; } __attribute__((packed)) t = { KL_AHB_TICKET, id };
    ssize_t n = write(sockfd, &t, sizeof t);
    if (n != (ssize_t)sizeof t) {
        pthread_mutex_lock(&g_ahb_mu);
        g_ahb_sent[slot].ahb = NULL;
        pthread_mutex_unlock(&g_ahb_mu);
        kl_ahb_ref_fn rel = kl_ahb_release_fn();
        if (rel) rel((void *)ahb);
        return n < 0 ? -errno : -EIO;
    }
    return 0;
}
static int kl_AHardwareBuffer_recvHandleFromUnixSocket(int sockfd, void **outBuffer) {
    if (!outBuffer || sockfd < 0) return -EINVAL;
    *outBuffer = NULL;
    struct { uint32_t magic; uint64_t id; } __attribute__((packed)) t;
    ssize_t n = read(sockfd, &t, sizeof t);
    if (n < 0) return -errno;
    if (n != (ssize_t)sizeof t || t.magic != KL_AHB_TICKET) return -EINVAL;
    pthread_mutex_lock(&g_ahb_mu);
    for (int i = 0; i < KL_AHB_SENT_MAX; i++)
        if (g_ahb_sent[i].ahb && g_ahb_sent[i].id == t.id) {
            *outBuffer = g_ahb_sent[i].ahb;                      // the sender's reference becomes the receiver's
            g_ahb_sent[i].ahb = NULL;
            break;
        }
    pthread_mutex_unlock(&g_ahb_mu);
    return *outBuffer ? 0 : -ENOENT;
}

// ================================================================== dispatch
static const struct { const char *name; void *fn; } g_ndk[] = {
#define N(sym, fn) {sym, (void *)(fn)}
    N("ALooper_forThread",  kl_ALooper_forThread),
    N("ALooper_prepare",    kl_ALooper_prepare),
    N("ALooper_acquire",    kl_ALooper_acquire),
    N("ALooper_release",    kl_ALooper_release),
    N("ALooper_pollOnce",   kl_ALooper_pollOnce),
    N("ALooper_pollAll",    kl_ALooper_pollAll),
    N("ALooper_wake",       kl_ALooper_wake),
    N("ALooper_addFd",      kl_ALooper_addFd),
    N("ALooper_removeFd",   kl_ALooper_removeFd),

    N("ANativeWindow_acquire",            kl_ANativeWindow_acquire),
    N("ANativeWindow_release",            kl_ANativeWindow_release),
    N("ANativeWindow_fromSurface",        kl_ANativeWindow_fromSurface),
    N("ANativeWindow_getWidth",           kl_ANativeWindow_getWidth),
    N("ANativeWindow_getHeight",          kl_ANativeWindow_getHeight),
    N("ANativeWindow_getFormat",          kl_ANativeWindow_getFormat),
    N("ANativeWindow_setBuffersGeometry", kl_ANativeWindow_setBuffersGeometry),
    N("ANativeWindow_setBuffersTransform",  kl_ANativeWindow_setBuffersTransform),
    N("ANativeWindow_setBuffersDataSpace",  kl_ANativeWindow_setBuffersDataSpace),
    N("ANativeWindow_getBuffersDataSpace",  kl_ANativeWindow_getBuffersDataSpace),
    N("ANativeWindow_setFrameRate",         kl_ANativeWindow_setFrameRate),
    N("ANativeWindow_setFrameRateWithChangeStrategy", kl_ANativeWindow_setFrameRateWithChangeStrategy),
    N("ANativeWindow_clearFrameRate",       kl_ANativeWindow_clearFrameRate),
    N("ANativeWindow_tryAllocateBuffers",   kl_ANativeWindow_tryAllocateBuffers),
    N("ANativeWindow_lock",               kl_ANativeWindow_lock),
    N("ANativeWindow_unlockAndPost",      kl_ANativeWindow_unlockAndPost),

    N("AAssetManager_fromJava", kl_AAssetManager_fromJava),
    N("AAssetManager_open",     kl_AAssetManager_open),
    N("AAsset_read",            kl_AAsset_read),
    N("AAsset_getLength64",     kl_AAsset_getLength64),
    N("AAsset_seek64",          kl_AAsset_seek64),
    N("AAsset_close",           kl_AAsset_close),
    N("AAsset_getLength",       kl_AAsset_getLength),
    N("AAsset_getBuffer",       kl_AAsset_getBuffer),
    N("AAsset_openFileDescriptor", kl_AAsset_openFileDescriptor),
    N("AAsset_openFileDescriptor64", kl_AAsset_openFileDescriptor64),
    N("AAsset_seek",            kl_AAsset_seek),
    N("AAsset_getRemainingLength",   kl_AAsset_getRemainingLength64),  // off_t: 64-bit on arm64
    N("AAsset_getRemainingLength64", kl_AAsset_getRemainingLength64),
    N("AAsset_isAllocated",     kl_AAsset_isAllocated),
    N("AAssetDir_rewind",       kl_AAssetDir_rewind),
    N("AAssetManager_openDir",  kl_AAssetManager_openDir),
    N("AAssetDir_getNextFileName", kl_AAssetDir_getNextFileName),
    N("AAssetDir_close",        kl_AAssetDir_close),

    N("ASensorManager_getInstance",           kl_ASensorManager_getInstance),
    N("ASensorManager_getInstanceForPackage", kl_ASensorManager_getInstanceForPackage),
    N("ASensorManager_getSensorList",         kl_ASensorManager_getSensorList),
    N("ASensorManager_getDefaultSensor",      kl_ASensorManager_getDefaultSensor),
    N("ASensorManager_createEventQueue",      kl_ASensorManager_createEventQueue),
    N("ASensorManager_destroyEventQueue",     kl_ASensorManager_destroyEventQueue),
    N("ASensorEventQueue_enableSensor",       kl_ASensorEventQueue_enableSensor),
    N("ASensorEventQueue_disableSensor",      kl_ASensorEventQueue_disableSensor),
    N("ASensorEventQueue_setEventRate",       kl_ASensorEventQueue_setEventRate),
    N("ASensorEventQueue_hasEvents",          kl_ASensorEventQueue_hasEvents),
    N("ASensorEventQueue_getEvents",          kl_ASensorEventQueue_getEvents),
    N("ASensor_getName",                      kl_ASensor_getName),
    N("ASensor_getVendor",                    kl_ASensor_getVendor),
    N("ASensor_getType",                      kl_ASensor_getType),
    N("ASensor_getResolution",                kl_ASensor_getResolution),
    N("ASensor_getMinDelay",                  kl_ASensor_getMinDelay),
    N("ASensor_getStringType",                kl_ASensor_getStringType),
    N("ASensor_getFifoMaxEventCount",         kl_ASensor_getFifoMaxEventCount),
    N("ASensor_getFifoReservedEventCount",    kl_ASensor_getFifoReservedEventCount),
    N("ASensor_getReportingMode",             kl_ASensor_getReportingMode),
    N("ASensor_isWakeUpSensor",               kl_ASensor_isWakeUpSensor),
    N("ASensor_getHandle",                    kl_ASensor_getHandle),
    N("ASensor_isDirectChannelTypeSupported", kl_ASensor_isDirectChannelTypeSupported),
    N("ASensor_getHighestDirectReportRateLevel", kl_ASensor_getHighestDirectReportRateLevel),
    N("ASensorManager_getDefaultSensorEx",    kl_ASensorManager_getDefaultSensorEx),
    N("ASensorManager_createSharedMemoryDirectChannel", kl_ASensorManager_createSharedMemoryDirectChannel),
    N("ASensorManager_createHardwareBufferDirectChannel", kl_ASensorManager_createHardwareBufferDirectChannel),
    N("ASensorManager_destroyDirectChannel",  kl_ASensorManager_destroyDirectChannel),
    N("ASensorManager_configureDirectReport", kl_ASensorManager_configureDirectReport),
    N("ASensorEventQueue_registerSensor",     kl_ASensorEventQueue_registerSensor),
    N("ASensorEventQueue_requestAdditionalInfoEvents", kl_ASensorEventQueue_requestAdditionalInfoEvents),

    N("AInputQueue_attachLooper",      kl_AInputQueue_attachLooper),
    N("AInputQueue_detachLooper",      kl_AInputQueue_detachLooper),
    N("AInputQueue_hasEvents",         kl_AInputQueue_hasEvents),
    N("AInputQueue_getEvent",          kl_AInputQueue_getEvent),
    N("AInputQueue_preDispatchEvent",  kl_AInputQueue_preDispatchEvent),
    N("AInputQueue_finishEvent",       kl_AInputQueue_finishEvent),
    N("AInputEvent_getType",           kl_AInputEvent_getType),
    N("AInputEvent_getDeviceId",       kl_AInputEvent_getDeviceId),
    N("AInputEvent_getSource",         kl_AInputEvent_getSource),
    N("AKeyEvent_getAction",           kl_AKeyEvent_getAction),
    N("AKeyEvent_getFlags",            kl_AKeyEvent_getFlags),
    N("AKeyEvent_getKeyCode",          kl_AKeyEvent_getKeyCode),
    N("AKeyEvent_getScanCode",         kl_AKeyEvent_getScanCode),
    N("AKeyEvent_getMetaState",        kl_AKeyEvent_getMetaState),
    N("AKeyEvent_getRepeatCount",      kl_AKeyEvent_getRepeatCount),
    N("AKeyEvent_getDownTime",         kl_AKeyEvent_getDownTime),
    N("AKeyEvent_getEventTime",        kl_AKeyEvent_getEventTime),
    N("AMotionEvent_getAction",        kl_AMotionEvent_getAction),
    N("AMotionEvent_getFlags",         kl_AMotionEvent_getFlags),
    N("AMotionEvent_getMetaState",     kl_AMotionEvent_getMetaState),
    N("AMotionEvent_getButtonState",   kl_AMotionEvent_getButtonState),
    N("AMotionEvent_getDownTime",      kl_AMotionEvent_getDownTime),
    N("AMotionEvent_getEventTime",     kl_AMotionEvent_getEventTime),
    N("AMotionEvent_getPointerCount",  kl_AMotionEvent_getPointerCount),
    N("AMotionEvent_getPointerId",     kl_AMotionEvent_getPointerId),
    N("AMotionEvent_getToolType",      kl_AMotionEvent_getToolType),
    N("AMotionEvent_getAxisValue",     kl_AMotionEvent_getAxisValue),
    N("AMotionEvent_getX",             kl_AMotionEvent_getX),
    N("AMotionEvent_getY",             kl_AMotionEvent_getY),
    N("AMotionEvent_getRawX",          kl_AMotionEvent_getRawX),
    N("AMotionEvent_getRawY",          kl_AMotionEvent_getRawY),
    N("AMotionEvent_getPressure",      kl_AMotionEvent_getPressure),

    N("AHardwareBuffer_toHardwareBuffer",         kl_AHardwareBuffer_toHardwareBuffer),
    N("AHardwareBuffer_fromHardwareBuffer",       kl_AHardwareBuffer_fromHardwareBuffer),
    N("AHardwareBuffer_sendHandleToUnixSocket",   kl_AHardwareBuffer_sendHandleToUnixSocket),
    N("AHardwareBuffer_recvHandleFromUnixSocket", kl_AHardwareBuffer_recvHandleFromUnixSocket),

    N("ANativeActivity_finish",        kl_ANativeActivity_finish),
    N("ANativeActivity_setWindowFormat", kl_ANativeActivity_setWindowFormat),
    N("ANativeActivity_showSoftInput", kl_ANativeActivity_showSoftInput),
#undef N
};

void *kl_ndk_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_ndk / sizeof g_ndk[0]; i++)
        if (strcmp(g_ndk[i].name, name) == 0) return g_ndk[i].fn;
    return kl_android_lookup(name);       // the rest of libandroid (kl_android.c); NULL if not served
}
