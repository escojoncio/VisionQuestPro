// OpenXR facade: XR_META_spatial_entity_discovery / room_mesh / semantic_label and the ovrp -> OpenXR anchor-create bridge,
// against stubs of kl_openxr.c / kl_envdepth.c.
// gcc -fsanitize=address,undefined -Wall -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag tests/t_xr_discovery.c kl_openxr_spatial.c kl_spatial.c kl_guardian.c -lm -lpthread -o /tmp/t_xr_discovery
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kl_openxr_priv.h"
#include "kl_spatial.h"
#include "kl_envdepth.h"

// ---- stubs ---------------------------------------------------------------------------------------------------------------------
float kl_ovrp_guest_origin_y(void) { return 0.0f; }
const char *kl_jni_files_dir(void) { return "/tmp"; }
int kl_envdepth_supported(void) { return 0; }
int kl_envdepth_create(int h) { (void)h; return -1; }
void kl_envdepth_destroy(void) {}
int kl_envdepth_created(void) { return 0; }
int kl_envdepth_start(void) { return -1; }
void kl_envdepth_stop(void) {}
void kl_envdepth_set_hand_removal(int on) { (void)on; }
int kl_envdepth_alloc(void) { return 0; }
void kl_envdepth_free(void) {}
uint64_t kl_envdepth_image(int i) { (void)i; return 0; }
int kl_envdepth_is_vulkan(void) { return 0; }
int kl_envdepth_acquire(kl_envdepth_frame *f) { (void)f; return -1; }
void kl_envdepth_rebase(kl_envdepth_frame *f, const float e[7]) { (void)f; (void)e; }
void *kl_openxr_lookup(const char *n) { (void)n; return NULL; }
void *kl_openxr_sym(const char *n) { (void)n; return NULL; }
static char g_sess;
void *klxr_current_session(void) { return &g_sess; }
const char *klxr_handle_class(void *h) { return h == (void *)&g_sess ? "session" : "stub-handle"; }
int64_t klxr_time_now(void) { return 1; }
static int g_enabled = 1;
int klxr_spatial_entity_enabled(void) { return g_enabled; }
static void *g_spaces[4096]; static int g_nsp;
void *klxr_space_create_external(void *user) { g_spaces[g_nsp] = user; return &g_spaces[g_nsp++]; }
static klxr_row_def g_rows[512]; static size_t g_nrows;
static struct { char name[64]; int (*gate)(void); const char *const *fns; } g_ext[64]; static int g_next;
void klxr_register_rows(const klxr_row_def *d, size_t n) { for (size_t i = 0; i < n; i++) g_rows[g_nrows++] = d[i]; }
void klxr_register_refused(const char *e, const char *const *n) { (void)e; (void)n; }
void klxr_register_extension(const char *name, uint32_t v, int (*gate)(void), const char *const *fns) {
    (void)v; snprintf(g_ext[g_next].name, 64, "%s", name); g_ext[g_next].gate = gate; g_ext[g_next].fns = fns; g_next++;
}
static struct { uint32_t type; uint8_t d[64]; size_t len; } g_ev[64]; static int g_nev;
int klxr_push_event(uint32_t type, const void *p, size_t len) {
    if (g_nev >= 64) return -1;
    g_ev[g_nev].type = type; g_ev[g_nev].len = len; memcpy(g_ev[g_nev].d, p, len); g_nev++; return 0;
}
// the last row registered under a name wins (as klxr_register_rows)
static void *ROW(const char *name) { void *r = NULL; for (size_t i = 0; i < g_nrows; i++) if (!strcmp(g_rows[i].name, name)) r = g_rows[i].fn; return r; }
static int EXT(const char *name) { for (int i = 0; i < g_next; i++) if (!strcmp(g_ext[i].name, name)) return i; return -1; }

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static kl_uuid U16(int n) { kl_uuid u; memset(&u, 0, sizeof u); u.b[0] = (uint8_t)n; u.b[15] = 0x7f; return u; }

