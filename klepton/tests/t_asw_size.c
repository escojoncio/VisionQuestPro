// Host test of kl_ovrp.c's KL-TEST aswsize block (cut out at build time): the two SIZE out-params of
// ovrp_GetLayerTextureSpaceWarp (x4 = motion-vector size, x6 = depth size, each an ovrpSizei) carry the
// real size of the images served. UE4's OculusHMD builds its SpaceWarp framebuffer from them (0x0 there
// gave a 0x0 render area and ~92 % of the draws dropped); Unity ignores them.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int w, h; } ovrp_sizei;
#include "aswsize.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    uint64_t m, d;
    ovrp_sizei s;

    // Motion and depth both served: both out-params carry the image size.
    m = d = ~0ull;
    klovrp_asw_put_sizes(&m, &d, 1337, 1073, 1);
    memcpy(&s, &m, sizeof s); CHECK(s.w == 1337 && s.h == 1073, "motion size is the image size");
    memcpy(&s, &d, sizeof s); CHECK(s.w == 1337 && s.h == 1073, "depth size is the image size");

    // Depth format None: no depth image, so no depth size (and the whole 8 bytes are written).
    m = d = ~0ull;
    klovrp_asw_put_sizes(&m, &d, 744, 600, 0);
    memcpy(&s, &m, sizeof s); CHECK(s.w == 744 && s.h == 600, "motion size without depth");
    CHECK(d == 0, "no depth image -> depth size 0x0, all 8 bytes");

    // Nothing is written past the 8 bytes of an ovrpSizei.
    uint64_t pair[2] = { ~0ull, 0xA5A5A5A5A5A5A5A5ull }, pair2[2] = { ~0ull, 0xA5A5A5A5A5A5A5A5ull };
    klovrp_asw_put_sizes(&pair[0], &pair2[0], 8, 4, 1);
    CHECK(pair[1] == 0xA5A5A5A5A5A5A5A5ull && pair2[1] == 0xA5A5A5A5A5A5A5A5ull, "no write past the struct");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
