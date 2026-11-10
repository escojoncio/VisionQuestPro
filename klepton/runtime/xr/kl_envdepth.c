// kl_envdepth.c - environment depth state shared by the ovrp and OpenXR facades
// (see kl_envdepth.h for the contract and the image convention).
//
// Who does what:
//   guest thread   create / start / alloc / acquire   (the facades)
//   compositor     job_begin -> rasterise -> job_done  (KleptonEnvDepth.swift)
// One mutex guards the ring; the image set is only ever replaced by alloc/free,
// and a job that outlives its set is dropped by its generation.
//
// The ring: KLED_IMAGES slots, each FREE / RENDER / READY / HELD. The guest may
// still be sampling the image it was handed one acquire ago, so TWO acquired
// images (held, prev) are never rewritten; the newest finished one waits as
// READY; one more is the render target. Acquire with nothing newer re-serves the
// held frame rather than saying "not available".
//
// Vulkan guest: the images are VkImages from kl_vulkan_xr_image; the compositor
// draws straight into the MTLTexture MoltenVK backs them with (zero copy).
// GL guest: Metal cannot share a texture with a GL name that already has its
// own storage, so the compositor reads the 2 layers back into cpu[] and the
// acquire call (on the guest's GL thread) uploads them.
#include "kl_envdepth.h"
#include "kl_spatial.h"
#include "kl_ovrp.h"       // kl_ovrp_eye_view, kl_ovrp_guest_origin_y
#include "kl_vulkan.h"
#include "kl_egl.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VK_FORMAT_D16 124                 // VK_FORMAT_D16_UNORM
#define GL_TEXTURE_2D_ARRAY_    0x8C1A
#define GL_TEXTURE_BINDING_2DA  0x8C1D
#define GL_DEPTH_COMPONENT_     0x1902
#define GL_DEPTH_COMPONENT16_   0x81A5
#define GL_UNSIGNED_SHORT_      0x1403
#define GL_PIXEL_UNPACK_BUFFER_ 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING_ 0x88EF
#define GL_TEXTURE_MIN_FILTER_  0x2801
#define GL_TEXTURE_MAG_FILTER_  0x2800
#define GL_TEXTURE_WRAP_S_      0x2802
#define GL_TEXTURE_WRAP_T_      0x2803
#define GL_NEAREST_             0x2600
#define GL_CLAMP_TO_EDGE_       0x812F

#define ONCE(msg) do { static int _o; if (!_o) { _o = 1; fprintf(stderr, "  [envdepth] %s\n", (msg)); } } while (0)

enum { S_FREE, S_RENDER, S_READY, S_HELD, S_LOST };   // S_LOST: its job timed out, the GPU may still write it

// The depth convention of the images (kl_envdepth.h): FORWARD-Z in OpenGL's window range, which is what the guest's own code
// decodes. Read out of Harem Life VR's libil2cpp (Meta.XR.EnvironmentDepth v71): EnvironmentDepthUtils.CalculateDepthCameraMatrices
// builds a GL projection from the frame's tangents and nearZ/farZ (m22 = -(f+n)/(f-n), m23 = -2fn/(f-n)) and
// ComputeNdcToLinearDepthParameters gives the shader x = -2fn/(f-n), y = -(f+n)/(f-n), linear = x / ((2d - 1) + y). So
// d = (ndc.z + 1) / 2 = f (z - n) / ((f - n) z): 0 at the near plane, 1 at the far plane or where there is no geometry. The
// reversed-Z form this replaces decoded every surface as ~near (0.1 m): the world "touched the camera" and hid every model.
// d(z) for a view-axis distance z, and the projection terms that give it in Metal's [0,1] clip range (clip.z = a * eyeZ + b,
// clip.w = -eyeZ, eyeZ = -z); KleptonEnvDepth.swift builds its matrix from the same two numbers. KL-TEST envdepthz.
// KL-TEST-BEGIN envdepthz
static float kled_depth_fwd(float z, float n, float f) {
    if (!(z > 0.0f) || !(f > n) || !(n > 0.0f)) return 1.0f;      // no geometry: as far as it gets, occludes nothing
    if (z <= n) return 0.0f;
    if (z >= f) return 1.0f;
    return f * (z - n) / ((f - n) * z);
}
static void kled_proj_ab(float n, float f, float *a, float *b) { *a = -f / (f - n); *b = -f * n / (f - n); }
// KL-TEST-END envdepthz

