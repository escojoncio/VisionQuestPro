// kl_spatial.c - the spatial store (planes, meshes, rooms, anchors, async
// requests, CPU raycast) behind kl_spatial.h. Lote 3 implements it; Lote 4
// (kl_openxr_spatial.c) and Lote 3's ovrp facade (kl_ovrp_spatial.c) consume it.
//
// Writers are ARKit's tasks (KleptonSpatial.swift); readers are guest threads.
// One mutex guards everything and readers copy out under it, so nothing here
// retains a pointer into Swift memory or hands one of ours to the guest.
//
// Coordinates: the store keeps what ARKit said (origin of the visionOS world).
// Every reader converts with kl_spatial_to_guest(), the SAME transform the
// guest's head goes through: the compositor's recenterXform, then the guest
// origin Y that kl_ovrp subtracts from every pose it reports
// (kl_ovrp_guest_origin_y: 0 under floor/stage tracking, the eye height under
// EyeLevel). Re-evaluated on every read, so an anchor stays world-locked when
// the eye height is re-latched.
#include "kl_spatial.h"
#include "kl_ovrp.h"      // kl_ovrp_guest_origin_y
#include "kl_jni.h"       // kl_jni_files_dir
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KLSP_MAX_ENT        4096
#define KLSP_MAX_HANDLES    8192
#define KLSP_MAX_SAVED      1024
#define KLSP_MAX_PEND       128
#define KLSP_MAX_LISTENERS  4
#define KLSP_MESH_V_MAX     65535u        // vertices per mesh anchor
#define KLSP_MESH_V_TOTAL   300000u       // vertices over all meshes (3.6 MB)
#define KLSP_POLY_MAX       256

typedef struct {
    int kind;
    kl_uuid id;
    float m[16];                // as ARKit/Swift gave it
    float w, h;
    int label;
    int tracked;
    float *poly; int npoly;     // plane polygon (xy pairs) or NULL
    float *v; uint32_t nv;      // mesh vertices (xyz), local
    uint32_t *t; uint32_t nt;   // mesh triangles (3 indices each)
    kl_uuid *mem; uint32_t nmem;   // room: plane ids
    int mine;                   // anchor: visible to this game
} ent;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ent *g_e;
static uint32_t g_n, g_cap;

static int g_wants_scene, g_wants_anchors;
static int g_cap_planes, g_cap_mesh, g_cap_room, g_cap_anchors;
static float g_rc[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static kl_spatial_host g_host;
static int g_have_host;
static uint32_t g_mesh_verts;       // total over g_e
static const kl_uuid k_zero;

static void once(int *flag, const char *msg) {
    if (!*flag) { *flag = 1; fprintf(stderr, "  [spatial] %s\n", msg); }
}

// ---- uuid helpers ----------------------------------------------------------
static int uuid_eq(kl_uuid a, kl_uuid b) { return memcmp(a.b, b.b, 16) == 0; }
static int uuid_zero(kl_uuid a) { return uuid_eq(a, k_zero); }
static kl_uuid k_synth_room = {{'K','L','S','P','A','T','I','A','L','-','R','O','O','M','0','1'}};

static void uuid_str(kl_uuid u, char out[37]) {
    static const char *hx = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hx[u.b[i] >> 4]; out[o++] = hx[u.b[i] & 15];
    }
    out[o] = 0;
}
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int uuid_parse(const char *s, kl_uuid *u) {
    int o = 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return 0; continue; }
        int a = hexv(s[i]), b = hexv(s[i + 1]);
        if (a < 0 || b < 0 || o >= 16) return 0;
        u->b[o++] = (uint8_t)(a * 16 + b); i++;
    }
    return o == 16;
}

// ---- 4x4 column-major helpers (rigid transforms only) -----------------------
static void m_mul(const float *a, const float *b, float *o) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(o, r, sizeof r);
}
static void m_rigid_inv(const float *m, float *o) {
    float r[16] = {0};
    for (int c = 0; c < 3; c++) for (int rr = 0; rr < 3; rr++) r[c * 4 + rr] = m[rr * 4 + c];
    for (int rr = 0; rr < 3; rr++)
        r[12 + rr] = -(r[0 + rr] * m[12] + r[4 + rr] * m[13] + r[8 + rr] * m[14]);
    r[15] = 1;
    memcpy(o, r, sizeof r);
}