// ---- wire structs (openxr.h layouts) ---------------------------------------------------------------------------------------------
typedef struct { int32_t type; const void *next; } Hdr;
typedef struct { int32_t type; const void *next; uint32_t n; const Hdr *const *filters; } DiscInfo;
typedef struct { int32_t type; const void *next; int32_t comp; } CompF;
typedef struct { int32_t type; const void *next; uint32_t n; _Alignas(8) const uint8_t (*uuids)[16]; } UuidF;
typedef struct { void *space; uint8_t uuid[16]; } Res;
typedef struct { int32_t type; const void *next; uint32_t cap, count; Res *res; } Results;
typedef struct { int32_t type; const void *next; uint32_t n; const int32_t *labels; } MeshInfo;
typedef struct { uint8_t uuid[16], parent[16]; int32_t label; } Face;
typedef struct { int32_t type; void *next; uint32_t vcap, vcount; float (*v)[3]; uint32_t fcap, fcount; Face *faces; } Mesh;
typedef int32_t (*fdisc)(void *, const DiscInfo *, uint64_t *);
typedef int32_t (*fret)(void *, uint64_t, Results *);
typedef int32_t (*fmesh)(void *, const MeshInfo *, Mesh *);

static void plane_at(int n, float x, float y, float z, float w, float h, int label) {
    float m[16] = {0};
    m[0] = 1; m[5] = 1; m[10] = 1; m[15] = 1; m[12] = x; m[13] = y; m[14] = z;
    kl_spatial_plane(U16(n), 0, m, w, h, label, NULL, 0);
}

