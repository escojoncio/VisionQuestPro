// Host test of the room handed to a game when the real planes are not a complete room (kl_spatial_synth_room, kl_spatial.c).
// Only measured data and the user's confirmed zone build it, in this order (log "[room] source=..."): real (complete set, nothing
// added) -> outline (footprint from the real WALL planes joined by the minimal closure, no hull, no margin; non-convex rooms stay
// non-convex) -> guardian (the user's confirmed zone, one wall per edge) -> fallback (the old box around the play zone, margin
// included, only when there is nothing measured and no zone). Height: real ceiling, else the room's mesh, else 2.4 m. Floor: the
// guardian's (confirmed) floor, else the real one.
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

// A wall plane from guest point (x0, z0) to (x1, z1): X = tangent, Y = normal (left of the tangent), Z down; returns its length.
static float wallseg(float *m, float x0, float z0, float x1, float z1, float y) {
    float l = hypotf(x1 - x0, z1 - z0), tx = (x1 - x0) / l, tz = (z1 - z0) / l;
    memset(m, 0, 64); m[0] = tx; m[2] = tz; m[4] = -tz; m[6] = tx; m[9] = -1.0f; m[15] = 1; m[12] = (x0 + x1) * 0.5f; m[13] = y; m[14] = (z0 + z1) * 0.5f;
    return l;
}
// Real walls of a 4 x 3 room centred on the origin WITHOUT its back wall (z = +1.5), ids 10-12, mixed orientations.
static void walls3(float h) {
    float m[16];
    wallm(m, 0.0f, h * 0.5f, -1.5f, 0); real_plane(10, m, 4.0f, h, KLSP_LBL_WALL);
    wallm(m, 2.0f, h * 0.5f, 0.0f, 1);  real_plane(11, m, 3.0f, h, KLSP_LBL_WALL);
    wallm(m, -2.0f, h * 0.5f, 0.0f, 1); real_plane(12, m, 3.0f, h, KLSP_LBL_WALL);
}
static void zone_rect(float hw, float hd) {         // the user's confirmed zone: a rectangle around the guest origin
    float p[8] = {-hw, -hd, hw, -hd, hw, hd, -hw, hd};
    CK(kl_guardian_set_boundary(p, 4, KLG_MODE_ROOMSCALE, 0) >= 0);
}
static void guardian_off(void) { kl_guardian_clear(); kl_guardian_set_floor(0.0f, 0); }
// The synthetic floor of the first room: its bounding size and polygon vertex count.
static int floor_dims(float *w, float *d, uint32_t *np) {
    kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
    float p[2 * KLSP_POLY_MAX];
    if (!kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) || !kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL)) return 0;
    return kl_spatial_plane_info(fl, w, d, NULL, KLSP_POLY_MAX, p, np);
}

