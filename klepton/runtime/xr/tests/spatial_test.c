// Lote 3: kl_spatial.c + kl_ovrp_spatial.c against stubs of kl_ovrp.c / kl_jni.
// clang -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag \
//   tests/spatial_test.c kl_spatial.c kl_ovrp_spatial.c -lm -lpthread -o /tmp/spatial_test && /tmp/spatial_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include "kl_ovrp_priv.h"
#include "kl_spatial.h"
#include "kl_envdepth.h"

// ---- stubs ------------------------------------------------------------------
static float g_oy = 0.0f;
float kl_ovrp_guest_origin_y(void) { return g_oy; }
static float g_bw = 3.0f, g_bd = 3.0f;
int kl_ovrp_boundary(float *w, float *d) { *w = g_bw; *d = g_bd; return 1; }
static char g_dir[256];
const char *kl_jni_files_dir(void) { return g_dir; }
void kl_ovrp_hit(const char *n) { (void)n; }
static float g_head[3] = {1.5f, 1.6f, -0.5f};
void kl_ovrp_get_head_pose(float *px, float *py, float *pz, float *qx, float *qy, float *qz, float *qw) {
    *px = g_head[0]; *py = g_head[1]; *pz = g_head[2]; *qx = *qy = *qz = 0; *qw = 1;
}
int kl_ovrp_diag_ok(void) { return 1; }
ovrpResult kl_ovrp_ok(const char *n) { (void)n; return 0; }
ovrpResult kl_ovrp_unsupported(const char *n) { (void)n; return -1004; }
static struct { int type; unsigned char d[256]; size_t len; } g_ev[64];
static int g_nev;
int kl_ovrp_push_event(int32_t type, const void *p, size_t len) {
    if (g_nev >= 64) return -1;
    g_ev[g_nev].type = type; g_ev[g_nev].len = len; memcpy(g_ev[g_nev].d, p, len); g_nev++; return 0;
}
static const kl_ovrp_entry *g_tab; static size_t g_ntab;
void kl_ovrp_register(const kl_ovrp_entry *t, size_t n, const char *const *r) { g_tab = t; g_ntab = n; (void)r; }
void kl_ovrp_spatial_register(void);
static void *F(const char *name) { for (size_t i = 0; i < g_ntab; i++) if (!strcmp(g_tab[i].name, name)) return g_tab[i].fn; return NULL; }

// "[sp-sum]" sink (kl_ovrp_spatial.c): the lines it would write to stderr.
void kl_ovrp_sp_sum_set_sink(void (*sink)(const char *line));
void kl_ovrp_sp_sum_tick_at(uint64_t now_ns);
static char g_sum[8][1100];
static char g_sumlast[1100];
static int g_nsum;
static void sum_sink(const char *line) { snprintf(g_sumlast, sizeof g_sumlast, "%s", line); if (g_nsum < 8) snprintf(g_sum[g_nsum], sizeof g_sum[0], "%s", line); g_nsum++; }

#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int fails;
static kl_uuid U16(int n) { kl_uuid u; memset(&u, 0, sizeof u); u.b[0] = (uint8_t)n; u.b[15] = 0x7f; return u; }
static void horiz(float *m, float x, float y, float z, int down) { memset(m, 0, 64); m[0] = 1; m[5] = down ? -1.0f : 1.0f; m[10] = down ? -1.0f : 1.0f; m[15] = 1; m[12] = x; m[13] = y; m[14] = z; }   // ARKit frame: normal +Y (down: 180 deg about X)
static void ident(float *m, float x, float y, float z) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; m[12] = x; m[13] = y; m[14] = z; }

// kl_envdepth.c is not linked: environment depth reports itself unsupported.
int kl_envdepth_supported(void) { return 0; }
int kl_envdepth_create(int hr) { (void)hr; return -1; }
void kl_envdepth_destroy(void) {}
int kl_envdepth_created(void) { return 0; }
int kl_envdepth_start(void) { return -1; }
void kl_envdepth_stop(void) {}
void kl_envdepth_set_hand_removal(int on) { (void)on; }
int kl_envdepth_alloc(void) { return 0; }
void kl_envdepth_free(void) {}
int kl_envdepth_alloc_state(void) { return 0; }
uint64_t kl_envdepth_image(int i) { (void)i; return 0; }
int kl_envdepth_is_vulkan(void) { return 0; }
int kl_envdepth_acquire(kl_envdepth_frame *f) { (void)f; return -1; }
void kl_envdepth_rebase(kl_envdepth_frame *f, const float e[7]) { (void)f; (void)e; }

// host: completes anchors synchronously from a worker-less stub
static kl_uuid g_next_anchor;
static void h_create(uint64_t req, const float m[16]) {
    float c[16]; memcpy(c, m, 64);
    kl_spatial_anchor(g_next_anchor, 0, c, 1);
    kl_spatial_request_done(req, 1, g_next_anchor);
}
static void h_remove(uint64_t req, kl_uuid id) { float m[16]; ident(m, 0, 0, 0); (void)m; kl_spatial_anchor(id, 1, m, 0); kl_spatial_request_done(req, 1, id); }