// KL-TEST-BEGIN envdepthjob (extracted by tools/ci/host-tests.sh into klepton/tests/t_envdepth_job.c)
// A job that has not finished this long after it began is given up (a command buffer that never completes).
#define KLED_JOB_TIMEOUT_S 2.0
// Has the job in flight (if any) been running past the timeout? Clocks are CLOCK_MONOTONIC seconds.
static int kled_job_expired(int rendering, double began_s, double at_s, double timeout_s) {
    return rendering && at_s - began_s >= timeout_s;
}
// Is a finishing job the one in flight? A late finish of a job that timed out (and maybe was replaced) is not,
// so it must not clear the flag of the newer job. seq is bumped on every job_begin.
static int kled_job_is_current(int rendering, uint32_t job_seq, uint32_t cur_seq) {
    return rendering && job_seq == cur_seq;
}
// What job_done does with a finishing job: it is the one in flight (CURRENT), or it is a late finish of a job given
// up whose slot is still S_LOST (FREE_LOST: the slot is free again), or it is a late finish whose slot is no longer
// S_LOST (IGNORE). Only CURRENT may touch rendering and the ready/held bookkeeping of the newer job.
enum { KLED_DONE_CURRENT, KLED_DONE_FREE_LOST, KLED_DONE_IGNORE };
static int kled_done_action(int rendering, uint32_t job_seq, uint32_t cur_seq, int slot_lost) {
    if (kled_job_is_current(rendering, job_seq, cur_seq)) return KLED_DONE_CURRENT;
    return slot_lost ? KLED_DONE_FREE_LOST : KLED_DONE_IGNORE;
}
// Is environment depth supported? While ARKit's answer is pending (known = 0) the game may already enumerate, so a game
// that wants the scene is told yes; once it is in, only a running scene-mesh provider says yes (world sensing denied
// or no SceneReconstructionProvider = no).
static int kled_supported(int mesh_available, int wants_scene, int known) {
    return mesh_available || (wants_scene && !known);
}
// KL-TEST-END envdepthjob
enum { API_NONE, API_VULKAN, API_GL };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int created, started, hand_removal;
    int api;                              // API_* of the allocated set
    uint32_t gen;                         // bumps on every free
    uint64_t img[KLED_IMAGES];            // VkImage / GL name
    void    *mtl[KLED_IMAGES];            // Vulkan: borrowed MTLTexture
    uint16_t *cpu[KLED_IMAGES];           // GL: 2 layers of w*h, row 0 = top (kept for the process)
    int state[KLED_IMAGES];
    kl_envdepth_frame fr[KLED_IMAGES];
    int ready, held, prev, rendering;
    uint32_t job_seq;                     // seq of the job in flight (bumps on every job_begin)
    int job_slot;                         // its slot
    double job_t;                         // CLOCK_MONOTONIC seconds when it began
    int inflight;                         // jobs handed to the compositor and not yet done (stale ones too)
} g = { .ready = -1, .held = -1, .prev = -1 };
static int g_lost_last;                   // S_LOST count at the last lost_note log
static double g_lost_t = -1e9;            // when it was logged

// Slots that stay S_LOST are not reusable while the GPU may write them; with held and prev that can leave no free
// slot and freeze the depth. Log (not ONCE) when 2+ are lost or job_begin is starved: at most every 5 s, always when
// the count changes. Called with g_mu held.
static void lost_note(double t, int starved) {
    int n = 0;
    for (int i = 0; i < KLED_IMAGES; i++) if (g.state[i] == S_LOST) n++;
    if (n < 2 && !starved) { g_lost_last = n; return; }
    if (n == g_lost_last && t - g_lost_t < 5.0) return;
    g_lost_last = n;
    g_lost_t = t;
    fprintf(stderr, "  [envdepth] %d of %d depth slots lost to hung jobs%s\n", n, KLED_IMAGES,
            starved ? ", no free slot: depth not refreshed" : "");
}

