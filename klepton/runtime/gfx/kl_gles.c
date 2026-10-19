// The guest's GLES on VPKEngine (vpgles, inside VPVulkan): see kl_gles.h.
//
// vpgles lives in the same VPVulkan dylib Klepton already loads for Vulkan games (KL_MVK_DYLIB;
// the app dlopens it RTLD_GLOBAL first), so its entry points are found with dlsym on that path.
// vpgles brings up its own VkDevice on VPVulkan; the game's GL frames therefore go through
// VPVulkan's Metal backend, shader translation (GLSL ES -> SPIR-V -> VPIR -> MSL), pipeline cache,
// 1x MSAA + VPKTAA and foveation like a Vulkan game's.
//
// The present callback is not used for VR: the eye layers are VkImages published to the
// compositor through the same seam as the Vulkan path (kl_glfb_note_eye_mtl_texture).
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kl_env.h"
#include "kl_gles.h"
#include "kl_glfb.h"
#include "kl_vulkan.h"
#include "../diag/kl_wait.h"   // per-thread EGL context / in-GL-call marks for the [snap] dumps
#include <stdatomic.h>
#include <time.h>

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define KL_GLES_HAVE_VK 1
#endif
#endif

typedef int (*vpgles_init_fn)(void *);
typedef void *(*vpgles_proc_fn)(const char *);
typedef void (*vpgles_device_fn)(void **, void **, void **, void **);
typedef uint32_t (*vpgles_wrap_fn)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*vpgles_flush_fn)(int);

static vpgles_proc_fn   g_proc;
static vpgles_device_fn g_device;
static vpgles_wrap_fn   g_wrap;
static uint32_t (*g_wrap_as)(uint32_t, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static vpgles_flush_fn  g_flush;
static void            *g_vk_gipa;
static void            *g_vk_lib;
static int              g_state;  // 0 unknown, 1 up, -1 off/failed
static pthread_once_t   g_once = PTHREAD_ONCE_INIT;

// ---- diagnostics (no behaviour change) -------------------------------------
// Capped event ring of the EGL context-lifecycle calls plus any glFinish /
// glClientWaitSync / eglClientWaitSync that blocks > 100 ms. Written lock-free
// (a torn entry costs one garbled dump line); the first KL_GLES_LOG_N events
// (default 300) are also printed live, slow waits and failures always are.
#define GLES_RING 256
typedef struct {
    uint64_t t_ns, tid; const char *call;
    const void *ctx, *draw, *read; long res;
} gles_ev;
static gles_ev         g_ev[GLES_RING];
static _Atomic unsigned g_nev;
static uint64_t gles_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }

void kl_gles_egl_log(const char *call, const void *ctx, const void *draw, const void *read, long res) {
    unsigned n = atomic_fetch_add(&g_nev, 1);
    gles_ev *e = &g_ev[n % GLES_RING];
    e->t_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); e->tid = gles_tid();
    e->call = call; e->ctx = ctx; e->draw = draw; e->read = read; e->res = res;
    static unsigned cap = ~0u;
    if (cap == ~0u) cap = kl_env_uint("KL_GLES_LOG_N", 300u);
    if (n < cap)
        fprintf(stderr, "  [gles] t%llu %s ctx=%p draw=%p read=%p -> %#lx\n",
                (unsigned long long)e->tid, call, ctx, draw, read, res);
}

void kl_gles_egl_dump(FILE *out) {
    unsigned n = atomic_load(&g_nev);
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    fprintf(out, "  [snap] EGL/GL call log (last %u of %u):\n", n < 64 ? n : 64, n);
    for (unsigned k = n < 64 ? n : 64; k > 0; k--) {
        const gles_ev *e = &g_ev[(n - k) % GLES_RING];
        fprintf(out, "    -%6.1f s t%llu %-24s ctx=%p draw=%p read=%p -> %#lx\n",
                (double)(now - e->t_ns) / 1e9, (unsigned long long)e->tid, e->call ? e->call : "?",
                e->ctx, e->draw, e->read, e->res);
    }
}

static void gles_slow(const char *call, uint64_t t0) {
    uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0;
    if (dt < 100000000ull) return;
    fprintf(stderr, "  [gles] SLOW %s on t%llu took %llu ms\n", call,
            (unsigned long long)gles_tid(), (unsigned long long)(dt / 1000000ull));
    kl_gles_egl_log(call, NULL, NULL, NULL, (long)(dt / 1000000ull));
}

