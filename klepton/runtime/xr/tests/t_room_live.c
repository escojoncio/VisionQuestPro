// Live room: after the game has been handed a room, a significant change of planes / scene mesh rebuilds it and notifies once, at
// most every 2 s, and never touches the floor the user fixed (kl_guardian).
// clang -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag tests/t_room_live.c kl_spatial.c kl_guardian.c -lm -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kl_spatial.h"
#include "kl_guardian.h"

float kl_ovrp_guest_origin_y(void) { return 0.0f; }
const char *kl_jni_files_dir(void) { return "/tmp"; }

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static kl_uuid U16(int n) { kl_uuid u; memset(&u, 0, sizeof u); u.b[0] = (uint8_t)n; u.b[15] = 0x7f; return u; }
static void horiz(float *m, float x, float y, float z, int down) { memset(m, 0, 64); m[0] = 1; m[5] = down ? -1.0f : 1.0f; m[10] = down ? -1.0f : 1.0f; m[15] = 1; m[12] = x; m[13] = y; m[14] = z; }
// wall i of the 4 x 5 ring around the origin (same frames as spatial_test.c): 0/2 run along X, 1/3 along Z
static void wall(int i, float xoff) {
    float m[16];
    memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1;
    m[12] = (i % 2 ? (i == 1 ? 2 : -2) : 0) + xoff; m[13] = 1.3f; m[14] = (i % 2 ? 0 : (i == 0 ? -2.5f : 2.5f));
    if (i % 2) { m[0] = 0; m[2] = 1; }
    m[4] = -m[2]; m[5] = 0; m[6] = m[0]; m[10] = 0; m[9] = -1;
    kl_spatial_plane(U16(10 + i), 0, m, i % 2 ? 5.0f : 4.0f, 2.6f, KLSP_LBL_WALL, NULL, 0);
}
static int g_cb;
static void on_room(void) { g_cb++; }
#define SEC 1000000000ull

int main(void) {
    float m[16], y;
    kl_spatial_set_room_listener(on_room);
    kl_guardian_set_floor(-0.3f, KLG_FLOOR_MANUAL);                       // the floor the user fixed

    horiz(m, 0, 0, 0, 0); kl_spatial_plane(U16(1), 0, m, 4, 5, KLSP_LBL_FLOOR, NULL, 0);
    horiz(m, 0, 2.6f, 0, 1); kl_spatial_plane(U16(2), 0, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);
    wall(0, 0); wall(1, 0); wall(2, 0);                                   // not a complete set: a synthetic room completes it

    // before the game was ever handed a room nothing is built or notified
    uint32_t planes0 = kl_spatial_count(KLSP_PLANE);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 10 * SEC) == 0 && g_cb == 0);
    CK(kl_spatial_count(KLSP_PLANE) == planes0 && kl_spatial_room_generation() == 0);

    CK(kl_spatial_synth_room(0, 0, 3, 3) == 1);                           // the game's scene load: baseline
    uint32_t rg = kl_spatial_room_generation();
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 20 * SEC) == 0 && g_cb == 0 && kl_spatial_room_generation() == rg);   // nothing changed

    // a table is furniture, not part of the room's shape: no notification
    horiz(m, 0.5f, 0.75f, -1, 0); kl_spatial_plane(U16(3), 0, m, 1.2f, 0.8f, KLSP_LBL_TABLE, NULL, 0);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 23 * SEC) == 0 && g_cb == 0);

    // a scene mesh with many points inside the room is a significant change ...
    {
        enum { NV = 400 };
        float v[NV * 3]; uint32_t t[(NV - 2) * 3]; float mm[16];
        for (int i = 0; i < NV; i++) { v[3 * i] = -1.5f + 3.0f * (float)(i % 20) / 19.0f; v[3 * i + 1] = 0.9f; v[3 * i + 2] = -1.5f + 3.0f * (float)(i / 20) / 19.0f; }
        for (int i = 0; i < NV - 2; i++) { t[3 * i] = (uint32_t)i; t[3 * i + 1] = (uint32_t)i + 1; t[3 * i + 2] = (uint32_t)i + 2; }
        horiz(mm, 0, 0, 0, 0);
        kl_spatial_mesh(U16(50), 0, mm, v, NV, t, NV - 2);
    }
    // ... but not before 2 s have passed since the last check (23 s): nothing is rebuilt or notified at 24 s
    uint32_t gen_before = kl_spatial_generation();
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 24 * SEC) == 0 && g_cb == 0 && kl_spatial_generation() == gen_before);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 25 * SEC + 1) == 1 && g_cb == 1 && kl_spatial_room_generation() == rg + 1);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 28 * SEC) == 0 && g_cb == 1);          // same room: quiet

    // a wall moved 1 m changes the footprint: notified; a further move 1 s later waits for the 2 s limit
    wall(2, 1.0f);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 29 * SEC) == 1 && g_cb == 2);
    wall(2, -1.0f);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 30 * SEC) == 0 && g_cb == 2);          // rate-limited: not rebuilt yet
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 31 * SEC + 1) == 1 && g_cb == 3);      // and not lost
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 34 * SEC) == 0 && g_cb == 3);
    // a wall that moves 2 cm is jitter, not a new room
    wall(2, -0.98f);
    uint32_t gjit = kl_spatial_generation();
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 37 * SEC) == 0 && g_cb == 3);
    CK(kl_spatial_generation() == gjit);                                  // and the synthetic room's geometry was not touched either

    // the fourth wall closes the ring: the real set replaces the synthetic one
    wall(2, 0); wall(3, 0);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 40 * SEC) == 1 && g_cb == 4);
    CK(kl_spatial_live_update_at(0, 0, 3, 3, 43 * SEC) == 0 && g_cb == 4);

    CK(kl_guardian_get_floor(&y) == KLG_FLOOR_MANUAL && fabsf(y + 0.3f) < 1e-6f);   // the user's floor was never touched
    CK(kl_spatial_live_update_at(NAN, 0, 3, 3, 50 * SEC) == 0);                     // a NaN head poisons nothing
    printf(fails ? "t_room_live: %d FAILED\n" : "t_room_live: all passed\n", fails);
    return fails != 0;
}