void kl_spatial_set_recenter(const float m4[16]) {
    pthread_mutex_lock(&g_mu);
    memcpy(g_rc, m4, sizeof g_rc);
    pthread_mutex_unlock(&g_mu);
}
static void to_guest_locked(const float *in, float *out) {
    m_mul(g_rc, in, out);
    out[13] -= kl_ovrp_guest_origin_y();
}
void kl_spatial_to_guest(const float arkit_m4[16], float guest_m4[16]) {
    float t[16];
    pthread_mutex_lock(&g_mu);
    to_guest_locked(arkit_m4, t);
    pthread_mutex_unlock(&g_mu);
    memcpy(guest_m4, t, sizeof t);
}
void kl_spatial_from_guest(const float guest_m4[16], float arkit_m4[16]) {
    float t[16], ri[16];
    memcpy(t, guest_m4, sizeof t);
    t[13] += kl_ovrp_guest_origin_y();
    pthread_mutex_lock(&g_mu);
    m_rigid_inv(g_rc, ri);
    pthread_mutex_unlock(&g_mu);
    m_mul(ri, t, arkit_m4);
}

// ---- store -----------------------------------------------------------------
static ent *find_locked(kl_uuid id) {
    for (uint32_t i = 0; i < g_n; i++) if (uuid_eq(g_e[i].id, id)) return &g_e[i];
    return NULL;
}
static void ent_free(ent *e) {
    if (e->kind == KLSP_MESH) g_mesh_verts -= e->nv;
    free(e->poly); free(e->v); free(e->t); free(e->mem);
}
static void del_locked(ent *e) {
    ent_free(e);
    *e = g_e[--g_n];
}
// Synthetic box-room planes (kl_spatial_synth_room) carry this uuid prefix; real ARKit data replaces them.
static int is_synth(kl_uuid id) { return memcmp(id.b, "KLSYNTH", 7) == 0; }
static void purge_synth_locked(void) {
    for (uint32_t i = 0; i < g_n; ) {
        if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id)) del_locked(&g_e[i]);
        else i++;
    }
}
static ent *upsert_locked(int kind, kl_uuid id) {
    ent *e = find_locked(id);
    if (e) return e->kind == kind ? e : NULL;       // a uuid never changes kind
    if (g_n >= KLSP_MAX_ENT) return NULL;
    if (g_n == g_cap) {
        uint32_t nc = g_cap ? g_cap * 2 : 64;
        ent *ne = (ent *)realloc(g_e, nc * sizeof(ent));
        if (!ne) return NULL;
        g_e = ne; g_cap = nc;
    }
    e = &g_e[g_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind; e->id = id;
    return e;
}

// ---- anchors persisted per game (anclas.json) --------------------------------
static kl_uuid g_sv[KLSP_MAX_SAVED];
static uint32_t g_nsv;
static int g_sv_loaded;

static int sv_path(char *buf, size_t n) {
    const char *d = kl_jni_files_dir();
    if (!d || !*d) return 0;
    snprintf(buf, n, "%s/anclas.json", d);
    return 1;
}
static void sv_load_locked(void) {
    if (g_sv_loaded) return;
    char path[1024];
    if (!sv_path(path, sizeof path)) return;      // files dir not known yet: try again later
    g_sv_loaded = 1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = (char *)malloc(64 * 1024 + 1);
    size_t n = buf ? fread(buf, 1, 64 * 1024, f) : 0;
    fclose(f);
    if (!buf) return;
    buf[n] = 0;
    for (size_t i = 0; i + 36 <= n && g_nsv < KLSP_MAX_SAVED; i++) {
        if (buf[i] != '"') continue;
        kl_uuid u;
        if (i + 37 < n && uuid_parse(buf + i + 1, &u) && buf[i + 37] == '"') { g_sv[g_nsv++] = u; i += 37; }
    }
    free(buf);
    fprintf(stderr, "  [spatial] %u saved anchors read from %s\n", g_nsv, path);
}
static void sv_write_locked(void) {
    char path[1024], tmp[1100];
    if (!sv_path(path, sizeof path)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fputs("{\"anchors\":[", f);
    for (uint32_t i = 0; i < g_nsv; i++) {
        char s[37]; uuid_str(g_sv[i], s);
        fprintf(f, "%s\"%s\"", i ? "," : "", s);
    }
    fputs("]}\n", f);
    int ok = fclose(f) == 0;
    if (ok) rename(tmp, path); else remove(tmp);
}
static int sv_index(kl_uuid id) {
    for (uint32_t i = 0; i < g_nsv; i++) if (uuid_eq(g_sv[i], id)) return (int)i;
    return -1;
}

void kl_spatial_anchor_set_saved(kl_uuid id, int saved) {
    pthread_mutex_lock(&g_mu);
    sv_load_locked();
    int ix = sv_index(id);
    if (saved && ix < 0 && g_nsv < KLSP_MAX_SAVED) { g_sv[g_nsv++] = id; sv_write_locked(); }
    else if (!saved && ix >= 0) { g_sv[ix] = g_sv[--g_nsv]; sv_write_locked(); }
    pthread_mutex_unlock(&g_mu);
}
int kl_spatial_anchor_saved(kl_uuid id) {
    pthread_mutex_lock(&g_mu);
    sv_load_locked();
    int r = sv_index(id) >= 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}
int kl_spatial_anchor_tracked(kl_uuid id) {
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    int r = e && e->kind == KLSP_ANCHOR && e->tracked;
    pthread_mutex_unlock(&g_mu);
    return r;
}

// ---- Swift -> C ------------------------------------------------------------
void kl_spatial_set_policy(int wants_scene, int wants_anchors) {
    static int logged;
    pthread_mutex_lock(&g_mu);
    g_wants_scene = wants_scene; g_wants_anchors = wants_anchors;
    pthread_mutex_unlock(&g_mu);
    if (!logged) { logged = 1; fprintf(stderr, "  [spatial] policy: scene=%d anchors=%d\n", wants_scene, wants_anchors); }
}
void kl_spatial_policy(int *wants_scene, int *wants_anchors) {
    pthread_mutex_lock(&g_mu);
    if (wants_scene) *wants_scene = g_wants_scene;
    if (wants_anchors) *wants_anchors = g_wants_anchors;
    pthread_mutex_unlock(&g_mu);
}
void kl_spatial_set_available(int planes_ok, int mesh_ok, int room_ok, int anchors_ok) {
    pthread_mutex_lock(&g_mu);
    g_cap_planes = planes_ok != 0; g_cap_mesh = mesh_ok != 0;
    g_cap_room = room_ok != 0; g_cap_anchors = anchors_ok != 0;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [spatial] available: planes=%d mesh=%d room=%d anchors=%d\n",
            planes_ok != 0, mesh_ok != 0, room_ok != 0, anchors_ok != 0);
}
int kl_spatial_available(int cap) {
    pthread_mutex_lock(&g_mu);
    int r = (cap == KLSP_CAP_PLANES && g_cap_planes) || (cap == KLSP_CAP_MESH && g_cap_mesh)
         || (cap == KLSP_CAP_ROOM && g_cap_room) || (cap == KLSP_CAP_ANCHORS && g_cap_anchors);
    pthread_mutex_unlock(&g_mu);
    return r;
}

void kl_spatial_plane(kl_uuid id, int removed, const float m4[16], float w, float h, int label,
                      const float *poly2d, int npoly) {
    static int lg;
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    if (removed) { if (e && e->kind == KLSP_PLANE) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    e = upsert_locked(KLSP_PLANE, id);
    if (e) {
        memcpy(e->m, m4, sizeof e->m);
        e->w = w; e->h = h; e->label = label; e->tracked = 1;
        if (!is_synth(id)) {        // real planes replace the synthetic room once they hold a floor and 3+ walls
            int fl = 0, wl = 0;
            for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && !is_synth(g_e[i].id)) {
                fl += g_e[i].label == KLSP_LBL_FLOOR; wl += g_e[i].label == KLSP_LBL_WALL;
            }
            if (fl >= 1 && wl >= 3) purge_synth_locked();
        }
        free(e->poly); e->poly = NULL; e->npoly = 0;
        if (poly2d && npoly >= 3 && npoly <= KLSP_POLY_MAX) {
            e->poly = (float *)malloc((size_t)npoly * 2 * sizeof(float));
            if (e->poly) { memcpy(e->poly, poly2d, (size_t)npoly * 2 * sizeof(float)); e->npoly = npoly; }
        }
    }
    pthread_mutex_unlock(&g_mu);
    once(&lg, "first plane received");
}

void kl_spatial_mesh(kl_uuid id, int removed, const float m4[16], const float *verts3, uint32_t nv,
                     const uint32_t *tri3, uint32_t nt) {
    static int lg, lcap;
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    if (removed) { if (e && e->kind == KLSP_MESH) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    if (nv == 0 || nt == 0 || !verts3 || !tri3) { if (e && e->kind == KLSP_MESH) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    if (nv > KLSP_MESH_V_MAX) { pthread_mutex_unlock(&g_mu); once(&lcap, "mesh anchor over the per-mesh vertex cap: ignored"); return; }
    uint32_t old = (e && e->kind == KLSP_MESH) ? e->nv : 0;
    if (g_mesh_verts - old + nv > KLSP_MESH_V_TOTAL) { pthread_mutex_unlock(&g_mu); once(&lcap, "scene mesh over the total vertex cap: update ignored"); return; }
    float *v = (float *)malloc((size_t)nv * 3 * sizeof(float));
    uint32_t *t = (uint32_t *)malloc((size_t)nt * 3 * sizeof(uint32_t));
    if (!v || !t) { free(v); free(t); pthread_mutex_unlock(&g_mu); return; }
    memcpy(v, verts3, (size_t)nv * 3 * sizeof(float));
    memcpy(t, tri3, (size_t)nt * 3 * sizeof(uint32_t));
    e = upsert_locked(KLSP_MESH, id);
    if (!e) { free(v); free(t); pthread_mutex_unlock(&g_mu); return; }
    g_mesh_verts -= e->nv;
    free(e->v); free(e->t);
    e->v = v; e->t = t; e->nv = nv; e->nt = nt; e->tracked = 1;
    g_mesh_verts += nv;
    memcpy(e->m, m4, sizeof e->m);
    pthread_mutex_unlock(&g_mu);
    once(&lg, "first scene mesh received");
}

void kl_spatial_room(kl_uuid id, int removed, const kl_uuid *plane_ids, uint32_t n) {
    static int lg;
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    if (removed) { if (e && e->kind == KLSP_ROOM) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    purge_synth_locked();           // a real room replaces the synthetic one
    e = upsert_locked(KLSP_ROOM, id);
    if (e) {
        free(e->mem); e->mem = NULL; e->nmem = 0;
        if (plane_ids && n) {
            if (n > 1024) n = 1024;
            e->mem = (kl_uuid *)malloc(n * sizeof(kl_uuid));
            if (e->mem) { memcpy(e->mem, plane_ids, n * sizeof(kl_uuid)); e->nmem = n; }
        }
        e->tracked = 1;
    }
    pthread_mutex_unlock(&g_mu);
    once(&lg, "first room received");
}

void kl_spatial_anchor(kl_uuid id, int removed, const float m4[16], int tracked) {
    static int lg;
    pthread_mutex_lock(&g_mu);
    sv_load_locked();
    ent *e = find_locked(id);
    if (removed) { if (e && e->kind == KLSP_ANCHOR) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    int fresh = e == NULL;
    e = upsert_locked(KLSP_ANCHOR, id);
    if (e) {
        memcpy(e->m, m4, sizeof e->m);
        e->tracked = tracked != 0;
        if (fresh) e->mine = sv_index(id) >= 0;   // WorldAnchors are per APP: only this game's are shown
    }
    pthread_mutex_unlock(&g_mu);
    once(&lg, "first world anchor received");
}

// ---- async requests ---------------------------------------------------------
typedef struct { uint64_t req; int kind; kl_uuid id; } pend;      // kind 1 create, 2 remove
static pend g_pend[KLSP_MAX_PEND];
static uint32_t g_npend;
static uint64_t g_reqctr = 0x100;
static kl_spatial_listener g_lis[KLSP_MAX_LISTENERS];
static uint32_t g_nlis;

uint64_t kl_spatial_new_request(void) {
    return __atomic_add_fetch(&g_reqctr, 1, __ATOMIC_RELAXED);
}
void kl_spatial_add_listener(kl_spatial_listener fn) {
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nlis; i++) if (g_lis[i] == fn) { pthread_mutex_unlock(&g_mu); return; }
    if (g_nlis < KLSP_MAX_LISTENERS) g_lis[g_nlis++] = fn;
    pthread_mutex_unlock(&g_mu);
}
void kl_spatial_set_host(const kl_spatial_host *h) {
    pthread_mutex_lock(&g_mu);
    if (h) { g_host = *h; g_have_host = 1; } else { memset(&g_host, 0, sizeof g_host); g_have_host = 0; }
    pthread_mutex_unlock(&g_mu);
}
static int pend_add_locked(uint64_t req, int kind, kl_uuid id) {
    if (g_npend >= KLSP_MAX_PEND) return 0;
    g_pend[g_npend].req = req; g_pend[g_npend].kind = kind; g_pend[g_npend].id = id; g_npend++;
    return 1;
}
int kl_spatial_create_anchor(uint64_t req, const float guest_m4[16]) {
    float a[16];
    kl_spatial_from_guest(guest_m4, a);
    pthread_mutex_lock(&g_mu);
    void (*fn)(uint64_t, const float *) = (g_have_host && g_cap_anchors) ? g_host.create_anchor : NULL;
    int ok = fn && pend_add_locked(req, 1, k_zero);
    pthread_mutex_unlock(&g_mu);
    if (!ok) return -1;
    fn(req, a);
    return 0;
}
int kl_spatial_remove_anchor(uint64_t req, kl_uuid id) {
    pthread_mutex_lock(&g_mu);
    void (*fn)(uint64_t, kl_uuid) = (g_have_host && g_cap_anchors) ? g_host.remove_anchor : NULL;
    int ok = fn && pend_add_locked(req, 2, id);
    pthread_mutex_unlock(&g_mu);
    if (!ok) return -1;
    fn(req, id);
    return 0;
}
void kl_spatial_request_done(uint64_t req, int ok, kl_uuid id) {
    kl_spatial_listener ls[KLSP_MAX_LISTENERS];
    uint32_t nl;
    pthread_mutex_lock(&g_mu);
    sv_load_locked();
    for (uint32_t i = 0; i < g_npend; i++) {
        if (g_pend[i].req != req) continue;
        int kind = g_pend[i].kind;
        kl_uuid rid = uuid_zero(id) ? g_pend[i].id : id;
        g_pend[i] = g_pend[--g_npend];
        if (kind == 1 && ok) {
            ent *e = find_locked(rid);
            if (e && e->kind == KLSP_ANCHOR) e->mine = 1;
        } else if (kind == 2 && ok) {
            ent *e = find_locked(rid);
            if (e && e->kind == KLSP_ANCHOR) del_locked(e);
            int ix = sv_index(rid);
            if (ix >= 0) { g_sv[ix] = g_sv[--g_nsv]; sv_write_locked(); }
        }
        id = rid;
        break;
    }
    nl = g_nlis; memcpy(ls, g_lis, sizeof ls);
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < nl; i++) ls[i](req, ok, id);
}

// ---- handles ---------------------------------------------------------------
// Handles are small (< 4 GB) on purpose: no valid user pointer on arm64 is, so a
// facade can tell "the handle by value" from "a pointer to the handle" (the
// OVRPlugin entry points take it as `ref UInt64`).
static kl_uuid g_hu[KLSP_MAX_HANDLES];
static uint32_t g_nh;
#define KLSP_H_BASE 0x1000ull

uint64_t kl_spatial_handle_for(kl_uuid id) {
    if (uuid_zero(id)) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t h = 0;
    for (uint32_t i = 0; i < g_nh; i++) if (uuid_eq(g_hu[i], id)) { h = KLSP_H_BASE + i; break; }
    if (!h && g_nh < KLSP_MAX_HANDLES) { g_hu[g_nh] = id; h = KLSP_H_BASE + g_nh; g_nh++; }
    pthread_mutex_unlock(&g_mu);
    return h;
}
static int kind_locked(kl_uuid id) {
    if (uuid_eq(id, k_synth_room)) {
        for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE) return KLSP_ROOM;
        return 0;
    }
    ent *e = find_locked(id);
    if (!e) return 0;
    if (e->kind == KLSP_ANCHOR && !e->mine) return 0;
    return e->kind;
}
int kl_spatial_kind_of(kl_uuid id) {
    pthread_mutex_lock(&g_mu);
    int k = kind_locked(id);
    pthread_mutex_unlock(&g_mu);
    return k;
}
int kl_spatial_uuid_for_handle(uint64_t h, kl_uuid *id, int *kind) {
    if (h < KLSP_H_BASE) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t ix = h - KLSP_H_BASE;
    int ok = ix < g_nh;
    if (ok) { if (id) *id = g_hu[ix]; if (kind) *kind = kind_locked(g_hu[ix]); }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---- rooms ------------------------------------------------------------------
typedef struct { float ang; kl_uuid id; } wall_ang;
static int wall_cmp(const void *a, const void *b) {
    float x = ((const wall_ang *)a)->ang, y = ((const wall_ang *)b)->ang;
    return x < y ? -1 : x > y;
}
static int room_info_locked(kl_uuid id, kl_uuid *floor, kl_uuid *ceiling,
                            uint32_t wcap, kl_uuid *walls, uint32_t *nwalls,
                            uint32_t mcap, kl_uuid *members, uint32_t *nmembers) {
    ent *room = NULL;
    if (!uuid_eq(id, k_synth_room)) {
        room = find_locked(id);
        if (!room || room->kind != KLSP_ROOM) return 0;
    } else if (kind_locked(id) != KLSP_ROOM) return 0;
    // members: the room's planes that exist; every plane for the synthetic room or a room whose planes we lack
    uint32_t cnt = 0;
    kl_uuid fl = k_zero, ce = k_zero;
    float fly = 1e30f, cey = -1e30f, cx = 0, cz = 0;
    uint32_t nw = 0;
    wall_ang *wa = (wall_ang *)calloc(g_n ? g_n : 1, sizeof(wall_ang));
    if (!wa) return 0;
    int restrict_to_room = room && room->nmem > 0;
    if (restrict_to_room) {
        int any = 0;
        for (uint32_t k = 0; k < room->nmem && !any; k++) {
            ent *p = find_locked(room->mem[k]);
            any = p && p->kind == KLSP_PLANE;
        }
        restrict_to_room = any;
    }
    float wx[1024], wz[1024]; uint32_t wi[1024];
    int has_synth = 0;
    for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id)) has_synth = 1;
    for (uint32_t i = 0; i < g_n; i++) {
        ent *p = &g_e[i];
        if (p->kind != KLSP_PLANE) continue;
        if (!room && !is_synth(p->id) && has_synth && (p->label == KLSP_LBL_FLOOR || p->label == KLSP_LBL_CEILING || p->label == KLSP_LBL_WALL)) continue;
        if (restrict_to_room) {
            int in = 0;
            for (uint32_t k = 0; k < room->nmem; k++) if (uuid_eq(room->mem[k], p->id)) { in = 1; break; }
            if (!in) continue;
        }
        if (members && cnt < mcap) members[cnt] = p->id;
        cnt++;
        if (p->label == KLSP_LBL_FLOOR && p->m[13] < fly) { fly = p->m[13]; fl = p->id; }
        if (p->label == KLSP_LBL_CEILING && p->m[13] > cey) { cey = p->m[13]; ce = p->id; }
        if (p->label == KLSP_LBL_WALL && nw < 1024) {
            wi[nw] = i; wx[nw] = p->m[12]; wz[nw] = p->m[14]; cx += p->m[12]; cz += p->m[14]; nw++;
        }
    }
    if (nw) { cx /= nw; cz /= nw; }
    for (uint32_t k = 0; k < nw; k++) { wa[k].ang = atan2f(wz[k] - cz, wx[k] - cx); wa[k].id = g_e[wi[k]].id; }
    qsort(wa, nw, sizeof(wall_ang), wall_cmp);
    if (floor) *floor = fl;
    if (ceiling) *ceiling = ce;
    if (walls) for (uint32_t k = 0; k < nw && k < wcap; k++) walls[k] = wa[k].id;
    if (nwalls) *nwalls = nw;
    if (nmembers) *nmembers = cnt;
    free(wa);
    return 1;
}
int kl_spatial_room_info(kl_uuid id, kl_uuid *floor, kl_uuid *ceiling,
                         uint32_t wcap, kl_uuid *walls, uint32_t *nwalls,
                         uint32_t mcap, kl_uuid *members, uint32_t *nmembers) {
    pthread_mutex_lock(&g_mu);
    int r = room_info_locked(id, floor, ceiling, wcap, walls, nwalls, mcap, members, nmembers);
    pthread_mutex_unlock(&g_mu);
    return r;
}

// ---- readers ----------------------------------------------------------------
static int real_rooms_locked(void) {
    int n = 0;
    for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_ROOM) n++;
    return n;
}
static int visible(const ent *e) { return e->kind != KLSP_ANCHOR || e->mine; }

