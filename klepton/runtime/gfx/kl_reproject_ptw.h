// Positional timewarp (PTW) — reprojection with the guest's per-pixel depth.
//
// The rotation-only pass (kl_reproject.h) places the guest's picture as a flat
// quad far away, which corrects head ROTATION exactly and head TRANSLATION not
// at all. This is the Oculus "Positional TimeWarp" / ASW 2.0 shape instead:
// a dense grid over the eye picture, each vertex pushed out to the distance the
// guest's depth buffer says is there, i.e. the picture turned back into a
// height-field mesh in the render eye's space, then looked at from where the
// display eye is NOW (full rigid delta, translation included).
//
// Disocclusion: the mesh is continuous, so a region the old viewpoint could not
// see is filled by the triangles spanning the depth edge (rubber-sheet stretch
// of the background texels, as Oculus PTW does). Each vertex takes the NEAREST
// depth of a small neighbourhood (dilation), so the stretch lands on the
// background side and foreground silhouettes stay sharp instead of tearing.
// Depth at the far plane / infinity is clamped to `far_cap` metres.
//
// The pass writes the real reprojected depth, so Compositor Services receives
// per-pixel geometry (OpenXR XR_KHR_composition_layer_depth analogue) and its
// own late-stage correction is right for near content too.
//
// Plain C, no simd: the maths here is checked on Linux (tests/t_ptw.c).
#ifndef KL_REPROJECT_PTW_H
#define KL_REPROJECT_PTW_H

#include <stdint.h>

// Grid cells per side of the PTW mesh (per eye). 96x96 = 55k vertices per eye.
#define KL_PTW_GRID_DEFAULT 96
#define KL_PTW_GRID_MAX     256

// Bound at vertex buffer(2), one per amplified view; matches `KLPtw` in
// kl_reproject.c's MSL. Column-major matrices. 192 bytes.
typedef struct {
    float    model_view[16]; // render EYE space -> display eye space, WITH translation
    float    depth_p[4];     // near, far (0 = infinite), reversed (1/0), far_cap metres
    float    tex_size[4];    // depth texture w, h, 1/w, 1/h
    uint32_t grid_n;         // cells per side
    uint32_t depth_slice;    // array layer of this eye in its depth texture
    uint32_t dilate;         // 1 = nearest-of-neighbourhood depth
    uint32_t on;             // 0 = this view has no usable depth (vertex collapses)
    // ASW (object extrapolation, KL_CP_ASW): MV scale x/y (motion-vector value -> eye-picture
    // uv, signed as VPKTAA's), fraction of a guest frame to extrapolate, 1 = MVs bound.
    float    asw[4];
    float    mv_size[4];     // MV texture w, h, per-axis cap (picture fraction), array slice
    float    pad[12];
} kl_ptw_uniforms;

// Metres in front of the eye for a device depth value d (Vulkan clip z in [0,1]).
float kl_ptw_linear_depth(float d, float near_z, float far_z, int reversed, float far_cap);

// out = column-major model-view taking a point in the guest's render EYE space
// to the display eye's space.
//   render_q/render_p  head pose the guest rendered with (tracking space)
//   origin_y           kl_ovrp_guest_origin_y (EyeLevel offset), added to p.y
//   eye_off            the render eye's offset from the head (metres, head space)
//   origin_from_head   the display head pose NOW (column-major)
//   head_from_view     the display eye in head space (column-major, may be canted)
void kl_ptw_model_view(float out[16], const float render_q[4], const float render_p[3],
                       float origin_y, const float eye_off[3],
                       const float origin_from_head[16], const float head_from_view[16]);

// The vertex's point in render eye space for grid parameter (cx, cy) in [0,1]
// (cy = 1 is UP) at linear depth z, for render tangents l, r, t, b.
void kl_ptw_point(float out[3], float cx, float cy, float z, const float tan4[4]);

// Fill the parts of one view's uniforms that are not textures.
void kl_ptw_fill(kl_ptw_uniforms *u, const float model_view[16],
                 float near_z, float far_z, int reversed, float far_cap,
                 int tex_w, int tex_h, uint32_t grid_n, uint32_t depth_slice,
                 int dilate);

// ---------------------------------------------------------------------------
// ASW 2.0 object extrapolation (Oculus Application SpaceWarp). The guest's
// motion vectors (SpaceWarp layer, 0x300 / ovrp_GetLayerTextureSpaceWarp) say
// how far each surface moved, in picture uv, over ONE guest frame. When the
// frame on screen is older than the guest's cadence says it should be, each
// PTW vertex is displaced along its MV by `frac` of that motion before the
// head-pose reprojection, so moving objects keep moving instead of
// freezing with the (correctly reprojected) world. Camera motion is NOT in the
// MVs (AppSW excludes it); the PTW delta already carries it.

// How many guest frames the picture is overdue: (age - lead) / interval, clamped
// to [0, max_frac]. `lead` is the age a FRESH frame has at first use (render
// latency), so a frame shown on time extrapolates by 0. Zero on bad input.
float kl_asw_fraction(float age_ms, float lead_ms, float interval_ms, float max_frac);

// Picture-space displacement of a vertex for motion vector mv (as stored) in
// grid-parameter space (cy UP): mv * scale * frac, y flipped when the MV texture
// origin is top-left, each axis capped at +-max_c. Returns 0 and dc = 0 for a
// NaN / infinite / |mv| > 4 vector (VPKTAA's validity rule), else 1.
int kl_asw_displace(float dc[2], const float mv[2], const float scale[2],
                    float frac, int flip_y, float max_c);

// Turn extrapolation on for one view's uniforms (after kl_ptw_fill).
void kl_ptw_fill_asw(kl_ptw_uniforms *u, float scale_x, float scale_y, float frac,
                     int mv_w, int mv_h, uint32_t mv_slice, float max_c);

// MSL to APPEND to kl_reproject_msl(); vertex function kl_ptw_v.
const char *kl_reproject_ptw_msl(void);

// Column-major 4x4 helpers (exposed for the test).
void kl_ptw_mul(float out[16], const float a[16], const float b[16]);
int  kl_ptw_invert_rigid(float out[16], const float m[16]);
void kl_ptw_transform(float out[4], const float m[16], const float v[4]);

#endif
