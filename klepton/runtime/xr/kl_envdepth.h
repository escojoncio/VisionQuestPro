// kl_envdepth.h - environment depth (Quest 3's XR_META_environment_depth and
// ovrp_*EnvironmentDepth*) served from the ARKit scene mesh.
//
// ONE state shared by both facades (kl_ovrp_spatial.c, kl_openxr_spatial.c):
// the provider's lifecycle, a ring of KLED_IMAGES depth images the guest
// samples, and the per-frame record (near/far, eye poses, tangents) that goes
// with each. The picture itself is rasterised by KleptonEnvDepth.swift on the
// compositor thread: the scene mesh (planes as the fallback) is drawn from each
// eye's pose into a small 2-layer depth image, and handed over here.
//
// The image the guest gets:
//   * D16_UNORM (VK_FORMAT_D16_UNORM / GL_DEPTH_COMPONENT16), 2 array layers,
//     one per eye (layer 0 left), KLED_W x KLED_H;
//   * NON-linear depth, forward-Z, window range [0,1]:
//         d = far * (z - near) / (z * (far - near))      z = distance along the view axis
//     with near = KLED_NEAR and far = KLED_FAR (finite). Anything with no
//     geometry reads 1.0, i.e. "far away - occludes nothing";
//   * rendered with the usual perspective for the eye's pose and asymmetric
//     field of view, so the guest reprojects it exactly as it would on Quest.
//     Vulkan: row 0 is the top of the view. GL: row 0 is the bottom (uploaded
//     flipped), as a GL render would leave it.
//
// Nothing here runs unless a guest created a provider AND started it, and it
// only exists when ARKit's scene reconstruction runs and is authorised.
#ifndef KL_ENVDEPTH_H
#define KL_ENVDEPTH_H
#include <stdint.h>

enum { KLED_IMAGES = 4, KLED_W = 320, KLED_H = 320 };
#define KLED_NEAR 0.1f
#define KLED_FAR  20.0f

// Pose = { px, py, pz, qx, qy, qz, qw } in the guest's tracking space.
// tan = { left, right, up, down }, all positive (cp_view_get_tangents order).
typedef struct {
    int    index;                 // which image of the ring holds this frame
    double time_s;                // CLOCK_MONOTONIC seconds when it was rasterised
    float  near_z, far_z;
    float  pose[2][7];
    float  tan[2][4];
} kl_envdepth_frame;

// ---- facades (guest threads) -----------------------------------------------
// 1 when ARKit scene reconstruction runs and the user authorised it. The only
// condition under which either facade announces depth.
int  kl_envdepth_supported(void);
// Provider lifecycle. create fails (-1) when unsupported. destroy releases the
// images too. start/stop gate the rasteriser.
int  kl_envdepth_create(int hand_removal);
void kl_envdepth_destroy(void);
int  kl_envdepth_created(void);
int  kl_envdepth_start(void);
void kl_envdepth_stop(void);
void kl_envdepth_set_hand_removal(int on);   // stored; ARKit's mesh has no hands in it either way
// The images: allocated on first use (on the calling thread; a GL guest needs
// its context current). Returns the count (KLED_IMAGES) or 0 on failure.
int  kl_envdepth_alloc(void);
void kl_envdepth_free(void);
int  kl_envdepth_alloc_state(void);          // 1 when the images exist
// Image `i` as the guest names it: a VkImage handle, or a GL texture name.
uint64_t kl_envdepth_image(int i);
int  kl_envdepth_is_vulkan(void);
// Takes the newest finished frame. Returns 1 (f filled), 0 none yet, -1 not
// started. A GL guest's texture is updated here, so call it on the GL thread.
int  kl_envdepth_acquire(kl_envdepth_frame *f);
// The frame's poses are in the guest's tracking space. For a guest that asks in
// another space: `eye0_rel` is eye 0's pose located in that space (OpenXR
// xrLocateViews with displayTime 0, i.e. the same latched head this module
// reads), and every pose of `f` is rebased with it.
void kl_envdepth_rebase(kl_envdepth_frame *f, const float eye0_rel[7]);

// ---- compositor (KleptonEnvDepth.swift) ------------------------------------
typedef struct {
    int      slot;                // image being rendered
    uint32_t gen;                 // the image set it belongs to (job_done ignores a stale one)
    uint32_t seq;                 // which job it is (job_done ignores one that timed out and was replaced)
    int      w, h;
    void    *mtl_tex;             // Vulkan guest: the image's MTLTexture (2D array, depth16). NULL for GL
    uint16_t *cpu;                // GL guest: 2 layers of w*h depth16 to fill, layer 0 first, row 0 = top
    float    near_z, far_z;
    float    pose[2][7];
    float    tan[2][4];
} kl_envdepth_job;
// 1 when a picture is due (started, images exist, newest one old or taken). A job the
// compositor never finishes is given up after KLED_JOB_TIMEOUT_S and a new one may start.
int  kl_envdepth_job_begin(kl_envdepth_job *j);
// The render finished (ok = 0 abandons the slot).
void kl_envdepth_job_done(const kl_envdepth_job *j, int ok);

#endif
