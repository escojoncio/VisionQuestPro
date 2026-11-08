// Host test of vq_config.c's vq_quest3_resolve: the Quest 2 / Quest 3 profile of one game.
// choice: 0 automatic, 2 Quest 2, 3 Quest 3. Automatic = Quest 3 only if the global setting is on and, when the
// manifest declares supportedDevices, it lists quest3; no declaration follows the global setting alone.
#include <stdio.h>
#include "vq_config.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    // Automatic, global on: Maestro-like (declares devices without quest3) -> Quest 2; with quest3 -> Quest 3.
    CHECK(vq_quest3_resolve(0, 1, 1, 0) == 0, "auto, declared without quest3 -> Quest 2");
    CHECK(vq_quest3_resolve(0, 1, 1, 1) == 1, "auto, declared with quest3 -> Quest 3");
    // No declaration at all: the global setting decides (the behaviour before the per-game profile).
    CHECK(vq_quest3_resolve(0, 1, 0, 0) == 1, "auto, undeclared, global on -> Quest 3");
    CHECK(vq_quest3_resolve(0, 0, 0, 0) == 0, "auto, undeclared, global off -> Quest 2");
    // Global off wins over a manifest that lists quest3 in automatic.
    CHECK(vq_quest3_resolve(0, 0, 1, 1) == 0, "auto, global off, manifest quest3 -> Quest 2");
    CHECK(vq_quest3_resolve(0, 0, 1, 0) == 0, "auto, global off, manifest without quest3 -> Quest 2");
    // The game's own choice ignores the global setting and the manifest.
    for (int g = 0; g <= 1; g++)
        for (int d = 0; d <= 1; d++)
            for (int m = 0; m <= 1; m++) {
                CHECK(vq_quest3_resolve(2, g, d, m) == 0, "override Quest 2 always 0");
                CHECK(vq_quest3_resolve(3, g, d, m) == 1, "override Quest 3 always 1");
            }
    // Garbage choice is automatic.
    CHECK(vq_quest3_resolve(7, 1, 1, 1) == 1 && vq_quest3_resolve(-1, 1, 1, 0) == 0, "unknown choice = automatic");
    printf(fails ? "t_quest_profile: %d FAILED\n" : "t_quest_profile: ok\n", fails);
    return fails != 0;
}