// ---- GL gateway (only for a GL guest) ---------------------------------------
static void (*gl_GenTextures)(int32_t, uint32_t *);
static void (*gl_DeleteTextures)(int32_t, const uint32_t *);
static void (*gl_BindTexture)(uint32_t, uint32_t);
static void (*gl_BindBuffer)(uint32_t, uint32_t);
static void (*gl_TexStorage3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);
static void (*gl_TexSubImage3D)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                                uint32_t, uint32_t, const void *);
static void (*gl_TexParameteri)(uint32_t, uint32_t, int32_t);
static void (*gl_GetIntegerv)(uint32_t, int32_t *);
static int gl_resolve(void) {
    if (gl_GenTextures) return 1;
    gl_GenTextures    = kl_egl_sym("glGenTextures");
    gl_DeleteTextures = kl_egl_sym("glDeleteTextures");
    gl_BindTexture    = kl_egl_sym("glBindTexture");
    gl_BindBuffer     = kl_egl_sym("glBindBuffer");
    gl_TexStorage3D   = kl_egl_sym("glTexStorage3D");
    gl_TexSubImage3D  = kl_egl_sym("glTexSubImage3D");
    gl_TexParameteri  = kl_egl_sym("glTexParameteri");
    gl_GetIntegerv    = kl_egl_sym("glGetIntegerv");
    return gl_GenTextures && gl_BindTexture && gl_TexStorage3D && gl_TexSubImage3D && gl_GetIntegerv;
}

