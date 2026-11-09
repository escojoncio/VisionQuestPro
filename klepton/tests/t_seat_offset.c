// Host test of kl_ovrp.c's KL-TEST seatoff block (cut out at build time): the "Sentado" offset math.
// Seated + a floor/stage origin: the guest sees every Y raised by max(0, standing height - measured eye
// height); klovrp_guest_origin_calc returns it NEGATED, because callers subtract the origin (guest =
// display - origin) and the compositor adds it back (display = guest + origin) — one number for both
// directions, so the picture and the reprojection cannot disagree. Eye level (0) is unchanged.
#include <math.h>
#include <stdio.h>
#include "seatoff.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-5f)

int main(void) {
    // Seated on, floor (1) or stage (2): standing height minus the measured one.
    CHECK(NEAR(klovrp_seat_offset_calc(1, 1, 1.65f, 0.992f), 0.658f), "floor: 1.65 - 0.992");
    CHECK(NEAR(klovrp_seat_offset_calc(1, 2, 1.65f, 0.992f), 0.658f), "stage: same");
    // Off, or eye level / unknown origin: nothing.
    CHECK(klovrp_seat_offset_calc(0, 1, 1.65f, 0.992f) == 0.0f, "seated off");
    CHECK(klovrp_seat_offset_calc(1, 0, 1.65f, 0.992f) == 0.0f, "eye level is untouched");
    CHECK(klovrp_seat_offset_calc(1, 4, 1.65f, 0.992f) == 0.0f, "view/other origin is untouched");
    // Never lowers: a user already taller than the standing height gets 0.
    CHECK(klovrp_seat_offset_calc(1, 1, 1.65f, 1.70f) == 0.0f, "measured above standing -> 0");
    CHECK(klovrp_seat_offset_calc(1, 1, 1.65f, 1.65f) == 0.0f, "equal -> 0");
    // Not measured yet (0), or garbage: no offset (a default height must not be mistaken for a measurement).
    CHECK(klovrp_seat_offset_calc(1, 1, 1.65f, 0.0f) == 0.0f, "unmeasured -> 0");
    CHECK(klovrp_seat_offset_calc(1, 1, 1.65f, NAN) == 0.0f, "NaN measured -> 0");
    CHECK(klovrp_seat_offset_calc(1, 1, NAN, 0.99f) == 0.0f, "NaN standing -> 0");
    // Re-latched eye height (posture change / recenter) recomputes: standing up shrinks it.
    CHECK(NEAR(klovrp_seat_offset_calc(1, 1, 1.65f, 1.30f), 0.35f), "re-latched 1.30");

    // The origin the guest-space conversion uses (guest_y = display_y - origin).
    float off = klovrp_seat_offset_calc(1, 1, 1.65f, 0.992f);
    // Floor + seated: origin is -offset, so the guest's head is display 0.992 + 0.658 = 1.65.
    float o = klovrp_guest_origin_calc(1, 1, 1, 0.992f, off);
    CHECK(NEAR(o, -0.658f), "floor seated: origin = -offset");
    CHECK(NEAR(0.992f - o, 1.65f), "guest head Y = standing height");
    CHECK(NEAR((0.992f - o) + o, 0.992f), "compositor adds the origin back: display Y again (no double offset)");
    // Floor, seated off: 0, as before.
    CHECK(klovrp_guest_origin_calc(1, 1, 1, 0.992f, 0.0f) == 0.0f, "floor, not seated: 0 as before");
    CHECK(klovrp_guest_origin_calc(2, 0, 1, 0.992f, 0.0f) == 0.0f, "stage, not seated: 0 as before");
    // Eye level keeps today's behaviour: the measured height once a head exists, 0 before / when switched off.
    CHECK(NEAR(klovrp_guest_origin_calc(0, 1, 1, 0.992f, 0.0f), 0.992f), "eye level: measured height");
    CHECK(klovrp_guest_origin_calc(0, 1, 0, 0.992f, 0.0f) == 0.0f, "eye level, no head yet: 0");
    CHECK(klovrp_guest_origin_calc(0, 0, 1, 0.992f, 0.0f) == 0.0f, "eye level, KL_OVRP_EYELEVEL_ZERO=0: 0");
    // An origin outside 0/1/2 (4 = view) is left alone.
    CHECK(klovrp_guest_origin_calc(4, 1, 1, 0.992f, 0.0f) == 0.0f, "other origin: 0");

    // A guardian floor at gy (world frame): the raise is measured over it, since the origin adds gy (no double count).
    {
        float gy = 0.1f, head = 1.0f;                      // head 1.0 over the world origin, floor 0.1 above it
        float m = klovrp_seat_measured(head, 0, 1, gy);
        CHECK(NEAR(m, 0.9f), "measured over the guardian floor");
        float so = klovrp_seat_offset_calc(1, 1, 1.65f, m);
        float org = klovrp_guest_origin_calc(1, 1, 1, 0.0f, so) + gy;     // klovrp_guest_origin_y
        CHECK(NEAR(head - org, 1.65f), "guest head Y = standing height");
        // the old way (raise measured over the world origin, gy still added to the origin) lands short by gy
        float old = klovrp_seat_offset_calc(1, 1, 1.65f, head), org_old = klovrp_guest_origin_calc(1, 1, 1, 0.0f, old) + gy;
        CHECK(NEAR(head - org_old, 1.65f - gy), "old: standing height missed by gy");
        CHECK(NEAR(klovrp_seat_measured(head, 0, 0, gy), head), "no floor: unchanged");
        CHECK(NEAR(klovrp_seat_measured(head, 1, 1, gy), head), "eye-height override: already over the floor");
        CHECK(klovrp_seat_measured(0.0f, 0, 1, gy) == 0.0f, "unmeasured stays 0");
    }

    // The stand height setting: default and sanity range.
    CHECK(NEAR(klovrp_stand_height_clamp(1.65f), 1.65f), "stand height kept");
    CHECK(NEAR(klovrp_stand_height_clamp(0.0f), 1.65f), "0 -> default");
    CHECK(NEAR(klovrp_stand_height_clamp(NAN), 1.65f), "NaN -> default");
    CHECK(NEAR(klovrp_stand_height_clamp(5.0f), 1.65f), "absurd -> default");
    CHECK(NEAR(klovrp_stand_height_clamp(1.40f), 1.40f) && NEAR(klovrp_stand_height_clamp(2.00f), 2.00f), "app range kept");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
