// Host test of vq_config.c's vq_area_resolve: the play area of one game ("Área de juego").
// Mode 0 free: a roomscale rectangle w x d, each side 2..6 m, default 3 (also for NaN / <= 0). Mode 1 seated: a circle, the
// size is its diameter (2 x 1.0 m radius). Unknown modes are free.
#include <stdio.h>
#include <math.h>
#include "vq_config.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    float w, d;
    CHECK(vq_area_resolve(0, 4.0f, 5.5f, &w, &d) == 0 && w == 4.0f && d == 5.5f, "free keeps a valid size");
    CHECK(vq_area_resolve(0, 3.0f, 3.0f, &w, &d) == 0 && w == 3.0f && d == 3.0f, "free default 3 x 3");
    CHECK(vq_area_resolve(0, 1.0f, 9.0f, &w, &d) == 0 && w == 2.0f && d == 6.0f, "free clamps to 2..6");
    CHECK(vq_area_resolve(0, 0.0f, -1.0f, &w, &d) == 0 && w == 3.0f && d == 3.0f, "free: 0 / negative -> default");
    CHECK(vq_area_resolve(0, NAN, INFINITY, &w, &d) == 0 && w == 3.0f && d == 3.0f, "free: NaN / inf -> default");
    CHECK(vq_area_resolve(1, 4.0f, 5.0f, &w, &d) == 1 && w == 2.0f && d == 2.0f, "seated ignores the sliders: diameter 2 m");
    CHECK(vq_area_resolve(7, 4.0f, 5.0f, &w, &d) == 0 && w == 4.0f && d == 5.0f, "unknown mode = free");
    CHECK(vq_area_resolve(-1, 2.5f, 2.5f, &w, &d) == 0 && w == 2.5f, "negative mode = free");
    printf(fails ? "t_vq_area: %d FAILED\n" : "t_vq_area: ok\n", fails);
    return fails != 0;
}
