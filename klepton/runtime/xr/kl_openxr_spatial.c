// kl_openxr_spatial.c - Lote 4: OpenXR spatial/scene/passthrough/depth extensions
// (XR_FB / META / EXT) over kl_spatial.h. Registered with kl_openxr.c through
// kl_openxr_spatial_register (see kl_openxr_priv.h for the recipe).
//
// Model: ONE entity registry (ent_t) keyed by uuid. An entity is a view of a
// kl_spatial item (ARKit plane, mesh, WorldAnchor, room). Anchors live in the
// common kl_spatial store (shared with the ovrp facade; persistence = WorldAnchor
// + anclas.json). Each entity owns at most one XrSpace; its pose is answered by
// kl_openxr_spatial_pose (the hook kl_openxr.c calls from xrLocateSpace, see
// klxr_space_create_external in kl_openxr_priv.h).
//
// Offline only. Async calls (create/save/erase/query/capture/status) complete
// inside the call and queue their completion event with klxr_push_event, which
// the runtime delivers from the poll AFTER the one that returned the request id.
//
// Struct type / event numbers and layouts were verified against the Khronos
// OpenXR-SDK openxr.h. KL_XR_SPATIAL_LAX=1 still skips every input type check.
#include "kl_openxr_priv.h"
#include "kl_spatial.h"
#include "kl_envdepth.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------- hooks
// klxr_space_create_external / kl_openxr_spatial_* are declared in kl_openxr_priv.h.

// ---------------------------------------------------------------- types
typedef struct { int32_t type; const void *next; } XrHdr;
typedef struct { uint8_t data[16]; } XrUuidEXT;
typedef struct { float q[4], p[3]; } KPose;                    // == XrPosef
typedef struct { int32_t type; const void *next; uint64_t flags; KPose pose; } KSpaceLoc;
typedef uint64_t XrAsyncRequestIdFB;
typedef uint32_t XrBool32;

enum {
    R_OK = 0, R_VALIDATION = -1, R_RUNTIME = -2, R_FUNC_UNSUP = -7, R_FEATURE_UNSUP = -8,
    R_LIMIT = -10, R_SIZE = -11, R_HANDLE = -12, R_POSE_INVALID = -39,
    R_COMP_UNSUP = -1000113000, R_COMP_NOT_ENABLED = -1000113001,
    R_COMP_ALREADY_SET = -1000113003, R_CLOUD_DISABLED = -1000169004,
};
enum {   // XrStructureType / event types
    T_SPATIAL_ANCHOR_CREATE_INFO = 1000113003, T_SYS_SPATIAL_ENTITY = 1000113004,
    T_COMP_STATUS = 1000113001, T_COMP_STATUS_SET_INFO = 1000113007,
    E_ANCHOR_CREATE_COMPLETE = 1000113005, E_SET_STATUS_COMPLETE = 1000113006,
    T_QUERY_INFO = 1000156001, T_QUERY_RESULTS = 1000156002, T_FILTER_STORAGE = 1000156003,
    T_FILTER_COMPONENT = 1000156052, T_FILTER_UUID = 1000156054,
    E_QUERY_RESULTS_AVAILABLE = 1000156103, E_QUERY_COMPLETE = 1000156104,
    T_SAVE_INFO = 1000158000, T_ERASE_INFO = 1000158001,
    E_SAVE_COMPLETE = 1000158106, E_ERASE_COMPLETE = 1000158107,
    T_LIST_SAVE_INFO = 1000238000, E_LIST_SAVE_COMPLETE = 1000238001,
    T_USER_CREATE_INFO = 1000241001,
    T_SEMANTIC_LABELS = 1000175000, T_ROOM_LAYOUT = 1000175001, T_BOUNDARY2D = 1000175002,
    T_SCENE_CAPTURE_REQUEST = 1000198050, E_SCENE_CAPTURE_COMPLETE = 1000198001,
    T_SPACE_CONTAINER = 1000199000,
    T_TRIMESH_GET_INFO = 1000269001, T_TRIMESH = 1000269002,
    T_TRIANGLE_MESH_CREATE_INFO = 1000117001, T_SYS_PLANE_DETECTION = 1000429007,
};
enum {   // XrSpaceComponentTypeFB
    C_LOCATABLE = 0, C_STORABLE = 1, C_SHARABLE = 2, C_BOUNDED_2D = 3, C_BOUNDED_3D = 4,
    C_SEMANTIC = 5, C_ROOM_LAYOUT = 6, C_CONTAINER = 7, C_TRIMESH_BIT = 8,
    C_TRIMESH_META = 1000269000,
};
#define KIND_ROOM_SYNTH 101   // room derived from the planes when ARKit has no room

static int g_lax(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("KL_XR_SPATIAL_LAX"); v = e && *e == '1'; }
    return v;
}
#define CHK_TYPE(p, t) do { if ((p)->type != (t) && !g_lax()) return R_VALIDATION; } while (0)

static void once_log(int *flag, const char *msg) {
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) return;
    fprintf(stderr, "  [xr] spatial: %s\n", msg);
}
#define LOG1(msg) do { static int _f; once_log(&_f, (msg)); } while (0)

static int sess_ok(void *s) { return s && s == klxr_current_session(); }

// ---------------------------------------------------------------- math
static void qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0]) };
    o[0] = v[0] + q[3]*t[0] + q[1]*t[2] - q[2]*t[1];
    o[1] = v[1] + q[3]*t[1] + q[2]*t[0] - q[0]*t[2];
    o[2] = v[2] + q[3]*t[2] + q[0]*t[1] - q[1]*t[0];
}
static KPose pose_mul(KPose a, KPose b) {
    KPose o; float r[3];
    qmul(a.q, b.q, o.q);
    qrot(a.q, b.p, r);
    for (int i = 0; i < 3; i++) o.p[i] = a.p[i] + r[i];
    return o;
}
// Column-major 4x4 (translation in m[12..14]) -> pose.
static KPose pose_from_m4(const float m[16]) {
    KPose o;
    float r00 = m[0], r10 = m[1], r20 = m[2], r01 = m[4], r11 = m[5], r21 = m[6],
          r02 = m[8], r12 = m[9], r22 = m[10];
    float tr = r00 + r11 + r22;
    if (tr > 0) { float s = sqrtf(tr + 1.f) * 2; o.q[3] = s/4; o.q[0] = (r21-r12)/s; o.q[1] = (r02-r20)/s; o.q[2] = (r10-r01)/s; }
    else if (r00 > r11 && r00 > r22) { float s = sqrtf(1.f+r00-r11-r22)*2; o.q[3] = (r21-r12)/s; o.q[0] = s/4; o.q[1] = (r01+r10)/s; o.q[2] = (r02+r20)/s; }
    else if (r11 > r22) { float s = sqrtf(1.f+r11-r00-r22)*2; o.q[3] = (r02-r20)/s; o.q[0] = (r01+r10)/s; o.q[1] = s/4; o.q[2] = (r12+r21)/s; }
    else { float s = sqrtf(1.f+r22-r00-r11)*2; o.q[3] = (r10-r01)/s; o.q[0] = (r02+r20)/s; o.q[1] = (r12+r21)/s; o.q[2] = s/4; }
    float n = sqrtf(o.q[0]*o.q[0]+o.q[1]*o.q[1]+o.q[2]*o.q[2]+o.q[3]*o.q[3]);
    if (n > 0) for (int i = 0; i < 4; i++) o.q[i] /= n; else o.q[3] = 1;
    o.p[0] = m[12]; o.p[1] = m[13]; o.p[2] = m[14];
    return o;
}
// ARKit planes have their normal on +Y; Meta/EXT planes on +Z with the 2D rect in
// XY. Rx(-90deg): Meta x = ARKit x, Meta y = -ARKit z, Meta z = ARKit y.
static KPose plane_to_meta(KPose p) {
    const float s = 0.70710678f;
    KPose r = { {-s, 0, 0, s}, {0, 0, 0} };
    return pose_mul(p, r);
}

// ---------------------------------------------------------------- entity registry
#define ENT_MAX 2048
typedef struct {
    int used, kind, erased, storable_off;
    kl_uuid id;
    void *handle;         // the XrSpace, NULL until asked for
} ent_t;
static ent_t g_ent[ENT_MAX];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_req_next = 1;

static XrAsyncRequestIdFB next_req(void) { return __atomic_fetch_add(&g_req_next, 1, __ATOMIC_RELAXED); }

static void ensure_listener(void);
static void ev_store(int32_t type, uint64_t req, int32_t res, ent_t *e, int loc);

