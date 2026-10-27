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

    // Enqueue route (Unity): EndFrame4 carries the eye layer only (list_idx -1).
    struct klovrp_pt_enq s = { 0 };
    int id, under;
    // Initialised + shape 7 enqueued, flags 0x4 (no OnTop): passthrough ON, underlay.
    klovrp_pt_enq_note(&s, 1, 0, 7, 4, 0x4);
    CHECK(klovrp_pt_enq_frame(&s, -1, 0, 0, &id, &under) == 1 && id == 4 && under == 1, "enqueued shape 7 -> on, underlay");
    // Re-enqueued every frame: stays on far beyond the idle limit.
    for (int i = 0; i < 20; i++) {
        klovrp_pt_enq_note(&s, 1, 0, 7, 4, 0x4);
        CHECK(klovrp_pt_enq_frame(&s, -1, 0, 0, &id, &under) == 1, "re-enqueued every frame stays on");
    }
    // OnTop flag: overlay.
    klovrp_pt_enq_note(&s, 1, 0, 7, 4, 0x5);
    CHECK(klovrp_pt_enq_frame(&s, -1, 0, 0, &id, &under) == 1 && under == 0, "OnTop -> overlay");
    // No re-enqueue: off after KLOVRP_PT_ENQ_IDLE EndFrame4s.
    int still = 1;
    for (int i = 0; i < KLOVRP_PT_ENQ_IDLE; i++) still = klovrp_pt_enq_frame(&s, -1, 0, 0, &id, &under);
    CHECK(still == 0 && id == 0, "no re-enqueue -> off");
    // Not initialised: never on.
    struct klovrp_pt_enq u = { 0 };
    klovrp_pt_enq_note(&u, 0, 0, 7, 4, 0x4);
    CHECK(klovrp_pt_enq_frame(&u, -1, 0, 0, &id, &under) == 0, "enqueued but uninitialised -> off");
    // Eye layer, or a non-passthrough shape, enqueued: off.
    klovrp_pt_enq_note(&u, 1, 1, 7, 4, 0x4);
    klovrp_pt_enq_note(&u, 1, 0, 0, 5, 0x4);
    CHECK(klovrp_pt_enq_frame(&u, -1, 0, 0, &id, &under) == 0, "eye / plain quad enqueued -> off");
    // Destroy of another layer keeps it on; destroy of its own turns it off at once.
    klovrp_pt_enq_note(&u, 1, 0, 8, 6, 0x4);
    klovrp_pt_enq_destroy(&u, 9);
    CHECK(klovrp_pt_enq_frame(&u, -1, 0, 0, &id, &under) == 1 && id == 6, "other layer destroyed -> still on");
    klovrp_pt_enq_destroy(&u, 6);
    CHECK(klovrp_pt_enq_frame(&u, -1, 0, 0, &id, &under) == 0, "destroy -> off");
    // The EndFrame4 list still works on its own and wins: eye 1, passthrough 0 -> underlay; after -> overlay.
    struct klovrp_pt_enq z = { 0 };
    CHECK(klovrp_pt_enq_frame(&z, 0, 7, 1, &id, &under) == 1 && id == 7 && under == 1, "list underlay");
    CHECK(klovrp_pt_enq_frame(&z, 1, 7, 0, &id, &under) == 1 && under == 0, "list overlay");
    CHECK(klovrp_pt_enq_frame(&z, -1, 0, 0, &id, &under) == 0, "empty list, nothing enqueued -> off");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
