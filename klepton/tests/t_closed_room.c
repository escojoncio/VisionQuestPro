// Host test of the pure geometry behind the synthetic room (kl_spatial.c, KL-TEST closedroom): a box room is always
// four walls, in ascending atan2(z, x) around the centre (clockwise seen from above, Y up), each ending where the next one starts;
// partial real walls are completed into the smallest box (turned to the longest wall) that holds them and the play zone.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "closedroom.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

// Directed continuity (end of k == start of k+1), positive signed area in (x, z) (ascending angle), side lengths w, d, w, d.
static int box_ok(const klsp_rect *r, const klsp_seg *s) {
    float area = 0;
    for (int k = 0; k < 4; k++) {
        const klsp_seg *n = &s[(k + 1) % 4];
        if (!NEAR(s[k].x1, n->x0) || !NEAR(s[k].z1, n->z0)) return 0;
        area += s[k].x0 * s[k].z1 - s[k].x1 * s[k].z0;
        float len = hypotf(s[k].x1 - s[k].x0, s[k].z1 - s[k].z0);
        if (!NEAR(len, k % 2 ? r->d : r->w)) return 0;
    }
    return area > 0;
}
static int inside(const klsp_rect *r, float x, float z) {       // in the box, 1 mm of slack
    float c = cosf(r->yaw), sn = sinf(r->yaw), dx = x - r->cx, dz = z - r->cz;
    return fabsf(c * dx + sn * dz) <= r->w * 0.5f + 1e-3f && fabsf(-sn * dx + c * dz) <= r->d * 0.5f + 1e-3f;
}