// i-th visible entity of a kind (rooms: real ones, then the synthetic one).
static ent *nth_locked(int kind, uint32_t i) {
    for (uint32_t k = 0; k < g_n; k++) {
        if (g_e[k].kind != kind || !visible(&g_e[k])) continue;
        if (i-- == 0) return &g_e[k];
    }
    return NULL;
}
uint32_t kl_spatial_count(int kind) {
    pthread_mutex_lock(&g_mu);
    uint32_t n = 0;
    for (uint32_t k = 0; k < g_n; k++) if (g_e[k].kind == kind && visible(&g_e[k])) n++;
    if (kind == KLSP_ROOM && n == 0 && kind_locked(k_synth_room) == KLSP_ROOM) n = 1;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int kl_spatial_get(int kind, uint32_t i, kl_uuid *id, float m4[16]) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    if (kind == KLSP_ROOM) {
        int real = real_rooms_locked();
        if (i < (uint32_t)real) {
            ent *e = nth_locked(KLSP_ROOM, i);
            if (e) { if (id) *id = e->id; ok = 1; }
        } else if (real == 0 && i == 0 && kind_locked(k_synth_room) == KLSP_ROOM) {
            if (id) *id = k_synth_room;
            ok = 1;
        }
        if (ok && m4) {          // a room sits at the floor under the centroid of its planes
            float cx = 0, cz = 0, fy = 1e30f; int n = 0;
            for (uint32_t k = 0; k < g_n; k++) if (g_e[k].kind == KLSP_PLANE) {
                cx += g_e[k].m[12]; cz += g_e[k].m[14]; n++;
                if (g_e[k].label == KLSP_LBL_FLOOR && g_e[k].m[13] < fy) fy = g_e[k].m[13];
            }
            float a[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            if (n) { a[12] = cx / n; a[14] = cz / n; }
            a[13] = fy < 1e29f ? fy : 0.0f;
            to_guest_locked(a, m4);
        }
    } else {
        ent *e = nth_locked(kind, i);
        if (e) { if (id) *id = e->id; if (m4) to_guest_locked(e->m, m4); ok = 1; }
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int kl_spatial_plane_info(kl_uuid id, float *w, float *h, int *label, uint32_t cap, float *poly2d, uint32_t *npoly) {
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    int ok = e && e->kind == KLSP_PLANE;
    if (ok) {
        if (w) *w = e->w;
        if (h) *h = e->h;
        if (label) *label = e->label;
        if (e->npoly) {
            if (npoly) *npoly = (uint32_t)e->npoly;
            if (poly2d) for (uint32_t i = 0; i < cap && i < (uint32_t)e->npoly; i++) {
                poly2d[2 * i] = e->poly[2 * i]; poly2d[2 * i + 1] = e->poly[2 * i + 1];
            }
        } else {
            float hw = e->w * 0.5f, hh = e->h * 0.5f;
            const float r[8] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh};
            if (npoly) *npoly = 4;
            if (poly2d) for (uint32_t i = 0; i < cap && i < 4; i++) { poly2d[2 * i] = r[2 * i]; poly2d[2 * i + 1] = r[2 * i + 1]; }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int kl_spatial_mesh_info(kl_uuid id, uint32_t vcap, float *v, uint32_t *nv, uint32_t tcap, uint32_t *t, uint32_t *nt) {
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    int ok = e && e->kind == KLSP_MESH;
    if (ok) {
        if (nv) *nv = e->nv;
        if (nt) *nt = e->nt;
        if (v) { uint32_t n = e->nv < vcap ? e->nv : vcap; memcpy(v, e->v, (size_t)n * 3 * sizeof(float)); }
        if (t) { uint32_t n = e->nt < tcap ? e->nt : tcap; memcpy(t, e->t, (size_t)n * 3 * sizeof(uint32_t)); }
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---- raycast (CPU, guest space) ---------------------------------------------
// Ray into the entity's local frame: R^T (p - t). Hit of the nearest plane
// rectangle or mesh triangle within maxd; the normal faces the ray origin.
static void ray_local(const float *g, const float o[3], const float d[3], float lo[3], float ld[3]) {
    for (int r = 0; r < 3; r++) {
        lo[r] = g[r * 4 + 0] * (o[0] - g[12]) + g[r * 4 + 1] * (o[1] - g[13]) + g[r * 4 + 2] * (o[2] - g[14]);
        ld[r] = g[r * 4 + 0] * d[0] + g[r * 4 + 1] * d[1] + g[r * 4 + 2] * d[2];
    }
}
static void local_dir_to_world(const float *g, const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = g[0 + r] * p[0] + g[4 + r] * p[1] + g[8 + r] * p[2];
}

int kl_spatial_raycast(const float o[3], const float d[3], float maxd, float *hit_pos, float *hit_nrm, kl_uuid *hit_id) {
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 1e-9f) || !(maxd > 0)) return 0;
    float dir[3] = {d[0] / dl, d[1] / dl, d[2] / dl};
    float best = maxd, bn[3] = {0, 1, 0};
    kl_uuid bid = k_zero;
    int hit = 0;
    pthread_mutex_lock(&g_mu);
    for (uint32_t k = 0; k < g_n; k++) {
        ent *e = &g_e[k];
        if (e->kind != KLSP_PLANE && e->kind != KLSP_MESH) continue;
        float g[16], lo[3], ld[3];
        to_guest_locked(e->m, g);
        ray_local(g, o, dir, lo, ld);
        if (e->kind == KLSP_PLANE) {
            // ARKit plane frame: normal +Y, rectangle w (X) by h (Z).
            if (fabsf(ld[1]) < 1e-7f) continue;
            float t = -lo[1] / ld[1];
            if (t <= 0 || t >= best) continue;
            float x = lo[0] + ld[0] * t, z = lo[2] + ld[2] * t;
            if (fabsf(x) > e->w * 0.5f || fabsf(z) > e->h * 0.5f) continue;
            float n[3] = {0, ld[1] > 0 ? -1.0f : 1.0f, 0};
            local_dir_to_world(g, n, bn);
            best = t; bid = e->id; hit = 1;
        } else {
            for (uint32_t i = 0; i < e->nt; i++) {
                const float *a = e->v + 3 * (size_t)e->t[3 * i], *b = e->v + 3 * (size_t)e->t[3 * i + 1],
                            *c = e->v + 3 * (size_t)e->t[3 * i + 2];
                if (e->t[3 * i] >= e->nv || e->t[3 * i + 1] >= e->nv || e->t[3 * i + 2] >= e->nv) continue;
                float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                float p[3] = {ld[1] * e2[2] - ld[2] * e2[1], ld[2] * e2[0] - ld[0] * e2[2], ld[0] * e2[1] - ld[1] * e2[0]};
                float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (fabsf(det) < 1e-9f) continue;
                float inv = 1.0f / det;
                float s[3] = {lo[0] - a[0], lo[1] - a[1], lo[2] - a[2]};
                float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (u < 0 || u > 1) continue;
                float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                float vv = (ld[0] * q[0] + ld[1] * q[1] + ld[2] * q[2]) * inv;
                if (vv < 0 || u + vv > 1) continue;
                float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (t <= 0 || t >= best) continue;
                float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl < 1e-12f) continue;
                float sg = (n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2]) > 0 ? -1.0f / nl : 1.0f / nl;
                n[0] *= sg; n[1] *= sg; n[2] *= sg;
                local_dir_to_world(g, n, bn);
                best = t; bid = e->id; hit = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (!hit) return 0;
    if (hit_pos) { hit_pos[0] = o[0] + dir[0] * best; hit_pos[1] = o[1] + dir[1] * best; hit_pos[2] = o[2] + dir[2] * best; }
    if (hit_nrm) { hit_nrm[0] = bn[0]; hit_nrm[1] = bn[1]; hit_nrm[2] = bn[2]; }
    if (hit_id) *hit_id = bid;
    return 1;
}

// ---- synthetic box room (no ARKit data: Space Setup must still end with a room) ----
// Floor at the tracking floor, ceiling 2.5 m, four walls of a 4x4 m box centred on (hx, hz) (guest space).
// Planes use the ARKit frame (normal +Y; walls: local Z points down so Meta +Y is up, see kl_spatial.h).
// Returns 1 when a synthetic room is present now, 0 when real planes/rooms exist (they win, nothing is added).
int kl_spatial_synth_room(float hx, float hz) {
    const float W = 4.0f, H = 2.5f, oy = kl_ovrp_guest_origin_y(), fy = -oy;
    pthread_mutex_lock(&g_mu);
    int synth = 0;
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_e[i].kind == KLSP_ROOM) { pthread_mutex_unlock(&g_mu); return 0; }
        if (g_e[i].kind == KLSP_PLANE) { if (!is_synth(g_e[i].id)) { pthread_mutex_unlock(&g_mu); return 0; } synth++; }
    }
    if (synth == 6) { pthread_mutex_unlock(&g_mu); return 1; }
    purge_synth_locked();                              // incomplete set: rebuild
    // inward normals: +Z, -X, -Z, +X; tangent t = (nz, 0, -nx)
    static const float nn[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    for (int k = 0; k < 6; k++) {
        float g[16] = {0};
        g[15] = 1;
        int label; float w = W, h = H;
        if (k < 2) {                                   // 0 floor (normal up), 1 ceiling (normal down)
            label = k ? KLSP_LBL_CEILING : KLSP_LBL_FLOOR; h = W;
            g[0] = 1; g[5] = k ? -1.0f : 1.0f; g[10] = k ? -1.0f : 1.0f;
            g[12] = hx; g[13] = fy + (k ? H : 0.0f); g[14] = hz;
        } else {
            label = KLSP_LBL_WALL;
            float nx = nn[k - 2][0], nz = nn[k - 2][1];
            g[0] = nz; g[2] = -nx;                     // X = tangent
            g[4] = nx; g[6] = nz;                      // Y = inward normal
            g[9] = -1.0f;                              // Z = down
            g[12] = hx - nx * W * 0.5f; g[13] = fy + H * 0.5f; g[14] = hz - nz * W * 0.5f;
        }
        float a[16], ri[16];
        g[13] += oy;                                   // as kl_spatial_from_guest, already under the lock
        m_rigid_inv(g_rc, ri);
        m_mul(ri, g, a);
        kl_uuid id = k_zero;
        memcpy(id.b, "KLSYNTH", 7); id.b[8] = (uint8_t)k;
        ent *e = upsert_locked(KLSP_PLANE, id);
        if (!e) { purge_synth_locked(); pthread_mutex_unlock(&g_mu); return 0; }
        memcpy(e->m, a, sizeof e->m);
        e->w = w; e->h = h; e->label = label; e->tracked = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return 1;
}
