// Android NDK shim. See kl_ndk.h for the measured scope.
//
// Three independent pieces:
//   ALooper       — real implementation over poll() and a self-pipe. Unity waits
//                   on this, so a stub that returns immediately would spin.
//   ANativeWindow — a synthetic window carrying geometry. No pixels until EGL binds one.
//   ASensor       — one real sensor (head gyroscope); none invented (see below).
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

static void kl_ALooper_wake(kl_looper *l) {
    if (!l) return;
    char c = 1;
    ssize_t n = write(l->wake_w, &c, 1);
    (void)n;   // EAGAIN just means a wake is already pending, which is the point
}

static int kl_ALooper_addFd(kl_looper *l, int fd, int ident, int events,
                            kl_looper_cb cb, void *data) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {          // re-registering replaces
            l->fds[i].ident = ident; l->fds[i].events = events;
            l->fds[i].cb = cb; l->fds[i].data = data;
            pthread_mutex_unlock(&l->lock);
            return 1;
        }
    if (l->nfds == KL_LOOPER_MAX_FDS) { pthread_mutex_unlock(&l->lock); return -1; }
    l->fds[l->nfds++] = (typeof(l->fds[0])){fd, ident, events, cb, data};
    pthread_mutex_unlock(&l->lock);
    return 1;
}

static int kl_ALooper_removeFd(kl_looper *l, int fd) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {
            l->fds[i] = l->fds[--l->nfds];
            pthread_mutex_unlock(&l->lock);
            return 1;
        }
    pthread_mutex_unlock(&l->lock);
    return 0;
}

static int kl_looper_poll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    kl_looper *l = kl_ALooper_forThread();
    if (!l) return ALOOPER_POLL_ERROR;

    // Frame callbacks the vsync clock has made due for this thread run first, as
    // the Choreographer's own fd callback would: they are "a callback ran".
    if (kl_android_choreo_dispatch()) return ALOOPER_POLL_CALLBACK;

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

    int r = poll(pfd, (nfds_t)n, timeoutMillis);
    if (r < 0)  return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;

    if (pfd[0].revents & POLLIN) {         // drain the wake pipe
        char buf[64];
        while (read(l->wake_r, buf, sizeof buf) > 0) { }
        if (kl_android_choreo_dispatch()) return ALOOPER_POLL_CALLBACK;   // the wake was a vsync
        return ALOOPER_POLL_WAKE;
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
static const void *kl_AAsset_getBuffer(kl_asset *a) {
    if (!a || !a->f) return NULL;
    if (a->buf) return a->buf;
    if (a->len < 0) return NULL;
    void *buf = malloc((size_t)a->len + 1);
    if (!buf) return NULL;
    off_t was = ftello(a->f);
    fseeko(a->f, 0, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)a->len, a->f);
    fseeko(a->f, was, SEEK_SET);
    ((char *)buf)[got] = '\0';
    a->buf = buf;
    return a->buf;
}

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

static int kl_AAsset_read(kl_asset *a, void *buf, size_t count) {
    if (!a || !a->f) return -1;
    return (int)fread(buf, 1, count, a->f);
}
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
// One sensor, because it is the only one with a real source: the gyroscope.
// kl_ovrp_head_motion gives the head's angular velocity in tracking space
// (derived from successive poses, and it says when it has no basis); it is
// rotated into the head frame to match Android's device axes. Not listed,
// deliberately: the accelerometer (the runtime has no acceleration, and a
// differentiated pose is noise), the magnetometer and the proximity sensor
// (no source at all). A fabricated reading would feed the engine invented
// motion, whereas a shorter list is a configuration Android devices ship.
#define KL_ASENSOR_TYPE_GYROSCOPE 4
typedef struct kl_sensor {
    const char *name, *vendor;
    int         type, min_delay_us;
    float       resolution;
    const char *string_type;
    int         fifo_max, fifo_reserved, reporting_mode, wake_up, handle;
} kl_sensor;

static const kl_sensor kl_sensor_gyro = {
    "Quest Head Gyroscope", "VisionQuestPro", KL_ASENSOR_TYPE_GYROSCOPE, 5000,
    0.001f, "android.sensor.gyroscope", 0, 0, 0 /* continuous */, 0, 1 };
static const kl_sensor  *g_sensors[]  = { &kl_sensor_gyro };
static const unsigned    g_nsensors   = 1;

// Bionic's ASensorEvent: 104 bytes; the gyroscope vector is data[0..2], rad/s.
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
    int          pipe_r, pipe_w;   // one byte per due sample, written by the ticker
    pthread_t    ticker;
    volatile int on, period_us, quit, started;
} kl_sensor_queue;

