// Host test of the room built from real data (kl_spatial_synth_room, kl_spatial.c): when the real planes are not a complete
// room -- also when a RoomAnchor exists but its walls are fragments -- the room handed to the game is the convex hull of what
// was measured (wall/floor/ceiling planes of the RoomAnchor, scene-mesh vertices near them): one wall per hull edge (each >= KLSP_MIN_WALL, inward normal),
// the real ceiling height (2.4 m without one), floor and ceiling as that polygon, a closed ring, from a single source.
// gcc -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag tests/t_room_build.c -lm -lpthread
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include "kl_spatial.c"          // statics included on purpose (klsp_ring_closed, find_locked, is_synth)

float kl_ovrp_guest_origin_y(void) { return 0.0f; }
const char *kl_jni_files_dir(void) { return "/tmp"; }

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static kl_uuid U16(int n) { kl_uuid u; memset(&u, 0, sizeof u); u.b[0] = (uint8_t)n; u.b[15] = 0x7f; return u; }
static void horiz(float *m, float x, float y, float z, int down) { memset(m, 0, 64); m[0] = 1; m[5] = down ? -1.0f : 1.0f; m[10] = down ? -1.0f : 1.0f; m[15] = 1; m[12] = x; m[13] = y; m[14] = z; }
// ARKit-side wall frame as Swift sends it: X = tangent, Y = normal (left of the tangent), Z down.
static void wallm(float *m, float x, float y, float z, int alongz) {
    float tx = alongz ? 0.0f : 1.0f, tz = alongz ? 1.0f : 0.0f;
    memset(m, 0, 64); m[0] = tx; m[2] = tz; m[4] = -tz; m[6] = tx; m[9] = -1.0f; m[15] = 1; m[12] = x; m[13] = y; m[14] = z;
}
static void reset(void) {                       // empty store
    pthread_mutex_lock(&g_mu);
    purge_synth_locked();
    while (g_n) del_locked(&g_e[g_n - 1]);
    pthread_mutex_unlock(&g_mu);
}
static char g_log[8192];
static int g_errfd = -1;
static void cap_begin(void) { fflush(stderr); g_errfd = dup(2); int fd = open("/tmp/t_room_build.err", O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(fd, 2); close(fd); }
static void cap_end(void) {
    fflush(stderr); dup2(g_errfd, 2); close(g_errfd);
    FILE *f = fopen("/tmp/t_room_build.err", "r"); size_t n = f ? fread(g_log, 1, sizeof g_log - 1, f) : 0; if (f) fclose(f);
    g_log[n] = 0;
}
static int synth_planes(void) {
    int n = 0;
    for (uint32_t i = 0; i < g_n; i++) n += g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id);
    return n;
}
static void real_plane(int id, const float *m, float w, float h, int label) { kl_spatial_plane(U16(id), 0, m, w, h, label, NULL, 0); }

// The room the game gets (first room): floor, ceiling and >= 4 walls, all synthetic (one source), walls >= KLSP_MIN_WALL with
// inward normals, chained end -> start, a closed ring, height h, floor at floor_y. Returns the number of walls.
static int check_room(const char *tag, float h, float floor_y) {
    kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}}, ws[64];
    uint32_t nw = 0;
    printf("%s: ", tag);
    CK(kl_spatial_count(KLSP_ROOM) >= 1 && kl_spatial_get(KLSP_ROOM, 0, &rid, NULL));
    CK(kl_spatial_room_info(rid, &fl, &ce, 64, ws, &nw, 0, NULL, NULL));
    CK(nw >= 4 && nw <= 64);
    if (nw < 4 || nw > 64) { puts("(no room)"); return 0; }
    CK(is_synth(fl) && is_synth(ce));
    for (uint32_t k = 0; k < nw; k++) CK(is_synth(ws[k]));            // no raw fragment is handed over as a wall
    CK(synth_planes() == (int)nw + 2);
    const ent *f = find_locked(fl), *c = find_locked(ce);
    CK(f && c && fabsf(f->m[13] - floor_y) < 1e-3f && fabsf(c->m[13] - (floor_y + h)) < 1e-3f);
    klsp_seg sg[64];
    float cx = 0, cz = 0;
    for (uint32_t k = 0; k < nw; k++) {
        const ent *e = find_locked(ws[k]);
        float hw = e->w * 0.5f;
        CK(e->w >= KLSP_MIN_WALL && fabsf(e->h - h) < 1e-3f && fabsf(e->m[13] - (floor_y + h * 0.5f)) < 1e-3f);
        sg[k] = (klsp_seg){e->m[12] - e->m[0] * hw, e->m[14] - e->m[2] * hw, e->m[12] + e->m[0] * hw, e->m[14] + e->m[2] * hw};
        cx += e->m[12]; cz += e->m[14];
    }
    cx /= nw; cz /= nw;
    for (uint32_t k = 0; k < nw; k++) {
        const ent *e = find_locked(ws[k]);
        CK(e->m[4] * (cx - e->m[12]) + e->m[6] * (cz - e->m[14]) > 0);           // normal toward the room
        const klsp_seg *n = &sg[(k + 1) % nw];
        CK(fabsf(sg[k].x1 - n->x0) < 1e-3f && fabsf(sg[k].z1 - n->z0) < 1e-3f);   // chained like the box
    }
    CK(klsp_ring_closed(sg, (int)nw, 0.35f));
    printf("%u walls\n", nw);
    return (int)nw;
}
// A polygon of a plane (floor/ceiling), shoelace in plane XY: > 0 = counter-clockwise.
static float poly_area2(kl_uuid id, uint32_t *np) {
    float p[2 * KLSP_POLY_MAX];
    uint32_t n = 0;
    CK(kl_spatial_plane_info(id, NULL, NULL, NULL, KLSP_POLY_MAX, p, &n));
    float a = 0;
    for (uint32_t i = 0; i < n; i++) { uint32_t j = (i + 1) % n; a += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1]; }
    *np = n;
    return a;
}