int main(void) {
    kl_openxr_spatial_register();
    CK(EXT("XR_META_spatial_entity_discovery") >= 0 && EXT("XR_META_spatial_entity_semantic_label") >= 0 &&
       EXT("XR_META_spatial_entity_room_mesh") >= 0);
    int ie = EXT("XR_META_spatial_entity_discovery");
    CK(ie >= 0 && g_ext[ie].fns && g_ext[ie].fns[0] && !g_ext[ie].fns[2]);
    CK(EXT("XR_META_spatial_entity_semantic_label") >= 0 && g_ext[EXT("XR_META_spatial_entity_semantic_label")].fns == NULL);
    fdisc disc = (fdisc)ROW("xrDiscoverSpacesMETA");
    fret ret = (fret)ROW("xrRetrieveSpaceDiscoveryResultsMETA");
    fmesh mesh = (fmesh)ROW("xrGetSpaceRoomMeshMETA");
    CK(disc && ret && mesh);
    // the "feature unsupported" stub must not shadow them (it is registered after the real rows)
    int32_t (*fu)(void) = ROW("xrShareSpacesMETA");
    CK(fu && (void *)fu != (void *)disc && fu() == -8);
    CK(ROW("xrSaveSpacesMETA") && ((int32_t (*)(void))ROW("xrSaveSpacesMETA"))() == -8);   // persistence stays refused

    kl_spatial_set_policy(1, 1);
    kl_spatial_set_available(1, 1, 1, 1);
    CK(g_ext[ie].gate());
    void *S = &g_sess;
    uint64_t rq = 0;
    int32_t r;

    // nothing scanned: completes with 0 results, no room is invented
    DiscInfo di = { 1000247001, NULL, 0, NULL };
    r = disc(S, &di, &rq); CK(r == 0 && rq);
    Results out = { 1000247006, NULL, 0, 0, NULL };
    r = ret(S, rq, &out); CK(r == 0 && out.count == 0);
    CK(g_nev == 2 && g_ev[0].type == 1000247007 && g_ev[1].type == 1000247008);
    uint64_t evreq; memcpy(&evreq, g_ev[0].d, 8); CK(evreq == rq);
    memcpy(&evreq, g_ev[1].d, 8); int32_t evres; memcpy(&evres, g_ev[1].d + 8, 4); CK(evreq == rq && evres == 0);
    g_nev = 0;

    // a floor, a ceiling, 2 walls and a table; ARKit frame (normal +Y)
    plane_at(1, 0, 0, 0, 4, 5, KLSP_LBL_FLOOR);
    plane_at(2, 0, 2.5f, 0, 4, 5, KLSP_LBL_CEILING);
    plane_at(3, 2, 1.2f, 0, 5, 2.4f, KLSP_LBL_WALL);
    plane_at(4, -2, 1.2f, 0, 5, 2.4f, KLSP_LBL_WALL);
    plane_at(5, 0.5f, 0.75f, 0.5f, 1.2f, 0.8f, KLSP_LBL_TABLE);
    CK(kl_spatial_count(KLSP_PLANE) == 5);

    CompF cb = { 1000247004, NULL, 3 /* BOUNDED_2D */ };
    const Hdr *fl[2] = { (const Hdr *)&cb, NULL };
    di = (DiscInfo){ 1000247001, NULL, 1, fl };
    r = disc(S, &di, &rq); CK(r == 0);
    out = (Results){ 1000247006, NULL, 0, 0, NULL };
    r = ret(S, rq, &out); CK(r == 0 && out.count == 5);                 // 5 planes; the room has no BOUNDED_2D
    Res buf[16] = {{0}};
    out.cap = 4; out.res = buf; r = ret(S, rq, &out); CK(r == -11);      // too small: nothing lost
    out.cap = 16; r = ret(S, rq, &out); CK(r == 0 && out.count == 5);
    int labelled = 0;
    for (uint32_t i = 0; i < out.count; i++) if (buf[i].space && buf[i].uuid[15] == 0x7f) labelled++;
    CK(labelled == 5);
    r = ret(S, rq, &out); CK(r == 0 && out.count == 0);                 // handed out once; a second retrieve (two events) succeeds empty
    out = (Results){ 1000247006, NULL, 0, 0, NULL }; CK(ret(S, 987654, &out) != 0);   // never seen: validation
    CK(g_nev == 2 && g_ev[0].type == 1000247007 && g_ev[1].type == 1000247008);
    g_nev = 0;

    // semantic component filter (OR with the other component) + uuid filter (AND)
    CompF cs = { 1000247004, NULL, 5 /* SEMANTIC */ };
    uint8_t want[2][16]; memcpy(want[0], U16(3).b, 16); memcpy(want[1], U16(5).b, 16);
    UuidF uf = { 1000247003, NULL, 2, (const uint8_t (*)[16])want };
    const Hdr *fl2[3] = { (const Hdr *)&cs, (const Hdr *)&uf, NULL };
    di = (DiscInfo){ 1000247001, NULL, 2, fl2 };
    r = disc(S, &di, &rq); CK(r == 0);
    out = (Results){ 1000247006, NULL, 16, 0, buf };
    r = ret(S, rq, &out); CK(r == 0 && out.count == 2);
    UuidF none = { 1000247003, NULL, 0, NULL };
    const Hdr *fl3[2] = { (const Hdr *)&none, NULL };
    di = (DiscInfo){ 1000247001, NULL, 1, fl3 };
    r = disc(S, &di, &rq); CK(r == 0);
    out = (Results){ 1000247006, NULL, 16, 0, buf };
    r = ret(S, rq, &out); CK(r == 0 && out.count == 0);                  // an empty uuid list matches nothing
    g_nev = 0;
    // validation
    CK(disc(S, NULL, &rq) == -1);
    di = (DiscInfo){ 77, NULL, 0, NULL };  CK(disc(S, &di, &rq) == -1);   // wrong type
    Hdr bad = { 12345, NULL }; const Hdr *fl4[2] = { &bad, NULL };
    di = (DiscInfo){ 1000247001, NULL, 1, fl4 }; CK(disc(S, &di, &rq) == -1);   // unknown filter
    di = (DiscInfo){ 1000247001, NULL, 1, NULL }; CK(disc(S, &di, &rq) == -1);
    out = (Results){ 1000247006, NULL, 0, 0, NULL }; CK(ret(S, 999999, &out) != 0);
    out = (Results){ 5, NULL, 0, 0, NULL }; CK(ret(S, rq, &out) == -1);
    CK(disc((void *)&fails, &(DiscInfo){ 1000247001, NULL, 0, NULL }, &rq) == -12);   // not our session
    g_nev = 0;

    // the room: a synthetic one appears with planes; its mesh is the floor / ceiling / wall quads (no table)
    CompF cr = { 1000247004, NULL, 6 /* ROOM_LAYOUT */ };
    const Hdr *fl5[2] = { (const Hdr *)&cr, NULL };
    di = (DiscInfo){ 1000247001, NULL, 1, fl5 };
    r = disc(S, &di, &rq); CK(r == 0);
    out = (Results){ 1000247006, NULL, 16, 0, buf };
    r = ret(S, rq, &out); CK(r == 0 && out.count == 1);
    void *room = buf[0].space;

    // xrGetSpaceContainerFB(XrSession session, XrSpace space, XrSpaceContainerFB*): session + room -> the list; a bad space or session -> HANDLE_INVALID
    {
        typedef struct { int32_t type; const void *next; uint32_t cap, count; uint8_t (*uuids)[16]; } Cont;
        int32_t (*gc)(void *, void *, Cont *) = (int32_t (*)(void *, void *, Cont *))ROW("xrGetSpaceContainerFB");
        CK(gc != NULL);
        Cont c = { 1000199000, NULL, 0, 0, NULL };
        CK(gc(S, room, &c) == 0 && c.count == 5);                                   // size query: the 5 planes
        uint8_t ids[8][16];
        c = (Cont){ 1000199000, NULL, 8, 0, ids };
        CK(gc(S, room, &c) == 0 && c.count == 5);
        int found = 0;
        for (uint32_t i = 0; i < c.count; i++) for (int n = 1; n <= 5; n++) if (!memcmp(ids[i], U16(n).b, 16)) found++;
        CK(found == 5);                                                              // every plane listed once
        c = (Cont){ 1000199000, NULL, 2, 0, ids };
        CK(gc(S, room, &c) == -11 && c.count == 5);                                  // capacity too small
        c = (Cont){ 1000199000, NULL, 0, 0, NULL };
        CK(gc(S, &fails, &c) == -12);                                                // not a space
        CK(gc(S, NULL, &c) == -12);
        CK(gc(S, S, &c) == -12);                                                     // the session passed as the space (the old 2-argument shape)
        CK(gc((void *)&fails, room, &c) == -12);                                     // not our session
        CK(gc(S, room, NULL) == -1);
        c = (Cont){ 5, NULL, 0, 0, NULL };
        CK(gc(S, room, &c) == -1);                                                   // wrong struct type
    }
    MeshInfo mi = { 1000553001, NULL, 0, NULL };
    Mesh mm = { 1000553002, NULL, 0, 0, NULL, 0, 0, NULL };
    r = mesh(room, &mi, &mm); CK(r == 0 && mm.fcount == 4 && mm.vcount == 16);
    float vv[16][3]; Face ff[8];
    mm.vcap = 16; mm.v = vv; mm.fcap = 8; mm.faces = ff;
    r = mesh(room, &mi, &mm); CK(r == 0 && mm.fcount == 4);
    int nflo = 0, nwall = 0, nceil = 0;
    for (uint32_t i = 0; i < mm.fcount; i++) { nflo += ff[i].label == 1; nceil += ff[i].label == 2; nwall += ff[i].label == 3; }
    CK(nflo == 1 && nceil == 1 && nwall == 2);
    // floor quad: 4 x 5 in X/Z around the origin at y 0
    for (uint32_t i = 0; i < mm.fcount; i++) if (ff[i].label == 1) {
        float xs = 0, zs = 0;
        for (int c = 0; c < 4; c++) { CK(fabsf(vv[i*4+c][1]) < 1e-5f); xs += fabsf(vv[i*4+c][0]); zs += fabsf(vv[i*4+c][2]); }
        CK(fabsf(xs - 8.0f) < 1e-4f && fabsf(zs - 10.0f) < 1e-4f);
    }
    for (uint32_t i = 0; i < mm.fcount; i++) {
        uint32_t ix[6]; typedef struct { int32_t type; void *next; uint32_t cap, count; uint32_t *indices; } FI;
        FI fi = { 1000553000, NULL, 6, 0, ix };
        int32_t (*fidx)(void *, const void *, FI *) = ROW("xrGetSpaceRoomMeshFaceIndicesMETA");
        CK(fidx && fidx(room, ff[i].uuid, &fi) == 0 && fi.count == 6 && ix[0] == i * 4 && ix[5] == i * 4 + 3);
    }
    int32_t only_floor[1] = { 1 };
    mi.n = 1; mi.labels = only_floor;
    mm.vcap = mm.fcap = 0;
    r = mesh(room, &mi, &mm); CK(r == 0 && mm.fcount == 1 && mm.vcount == 4);
    mm.vcap = 2; mm.fcap = 1; r = mesh(room, &mi, &mm); CK(r == -11);
    mm.vcap = mm.fcap = 0; mi.n = 0; mi.labels = NULL;
    // a plane is not a room
    CompF cb2 = { 1000247004, NULL, 3 };
    const Hdr *fl6[2] = { (const Hdr *)&cb2, NULL };
    di = (DiscInfo){ 1000247001, NULL, 1, fl6 };
    disc(S, &di, &rq); out = (Results){ 1000247006, NULL, 16, 0, buf }; ret(S, rq, &out);
    CK(mesh(buf[0].space, &mi, &mm) == -1000113000);
    g_nev = 0;

    // ---- anchor-create bridge (ovrp -> OpenXR queue) ----------------------------------------------------------------------------
    kl_uuid aid = U16(60);
    float am[16] = {0}; am[0] = am[5] = am[10] = am[15] = 1; am[13] = 1;
    kl_spatial_anchor(aid, 0, am, 1);
    g_enabled = 1;
    kl_spatial_notify_create(4242, 1, aid);
    CK(g_nev == 1 && g_ev[0].type == 1000113005);
    uint64_t breq; int32_t bres; void *bsp; uint8_t buu[16];
    memcpy(&breq, g_ev[0].d, 8); memcpy(&bres, g_ev[0].d + 8, 4); memcpy(&bsp, g_ev[0].d + 16, 8); memcpy(buu, g_ev[0].d + 24, 16);
    CK(breq == 4242 && bres == 0 && bsp && memcmp(buu, aid.b, 16) == 0);
    kl_spatial_notify_create(4242, 1, aid);                              // same requestId: not delivered twice
    CK(g_nev == 1);
    kl_spatial_notify_create(4243, 0, aid);                              // a failed create: result != 0, no space
    CK(g_nev == 2);
    memcpy(&breq, g_ev[1].d, 8); memcpy(&bres, g_ev[1].d + 8, 4); memcpy(&bsp, g_ev[1].d + 16, 8);
    CK(breq == 4243 && bres != 0 && bsp == NULL);
    g_enabled = 0;                                                       // the guest never enabled XR_FB_spatial_entity
    kl_spatial_notify_create(4244, 1, aid);
    CK(g_nev == 2);

    printf(fails ? "t_xr_discovery: %d FAILED\n" : "t_xr_discovery: all OK\n", fails);
    return fails != 0;
}
