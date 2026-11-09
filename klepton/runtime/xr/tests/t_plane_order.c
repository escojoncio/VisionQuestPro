// Order a game gets the planes in: FLOOR, CEILING, WALL (the room's order), the rest, and OTHER always last. Handles stay stable.
// clang -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag tests/t_plane_order.c kl_spatial.c kl_guardian.c -lm -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kl_spatial.h"

float kl_ovrp_guest_origin_y(void) { return 0.0f; }
const char *kl_jni_files_dir(void) { return "/tmp"; }

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static kl_uuid U16(int n) { kl_uuid u; memset(&u, 0, sizeof u); u.b[0] = (uint8_t)n; u.b[15] = 0x7f; return u; }
static void at(float *m, float x, float y, float z) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; m[12] = x; m[13] = y; m[14] = z; }
static int label_at(uint32_t i) { kl_uuid id; int l = -1; if (!kl_spatial_get(KLSP_PLANE, i, &id, NULL)) return -1; kl_spatial_plane_info(id, NULL, NULL, &l, 0, NULL, NULL); return l; }

int main(void) {
    float m[16];
    // arrival order: OTHER, TABLE, wall, OTHER, wall, ceiling, floor, wall, COUCH
    at(m, 0.2f, 0.3f, 0.1f); kl_spatial_plane(U16(1), 0, m, 0.43f, 0.17f, KLSP_LBL_OTHER, NULL, 0);
    at(m, 0.5f, 0.75f, -1); kl_spatial_plane(U16(2), 0, m, 1.2f, 0.8f, KLSP_LBL_TABLE, NULL, 0);
    at(m, 0, 1.3f, -2.5f); kl_spatial_plane(U16(3), 0, m, 4, 2.6f, KLSP_LBL_WALL, NULL, 0);
    at(m, 1.0f, 0.4f, 0.0f); kl_spatial_plane(U16(4), 0, m, 0.3f, 0.3f, KLSP_LBL_OTHER, NULL, 0);
    at(m, 2, 1.3f, 0); kl_spatial_plane(U16(5), 0, m, 5, 2.6f, KLSP_LBL_WALL, NULL, 0);
    at(m, 0, 2.6f, 0); kl_spatial_plane(U16(6), 0, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);
    at(m, 0, 0, 0); kl_spatial_plane(U16(7), 0, m, 4, 5, KLSP_LBL_FLOOR, NULL, 0);
    at(m, 0, 1.3f, 2.5f); kl_spatial_plane(U16(8), 0, m, 4, 2.6f, KLSP_LBL_WALL, NULL, 0);
    at(m, -1, 0.4f, 1); kl_spatial_plane(U16(9), 0, m, 1.5f, 0.8f, KLSP_LBL_COUCH, NULL, 0);
    CK(kl_spatial_count(KLSP_PLANE) == 9);
    const int want[9] = {KLSP_LBL_FLOOR, KLSP_LBL_CEILING, KLSP_LBL_WALL, KLSP_LBL_WALL, KLSP_LBL_WALL, KLSP_LBL_TABLE, KLSP_LBL_COUCH, KLSP_LBL_OTHER, KLSP_LBL_OTHER};
    for (uint32_t i = 0; i < 9; i++) CK(label_at(i) == want[i]);
    // within a category the arrival order is kept (table before couch), and every plane is listed exactly once
    kl_uuid seen[9]; int dup = 0;
    for (uint32_t i = 0; i < 9; i++) { kl_spatial_get(KLSP_PLANE, i, &seen[i], NULL); for (uint32_t j = 0; j < i; j++) dup |= !memcmp(&seen[i], &seen[j], 16); }
    CK(!dup && seen[0].b[0] == 7 && seen[1].b[0] == 6 && seen[5].b[0] == 2 && seen[6].b[0] == 9 && seen[7].b[0] == 1 && seen[8].b[0] == 4);

    // handles are given in listing order: the floor first, an OTHER last; they never change afterwards
    uint64_t h[9];
    for (uint32_t i = 0; i < 9; i++) h[i] = kl_spatial_handle_for(seen[i]);
    for (uint32_t i = 1; i < 9; i++) CK(h[i] == h[i - 1] + 1);
    at(m, 0, 2.7f, 0); kl_spatial_plane(U16(20), 0, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);     // a second ceiling arrives later
    kl_uuid again[10];
    for (uint32_t i = 0; i < 10; i++) kl_spatial_get(KLSP_PLANE, i, &again[i], NULL);
    for (uint32_t i = 0; i < 9; i++) CK(kl_spatial_handle_for(seen[i]) == h[i]);
    CK(label_at(2) == KLSP_LBL_CEILING && again[2].b[0] == 20);                               // listed among the ceilings ...
    CK(kl_spatial_handle_for(again[2]) == h[8] + 1);                                          // ... with a new handle, nothing renumbered
    kl_spatial_plane(U16(20), 1, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);

    // the room's container: floor, ceiling, walls in the room's order (around the centre), the rest, OTHER last
    kl_uuid rid, fl, ce, ws[8], mem[16]; uint32_t nw = 0, nm = 0;
    kl_spatial_room(U16(30), 0, NULL, 0);                                                      // a room anchor with no member list: every plane
    CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL));
    CK(kl_spatial_room_info(rid, &fl, &ce, 8, ws, &nw, 16, mem, &nm) && nm == 9 && nw == 3);
    CK(mem[0].b[0] == 7 && mem[1].b[0] == 6);
    CK(mem[2].b[0] == ws[0].b[0] && mem[3].b[0] == ws[1].b[0] && mem[4].b[0] == ws[2].b[0]);
    CK(mem[5].b[0] == 2 && mem[6].b[0] == 9 && mem[7].b[0] == 1 && mem[8].b[0] == 4);
    printf(fails ? "t_plane_order: %d FAILED\n" : "t_plane_order: all passed\n", fails);
    return fails != 0;
}
