// Host test of klvk_retire_due (kl_vulkan.c, KL-TEST retiredue block): when a retired image (replaced eye / SpaceWarp /
// depth-snapshot set) is destroyed. Fence-and-frame based (the GPU's completed-frame serial), not a fixed 5 s wait.
// Build: tools/ci/host-tests.sh (ASan + UBSan); the block is extracted into retiredue.inc.
#include <stdio.h>

#include "retiredue.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define AFTER 5.0

int main(void) {
    // Normal image (a depth snapshot), retired in frame 100, the GPU has completed frame 100.
    CHECK(!klvk_retire_due(0, -1, 0.01, 100, 100, 100, AFTER, 0, 0), "not due the instant it is retired");
    CHECK(!klvk_retire_due(0, -1, 0.05, 102, 100, 102, AFTER, 0, 0), "2 frame ends later is not enough");
    CHECK(!klvk_retire_due(0, -1, 0.05, 103, 100, 100, AFTER, 0, 0),
          "3 frame ends but the GPU has not completed the frame in progress at retirement (serial 100 < 101)");
    CHECK(klvk_retire_due(0, -1, 0.05, 103, 100, 101, AFTER, 0, 0), "3 frame ends and the GPU done: freed in 50 ms, not 5 s");
    CHECK(klvk_retire_due(0, -1, 0.05, 103, 100, 103, AFTER, 0, 0), "...and with the GPU further ahead");
    // A guest that ends no frames (synchronous load): wall clock only, the 4x grace.
    CHECK(!klvk_retire_due(0, -1, 5.5, 100, 100, 100, AFTER, 0, 0), "no frame ends: 5 s is not enough any more than 0.5 s");
    CHECK(klvk_retire_due(0, -1, 20.0, 100, 100, 100, AFTER, 0, 0), "no frame ends: 4x the grace frees it");
    // Memory pressure / trim: settled (3 frames and GPU, or 0.5 s).
    CHECK(!klvk_retire_due(0, -1, 0.2, 100, 100, 100, AFTER, 1, 0), "force_quick: not before settled");
    CHECK(klvk_retire_due(0, -1, 0.6, 100, 100, 100, AFTER, 1, 0), "force_quick: 0.5 s on the wall clock");
    CHECK(klvk_retire_due(0, -1, 0.6, 100, 100, 100, AFTER, 0, 1), "trimming window: the same");
    CHECK(klvk_retire_due(0, -1, 0.05, 104, 100, 101, AFTER, 1, 0), "force_quick: 3 frames and the GPU");

    // SpaceWarp image (quick 2): GPU done, or 2 s; force_quick at once.
    CHECK(!klvk_retire_due(2, -1, 0.1, 101, 100, 101, AFTER, 0, 0), "asw: 1 frame is not enough");
    CHECK(klvk_retire_due(2, -1, 0.1, 103, 100, 101, AFTER, 0, 0), "asw: 3 frames and the GPU done, no 0.5 s floor");
    CHECK(!klvk_retire_due(2, -1, 1.9, 100, 100, 100, AFTER, 0, 0), "asw: no frame ends, 1.9 s");
    CHECK(klvk_retire_due(2, -1, 2.0, 100, 100, 100, AFTER, 0, 0), "asw: no frame ends, 2 s");
    CHECK(klvk_retire_due(2, -1, 0.0, 100, 100, 100, AFTER, 1, 0), "asw: force_quick");

    // quick 1 (destroyed OpenXR image, never-published eye image): 0.5 s, or force_quick.
    CHECK(!klvk_retire_due(1, -1, 0.4, 200, 100, 200, AFTER, 0, 0), "quick: 0.4 s");
    CHECK(klvk_retire_due(1, -1, 0.5, 100, 100, 100, AFTER, 0, 0), "quick: 0.5 s");
    CHECK(klvk_retire_due(1, -1, 0.0, 100, 100, 100, AFTER, 1, 0), "quick: force_quick");

    // Replaced eye image: held (replacement not published) stays; published goes when settled.
    CHECK(!klvk_retire_due(0, 1, 3.0, 500, 100, 500, AFTER, 0, 0), "eye held: stays while the replacement is unpublished");
    CHECK(!klvk_retire_due(0, 1, 19.9, 500, 100, 500, AFTER, 0, 0), "eye held: still, just short of 4x the grace");
    CHECK(klvk_retire_due(0, 1, 20.0, 500, 100, 500, AFTER, 0, 0), "eye held: 4x the grace is the ceiling");
    CHECK(!klvk_retire_due(0, 0, 0.1, 101, 100, 101, AFTER, 0, 0), "eye published: not before settled");
    CHECK(klvk_retire_due(0, 0, 0.1, 103, 100, 101, AFTER, 0, 0), "eye published: 3 frames and the GPU");
    CHECK(klvk_retire_due(0, 0, 0.5, 100, 100, 100, AFTER, 0, 0), "eye published: 0.5 s with no frame ends");

    // Touched again by the guest (retire_touch moves `frame` forward): the count restarts from there.
    CHECK(!klvk_retire_due(0, -1, 0.05, 110, 108, 110, AFTER, 0, 0), "touched in frame 108: 2 frames are not enough");
    CHECK(klvk_retire_due(0, -1, 0.05, 111, 108, 109, AFTER, 0, 0), "touched in frame 108: 3 frames and the GPU done");
    return fails ? 1 : (printf("t_retire_due ok\n"), 0);
}
