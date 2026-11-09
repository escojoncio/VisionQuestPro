// The envdepth convention (kl_envdepth.c KL-TEST envdepthz): reversed-Z, 1 at the near plane, 0 at the far plane and where nothing is drawn;
// and the projection terms KleptonEnvDepth.swift uses give exactly that.
#include <math.h>
#include <stdio.h>
#include "envdepthz.inc"

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) <= 1e-5f)

int main(void) {
    const float n = 0.1f, f = 20.0f;
    CK(NEAR(kled_depth_rev(n, n, f), 1.0f));            // at the near plane: close
    CK(NEAR(kled_depth_rev(f, n, f), 0.0f));            // at the far plane: infinitely far
    CK(kled_depth_rev(1.0f, n, f) > kled_depth_rev(2.0f, n, f));       // nearer = larger
    CK(kled_depth_rev(0.05f, n, f) == 1.0f && kled_depth_rev(50.0f, n, f) == 0.0f);   // clamped
    CK(kled_depth_rev(NAN, n, f) == 0.0f && kled_depth_rev(-1.0f, n, f) == 0.0f);     // no geometry never reads "touching the camera"
    CK(NEAR(kled_depth_rev(1.0f, n, f), 0.1f * 19.0f / (1.0f * 19.9f)));
    // the projection: clip.z = a * eyeZ + b, clip.w = -eyeZ (eye looks down -Z), depth = clip.z / clip.w
    float a, b;
    kled_proj_ab(n, f, &a, &b);
    for (float z = 0.1f; z <= 20.0f; z += 0.37f) {
        float d = (a * -z + b) / z;
        CK(fabsf(d - kled_depth_rev(z, n, f)) <= 1e-4f);
        CK(d >= -1e-5f && d <= 1.0f + 1e-5f);
    }
    printf(fails ? "t_envdepth_z: %d FAILED\n" : "t_envdepth_z: all passed\n", fails);
    return fails != 0;
}
