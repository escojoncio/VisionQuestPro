// Host test of kl_ovrp.c's KL-TEST ptscan block (cut out at build time): which submitted layers make
// a frame a passthrough frame. Only a passthrough-shaped (OVRPlugin enum 7, 8, 10, 11) non-eye layer
// counts, and only once InsightPassthrough is initialised.
#include <stdio.h>
#include "ptscan.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    int eye;
    struct klovrp_pt_item a[3];

    // Every passthrough shape counts, the others do not.
    int pt[] = { 7, 8, 10, 11 }, no[] = { 0, 1, 2, 3, 4, 5, 6, 9, 12, -1 };
    for (unsigned i = 0; i < sizeof pt / sizeof *pt; i++) CHECK(klovrp_shape_is_passthrough(pt[i]), "passthrough shape counted");
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++) CHECK(!klovrp_shape_is_passthrough(no[i]), "non-passthrough shape ignored");

    // SurfaceProjectedPassthrough (8) before the eye layer: underlay, ON.
    a[0] = (struct klovrp_pt_item){ 0, 8 };
    a[1] = (struct klovrp_pt_item){ 1, 3 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == 0 && eye == 1, "shape 8 underlay");
    // Not initialised: not shown.
    CHECK(klovrp_pt_decide(a, 2, 0, &eye) == -1, "uninitialised -> off");
    // Eye layer only: off.
    a[0] = (struct klovrp_pt_item){ 1, 3 };
    CHECK(klovrp_pt_decide(a, 1, 1, &eye) == -1 && eye == 0, "eye only -> off");
    // A non-passthrough quad before the eye layer does not turn it on.
    a[0] = (struct klovrp_pt_item){ 0, 0 };
    a[1] = (struct klovrp_pt_item){ 1, 3 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == -1 && eye == 1, "plain quad -> off");
    // A passthrough layer after the eye layer: overlay, index 1.
    a[0] = (struct klovrp_pt_item){ 1, 3 };
    a[1] = (struct klovrp_pt_item){ 0, 7 };
    CHECK(klovrp_pt_decide(a, 2, 1, &eye) == 1 && eye == 0, "shape 7 overlay");
    // A passthrough-shaped layer flagged as the eye layer never counts; an empty list (paused layers are absent) is off.
    a[0] = (struct klovrp_pt_item){ 1, 8 };
    CHECK(klovrp_pt_decide(a, 1, 1, &eye) == -1, "eye layer shape ignored");
    CHECK(klovrp_pt_decide(a, 0, 1, &eye) == -1 && eye == -1, "empty list");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