static char g_sensor_manager;      // opaque, non-NULL handle

// Head-frame angular velocity: world rate rotated by the conjugate of the head
// orientation. Returns 0 when the runtime has no measurement.
static int kl_gyro_sample(float w[3]) {
    float lin[3], ang[3], px, py, pz, qx, qy, qz, qw;
    if (!kl_ovrp_head_motion(lin, ang)) return 0;
    kl_ovrp_get_guest_head_pose(&px, &py, &pz, &qx, &qy, &qz, &qw);
    // v' = q^-1 v q  via t = 2 * cross(-q.xyz, v); v' = v + qw*t + cross(-q.xyz, t)
    float ux = -qx, uy = -qy, uz = -qz;
    float tx = 2 * (uy * ang[2] - uz * ang[1]);
    float ty = 2 * (uz * ang[0] - ux * ang[2]);
    float tz = 2 * (ux * ang[1] - uy * ang[0]);
    w[0] = ang[0] + qw * tx + (uy * tz - uz * ty);
    w[1] = ang[1] + qw * ty + (uz * tx - ux * tz);
    w[2] = ang[2] + qw * tz + (ux * ty - uy * tx);
    return 1;
}

static void *kl_sensor_ticker(void *arg) {
    kl_sensor_queue *q = arg;
    while (!q->quit) {
        usleep(q->period_us > 0 ? (useconds_t)q->period_us : 20000);
        if (q->on && !q->quit) { char c = 1; (void)!write(q->pipe_w, &c, 1); }
    }
    return NULL;
}

static void *kl_ASensorManager_getInstance(void) { return &g_sensor_manager; }
static void *kl_ASensorManager_getInstanceForPackage(const char *pkg) { (void)pkg; return &g_sensor_manager; }

static int kl_ASensorManager_getSensorList(void *mgr, const kl_sensor ***list) {
    (void)mgr;
    if (list) *list = g_sensors;
    return (int)g_nsensors;
}

static const kl_sensor *kl_ASensorManager_getDefaultSensor(void *mgr, int type) {
    (void)mgr;
    for (unsigned i = 0; i < g_nsensors; i++)
        if (g_sensors[i]->type == type) return g_sensors[i];
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

static int kl_ASensorEventQueue_enableSensor(void *qp, const void *s) {
    kl_sensor_queue *q = qp;
    if (!q || s != &kl_sensor_gyro) return -EINVAL;
    if (!q->started) {
        if (pthread_create(&q->ticker, NULL, kl_sensor_ticker, q) != 0) return -EAGAIN;
        q->started = 1;
    }
    q->on = 1;
    return 0;
}
static int kl_ASensorEventQueue_disableSensor(void *qp, const void *s) {
    kl_sensor_queue *q = qp;
    if (!q || s != &kl_sensor_gyro) return -EINVAL;
    q->on = 0;
    return 0;
}
static int kl_ASensorEventQueue_setEventRate(void *qp, const void *s, int32_t us) {
    kl_sensor_queue *q = qp;
    if (!q || s != &kl_sensor_gyro) return -EINVAL;
    q->period_us = us < kl_sensor_gyro.min_delay_us ? kl_sensor_gyro.min_delay_us : us;
    return 0;
}
static int kl_ASensorEventQueue_hasEvents(void *qp) {
    kl_sensor_queue *q = qp;
    if (!q) return -EINVAL;
    struct pollfd pf = { q->pipe_r, POLLIN, 0 };
    return poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN) ? 1 : 0;
}

// ssize_t: 0 means "no events pending", which is not an error. At most one
// event per call: the pipe is drained and the sample taken now, and a tick with
// no measurement behind it produces nothing rather than a zero.
static ssize_t kl_ASensorEventQueue_getEvents(void *qp, void *events, size_t count) {
    kl_sensor_queue *q = qp;
    if (!q || !events) return -EINVAL;
    char buf[64];
    ssize_t n = read(q->pipe_r, buf, sizeof buf);
    if (n <= 0 || !q->on || count < 1) return 0;
    float w[3];
    if (!kl_gyro_sample(w)) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    kl_sensor_event *e = events;
    memset(e, 0, sizeof *e);
    e->version = (int32_t)sizeof *e; e->sensor = kl_sensor_gyro.handle;
    e->type = KL_ASENSOR_TYPE_GYROSCOPE;
    e->timestamp = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    e->data[0] = w[0]; e->data[1] = w[1]; e->data[2] = w[2];
    e->data[3] = 3;   // status byte of sensor_vec_t: ACCURACY_HIGH
    return 1;
}

