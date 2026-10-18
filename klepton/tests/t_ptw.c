// Positional timewarp maths (kl_reproject_ptw.c), runnable on Linux:
//   cc -O1 -o /tmp/t_ptw tests/t_ptw.c runtime/gfx/kl_reproject_ptw.c -lm && /tmp/t_ptw
#include <math.h>
#include <stdio.h>
#include "../runtime/gfx/kl_reproject_ptw.h"

static int fails;
#define NEAR(a, b, e, what) do { double _a = (a), _b = (b); \
    if (!(fabs(_a - _b) <= (e))) { printf("FAIL %s: %.6f vs %.6f\n", what, _a, _b); fails++; } } while (0)

static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

int main(void) {
    // 1. depth linearisation, both conventions, finite and infinite far.
    float n = 0.1f, f = 100.0f;
    for (float z = 0.2f; z < 90.0f; z *= 1.7f) {
        float drev = n * (f - z) / (z * (f - n));          // reversed, finite far
        float dstd = f * (z - n) / (z * (f - n));          // standard, finite far
        NEAR(kl_ptw_linear_depth(drev, n, f, 1, 1000), z, z * 1e-4, "reversed finite");
        NEAR(kl_ptw_linear_depth(dstd, n, f, 0, 1000), z, z * 1e-3, "standard finite");
        NEAR(kl_ptw_linear_depth(n / z, n, 0, 1, 1000), z, z * 1e-4, "reversed infinite");
        NEAR(kl_ptw_linear_depth(1 - n / z, n, 0, 0, 1000), z, z * 1e-3, "standard infinite");
    }
    NEAR(kl_ptw_linear_depth(0.0f, n, 0, 1, 300), 300, 0, "reversed clear = far cap");
    NEAR(kl_ptw_linear_depth(1.0f, n, f, 1, 300), n, 1e-6, "reversed near plane");

    // 2. Same pose now as rendered: every grid point maps back onto its own ray.
    float q[4] = {0.1f, 0.3f, -0.05f, 0.95f}, p[3] = {0.2f, 1.5f, -0.3f}, eye[3] = {-0.032f, 0, 0};
    float tan4[4] = {1.0f, 1.2f, 0.9f, 1.1f};
    // origin_from_head equal to the render head pose (built through the same path, eye 0).
    float zero[3] = {0, 0, 0}, ofh[16], hfv[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -0.032f,0,0,1};
    kl_ptw_model_view(ofh, q, p, 0, zero, I, I);
    float tmp[16]; for (int i = 0; i < 16; i++) tmp[i] = ofh[i];  // model_view(I,I) = origin_from_head
    float mv[16];
    kl_ptw_model_view(mv, q, p, 0, eye, tmp, hfv);
    for (int i = 0; i < 16; i++) NEAR(mv[i], I[i], 1e-5, "identity model-view");

    // 3. Pure lateral head translation dx: a point at depth z shifts by dx/z in tangent.
    float dx = 0.05f;
    float moved[16]; for (int i = 0; i < 16; i++) moved[i] = tmp[i];
    // head now 5 cm to the right in ITS OWN frame
    float step[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, dx,0,0,1};
    kl_ptw_mul(moved, tmp, step);
    kl_ptw_model_view(mv, q, p, 0, eye, moved, hfv);
    for (float z = 0.5f; z < 50; z *= 3) {
        float pt[3]; kl_ptw_point(pt, 0.5f, 0.5f, z, tan4);
        float v[4] = {pt[0], pt[1], pt[2], 1}, o[4];
        kl_ptw_transform(o, mv, v);
        float tx0 = pt[0] / -pt[2], tx1 = o[0] / -o[2];
        NEAR(tx1 - tx0, -dx / z, 1e-5, "parallax = -dx/z");
        NEAR(o[2], pt[2], 1e-5, "depth unchanged by lateral move");
    }

    // 4. Grid point at (cx, cy) lies on the guest's frustum ray.
    float pt[3]; kl_ptw_point(pt, 0.0f, 1.0f, 2.0f, tan4);
    NEAR(pt[0] / -pt[2], -tan4[0], 1e-6, "left edge tangent");
    NEAR(pt[1] / -pt[2],  tan4[2], 1e-6, "top edge tangent");

    // 5. Uniform block layout matches the MSL struct (192 bytes, gridN at 96).
    NEAR(sizeof(kl_ptw_uniforms), 192, 0, "uniform size");
    NEAR(__builtin_offsetof(kl_ptw_uniforms, grid_n), 96, 0, "gridN offset");

    // 6. ASW extrapolation: known MV -> expected displacement.
    {
        float sc[2] = {0.5f, 0.5f}, mvv[2] = {0.04f, -0.02f}, dc[2];
        // NDC delta (0.04, -0.02) -> uv (0.02, -0.01); half a guest frame -> (0.01, -0.005).
        NEAR(kl_asw_displace(dc, mvv, sc, 0.5f, 0, 1.0f), 1, 0, "mv valid");
        NEAR(dc[0], 0.01, 1e-7, "asw dx"); NEAR(dc[1], -0.005, 1e-7, "asw dy");
        // top-left-origin MV texture (Vulkan): y flips into grid space (cy UP).
        kl_asw_displace(dc, mvv, sc, 0.5f, 1, 1.0f);
        NEAR(dc[0], 0.01, 1e-7, "asw dx flip"); NEAR(dc[1], 0.005, 1e-7, "asw dy flip");
        // Metric check: a vertex at depth z moves by dc.x * (l + r) * z sideways.
        float c0x = 0.5f, z = 3.0f, p0[3], p1[3];
        kl_ptw_point(p0, c0x, 0.5f, z, tan4);
        kl_ptw_point(p1, c0x + dc[0], 0.5f + dc[1], z, tan4);
        NEAR(p1[0] - p0[0], 0.01 * (tan4[0] + tan4[1]) * z, 1e-6, "asw metres x");
        NEAR(p1[1] - p0[1], 0.005 * (tan4[2] + tan4[3]) * z, 1e-6, "asw metres y");
        NEAR(p1[2], p0[2], 0, "asw keeps depth");
        // frac 0 = identity; cap; invalid vectors are ignored.
        kl_asw_displace(dc, mvv, sc, 0.0f, 0, 1.0f);
        NEAR(dc[0], 0, 0, "frac 0 dx"); NEAR(dc[1], 0, 0, "frac 0 dy");
        kl_asw_displace(dc, mvv, sc, 100.0f, 0, 0.05f);
        NEAR(dc[0], 0.05, 1e-7, "cap +"); NEAR(dc[1], -0.05, 1e-7, "cap -");
        float bad[2] = {NAN, 0.0f}, big[2] = {5.0f, 0.0f};
        NEAR(kl_asw_displace(dc, bad, sc, 1.0f, 0, 1.0f), 0, 0, "nan rejected");
        NEAR(kl_asw_displace(dc, big, sc, 1.0f, 0, 1.0f), 0, 0, "|mv|>4 rejected");
        NEAR(dc[0], 0, 0, "rejected dc = 0");
        // sign of the scale (VPKTAA's mvSign = -1) reverses the motion.
        float neg[2] = {-0.5f, -0.5f};
        kl_asw_displace(dc, mvv, neg, 1.0f, 0, 1.0f);
        NEAR(dc[0], -0.02, 1e-7, "negative scale");
        // Fraction: on time = 0; one interval late = 1; cap; bad input = 0.
        NEAR(kl_asw_fraction(12.0f, 12.0f, 11.1f, 1.5f), 0, 0, "on time");
        NEAR(kl_asw_fraction(8.0f, 12.0f, 11.1f, 1.5f), 0, 0, "early = 0");
        NEAR(kl_asw_fraction(23.1f, 12.0f, 11.1f, 1.5f), 1, 1e-5, "one frame late");
        NEAR(kl_asw_fraction(60.0f, 12.0f, 11.1f, 1.5f), 1.5, 0, "capped");
        NEAR(kl_asw_fraction(20.0f, 12.0f, 0.0f, 1.5f), 0, 0, "no interval");
        // Uniform fill.
        kl_ptw_uniforms u; kl_ptw_fill(&u, I, 0.1f, 0, 1, 200, 64, 64, 8, 0, 1);
        NEAR(u.asw[3], 0, 0, "asw off by default");
        kl_ptw_fill_asw(&u, 0.5f, 0.5f, 0.7f, 32, 32, 1, 0.1f);
        NEAR(u.asw[3], 1, 0, "asw on"); NEAR(u.mv_size[3], 1, 0, "mv slice");
        kl_ptw_fill_asw(&u, 0.5f, 0.5f, 0.0f, 32, 32, 1, 0.1f);
        NEAR(u.asw[3], 0, 0, "asw off at frac 0");
        NEAR(__builtin_offsetof(kl_ptw_uniforms, asw), 112, 0, "asw offset");
    }

    printf(fails ? "t_ptw: %d FAILED\n" : "t_ptw: ok\n", fails);
    return fails != 0;
}
