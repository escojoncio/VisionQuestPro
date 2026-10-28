// Lote 3: kl_spatial.c + kl_ovrp_spatial.c against stubs of kl_ovrp.c / kl_jni.
// clang -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag \
//   tests/spatial_test.c kl_spatial.c kl_ovrp_spatial.c -lm -lpthread -o /tmp/spatial_test && /tmp/spatial_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "kl_ovrp_priv.h"
#include "kl_spatial.h"
#include "kl_envdepth.h"

// ---- stubs ------------------------------------------------------------------
static float g_oy = 0.0f;
float kl_ovrp_guest_origin_y(void) { return g_oy; }
static char g_dir[256];
const char *kl_jni_files_dir(void) { return g_dir; }
void kl_ovrp_hit(const char *n) { (void)n; }
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
    horiz(m, 0, 0, 0, 0); kl_spatial_plane(U16(1), 0, m, 4, 5, KLSP_LBL_FLOOR, NULL, 0);
    horiz(m, 0, 2.6f, 0, 1); kl_spatial_plane(U16(2), 0, m, 4, 5, KLSP_LBL_CEILING, NULL, 0);
    horiz(m, 0.5f, 0.75f, -1, 0); kl_spatial_plane(U16(3), 0, m, 1.2f, 0.8f, KLSP_LBL_TABLE, NULL, 0);
    for (int i = 0; i < 4; i++) { ident(m, (i % 2 ? (i == 1 ? 2 : -2) : 0), 5.0f, (i % 2 ? 0 : (i == 0 ? -2.5f : 2.5f))); kl_spatial_plane(U16(10 + i), 0, m, 4, 2.6f, KLSP_LBL_WALL, NULL, 0); }
    CK(kl_spatial_count(KLSP_PLANE) == 7);
    CK(kl_spatial_count(KLSP_ROOM) == 1);                          // synthetic

    // --- query by component Bounded2D (3): 7 planes + 1 room? room has no Bounded2D
    typedef struct { int32_t qt, mx; double to; int32_t loc, act, ft; void *ids; int32_t nids; void *comps; int32_t ncomps; } qi;
    int32_t want[1] = {3};
    qi q = {0, 100, 0, 0, 0, 2, NULL, 0, want, 1};
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
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t, uint64_t *))F("ovrp_SaveSpace"))(&ah, 0, 0, &sreq) == 0 && g_nev == 1 && g_ev[0].type == 53);
    int32_t sres; memcpy(&sres, g_ev[0].d + 16, 4); CK(sres == 0 && g_ev[0].len == 40);
    CK(((int32_t(*)(const qi *, uint64_t *))F("ovrp_QuerySpaces"))(&q, &req) == 0);
    CK(((int32_t(*)(uint64_t *, int32_t, int32_t *, void *))F("ovrp_RetrieveSpaceQueryResults"))(&req, 16, &cnt, res) == 0 && cnt == 1);
    char path[300]; snprintf(path, sizeof path, "%s/anclas.json", g_dir);
    FILE *f = fopen(path, "r"); char js[256] = {0}; if (f) { fread(js, 1, 255, f); fclose(f); }
    CK(strstr(js, "\"32000000-0000-0000-0000-00000000007F\"") != NULL);   // uuid text starts with the first byte 0x32
    // Destroy keeps a saved anchor; Erase removes it with an event
    CK(((int32_t(*)(uint64_t *))F("ovrp_DestroySpace"))(&ah) == 0 && kl_spatial_count(KLSP_ANCHOR) == 1);
    g_nev = 0;
    CK(((int32_t(*)(uint64_t *, int32_t, uint64_t *))F("ovrp_EraseSpace"))(&ah, 0, &sreq) == 0);
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

    // handle by value works too
    kl_uuid ou;
    CK(((int32_t(*)(uint64_t, kl_uuid *))F("ovrp_GetSpaceUuid"))(tbl, &ou) == 0 && ou.b[0] == 3);

    // mesh removal and plane removal
    kl_spatial_mesh(U16(30), 1, m, NULL, 0, NULL, 0);
    CK(kl_spatial_count(KLSP_MESH) == 0);
    puts(fails ? "FALLA" : "OK");
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); if (system(cmd)) {}
    return fails != 0;
}