// Wrappers around the vpgles entry points; each forwards the arguments
// untouched and returns the real result. `kl_gl_enter/leave` mark the thread as
// being inside the call, which is what the thread snapshot prints.
static void *r_MakeCurrent, *r_CreateContext, *r_DestroyContext, *r_CreatePbuf,
            *r_DestroySurface, *r_GetError, *r_Finish, *r_CWaitSync, *r_ECWaitSync;
static __thread const char *t_last_egl;      // last wrapped EGL call, to name an eglGetError

static unsigned w_MakeCurrent(void *d, void *dr, void *rd, void *c) {
    kl_gl_enter("eglMakeCurrent");
    unsigned r = ((unsigned (*)(void *, void *, void *, void *))r_MakeCurrent)(d, dr, rd, c);
    kl_gl_leave();
    t_last_egl = c ? "eglMakeCurrent" : "eglMakeCurrent(NO_CONTEXT)";
    if (r) kl_wait_set_ctx(c);
    kl_gles_egl_log(t_last_egl, c, dr, rd, (long)r);
    return r;
}
static void *w_CreateContext(void *d, void *cfg, void *share, const int32_t *a) {
    kl_gl_enter("eglCreateContext");
    void *r = ((void *(*)(void *, void *, void *, const int32_t *))r_CreateContext)(d, cfg, share, a);
    kl_gl_leave();
    t_last_egl = "eglCreateContext";
    kl_gles_egl_log(t_last_egl, r, share, NULL, (long)(uintptr_t)r);
    return r;
}
static unsigned w_DestroyContext(void *d, void *c) {
    kl_gl_enter("eglDestroyContext");
    unsigned r = ((unsigned (*)(void *, void *))r_DestroyContext)(d, c);
    kl_gl_leave();
    t_last_egl = "eglDestroyContext";
    kl_gles_egl_log(t_last_egl, c, NULL, NULL, (long)r);
    return r;
}
static void *w_CreatePbuf(void *d, void *cfg, const int32_t *a) {
    kl_gl_enter("eglCreatePbufferSurface");
    void *r = ((void *(*)(void *, void *, const int32_t *))r_CreatePbuf)(d, cfg, a);
    kl_gl_leave();
    t_last_egl = "eglCreatePbufferSurface";
    kl_gles_egl_log(t_last_egl, NULL, r, NULL, (long)(uintptr_t)r);
    return r;
}
static unsigned w_DestroySurface(void *d, void *s) {
    kl_gl_enter("eglDestroySurface");
    unsigned r = ((unsigned (*)(void *, void *))r_DestroySurface)(d, s);
    kl_gl_leave();
    t_last_egl = "eglDestroySurface";
    kl_gles_egl_log(t_last_egl, NULL, s, NULL, (long)r);
    return r;
}
// The error the GUEST reads is the one logged; the wrapper never calls the real
// eglGetError on its own, because that would consume the error.
static int w_GetError(void) {
    int e = ((int (*)(void))r_GetError)();
    if (e != 0x3000) {
        kl_gles_egl_log("eglGetError", NULL, NULL, NULL, e);
        fprintf(stderr, "  [gles] t%llu eglGetError -> %#x after %s\n",
                (unsigned long long)gles_tid(), e, t_last_egl ? t_last_egl : "?");
    }
    return e;
}
static void w_Finish(void) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    kl_gl_enter("glFinish");
    ((void (*)(void))r_Finish)();
    kl_gl_leave();
    gles_slow("glFinish", t0);
}
static uint32_t w_ClientWaitSync(void *sync, uint32_t flags, uint64_t timeout) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    kl_gl_enter("glClientWaitSync");
    uint32_t r = ((uint32_t (*)(void *, uint32_t, uint64_t))r_CWaitSync)(sync, flags, timeout);
    kl_gl_leave();
    gles_slow("glClientWaitSync", t0);
    return r;
}
static int32_t w_EglClientWaitSync(void *d, void *sync, int32_t flags, uint64_t timeout) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    kl_gl_enter("eglClientWaitSync");
    int32_t r = ((int32_t (*)(void *, void *, int32_t, uint64_t))r_ECWaitSync)(d, sync, flags, timeout);
    kl_gl_leave();
    gles_slow("eglClientWaitSync", t0);
    return r;
}