static ent_t *ent_find(kl_uuid id) {
    for (int i = 0; i < ENT_MAX; i++)
        if (g_ent[i].used && memcmp(&g_ent[i].id, &id, 16) == 0) return &g_ent[i];
    return NULL;
}
static ent_t *ent_get(int kind, kl_uuid id) {
    pthread_mutex_lock(&g_mu);
    ent_t *e = ent_find(id);
    if (!e) {
        for (int i = 0; i < ENT_MAX && !e; i++)
            if (!g_ent[i].used) {
                e = &g_ent[i];
                memset(e, 0, sizeof *e);
                e->used = 1; e->kind = kind; e->id = id;
            }
        if (!e) fprintf(stderr, "  [xr] spatial: entity table full (%d)\n", ENT_MAX);
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}
static ent_t *ent_of_space(void *h) {
    if (!h) return NULL;
    pthread_mutex_lock(&g_mu);
    ent_t *r = NULL;
    for (int i = 0; i < ENT_MAX; i++)
        if (g_ent[i].used && g_ent[i].handle == h) { r = &g_ent[i]; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
static void *space_for(ent_t *e) {
    pthread_mutex_lock(&g_mu);
    if (!e->handle) e->handle = klxr_space_create_external(e);
    void *h = e->handle;
    pthread_mutex_unlock(&g_mu);
    return h;
}
void kl_openxr_spatial_space_destroyed(void *user) {
    ent_t *e = user;
    if (e < g_ent || e >= g_ent + ENT_MAX) return;
    pthread_mutex_lock(&g_mu);
    e->handle = NULL;
    int drop = e->used && !e->erased && e->kind == KLSP_ANCHOR;
    pthread_mutex_unlock(&g_mu);
    // Quest semantics (as the ovrp facade): an anchor never saved dies with its handle.
    if (drop && !kl_spatial_anchor_saved(e->id)) {
        ensure_listener();
        (void)kl_spatial_remove_anchor(kl_spatial_new_request(), e->id);
    }
}

static kl_uuid synth_room_id(void) { kl_uuid u = {{'K','L','r','o','o','m',0,0,0,0,0,0,0,0,0,1}}; return u; }

// kl_spatial lookups (return values of the readers are not relied on: outputs are
// pre-set to sentinels).
static int sp_find(int kind, kl_uuid id, float m4[16]) {
    uint32_t n = kl_spatial_count(kind);
    for (uint32_t i = 0; i < n; i++) {
        kl_uuid u; float m[16];
        memset(&u, 0, sizeof u);
        kl_spatial_get(kind, i, &u, m);
        if (memcmp(&u, &id, 16) == 0) { if (m4) memcpy(m4, m, sizeof m); return 1; }
    }
    return 0;
}
static const char *label_str(int l) {
    switch (l) {
        case KLSP_LBL_FLOOR: return "FLOOR"; case KLSP_LBL_CEILING: return "CEILING";
        case KLSP_LBL_WALL: return "WALL_FACE"; case KLSP_LBL_TABLE: return "TABLE";
        case KLSP_LBL_COUCH: return "COUCH"; case KLSP_LBL_DOOR: return "DOOR_FRAME";
        case KLSP_LBL_WINDOW: return "WINDOW_FRAME"; default: return "OTHER";
    }
}
static int plane_geom(kl_uuid id, float *w, float *h, int *label) {
    float ww = 0, hh = 0; int l = KLSP_LBL_OTHER; uint32_t np = 0;
    kl_spatial_plane_info(id, &ww, &hh, &l, 0, NULL, &np);
    if (w) *w = ww; if (h) *h = hh; if (label) *label = l;
    return ww > 0 || hh > 0;
}

// Pose of an entity in the tracking space; 0 when it is gone/lost.
int kl_openxr_spatial_pose(void *user, float q[4], float p[3]) {
    ent_t *e = user;
    if (e < g_ent || e >= g_ent + ENT_MAX || !e->used || e->erased) return 0;
    KPose k = {{0,0,0,1},{0,0,0}};
    float m[16];
    switch (e->kind) {
        case KIND_ROOM_SYNTH: break;
        case KLSP_PLANE: if (!sp_find(KLSP_PLANE, e->id, m)) return 0; k = plane_to_meta(pose_from_m4(m)); break;
        case KLSP_ANCHOR: if (!sp_find(KLSP_ANCHOR, e->id, m)) return 0; k = pose_from_m4(m); break;
        default: if (!sp_find(e->kind, e->id, m)) return 0; k = pose_from_m4(m); break;
    }
    memcpy(q, k.q, 16); memcpy(p, k.p, 12);
    return e->kind == KLSP_ANCHOR ? kl_spatial_anchor_tracked(e->id) : 1;
}

// Components an entity can ever have, and which are enabled now.
static uint32_t comp_bit(int32_t t) { return t == C_TRIMESH_META ? 1u << C_TRIMESH_BIT : (t >= 0 && t <= 7) ? 1u << t : 0; }
static uint32_t comp_supported(const ent_t *e) {
    switch (e->kind) {
        case KLSP_ANCHOR: return comp_bit(C_LOCATABLE) | comp_bit(C_STORABLE);
        case KLSP_PLANE: return comp_bit(C_LOCATABLE) | comp_bit(C_BOUNDED_2D) | comp_bit(C_SEMANTIC);
        case KLSP_MESH: return comp_bit(C_LOCATABLE) | comp_bit(C_BOUNDED_3D) | comp_bit(C_SEMANTIC) | comp_bit(C_TRIMESH_META);
        default: return comp_bit(C_LOCATABLE) | comp_bit(C_ROOM_LAYOUT) | comp_bit(C_CONTAINER);   // rooms
    }
}
static uint32_t comp_enabled(const ent_t *e) {
    uint32_t m = comp_supported(e);
    if (e->storable_off) m &= ~comp_bit(C_STORABLE);
    return m;
}
static const int32_t k_comp_order[] = { C_LOCATABLE, C_STORABLE, C_BOUNDED_2D, C_BOUNDED_3D, C_SEMANTIC,
                                         C_ROOM_LAYOUT, C_CONTAINER, C_TRIMESH_META };

static ent_t *space_ent(void *space) { return ent_of_space(space); }

// All live entities (registering kl_spatial items on the way). Caller frees.
static int ent_all(ent_t ***out) {
    int cap = 64, n = 0;
    ent_t **v = malloc(cap * sizeof *v);
    if (!v) { *out = NULL; return 0; }
#define ADD(e) do { ent_t *_e = (e); if (_e && !_e->erased) { if (n == cap) { cap *= 2; ent_t **nv = realloc(v, cap * sizeof *v); if (!nv) goto done; v = nv; } v[n++] = _e; } } while (0)
    static const int kinds[] = { KLSP_ANCHOR, KLSP_PLANE, KLSP_MESH, KLSP_ROOM };
    for (int k = 0; k < 4; k++) {
        uint32_t c = kl_spatial_count(kinds[k]);
        for (uint32_t i = 0; i < c; i++) {
            kl_uuid u; float m[16]; memset(&u, 0, sizeof u);
            kl_spatial_get(kinds[k], i, &u, m);
            ADD(ent_get(kinds[k], u));
        }
    }
    if (kl_spatial_count(KLSP_ROOM) == 0 && kl_spatial_count(KLSP_PLANE) > 0) {
        ent_t *r = ent_get(KIND_ROOM_SYNTH, synth_room_id());
        ADD(r);
    }
done:
#undef ADD
    *out = v;
    return n;
}

// ---------------------------------------------------------------- events
static void push(int32_t type, void *ev, size_t size) {
    if (klxr_push_event((uint32_t)type, (char *)ev + 16, size - 16) != 0)
        fprintf(stderr, "  [xr] spatial: event queue full, %d dropped\n", type);
}
typedef struct { int32_t type; const void *next; uint64_t req; int32_t result, _p; void *space; XrUuidEXT uuid; } EvAnchor;
typedef struct { EvAnchor a; int32_t comp; XrBool32 enabled; } EvStatus;
typedef struct { EvAnchor a; int32_t location, _p2; } EvStore;
typedef struct { int32_t type; const void *next; uint64_t req; int32_t result, _p; } EvReq;

static void ev_anchor(int32_t type, uint64_t req, int32_t res, ent_t *e) {
    EvAnchor ev = { type, NULL, req, res, 0, e ? space_for(e) : NULL, {{0}} };
    if (e) memcpy(&ev.uuid, &e->id, 16);
    push(type, &ev, sizeof ev);
}

// ---------------------------------------------------------------- locating other spaces
typedef XrResult (*pfn_locate)(void *, void *, int64_t, void *);
typedef XrResult (*pfn_crs)(void *, const void *, void **);
static void *stage_space(void) {
    static void *stage;
    if (stage) return stage;
    pfn_crs crs = (pfn_crs)kl_openxr_lookup("xrCreateReferenceSpace");
    struct { int32_t type; const void *next; int32_t ref; KPose pose; } ci = { 37, NULL, 3, {{0,0,0,1},{0,0,0}} };
    void *s = NULL;
    if (crs && crs(klxr_current_session(), &ci, &s) == R_OK) stage = s;
    return stage;
}
// pose of `space` in `base` at `t`. 0 when untracked.
static int locate_in(void *space, void *base, int64_t t, KPose *out) {
    pfn_locate loc = (pfn_locate)kl_openxr_lookup("xrLocateSpace");
    if (!loc || !base || !space) return 0;
    KSpaceLoc l = { 42, NULL, 0, {{0,0,0,1},{0,0,0}} };
    if (loc(space, base, t > 0 ? t : klxr_time_now(), &l) != R_OK) return 0;
    if ((l.flags & 3) != 3) return 0;
    *out = l.pose;
    return 1;
}

// ---------------------------------------------------------------- async requests (common kl_spatial store)
enum { P_CREATE = 1, P_ERASE = 2 };
typedef struct { uint64_t req; int kind; int32_t location; } fpend;
static fpend g_fp[64];
static unsigned g_nfp;
static pthread_mutex_t g_fp_mu = PTHREAD_MUTEX_INITIALIZER;
static void pend_add(uint64_t req, int kind, int32_t loc) {
    pthread_mutex_lock(&g_fp_mu);
    if (g_nfp == 64) g_fp[0] = g_fp[--g_nfp];
    g_fp[g_nfp].req = req; g_fp[g_nfp].kind = kind; g_fp[g_nfp].location = loc; g_nfp++;
    pthread_mutex_unlock(&g_fp_mu);
}
static int pend_take(uint64_t req, fpend *out) {
    int r = 0;
    pthread_mutex_lock(&g_fp_mu);
    for (unsigned i = 0; i < g_nfp; i++) if (g_fp[i].req == req) { *out = g_fp[i]; g_fp[i] = g_fp[--g_nfp]; r = 1; break; }
    pthread_mutex_unlock(&g_fp_mu);
    return r;
}
// Result of an anchor create/remove (any thread). Requests of the ovrp facade or
// fire-and-forget ones are not in our table and are ignored.
static void on_done(uint64_t req, int ok, kl_uuid id) {
    fpend p;
    if (!pend_take(req, &p)) return;
    if (p.kind == P_CREATE) {
        ent_t *e = ok ? ent_get(KLSP_ANCHOR, id) : NULL;
        ev_anchor(E_ANCHOR_CREATE_COMPLETE, req, ok && e ? R_OK : R_RUNTIME, e);
    } else {
        ent_t *e = ent_get(KLSP_ANCHOR, id);
        if (ok && e) { pthread_mutex_lock(&g_mu); e->erased = 1; pthread_mutex_unlock(&g_mu); }   // XrSpace stays valid, untracked
        ev_store(E_ERASE_COMPLETE, req, ok ? R_OK : R_RUNTIME, e, p.location);
    }
}
static int g_listening;
static void ensure_listener(void) {
    if (!__atomic_exchange_n(&g_listening, 1, __ATOMIC_RELAXED)) kl_spatial_add_listener(on_done);
}

// ================================================================ XR_FB_spatial_entity
typedef struct { int32_t type; const void *next; void *space; KPose pose; int64_t time; } AnchorCI;
static void m4_from_pose(KPose k, float m[16]) {   // column-major rigid
    float x = k.q[0], y = k.q[1], z = k.q[2], w = k.q[3];
    m[0] = 1-2*(y*y+z*z); m[1] = 2*(x*y+z*w);   m[2] = 2*(x*z-y*w);   m[3] = 0;
    m[4] = 2*(x*y-z*w);   m[5] = 1-2*(x*x+z*z); m[6] = 2*(y*z+x*w);   m[7] = 0;
    m[8] = 2*(x*z+y*w);   m[9] = 2*(y*z-x*w);   m[10] = 1-2*(x*x+y*y); m[11] = 0;
    m[12] = k.p[0]; m[13] = k.p[1]; m[14] = k.p[2]; m[15] = 1;
}
static XrResult CreateSpatialAnchorFB(void *session, const AnchorCI *ci, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ci || !req) return R_VALIDATION;
    CHK_TYPE(ci, T_SPATIAL_ANCHOR_CREATE_INFO);
    if (!kl_spatial_available(KLSP_CAP_ANCHORS)) { LOG1("anchors unavailable (no world sensing)"); return R_FEATURE_UNSUP; }
    void *stage = stage_space();
    KPose base;
    if (!stage || !locate_in(ci->space, stage, ci->time, &base)) return R_POSE_INVALID;
    float m[16];
    m4_from_pose(pose_mul(base, ci->pose), m);
    ensure_listener();
    uint64_t id = kl_spatial_new_request();
    pend_add(id, P_CREATE, 0);
    if (kl_spatial_create_anchor(id, m) != 0) { fpend p; pend_take(id, &p); return R_RUNTIME; }
    *req = id;          // the completion event arrives through on_done
    return R_OK;
}
static XrResult GetSpaceUuidFB(void *space, XrUuidEXT *uuid) {
    ent_t *e = space_ent(space);
    if (!uuid) return R_VALIDATION;
    if (!e) return R_COMP_UNSUP;
    memcpy(uuid, &e->id, 16);
    return R_OK;
}
static XrResult EnumerateSpaceSupportedComponentsFB(void *space, uint32_t cap, uint32_t *count, int32_t *out) {
    ent_t *e = space_ent(space);
    if (!count) return R_VALIDATION;
    if (!e) return R_HANDLE;
    uint32_t sup = comp_supported(e), n = 0;
    int32_t tmp[8];
    for (size_t i = 0; i < sizeof k_comp_order / sizeof *k_comp_order; i++)
        if (sup & comp_bit(k_comp_order[i])) tmp[n++] = k_comp_order[i];
    *count = n;
    if (cap == 0) return R_OK;
    if (cap < n) return R_SIZE;
    if (!out) return R_VALIDATION;
    memcpy(out, tmp, n * sizeof *tmp);
    return R_OK;
}
typedef struct { int32_t type; const void *next; int32_t comp; XrBool32 enabled; int64_t timeout; } StatusSetInfo;
typedef struct { int32_t type; const void *next; XrBool32 enabled, pending; } CompStatus;
static XrResult SetSpaceComponentStatusFB(void *space, const StatusSetInfo *si, XrAsyncRequestIdFB *req) {
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!si || !req) return R_VALIDATION;
    CHK_TYPE(si, T_COMP_STATUS_SET_INFO);
    uint32_t b = comp_bit(si->comp);
    if (!b || !(comp_supported(e) & b)) return R_COMP_UNSUP;
    int on = (comp_enabled(e) & b) != 0;
    if (on == (si->enabled != 0)) return R_COMP_ALREADY_SET;
    if (si->comp == C_STORABLE) e->storable_off = !si->enabled;
    *req = next_req();
    EvStatus ev = { { E_SET_STATUS_COMPLETE, NULL, *req, R_OK, 0, space, {{0}} }, si->comp, si->enabled };
    memcpy(&ev.a.uuid, &e->id, 16);
    push(E_SET_STATUS_COMPLETE, &ev, sizeof ev);
    return R_OK;
}
static XrResult GetSpaceComponentStatusFB(void *space, int32_t comp, CompStatus *st) {
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!st) return R_VALIDATION;
    CHK_TYPE(st, T_COMP_STATUS);
    uint32_t b = comp_bit(comp);
    if (!b || !(comp_supported(e) & b)) return R_COMP_UNSUP;
    st->enabled = (comp_enabled(e) & b) != 0;
    st->pending = 0;
    return R_OK;
}
// Not tracked as a separate state here: always fully confident / no-op.
static XrResult GetSpaceConfidenceFB(void *space, float *c) {
    if (!space_ent(space)) return R_HANDLE;
    if (!c) return R_VALIDATION;
    *c = 1.f;
    return R_OK;
}
static XrResult NoopSession(void *session) { return sess_ok(session) ? R_OK : R_HANDLE; }   // xrRebaseSpacesFB, xrSetEffectiveSingleRoomFB (extra args ignored)
static XrResult SetSpaceComponentStatusPrivilegedFB(void *space, const StatusSetInfo *si, XrAsyncRequestIdFB *req) {
    return SetSpaceComponentStatusFB(space, si, req);
}