// ---- pose math (px py pz qx qy qz qw) ---------------------------------------
static void qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
// o = a * b (rigid transforms; o may alias neither).
static void pmul(const float a[7], const float b[7], float o[7]) {
    float r[3];
    qrot(a + 3, b, r);
    o[0] = a[0] + r[0]; o[1] = a[1] + r[1]; o[2] = a[2] + r[2];
    qmul(a + 3, b + 3, o + 3);
}
static void pinv(const float a[7], float o[7]) {
    float qi[4] = { -a[3], -a[4], -a[5], a[6] }, np[3] = { -a[0], -a[1], -a[2] }, r[3];
    qrot(qi, np, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
    memcpy(o + 3, qi, sizeof qi);
}

// One eye as the guest's tracking space has it: the same view xrLocateViews and
// ovrp_GetNodePoseState are served from (kl_ovrp_eye_view), minus the guest's
// origin height - the transform kl_spatial applies to the mesh.
static void eye_view(int e, float pose[7], float tan[4]) {
    kl_ovrp_eye_view(e, &pose[0], &pose[1], &pose[2], &pose[3], &pose[4], &pose[5], &pose[6], tan);
    pose[1] -= kl_ovrp_guest_origin_y();
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ---- support / lifecycle -------------------------------------------------------
// Announced by policy too while ARKit's answer is pending (kled_supported).
int kl_envdepth_supported(void) {
    int ws = 0;
    kl_spatial_policy(&ws, NULL);
    return kled_supported(kl_spatial_available(KLSP_CAP_MESH), ws, kl_spatial_available_known());
}

int kl_envdepth_create(int hand_removal) {
    if (!kl_envdepth_supported()) return -1;
    pthread_mutex_lock(&g_mu);
    g.created = 1;
    g.hand_removal = hand_removal != 0;
    pthread_mutex_unlock(&g_mu);
    ONCE("provider created (scene mesh -> depth)");
    return 0;
}

int kl_envdepth_created(void) {
    pthread_mutex_lock(&g_mu);
    int c = g.created;
    pthread_mutex_unlock(&g_mu);
    return c;
}

void kl_envdepth_set_hand_removal(int on) {
    pthread_mutex_lock(&g_mu);
    g.hand_removal = on != 0;
    pthread_mutex_unlock(&g_mu);
}

int kl_envdepth_start(void) {
    pthread_mutex_lock(&g_mu);
    int ok = g.created;
    if (ok) g.started = 1;
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}

void kl_envdepth_stop(void) {
    pthread_mutex_lock(&g_mu);
    g.started = 0;
    pthread_mutex_unlock(&g_mu);
}

void kl_envdepth_destroy(void) {
    kl_envdepth_stop();
    kl_envdepth_free();
    pthread_mutex_lock(&g_mu);
    g.created = 0;
    pthread_mutex_unlock(&g_mu);
}

// ---- the images ------------------------------------------------------------------
int kl_envdepth_is_vulkan(void) {
    pthread_mutex_lock(&g_mu);
    int v = g.api == API_VULKAN;
    pthread_mutex_unlock(&g_mu);
    return v;
}

int kl_envdepth_alloc_state(void) {
    pthread_mutex_lock(&g_mu);
    int a = g.api != API_NONE;
    pthread_mutex_unlock(&g_mu);
    return a;
}

uint64_t kl_envdepth_image(int i) {
    if ((unsigned)i >= KLED_IMAGES) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.img[i];
    pthread_mutex_unlock(&g_mu);
    return v;
}

int kl_envdepth_alloc(void) {
    pthread_mutex_lock(&g_mu);
    if (g.api != API_NONE) { pthread_mutex_unlock(&g_mu); return KLED_IMAGES; }
    pthread_mutex_unlock(&g_mu);

    uint64_t img[KLED_IMAGES] = {0};
    void *mtl[KLED_IMAGES] = {0};
    int api = API_NONE;
    if (kl_vulkan_guest_active()) {
        api = API_VULKAN;
        for (int i = 0; i < KLED_IMAGES; i++) {
            img[i] = kl_vulkan_xr_image(KLED_W, KLED_H, 2, 1, VK_FORMAT_D16, 1);
            mtl[i] = img[i] ? kl_vulkan_xr_image_mtl(img[i]) : NULL;
            if (!img[i] || !mtl[i]) {
                fprintf(stderr, "  [envdepth] Vulkan depth image %d failed (image %llx, MTLTexture %p)\n",
                        i, (unsigned long long)img[i], mtl[i]);
                for (int j = 0; j <= i; j++) if (img[j]) kl_vulkan_xr_image_free(img[j]);
                return 0;
            }
        }
    } else if (kl_egl_current_context() && gl_resolve()) {
        api = API_GL;
        uint32_t names[KLED_IMAGES] = {0};
        gl_GenTextures(KLED_IMAGES, names);
        int32_t bound = 0;
        gl_GetIntegerv(GL_TEXTURE_BINDING_2DA, &bound);
        for (int i = 0; i < KLED_IMAGES; i++) {
            gl_BindTexture(GL_TEXTURE_2D_ARRAY_, names[i]);
            gl_TexStorage3D(GL_TEXTURE_2D_ARRAY_, 1, GL_DEPTH_COMPONENT16_, KLED_W, KLED_H, 2);
            if (gl_TexParameteri) {
                gl_TexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
                gl_TexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
                gl_TexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
                gl_TexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
            }
            img[i] = names[i];
        }
        gl_BindTexture(GL_TEXTURE_2D_ARRAY_, (uint32_t)bound);
    } else {
        ONCE("no Vulkan device and no current GL context: depth images not allocated yet");
        return 0;
    }

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < KLED_IMAGES; i++) {
        g.img[i] = img[i];
        g.mtl[i] = mtl[i];
        g.state[i] = S_FREE;
        if (api == API_GL && !g.cpu[i])
            g.cpu[i] = calloc((size_t)KLED_W * KLED_H * 2, sizeof(uint16_t));
    }
    g.ready = g.held = g.prev = -1;
    g.rendering = 0;
    g_lost_last = 0;
    g.api = api;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [envdepth] %d depth images %dx%dx2 D16 (%s)\n", KLED_IMAGES, KLED_W, KLED_H,
            api == API_VULKAN ? "Vulkan, zero copy" : "GL, uploaded per acquire");
    return KLED_IMAGES;
}