int main(void) {
    kl_spatial_set_available(1, 1, 1, 0);

    // --- 1. RoomAnchor whose walls are fragments (0.10 - 0.6 m) + real floor 4 x 3 and ceiling at 2.6 m: a built 4 x 3 room.
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.6f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 2.0f, 1.3f, 0.0f, 1);  real_plane(11, m, 0.4f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 0.3f, 1.3f, 1.5f, 0);  real_plane(12, m, 0.1f, 2.6f, KLSP_LBL_WALL);
        wallm(m, -2.0f, 1.3f, 0.2f, 1); real_plane(13, m, 0.25f, 2.6f, KLSP_LBL_WALL);
        horiz(m, 0.5f, 0.75f, -1.0f, 0); real_plane(14, m, 1.2f, 0.8f, KLSP_LBL_TABLE);
        kl_uuid ids[7] = {U16(1), U16(2), U16(10), U16(11), U16(12), U16(13), U16(14)};
        kl_spatial_room(U16(60), 0, ids, 7);
        cap_begin();
        int r = kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f);
        cap_end();
        CK(r == 1);
        int nw = check_room("fragments + floor + ceiling", 2.6f, 0.0f);
        CK(nw == 4);
        CK(strstr(g_log, "[room] built from real: walls=4 hull_pts=") && strstr(g_log, " h=2.60 floor_y=0.00 src=roomanchor"));
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        uint32_t nf = 0, nc = 0;                                   // floor and ceiling are the hull polygon, counter-clockwise
        CK(poly_area2(fl, &nf) > 0 && nf == 4 && poly_area2(ce, &nc) > 0 && nc == 4);
        const ent *f = find_locked(fl);
        CK(fabsf(f->w - 4.0f) < 1e-3f && fabsf(f->h - 3.0f) < 1e-3f);
        CK(kl_spatial_count(KLSP_ROOM) == 1);                      // still one room
        // A second capture keeps the room (same planes, no rebuild).
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1 && synth_planes() == 6);
        // The RoomAnchor updating (ARKit does it all the time) must not tear the built room down.
        kl_spatial_room(U16(60), 0, ids, 7);
        CK(synth_planes() == 6);
        check_room("after a RoomAnchor update", 2.6f, 0.0f);
    }

    // --- 2. No real ceiling: 2.4 m high, floor at the real floor (y = 0.1).
    reset();
    {
        float m[16];
        horiz(m, 0, 0.1f, 0, 0); real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 2.0f, 1.3f, 0.0f, 1);  real_plane(11, m, 0.4f, 2.6f, KLSP_LBL_WALL);
        kl_uuid ids[3] = {U16(1), U16(10), U16(11)};
        kl_spatial_room(U16(60), 0, ids, 3);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(check_room("no ceiling", 2.4f, 0.1f) == 4);
        CK(strstr(g_log, " h=2.40 floor_y=0.10 src=roomanchor"));
    }

    // --- 3. Planes only (no RoomAnchor): a floor 4 x 3 the size of the room is enough for a built room.
    reset();
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.5f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.6f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(check_room("planes only", 2.5f, 0.0f) == 4);
        CK(strstr(g_log, "src=planes"));
    }

    // --- 4. Scene mesh only (no plane to bound it): not used alone -> the usual synthetic box.
    reset();
    {
        float m[16], v[9 * 3] = {-1.5f, 0, -1.2f,  1.5f, 0, -1.2f,  1.5f, 0, 1.2f,  -1.5f, 0, 1.2f,
                                 -1.5f, 2.4f, -1.2f,  1.5f, 2.4f, -1.2f,  1.5f, 2.4f, 1.2f,  -1.5f, 2.4f, 1.2f,
                                 10.0f, 5.0f, 0.0f};
        uint32_t t[3 * 2] = {0, 1, 2, 0, 2, 3};
        horiz(m, 0, 0, 0, 0);
        kl_spatial_mesh(U16(30), 0, m, v, 9, t, 2);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(!strstr(g_log, "built from real") && synth_planes() == 6);
        check_room("mesh only -> synthetic box", 2.5f, 0.0f);
    }

    // --- 4b. A floor plane bounds the mesh: its vertices within 0.5 m of the plane's box count (also with a far triangle at
    //         +-30 m, which must not make a 60 x 60 m room), those beyond and above the ceiling band do not.
    reset();
    {
        float m[16], v[12 * 3] = {-1.9f, 0, -1.2f,  1.9f, 0, -1.2f,  1.9f, 0, 1.2f,  -1.9f, 0, 1.2f,
                                  -1.9f, 2.4f, -1.2f,  1.9f, 2.4f, -1.2f,  1.9f, 2.4f, 1.2f,  -1.9f, 2.4f, 1.2f,
                                  10.0f, 5.0f, 0.0f,
                                  -30.0f, 0, -30.0f,  30.0f, 0, -30.0f,  30.0f, 0, 30.0f};
        uint32_t t[3 * 2] = {0, 1, 2, 0, 2, 3};
        horiz(m, 0, 0, 0, 0);
        real_plane(1, m, 3.0f, 2.4f, KLSP_LBL_FLOOR);
        kl_spatial_mesh(U16(30), 0, m, v, 12, t, 2);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(synth_planes() == 6 && strstr(g_log, "[room] built from real: walls=4 hull_pts=") && strstr(g_log, "src=planes"));
        float fw = 0, fh = 0;
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(kl_spatial_plane_info(fl, &fw, &fh, NULL, 0, NULL, NULL) && fabsf(fw - 3.8f) < 0.05f && fabsf(fh - 2.4f) < 0.05f);
        check_room("floor + bounded mesh", 2.4f, 0.0f);
    }

    // --- B1. The head outside the measured footprint (4 wall fragments making 2 x 2 m, head at 5, 5): the player must stand
    //         inside the walls -> the room is not the 2 x 2 hull.
    reset();
    {
        float m[16];
        wallm(m, 0.0f, 1.0f, -1.0f, 0); real_plane(10, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, 1.0f, 1.0f, 0.0f, 1);  real_plane(11, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, 0.0f, 1.0f, 1.0f, 0);  real_plane(12, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, -1.0f, 1.0f, 0.0f, 1); real_plane(13, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(5.0f, 5.0f, 3.0f, 3.0f) == 1);
        cap_end();
        check_room("head outside the fragments", 2.5f, 0.0f);
        int inside = 0, walls = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && g_e[i].label == KLSP_LBL_WALL) {
            walls++; inside += g_e[i].m[4] * (5.0f - g_e[i].m[12]) + g_e[i].m[6] * (5.0f - g_e[i].m[14]) > 0;
        }
        CK(walls >= 4 && inside == walls);
        // control: the head inside the same fragments -> the 2 x 2 hull room
        reset();
        wallm(m, 0.0f, 1.0f, -1.0f, 0); real_plane(10, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, 1.0f, 1.0f, 0.0f, 1);  real_plane(11, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, 0.0f, 1.0f, 1.0f, 0);  real_plane(12, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        wallm(m, -1.0f, 1.0f, 0.0f, 1); real_plane(13, m, 2.0f, 2.0f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] built from real: walls=4"));
    }

    // --- B2. Planes of another room (not members of the RoomAnchor) and a far mesh triangle do not enlarge the room.
    reset();
    {
        float m[16], v[3 * 3] = {-30.0f, 0, -30.0f,  30.0f, 0, -30.0f,  30.0f, 0, 30.0f};
        uint32_t t[3] = {0, 1, 2};
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.5f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.5f, KLSP_LBL_WALL);
        horiz(m, 20.0f, 0, 0, 0);    real_plane(21, m, 4, 3, KLSP_LBL_FLOOR);       // another room, 20 m away
        wallm(m, 20.0f, 1.3f, 1.5f, 0); real_plane(22, m, 3.0f, 2.5f, KLSP_LBL_WALL);
        horiz(m, 0, 0, 0, 0);
        kl_spatial_mesh(U16(30), 0, m, v, 3, t, 1);
        kl_uuid ids[3] = {U16(1), U16(2), U16(10)};
        kl_spatial_room(U16(60), 0, ids, 3);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        float fw = 0, fh = 0;
        kl_uuid fl = {{0}};
        kl_uuid rid = {{0}}, ce = {{0}};
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(kl_spatial_plane_info(fl, &fw, &fh, NULL, 0, NULL, NULL) && fabsf(fw - 4.0f) < 0.05f && fabsf(fh - 3.0f) < 0.05f);
        check_room("RoomAnchor members only", 2.5f, 0.0f);
    }
    reset();
    {   // no RoomAnchor: the far triangle still cannot enlarge a room bounded by its planes
        float m[16], v[3 * 3] = {-30.0f, 0, -30.0f,  30.0f, 0, -30.0f,  30.0f, 0, 30.0f};
        uint32_t t[3] = {0, 1, 2};
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        kl_spatial_mesh(U16(30), 0, m, v, 3, t, 1);
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        float fw = 0, fh = 0;
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(kl_spatial_plane_info(fl, &fw, &fh, NULL, 0, NULL, NULL) && fabsf(fw - 4.0f) < 0.05f && fabsf(fh - 3.0f) < 0.05f);
    }

    // --- B3. The room follows the data: a first capture with 2 planes and no mesh builds the minimum; a second with a mesh and
    //         more planes rebuilds it (same ids, never two sources); a third identical one leaves it alone.
    reset();
    {
        float m[16], v[8 * 3] = {-2.4f, 0, -1.5f,  2.4f, 0, -1.5f,  2.4f, 0, 1.5f,  -2.4f, 0, 1.5f,
                                 -2.4f, 2.4f, -1.5f,  2.4f, 2.4f, -1.5f,  2.4f, 2.4f, 1.5f,  -2.4f, 2.4f, 1.5f};
        uint32_t t[3 * 2] = {0, 1, 2, 0, 2, 3};
        kl_uuid before[8], after[8];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.6f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "synthetic room") != NULL && synth_planes() == 6);
        float fw = 0, fh = 0;
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(kl_spatial_plane_info(fl, &fw, &fh, NULL, 0, NULL, NULL) && fabsf(fw - 4.0f) < 0.05f);
        int nb = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && nb < 8) before[nb++] = g_e[i].id;
        // second capture: a mesh and another wall
        horiz(m, 0, 0, 0, 0);
        kl_spatial_mesh(U16(30), 0, m, v, 8, t, 2);
        wallm(m, 2.0f, 1.3f, 0.0f, 1); real_plane(11, m, 0.4f, 2.6f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] built from real") != NULL && synth_planes() == 6);        // rebuilt, one source only
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(kl_spatial_plane_info(fl, &fw, &fh, NULL, 0, NULL, NULL) && fabsf(fw - 4.8f) < 0.05f);
        check_room("rebuilt with more data", 2.4f, 0.0f);
        int na = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && na < 8) after[na++] = g_e[i].id;
        CK(nb == 6 && na == 6);
        for (int i = 0; i < 6; i++) { int f = 0; for (int j = 0; j < 6; j++) f |= uuid_eq(before[i], after[j]); CK(f); }     // ids stable by index
        // third capture, same input: nothing is rebuilt
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(g_log[0] == 0 && synth_planes() == 6);
        for (int i = 0; i < 6; i++) { int f = 0; for (uint32_t j = 0; j < g_n; j++) f |= uuid_eq(after[i], g_e[j].id); CK(f); }
    }

    // --- 5. Envelope with < 4 useful edges (three far fragments -> a triangle): the usual synthetic box, nothing built.
    reset();
    {
        float m[16];
        wallm(m, 0.0f, 1.0f, -1.5f, 0); real_plane(10, m, 0.1f, 2.0f, KLSP_LBL_WALL);
        wallm(m, 1.5f, 1.0f, 1.0f, 0);  real_plane(11, m, 0.1f, 2.0f, KLSP_LBL_WALL);
        wallm(m, -1.5f, 1.0f, 1.0f, 0); real_plane(12, m, 0.1f, 2.0f, KLSP_LBL_WALL);
        kl_uuid ids[3] = {U16(10), U16(11), U16(12)};
        kl_spatial_room(U16(60), 0, ids, 3);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(!strstr(g_log, "built from real") && strstr(g_log, "synthetic room"));
        CK(synth_planes() == 6);
        check_room("triangle -> synthetic box", 2.5f, 0.0f);
    }
    reset();
    {   // nothing at all but a RoomAnchor without planes: the synthetic box around the play zone (4 x 4)
        kl_spatial_room(U16(60), 0, NULL, 0);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(!strstr(g_log, "built from real") && synth_planes() == 6);
        check_room("RoomAnchor without planes -> synthetic box", 2.5f, 0.0f);
        const ent *f = NULL;
        kl_uuid fl = {{0}}; kl_spatial_room_info(U16(60), &fl, NULL, 0, NULL, NULL, 0, NULL, NULL);
        f = find_locked(fl);
        CK(f && fabsf(f->w - 4.0f) < 1e-3f && fabsf(f->h - 4.0f) < 1e-3f);
    }

    // --- 6. A complete real room (floor, ceiling, 4 closing walls) with its RoomAnchor: untouched, its own walls handed over.
    reset();
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 5, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.6f, 0, 1); real_plane(2, m, 4, 5, KLSP_LBL_CEILING);
        kl_uuid ids[6] = {U16(1), U16(2), U16(10), U16(11), U16(12), U16(13)};
        for (int i = 0; i < 4; i++) {
            wallm(m, i % 2 ? (i == 1 ? 2.0f : -2.0f) : 0.0f, 1.3f, i % 2 ? 0.0f : (i == 0 ? -2.5f : 2.5f), i % 2);
            real_plane(10 + i, m, i % 2 ? 5.0f : 4.0f, 2.6f, KLSP_LBL_WALL);
        }
        kl_spatial_room(U16(60), 0, ids, 6);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 0);
        cap_end();
        CK(synth_planes() == 0 && !strstr(g_log, "built from real"));
        kl_uuid fl = {{0}}, ce = {{0}}, ws[8]; uint32_t nw = 0;
        CK(kl_spatial_room_info(U16(60), &fl, &ce, 8, ws, &nw, 0, NULL, NULL) && nw == 4 && fl.b[0] == 1 && ce.b[0] == 2);
        for (uint32_t k = 0; k < nw; k++) CK(ws[k].b[0] >= 10 && ws[k].b[0] <= 13);
    }

    // --- 7. The guardian's default play space from the room alone (no game asked for the scene, kl_spatial_synth_room not called):
    //        kl_spatial_update_guardian_default stores the 4 x 3 outline; a second call within 2 s is rate-limited; a user boundary wins.
    reset();
    kl_guardian_clear();
    g_gd_last_ns = 0;
    {
        float m[16], p[8] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.6f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 0.6f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 2.0f, 1.3f, 0.0f, 1);  real_plane(11, m, 0.4f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 0.3f, 1.3f, 1.5f, 0);  real_plane(12, m, 0.1f, 2.6f, KLSP_LBL_WALL);
        wallm(m, -2.0f, 1.3f, 0.2f, 1); real_plane(13, m, 0.25f, 2.6f, KLSP_LBL_WALL);
        kl_uuid ids[6] = {U16(1), U16(2), U16(10), U16(11), U16(12), U16(13)};
        kl_spatial_room(U16(60), 0, ids, 6);
        CK(!kl_guardian_configured());
        CK(kl_spatial_update_guardian_default(0.0f, 0.0f) == 1);
        float w = 0, d = 0;
        CK(kl_guardian_boundary_source() == KLG_BSRC_ROOM && kl_guardian_playspace_ok() && kl_guardian_playspace(&w, &d, NULL, NULL, NULL));
        CK(fabsf(fmaxf(w, d) - 4.0f) < 0.1f && fabsf(fminf(w, d) - 3.0f) < 0.1f);
        uint32_t gen = kl_guardian_generation();
        CK(kl_spatial_update_guardian_default(0.0f, 0.0f) == 0 && kl_guardian_generation() == gen);   // rate-limited
        g_gd_last_ns = 0;
        kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0);                                          // user boundary
        CK(kl_spatial_update_guardian_default(0.0f, 0.0f) == 0 && kl_guardian_boundary_source() == KLG_BSRC_USER);
        kl_guardian_clear();
    }

    puts(fails ? "t_room_build: FAILED" : "t_room_build: ok");
    return fails != 0;
}