// ================================================================ XR_FB_spatial_entity_user
typedef struct { uint32_t magic; uint64_t id; } UserH;
#define USER_MAGIC 0x55535244u
static XrResult CreateSpaceUserFB(void *session, const struct { int32_t type; const void *next; uint64_t id; } *ci, void **out) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ci || !out) return R_VALIDATION;
    CHK_TYPE(ci, T_USER_CREATE_INFO);
    UserH *u = calloc(1, sizeof *u);
    if (!u) return R_RUNTIME;
    u->magic = USER_MAGIC; u->id = ci->id;
    *out = u;
    return R_OK;
}
static XrResult GetSpaceUserIdFB(void *user, uint64_t *id) {
    UserH *u = user;
    if (!u || u->magic != USER_MAGIC) return R_HANDLE;
    if (!id) return R_VALIDATION;
    *id = u->id;
    return R_OK;
}
static XrResult DestroySpaceUserFB(void *user) {
    UserH *u = user;
    if (!u || u->magic != USER_MAGIC) return R_HANDLE;
    u->magic = 0; free(u);
    return R_OK;
}

// ================================================================ XR_FB_spatial_entity_query
typedef struct { int32_t type; const void *next; } Filter;
typedef struct { int32_t type; const void *next; uint32_t n; _Alignas(8) const XrUuidEXT *uuids; } UuidFilter;
typedef struct { int32_t type; const void *next; int32_t comp; } CompFilter;
typedef struct { int32_t type; const void *next; int32_t location; } StoreFilter;
typedef struct { int32_t type; const void *next; int32_t action; uint32_t maxResults; int64_t timeout;
                 const Filter *filter, *exclude; } QueryInfo;
typedef struct { void *space; XrUuidEXT uuid; } QueryResult;
typedef struct { int32_t type; const void *next; uint32_t cap, count; QueryResult *results; } QueryResults;

#define PEND_MAX 8
static struct { uint64_t req; uint32_t n; QueryResult *r; } g_pend[PEND_MAX];
static unsigned g_pend_next;

static int filter_pass(const Filter *f, const ent_t *e) {   // 1 pass, -1 invalid
    if (!f) return 1;
    if (f->type == T_FILTER_UUID) {
        const UuidFilter *u = (const UuidFilter *)f;
        for (uint32_t i = 0; i < u->n; i++) if (memcmp(&u->uuids[i], &e->id, 16) == 0) return 1;
        return 0;
    }
    if (f->type == T_FILTER_COMPONENT) {
        uint32_t b = comp_bit(((const CompFilter *)f)->comp);
        return b && (comp_enabled(e) & b) ? 1 : 0;
    }
    if (f->type == T_FILTER_STORAGE) {
        int loc = ((const StoreFilter *)f)->location;
        if (loc == 2) return 0;                                  // cloud: nothing, offline
        return e->kind == KLSP_ANCHOR ? kl_spatial_anchor_saved(e->id) : 1;
    }
    return g_lax() ? 1 : -1;
}
static XrResult QuerySpacesFB(void *session, const QueryInfo *qi, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!qi || !req) return R_VALIDATION;
    CHK_TYPE(qi, T_QUERY_INFO);
    if (qi->action != 0) return R_VALIDATION;
    ent_t **all; int n = ent_all(&all);
    QueryResult *res = calloc(n ? (size_t)n : 1, sizeof *res);
    if (!res) { free(all); return R_RUNTIME; }
    uint32_t cnt = 0, mx = qi->maxResults ? qi->maxResults : 0xffffffffu;
    for (int i = 0; i < n && cnt < mx; i++) {
        int a = filter_pass(qi->filter, all[i]), b = qi->exclude ? filter_pass(qi->exclude, all[i]) : 0;
        if (a < 0 || b < 0) { free(res); free(all); return R_VALIDATION; }
        if (!a || b) continue;
        void *sp = space_for(all[i]);
        if (!sp) continue;
        res[cnt].space = sp;
        memcpy(&res[cnt].uuid, &all[i]->id, 16);
        cnt++;
    }
    free(all);
    *req = next_req();
    unsigned slot = g_pend_next++ % PEND_MAX;
    free(g_pend[slot].r);
    g_pend[slot].req = *req; g_pend[slot].n = cnt; g_pend[slot].r = res;
    EvReq ra = { E_QUERY_RESULTS_AVAILABLE, NULL, *req, 0, 0 };
    push(E_QUERY_RESULTS_AVAILABLE, &ra, 24);                    // type,next,requestId
    EvReq qc = { E_QUERY_COMPLETE, NULL, *req, R_OK, 0 };
    push(E_QUERY_COMPLETE, &qc, sizeof qc);
    return R_OK;
}
static XrResult RetrieveSpaceQueryResultsFB(void *session, XrAsyncRequestIdFB req, QueryResults *out) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!out) return R_VALIDATION;
    CHK_TYPE(out, T_QUERY_RESULTS);
    for (unsigned i = 0; i < PEND_MAX; i++) {
        if (!g_pend[i].r || g_pend[i].req != req) continue;
        out->count = g_pend[i].n;
        if (out->cap == 0) return R_OK;
        if (out->cap < g_pend[i].n) return R_SIZE;
        if (!out->results) return R_VALIDATION;
        memcpy(out->results, g_pend[i].r, g_pend[i].n * sizeof(QueryResult));
        free(g_pend[i].r); g_pend[i].r = NULL;
        return R_OK;
    }
    return R_VALIDATION;
}

// ================================================================ XR_FB_spatial_entity_storage (+_batch)
typedef struct { int32_t type; const void *next; void *space; int32_t location, mode; } SaveInfo;
typedef struct { int32_t type; const void *next; void *space; int32_t location; } EraseInfo;
typedef struct { int32_t type; const void *next; uint32_t n; _Alignas(8) void *const *spaces; int32_t location; } ListSaveInfo;

