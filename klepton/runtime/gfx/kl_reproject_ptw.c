// Positional timewarp — CPU half. See kl_reproject_ptw.h.
#include <math.h>
#include <string.h>
#include "kl_reproject_ptw.h"

float kl_ptw_linear_depth(float d, float n, float f, int reversed, float far_cap) {
    if (!(n > 0.0f)) return far_cap;
    float z;
    if (reversed) {
        // reversed: d = 1 at near, 0 at far (infinite far: d = n / z)
        if (f > 0.0f) { float den = n + d * (f - n); z = den > 0.0f ? n * f / den : far_cap; }
        else z = d > 0.0f ? n / d : far_cap;
    } else {
        if (f > 0.0f) { float den = f - d * (f - n); z = den > 0.0f ? n * f / den : far_cap; }
        else z = d < 1.0f ? n / (1.0f - d) : far_cap;
    }
    if (!(z > 0.0f) || z > far_cap) z = far_cap;
    if (z < n) z = n;
    return z;
}

void kl_ptw_mul(float o[16], const float a[16], const float b[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(o, r, sizeof r);
}

int kl_ptw_invert_rigid(float o[16], const float m[16]) {
    // [R t] -> [R^T, -R^T t]
    float r[16] = {0};
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) r[c * 4 + row] = m[row * 4 + c];
    for (int row = 0; row < 3; row++)
        r[12 + row] = -(r[0 + row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    r[15] = 1.0f;
    memcpy(o, r, sizeof r);
    return 1;
}

void kl_ptw_transform(float o[4], const float m[16], const float v[4]) {
    float r[4];
    for (int row = 0; row < 4; row++)
        r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    memcpy(o, r, sizeof r);
}

static void ptw_quat_matrix(float o[16], const float q[4], const float p[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = x * x + y * y + z * z + w * w;
    if (!(n > 1e-6f)) { x = y = z = 0; w = 1; n = 1; }
    float s = 2.0f / n;
    memset(o, 0, 16 * sizeof(float));
    o[0] = 1 - s * (y * y + z * z); o[1] = s * (x * y + w * z);     o[2] = s * (x * z - w * y);
    o[4] = s * (x * y - w * z);     o[5] = 1 - s * (x * x + z * z); o[6] = s * (y * z + w * x);
    o[8] = s * (x * z + w * y);     o[9] = s * (y * z - w * x);     o[10] = 1 - s * (x * x + y * y);
    o[12] = p[0]; o[13] = p[1]; o[14] = p[2]; o[15] = 1;
}

void kl_ptw_model_view(float out[16], const float render_q[4], const float render_p[3],
                       float origin_y, const float eye_off[3],
                       const float origin_from_head[16], const float head_from_view[16]) {
    // view <- head_now <- origin <- render_head <- render_eye
    float p[3] = { render_p[0], render_p[1] + origin_y, render_p[2] };
    float origin_from_rhead[16], rhead_from_reye[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,
                                                        eye_off[0], eye_off[1], eye_off[2], 1};
    ptw_quat_matrix(origin_from_rhead, render_q, p);
    float origin_from_reye[16];
    kl_ptw_mul(origin_from_reye, origin_from_rhead, rhead_from_reye);
    float origin_from_view[16], view_from_origin[16];
    kl_ptw_mul(origin_from_view, origin_from_head, head_from_view);
    kl_ptw_invert_rigid(view_from_origin, origin_from_view);
    kl_ptw_mul(out, view_from_origin, origin_from_reye);
}

void kl_ptw_point(float o[3], float cx, float cy, float z, const float t[4]) {
    // t = left, right, top, bottom (positive tangents); eye looks down -Z.
    o[0] = (-t[0] + (t[0] + t[1]) * cx) * z;
    o[1] = (-t[3] + (t[2] + t[3]) * cy) * z;
    o[2] = -z;
}

void kl_ptw_fill(kl_ptw_uniforms *u, const float mv[16],
                 float near_z, float far_z, int reversed, float far_cap,
                 int tex_w, int tex_h, uint32_t grid_n, uint32_t depth_slice, int dilate) {
    memset(u, 0, sizeof *u);
    memcpy(u->model_view, mv, sizeof u->model_view);
    u->depth_p[0] = near_z; u->depth_p[1] = far_z > 0 ? far_z : 0;
    u->depth_p[2] = reversed ? 1.0f : 0.0f; u->depth_p[3] = far_cap;
    u->tex_size[0] = (float)tex_w; u->tex_size[1] = (float)tex_h;
    u->tex_size[2] = tex_w > 0 ? 1.0f / tex_w : 0; u->tex_size[3] = tex_h > 0 ? 1.0f / tex_h : 0;
    if (grid_n < 2) grid_n = 2;
    if (grid_n > KL_PTW_GRID_MAX) grid_n = KL_PTW_GRID_MAX;
    u->grid_n = grid_n;
    u->depth_slice = depth_slice;
    u->dilate = dilate ? 1u : 0u;
    u->on = (near_z > 0 && tex_w > 0 && tex_h > 0) ? 1u : 0u;
}

// The PTW vertex stage. APPENDED to kl_reproject_msl() (it reuses KLReproj,
// VOut, kl_offscreen and the fragment kl_reproject_f), so the picture is
// sampled, decoded, faded and keyed exactly as in the rotation-only pass.
// Drawn as 6 * n * n vertices (non-indexed triangles), n = KLPtw.gridN.
static const char kl_msl_ptw[] =
"\n"
"struct KLPtw {\n"
"    float4x4 modelView;  // render EYE space -> display eye space\n"
"    float4   depthP;     // near, far (0 = inf), reversed, far cap (m)\n"
"    float4   texSize;    // w, h, 1/w, 1/h of the depth texture\n"
"    uint     gridN;\n"
"    uint     depthSlice;\n"
"    uint     dilate;\n"
"    uint     on;\n"
"    float    pad[20];\n"
"};\n"
"\n"
"static float kl_ptw_lin(float d, float4 p)\n"
"{\n"
"    float n = p.x, f = p.y, z;\n"
"    if (p.z > 0.5) z = f > 0.0 ? n * f / max(n + d * (f - n), 1e-9) : n / max(d, 1e-9);\n"
"    else           z = f > 0.0 ? n * f / max(f - d * (f - n), 1e-9) : n / max(1.0 - d, 1e-9);\n"
"    return clamp(z, n, p.w);\n"
"}\n"
"\n"
// Nearest surface of a 3x3 texel neighbourhood (dilate) or the texel itself.
// Nearest, not average: averaging across a silhouette invents a surface
// halfway between the two that neither eye ever saw.
"static float kl_ptw_depth(depth2d_array<float> dt, float2 uv, constant KLPtw &k)\n"
"{\n"
// read(), not sample(): a vertex function has no derivatives for implicit LOD.
"    int2 hi = int2(k.texSize.xy) - 1;\n"
"    int2 t0 = clamp(int2(uv * k.texSize.xy), int2(0), hi);\n"
"    float z = k.depthP.w;\n"
"    int r = k.dilate != 0u ? 1 : 0;\n"
"    for (int j = -r; j <= r; j++)\n"
"        for (int i = -r; i <= r; i++)\n"
"            z = min(z, kl_ptw_lin(dt.read(uint2(clamp(t0 + int2(i, j), int2(0), hi)), k.depthSlice), k.depthP));\n"
"    return z;\n"
"}\n"
"\n"
// The colour uv for grid parameter c, bilinear in the unwarp grid's cell, so
// a foveated / cropped eye is read exactly as the rotation-only pass reads it.
"static float2 kl_ptw_uv(constant float2 *grid, uint gridEye, float2 c)\n"
"{\n"
"    uint nx = uint(grid[0].x), ny = uint(grid[0].y);\n"
"    uint base = 1u + gridEye * (nx + 1u) * (ny + 1u);\n"
"    float2 g = c * float2(float(nx), float(ny));\n"
"    uint2 g0 = min(uint2(g), uint2(nx - 1u, ny - 1u));\n"
"    float2 f = g - float2(g0);\n"
"    float2 a = grid[base + g0.y * (nx + 1u) + g0.x];\n"
"    float2 b = grid[base + g0.y * (nx + 1u) + g0.x + 1u];\n"
"    float2 d = grid[base + (g0.y + 1u) * (nx + 1u) + g0.x];\n"
"    float2 e = grid[base + (g0.y + 1u) * (nx + 1u) + g0.x + 1u];\n"
"    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);\n"
"}\n"
"\n"
"vertex VOut kl_ptw_v(uint vid [[vertex_id]],\n"
"                     ushort amp [[amplification_id]],\n"
"                     constant KLReproj *ua [[buffer(0)]],\n"
"                     constant float2 *grid [[buffer(1)]],\n"
"                     constant KLPtw *pa [[buffer(2)]],\n"
"                     depth2d_array<float> d0 [[texture(0)]],\n"
"                     depth2d_array<float> d1 [[texture(1)]])\n"
"{\n"
"    constant KLReproj &u = ua[amp];\n"
"    constant KLPtw &k = pa[amp];\n"
"    uint n = max(k.gridN, 2u);\n"
"    uint cell = vid / 6u, q = vid % 6u;\n"
"    uint2 g = uint2(cell % n, cell / n);\n"
"    g.x += (q == 1u || q == 3u || q == 4u) ? 1u : 0u;\n"
"    g.y += (q == 2u || q == 4u || q == 5u) ? 1u : 0u;\n"
"    float2 c = float2(g) / float(n);\n"
"    float2 uv = kl_ptw_uv(grid, u.gridEye, c);\n"
"    if (u.flipY != 0u) uv.y = 1.0 - uv.y;\n"
"    float z = amp == 0 ? kl_ptw_depth(d0, uv, k) : kl_ptw_depth(d1, uv, k);\n"
"    float x = mix(-u.tangents.x, u.tangents.y, c.x) * z;\n"
"    float y = mix(-u.tangents.w, u.tangents.z, c.y) * z;\n"
"    VOut o;\n"
"    o.slice = u.slice;\n"
"    o.srgb = u.srgbDecode;\n"
"    o.chromaOn = u.chromaOn;\n"
"    o.chromaKey = float3(u.chromaKey);\n"
"    o.chromaRange = float2(u.chromaMin, u.chromaMax);\n"
"    o.fadeOn = u.fadeOn;\n"
"    o.fadeS = float3(u.fadeScale[0], u.fadeScale[1], u.fadeScale[2]);\n"
"    o.fadeO = float3(u.fadeOffset[0], u.fadeOffset[1], u.fadeOffset[2]);\n"
"    o.src = uint(amp);\n"
"    o.pos = (u.visible == 0u || k.on == 0u) ? kl_offscreen\n"
"          : u.projection * k.modelView * float4(x, y, -z, 1.0);\n"
"    o.uv = uv;\n"
"    return o;\n"
"}\n";

const char *kl_reproject_ptw_msl(void) { return kl_msl_ptw; }