// Returns the wrapper for the entry points above, else `p` unchanged.
static void *gles_diag_wrap(const char *name, void *p) {
#define W(n, real, wrap) if (!strcmp(name, n)) { if (!real) real = p; return real == p ? (void *)wrap : p; }
    W("eglMakeCurrent", r_MakeCurrent, w_MakeCurrent)
    W("eglCreateContext", r_CreateContext, w_CreateContext)
    W("eglDestroyContext", r_DestroyContext, w_DestroyContext)
    W("eglCreatePbufferSurface", r_CreatePbuf, w_CreatePbuf)
    W("eglDestroySurface", r_DestroySurface, w_DestroySurface)
    W("eglGetError", r_GetError, w_GetError)
    W("glFinish", r_Finish, w_Finish)
    W("glClientWaitSync", r_CWaitSync, w_ClientWaitSync)
    W("eglClientWaitSync", r_ECWaitSync, w_EglClientWaitSync)
#undef W
    return p;
}

// The route line the log was missing: which GLES stack serves the guest, and why.
static void gles_route(const char *route, const char *why) {
    fprintf(stderr, "[gles] ruta: %s — %s\n", route, why);
}

static void kl_gles_bringup(void) {
    g_state = -1;
    if (!kl_env_on("KL_GLES_VPK", 0)) {
        gles_route(kl_glfb_enabled() ? "ANGLE" : "otra (gateway nulo de kl_egl.c, sin GL real)",
                   kl_glfb_enabled() ? "KL_GLES_VPK=0 (OpenGL ES con VPKEngine desactivado), KL_GLFB activo"
                                     : "KL_GLES_VPK=0 y KL_GLFB inactivo");
        return;
    }
    const char *path = kl_env_str("KL_MVK_DYLIB", NULL);
    void *h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) {
        fprintf(stderr, "  [gles] KL_GLES_VPK=1 but VPVulkan is not loaded (KL_MVK_DYLIB=%s): "
                        "GLES stays on the previous gateway\n", path ? path : "(unset)");
        gles_route(kl_glfb_enabled() ? "ANGLE" : "otra (gateway nulo de kl_egl.c)",
                   "KL_GLES_VPK=1 pero VPVulkan no carga (fallback)");
        return;
    }
    vpgles_init_fn init = (vpgles_init_fn)dlsym(h, "vpgles_init");
    g_proc   = (vpgles_proc_fn)dlsym(h, "vpgles_proc");
    g_device = (vpgles_device_fn)dlsym(h, "vpgles_device");
    g_wrap   = (vpgles_wrap_fn)dlsym(h, "vpgles_wrap_image");
    g_wrap_as = (uint32_t (*)(uint32_t, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))dlsym(h, "vpgles_wrap_image_as");
    g_flush  = (vpgles_flush_fn)dlsym(h, "vpgles_flush");
    g_vk_gipa = dlsym(h, "vkGetInstanceProcAddr");
    if (!init || !g_proc || !g_device || !g_wrap || !g_flush || !g_vk_gipa) {
        fprintf(stderr, "  [gles] this VPVulkan has no vpgles entry points — GLES stays on the "
                        "previous gateway\n");
        gles_route(kl_glfb_enabled() ? "ANGLE" : "otra (gateway nulo de kl_egl.c)",
                   "KL_GLES_VPK=1 pero a este VPVulkan le faltan los puntos de entrada vpgles (fallback)");
        return;
    }
    if (!init(g_vk_gipa)) {
        fprintf(stderr, "  [gles] vpgles_init failed (see gles.err) — GLES stays on the previous gateway\n");
        gles_route(kl_glfb_enabled() ? "ANGLE" : "otra (gateway nulo de kl_egl.c)",
                   "KL_GLES_VPK=1 pero vpgles_init fallo (fallback)");
        return;
    }
    g_vk_lib = h;
    __atomic_store_n(&g_state, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "  [gles] guest GLES served by VPKEngine (vpgles on VPVulkan)\n");
    gles_route("VPKEngine (vpgles)", "KL_GLES_VPK=1 y vpgles_init correcto; las entradas gl*/egl* vienen de vpgles");
}

int kl_gles_enabled(void) {
    pthread_once(&g_once, kl_gles_bringup);
    return g_state == 1;
}

int kl_gles_vpk_up(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == 1; }

void *kl_gles_vk_handle(void) { return kl_gles_vpk_up() ? g_vk_lib : NULL; }

// Every name vpgles does not resolve is logged (once per name) and remembered: the caller falls back to
// its own answer (kl_egl.c: legacy impl or a stub that logs and returns 0, never aborts), and the list
// below is the exact work list of GL/EGL functions vpgles lacks. Written at the end of the session
// (kl_gles_report_unresolved, called from kl_egl_report) so no NULL goes unrecorded.
#define GLES_MISS_MAX 1024
static char *g_miss[GLES_MISS_MAX];
static _Atomic unsigned g_nmiss;
static pthread_mutex_t g_miss_mu = PTHREAD_MUTEX_INITIALIZER;