static int store_check(int location) { return location == 1 ? R_OK : location == 2 ? R_CLOUD_DISABLED : R_VALIDATION; }
static void ev_store(int32_t type, uint64_t req, int32_t res, ent_t *e, int loc) {
    EvStore ev = { { type, NULL, req, res, 0, e ? space_for(e) : NULL, {{0}} }, loc, 0 };
    if (e) memcpy(&ev.a.uuid, &e->id, 16);
    push(type, &ev, sizeof ev);
}
static XrResult SaveSpaceFB(void *session, const SaveInfo *si, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!si || !req) return R_VALIDATION;
    CHK_TYPE(si, T_SAVE_INFO);
    ent_t *e = space_ent(si->space);
    if (!e) return R_HANDLE;
    int r = store_check(si->location);
    if (r) return r;
    if (!(comp_enabled(e) & comp_bit(C_STORABLE))) return R_COMP_NOT_ENABLED;
    if (e->kind != KLSP_ANCHOR || si->location != 1) return R_COMP_NOT_ENABLED;
    kl_spatial_anchor_set_saved(e->id, 1);      // the WorldAnchor persists by itself; this records "this game's"
    *req = next_req();
    ev_store(E_SAVE_COMPLETE, *req, R_OK, e, si->location);
    return R_OK;
}
static XrResult SaveSpaceListFB(void *session, const ListSaveInfo *li, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!li || !req) return R_VALIDATION;
    CHK_TYPE(li, T_LIST_SAVE_INFO);
    int r = store_check(li->location);
    if (r) return r;
    if (li->n && !li->spaces) return R_VALIDATION;
    for (uint32_t i = 0; i < li->n; i++) {
        ent_t *e = space_ent(li->spaces[i]);
        if (!e) return R_HANDLE;
        if (!(comp_enabled(e) & comp_bit(C_STORABLE)) || e->kind != KLSP_ANCHOR) return R_COMP_NOT_ENABLED;
    }
    for (uint32_t i = 0; i < li->n; i++) kl_spatial_anchor_set_saved(space_ent(li->spaces[i])->id, 1);
    *req = next_req();
    EvReq ev = { E_LIST_SAVE_COMPLETE, NULL, *req, R_OK, 0 };
    push(E_LIST_SAVE_COMPLETE, &ev, sizeof ev);
    return R_OK;
}
static XrResult EraseSpaceFB(void *session, const EraseInfo *ei, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ei || !req) return R_VALIDATION;
    CHK_TYPE(ei, T_ERASE_INFO);
    ent_t *e = space_ent(ei->space);
    if (!e) return R_HANDLE;
    int r = store_check(ei->location);
    if (r) return r;
    *req = next_req();
    if (e->kind != KLSP_ANCHOR) {               // only anchors are erasable (planes/meshes/rooms belong to visionOS)
        ev_store(E_ERASE_COMPLETE, *req, R_COMP_NOT_ENABLED, e, ei->location);
        return R_OK;
    }
    ensure_listener();
    pend_add(*req, P_ERASE, ei->location);
    if (kl_spatial_remove_anchor(*req, e->id) != 0) {
        fpend p; pend_take(*req, &p);
        ev_store(E_ERASE_COMPLETE, *req, R_RUNTIME, e, ei->location);
    }
    return R_OK;
}

// ================================================================ XR_FB_spatial_entity_container
typedef struct { int32_t type; const void *next; uint32_t cap, count; XrUuidEXT *uuids; } Container;
static XrResult GetSpaceContainerFB(void *space, Container *c) {
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!c) return R_VALIDATION;
    CHK_TYPE(c, T_SPACE_CONTAINER);
    if (!(comp_supported(e) & comp_bit(C_CONTAINER))) return R_COMP_UNSUP;
    ent_t **all; int n = ent_all(&all);
    uint32_t cnt = 0;
    for (int i = 0; i < n; i++) if (all[i]->kind == KLSP_PLANE || all[i]->kind == KLSP_MESH) cnt++;
    c->count = cnt;
    XrResult r = R_OK;
    if (c->cap) {
        if (c->cap < cnt) r = R_SIZE;
        else if (!c->uuids) r = R_VALIDATION;
        else { uint32_t k = 0; for (int i = 0; i < n; i++) if (all[i]->kind == KLSP_PLANE || all[i]->kind == KLSP_MESH) memcpy(&c->uuids[k++], &all[i]->id, 16); }
    }
    free(all);
    return r;
}

// ================================================================ XR_FB_scene
typedef struct { float x, y; } V2;
typedef struct { V2 off; V2 ext; } Rect2D;
typedef struct { float ox, oy, oz, w, h, d; } Rect3D;
typedef struct { int32_t type; const void *next; uint32_t cap, count; char *buf; } SemLabels;
typedef struct { int32_t type; const void *next; XrUuidEXT floor, ceiling; uint32_t cap, count; XrUuidEXT *walls; } RoomLayout;
typedef struct { int32_t type; const void *next; uint32_t cap, count; V2 *verts; } Boundary2D;

static XrResult GetSpaceBoundingBox2DFB(void *session, void *space, Rect2D *r) {
    if (!sess_ok(session)) return R_HANDLE;
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!r) return R_VALIDATION;
    if (e->kind != KLSP_PLANE) return R_COMP_UNSUP;
    float w, h; plane_geom(e->id, &w, &h, NULL);
    r->off = (V2){ -w/2, -h/2 }; r->ext = (V2){ w, h };
    return R_OK;
}
static XrResult GetSpaceBoundingBox3DFB(void *session, void *space, Rect3D *r) {
    if (!sess_ok(session)) return R_HANDLE;
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!r) return R_VALIDATION;
    if (e->kind != KLSP_MESH) return R_COMP_UNSUP;
    uint32_t nv = 0, nt = 0;
    kl_spatial_mesh_info(e->id, 0, NULL, &nv, 0, NULL, &nt);
    memset(r, 0, sizeof *r);
    if (!nv) return R_OK;
    float *v = malloc((size_t)nv * 12);
    if (!v) return R_RUNTIME;
    uint32_t nv2 = 0, nt2 = 0;
    kl_spatial_mesh_info(e->id, nv, v, &nv2, 0, NULL, &nt2);
    if (nv2 > nv) nv2 = nv;
    float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
    for (uint32_t i = 0; i < nv2; i++) for (int k = 0; k < 3; k++) {
        if (v[3*i+k] < lo[k]) lo[k] = v[3*i+k];
        if (v[3*i+k] > hi[k]) hi[k] = v[3*i+k];
    }
    free(v);
    if (nv2) *r = (Rect3D){ lo[0], lo[1], lo[2], hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2] };
    return R_OK;
}
static XrResult GetSpaceSemanticLabelsFB(void *session, void *space, SemLabels *s) {
    if (!sess_ok(session)) return R_HANDLE;
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!s) return R_VALIDATION;
    CHK_TYPE(s, T_SEMANTIC_LABELS);
    if (!(comp_supported(e) & comp_bit(C_SEMANTIC))) return R_COMP_UNSUP;
    const char *str = "OTHER";
    if (e->kind == KLSP_MESH) str = "GLOBAL_MESH";
    else { int l = KLSP_LBL_OTHER; plane_geom(e->id, NULL, NULL, &l); str = label_str(l); }
    uint32_t len = (uint32_t)strlen(str) + 1;
    s->count = len;
    if (s->cap == 0) return R_OK;
    if (s->cap < len) return R_SIZE;
    if (!s->buf) return R_VALIDATION;
    memcpy(s->buf, str, len);
    return R_OK;
}
static XrResult GetSpaceBoundary2DFB(void *session, void *space, Boundary2D *b) {
    if (!sess_ok(session)) return R_HANDLE;
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!b) return R_VALIDATION;
    CHK_TYPE(b, T_BOUNDARY2D);
    if (e->kind != KLSP_PLANE) return R_COMP_UNSUP;
    float w, h; plane_geom(e->id, &w, &h, NULL);
    uint32_t np = 0;
    kl_spatial_plane_info(e->id, NULL, NULL, NULL, 0, NULL, &np);
    V2 rect[4] = { {-w/2,-h/2}, {w/2,-h/2}, {w/2,h/2}, {-w/2,h/2} };
    uint32_t n = np >= 3 ? np : 4;
    b->count = n;
    if (b->cap == 0) return R_OK;
    if (b->cap < n) return R_SIZE;
    if (!b->verts) return R_VALIDATION;
    if (np >= 3) {
        float *p = calloc((size_t)np * 4, sizeof *p);      // 2x slack: cap unit is the reader's
        if (!p) return R_RUNTIME;
        uint32_t got = 0;
        kl_spatial_plane_info(e->id, NULL, NULL, NULL, np, p, &got);
        if (got > np) got = np;
        for (uint32_t i = 0; i < got; i++) b->verts[i] = (V2){ p[2*i], -p[2*i+1] };   // ARKit (x,z) -> Meta (x,y)
        for (uint32_t i = got; i < np; i++) b->verts[i] = got ? b->verts[got-1] : rect[0];
        free(p);
    } else memcpy(b->verts, rect, sizeof rect);
    return R_OK;
}
static XrResult GetSpaceRoomLayoutFB(void *session, void *space, RoomLayout *rl) {
    if (!sess_ok(session)) return R_HANDLE;
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!rl) return R_VALIDATION;
    CHK_TYPE(rl, T_ROOM_LAYOUT);
    if (!(comp_supported(e) & comp_bit(C_ROOM_LAYOUT))) return R_COMP_UNSUP;
    uint32_t np = kl_spatial_count(KLSP_PLANE), nw = 0;
    kl_uuid *walls = calloc(np ? np : 1, sizeof *walls), floor_ = {{0}}, ceil_ = {{0}};
    if (!walls) return R_RUNTIME;
    int have_f = 0, have_c = 0;
    for (uint32_t i = 0; i < np; i++) {
        kl_uuid u; float m[16]; int l = KLSP_LBL_OTHER;
        memset(&u, 0, sizeof u);
        kl_spatial_get(KLSP_PLANE, i, &u, m);
        plane_geom(u, NULL, NULL, &l);
        if (l == KLSP_LBL_FLOOR && !have_f) { floor_ = u; have_f = 1; }
        else if (l == KLSP_LBL_CEILING && !have_c) { ceil_ = u; have_c = 1; }
        else if (l == KLSP_LBL_WALL) walls[nw++] = u;
    }
    memcpy(&rl->floor, &floor_, 16); memcpy(&rl->ceiling, &ceil_, 16);
    rl->count = nw;
    XrResult r = R_OK;
    if (rl->cap) {
        if (rl->cap < nw) r = R_SIZE;
        else if (nw && !rl->walls) r = R_VALIDATION;
        else if (nw) memcpy(rl->walls, walls, nw * 16);
    }
    free(walls);
    return r;
}

// ================================================================ XR_FB_scene_capture
typedef struct { int32_t type; const void *next; uint32_t bytes; _Alignas(8) const char *request; } CaptureReq;
static int gate_scene(void);
static XrResult RequestSceneCaptureFB(void *session, const CaptureReq *ci, XrAsyncRequestIdFB *req) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ci || !req) return R_VALIDATION;
    CHK_TYPE(ci, T_SCENE_CAPTURE_REQUEST);
    if (ci->bytes && !ci->request) return R_VALIDATION;
    // visionOS maps the room by itself (RoomTracking/PlaneDetection): there is
    // nothing to launch, so the "capture" completes at once.
    LOG1("scene capture completes immediately (the room is tracked by visionOS)");
    *req = next_req();
    // Announced by policy before ARKit answers: complete OK too, the room reads empty until data arrives.
    int avail = kl_spatial_available(KLSP_CAP_PLANES) || kl_spatial_available(KLSP_CAP_ROOM) || gate_scene();
    EvReq ev = { E_SCENE_CAPTURE_COMPLETE, NULL, *req, avail ? R_OK : R_RUNTIME, 0 };
    push(E_SCENE_CAPTURE_COMPLETE, &ev, sizeof ev);
    return R_OK;
}