typedef int32_t (*fn3)(uint64_t, uint64_t, uint64_t);
// stderr -> file while a block runs, so the [sp-call] / [sp-loc-bad] lines can be read back.
static int g_se = -1;
static char g_cap[24576];
static void cap_begin(void) {
    char p[300]; snprintf(p, sizeof p, "%s/stderr.txt", g_dir);
    fflush(stderr); g_se = dup(2);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(fd, 2); close(fd);
}
static void cap_end(void) {
    char p[300]; snprintf(p, sizeof p, "%s/stderr.txt", g_dir);
    fflush(stderr); dup2(g_se, 2); close(g_se);
    FILE *f = fopen(p, "r"); size_t n = f ? fread(g_cap, 1, sizeof g_cap - 1, f) : 0; if (f) fclose(f);
    g_cap[n] = 0;
}
static int count_str(const char *hay, const char *needle) {
    int n = 0; for (const char *q = hay; (q = strstr(q, needle)); q++) n++;
    return n;
}
// the first line of g_cap holding all of the given fragments (NULL-terminated list), copied to out
static int cap_line(char *out, size_t cap, const char *a, const char *b, const char *c) {
    for (const char *q = g_cap; *q; ) {
        const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q);
        char ln[512]; snprintf(ln, sizeof ln, "%.*s", (int)(l < sizeof ln - 1 ? l : sizeof ln - 1), q);
        if (strstr(ln, a) && (!b || strstr(ln, b)) && (!c || strstr(ln, c))) { snprintf(out, cap, "%s", ln); return 1; }
        q += l + (e ? 1 : 0);
    }
    return 0;
}
int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/kl_spatial_test_%d", (int)getpid());
    char cmd[300]; snprintf(cmd, sizeof cmd, "mkdir -p %s", g_dir); if (system(cmd)) return 2;
    kl_ovrp_spatial_register();
    CK(g_ntab >= 40);

    // --- coordinates: guest = recenter * arkit, Y minus the guest origin
    float a[16], g[16], back[16];
    ident(a, 1, 2, 3); g_oy = 1.6f;
    kl_spatial_to_guest(a, g);
    CK(fabsf(g[12] - 1) < 1e-6f && fabsf(g[13] - 0.4f) < 1e-5f && fabsf(g[14] - 3) < 1e-6f);
    kl_spatial_from_guest(g, back);
    CK(fabsf(back[13] - 2) < 1e-5f);
    float rc[16] = {0,0,-1,0, 0,1,0,0, 1,0,0,0, 5,0,0,1};       // 90 deg yaw + translate
    kl_spatial_set_recenter(rc);
    kl_spatial_to_guest(a, g); kl_spatial_from_guest(g, back);
    for (int i = 0; i < 16; i++) CK(fabsf(back[i] - a[i]) < 1e-5f);
    float id4[16]; ident(id4, 0, 0, 0); kl_spatial_set_recenter(id4); g_oy = 0;

    // --- unavailable: refuse
    uint64_t req = 99;
    typedef struct { int32_t b; ovrpPosef p; double t; } ci;
    ci info = {1, {{0,0,0,1},{0,1,0}}, 0};
    CK(((int32_t(*)(const ci *, uint64_t *))F("ovrp_CreateSpatialAnchor"))(&info, &req) == -1004 && req == 0);

    // --- plane (table) + floor + ceiling + 4 walls
    kl_spatial_set_available(1, 1, 0, 1);
    kl_spatial_host h = {h_create, h_remove}; kl_spatial_set_host(&h);
    float m[16];

    // --- capture with no plane/room data: synthetic room, event 100 SUCCESS, room query finds floor + walls
    setenv("KL_SPATIAL_CAPTURE_TIMEOUT_MS", "300", 1);
    g_nev = 0;
    CK(((int32_t(*)(const void *, uint64_t *))F("ovrp_RequestSceneCapture"))(&info, &req) == 0 && req);
    for (int i = 0; i < 40 && g_nev == 0; i++) usleep(100000);
    CK(g_nev == 1 && g_ev[0].type == 100);
    { int32_t r100 = -1; memcpy(&r100, g_ev[0].d + 8, 4); CK(r100 == 0); }
    {
        typedef struct { int32_t qt, mx; double to; int32_t loc, act, ft; void *ids; int32_t nids; void *comps; int32_t ncomps; } qi0;
        int32_t w6[1] = {6}; qi0 q0 = {0, 100, 0, 1, 0, 2, NULL, 0, w6, 1};
        struct { uint64_t space; kl_uuid u; } r0[16]; int32_t c0 = -1; uint64_t rq0 = 0;
        CK(((int32_t(*)(const qi0 *, uint64_t *))F("ovrp_QuerySpaces"))(&q0, &rq0) == 0);
        CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&rq0, 16, &c0, r0) == 0 && c0 >= 1);
        uint64_t rm0 = r0[0].space;
        typedef struct { kl_uuid floor, ceil; int32_t wcap, wcount; kl_uuid *walls; } rl0;
        rl0 l0; kl_uuid w0[8]; memset(&l0, 0, sizeof l0); l0.wcap = 8; l0.walls = w0;
        CK(((int32_t(*)(uint64_t *, rl0 *))F("ovrp_GetSpaceRoomLayout"))(&rm0, &l0) == 0 && l0.wcount == 4);
        float fw, fh2, gm[16]; int lb;
        CK(kl_spatial_plane_info(l0.floor, &fw, &fh2, &lb, 0, NULL, NULL) && lb == KLSP_LBL_FLOOR);
        CK(fabsf(fw - 4.0f) < 1e-4f && fabsf(fh2 - 4.0f) < 1e-4f);   // 3 x 3 play area + 0.5 m margin per side
        float cx = 0, cz = 0; int nw = 0;                       // 4 walls around the head XZ
        for (uint32_t i = 0; i < kl_spatial_count(KLSP_PLANE); i++) { kl_uuid pid; int l2;
            if (kl_spatial_get(KLSP_PLANE, i, &pid, gm) && kl_spatial_plane_info(pid, NULL, NULL, &l2, 0, NULL, NULL) && l2 == KLSP_LBL_WALL) { cx += gm[12]; cz += gm[14]; nw++; } }
        CK(nw == 4 && fabsf(cx / 4 - 1.5f) < 1e-4f && fabsf(cz / 4 + 0.5f) < 1e-4f);

        // component status: Set(enable) is stored per (space, component); unsupported stays off
        typedef int32_t (*fset)(uint64_t *, int32_t, int32_t, double, uint64_t *);
        typedef int32_t (*fget)(uint64_t *, int32_t, int32_t *, int32_t *);
        int32_t en = -1, pe = -1; uint64_t sr = 0, rmh = rm0;
        CK(((fget)F("ovrp_GetSpaceComponentStatus"))(&rmh, 6, &en, &pe) == 0 && en == 1);
        g_nev = 0;
        CK(((fset)F("ovrp_SetSpaceComponentStatus"))(&rmh, 6, 0, 0.0, &sr) == 0 && g_nev == 1 && g_ev[0].type == 50);
        { int32_t ee; memcpy(&ee, g_ev[0].d + 44, 4); CK(ee == 0); }
        CK(((fget)F("ovrp_GetSpaceComponentStatus"))(&rmh, 6, &en, &pe) == 0 && en == 0);
        CK(((fget)F("ovrp_GetSpaceComponentStatus"))(&rmh, 7, &en, &pe) == 0 && en == 1);   // other component untouched
        g_nev = 0;
        CK(((fset)F("ovrp_SetSpaceComponentStatus"))(&rmh, 6, 1, 0.0, &sr) == 0 && g_nev == 1);
        { int32_t ee; memcpy(&ee, g_ev[0].d + 44, 4); CK(ee == 1); }
        CK(((fget)F("ovrp_GetSpaceComponentStatus"))(&rmh, 6, &en, &pe) == 0 && en == 1);
        CK(((fset)F("ovrp_SetSpaceComponentStatus"))(&rmh, 3, 1, 0.0, &sr) == 0);          // Bounded2D on a room: unsupported
        CK(((fget)F("ovrp_GetSpaceComponentStatus"))(&rmh, 3, &en, &pe) == 0 && en == 0);
    }

    // one real TABLE plane must not evict the synthetic room: layout stays floor + 4 walls
    horiz(m, 0.5f, 0.75f, -1, 0); kl_spatial_plane(U16(3), 0, m, 1.2f, 0.8f, KLSP_LBL_TABLE, NULL, 0);
    CK(kl_spatial_count(KLSP_PLANE) == 7);
    {
        kl_uuid rid = {{0}}, fl, ce, ws[8]; uint32_t nw = 0;
        CK(kl_spatial_count(KLSP_ROOM) == 1 && kl_spatial_get(KLSP_ROOM, 0, &rid, NULL));
        CK(kl_spatial_room_info(rid, &fl, &ce, 8, ws, &nw, 0, NULL, NULL) && nw == 4 && memcmp(fl.b, "KLSYNTH", 7) == 0);
    }
    horiz(m, 0, 0, 0, 0); kl_spatial_plane(U16(1), 0, m, 4, 5, KLSP_LBL_FLOOR, NULL, 0);
    horiz(m, 0, 2.6f, 0, 1); kl_spatial_plane(U16(2), 0, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);
    horiz(m, 0.5f, 0.75f, -1, 0); kl_spatial_plane(U16(3), 0, m, 1.2f, 0.8f, KLSP_LBL_TABLE, NULL, 0);
    for (int i = 0; i < 4; i++) {
        ident(m, (i % 2 ? (i == 1 ? 2 : -2) : 0), 5.0f, (i % 2 ? 0 : (i == 0 ? -2.5f : 2.5f)));
        if (i % 2) { m[0] = 0; m[2] = 1; }                      // side walls run along Z: the four walls close a 4 x 5 ring
        m[4] = -m[2]; m[5] = 0; m[6] = m[0]; m[10] = 0; m[9] = -1;   // wall frame: Z down, normal (Y) left of the tangent; walls 2 and 3 face outward
        kl_spatial_plane(U16(10 + i), 0, m, i % 2 ? 5.0f : 4.0f, 2.6f, KLSP_LBL_WALL, NULL, 0);
        if (i == 2) CK(kl_spatial_count(KLSP_PLANE) == 12);     // real floor + ceiling + 3 walls: synthetic set (6) still there
        if (i == 3) CK(kl_spatial_count(KLSP_PLANE) == 7);      // real floor + ceiling + 4 closing walls: synthetic purged
    }
    CK(kl_spatial_count(KLSP_PLANE) == 7);
    CK(kl_spatial_count(KLSP_ROOM) == 1);                          // synthetic
    {   // the purge hands over the real walls turned inward and chained end -> start, like the synthetic ones
        float wx[4], wz[4], nx[4], nz[4], tx[4], tz[4], hl[4]; int nw = 0;
        for (uint32_t i = 0; i < kl_spatial_count(KLSP_PLANE) && nw < 4; i++) {
            kl_uuid pid; float gm[16], pw; int lb;
            if (!kl_spatial_get(KLSP_PLANE, i, &pid, gm) || !kl_spatial_plane_info(pid, &pw, NULL, &lb, 0, NULL, NULL) || lb != KLSP_LBL_WALL) continue;
            wx[nw] = gm[12]; wz[nw] = gm[14]; tx[nw] = gm[0]; tz[nw] = gm[2]; nx[nw] = gm[4]; nz[nw] = gm[6]; hl[nw] = pw * 0.5f; nw++;
        }
        CK(nw == 4);
        for (int k = 0; k < nw; k++) {
            CK(nx[k] * -wx[k] + nz[k] * -wz[k] > 0);                // normal toward the room centre (0, 0)
            CK(fabsf(nx[k] + tz[k]) < 1e-5f && fabsf(nz[k] - tx[k]) < 1e-5f);   // and left of the tangent
            int found = 0;
            for (int j = 0; j < nw; j++) {                          // this wall's end is the start of another one
                float ex = wx[k] + tx[k] * hl[k], ez = wz[k] + tz[k] * hl[k];
                found |= fabsf(ex - (wx[j] - tx[j] * hl[j])) < 1e-4f && fabsf(ez - (wz[j] - tz[j] * hl[j])) < 1e-4f;
            }
            CK(found);                                              // closed 4 x 5 ring: every end is the start of the next wall
        }
    }

    // --- query by component Bounded2D (3): 7 planes + 1 room? room has no Bounded2D
    typedef struct { int32_t qt, mx; double to; int32_t loc, act, ft; void *ids; int32_t nids; void *comps; int32_t ncomps; } qi;
    int32_t want[1] = {3};
    qi q = {0, 100, 0, 1, 0, 2, NULL, 0, want, 1};
    g_nev = 0;
    CK(((int32_t(*)(const qi *, uint64_t *))F("ovrp_QuerySpaces"))(&q, &req) == 0 && req);
    CK(g_nev == 2 && g_ev[0].type == 51 && g_ev[1].type == 52);
    int32_t cnt = -1;
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 0, &cnt, NULL) == 0 && cnt == 7);
    struct { uint64_t space; kl_uuid u; } res[16];
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 7);

    // room query by RoomLayout (6)
    want[0] = 6; g_nev = 0;
    CK(((int32_t(*)(const qi *, uint64_t *))F("ovrp_QuerySpaces"))(&q, &req) == 0);
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 1);
    uint64_t room = res[0].space;

    // room layout: floor, ceiling, 4 walls (two-call), container = 7 planes
    typedef struct { kl_uuid floor, ceil; int32_t wcap, wcount; kl_uuid *walls; } rl;
    rl lay; kl_uuid walls[8]; memset(&lay, 0, sizeof lay);
    lay.wcap = 8; lay.walls = walls;
    CK(((int32_t(*)(uint64_t *, rl *))F("ovrp_GetSpaceRoomLayout"))(&room, &lay) == 0);
    CK(lay.wcount == 4 && lay.floor.b[0] == 1 && lay.ceil.b[0] == 2);
    typedef struct { int32_t cap, count; void *p; } cl;
    kl_uuid mem[16]; cl cont = {16, 0, mem};
    CK(((int32_t(*)(uint64_t *, cl *))F("ovrp_GetSpaceContainer"))(&room, &cont) == 0 && cont.count == 7);

    // semantic label + boxes on the table plane
    uint64_t tbl = kl_spatial_handle_for(U16(3));
    char lbl[32]; cl sl = {32, 0, lbl};
    CK(((int32_t(*)(uint64_t *, cl *))F("ovrp_GetSpaceSemanticLabels"))(&tbl, &sl) == 0 && !strcmp(lbl, "TABLE") && sl.count == 6);
    float r2[4], b3[6];
    CK(((int32_t(*)(uint64_t *, float *))F("ovrp_GetSpaceBoundingBox2D"))(&tbl, r2) == 0);
    CK(fabsf(r2[0] + 0.6f) < 1e-5f && fabsf(r2[2] - 1.2f) < 1e-5f);
    CK(((int32_t(*)(uint64_t *, float *))F("ovrp_GetSpaceBoundingBox3D"))(&tbl, b3) == 0);
    CK(fabsf(b3[5] - 0.75f) < 1e-5f && fabsf(b3[2] + 0.75f) < 1e-5f);        // volume hangs 0.75 m down to the floor
    float poly[16]; cl bd = {8, 0, poly};
    CK(((int32_t(*)(uint64_t *, cl *))F("ovrp_GetSpaceBoundary2D"))(&tbl, &bd) == 0 && bd.count == 4);
    int32_t comps[8]; uint32_t nc = 0;
    CK(((int32_t(*)(uint64_t *, uint32_t, uint32_t *, int32_t *))F("ovrp_EnumerateSpaceSupportedComponents"))(&tbl, 8, &nc, comps) == 0 && nc == 4);

    // locate a plane: pose of the rectangle
    struct { uint64_t flags; ovrpPosef p; } loc;
    CK(((int32_t(*)(void *, uint64_t *, int32_t))F("ovrp_LocateSpace2"))(&loc, &tbl, 1) == 0);
    CK(loc.flags == 0xF && fabsf(loc.p.Position.y - 0.75f) < 1e-5f && fabsf(fabsf(loc.p.Orientation.w) - 0.70710678f) < 1e-4f);
    g_oy = 1.6f;
    CK(((int32_t(*)(void *, uint64_t *, int32_t))F("ovrp_LocateSpace2"))(&loc, &tbl, 0) == 0 && fabsf(loc.p.Position.y - (0.75f - 1.6f)) < 1e-5f);
    g_oy = 0;

    // raycast straight down onto the table
    float o[3] = {0.5f, 2.0f, -1}, d[3] = {0, -1, 0}, hp[3], hn[3]; kl_uuid hid;
    CK(kl_spatial_raycast(o, d, 10, hp, hn, &hid) && hid.b[0] == 3 && fabsf(hp[1] - 0.75f) < 1e-5f && fabsf(hn[1] - 1) < 1e-5f);
    float o2[3] = {1.5f, 2.0f, 0}; CK(kl_spatial_raycast(o2, d, 10, hp, hn, &hid) && hid.b[0] == 1);   // floor, not table

    // mesh: one triangle on the floor-ish, raycast + counts
    float mv[9] = {0,0,0, 1,0,0, 0,0,1}; uint32_t mt[3] = {0,1,2};
    ident(m, 10, 0.2f, 10); kl_spatial_available(KLSP_CAP_MESH);
    kl_spatial_mesh(U16(30), 0, m, mv, 3, mt, 1);
    uint64_t mh = kl_spatial_handle_for(U16(30));
    int32_t nvv = 0, nii = 0; float vout[9]; uint32_t iout[3];
    CK(((int32_t(*)(uint64_t, int32_t, int32_t *, float *, int32_t, int32_t *, uint32_t *))F("ovrp_GetSpaceTriangleMesh"))(mh, 3, &nvv, vout, 3, &nii, iout) == 0 && nvv == 3 && nii == 3);
    float om[3] = {10.2f, 3, 10.2f}; CK(kl_spatial_raycast(om, d, 10, hp, hn, &hid) && hid.b[0] == 30 && fabsf(hp[1] - 0.2f) < 1e-5f);

    // anchor: create -> event; not visible as saved until SaveSpace; query by Storable only after save
    kl_uuid a1 = U16(50); g_next_anchor = a1; g_nev = 0;
    CK(((int32_t(*)(const ci *, uint64_t *))F("ovrp_CreateSpatialAnchor"))(&info, &req) == 0 && req);
    CK(g_nev == 1 && g_ev[0].type == 49);
    uint64_t evreq, evspace; int32_t evres;
    memcpy(&evreq, g_ev[0].d, 8); memcpy(&evres, g_ev[0].d + 8, 4); memcpy(&evspace, g_ev[0].d + 16, 8);
    CK(evreq == req && evres == 0 && evspace == kl_spatial_handle_for(a1) && g_ev[0].d[24] == 50);
    CK(kl_spatial_count(KLSP_ANCHOR) == 1);
    uint64_t ah = evspace;
    CK(((int32_t(*)(void *, uint64_t *, int32_t))F("ovrp_LocateSpace2"))(&loc, &ah, 0) == 0 && fabsf(loc.p.Position.y - 1) < 1e-5f);
    want[0] = 1; q.mx = 100;
    CK(((int32_t(*)(const qi *, uint64_t *))F("ovrp_QuerySpaces"))(&q, &req) == 0);
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 0);
    g_nev = 0; uint64_t sreq = 0;
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t, uint64_t *))F("ovrp_SaveSpace"))(&ah, 1, 0, &sreq) == 0 && g_nev == 1 && g_ev[0].type == 53);
    int32_t sres; memcpy(&sres, g_ev[0].d + 16, 4); CK(sres == 0 && g_ev[0].len == 40);
    CK(((int32_t(*)(const qi *, uint64_t *))F("ovrp_QuerySpaces"))(&q, &req) == 0);
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 1);
    char path[300]; snprintf(path, sizeof path, "%s/anclas.json", g_dir);
    FILE *f = fopen(path, "r"); char js[256] = {0}; if (f) { fread(js, 1, 255, f); fclose(f); }
    CK(strstr(js, "\"32000000-0000-0000-0000-00000000007F\"") != NULL);   // uuid text starts with the first byte 0x32
    // Destroy keeps a saved anchor; Erase removes it with an event
    CK(((int32_t(*)(uint64_t *))F("ovrp_DestroySpace"))(&ah) == 0 && kl_spatial_count(KLSP_ANCHOR) == 1);
    g_nev = 0;
    CK(((int32_t(*)(uint64_t *, int32_t, uint64_t *))F("ovrp_EraseSpace"))(&ah, 1, &sreq) == 0);
    CK(g_nev == 1 && g_ev[0].type == 54 && kl_spatial_count(KLSP_ANCHOR) == 0 && !kl_spatial_anchor_saved(a1));
    // an unsaved anchor dies with DestroySpace
    kl_uuid a2 = U16(51); g_next_anchor = a2;
    CK(((int32_t(*)(const ci *, uint64_t *))F("ovrp_CreateSpatialAnchor"))(&info, &req) == 0 && kl_spatial_count(KLSP_ANCHOR) == 1);
    uint64_t h2 = kl_spatial_handle_for(a2);
    CK(((int32_t(*)(uint64_t *))F("ovrp_DestroySpace"))(&h2) == 0 && kl_spatial_count(KLSP_ANCHOR) == 0);

    // scene capture completes (planes exist: settle 1.5 s)
    g_nev = 0;
    CK(((int32_t(*)(const void *, uint64_t *))F("ovrp_RequestSceneCapture"))(NULL ? NULL : &info, &req) == 0 && req);
    for (int i = 0; i < 40 && g_nev == 0; i++) usleep(100000);
    CK(g_nev == 1 && g_ev[0].type == 100);

    // right after the capture: query by components with loc=1 (OVRPlugin Local) finds what has the component
    typedef int32_t (*fq)(const qi *, uint64_t *);
    typedef int32_t (*fr)(uint64_t *, int32_t, int32_t *, void *);
    want[0] = 3; q.loc = 1;
    CK(((fq)F("ovrp_QuerySpaces"))(&q, &req) == 0 && ((fr)F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 7);
    want[0] = 1000269000;                                                                          // TriangleMesh: the mesh from above
    CK(((fq)F("ovrp_QuerySpaces"))(&q, &req) == 0 && ((fr)F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 1);
    want[0] = 424242;                                                                              // nonexistent component
    CK(((fq)F("ovrp_QuerySpaces"))(&q, &req) == 0 && ((fr)F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 0);
    want[0] = 3; q.loc = 0;                                                                        // Invalid/unfilled storage location: served as Local
    CK(((fq)F("ovrp_QuerySpaces"))(&q, &req) == 0 && ((fr)F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 7);
    want[0] = 3; q.loc = 2;                                                                        // Cloud: not served
    CK(((fq)F("ovrp_QuerySpaces"))(&q, &req) == 0 && ((fr)F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 0);

    // handle by value works too
    kl_uuid ou;
    CK(((int32_t(*)(uint64_t, kl_uuid *))F("ovrp_GetSpaceUuid"))(tbl, &ou) == 0 && ou.b[0] == 3);

    // mesh removal and plane removal
    kl_spatial_mesh(U16(30), 1, m, NULL, 0, NULL, 0);
    CK(kl_spatial_count(KLSP_MESH) == 0);
    uint64_t h70 = 0; ovrpPosef p70;                              // a located plane that is retired later (cached locate)
    // --- diagnostics of locate: [sp-call] ok/why/kind/flags/pose/origin, [sp-loc-bad] (cap 10), window counters.
    // LocateSpace2 has been called 3 times so far (above), so the calls below are n=4.. and the loop reaches n=100.
    {
        typedef int32_t (*floc)(void *, uint64_t *, int32_t);
        floc L2 = (floc)F("ovrp_LocateSpace2"), L1 = (floc)F("ovrp_LocateSpace");
        char ln[512], hx[40], fr[64];
        unsigned l2n = 3;
        // an untracked anchor (flags 0x3), the real room that orphans the synthetic room's handle (why=pose)
        kl_uuid au = U16(52); g_next_anchor = au;
        CK(((int32_t(*)(const ci *, uint64_t *))F("ovrp_CreateSpatialAnchor"))(&info, &req) == 0);
        float am[16]; ident(am, 0, 1, 0); kl_spatial_anchor(au, 0, am, 0);
        uint64_t uh = kl_spatial_handle_for(au);
        kl_uuid synth_id; int synth_kind = 0;
        CK(kl_spatial_uuid_for_handle(room, &synth_id, &synth_kind) && synth_kind == KLSP_ROOM);
        kl_uuid realroom = U16(60);
        kl_spatial_room(realroom, 0, NULL, 0);
        CK(kl_spatial_uuid_for_handle(room, &synth_id, &synth_kind) && synth_kind == KLSP_ROOM);   // still a room by kind, but kl_spatial_get no longer lists it
        uint64_t unk = 0x7777, unk2 = 0x7777;
        cap_begin();
        // valid plane (tracked): ok=1 flags 0xf, its pose, the origin arg
        CK(L2(&loc, &tbl, 1) == 0 && ++l2n);
        // untracked anchor: flags 0x3
        CK(L2(&loc, &uh, 0) == 0 && loc.flags == 0x3 && ++l2n);
        // unknown handle: SUCCESS with flags 0, why=space
        loc.flags = 0xdead;
        CK(L2(&loc, &unk, 2) == 0 && loc.flags == 0 && ++l2n);
        CK(L2(&loc, &unk2, 2) == 0 && ++l2n);                    // same handle again: no second [sp-loc-bad]
        // known handle without a pose: why=pose
        uint64_t rh = room;
        CK(L2(&loc, &rh, 1) == 0 && loc.flags == 0 && ++l2n);
        // v1: failure is OPERATION_FAILED, success is a pose; both logged
        struct { ovrpPosef p; } p1;
        CK(L1(&p1, &unk, 1) != 0);
        CK(L1(&p1, &tbl, 1) == 0 && fabsf(p1.p.Position.y - 0.75f) < 1e-5f);
        // plan order (space, origin, out): origin is the second argument
        CK(((fn3)F("ovrp_LocateSpace2"))(tbl, 3, (uint64_t)(uintptr_t)&loc) == 0 && ++l2n);
        // a located plane that ARKit then retires: LocateSpace2 gives its last pose with flags 0x3, why=cached, no [sp-loc-bad]
        horiz(m, 0.3f, 0.9f, -0.4f, 0); kl_spatial_plane(U16(70), 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        h70 = kl_spatial_handle_for(U16(70));
        CK(L2(&loc, &h70, 1) == 0 && loc.flags == 0xF && ++l2n);                          // n=10
        p70 = loc.p;
        kl_spatial_plane(U16(70), 1, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        { kl_uuid tu; int tk = -1; CK(kl_spatial_uuid_for_handle(h70, &tu, &tk) && tk == 0); }   // the handle is known, the plane is gone
        loc.flags = 0xdead; memset(&loc.p, 0x55, sizeof loc.p);
        CK(L2(&loc, &h70, 2) == 0 && loc.flags == 0x3 && memcmp(&loc.p, &p70, sizeof p70) == 0 && ++l2n);   // n=11
        // 12 more distinct unknown handles, then fill up to the 100th call
        for (int i = 0; i < 12; i++) { uint64_t h = 0x8000 + i; CK(L2(&loc, &h, 1) == 0 && ++l2n); }
        for (; l2n < 100; ) { uint64_t h = 0x8000; CK(L2(&loc, &h, 1) == 0 && ++l2n); }
        cap_end();
        snprintf(hx, sizeof hx, "space=0x%llx ", (unsigned long long)tbl);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=4 ", hx, "ok=1 why=none kind=1 flags=0xf p=(0.50,0.75,-1.00) q=("));
        CK(strstr(ln, ") origin=1") != NULL);
        snprintf(hx, sizeof hx, "space=0x%llx ", (unsigned long long)uh);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=5 ", hx, "ok=1 why=none kind=3 flags=0x3 p=(0.00,1.00,0.00) q=("));
        CK(strstr(ln, ") origin=0") != NULL);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=6 ", "space=0x7777 ok=0 why=space kind=0 flags=0x0 ", "p=(0.00,0.00,0.00) q=(0.000,0.000,0.000,0.000) origin=2 ret=0"));
        snprintf(hx, sizeof hx, "space=0x%llx ", (unsigned long long)room);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=8 ", hx, "ok=0 why=pose kind=4 flags=0x0 "));
        CK(strstr(ln, "origin=1 ret=0") != NULL);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace ", "space=0x7777 ok=0 why=space ", "origin=1 ret="));
        CK(strstr(ln, "ret=0") == NULL);                        // v1: OPERATION_FAILED
        snprintf(hx, sizeof hx, "space=0x%llx ", (unsigned long long)tbl);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace ", hx, "ok=1 why=none kind=1 flags=0x0 p=(0.50,0.75,-1.00)"));
        CK(strstr(ln, "ret=0") != NULL);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=9 ", hx, "ok=1 why=none kind=1 flags=0xf p=(0.50,0.75,-1.00) q=(") && strstr(ln, "origin=3 ret=0"));   // plan order
        snprintf(fr, sizeof fr, "n=100 space=0x8000 ok=0 why=space");
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 ", fr, "origin=1 ret=0"));                         // the n=100 line carries the new fields too
        snprintf(hx, sizeof hx, "space=0x%llx ", (unsigned long long)h70);
        CK(cap_line(ln, sizeof ln, "fn=ovrp_LocateSpace2 n=11 ", hx, "ok=0 why=cached kind=0 flags=0x3 p=(0.30,0.90,-0.40) q=(") && strstr(ln, "origin=2 ret=0"));
        snprintf(hx, sizeof hx, "[sp-loc-bad] space=0x%llx ", (unsigned long long)h70);
        CK(count_str(g_cap, hx) == 0);                           // cached is not a failure
        // [sp-loc-bad]: once per distinct handle, why and kind, capped at 10 in total
        CK(count_str(g_cap, "[sp-loc-bad]") == 10);
        CK(count_str(g_cap, "[sp-loc-bad] space=0x7777 kind=0 why=space\n") == 1);
        snprintf(hx, sizeof hx, "[sp-loc-bad] space=0x%llx kind=4 why=pose\n", (unsigned long long)room);
        CK(count_str(g_cap, hx) == 1);
        CK(count_str(g_cap, "[sp-loc-bad] space=0x8000 ") == 1 && count_str(g_cap, "[sp-loc-bad] space=0x8007 ") == 1 && count_str(g_cap, "[sp-loc-bad] space=0x8008 ") == 0);
        // restore the state the rest of the test expects
        kl_spatial_room(realroom, 1, NULL, 0);
        CK(((int32_t(*)(uint64_t *))F("ovrp_DestroySpace"))(&uh) == 0 && kl_spatial_count(KLSP_ANCHOR) == 0);
        // a destroyed anchor, located OK before, is not served from the last-pose table: zeros / failure as before
        loc.flags = 0xdead; CK(L2(&loc, &uh, 0) == 0 && loc.flags == 0);
        { struct { ovrpPosef p; } pa; CK(L1(&pa, &uh, 0) != 0); }
        loc.flags = 0xdead; CK(L2(&loc, &ah, 0) == 0 && loc.flags == 0);              // the erased anchor from above too
    }
    // [sp-sum]: nothing before 5 s, one line after, a Poll-only line while nothing changes, then the PollEvent counters.
    {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t base = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, sec = 1000000000ull;
        kl_ovrp_sp_sum_set_sink(sum_sink);
        kl_ovrp_sp_sum_tick_at(base);
        CK(g_nsum == 0);
        kl_ovrp_sp_sum_tick_at(base + 6 * sec);
        CK(g_nsum == 1 && !strncmp(g_sum[0], "[sp-sum] t=", 11) && strstr(g_sum[0], " CreateSpatialAnchor=") && strstr(g_sum[0], " GetSpaceUuid=")
           && strstr(g_sum[0], "/0") && strlen(g_sum[0]) <= 900);
        kl_ovrp_sp_sum_tick_at(base + 12 * sec);
        CK(g_nsum == 2 && strstr(g_sum[1], " Poll=0") && !strchr(g_sum[1], '/'));   // nothing changed: the heartbeat, Poll alone
        kl_ovrp_sp_note_poll(-1); kl_ovrp_sp_note_poll(49); kl_ovrp_sp_note_poll(49);
        kl_ovrp_sp_sum_tick_at(base + 14 * sec);
        CK(g_nsum == 2);                                   // not yet 5 s after the last line
        kl_ovrp_sp_sum_tick_at(base + 20 * sec);
        CK(g_nsum == 3 && strstr(g_sum[2], " Poll=3 ev49=2") && !strstr(g_sum[2], "CreateSpatialAnchor"));
        // Heartbeat cap: 720 Poll-alone lines per process (one was written above); the next ones are dropped, a line with changes is not.
        int n0 = g_nsum;
        uint64_t t = base + 20 * sec;
        for (int i = 0; i < 719; i++) { t += 6 * sec; kl_ovrp_sp_sum_tick_at(t); }
        CK(g_nsum - n0 == 719);
        t += 6 * sec; kl_ovrp_sp_sum_tick_at(t);
        t += 6 * sec; kl_ovrp_sp_sum_tick_at(t);
        CK(g_nsum - n0 == 719);                            // heartbeat 721 and 722: silent
        kl_ovrp_sp_note_poll(50);
        t += 6 * sec; kl_ovrp_sp_sum_tick_at(t);
        CK(g_nsum - n0 == 720 && strstr(g_sumlast, " ev50=1"));   // changes still get their line
        // Window counters: the previous windows drained everything, so this line holds exactly these calls.
        {
            typedef int32_t (*floc)(void *, uint64_t *, int32_t);
            floc L2 = (floc)F("ovrp_LocateSpace2");
            uint64_t bad = 0x7778;
            int nl = g_nsum;
            CK(L2(&loc, &tbl, 1) == 0 && L2(&loc, &tbl, 1) == 0);                     // ok x2
            CK(L2(&loc, &bad, 1) == 0 && L2(&loc, &bad, 1) == 0 && L2(&loc, &bad, 1) == 0);   // bad space x3
            kl_spatial_room(U16(61), 0, NULL, 0);
            uint64_t rh = room;
            CK(L2(&loc, &rh, 1) == 0);                                                // bad pose x1
            kl_spatial_room(U16(61), 1, NULL, 0);
            CK(L2(&loc, &h70, 1) == 0 && L2(&loc, &h70, 1) == 0);                    // cached x2
            t += 6 * sec; kl_ovrp_sp_sum_tick_at(t);
            CK(g_nsum - nl == 1 && strstr(g_sumlast, " LocateSpace2=") && strstr(g_sumlast, " loc_ok=2 loc_bad_space=3 loc_bad_pose=1 loc_cached=2"));
            kl_ovrp_sp_note_poll(51);                                                  // (a Poll-alone line would be silent: heartbeat cap spent)
            t += 6 * sec; kl_ovrp_sp_sum_tick_at(t);
            CK(g_nsum - nl == 2 && strstr(g_sumlast, " ev51=1") && !strstr(g_sumlast, "loc_"));       // window reset: not repeated
        }
        kl_ovrp_sp_sum_set_sink(NULL);
    }
    // last pose per handle: v1, handles never located, the plane coming back, the table limit
    {
        typedef int32_t (*floc)(void *, uint64_t *, int32_t);
        floc L2 = (floc)F("ovrp_LocateSpace2"), L1 = (floc)F("ovrp_LocateSpace");
        struct { ovrpPosef p; } p1;
        CK(L1(&p1, &h70, 1) == 0 && memcmp(&p1.p, &p70, sizeof p70) == 0);          // v1: SUCCESS with the last pose
        horiz(m, 0.1f, 0.2f, 0.3f, 0); kl_spatial_plane(U16(72), 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        uint64_t h72 = kl_spatial_handle_for(U16(72));
        kl_spatial_plane(U16(72), 1, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);               // known handle, never located: no pose to give
        loc.flags = 0xdead;
        CK(L2(&loc, &h72, 1) == 0 && loc.flags == 0 && L1(&p1, &h72, 1) != 0);
        horiz(m, 0.3f, 0.9f, -0.4f, 0); kl_spatial_plane(U16(70), 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);   // comes back: the live pose again
        CK(L2(&loc, &h70, 1) == 0 && loc.flags == 0xF);
        // a plane the game destroys / erases stops answering from the table (also when ARKit retired it first)
        {
            uint64_t hd[3]; kl_uuid ud[3]; int32_t (*erases)(uint32_t, const uint64_t *, uint64_t, uint64_t, uint64_t *) = (void *)F("ovrp_EraseSpaces");
            uint64_t ereq = 0;
            for (int i = 0; i < 3; i++) {
                ud[i] = U16(80 + i); horiz(m, 0.1f * (float)i, 0.5f, 0, 0); kl_spatial_plane(ud[i], 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
                hd[i] = kl_spatial_handle_for(ud[i]);
                CK(L2(&loc, &hd[i], 1) == 0 && loc.flags == 0xF);
                kl_spatial_plane(ud[i], 1, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
                loc.flags = 0xdead; CK(L2(&loc, &hd[i], 1) == 0 && loc.flags == 0x3);         // cached before the game lets go
            }
            CK(((int32_t(*)(uint64_t *))F("ovrp_DestroySpace"))(&hd[0]) == 0);
            ((int32_t(*)(uint64_t *, int32_t, uint64_t *))F("ovrp_EraseSpace"))(&hd[1], 1, &ereq);
            erases(1, &hd[2], 0, 0, &ereq);
            for (int i = 0; i < 3; i++) { loc.flags = 0xdead; CK(L2(&loc, &hd[i], 1) == 0 && loc.flags == 0); }
            loc.flags = 0xdead; CK(L2(&loc, &h70, 1) == 0 && loc.flags == 0xF);              // other entries untouched (h70 is live again)
        }
        // table limit (8192 = the handle limit): locate planes until kl_spatial runs out of handles, retire them, ask again
        enum { NP = 8300 };
        static uint64_t hs[NP];
        int nh = 0;
        for (int i = 0; i < NP; i++) {
            kl_uuid u = U16(0); u.b[1] = (uint8_t)i; u.b[2] = (uint8_t)(i >> 8); u.b[3] = 0x5a;
            horiz(m, 0, 0.001f * (float)(i + 1), 0, 0); kl_spatial_plane(u, 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
            hs[i] = kl_spatial_handle_for(u);
            if (hs[i]) { CK(L2(&loc, &hs[i], 1) == 0 && loc.flags == 0xF); nh++; }
            kl_spatial_plane(u, 1, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        }
        CK(nh > 8000 && nh < NP && hs[NP - 1] == 0);                                      // handles ran out before the planes did
        int ncached = 0;
        for (int i = 0; i < NP; i++) {
            if (!hs[i]) continue;
            loc.flags = 0xdead;
            CK(L2(&loc, &hs[i], 1) == 0);
            if (loc.flags == 0x3) { ncached++; CK(fabsf(loc.p.Position.y - 0.001f * (float)(i + 1)) < 1e-6f); }
            else CK(loc.flags == 0);
        }
        CK(ncached == nh);                                                                // every located handle fits (the old 1024 limit is gone)
        // a handle already in the (full) table still updates its pose
        horiz(m, 0.4f, 0.8f, -0.2f, 0); kl_spatial_plane(U16(70), 0, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        CK(L2(&loc, &h70, 1) == 0 && loc.flags == 0xF);
        kl_spatial_plane(U16(70), 1, m, 1, 1, KLSP_LBL_TABLE, NULL, 0);
        CK(L2(&loc, &h70, 1) == 0 && loc.flags == 0x3 && fabsf(loc.p.Position.x - 0.4f) < 1e-6f && fabsf(loc.p.Position.y - 0.8f) < 1e-6f);
    }
    puts(fails ? "FALLA" : "OK");
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); if (system(cmd)) {}
    return fails != 0;
}