int main(void) {
    kl_spatial_set_available(1, 1, 1, 0);

    // --- 1. Three of the four real walls (the back one is missing) + floor + ceiling 2.6 m, in a RoomAnchor: source=outline, the
    //        measured 4 x 3 -- no margin, no hull -- the missing side closed by the straight line between the open ends.
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.6f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        walls3(2.6f);
        horiz(m, 0.5f, 0.75f, -1.0f, 0); real_plane(14, m, 1.2f, 0.8f, KLSP_LBL_TABLE);
        kl_uuid ids[6] = {U16(1), U16(2), U16(10), U16(11), U16(12), U16(14)};
        kl_spatial_room(U16(60), 0, ids, 6);
        cap_begin();
        int r = kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f);
        cap_end();
        CK(r == 1);
        int nw = check_room("3 of 4 walls -> outline", 2.6f, 0.0f);
        CK(nw == 4);
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.60 nwalls=4") && strstr(g_log, "hsrc=ceiling floor_y=0.00 src=roomanchor"));
        CK(!strstr(g_log, "source=fallback"));
        float fw = 0, fh = 0;
        uint32_t nf = 0, nc = 0;
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        CK(floor_dims(&fw, &fh, &nf) && fabsf(fw - 4.0f) < 1e-3f && fabsf(fh - 3.0f) < 1e-3f && nf == 4);     // no 0.5 m margin
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        CK(poly_area2(fl, &nf) > 0 && nf == 4 && poly_area2(ce, &nc) > 0 && nc == 4);                       // counter-clockwise polygons
        CK(fabsf(poly_area2(fl, &nf) - 24.0f) < 0.05f);                                                      // 12 m2
        CK(kl_spatial_count(KLSP_ROOM) == 1);                      // still one room
        // A second capture keeps the room (same planes, no rebuild).
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1 && synth_planes() == 6);
        cap_end();
        CK(g_log[0] == 0);
        // The RoomAnchor updating (ARKit does it all the time) must not tear the built room down.
        kl_spatial_room(U16(60), 0, ids, 6);
        CK(synth_planes() == 6);
        check_room("after a RoomAnchor update", 2.6f, 0.0f);
    }

    // --- 2. No real ceiling: the height is the room's mesh (max y inside the footprint), else 2.4 m. Floor at the real floor (0.1).
    reset();
    {
        float m[16];
        horiz(m, 0, 0.1f, 0, 0); real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        kl_uuid ids[4] = {U16(1), U16(10), U16(11), U16(12)};
        kl_spatial_room(U16(60), 0, ids, 4);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(check_room("no ceiling, no mesh", 2.4f, 0.1f) == 4);
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.40 nwalls=4 hsrc=default floor_y=0.10 src=roomanchor"));
        // the scene mesh: top at 2.55 above the floor inside the footprint; a stray point outside it (10, 5) and one inside
        // but beyond 6 m are not the ceiling
        reset();
        horiz(m, 0, 0.1f, 0, 0); real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        kl_spatial_room(U16(60), 0, ids, 4);
        float v[6 * 3] = {-1.0f, 0.1f, -1.0f,  1.0f, 2.65f, -1.0f,  1.0f, 1.2f, 1.0f,  10.0f, 5.0f, 5.0f,  0.0f, 9.0f, 0.0f,  -30.0f, 3.0f, 30.0f};
        uint32_t t[3 * 2] = {0, 1, 2, 3, 4, 5};
        horiz(m, 0, 0, 0, 0);
        kl_spatial_mesh(U16(30), 0, m, v, 6, t, 2);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(check_room("mesh height", 2.55f, 0.1f) == 4);
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.55 nwalls=4 hsrc=mesh floor_y=0.10"));
    }

    // --- 3. Planes only (no RoomAnchor): the real walls of the store are enough for the outline.
    reset();
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.5f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        walls3(2.5f);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(check_room("planes only", 2.5f, 0.0f) == 4);
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.50 nwalls=4") && strstr(g_log, "src=planes"));
    }

    // --- 4. Scene mesh only, then a floor plane and mesh without walls: neither shapes the footprint -> the box (fallback),
    //        3 x 3 zone + 0.5 m per side, logged as such.
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
        CK(strstr(g_log, "[room] source=fallback dims=4.00x4.00 h=2.50 nwalls=4") && synth_planes() == 6);
        check_room("mesh only -> fallback box", 2.5f, 0.0f);
        reset();
        horiz(m, 0, 0, 0, 0);
        real_plane(1, m, 3.8f, 2.4f, KLSP_LBL_FLOOR);
        kl_spatial_mesh(U16(30), 0, m, v, 9, t, 2);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=fallback dims=4.00x4.00") && synth_planes() == 6);
        check_room("floor plane + mesh, no walls -> fallback box", 2.5f, 0.0f);
    }

    // --- 5. Fewer than 3 usable real walls (fragments under KLSP_MIN_WALL do not count; two walls are no outline) and no zone: fallback.
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
        CK(strstr(g_log, "[room] source=fallback") && strstr(g_log, "synthetic room") && synth_planes() == 6);
        check_room("fragments -> fallback box", 2.5f, 0.0f);
    }
    reset();
    {
        float m[16];
        wallm(m, 0.0f, 1.3f, -1.5f, 0); real_plane(10, m, 4.0f, 2.6f, KLSP_LBL_WALL);
        wallm(m, 2.0f, 1.3f, 0.0f, 1);  real_plane(11, m, 3.0f, 2.6f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=fallback"));
        check_room("two walls -> fallback box", 2.5f, 0.0f);
    }
    reset();
    {   // nothing at all but a RoomAnchor without planes: the fallback box around the play zone (4 x 4)
        kl_spatial_room(U16(60), 0, NULL, 0);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=fallback dims=4.00x4.00 h=2.50 nwalls=4") && synth_planes() == 6);
        check_room("RoomAnchor without planes -> fallback box", 2.5f, 0.0f);
        float fw = 0, fh = 0;
        uint32_t nf = 0;
        CK(floor_dims(&fw, &fh, &nf) && fabsf(fw - 4.0f) < 1e-3f && fabsf(fh - 4.0f) < 1e-3f);
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
        CK(synth_planes() == 0 && strstr(g_log, "[room] source=real dims=4.00x5.00 h=2.60 nwalls=4"));
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

    // --- B1. The head outside the measured footprint (4 wall pieces making 2 x 2 m, head at 5, 5): the player must stand inside
    //         the walls -> not an outline; the fallback box holds the head. Control: the head inside -> the 2 x 2 outline.
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
        CK(strstr(g_log, "[room] source=fallback"));
        check_room("head outside the walls", 2.5f, 0.0f);
        int inside = 0, walls = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && g_e[i].label == KLSP_LBL_WALL) {
            walls++; inside += g_e[i].m[4] * (5.0f - g_e[i].m[12]) + g_e[i].m[6] * (5.0f - g_e[i].m[14]) > 0;
        }
        CK(walls >= 4 && inside == walls);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=2.00x2.00 h=2.40 nwalls=4"));
        check_room("head inside the walls", 2.4f, 0.0f);
    }

    // --- B2. Walls of another room (not members of the RoomAnchor) and a far mesh triangle do not enlarge the room.
    reset();
    {
        float m[16], v[3 * 3] = {-30.0f, 0, -30.0f,  30.0f, 0, -30.0f,  30.0f, 0, 30.0f};
        uint32_t t[3] = {0, 1, 2};
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.5f);
        horiz(m, 20.0f, 0, 0, 0);    real_plane(21, m, 4, 3, KLSP_LBL_FLOOR);       // another room, 20 m away
        wallm(m, 20.0f, 1.3f, 1.5f, 0); real_plane(22, m, 3.0f, 2.5f, KLSP_LBL_WALL);
        horiz(m, 0, 0, 0, 0);
        kl_spatial_mesh(U16(30), 0, m, v, 3, t, 1);
        kl_uuid ids[4] = {U16(1), U16(10), U16(11), U16(12)};
        kl_spatial_room(U16(60), 0, ids, 4);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        float fw = 0, fh = 0;
        uint32_t nf = 0;
        CK(floor_dims(&fw, &fh, &nf) && fabsf(fw - 4.0f) < 1e-3f && fabsf(fh - 3.0f) < 1e-3f);
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.40 nwalls=4"));
        check_room("RoomAnchor members only", 2.4f, 0.0f);
    }

    // --- B3. The room follows the data: a first capture with 3 walls builds the outline (2.4 m); a second with a real ceiling at 2.7 m
    //         rebuilds it (same ids, never two sources); a third identical one leaves it alone.
    reset();
    {
        float m[16];
        kl_uuid before[8], after[8];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "synthetic room") != NULL && synth_planes() == 6);
        check_room("first capture", 2.4f, 0.0f);
        int nb = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && nb < 8) before[nb++] = g_e[i].id;
        horiz(m, 0, 2.7f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.70") != NULL && synth_planes() == 6);        // rebuilt, one source only
        check_room("rebuilt with the ceiling", 2.7f, 0.0f);
        int na = 0;
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id) && na < 8) after[na++] = g_e[i].id;
        CK(nb == 6 && na == 6);
        for (int i = 0; i < 6; i++) { int f = 0; for (int j = 0; j < 6; j++) f |= uuid_eq(before[i], after[j]); CK(f); }     // ids stable by index
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(g_log[0] == 0 && synth_planes() == 6);
    }

    // --- 8. No real walls but the user's confirmed zone (3 x 3) and floor (0.2): source=guardian, exactly the zone (not the 7 x 7 the
    //        caller passes), 4 walls, floor and ceiling = the zone, floor at the confirmed floor even when a real floor plane says 0.
    reset();
    {
        float m[16];
        kl_guardian_set_floor(0.2f, KLG_FLOOR_MANUAL);
        zone_rect(1.5f, 1.5f);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 7.0f, 7.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=3.00x3.00 h=2.40 nwalls=4 hsrc=default floor_y=0.20") && !strstr(g_log, "source=fallback"));
        CK(check_room("zone 3x3 -> guardian", 2.4f, 0.2f) == 4);
        float fw = 0, fh = 0;
        uint32_t nf = 0;
        CK(floor_dims(&fw, &fh, &nf) && fabsf(fw - 3.0f) < 1e-3f && fabsf(fh - 3.0f) < 1e-3f && nf == 4);
        reset();                                                // + a real floor plane at 0 and a real ceiling at 2.7: floor stays 0.2
        horiz(m, 0, 0.0f, 0, 0); real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.7f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 7.0f, 7.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=3.00x3.00 h=2.50 nwalls=4 hsrc=ceiling floor_y=0.20"));
        check_room("zone + real ceiling", 2.5f, 0.2f);
        guardian_off();
    }

    // --- 9. Nothing measured and no zone: the fallback box, logged. A real L-shaped room (6 walls, floor, ceiling) is not turned into
    //        a convex box: 6 walls, the L polygon (12 m2, not its 16 m2 bounding box), inward normals, whichever way the planes point.
    reset();
    guardian_off();
    {
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=fallback dims=4.00x4.00 h=2.50 nwalls=4") && synth_planes() == 6);
        reset();
        float m[16];
        const float L[6][2] = {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
        const int order[6] = {3, 0, 5, 1, 4, 2}, flip[6] = {1, 0, 0, 1, 0, 1};      // creation order and direction scrambled
        horiz(m, 2.0f, 0, 2.0f, 0);    real_plane(1, m, 4, 4, KLSP_LBL_FLOOR);
        horiz(m, 2.0f, 2.5f, 2.0f, 1); real_plane(2, m, 4, 4, KLSP_LBL_CEILING);
        for (int i = 0; i < 6; i++) {
            int k = order[i], a = k, b = (k + 1) % 6;
            if (flip[i]) { int t = a; a = b; b = t; }
            float l = wallseg(m, L[a][0], L[a][1], L[b][0], L[b][1], 1.25f);
            real_plane(10 + i, m, l, 2.5f, KLSP_LBL_WALL);
        }
        cap_begin();
        CK(kl_spatial_synth_room(1.0f, 1.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x4.00 h=2.50 nwalls=6"));
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}}, ws[16];
        uint32_t nw = 0, nf = 0;
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 16, ws, &nw, 0, NULL, NULL) && nw == 6);
        CK(is_synth(fl) && poly_area2(fl, &nf) > 0 && nf == 6 && fabsf(poly_area2(fl, &nf) - 24.0f) < 0.05f);
        for (uint32_t k = 0; k < nw && k < 16; k++) {
            const ent *e = find_locked(ws[k]);
            float px = e->m[12] + 0.1f * e->m[4], pz = e->m[14] + 0.1f * e->m[6];             // 10 cm along the wall's normal
            int in = (px > 0 && px < 4 && pz > 0 && pz < 2) || (px > 0 && px < 2 && pz > 0 && pz < 4);
            CK(is_synth(ws[k]) && in);
        }
    }

    // --- 10. The zone the player drew wins over the measured walls (guardian before outline), also when it is smaller, bigger or does not
    //         hold his head; a stationary circle is not a room at all (the outline / fallback decide, as without a drawing).
    reset();
    {
        float m[16];
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        zone_rect(1.0f, 1.0f);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=2.00x2.00 h=2.40 nwalls=4") && !strstr(g_log, "zone_outside_room"));
        check_room("drawn 2x2 inside measured 4x3", 2.4f, 0.0f);
        reset();
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        zone_rect(3.0f, 3.0f);
        cap_begin();
        CK(kl_spatial_synth_room(50.0f, 50.0f, 3.0f, 3.0f) == 1);               // head far outside the zone: nobody asks
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=6.00x6.00 h=2.40 nwalls=4"));
        guardian_off();
        reset();
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        walls3(2.6f);
        CK(kl_guardian_set_boundary(NULL, 0, KLG_MODE_STATIONARY, 1.0f) == 1);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00"));              // the circle is not used as a room
        reset();
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=fallback dims=4.00x4.00"));
        guardian_off();
    }

    // --- 11. The drawn zone as walls: a triangle is a room (V1 needs 3 walls) and never turns into the invented box; an edge under
    //         0.3 m is fused into its neighbour; a zone the guardian API closed (kl_guardian_zone_*) is the one handed over.
    reset();
    {
        kl_guardian_zone_clear();
        kl_guardian_zone_add(0, 0); kl_guardian_zone_add(4, 0); kl_guardian_zone_add(0, 4);
        CK(kl_guardian_zone_close() == KLG_ZONE_OK);
        cap_begin();
        CK(kl_spatial_synth_room(1.0f, 1.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=4.00x4.00 h=2.40 nwalls=3") && synth_planes() == 5);
        float fw = 0, fh = 0;
        uint32_t nf = 0;
        CK(floor_dims(&fw, &fh, &nf) && nf == 3);
        kl_guardian_zone_clear();
        reset();
        float p[10] = {-1.5f, -1.5f, 1.5f, -1.5f, 1.5f, 1.5f, 1.5f - 0.1f, 1.5f + 0.05f, -1.5f, 1.5f};   // a 10 cm jag on the top edge
        CK(kl_guardian_set_boundary(p, 5, KLG_MODE_ROOMSCALE, 0) >= 0);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=3.00x3.0") && strstr(g_log, " h=2.40 nwalls=4"));   // jag fused into one vertex
        guardian_off();
    }

    // --- 12. Outline defences. A free partition (1.5 m, inside, touching nothing) and one attached to a wall do not bend the ring.
    reset();
    {
        float m[16];
        walls3(2.6f);
        wallseg(m, 0.0f, -0.5f, 0.0f, 1.0f, 1.3f); real_plane(20, m, 1.5f, 2.6f, KLSP_LBL_WALL);     // free partition
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.40 nwalls=4"));
        check_room("3 walls + free partition", 2.4f, 0.0f);
        reset();
        wallseg(m, -2.0f, -1.5f, 2.0f, -1.5f, 1.3f); real_plane(10, m, 4.0f, 2.6f, KLSP_LBL_WALL);       // a whole 4 x 3 box ...
        wallseg(m, 2.0f, -1.5f, 2.0f, 1.5f, 1.3f);   real_plane(11, m, 3.0f, 2.6f, KLSP_LBL_WALL);
        wallseg(m, 2.0f, 1.5f, -2.0f, 1.5f, 1.3f);   real_plane(12, m, 4.0f, 2.6f, KLSP_LBL_WALL);
        wallseg(m, -2.0f, 1.5f, -2.0f, -1.5f, 1.3f); real_plane(13, m, 3.0f, 2.6f, KLSP_LBL_WALL);
        wallseg(m, 0.0f, -1.5f, 0.0f, 0.2f, 1.3f);   real_plane(20, m, 1.7f, 2.6f, KLSP_LBL_WALL);       // ... and a partition hanging from one wall
        horiz(m, 0, 0, 0, 0);    real_plane(1, m, 4, 3, KLSP_LBL_FLOOR);
        horiz(m, 0, 2.6f, 0, 1); real_plane(2, m, 4, 3, KLSP_LBL_CEILING);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.60 nwalls=4"));
        check_room("box + attached partition", 2.6f, 0.0f);
        // the same wall seen three times (exact duplicate, 2 cm off, a 1 m piece of it): one wall
        reset();
        walls3(2.6f);
        wallseg(m, -2.0f, -1.5f, 2.0f, -1.5f, 1.3f);   real_plane(21, m, 4.0f, 2.6f, KLSP_LBL_WALL);
        wallseg(m, -2.0f, -1.48f, 2.0f, -1.48f, 1.3f); real_plane(22, m, 4.0f, 2.6f, KLSP_LBL_WALL);
        wallseg(m, -0.5f, -1.5f, 0.5f, -1.5f, 1.3f);   real_plane(23, m, 1.0f, 2.6f, KLSP_LBL_WALL);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00 h=2.40 nwalls=4"));
        check_room("duplicated / overlapping walls", 2.4f, 0.0f);
    }
    // 40 walls (a 40-gon of 4 m radius): more than the old 32-wall array; 150 walls: more than the 128 looked at -> logged, no overflow
    reset();
    {
        float m[16];
        for (int k = 0; k < 40; k++) {
            float a0 = 6.2831853f * (float)k / 40.0f, a1 = 6.2831853f * (float)(k + 1) / 40.0f;
            float l = wallseg(m, 4.0f * cosf(a0), 4.0f * sinf(a0), 4.0f * cosf(a1), 4.0f * sinf(a1), 1.3f);
            real_plane(100 + k, m, l, 2.6f, KLSP_LBL_WALL);
        }
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=8.00x8.00") && !strstr(g_log, "only the first"));
        reset();
        for (int k = 0; k < 150; k++) {
            float a0 = 6.2831853f * (float)k / 150.0f, a1 = 6.2831853f * (float)(k + 1) / 150.0f;
            float l = wallseg(m, 20.0f * cosf(a0), 20.0f * sinf(a0), 20.0f * cosf(a1), 20.0f * sinf(a1), 1.3f);
            real_plane(100 + k, m, l, 2.6f, KLSP_LBL_WALL);
        }
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] outline: 150 real walls, only the first 128 used") && synth_planes() >= 5);
    }
    // two rooms, no RoomAnchor: each head gets the walls of its own room, never a mix
    reset();
    {
        float m[16];
        for (int r = 0; r < 2; r++) {
            float ox = r ? 20.0f : 0.0f, w = r ? 6.0f : 4.0f;
            wallseg(m, ox - w / 2, -1.5f, ox + w / 2, -1.5f, 1.3f); real_plane(10 + 10 * r, m, w, 2.6f, KLSP_LBL_WALL);
            wallseg(m, ox + w / 2, -1.5f, ox + w / 2, 1.5f, 1.3f);  real_plane(11 + 10 * r, m, 3.0f, 2.6f, KLSP_LBL_WALL);
            wallseg(m, ox + w / 2, 1.5f, ox - w / 2, 1.5f, 1.3f);   real_plane(12 + 10 * r, m, w, 2.6f, KLSP_LBL_WALL);
            wallseg(m, ox - w / 2, 1.5f, ox - w / 2, -1.5f, 1.3f);  real_plane(13 + 10 * r, m, 3.0f, 2.6f, KLSP_LBL_WALL);
        }
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=4.00x3.00"));
        cap_begin();
        CK(kl_spatial_synth_room(20.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=outline dims=6.00x3.00"));
    }

    // --- 13. A drawn L keeps its reflex corner (no convex fuse): 6 walls, the L's ~12 m2 (a hull would be 14), the 0.28 m chamfer at the inner
    //         corner merged into one vertex. A self-crossing drawing is refused and logged, then the next source decides.
    reset();
    guardian_off();
    {
        float lz[14] = {0, 0, 4, 0, 4, 2, 2.2f, 2, 2, 2.2f, 2, 4, 0, 4};
        CK(kl_guardian_set_boundary(lz, 7, KLG_MODE_ROOMSCALE, 0) == 1);
        cap_begin();
        CK(kl_spatial_synth_room(10.0f, 10.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=4.00x4.00 h=2.40 nwalls=6") && !strstr(g_log, "rejected"));
        kl_uuid rid = {{0}}, fl = {{0}}, ce = {{0}};
        uint32_t nf = 0;
        CK(kl_spatial_get(KLSP_ROOM, 0, &rid, NULL) && kl_spatial_room_info(rid, &fl, &ce, 0, NULL, NULL, 0, NULL, NULL));
        float a2 = poly_area2(fl, &nf);
        CK(nf == 6 && a2 > 23.5f && a2 < 25.0f);                          // 12.2 m2 (the merge moves the corner 10 cm); a hull is 14
        guardian_off();
        reset();
        float bow[8] = {0, 0, 3, 3, 3, 0, 0, 2.5f};
        CK(kl_guardian_set_boundary(bow, 4, KLG_MODE_ROOMSCALE, 0) >= 0);
        cap_begin();
        CK(kl_spatial_synth_room(1.0f, 1.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] drawn zone rejected: reason=self-crossing") && strstr(g_log, "[room] source=fallback"));
        guardian_off();
    }

    // --- 14. Redrawing the zone inside the same bounding box is another room: a rebuild (also in the live check, which now looks at the
    //         guardian's generation, not only the store's), with the other polygon.
    reset();
    {
        float sq[8] = {-1.5f, -1.5f, 1.5f, -1.5f, 1.5f, 1.5f, -1.5f, 1.5f}, tz[8] = {-1.5f, -1.5f, 1.5f, -1.5f, 1.5f, 1.5f, -0.5f, 1.5f};
        CK(kl_guardian_set_boundary(sq, 4, KLG_MODE_ROOMSCALE, 0) >= 0);
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=3.00x3.00"));
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(g_log[0] == 0);                                                   // same zone: nothing rebuilt
        CK(kl_guardian_set_boundary(tz, 4, KLG_MODE_ROOMSCALE, 0) >= 0);     // same box 3 x 3, a trapezoid
        cap_begin();
        CK(kl_spatial_synth_room(0.0f, 0.0f, 3.0f, 3.0f) == 1);
        cap_end();
        CK(strstr(g_log, "[room] source=guardian dims=3.00x3.00 h=2.40 nwalls=4"));
        uint64_t t0 = 5000ull * 1000000000ull;
        CK(kl_spatial_live_update_at(0, 0, 3, 3, t0) == 0);                  // first look after the build: nothing new
        CK(kl_guardian_set_boundary(sq, 4, KLG_MODE_ROOMSCALE, 0) >= 0);     // redrawn as the square again, store untouched
        cap_begin();
        CK(kl_spatial_live_update_at(0, 0, 3, 3, t0 + 3000000000ull) == 1);
        cap_end();
        CK(strstr(g_log, "room changed"));
        CK(kl_spatial_live_update_at(0, 0, 3, 3, t0 + 6000000000ull) == 0);
        guardian_off();
    }

    puts(fails ? "t_room_build: FAILED" : "t_room_build: ok");
    return fails != 0;
}