// ================================================================ XR_META_spatial_entity_mesh
typedef struct { int32_t type; const void *next; } TriMeshGetInfo;
typedef struct { int32_t type; const void *next; uint32_t vcap, vcount; float (*verts)[3]; uint32_t icap, icount; uint32_t *idx; } TriMesh;
static XrResult GetSpaceTriangleMeshMETA(void *space, const TriMeshGetInfo *gi, TriMesh *m) {
    ent_t *e = space_ent(space);
    if (!e) return R_HANDLE;
    if (!gi || !m) return R_VALIDATION;
    CHK_TYPE(gi, T_TRIMESH_GET_INFO);
    CHK_TYPE(m, T_TRIMESH);
    if (e->kind != KLSP_MESH) return R_COMP_UNSUP;
    uint32_t nv = 0, nt = 0;
    kl_spatial_mesh_info(e->id, 0, NULL, &nv, 0, NULL, &nt);
    m->vcount = nv; m->icount = nt * 3;
    if (m->vcap == 0 && m->icap == 0) return R_OK;
    if (m->vcap < nv || m->icap < nt * 3) return R_SIZE;
    if ((nv && !m->verts) || (nt && !m->idx)) return R_VALIDATION;
    float *v = malloc((size_t)(nv ? nv : 1) * 12);
    uint32_t *t = malloc((size_t)(nt ? nt : 1) * 12 * 2);       // 2x slack: cap unit is the reader's
    if (!v || !t) { free(v); free(t); return R_RUNTIME; }
    uint32_t nv2 = 0, nt2 = 0;
    kl_spatial_mesh_info(e->id, nv, v, &nv2, nt, t, &nt2);
    if (nv2 > nv) nv2 = nv;
    if (nt2 > nt) nt2 = nt;
    memcpy(m->verts, v, (size_t)nv2 * 12);
    memcpy(m->idx, t, (size_t)nt2 * 12);
    m->vcount = nv2; m->icount = nt2 * 3;
    free(v); free(t);
    return R_OK;
}

// ================================================================ XR_FB_triangle_mesh
#define TM_MAGIC 0x544d5348u
typedef struct { int32_t type; const void *next; uint64_t flags; int32_t winding; uint32_t vcount; const float *verts;
                 uint32_t tcount; const uint32_t *idx; } TriMeshCI;
typedef struct { uint32_t magic; int mutable_, updating; uint32_t vcap, tcap, vcount, tcount; float *v; uint32_t *i; } TMesh;
static TMesh *tm_of(void *h) { TMesh *m = h; return m && m->magic == TM_MAGIC ? m : NULL; }
static XrResult CreateTriangleMeshFB(void *session, const TriMeshCI *ci, void **out) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ci || !out) return R_VALIDATION;
    CHK_TYPE(ci, T_TRIANGLE_MESH_CREATE_INFO);
    int mut = (ci->flags & 1) != 0;
    if (!mut && (!ci->verts || !ci->idx || !ci->vcount || !ci->tcount)) return R_VALIDATION;
    TMesh *m = calloc(1, sizeof *m);
    if (!m) return R_RUNTIME;
    m->magic = TM_MAGIC; m->mutable_ = mut;
    m->vcap = m->vcount = ci->vcount; m->tcap = m->tcount = ci->tcount;
    m->v = calloc(m->vcap ? m->vcap : 1, 12); m->i = calloc(m->tcap ? m->tcap : 1, 12);
    if (!m->v || !m->i) { free(m->v); free(m->i); free(m); return R_RUNTIME; }
    if (ci->verts) memcpy(m->v, ci->verts, (size_t)m->vcount * 12);
    if (ci->idx) memcpy(m->i, ci->idx, (size_t)m->tcount * 12);
    *out = m;
    return R_OK;
}
static XrResult DestroyTriangleMeshFB(void *h) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    m->magic = 0; free(m->v); free(m->i); free(m);
    return R_OK;
}
static XrResult TriangleMeshGetVertexBufferFB(void *h, float **out) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (!out) return R_VALIDATION;
    if (!m->mutable_) return R_VALIDATION;
    *out = m->v;
    return R_OK;
}
static XrResult TriangleMeshGetIndexBufferFB(void *h, uint32_t **out) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (!out) return R_VALIDATION;
    if (!m->mutable_) return R_VALIDATION;
    *out = m->i;
    return R_OK;
}
static XrResult TriangleMeshBeginUpdateFB(void *h) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (!m->mutable_ || m->updating) return R_VALIDATION;
    m->updating = 1;
    return R_OK;
}
static XrResult TriangleMeshEndUpdateFB(void *h, uint32_t vc, uint32_t tc) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (!m->mutable_ || !m->updating || vc > m->vcap || tc > m->tcap) return R_VALIDATION;
    m->updating = 0; m->vcount = vc; m->tcount = tc;
    return R_OK;
}
static XrResult TriangleMeshBeginVertexBufferUpdateFB(void *h, uint32_t *count) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (!m->mutable_ || m->updating || !count) return R_VALIDATION;
    m->updating = 2;
    *count = m->vcount;
    return R_OK;
}
static XrResult TriangleMeshEndVertexBufferUpdateFB(void *h) {
    TMesh *m = tm_of(h);
    if (!m) return R_HANDLE;
    if (m->updating != 2) return R_VALIDATION;
    m->updating = 0;
    return R_OK;
}

// ================================================================ passthrough extras
// The core XR_FB_passthrough (create/start/pause/layers/style/geometry) is served
// by kl_openxr.c against the real compositor passthrough. Colour LUTs and
// preferences are accepted as no-ops: the room image is visionOS's own.
#define LUT_MAGIC 0x4c555421u
typedef struct { uint32_t magic; } LutH;
static XrResult CreatePassthroughColorLutMETA(void *pt, const void *info, void **out) {
    if (!pt) return R_HANDLE;
    if (!info || !out) return R_VALIDATION;
    LutH *l = calloc(1, sizeof *l);
    if (!l) return R_RUNTIME;
    l->magic = LUT_MAGIC;
    *out = l;
    LOG1("passthrough colour LUTs are accepted and ignored (visionOS draws the room)");
    return R_OK;
}
static XrResult DestroyPassthroughColorLutMETA(void *h) {
    LutH *l = h;
    if (!l || l->magic != LUT_MAGIC) return R_HANDLE;
    l->magic = 0; free(l);
    return R_OK;
}
static XrResult UpdatePassthroughColorLutMETA(void *h, const void *info) {
    LutH *l = h;
    if (!l || l->magic != LUT_MAGIC) return R_HANDLE;
    return info ? R_OK : R_VALIDATION;
}
static XrResult GetPassthroughPreferencesMETA(void *session, struct { int32_t type; const void *next; uint64_t flags; } *p) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!p) return R_VALIDATION;
    p->flags = 0;
    return R_OK;
}
static XrResult PassthroughLayerSetKeyboardHandsIntensityFB(void *layer, const void *in) {
    return layer && in ? R_OK : layer ? R_VALIDATION : R_HANDLE;
}

// ================================================================ XR_EXT_plane_detection
// Numbers and layouts verified against openxr.h; advertised whenever ARKit delivers planes.
#define PD_MAGIC 0x50444554u
#define PD_MAX 256
typedef struct { uint32_t magic; int state; uint32_t n; kl_uuid ids[PD_MAX]; } Detector;
typedef struct { int32_t type; const void *next; uint64_t flags; } PDCreate;
typedef struct { int32_t type; const void *next; void *base; int64_t time; uint32_t norient; _Alignas(8) const int32_t *orient;
                 uint32_t nsem; _Alignas(8) const int32_t *sem; uint32_t maxPlanes; float minArea; KPose bbPose; float bbExt[3]; } PDBegin;
typedef struct { int32_t type; const void *next; void *base; int64_t time; } PDGet;
typedef struct { int32_t type; const void *next; uint64_t id; uint64_t flags; KPose pose; V2 ext; int32_t orient, sem; uint32_t polyCount; } PDLoc;
typedef struct { int32_t type; const void *next; uint32_t cap, count; PDLoc *locs; } PDLocs;
typedef struct { int32_t type; const void *next; uint32_t cap, count; V2 *verts; } PDPoly;