static void gles_miss_atexit(void);
static void gles_note_unresolved(const char *name) {
    pthread_mutex_lock(&g_miss_mu);
    unsigned n = atomic_load(&g_nmiss);
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(g_miss[i], name)) { pthread_mutex_unlock(&g_miss_mu); return; }
    if (n < GLES_MISS_MAX) {
        g_miss[n] = strdup(name);
        atomic_store(&g_nmiss, n + 1);
    }
    static int hooked;
    if (!hooked) { hooked = 1; atexit(gles_miss_atexit); }
    pthread_mutex_unlock(&g_miss_mu);
    fprintf(stderr, "  [gles] vpgles does not resolve '%s' (NULL: the gateway answers with its own or a logging stub)\n", name);
}

// Also reached from the fault handler (signal context, via kl_egl_report): never blocks. trylock,
// so a fault taken while a thread holds g_miss_mu skips the list instead of deadlocking. The list
// prints once per size (g_miss_reported), so the crash report and the atexit hooks do not repeat it.
static _Atomic unsigned g_miss_reported;
void kl_gles_report_unresolved(FILE *out) {
    if (pthread_mutex_trylock(&g_miss_mu) != 0) return;
    unsigned n = atomic_load(&g_nmiss);
    if (n && atomic_exchange(&g_miss_reported, n) != n) {
        fprintf(out, "\n[gles] names asked of vpgles that it does not resolve: %u\n", n);
        for (unsigned i = 0; i < n; i++) fprintf(out, "    %s\n", g_miss[i]);
    }
    pthread_mutex_unlock(&g_miss_mu);
}

static void gles_miss_atexit(void) { kl_gles_report_unresolved(stderr); }

void *kl_gles_sym(const char *name) {
    if (!name || !kl_gles_enabled()) return NULL;
    void *p = g_proc(name);
    if (!p) {
        gles_note_unresolved(name);
        return NULL;
    }
    return gles_diag_wrap(name, p);
}

void kl_gles_frame_end(void) {
    // Vulkan titles never touch vpgles: no extra instance/device for them.
    if (!kl_vulkan_guest_active() && kl_gles_enabled()) g_flush(1);
}

#ifdef KL_GLES_HAVE_VK
// GL internalformat -> VkFormat for the layer formats ovrpTextureFormat maps to (klovrp_gl_format).
static VkFormat vk_format_of(uint32_t gl) {
    switch (gl) {
        case 0x8C43: return VK_FORMAT_R8G8B8A8_SRGB;        // SRGB8_ALPHA8
        case 0x8058: return VK_FORMAT_R8G8B8A8_UNORM;       // RGBA8
        case 0x881A: return VK_FORMAT_R16G16B16A16_SFLOAT;  // RGBA16F
        case 0x93A1: return VK_FORMAT_B8G8R8A8_UNORM;       // BGRA8_EXT
        case 0x8C3A: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x81A5: return VK_FORMAT_D16_UNORM;
        case 0x88F0: return VK_FORMAT_D24_UNORM_S8_UINT;
        case 0x8CAC: return VK_FORMAT_D32_SFLOAT;
        default:     return VK_FORMAT_R8G8B8A8_SRGB;
    }
}

// The MTLTexture behind a VkImage of VPVulkan (MoltenVK-compatible export).
static void *mtl_of(VkImage img) {
    static void (*get)(VkImage, void **);
    if (!get) {
        const char *path = kl_env_str("KL_MVK_DYLIB", NULL);
        void *h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
        if (h) get = (void (*)(VkImage, void **))dlsym(h, "vkGetMTLTextureMVK");
    }
    void *t = NULL;
    if (get) get(img, &t);
    return t;
}
#endif

#define KL_GLES_MTL_MAX 512
static uint32_t g_mtl_name[KL_GLES_MTL_MAX];
static void *g_mtl_tex[KL_GLES_MTL_MAX];
void *kl_gles_texture_mtl(uint32_t name) {
    for (int i = 0; i < KL_GLES_MTL_MAX && g_mtl_name[i]; i++)
        if (g_mtl_name[i] == name) return g_mtl_tex[i];
    return NULL;
}