void kl_envdepth_free(void) {
    pthread_mutex_lock(&g_mu);
    int api = g.api;
    uint64_t img[KLED_IMAGES];
    memcpy(img, g.img, sizeof img);
    g.api = API_NONE;
    g.gen++;
    memset(g.img, 0, sizeof g.img);
    memset(g.mtl, 0, sizeof g.mtl);
    g.ready = g.held = g.prev = -1;
    g.rendering = 0;
    pthread_mutex_unlock(&g_mu);
    // A job the compositor already holds draws into these textures: let it finish
    // (bounded) before they are released.
    for (int w = 0; w < 500; w++) {
        pthread_mutex_lock(&g_mu);
        int busy = g.inflight > 0;
        pthread_mutex_unlock(&g_mu);
        if (!busy) break;
        usleep(1000);
    }
    if (api == API_VULKAN) {
        for (int i = 0; i < KLED_IMAGES; i++) if (img[i]) kl_vulkan_xr_image_free(img[i]);
    } else if (api == API_GL && gl_DeleteTextures && kl_egl_current_context()) {
        uint32_t names[KLED_IMAGES];
        for (int i = 0; i < KLED_IMAGES; i++) names[i] = (uint32_t)img[i];
        gl_DeleteTextures(KLED_IMAGES, names);
    }
}

// ---- acquire -----------------------------------------------------------------------
// GL: the finished picture goes into the guest's texture, rows flipped (GL row 0
// is the bottom of the view). The guest's own bindings are put back.
static void gl_upload(int slot, uint64_t name) {
    static uint16_t flip[KLED_W * KLED_H];
    if (!kl_egl_current_context() || !gl_resolve() || !g.cpu[slot]) return;
    int32_t bound = 0, pbo = 0;
    gl_GetIntegerv(GL_TEXTURE_BINDING_2DA, &bound);
    gl_GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_, &pbo);
    if (pbo && gl_BindBuffer) gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER_, 0);
    gl_BindTexture(GL_TEXTURE_2D_ARRAY_, (uint32_t)name);
    for (int layer = 0; layer < 2; layer++) {
        const uint16_t *src = g.cpu[slot] + (size_t)layer * KLED_W * KLED_H;
        for (int y = 0; y < KLED_H; y++)
            memcpy(flip + (size_t)y * KLED_W, src + (size_t)(KLED_H - 1 - y) * KLED_W, KLED_W * sizeof(uint16_t));
        gl_TexSubImage3D(GL_TEXTURE_2D_ARRAY_, 0, 0, 0, layer, KLED_W, KLED_H, 1,
                         GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, flip);
    }
    gl_BindTexture(GL_TEXTURE_2D_ARRAY_, (uint32_t)bound);
    if (pbo && gl_BindBuffer) gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER_, (uint32_t)pbo);
}

int kl_envdepth_acquire(kl_envdepth_frame *f) {
    pthread_mutex_lock(&g_mu);
    if (!g.started) { pthread_mutex_unlock(&g_mu); return -1; }
    int s = g.ready, fresh = s >= 0;
    if (!fresh) s = g.held;                                   // nothing newer: serve the same frame again
    if (s < 0 || g.api == API_NONE) { pthread_mutex_unlock(&g_mu); return 0; }
    if (fresh) {
        if (g.prev >= 0) g.state[g.prev] = S_FREE;
        g.prev = g.held;
        g.held = s;
        g.ready = -1;
        g.state[s] = S_HELD;
    }
    if (f) *f = g.fr[s];
    int api = g.api;
    uint64_t name = g.img[s];
    pthread_mutex_unlock(&g_mu);
    if (fresh && api == API_GL) gl_upload(s, name);
    return 1;
}