static Detector *pd_of(void *h) { Detector *d = h; return d && d->magic == PD_MAGIC ? d : NULL; }
static XrResult CreatePlaneDetectorEXT(void *session, const PDCreate *ci, void **out) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!ci || !out) return R_VALIDATION;
    Detector *d = calloc(1, sizeof *d);
    if (!d) return R_RUNTIME;
    d->magic = PD_MAGIC;
    *out = d;
    return R_OK;
}
static XrResult DestroyPlaneDetectorEXT(void *h) {
    Detector *d = pd_of(h);
    if (!d) return R_HANDLE;
    d->magic = 0; free(d);
    return R_OK;
}
static int plane_sem(int l) { return l == KLSP_LBL_CEILING ? 1 : l == KLSP_LBL_FLOOR ? 2 : l == KLSP_LBL_WALL ? 3 : (l == KLSP_LBL_TABLE || l == KLSP_LBL_COUCH) ? 4 : 0; }
static int plane_orient(const float m[16]) {   // ARKit +Y normal = column 1
    float ny = m[5];
    return ny > 0.9f ? 0 : ny < -0.9f ? 1 : fabsf(ny) < 0.3f ? 2 : 3;
}
static XrResult BeginPlaneDetectionEXT(void *h, const PDBegin *bi) {
    Detector *d = pd_of(h);
    if (!d) return R_HANDLE;
    if (!bi) return R_VALIDATION;
    d->n = 0;
    uint32_t np = kl_spatial_count(KLSP_PLANE);
    for (uint32_t i = 0; i < np && d->n < PD_MAX; i++) {
        kl_uuid u; float m[16], w, hh; int l;
        memset(&u, 0, sizeof u);
        kl_spatial_get(KLSP_PLANE, i, &u, m);
        plane_geom(u, &w, &hh, &l);
        if (w * hh < bi->minArea) continue;
        int o = plane_orient(m), s = plane_sem(l), okO = bi->norient == 0, okS = bi->nsem == 0;
        for (uint32_t k = 0; k < bi->norient && bi->orient; k++) if (bi->orient[k] == o) okO = 1;
        for (uint32_t k = 0; k < bi->nsem && bi->sem; k++) if (bi->sem[k] == s) okS = 1;
        if (!okO || !okS) continue;
        if (bi->maxPlanes && d->n >= bi->maxPlanes) break;
        d->ids[d->n++] = u;
    }
    d->state = 2;      // DONE: ARKit already has the planes
    LOG1("plane detection answers from ARKit's current planes");
    return R_OK;
}
static XrResult GetPlaneDetectionStateEXT(void *h, int32_t *st) {
    Detector *d = pd_of(h);
    if (!d) return R_HANDLE;
    if (!st) return R_VALIDATION;
    *st = d->state;
    return R_OK;
}
static XrResult GetPlaneDetectionsEXT(void *h, const PDGet *gi, PDLocs *out) {
    Detector *d = pd_of(h);
    if (!d) return R_HANDLE;
    if (!gi || !out) return R_VALIDATION;
    if (d->state != 2) return R_VALIDATION;
    out->count = d->n;
    if (out->cap == 0) return R_OK;
    if (out->cap < d->n) return R_SIZE;
    if (d->n && !out->locs) return R_VALIDATION;
    for (uint32_t i = 0; i < d->n; i++) {
        ent_t *e = ent_get(KLSP_PLANE, d->ids[i]);
        PDLoc *L = &out->locs[i];
        float w = 0, hh = 0, m[16]; int l = 0;
        plane_geom(d->ids[i], &w, &hh, &l);
        memset(&L->flags, 0, sizeof L->flags);
        L->id = kl_spatial_handle_for(d->ids[i]);
        L->ext = (V2){ w, hh };
        L->sem = plane_sem(l);
        L->orient = sp_find(KLSP_PLANE, d->ids[i], m) ? plane_orient(m) : 3;
        uint32_t np = 0; kl_spatial_plane_info(d->ids[i], NULL, NULL, NULL, 0, NULL, &np);
        L->polyCount = np >= 3 ? 1 : 0;
        KPose p;
        void *sp = e ? space_for(e) : NULL;
        if (sp && locate_in(sp, gi->base, gi->time, &p)) { L->pose = p; L->flags = 0xf; }
        else L->flags = 0;
    }
    return R_OK;
}
static XrResult GetPlanePolygonBufferEXT(void *h, uint64_t plane_id, uint32_t index, PDPoly *pb) {
    Detector *d = pd_of(h);
    if (!d) return R_HANDLE;
    if (!pb || index != 0) return R_VALIDATION;
    for (uint32_t i = 0; i < d->n; i++) {
        if (kl_spatial_handle_for(d->ids[i]) != plane_id) continue;
        uint32_t np = 0;
        kl_spatial_plane_info(d->ids[i], NULL, NULL, NULL, 0, NULL, &np);
        pb->count = np;
        if (pb->cap == 0) return R_OK;
        if (pb->cap < np) return R_SIZE;
        if (np && !pb->verts) return R_VALIDATION;
        float *p = calloc((size_t)np * 4 + 4, sizeof *p);
        if (!p) return R_RUNTIME;
        uint32_t got = 0;
        kl_spatial_plane_info(d->ids[i], NULL, NULL, NULL, np, p, &got);
        if (got > np) got = np;
        for (uint32_t k = 0; k < np; k++) pb->verts[k] = k < got ? (V2){ p[2*k], -p[2*k+1] } : (V2){0, 0};
        free(p);
        return R_OK;
    }
    return R_VALIDATION;
}

// ================================================================ rejections
// Create/Start/Get on a feature this host will not have.
static XrResult FeatureUnsupported(void) { return R_FEATURE_UNSUP; }

// ================================================================ XR_META_environment_depth
// Served from the ARKit scene mesh (kl_envdepth.c; KleptonEnvDepth.swift draws it). Layouts as the
// spec has them, and cross-checked against the real libOVRPlugin's own use of the extension:
//   XrEnvironmentDepthImageMETA      {type, next, u32 swapchainIndex @16, f32 nearZ @20, f32 farZ @24, views[2] @32}
//   XrEnvironmentDepthImageViewMETA  {type, next, XrFovf fov @16, XrPosef pose @32}, 64 bytes
//   XrEnvironmentDepthImageAcquireInfoMETA {type, next, XrSpace space @16, XrTime displayTime @24}
//   XrEnvironmentDepthSwapchainStateMETA   {type, next, u32 width @16, u32 height @20}
// The struct TYPES are not checked: the pre-release METAX1 aliases below share the layouts but not the
// numbers. Poses are located the way xrLocateViews locates them (displayTime 0 = the latched head this
// module rendered from), in the space the guest names.
enum {
    ED_T_IMAGE_BASE = 1000024002,       // XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
    ED_T_IMAGE_VULKAN = 1000025001,     // XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR (== VULKAN2)
    T_SYS_ENV_DEPTH = 1000291007,
    ED_NOT_AVAILABLE = 1000291000,      // XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META, a success code
    R_CALL_ORDER = -37,
};
typedef struct { int32_t type; const void *next; float fov[4]; float q[4]; float p[3]; } EdView;
typedef struct { int32_t type; const void *next; uint32_t index; float near_z, far_z; EdView views[2]; } EdImage;
typedef struct { int32_t type; const void *next; void *space; int64_t time; } EdAcquireInfo;
typedef struct { int32_t type; void *next; float q[4]; float p[3]; float fov[4]; } XView;              // XrView
typedef struct { int32_t type; const void *next; int32_t config; int64_t time; void *space; } XViewLocate;
typedef struct { int32_t type; void *next; uint64_t flags; } XViewState;
typedef XrResult (*PFN_LocateViews)(void *, const XViewLocate *, XViewState *, uint32_t, uint32_t *, XView *);
_Static_assert(sizeof(EdView) == 64 && offsetof(EdImage, views) == 32, "XrEnvironmentDepthImageMETA layout");
_Static_assert(sizeof(XView) == 64 && offsetof(XView, fov) == 44, "XrView layout");

static char g_ed_provider_tag, g_ed_swapchain_tag;
static int g_ed_swapchain;
static pthread_mutex_t g_ed_mu = PTHREAD_MUTEX_INITIALIZER;

static XrResult ED_CreateProvider(void *session, const XrHdr *info, void **out) {
    if (!sess_ok(session)) return R_HANDLE;
    if (!info || !out) return R_VALIDATION;
    if (!kl_envdepth_supported()) return R_FEATURE_UNSUP;
    pthread_mutex_lock(&g_ed_mu);
    XrResult r = R_OK;
    if (kl_envdepth_created()) r = R_LIMIT;                          // one provider per session
    else if (kl_envdepth_create(0) != 0) r = R_FEATURE_UNSUP;
    else *out = &g_ed_provider_tag;
    pthread_mutex_unlock(&g_ed_mu);
    return r;
}
static XrResult ED_DestroyProvider(void *provider) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    pthread_mutex_lock(&g_ed_mu);
    kl_envdepth_destroy();
    g_ed_swapchain = 0;
    pthread_mutex_unlock(&g_ed_mu);
    return R_OK;
}
static XrResult ED_StartProvider(void *provider) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    return kl_envdepth_start() == 0 ? R_OK : R_HANDLE;
}
static XrResult ED_StopProvider(void *provider) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    kl_envdepth_stop();
    return R_OK;
}
static XrResult ED_CreateSwapchain(void *provider, const XrHdr *info, void **out) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    if (!info || !out) return R_VALIDATION;
    pthread_mutex_lock(&g_ed_mu);
    XrResult r = R_OK;
    if (g_ed_swapchain) r = R_LIMIT;
    else if (kl_envdepth_alloc() <= 0) r = R_RUNTIME;       // a GL guest needs its context current here
    else { g_ed_swapchain = 1; *out = &g_ed_swapchain_tag; }
    pthread_mutex_unlock(&g_ed_mu);
    return r;
}
static XrResult ED_DestroySwapchain(void *swapchain) {
    if (swapchain != &g_ed_swapchain_tag || !g_ed_swapchain) return R_HANDLE;
    pthread_mutex_lock(&g_ed_mu);
    kl_envdepth_free();
    g_ed_swapchain = 0;
    pthread_mutex_unlock(&g_ed_mu);
    return R_OK;
}
static XrResult ED_GetSwapchainState(void *swapchain, void *state) {
    if (swapchain != &g_ed_swapchain_tag || !g_ed_swapchain) return R_HANDLE;
    if (!state) return R_VALIDATION;
    ((uint32_t *)((char *)state + 16))[0] = KLED_W;
    ((uint32_t *)((char *)state + 16))[1] = KLED_H;
    return R_OK;
}
static XrResult ED_EnumerateImages(void *swapchain, uint32_t capacity, uint32_t *count_out, void *images) {
    if (swapchain != &g_ed_swapchain_tag || !g_ed_swapchain) return R_HANDLE;
    if (!count_out) return R_VALIDATION;
    *count_out = KLED_IMAGES;
    if (capacity == 0) return R_OK;
    if (capacity < KLED_IMAGES) return R_SIZE;
    if (!images) return R_VALIDATION;
    int vk = kl_envdepth_is_vulkan();
    int32_t want = vk ? ED_T_IMAGE_VULKAN : ED_T_IMAGE_BASE;
    if (*(const int32_t *)images != want && !g_lax()) return R_VALIDATION;   // the app's binding is not the session's
    // XrSwapchainImageVulkanKHR {type, next, VkImage @16} / XrSwapchainImageOpenGLESKHR {type, next, u32 @16}: 24 bytes each.
    for (uint32_t i = 0; i < KLED_IMAGES; i++) {
        char *e = (char *)images + (size_t)i * 24;
        uint64_t img = kl_envdepth_image((int)i);
        if (vk) *(uint64_t *)(e + 16) = img; else *(uint32_t *)(e + 16) = (uint32_t)img;
    }
    return R_OK;
}
static XrResult ED_Acquire(void *provider, const EdAcquireInfo *info, EdImage *out) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    if (!info || !out) return R_VALIDATION;
    kl_envdepth_frame f;
    int r = kl_envdepth_acquire(&f);
    if (r < 0) return R_CALL_ORDER;                 // not started
    if (r == 0) return ED_NOT_AVAILABLE;            // started, nothing rendered yet
    // The poses are in the tracking space; the guest asked for `info->space`.
    PFN_LocateViews locate = (PFN_LocateViews)kl_openxr_sym("xrLocateViews");
    XView v[2];
    memset(v, 0, sizeof v);
    v[0].type = v[1].type = 7;                      // XR_TYPE_VIEW
    XViewState vs = { 11, NULL, 0 };                // XR_TYPE_VIEW_STATE
    XViewLocate li = { 6, NULL, 2, 0, info->space };// XR_TYPE_VIEW_LOCATE_INFO, PRIMARY_STEREO, displayTime 0
    uint32_t n = 0;
    XrResult lr = locate ? locate(klxr_current_session(), &li, &vs, 2, &n, v) : R_RUNTIME;
    if (lr >= 0 && n == 2) {
        float rel0[7] = { v[0].p[0], v[0].p[1], v[0].p[2], v[0].q[0], v[0].q[1], v[0].q[2], v[0].q[3] };
        kl_envdepth_rebase(&f, rel0);
    } else {
        LOG1("environment depth: the acquire space could not be located, poses stay in the tracking space");
    }
    out->index = (uint32_t)f.index;
    out->near_z = f.near_z;
    out->far_z = f.far_z;
    for (int e = 0; e < 2; e++) {
        EdView *w = &out->views[e];
        w->fov[0] = -atanf(f.tan[e][0]);            // angleLeft
        w->fov[1] =  atanf(f.tan[e][1]);            // angleRight
        w->fov[2] =  atanf(f.tan[e][2]);            // angleUp
        w->fov[3] = -atanf(f.tan[e][3]);            // angleDown
        w->p[0] = f.pose[e][0]; w->p[1] = f.pose[e][1]; w->p[2] = f.pose[e][2];
        w->q[0] = f.pose[e][3]; w->q[1] = f.pose[e][4]; w->q[2] = f.pose[e][5]; w->q[3] = f.pose[e][6];
    }
    return R_OK;
}
static XrResult ED_SetHandRemoval(void *provider, const void *info) {
    if (provider != &g_ed_provider_tag || !kl_envdepth_created()) return R_HANDLE;
    if (!info) return R_VALIDATION;
    kl_envdepth_set_hand_removal(*(const uint32_t *)((const char *)info + 16) != 0);   // XrBool32 enabled
    return R_OK;
}
static int gate_envdepth(void) { return kl_envdepth_supported(); }

