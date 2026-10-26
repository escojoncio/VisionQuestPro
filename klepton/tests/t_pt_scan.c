// Host test of kl_ovrp.c's KL-TEST ptscan block (cut out at build time): which submitted layers make
// a frame a passthrough frame. Gundam submits an ovrpShape_SurfaceProjectedPassthrough (8) layer and
// never got "[pt] passthrough ON": only 6/7 were counted. An eye/projection layer carrying an
// InsightPassthroughStyle is a passthrough underlay with no passthrough shape.
#include <stdio.h>
#include "ptscan.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    int eye;
    struct klovrp_pt_item a[3];

    // Every passthrough shape counts, the others do not.
    int pt[] = { 6, 7, 8, 10, 11 }, no[] = { 0, 1, 2, 3, 4, 5, 9, 12, -1 };
    for (unsigned i = 0; i < sizeof pt / sizeof *pt; i++) CHECK(klovrp_shape_is_passthrough(pt[i]), "passthrough shape counted");
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++) CHECK(!klovrp_shape_is_passthrough(no[i]), "non-passthrough shape ignored");

    // SurfaceProjectedPassthrough (8) before the eye layer: underlay, ON.
    a[0] = (struct klovrp_pt_item){ 0, 8, 0 };
    a[1] = (struct klovrp_pt_item){ 1, 3, 0 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == 0 && eye == 1, "shape 8 underlay");
    // Not initialised: not shown.
    CHECK(klovrp_pt_decide(a, 2, 0, &eye) == -1, "uninitialised -> off");
    // Eye layer only: off.
    a[0] = (struct klovrp_pt_item){ 1, 3, 0 };
    CHECK(klovrp_pt_decide(a, 1, 1, &eye) == -1 && eye == 0, "eye only -> off");
    // Eye layer with a passthrough style, initialised: ON on that layer.
    a[0] = (struct klovrp_pt_item){ 1, 3, 1 };
    CHECK(klovrp_pt_decide(a, 1, 1, &eye) == 0, "styled eye layer -> on");
    CHECK(klovrp_pt_decide(a, 1, 0, &eye) == -1, "styled but uninitialised -> off");
    // Styled quad before the eye layer.
    a[0] = (struct klovrp_pt_item){ 0, 0, 1 };
    a[1] = (struct klovrp_pt_item){ 1, 3, 0 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == 0 && eye == 1, "styled projection underlay");
    // A shaped passthrough layer wins over a later styled layer.
    a[0] = (struct klovrp_pt_item){ 1, 3, 1 };
    a[1] = (struct klovrp_pt_item){ 0, 7, 0 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == 1, "shaped layer wins");
    // The eye layer's own shape (3) never counts as passthrough; a styled layer that is paused (absent) neither.
    CHECK(klovrp_pt_decide(a, 0, 1, &eye) == -1 && eye == -1, "empty list");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