// ---- rebasing for another space ---------------------------------------------------------
void kl_envdepth_rebase(kl_envdepth_frame *f, const float eye0_rel[7]) {
    if (!f || !eye0_rel) return;
    float now[7], tan[4], inv[7], m[7];
    eye_view(0, now, tan);
    pinv(now, inv);
    pmul(eye0_rel, inv, m);          // tracking space -> the asked space
    for (int e = 0; e < 2; e++) {
        float o[7];
        pmul(m, f->pose[e], o);
        memcpy(f->pose[e], o, sizeof o);
    }
}

// ---- compositor side -----------------------------------------------------------------------
int kl_envdepth_job_begin(kl_envdepth_job *j) {
    if (!j) return 0;
    pthread_mutex_lock(&g_mu);
    if (!g.started || g.api == API_NONE) { pthread_mutex_unlock(&g_mu); return 0; }
    double t = now_s();
    if (g.rendering) {
        if (!kled_job_expired(g.rendering, g.job_t, t, KLED_JOB_TIMEOUT_S)) { pthread_mutex_unlock(&g_mu); return 0; }
        // Lost: its slot is left alone until the late job_done (the GPU may still write it); a new job may start.
        if (g.state[g.job_slot] == S_RENDER) g.state[g.job_slot] = S_LOST;
        g.rendering = 0;
        ONCE("a depth job did not finish in time: given up, a new one starts");
        lost_note(t, 0);
    }
    if (g.ready >= 0 && t - g.fr[g.ready].time_s < 0.033) { pthread_mutex_unlock(&g_mu); return 0; }
    int s = -1;
    for (int i = 0; i < KLED_IMAGES; i++)
        if (g.state[i] == S_FREE && i != g.held && i != g.prev) { s = i; break; }
    if (s < 0) { lost_note(t, 1); pthread_mutex_unlock(&g_mu); return 0; }
    memset(j, 0, sizeof *j);
    j->slot = s;
    j->gen = g.gen;
    j->w = KLED_W; j->h = KLED_H;
    j->mtl_tex = g.api == API_VULKAN ? g.mtl[s] : NULL;
    j->cpu = g.api == API_GL ? g.cpu[s] : NULL;
    if (!j->mtl_tex && !j->cpu) { pthread_mutex_unlock(&g_mu); return 0; }
    j->near_z = KLED_NEAR; j->far_z = KLED_FAR;
    for (int e = 0; e < 2; e++) eye_view(e, j->pose[e], j->tan[e]);
    j->seq = ++g.job_seq;
    g.state[s] = S_RENDER;
    g.rendering = 1;
    g.job_slot = s;
    g.job_t = t;
    g.inflight++;
    g.fr[s].index = s;
    g.fr[s].time_s = t;
    g.fr[s].near_z = KLED_NEAR; g.fr[s].far_z = KLED_FAR;
    memcpy(g.fr[s].pose, j->pose, sizeof j->pose);
    memcpy(g.fr[s].tan, j->tan, sizeof j->tan);
    pthread_mutex_unlock(&g_mu);
    return 1;
}

void kl_envdepth_job_done(const kl_envdepth_job *j, int ok) {
    if (!j) return;
    pthread_mutex_lock(&g_mu);
    if (g.inflight > 0) g.inflight--;
    if (j->gen != g.gen || g.api == API_NONE) { pthread_mutex_unlock(&g_mu); return; }
    int act = kled_done_action(g.rendering, j->seq, g.job_seq, g.state[j->slot] == S_LOST);
    if (act != KLED_DONE_CURRENT) {
        if (act == KLED_DONE_FREE_LOST) { g.state[j->slot] = S_FREE; lost_note(now_s(), 0); }   // late finish: slot free again
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g.rendering = 0;
    if (ok) {
        if (g.ready >= 0) g.state[g.ready] = S_FREE;
        g.ready = j->slot;
        g.state[j->slot] = S_READY;
    } else {
        g.state[j->slot] = S_FREE;
    }
    pthread_mutex_unlock(&g_mu);
}