int main(void) {
    // Generated box: closed, ordered, right sizes, at any turn and offset.
    const float yaws[] = {0.0f, 0.5f, -1.2f, 3.0f};
    for (int i = 0; i < 4; i++) {
        klsp_rect r = {1.5f, -0.5f, 4.0f, 5.0f, yaws[i]};
        klsp_seg s[4];
        klsp_box_walls(&r, s);
        CHECK(box_ok(&r, s), "box walls: closed, ascending angle, sides w d w d");
        CHECK(klsp_ring_closed(s, 4, 0.01f), "generated ring is closed");
        float mx = 0, mz = 0;
        for (int k = 0; k < 4; k++) { mx += (s[k].x0 + s[k].x1) * 0.125f; mz += (s[k].z0 + s[k].z1) * 0.125f; }
        CHECK(NEAR(mx, 1.5f) && NEAR(mz, -0.5f), "box centred on the rect centre");
    }
    // Closure check: orientation of each wall does not matter; a gap, a missing wall or an overshoot decide.
    klsp_rect r0 = {0, 0, 4.0f, 3.0f, 0.0f};
    klsp_seg s[4], t[4];
    klsp_box_walls(&r0, s);
    memcpy(t, s, sizeof s);
    float sw = t[1].x0; t[1].x0 = t[1].x1; t[1].x1 = sw; sw = t[1].z0; t[1].z0 = t[1].z1; t[1].z1 = sw;
    CHECK(klsp_ring_closed(t, 4, 0.01f), "a wall with its ends swapped still closes");
    memcpy(t, s, sizeof s);
    t[2].x0 += 0.5f; t[2].x1 += 0.5f;
    CHECK(!klsp_ring_closed(t, 4, 0.1f), "a wall moved 0.5 m leaves a gap");
    CHECK(!klsp_ring_closed(s, 3, 0.1f), "three walls never close");
    memcpy(t, s, sizeof s);
    t[0].x1 += 0.3f;                                       // wall 0 runs 0.3 m past the corner: still touches wall 1
    CHECK(klsp_ring_closed(t, 4, 0.1f), "overshooting the corner still touches");

    // Fit, no walls: the zone around the head.
    klsp_rect f = klsp_fit_rect(NULL, 0, 1.0f, -2.0f, 4.0f, 3.0f);
    CHECK(NEAR(f.cx, 1.0f) && NEAR(f.cz, -2.0f) && NEAR(f.w, 4.0f) && NEAR(f.d, 3.0f), "no walls: the zone itself");
    // Fit, two real walls (axis aligned): x = -2..2 at z = -3 (longest) and z = -3..0 at x = 3. Zone 4 x 4 at the origin.
    klsp_seg p[2] = {{-2, -3, 2, -3}, {3, -3, 3, 0}};
    f = klsp_fit_rect(p, 2, 0, 0, 4.0f, 4.0f);
    CHECK(fabsf(sinf(f.yaw)) < 1e-3f, "yaw follows the longest wall");
    CHECK(NEAR(f.w, 5.0f) && NEAR(f.d, 5.0f) && NEAR(f.cx, 0.5f) && NEAR(f.cz, -0.5f), "partial walls: box holds walls and zone");
    // Fit, two adjacent walls of a turned room: same turn and size back, and everything inside.
    klsp_rect room = {2.0f, 1.0f, 6.0f, 5.0f, 0.5f};
    klsp_seg rs[4];
    klsp_box_walls(&room, rs);
    f = klsp_fit_rect(rs, 2, room.cx, room.cz, 3.0f, 3.0f);
    CHECK(fabsf(sinf(f.yaw - 0.5f)) < 1e-3f && NEAR(f.w, 6.0f) && NEAR(f.d, 5.0f) && NEAR(f.cx, 2.0f) && NEAR(f.cz, 1.0f),
          "turned room recovered from two walls");
    int in = 1;
    for (int k = 0; k < 2; k++) in &= inside(&f, rs[k].x0, rs[k].z0) && inside(&f, rs[k].x1, rs[k].z1);
    CHECK(in, "fit holds every wall end point");
    // The head far outside the walls is still held (zone follows the head).
    f = klsp_fit_rect(p, 2, 10.0f, 0, 2.0f, 2.0f);
    CHECK(inside(&f, 11.0f, 1.0f) && inside(&f, 9.0f, -1.0f) && inside(&f, -2.0f, -3.0f), "head zone held when outside the walls");
    // A fitted box always rebuilds into a closed ring.
    klsp_seg fs[4];
    klsp_box_walls(&f, fs);
    CHECK(box_ok(&f, fs) && klsp_ring_closed(fs, 4, 0.01f), "fitted box closes");

    // Invalid rings are never complete.
    klsp_seg bad[4], ring5[5], ring6[6];
    memcpy(bad, s, sizeof s);
    bad[2].x1 = NAN;
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "NaN end point: rejected");
    memcpy(bad, s, sizeof s);
    bad[0].z0 = INFINITY;
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "Inf end point: rejected");
    for (int k = 0; k < 4; k++) bad[k] = (klsp_seg){0, 0, 2, 0};
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "four walls on the same spot: rejected");
    bad[0] = (klsp_seg){0, 0, 3, 0}; bad[1] = (klsp_seg){2, 0, 5, 0}; bad[2] = (klsp_seg){4, 0, 7, 0}; bad[3] = (klsp_seg){0, 0, 7, 0};
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "four collinear walls that all touch: rejected (no area)");
    bad[0] = (klsp_seg){0, 0, 4, 0}; bad[1] = bad[0]; bad[2] = (klsp_seg){4, 0, 4, 3}; bad[3] = bad[2];
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "two walls each given twice: rejected");
    klsp_rect thin = {0, 0, 4.0f, 0.5f, 0.3f};
    klsp_box_walls(&thin, bad);
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "a 4 x 0.5 m box: rejected (and its 0.5 m walls)");
    klsp_rect tiny = {0, 0, 0.9f, 0.9f, 0.0f};
    klsp_box_walls(&tiny, bad);
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "a 0.9 x 0.9 m box: rejected (under 1 m2)");
    klsp_rect narrow = {0, 0, 4.0f, 0.25f, 0.0f};
    klsp_box_walls(&narrow, bad);
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "walls narrower than 0.3 m: rejected");
    for (int k = 0; k < 4; k++) bad[k] = s[3 - k];
    for (int k = 0; k < 4; k++) { float x = bad[k].x0, z = bad[k].z0; bad[k].x0 = bad[k].x1; bad[k].z0 = bad[k].z1; bad[k].x1 = x; bad[k].z1 = z; }
    CHECK(!klsp_ring_closed(bad, 4, 0.35f), "ring in descending angle order: rejected");
    CHECK(klsp_ring_closed(s, 4, 0.35f), "a 4 x 3 m box: accepted");
    // A pentagon (5 walls that close) is a valid convex room.
    for (int k = 0; k < 5; k++) {
        float a0 = 6.2831853f * k / 5, a1 = 6.2831853f * (k + 1) / 5;
        ring5[k] = (klsp_seg){3 * cosf(a0), 3 * sinf(a0), 3 * cosf(a1), 3 * sinf(a1)};
    }
    CHECK(klsp_ring_closed(ring5, 5, 0.35f), "pentagon: accepted");
    // A 4 x 3 box whose first wall comes as two planes (one with a 0.2 m gap): still a room.
    ring5[0] = (klsp_seg){s[0].x0, s[0].z0, (s[0].x0 + s[0].x1) * 0.5f - 0.1f, s[0].z0};
    ring5[1] = (klsp_seg){(s[0].x0 + s[0].x1) * 0.5f + 0.1f, s[0].z0, s[0].x1, s[0].z1};
    for (int k = 1; k < 4; k++) ring5[k + 1] = s[k];
    CHECK(klsp_ring_closed(ring5, 5, 0.35f), "box with a wall split in two planes: accepted");
    // An L-shaped room (6 walls, one reflex corner) is NOT complete: documented behaviour, the synthetic box stays.
    static const float lx[6] = {0, 4, 4, 2, 2, 0}, lz[6] = {0, 0, 2, 2, 4, 4};
    for (int k = 0; k < 6; k++) ring6[k] = (klsp_seg){lx[k], lz[k], lx[(k + 1) % 6], lz[(k + 1) % 6]};
    CHECK(!klsp_ring_closed(ring6, 6, 0.35f), "L-shaped room: not complete");
    static const float hx6[6] = {0, 4, 5, 4, 0, -1}, hz6[6] = {0, 0, 2, 4, 4, 2};
    for (int k = 0; k < 6; k++) ring6[k] = (klsp_seg){hx6[k], hz6[k], hx6[(k + 1) % 6], hz6[(k + 1) % 6]};
    CHECK(klsp_ring_closed(ring6, 6, 0.35f), "convex 6-wall room: accepted");

    // Wall orientation: walls handed over rolled the wrong way are recognised, and flipping exactly those chains the walls
    // end -> start with the normal (left of the tangent) inward, like the generated box.
    for (int i = 0; i < 4; i++) {
        klsp_rect ro = {1.5f, -0.5f, 4.0f, 5.0f, yaws[i]};
        klsp_seg g4[4], w4[4];
        klsp_box_walls(&ro, g4);
        for (int mask = 0; mask < 16; mask++) {
            for (int k = 0; k < 4; k++) {
                w4[k] = g4[k];
                if (mask >> k & 1) w4[k] = (klsp_seg){g4[k].x1, g4[k].z1, g4[k].x0, g4[k].z0};
            }
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                int out = klsp_wall_outward(&w4[k], ro.cx, ro.cz);
                ok &= out == (mask >> k & 1);
                if (out) w4[k] = (klsp_seg){w4[k].x1, w4[k].z1, w4[k].x0, w4[k].z0};
            }
            CHECK(ok, "wall_outward finds exactly the walls rolled the wrong way");
            CHECK(box_ok(&ro, w4), "after flipping them the walls chain like the synthetic box");
        }
    }
    printf(fails ? "t_closed_room: %d FAILED\n" : "t_closed_room: ok\n", fails);
    return fails != 0;
}