// Everything an ASensor can be asked, for a list that is empty today: when a
// Quest sensor table is filled in, these already answer from it. A NULL sensor
// gets the NDK's own "unknown" values (Android would fault; -1/0 is kinder and
// can only be reached by a guest that ignored a NULL from getDefaultSensor).
static const char *kl_ASensor_getStringType(const kl_sensor *s) { return s && s->string_type ? s->string_type : ""; }
static int kl_ASensor_getFifoMaxEventCount(const kl_sensor *s)      { return s ? s->fifo_max : 0; }
static int kl_ASensor_getFifoReservedEventCount(const kl_sensor *s) { return s ? s->fifo_reserved : 0; }
static int kl_ASensor_getReportingMode(const kl_sensor *s)          { return s ? s->reporting_mode : -1; }
static int kl_ASensor_isWakeUpSensor(const kl_sensor *s)            { return s ? s->wake_up : 0; }
static int kl_ASensor_getHandle(const kl_sensor *s)                 { return s ? s->handle : -1; }
// Direct channels (ashmem/hardware-buffer sensor streams) need a sensor that
// advertises them; none does here.
static int kl_ASensor_isDirectChannelTypeSupported(const kl_sensor *s, int type) { (void)s; (void)type; return 0; }
static int kl_ASensor_getHighestDirectReportRateLevel(const kl_sensor *s) { (void)s; return 0; /* ASENSOR_DIRECT_RATE_STOP */ }
static const kl_sensor *kl_ASensorManager_getDefaultSensorEx(void *mgr, int type, int wakeUp) {
    (void)mgr;
    for (unsigned i = 0; i < g_nsensors; i++)
        if (g_sensors[i]->type == type && (!!g_sensors[i]->wake_up == !!wakeUp)) return g_sensors[i];
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
// registerSensor needs a sensor to register; with none, the answer is the same
// -EINVAL enableSensor gives. requestAdditionalInfoEvents on a live queue is
// accepted (no sensor will ever send one).
static int kl_ASensorEventQueue_registerSensor(void *q, const void *s, int32_t period_us, int64_t latency_us) {
    (void)q; (void)s; (void)period_us; (void)latency_us; return -EINVAL;
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
// native_app_glue (OpenXR CTS, every NativeActivity sample) imports the input
// queue family and calls it only from onInputQueueCreated — which nothing here
// ever sends: input reaches a VR guest through OpenXR/OVRPlugin, not through an
// Android InputQueue. So these are the answers for a queue that never has an
// event, and they exist so that a guest built against the glue binds by name
// instead of carrying unresolved imports. getEvent < 0 is "no event"; the event
// accessors can only be reached with an event this queue never handed out.
static void    kl_AInputQueue_attachLooper(void *q, void *looper, int ident, void *cb, void *data) {
    (void)q; (void)looper; (void)ident; (void)cb; (void)data;
}
static void    kl_AInputQueue_detachLooper(void *q) { (void)q; }
static int32_t kl_AInputQueue_getEvent(void *q, void **out) { (void)q; if (out) *out = NULL; return -1; }
static int32_t kl_AInputQueue_preDispatchEvent(void *q, void *ev) { (void)q; (void)ev; return 0; }
static void    kl_AInputQueue_finishEvent(void *q, void *ev, int handled) { (void)q; (void)ev; (void)handled; }
static int32_t kl_AInputEvent_getType(const void *ev) { (void)ev; return 0; }
static int32_t kl_AKeyEvent_getAction(const void *ev) { (void)ev; return 0; }
static int32_t kl_AKeyEvent_getKeyCode(const void *ev) { (void)ev; return 0; }
static float   kl_AMotionEvent_getRawX(const void *ev, size_t i) { (void)ev; (void)i; return 0.0f; }
static float   kl_AMotionEvent_getRawY(const void *ev, size_t i) { (void)ev; (void)i; return 0.0f; }

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
    N("AInputQueue_getEvent",          kl_AInputQueue_getEvent),
    N("AInputQueue_preDispatchEvent",  kl_AInputQueue_preDispatchEvent),
    N("AInputQueue_finishEvent",       kl_AInputQueue_finishEvent),
    N("AInputEvent_getType",           kl_AInputEvent_getType),
    N("AKeyEvent_getAction",           kl_AKeyEvent_getAction),
    N("AKeyEvent_getKeyCode",          kl_AKeyEvent_getKeyCode),
    N("AMotionEvent_getRawX",          kl_AMotionEvent_getRawX),
    N("AMotionEvent_getRawY",          kl_AMotionEvent_getRawY),

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
