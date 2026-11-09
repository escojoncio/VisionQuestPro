// Host test of the Guardian core (kl_guardian.c): floor rules, manual floor nudge, inscribed play-space rectangle, point cap,
// room V1 / roombox rules, generation, and the unconfigured state (kl_ovrp.c then keeps its old 3x3 / env answers).
// gcc -Wall -I. tests/t_guardian.c kl_guardian.c -lm -lpthread
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "kl_guardian.h"

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

static void rect(float *p, float w, float d) { float q[8] = {0, 0, w, 0, w, d, 0, d}; memcpy(p, q, sizeof q); }

int main(void) {
    float y, w, d;
    int circle;

    // unconfigured: nothing set, kl_ovrp.c takes its own path
    CK(!kl_guardian_configured());
    CK(kl_guardian_get_floor(&y) == 0 && y == 0.0f);
    CK(!kl_guardian_effective(&circle, &w, &d));
    CK(kl_guardian_get_boundary(NULL, 0) == 0);
    CK(!kl_guardian_playspace_ok());
    CK(kl_guardian_boundary_source() == 0);

    // floor plausibility, edges inclusive
    CK(kl_guardian_floor_plausible(-1.5f, 0.0f, 1));
    CK(kl_guardian_floor_plausible(-2.5f, 0.0f, 1));
    CK(!kl_guardian_floor_plausible(-2.51f, 0.0f, 1));
    CK(kl_guardian_floor_plausible(0.3f, 1.0f, 1));            // -0.7
    CK(!kl_guardian_floor_plausible(0.4f, 1.0f, 1));           // -0.6: SLAM too close
    CK(kl_guardian_floor_plausible(0.4f, 1.0f, 0));            // anchor accepts it
    CK(kl_guardian_floor_plausible(1.0f, 1.0f, 0));            // 0
    CK(!kl_guardian_floor_plausible(1.01f, 1.0f, 0));
    CK(!kl_guardian_floor_plausible(NAN, 1.0f, 0));

    // set / get floor, generation
    uint32_t g0 = kl_guardian_generation();
    CK(kl_guardian_set_floor(-0.2f, KLG_FLOOR_AUTO) == 1);
    CK(kl_guardian_generation() == g0 + 1);
    CK(kl_guardian_set_floor(-0.2f, KLG_FLOOR_AUTO) == 0 && kl_guardian_generation() == g0 + 1);
    CK(kl_guardian_get_floor(&y) == KLG_FLOOR_AUTO && y == -0.2f);
    CK(kl_guardian_set_floor(NAN, KLG_FLOOR_MANUAL) == 0);
    CK(isnan(kl_guardian_get_user_preferred_floor()));
    // |y| > 10 m is rejected; AUTO ignores moves under 1 cm (no generation bump), MANUAL always applies
    CK(kl_guardian_set_floor(10.5f, KLG_FLOOR_AUTO) == 0 && kl_guardian_set_floor(-10.5f, KLG_FLOOR_MANUAL) == 0);
    CK(kl_guardian_set_floor(3e38f, KLG_FLOOR_AUTO) == 0 && kl_guardian_get_floor(&y) == KLG_FLOOR_AUTO && y == -0.2f);
    uint32_t gh = kl_guardian_generation();
    CK(kl_guardian_set_floor(-0.205f, KLG_FLOOR_AUTO) == 0 && kl_guardian_generation() == gh);
    CK(kl_guardian_get_floor(&y) == KLG_FLOOR_AUTO && y == -0.2f);
    CK(kl_guardian_set_floor(-0.215f, KLG_FLOOR_AUTO) == 1 && kl_guardian_generation() == gh + 1);
    CK(kl_guardian_set_floor(-0.2151f, KLG_FLOOR_MANUAL) == 1 && kl_guardian_get_floor(&y) == KLG_FLOOR_MANUAL);
    CK(kl_guardian_set_floor(-0.2f, KLG_FLOOR_AUTO) == 0 && kl_guardian_get_floor(&y) == KLG_FLOOR_MANUAL && y == -0.2151f);   // AUTO never replaces MANUAL
    CK(kl_guardian_set_floor(-0.2f, KLG_FLOOR_MANUAL) == 1);
    CK(kl_guardian_set_floor(0.0f, KLG_FLOOR_NONE) == 1 && kl_guardian_get_floor(&y) == 0);   // clear, then AUTO is accepted again
    CK(kl_guardian_set_floor(-0.2f, KLG_FLOOR_AUTO) == 1);             // back to the state the next block expects

    // nudge: 6 mm, dead zone, +-1 m around the controller, 0.1524 under the head, lowering to the hand
    kl_guardian_set_floor(0.0f, KLG_FLOOR_AUTO);
    float f = kl_guardian_floor_nudge(1.0f, 1.7f, 0.5f);               // hand 0.5 above the floor
    CK(NEAR(f, 0.006f, 1e-6f));
    f = kl_guardian_floor_nudge(-1.0f, 1.7f, 0.5f);
    CK(NEAR(f, 0.0f, 1e-6f));
    f = kl_guardian_floor_nudge(0.05f, 1.7f, 0.5f);                   // inside the dead zone
    CK(NEAR(f, 0.0f, 1e-6f));
    CK(kl_guardian_get_floor(&y) == KLG_FLOOR_MANUAL);
    f = kl_guardian_floor_nudge(1.0f, 1.7f, 0.0f);                    // hand at the floor: up 6 mm ...
    CK(NEAR(f, 0.006f, 1e-6f));
    f = kl_guardian_floor_nudge(1.0f, 1.7f, 0.0f);                    // ... and the hand pulls it back down first
    CK(NEAR(f, 0.006f, 1e-6f));
    kl_guardian_set_floor(-0.9995f, KLG_FLOOR_MANUAL);
    f = kl_guardian_floor_nudge(-1.0f, 3.0f, 0.0f);
    CK(NEAR(f, -1.0f, 1e-6f));
    kl_guardian_set_floor(1.5f, KLG_FLOOR_MANUAL);                    // head cap: floor <= head - 0.1524
    f = kl_guardian_floor_nudge(1.0f, 1.6f, 1.5f);
    CK(NEAR(f, 1.6f - 0.1524f, 1e-5f));
    kl_guardian_set_floor(0.0f, KLG_FLOOR_MANUAL);                    // hand under the floor pulls it down
    f = kl_guardian_floor_nudge(0.0f, 1.7f, -0.3f);
    CK(NEAR(f, -0.3f, 1e-6f));
    CK(NEAR(kl_guardian_floor_lower_to(-0.5f), -0.5f, 1e-6f));
    CK(NEAR(kl_guardian_floor_lower_to(0.2f), -0.5f, 1e-6f));         // never raises
    kl_guardian_set_floor(0.5f, KLG_FLOOR_MANUAL);
    CK(NEAR(kl_guardian_floor_nudge(1.0f, 3e38f, 3e38f), 0.5f, 1e-6f));   // absurd head height: floor stays within +-10 m
    CK(NEAR(kl_guardian_floor_lower_to(-1e30f), 0.5f, 1e-6f));
    kl_guardian_set_floor(0.0f, KLG_FLOOR_NONE);                      // no floor: starts at head - 1.15
    f = kl_guardian_floor_nudge(0.0f, 1.7f, NAN);
    CK(NEAR(f, 0.55f, 1e-5f));
    kl_guardian_floor_confirm();
    CK(NEAR(kl_guardian_get_user_preferred_floor(), 0.55f, 1e-5f));
    kl_guardian_floor_nudge(1.0f, 1.7f, NAN);
    CK(NEAR(kl_guardian_get_user_preferred_floor(), 0.55f, 1e-5f));   // the live floor moved, the confirmed one did not
    kl_guardian_set_floor(0.0f, KLG_FLOOR_NONE);

    // inscribed rectangle
    float p[8];
    rect(p, 1.1f, 1.1f);
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 0);
    CK(!kl_guardian_playspace_ok());
    CK(kl_guardian_effective(&circle, &w, &d) && circle && NEAR(w, 2.0f, 1e-5f));    // too small: stationary circle
    rect(p, 1.2f, 1.2f);
    uint32_t g1 = kl_guardian_generation();
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 1);
    CK(kl_guardian_generation() > g1);
    CK(kl_guardian_playspace_ok());
    CK(kl_guardian_effective(&circle, &w, &d) && !circle && NEAR(w, 1.2f, 0.05f) && NEAR(d, 1.2f, 0.05f));
    rect(p, 3.0f, 2.0f);
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 1);
    CK(kl_guardian_playspace(&w, &d, NULL, NULL, NULL) && NEAR(fmaxf(w, d), 3.0f, 0.05f) && NEAR(fminf(w, d), 2.0f, 0.05f));
    // clockwise input is normalised
    float cw[8] = {0, 0, 0, 2, 3, 2, 3, 0};
    CK(kl_guardian_set_boundary(cw, 4, KLG_MODE_ROOMSCALE, 0) == 1);
    float out[16];
    CK(kl_guardian_get_boundary(out, 4) == 4);
    float ar = 0;
    for (int k = 0; k < 4; k++) { int m = (k + 1) % 4; ar += out[2 * k] * out[2 * m + 1] - out[2 * m] * out[2 * k + 1]; }
    CK(ar > 0);
    // polygon test: inside positive, outside negative
    float dist, px, pz, nx, nz;
    CK(kl_guardian_polygon_test(1.5f, 1.0f, &dist, &px, &pz, &nx, &nz) && NEAR(dist, 1.0f, 1e-4f));
    CK(kl_guardian_polygon_test(4.0f, 1.0f, &dist, &px, &pz, &nx, &nz) && NEAR(dist, -1.0f, 1e-4f) && NEAR(px, 3.0f, 1e-4f));
    // PlayArea (inscribed rectangle) apart from OuterBoundary (polygon): L shape 3 x 3 with a 1.3 m arm
    {
        float l3[12] = {0, 0, 3, 0, 3, 1.3f, 1.3f, 1.3f, 1.3f, 3, 0, 3}, cs[8], ow, od;
        CK(kl_guardian_set_boundary(l3, 6, KLG_MODE_ROOMSCALE, 0) == 1);
        CK(kl_guardian_outer_size(&ow, &od) && NEAR(ow, 3.0f, 1e-5f) && NEAR(od, 3.0f, 1e-5f));
        CK(kl_guardian_playspace(&w, &d, NULL, NULL, NULL) && (w < 2.9f || d < 2.9f));
        CK(kl_guardian_playarea_corners(cs));
        float cw2 = 0, cd2 = 0, ccx = 0, ccz = 0;
        for (int k = 0; k < 4; k++) { ccx += cs[2 * k] * 0.25f; ccz += cs[2 * k + 1] * 0.25f; }
        float e0 = hypotf(cs[2] - cs[0], cs[3] - cs[1]), e1 = hypotf(cs[4] - cs[2], cs[5] - cs[3]);
        float pw, pd, pcx, pcz;
        kl_guardian_playspace(&pw, &pd, &pcx, &pcz, NULL);
        cw2 = e0; cd2 = e1;
        CK(NEAR(cw2, pw, 1e-3f) && NEAR(cd2, pd, 1e-3f) && NEAR(ccx, pcx, 1e-3f) && NEAR(ccz, pcz, 1e-3f));
        float ar2 = 0;
        for (int k = 0; k < 4; k++) { int m = (k + 1) % 4; ar2 += cs[2 * k] * cs[2 * m + 1] - cs[2 * m] * cs[2 * k + 1]; }
        CK(ar2 > 0);
        // the arm tip (0.5, 2.8) is inside the polygon but outside the play area; the centre of the play area is inside both
        float dd, qx, qz, nx2, nz2;
        CK(kl_guardian_polygon_test(0.65f, 2.8f, &dd, &qx, &qz, &nx2, &nz2) && dd > 0);
        CK(kl_guardian_playarea_test(0.65f, 2.8f, &dd, &qx, &qz, &nx2, &nz2) && dd < 0);
        CK(kl_guardian_playarea_test(pcx, pcz, &dd, &qx, &qz, &nx2, &nz2) && NEAR(dd, fminf(pw, pd) * 0.5f, 1e-3f));
        CK(NEAR(nx2 * nx2 + nz2 * nz2, 1.0f, 1e-4f));
        kl_guardian_set_boundary(NULL, 0, KLG_MODE_STATIONARY, 1.0f);   // circle: no rectangle, no outer polygon
        CK(!kl_guardian_playarea_corners(cs) && !kl_guardian_playarea_test(0, 0, &dd, &qx, &qz, &nx2, &nz2) && !kl_guardian_outer_size(&ow, &od));
    }
    // L shapes: arm 1.0 -> no; arm 1.3 -> yes
    float l1[12] = {0, 0, 3, 0, 3, 1, 1, 1, 1, 3, 0, 3};
    CK(kl_guardian_set_boundary(l1, 6, KLG_MODE_ROOMSCALE, 0) == 0);
    float l2[12] = {0, 0, 3, 0, 3, 1.3f, 1.3f, 1.3f, 1.3f, 3, 0, 3};
    CK(kl_guardian_set_boundary(l2, 6, KLG_MODE_ROOMSCALE, 0) == 1);
    CK(kl_guardian_playspace(&w, &d, NULL, NULL, NULL) && fminf(w, d) >= 1.25f);
    // a rectangle turned 30 degrees is found at its own angle
    float ang = 0.5235988f, c = cosf(ang), s = sinf(ang), tr[8], bx[8] = {0, 0, 2.4f, 0, 2.4f, 1.6f, 0, 1.6f};
    for (int k = 0; k < 4; k++) { tr[2 * k] = c * bx[2 * k] - s * bx[2 * k + 1]; tr[2 * k + 1] = s * bx[2 * k] + c * bx[2 * k + 1]; }
    CK(kl_guardian_set_boundary(tr, 4, KLG_MODE_ROOMSCALE, 0) == 1);
    CK(kl_guardian_playspace(&w, &d, NULL, NULL, NULL) && NEAR(fmaxf(w, d), 2.4f, 0.1f) && NEAR(fminf(w, d), 1.6f, 0.1f));

    // limit case: exactly 1.15 x 1.15 is valid (cell-count slack), 1.10 is not
    rect(p, 1.15f, 1.15f);
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 1 && kl_guardian_playspace_ok());
    rect(p, 1.10f, 1.10f);
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 0 && !kl_guardian_playspace_ok());
    rect(p, 1.2f, 1.2f);
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) == 1);

    // huge finite coordinates (extent overflows to inf), NaN, inf: rejected, state kept
    uint32_t gb = kl_guardian_generation();
    float huge[8] = {-3e38f, -3e38f, 3e38f, -3e38f, 3e38f, 3e38f, -3e38f, 3e38f};
    CK(kl_guardian_set_boundary(huge, 4, KLG_MODE_ROOMSCALE, 0) == -1);
    float far4[8] = {0, 0, 20000, 0, 20000, 20000, 0, 20000};
    CK(kl_guardian_set_boundary(far4, 4, KLG_MODE_ROOMSCALE, 0) == -1);
    float nanp[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
    CK(kl_guardian_set_boundary(nanp, 4, KLG_MODE_ROOMSCALE, 0) == -1);
    CK(kl_guardian_default_from_room(huge, 4) == 0);
    CK(kl_guardian_generation() == gb);

    // invalid input keeps the state
    uint32_t g2 = kl_guardian_generation();
    float bad[8] = {0, 0, NAN, 0, 1, 1, 0, 1};
    CK(kl_guardian_set_boundary(bad, 4, KLG_MODE_ROOMSCALE, 0) == -1);
    float line[6] = {0, 0, 1, 0, 2, 0};
    CK(kl_guardian_set_boundary(line, 3, KLG_MODE_ROOMSCALE, 0) == -1);
    CK(kl_guardian_set_boundary(p, 2, KLG_MODE_ROOMSCALE, 0) == -1);
    CK(kl_guardian_set_boundary(p, 4, 7, 0) == -1);
    CK(kl_guardian_generation() == g2);

    // more than 4096 points: clipped to 4096 (a 5000-point circle of 2 m radius)
    static float big[5000 * 2], back[4200 * 2];
    for (int k = 0; k < 5000; k++) { float a = 6.2831853f * (float)k / 5000.0f; big[2 * k] = 2.0f * cosf(a); big[2 * k + 1] = 2.0f * sinf(a); }
    int r = kl_guardian_set_boundary(big, 5000, KLG_MODE_ROOMSCALE, 0);
    CK(r >= 0);
    CK(kl_guardian_get_boundary(NULL, 0) == 4096);
    CK(kl_guardian_get_boundary(back, 4200) == 4096);
    CK(kl_guardian_get_boundary(back, 10) == 10);
    CK(r == 1 && kl_guardian_playspace(&w, &d, NULL, NULL, NULL) && w > 2.0f && d > 2.0f);
    // uniform subsampling, not truncation: a half disc of 5000 points (angle 0..pi) keeps its far end (x = -2) in the 4096
    for (int k = 0; k < 5000; k++) { float a = 3.1415927f * (float)k / 5000.0f; big[2 * k] = 2.0f * cosf(a); big[2 * k + 1] = 2.0f * sinf(a); }
    CK(kl_guardian_set_boundary(big, 5000, KLG_MODE_ROOMSCALE, 0) == 1 && kl_guardian_get_boundary(back, 4200) == 4096);
    {
        float mnx = 9;
        for (int k = 0; k < 4096; k++) mnx = fminf(mnx, back[2 * k]);
        CK(mnx < -1.99f);
    }

    // stationary: circle, radius kept, polygon dropped
    CK(kl_guardian_set_boundary(NULL, 0, KLG_MODE_STATIONARY, 1.5f) == 1);
    CK(kl_guardian_effective(&circle, &w, &d) && circle && NEAR(w, 3.0f, 1e-5f) && NEAR(d, 3.0f, 1e-5f));
    CK(kl_guardian_get_boundary(NULL, 0) == 0 && !kl_guardian_polygon_test(0, 0, &dist, &px, &pz, &nx, &nz));
    // a room outline only fills in when the user set none
    rect(p, 3.0f, 3.0f);
    CK(kl_guardian_default_from_room(p, 4) == 0);
    kl_guardian_clear();
    CK(!kl_guardian_configured());
    CK(kl_guardian_default_from_room(p, 4) == 1 && kl_guardian_boundary_source() == KLG_BSRC_ROOM);
    rect(p, 1.0f, 1.0f);
    CK(kl_guardian_default_from_room(p, 4) == 0 && kl_guardian_effective(&circle, &w, &d) && !circle && NEAR(w, 3.0f, 0.05f));
    kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0);            // user boundary (too small -> circle) now wins
    rect(p, 3.0f, 3.0f);
    CK(kl_guardian_default_from_room(p, 4) == 0 && kl_guardian_boundary_source() == KLG_BSRC_USER);
    kl_guardian_clear();
    CK(!kl_guardian_effective(&circle, &w, &d));

    // room V1
    CK(!kl_guardian_room_v1_valid(2, 1, 1, 0.0f, 2.5f));
    CK(kl_guardian_room_v1_valid(3, 1, 1, 0.0f, 2.5f));
    CK(!kl_guardian_room_v1_valid(3, 0, 1, 0.0f, 2.5f));
    CK(!kl_guardian_room_v1_valid(3, 1, 0, 0.0f, 2.5f));
    CK(!kl_guardian_room_v1_valid(4, 1, 1, 2.5f, 2.5f));
    CK(!kl_guardian_room_v1_valid(4, 1, 1, 2.5f, 0.0f));
    CK(!kl_guardian_roombox_vertices_ok(8) && !kl_guardian_roombox_vertices_ok(14) && kl_guardian_roombox_vertices_ok(12) && kl_guardian_roombox_vertices_ok(16));

    // hull_ok: area, count, finite, height range
    {
        float sq[8] = {0, 0, 2, 0, 2, 2, 0, 2}, thin[8] = {0, 0, 2, 0, 2, 0.001f, 0, 0.001f}, line3[6] = {0, 0, 1, 0, 2, 0};
        CK(kl_guardian_hull_ok(sq, 4, 2.4f) && kl_guardian_hull_ok(sq, 4, 1.5f) && kl_guardian_hull_ok(sq, 4, 6.0f));
        CK(!kl_guardian_hull_ok(sq, 4, 1.4f) && !kl_guardian_hull_ok(sq, 4, 6.1f) && !kl_guardian_hull_ok(sq, 4, NAN));
        CK(!kl_guardian_hull_ok(sq, 2, 2.4f) && !kl_guardian_hull_ok(NULL, 4, 2.4f));
        CK(!kl_guardian_hull_ok(thin, 4, 2.4f) && !kl_guardian_hull_ok(line3, 3, 2.4f));
        sq[2] = NAN;
        CK(!kl_guardian_hull_ok(sq, 4, 2.4f));
    }

    // ring closed: 4 walls of a 3 x 3 box, then one corner moved 6 cm / 4 cm
    float q[48];
    const float cx[4][2] = {{0, 0}, {3, 0}, {3, 3}, {0, 3}};
    for (int k = 0; k < 4; k++) {
        const float *a = cx[k], *b = cx[(k + 1) % 4];
        float v[12] = {a[0], 0, a[1], a[0], 2.5f, a[1], b[0], 2.5f, b[1], b[0], 0, b[1]};
        memcpy(q + 12 * k, v, sizeof v);
    }
    CK(kl_guardian_ring_closed(q, 4, 0.05f));
    CK(kl_guardian_roombox_valid(q, 4, 0.0f, 2.5f));
    q[12 + 0] += 0.04f;                                               // wall 1 vertex 0 off by 4 cm
    CK(kl_guardian_ring_closed(q, 4, 0.05f));
    q[12 + 0] += 0.03f;                                               // 7 cm now
    CK(!kl_guardian_ring_closed(q, 4, 0.05f));
    CK(!kl_guardian_ring_closed(q, 2, 0.05f));
    float q3[36];
    memcpy(q3, q, sizeof q3);                                         // 3 walls that do not meet
    CK(!kl_guardian_roombox_valid(q3, 3, 0.0f, 2.5f));

    printf(fails ? "t_guardian: %d FAILED\n" : "t_guardian: all passed\n", fails);
    return fails != 0;
}