static uint32_t layer_texture_impl(uint32_t as_name, int is_eye, int eye, int stage, int w, int h, uint32_t glfmt, int layers) {
#ifdef KL_GLES_HAVE_VK
    if (!kl_gles_enabled() || w <= 0 || h <= 0) return 0;
    void *inst = NULL, *phys = NULL, *dev = NULL, *queue = NULL;
    g_device(&inst, &phys, &dev, &queue);
    if (!dev) return 0;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)g_vk_gipa;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa((VkInstance)inst, "vkGetDeviceProcAddr");
    PFN_vkCreateImage create = (PFN_vkCreateImage)gdpa((VkDevice)dev, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements req = (PFN_vkGetImageMemoryRequirements)gdpa((VkDevice)dev, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)gdpa((VkDevice)dev, "vkAllocateMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)gdpa((VkDevice)dev, "vkBindImageMemory");
    if (!create || !req || !alloc || !bind) return 0;
    const VkFormat fmt = vk_format_of(glfmt);
    const int depth = fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT;
    if (layers < 1) layers = 1;
    VkImageCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = (uint32_t)w, ci.extent.height = (uint32_t)h, ci.extent.depth = 1;
    ci.mipLevels = 1, ci.arrayLayers = (uint32_t)layers, ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage img = VK_NULL_HANDLE;
    if (create((VkDevice)dev, &ci, NULL, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    req((VkDevice)dev, img, &mr);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = (mr.memoryTypeBits & 1u) || !mr.memoryTypeBits ? 0u : (uint32_t)__builtin_ctz(mr.memoryTypeBits);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (alloc((VkDevice)dev, &mai, NULL, &mem) != VK_SUCCESS) return 0;
    bind((VkDevice)dev, img, mem, 0);
    // The storage lives for the run (swapchain images are few and re-created only on resize).
    const uint32_t target = layers > 1 ? 0x8C1A /* TEXTURE_2D_ARRAY */ : 0x0DE1 /* TEXTURE_2D */;
    uint32_t name = as_name ? (g_wrap_as ? g_wrap_as(as_name, (void *)img, (uint32_t)fmt, target, (uint32_t)w, (uint32_t)h,
                                                     (uint32_t)layers, 1) : 0)
                            : g_wrap((void *)img, (uint32_t)fmt, target, (uint32_t)w, (uint32_t)h, (uint32_t)layers, 1);
    if (!name) return 0;
    void *mtl = mtl_of(img);
    if (mtl) {
        static int n_mtl;
        if (n_mtl < KL_GLES_MTL_MAX) { g_mtl_name[n_mtl] = name; g_mtl_tex[n_mtl] = mtl; n_mtl++; }
    }
    if (is_eye) {
        // Array: both eyes are slices of this one image; Stereo: one image per eye, slice 0.
        // eye < 0 (DoubleWide): one wide image holds both eyes, slice 0; each eye is cropped by
        // its own submit viewport, so both (eye, stage) keys are registered.
        if (layers > 1) {
            kl_glfb_note_eye_mtl_texture_o(0, stage, mtl, 0, w, h, 0);
            kl_glfb_note_eye_mtl_texture_o(1, stage, mtl, 1, w, h, 0);
        } else if (eye < 0) {
            kl_glfb_note_eye_mtl_texture_o(0, stage, mtl, 0, w, h, 0);
            kl_glfb_note_eye_mtl_texture_o(1, stage, mtl, 0, w, h, 0);
        } else {
            kl_glfb_note_eye_mtl_texture_o(eye, stage, mtl, 0, w, h, 0);
        }
        kl_glfb_note_eye_texture(eye < 0 ? 0 : eye, stage, name);
        if (eye < 0 || layers > 1) kl_glfb_note_eye_texture(1, stage, name);
    }
    fprintf(stderr, "  [gles] layer texture %s eye %d stage %d = GL %u (VkImage %p, %dx%d, %d layer%s, GL %#x)\n",
            is_eye ? "eye" : "quad", eye, stage, name, (void *)img, w, h, layers, layers > 1 ? "s" : "", glfmt);
    return name;
#else
    (void)as_name; (void)is_eye; (void)eye; (void)stage; (void)w; (void)h; (void)glfmt; (void)layers;
    return 0;
#endif
}

uint32_t kl_gles_layer_texture(int is_eye, int eye, int stage, int w, int h, uint32_t glfmt, int layers) {
    return layer_texture_impl(0, is_eye, eye, stage, w, h, glfmt, layers);
}

uint32_t kl_gles_legacy_eye_texture(uint32_t name, int eye, int stage, int w, int h, uint32_t glfmt, int layers) {
    uint32_t r = layer_texture_impl(name, 1, eye, stage, w, h, glfmt, layers);
    fprintf(stderr, "  [gles] legacy eye texture %u (eye %d stage %d %dx%d, %d layer%s) wrapped: %s\n", name, eye, stage, w, h,
            layers, layers > 1 ? "s" : "", r ? "ok" : "FAILED");
    return r;
}
