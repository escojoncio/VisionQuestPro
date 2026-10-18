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
static vpgles_flush_fn  g_flush;
static void            *g_vk_gipa;
static int              g_state;  // 0 unknown, 1 up, -1 off/failed
static pthread_once_t   g_once = PTHREAD_ONCE_INIT;

static void kl_gles_bringup(void) {
    g_state = -1;
    if (!kl_env_on("KL_GLES_VPK", 0)) return;
    const char *path = kl_env_str("KL_MVK_DYLIB", NULL);
    void *h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) {
        fprintf(stderr, "  [gles] KL_GLES_VPK=1 but VPVulkan is not loaded (KL_MVK_DYLIB=%s): "
                        "GLES stays on the previous gateway\n", path ? path : "(unset)");
        return;
    }
    vpgles_init_fn init = (vpgles_init_fn)dlsym(h, "vpgles_init");
    g_proc   = (vpgles_proc_fn)dlsym(h, "vpgles_proc");
    g_device = (vpgles_device_fn)dlsym(h, "vpgles_device");
    g_wrap   = (vpgles_wrap_fn)dlsym(h, "vpgles_wrap_image");
    g_flush  = (vpgles_flush_fn)dlsym(h, "vpgles_flush");
    g_vk_gipa = dlsym(h, "vkGetInstanceProcAddr");
    if (!init || !g_proc || !g_device || !g_wrap || !g_flush || !g_vk_gipa) {
        fprintf(stderr, "  [gles] this VPVulkan has no vpgles entry points — GLES stays on the "
                        "previous gateway\n");
        return;
    }
    if (!init(g_vk_gipa)) {
        fprintf(stderr, "  [gles] vpgles_init failed (see gles.err) — GLES stays on the previous gateway\n");
        return;
    }
    g_state = 1;
    fprintf(stderr, "  [gles] guest GLES served by VPKEngine (vpgles on VPVulkan)\n");
}

int kl_gles_enabled(void) {
    pthread_once(&g_once, kl_gles_bringup);
    return g_state == 1;
}

void *kl_gles_sym(const char *name) {
    if (!name || !kl_gles_enabled()) return NULL;
    return g_proc(name);
}

void kl_gles_frame_end(void) {
    if (kl_gles_enabled()) g_flush(1);
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

uint32_t kl_gles_layer_texture(int is_eye, int eye, int stage, int w, int h, uint32_t glfmt, int layers) {
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
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (alloc((VkDevice)dev, &mai, NULL, &mem) != VK_SUCCESS) return 0;
    bind((VkDevice)dev, img, mem, 0);
    // The storage lives for the run (swapchain images are few and re-created only on resize).
    const uint32_t target = layers > 1 ? 0x8C1A /* TEXTURE_2D_ARRAY */ : 0x0DE1 /* TEXTURE_2D */;
    uint32_t name = g_wrap((void *)img, (uint32_t)fmt, target, (uint32_t)w, (uint32_t)h, (uint32_t)layers, 1);
    if (!name) return 0;
    if (is_eye) {
        void *mtl = mtl_of(img);
        // Array: both eyes are slices of this one image; Stereo: one image per eye, slice 0.
        // eye < 0 (DoubleWide): one wide image holds both eyes, slice 0; each eye is cropped by
        // its own submit viewport, so both (eye, stage) keys are registered.
        if (layers > 1) {
            kl_glfb_note_eye_mtl_texture(0, stage, mtl, 0, w, h);
            kl_glfb_note_eye_mtl_texture(1, stage, mtl, 1, w, h);
        } else if (eye < 0) {
            kl_glfb_note_eye_mtl_texture(0, stage, mtl, 0, w, h);
            kl_glfb_note_eye_mtl_texture(1, stage, mtl, 0, w, h);
        } else {
            kl_glfb_note_eye_mtl_texture(eye, stage, mtl, 0, w, h);
        }
        kl_glfb_note_eye_texture(eye < 0 ? 0 : eye, stage, name);
        if (eye < 0 || layers > 1) kl_glfb_note_eye_texture(1, stage, name);
    }
    fprintf(stderr, "  [gles] layer texture %s eye %d stage %d = GL %u (VkImage %p, %dx%d, %d layer%s, GL %#x)\n",
            is_eye ? "eye" : "quad", eye, stage, name, (void *)img, w, h, layers, layers > 1 ? "s" : "", glfmt);
    return name;
#else
    (void)is_eye; (void)eye; (void)stage; (void)w; (void)h; (void)glfmt; (void)layers;
    return 0;
#endif
}
