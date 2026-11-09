// Eye height over the guardian floor (kl_ovrp.c KL-TEST flooreye): what ovrp_GetUserEyeHeight2 and the EyeLevel <-> floor
// relative pose answer. The head is measured over the visionOS world origin; a floor the user placed elsewhere moves it.
// clang -Wall -Wno-unused-function -I<dir with flooreye.inc> tests/t_floor_eye.c -lm
#include <math.h>
#include <stdio.h>
#include "flooreye.inc"

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) <= 1e-5f)

int main(void) {
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 0, 0.0f), 1.6f));      // no guardian floor: as before
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, 0.0f), 1.6f));      // floor at the world origin: same
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, -0.2f), 1.8f));     // floor 20 cm below the origin: the eye is higher over it
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, 0.3f), 1.3f));      // floor raised: lower over it
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 1, 1, -0.2f), 1.6f));     // KL_OVRP_EYE_HEIGHT is already "over the floor"
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 0, 0, 1, -0.2f), 1.6f));     // not measured yet: the default is not a head height
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, 1.5f), 1.6f));      // floor almost at the head: implausible, keep the measure
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, -5.0f), 1.6f));
    CK(NEAR(klovrp_eye_over_floor_calc(1.6f, 1, 0, 1, NAN), 1.6f));
    CK(NEAR(klovrp_eye_over_floor_calc(0.0f, 1, 0, 1, -0.2f), 0.0f));
    printf(fails ? "t_floor_eye: %d FAILED\n" : "t_floor_eye: all passed\n", fails);
    return fails != 0;
}
