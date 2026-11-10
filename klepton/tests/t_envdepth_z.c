// The envdepth convention (kl_envdepth.c KL-TEST envdepthz): forward-Z as Meta's SDK decodes it, 0 at the near plane, 1 at the far plane and
// where nothing is drawn; the projection terms KleptonEnvDepth.swift uses give exactly that; and the guest's own decode returns the distance.
#include <math.h>
#include <stdio.h>
#include "envdepthz.inc"

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) <= 1e-5f)

int main(void) {
    const float n = 0.1f, f = 20.0f;
    CK(NEAR(kled_depth_fwd(n, n, f), 0.0f));            // at the near plane
    CK(NEAR(kled_depth_fwd(f, n, f), 1.0f));            // at the far plane
    CK(kled_depth_fwd(1.0f, n, f) < kled_depth_fwd(2.0f, n, f));       // nearer = smaller
    CK(kled_depth_fwd(0.05f, n, f) == 0.0f && kled_depth_fwd(50.0f, n, f) == 1.0f);   // clamped
    CK(kled_depth_fwd(NAN, n, f) == 1.0f && kled_depth_fwd(-1.0f, n, f) == 1.0f);     // no geometry never reads "touching the camera"
    CK(NEAR(kled_depth_fwd(1.0f, n, f), 20.0f * 0.9f / (19.9f * 1.0f)));
    // What the guest does with it (Meta.XR.EnvironmentDepth: ComputeNdcToLinearDepthParameters + the occlusion shader):
    // linear = x / ((2 d - 1) + y), x = -2 f n / (f - n), y = -(f + n) / (f - n). It must give back the distance.
    for (float z = 0.15f; z < 19.0f; z *= 1.7f) {
        const float x = -2.0f * f * n / (f - n), y = -(f + n) / (f - n);
        const float lin = x / ((2.0f * kled_depth_fwd(z, n, f) - 1.0f) + y);
        CK(fabsf(lin - z) <= 1e-2f * z);
    }
    {   // ...and "no geometry" (1.0) decodes as the far plane, not as a wall at the camera.
        const float x = -2.0f * f * n / (f - n), y = -(f + n) / (f - n);
        CK(fabsf(x / ((2.0f * 1.0f - 1.0f) + y) - f) <= 1e-2f);
    }
    // the projection: clip.z = a * eyeZ + b, clip.w = -eyeZ (eye looks down -Z), depth = clip.z / clip.w
    float a, b;
    kled_proj_ab(n, f, &a, &b);
    for (float z = 0.1f; z <= 20.0f; z += 0.37f) {
        float d = (a * -z + b) / z;
        CK(fabsf(d - kled_depth_fwd(z, n, f)) <= 1e-4f);
        CK(d >= -1e-5f && d <= 1.0f + 1e-5f);
    }
    printf(fails ? "t_envdepth_z: %d FAILED\n" : "t_envdepth_z: all passed\n", fails);
    return fails != 0;
}