// ================================================================ gates and system props
static int have_hook(void) { return 1; }
// Anchors: advertised when WorldAnchors run, or when the game asked for them
// (policy is set before ARKit starts, availability only after it is authorised).
static int gate_anchors(void) {
    int ws = 0, wa = 0;
    kl_spatial_policy(&ws, &wa);
    return have_hook() && (kl_spatial_available(KLSP_CAP_ANCHORS) || wa);
}
// Scene data: the same, with the policy standing in until ARKit is authorised
// (the game may enumerate extensions before that; the answers are empty until data arrives).
static int wants_scene(void) { int ws = 0; kl_spatial_policy(&ws, NULL); return ws; }
static int gate_scene(void) {
    return have_hook() && (kl_spatial_available(KLSP_CAP_PLANES) || kl_spatial_available(KLSP_CAP_MESH) ||
                           kl_spatial_available(KLSP_CAP_ROOM) || wants_scene());
}
static int gate_mesh(void) { return have_hook() && (kl_spatial_available(KLSP_CAP_MESH) || wants_scene()); }
static int gate_planes(void) { return have_hook() && (kl_spatial_available(KLSP_CAP_PLANES) || wants_scene()); }
static int gate_always(void) { return 1; }

// Chain walker for xrGetSystemProperties (called by kl_openxr.c if wired).
int kl_openxr_spatial_system_props(void *chain) {
    int n = 0, depth = 0;
    for (const XrHdr *h = chain; h && depth < 8; h = h->next, depth++) {   // bounded like the caller's walk
        if (h->type == T_SYS_SPATIAL_ENTITY) {
            ((struct { int32_t t; const void *n; XrBool32 supports; } *)(void *)h)->supports = gate_anchors();
            n++;
        } else if (h->type == T_SYS_ENV_DEPTH) {   // XrSystemEnvironmentDepthPropertiesMETA {supportsEnvironmentDepth, supportsHandRemoval}
            XrBool32 *b = (XrBool32 *)((char *)h + 16);
            b[0] = b[1] = gate_envdepth();
            n++;
        } else if (h->type == T_SYS_PLANE_DETECTION) {   // XrSystemPlaneDetectionPropertiesEXT {XrFlags64 supportedFeatures @16}
            // PLANE_DETECTION | SEMANTIC_{CEILING,FLOOR,WALL,PLATFORM} | ORIENTATION
            *(uint64_t *)((char *)h + 16) = gate_planes() ? 0x7dull : 0;
            n++;
        }
    }
    return n;
}

// ================================================================ registration
static const klxr_row_def k_rows[] = {
    {"xrCreateSpatialAnchorFB", (void *)CreateSpatialAnchorFB},
    {"xrGetSpaceUuidFB", (void *)GetSpaceUuidFB},
    {"xrEnumerateSpaceSupportedComponentsFB", (void *)EnumerateSpaceSupportedComponentsFB},
    {"xrSetSpaceComponentStatusFB", (void *)SetSpaceComponentStatusFB},
    {"xrGetSpaceComponentStatusFB", (void *)GetSpaceComponentStatusFB},
    {"xrSetSpaceComponentStatusPrivilegedFB", (void *)SetSpaceComponentStatusPrivilegedFB},
    {"xrGetSpaceConfidenceFB", (void *)GetSpaceConfidenceFB},
    {"xrRebaseSpacesFB", (void *)NoopSession},
    {"xrSetEffectiveSingleRoomFB", (void *)NoopSession},
    {"xrCreateSpaceUserFB", (void *)CreateSpaceUserFB},
    {"xrGetSpaceUserIdFB", (void *)GetSpaceUserIdFB},
    {"xrDestroySpaceUserFB", (void *)DestroySpaceUserFB},
    {"xrQuerySpacesFB", (void *)QuerySpacesFB},
    {"xrRetrieveSpaceQueryResultsFB", (void *)RetrieveSpaceQueryResultsFB},
    {"xrSaveSpaceFB", (void *)SaveSpaceFB},
    {"xrSaveSpaceListFB", (void *)SaveSpaceListFB},
    {"xrEraseSpaceFB", (void *)EraseSpaceFB},
    {"xrGetSpaceContainerFB", (void *)GetSpaceContainerFB},
    {"xrGetSpaceBoundingBox2DFB", (void *)GetSpaceBoundingBox2DFB},
    {"xrGetSpaceBoundingBox3DFB", (void *)GetSpaceBoundingBox3DFB},
    {"xrGetSpaceSemanticLabelsFB", (void *)GetSpaceSemanticLabelsFB},
    {"xrGetSpaceBoundary2DFB", (void *)GetSpaceBoundary2DFB},
    {"xrGetSpaceRoomLayoutFB", (void *)GetSpaceRoomLayoutFB},
    {"xrRequestSceneCaptureFB", (void *)RequestSceneCaptureFB},
    {"xrGetSpaceTriangleMeshMETA", (void *)GetSpaceTriangleMeshMETA},
    {"xrCreateTriangleMeshFB", (void *)CreateTriangleMeshFB},
    {"xrDestroyTriangleMeshFB", (void *)DestroyTriangleMeshFB},
    {"xrTriangleMeshGetVertexBufferFB", (void *)TriangleMeshGetVertexBufferFB},
    {"xrTriangleMeshGetIndexBufferFB", (void *)TriangleMeshGetIndexBufferFB},
    {"xrTriangleMeshBeginUpdateFB", (void *)TriangleMeshBeginUpdateFB},
    {"xrTriangleMeshEndUpdateFB", (void *)TriangleMeshEndUpdateFB},
    {"xrTriangleMeshBeginVertexBufferUpdateFB", (void *)TriangleMeshBeginVertexBufferUpdateFB},
    {"xrTriangleMeshEndVertexBufferUpdateFB", (void *)TriangleMeshEndVertexBufferUpdateFB},
    {"xrCreatePassthroughColorLutMETA", (void *)CreatePassthroughColorLutMETA},
    {"xrDestroyPassthroughColorLutMETA", (void *)DestroyPassthroughColorLutMETA},
    {"xrUpdatePassthroughColorLutMETA", (void *)UpdatePassthroughColorLutMETA},
    {"xrGetPassthroughPreferencesMETA", (void *)GetPassthroughPreferencesMETA},
    {"xrPassthroughLayerSetKeyboardHandsIntensityFB", (void *)PassthroughLayerSetKeyboardHandsIntensityFB},
    {"xrCreatePlaneDetectorEXT", (void *)CreatePlaneDetectorEXT},
    {"xrDestroyPlaneDetectorEXT", (void *)DestroyPlaneDetectorEXT},
    {"xrBeginPlaneDetectionEXT", (void *)BeginPlaneDetectionEXT},
    {"xrGetPlaneDetectionStateEXT", (void *)GetPlaneDetectionStateEXT},
    {"xrGetPlaneDetectionsEXT", (void *)GetPlaneDetectionsEXT},
    {"xrGetPlanePolygonBufferEXT", (void *)GetPlanePolygonBufferEXT},
    {"xrCreateEnvironmentDepthProviderMETA", (void *)ED_CreateProvider},
    {"xrDestroyEnvironmentDepthProviderMETA", (void *)ED_DestroyProvider},
    {"xrStartEnvironmentDepthProviderMETA", (void *)ED_StartProvider},
    {"xrStopEnvironmentDepthProviderMETA", (void *)ED_StopProvider},
    {"xrCreateEnvironmentDepthSwapchainMETA", (void *)ED_CreateSwapchain},
    {"xrDestroyEnvironmentDepthSwapchainMETA", (void *)ED_DestroySwapchain},
    {"xrGetEnvironmentDepthSwapchainStateMETA", (void *)ED_GetSwapchainState},
    {"xrEnumerateEnvironmentDepthSwapchainImagesMETA", (void *)ED_EnumerateImages},
    {"xrAcquireEnvironmentDepthImageMETA", (void *)ED_Acquire},
    {"xrSetEnvironmentDepthHandRemovalMETA", (void *)ED_SetHandRemoval},
    {"xrCreateEnvironmentDepthProviderMETAX1", (void *)ED_CreateProvider},
    {"xrDestroyEnvironmentDepthProviderMETAX1", (void *)ED_DestroyProvider},
    {"xrStartEnvironmentDepthProviderMETAX1", (void *)ED_StartProvider},
    {"xrStopEnvironmentDepthProviderMETAX1", (void *)ED_StopProvider},
    {"xrCreateEnvironmentDepthSwapchainMETAX1", (void *)ED_CreateSwapchain},
    {"xrAcquireEnvironmentDepthImageMETAX1", (void *)ED_Acquire},
};

