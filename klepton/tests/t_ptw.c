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

    printf(fails ? "t_ptw: %d FAILED\n" : "t_ptw: ok\n", fails);
    return fails != 0;
}