// Entry points that answer XR_ERROR_FEATURE_UNSUPPORTED (no sharing, no keyboard,
// no markers, colocation, dynamic objects, virtual keyboard; depth is served: ED_*).
static const char *const k_feature_unsupported[] = {
    "xrShareSpacesFB", "xrShareSpacesMETA", "xrGetSpaceHeightMapFB",
    "xrCreateKeyboardSpaceFB", "xrQuerySystemTrackedKeyboardFB",
    "xrCreateMarkerTrackerAsyncMETAX1", "xrCreateMarkerTrackerCompleteMETAX1", "xrGetSpaceMarkerPayloadMETAX1",
    "xrStartColocationAdvertisementMETA", "xrStopColocationAdvertisementMETA",
    "xrStartColocationDiscoveryMETA", "xrStopColocationDiscoveryMETA",
    "xrCreateDynamicObjectTrackerMETA", "xrSetDynamicObjectTrackedClassesMETA", "xrGetSpaceDynamicObjectDataMETA",
    "xrCreateVirtualKeyboardMETA", "xrCreateVirtualKeyboardSpaceMETA", "xrSuggestVirtualKeyboardLocationMETA",
    "xrGetVirtualKeyboardScaleMETA", "xrSetVirtualKeyboardModelVisibilityMETA",
    "xrGetVirtualKeyboardModelAnimationStatesMETA", "xrGetVirtualKeyboardDirtyTexturesMETA",
    "xrGetVirtualKeyboardTextureDataMETA", "xrChangeVirtualKeyboardTextContextMETA",
    "xrSendVirtualKeyboardInputMETA",
    "xrDiscoverSpacesMETA", "xrRetrieveSpaceDiscoveryResultsMETA", "xrSaveSpacesMETA", "xrEraseSpacesMETA",
    "xrGetSpaceRoomMeshMETA", "xrGetSpaceRoomMeshFaceIndicesMETA", "xrGetSpaceRoomMeshMETAX2",
    "xrGetSpaceRoomFaceIndicesMETAX2", "xrGetSpaceRoomLabelMETAX1",
    "xrCreateEnvironmentRaycasterAsyncMETA", "xrCreateEnvironmentRaycasterCompleteMETA",
    "xrPerformEnvironmentRaycastMETA", "xrCreateEnvironmentRaycasterAsyncMETAX1",
    "xrCreateEnvironmentRaycasterCompleteMETAX1", "xrPerformEnvironmentRaycastMETAX1",
    "xrCreateSpatialContextAsyncEXT", "xrCreateSpatialContextCompleteEXT",
    "xrCreateSpatialDiscoverySnapshotAsyncEXT", "xrCreateSpatialDiscoverySnapshotCompleteEXT",
    "xrCreateSpatialUpdateSnapshotEXT", "xrCreateSpatialEntityFromIdEXT", "xrCreateSpatialAnchorEXT",
    "xrCreateSpatialPersistenceContextAsyncEXT", "xrCreateSpatialPersistenceContextCompleteEXT",
    "xrPersistSpatialEntityAsyncEXT", "xrPersistSpatialEntityCompleteEXT",
    "xrUnpersistSpatialEntityAsyncEXT", "xrUnpersistSpatialEntityCompleteEXT",
    "xrCreatePlaneFB", "xrCreatePlaneTrackerFB", "xrCreateRoomEntityFB", "xrCreateSpaceTriangleMeshFB", "xrCreateVolumeFB",
    "xrCreateContextualSpatialAnchorMETAI", "xrCreateLocalizationTrackerMETAI", "xrCreateRoomPolygonMETAI",
    "xrCreateSceneReconstructionTrackerMETAI", "xrCreateSceneTrackerMETAI",
    NULL
};
// Everything else of those families: no handle can exist, so the name is simply
// not served (XR_ERROR_FUNCTION_UNSUPPORTED, logged once).
static const char *const k_refused[] = {
    "xrDestroyMarkerTrackerMETAX1",
    "xrDestroyDynamicObjectTrackerMETA",
    "xrDestroyVirtualKeyboardMETA", "xrDestroySceneObjectTrackerMETA",
    "xrDestroyEnvironmentRaycasterMETA", "xrDestroyEnvironmentRaycasterMETAX1",
    "xrPollFutureEXT", "xrCancelFutureEXT",
    "xrEnumerateSpatialCapabilitiesEXT", "xrEnumerateSpatialCapabilityComponentTypesEXT",
    "xrEnumerateSpatialCapabilityFeaturesEXT", "xrDestroySpatialContextEXT",
    "xrDestroySpatialSnapshotEXT", "xrQuerySpatialComponentDataEXT",
    "xrGetSpatialBufferStringEXT", "xrGetSpatialBufferUint8EXT", "xrGetSpatialBufferUint16EXT",
    "xrGetSpatialBufferUint32EXT", "xrGetSpatialBufferFloatEXT", "xrGetSpatialBufferVector2fEXT",
    "xrGetSpatialBufferVector3fEXT", "xrDestroySpatialEntityEXT",
    "xrEnumerateSpatialPersistenceScopesEXT", "xrDestroySpatialPersistenceContextEXT",
    "xrAddSpaceContextMETAI", "xrApplySpacePlacementRecommendationMETAI",
    "xrComputeSpacePlacementRecommendationsMETAI", "xrDestroyLocalizationTrackerMETAI",
    "xrDestroySceneReconstructionTrackerMETAI", "xrDestroySceneTrackerMETAI", "xrIsRoomResumableMETAI",
    "xrRetrieveSpacePlacementRecommendationsMETAI", "xrUpdateRoomEntityMETAI",
    "xrUpdateSceneReconstructionTrackerRoomboxMETAI", "xrUpdateSceneReconstructionTrackerSettingsMETAI",
    "xrUpdateSceneTrackerRoomboxMETAI", "xrUpdateSceneTrackerSettingsMETAI",
    "xrDestroyPlaneTrackerFB", "xrGetPlaneTrackerStatusFB", "xrUpdatePlaneFB",
    "xrUpdatePlaneTrackerSettingsFB", "xrUpdateVolumeFB",
    NULL
};

static const char *const k_fns_entity[] = { "xrCreateSpatialAnchorFB", "xrGetSpaceUuidFB",
    "xrEnumerateSpaceSupportedComponentsFB", "xrSetSpaceComponentStatusFB", "xrGetSpaceComponentStatusFB", NULL };
static const char *const k_fns_query[] = { "xrQuerySpacesFB", "xrRetrieveSpaceQueryResultsFB", NULL };
static const char *const k_fns_storage[] = { "xrSaveSpaceFB", "xrEraseSpaceFB", NULL };
static const char *const k_fns_batch[] = { "xrSaveSpaceListFB", NULL };
static const char *const k_fns_user[] = { "xrCreateSpaceUserFB", "xrGetSpaceUserIdFB", "xrDestroySpaceUserFB", NULL };
static const char *const k_fns_container[] = { "xrGetSpaceContainerFB", NULL };
static const char *const k_fns_scene[] = { "xrGetSpaceBoundingBox2DFB", "xrGetSpaceBoundingBox3DFB",
    "xrGetSpaceSemanticLabelsFB", "xrGetSpaceBoundary2DFB", "xrGetSpaceRoomLayoutFB", NULL };
static const char *const k_fns_capture[] = { "xrRequestSceneCaptureFB", NULL };
static const char *const k_fns_mesh[] = { "xrGetSpaceTriangleMeshMETA", NULL };
static const char *const k_fns_trimesh[] = { "xrCreateTriangleMeshFB", "xrDestroyTriangleMeshFB",
    "xrTriangleMeshGetVertexBufferFB", "xrTriangleMeshGetIndexBufferFB", "xrTriangleMeshBeginUpdateFB",
    "xrTriangleMeshEndUpdateFB", "xrTriangleMeshBeginVertexBufferUpdateFB",
    "xrTriangleMeshEndVertexBufferUpdateFB", NULL };
static const char *const k_fns_lut[] = { "xrCreatePassthroughColorLutMETA", "xrDestroyPassthroughColorLutMETA",
    "xrUpdatePassthroughColorLutMETA", NULL };
static const char *const k_fns_pref[] = { "xrGetPassthroughPreferencesMETA", NULL };
static const char *const k_fns_envdepth[] = { "xrCreateEnvironmentDepthProviderMETA",
    "xrDestroyEnvironmentDepthProviderMETA", "xrStartEnvironmentDepthProviderMETA",
    "xrStopEnvironmentDepthProviderMETA", "xrCreateEnvironmentDepthSwapchainMETA",
    "xrDestroyEnvironmentDepthSwapchainMETA", "xrGetEnvironmentDepthSwapchainStateMETA",
    "xrEnumerateEnvironmentDepthSwapchainImagesMETA", "xrAcquireEnvironmentDepthImageMETA",
    "xrSetEnvironmentDepthHandRemovalMETA", NULL };
static const char *const k_fns_planes[] = { "xrCreatePlaneDetectorEXT", "xrDestroyPlaneDetectorEXT",
    "xrBeginPlaneDetectionEXT", "xrGetPlaneDetectionStateEXT", "xrGetPlaneDetectionsEXT",
    "xrGetPlanePolygonBufferEXT", NULL };

void kl_openxr_spatial_register(void) {
    klxr_register_rows(k_rows, sizeof k_rows / sizeof k_rows[0]);
    // FEATURE_UNSUPPORTED rows: same function for every name (args are ignored).
    klxr_row_def fu[96]; size_t nfu = 0;
    for (const char *const *n = k_feature_unsupported; *n && nfu < 96; n++)
        fu[nfu++] = (klxr_row_def){ *n, (void *)FeatureUnsupported };
    klxr_register_rows(fu, nfu);
    klxr_register_refused("XR_META/FB/EXT spatial (not served)", k_refused);

    // Anchors need no ARKit; scene data does (gate_scene). None is advertised until
    // kl_openxr.c provides klxr_space_create_external.
    klxr_register_extension("XR_FB_spatial_entity", 3, gate_anchors, k_fns_entity);
    klxr_register_extension("XR_FB_spatial_entity_query", 1, gate_anchors, k_fns_query);
    klxr_register_extension("XR_FB_spatial_entity_storage", 1, gate_anchors, k_fns_storage);
    klxr_register_extension("XR_FB_spatial_entity_storage_batch", 1, gate_anchors, k_fns_batch);
    klxr_register_extension("XR_FB_spatial_entity_user", 1, gate_anchors, k_fns_user);
    klxr_register_extension("XR_FB_spatial_entity_container", 2, gate_scene, k_fns_container);
    klxr_register_extension("XR_FB_scene", 4, gate_scene, k_fns_scene);
    klxr_register_extension("XR_FB_scene_capture", 1, gate_scene, k_fns_capture);
    klxr_register_extension("XR_META_spatial_entity_mesh", 1, gate_mesh, k_fns_mesh);
    klxr_register_extension("XR_EXT_plane_detection", 2, gate_planes, k_fns_planes);
    // Depth is the mesh, so it follows the scene reconstruction (ARKit running and authorised).
    klxr_register_extension("XR_META_environment_depth", 1, gate_envdepth, k_fns_envdepth);
    // Pure CPU / no-op, independent of ARKit.
    klxr_register_extension("XR_FB_triangle_mesh", 2, gate_always, k_fns_trimesh);
    klxr_register_extension("XR_META_passthrough_color_lut", 1, gate_always, k_fns_lut);
    klxr_register_extension("XR_META_passthrough_preferences", 1, gate_always, k_fns_pref);
}
