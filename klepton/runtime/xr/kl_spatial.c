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
#include "kl_guardian.h"  // floor height, room rules
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static int g_cap_known;             // the providers' state is settled (authorised and running, or denied / unsupported)
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
static uint32_t g_gen;              // rises with every write to the store (upsert / delete); read unlocked via kl_spatial_generation
static void del_locked(ent *e) {
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELAXED);
    ent_free(e);
    *e = g_e[--g_n];
}
// Synthetic box-room planes (kl_spatial_synth_room) carry this uuid prefix; real ARKit data replaces them.
static int is_synth(kl_uuid id) { return memcmp(id.b, "KLSYNTH", 7) == 0; }
static uint32_t g_synth_n;          // planes of the synthetic room as built (6 for the box; 2 + walls for an outline / zone)
static void purge_synth_locked(void) {
    g_synth_n = 0;
    for (uint32_t i = 0; i < g_n; ) {
        if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id)) del_locked(&g_e[i]);
        else i++;
    }
}

// ---- closed box room: pure geometry (host test t_closed_room) ----
// KL-TEST-BEGIN closedroom
#define KLSP_MIN_WALL 0.3f       // m: narrower wall planes are not walls of a room
#define KLSP_MIN_AREA 1.0f       // m2: smallest ring that counts as a room
#define KLSP_MIN_SIDE 1.0f       // m: smallest side of the box that holds the ring
typedef struct { float x0, z0, x1, z1; } klsp_seg;           // wall footprint on the floor (x, z)
typedef struct { float cx, cz, w, d, yaw; } klsp_rect;       // box centre, sides along its X (w) and Z (d), turn about Y
static void klsp_world(const klsp_rect *r, float lx, float lz, float *x, float *z) {
    float c = cosf(r->yaw), sn = sinf(r->yaw);
    *x = r->cx + c * lx - sn * lz; *z = r->cz + sn * lx + c * lz;
}
// The four walls in ascending atan2(z, x) around the centre (clockwise seen from above with Y up); wall k ends where wall k+1 starts.
static void klsp_box_walls(const klsp_rect *r, klsp_seg s[4]) {
    static const float kx[4] = {-1, 1, 1, -1}, kz[4] = {-1, -1, 1, 1};
    float px[4], pz[4];
    for (int k = 0; k < 4; k++) klsp_world(r, kx[k] * r->w * 0.5f, kz[k] * r->d * 0.5f, &px[k], &pz[k]);
    for (int k = 0; k < 4; k++) s[k] = (klsp_seg){px[k], pz[k], px[(k + 1) % 4], pz[(k + 1) % 4]};
}
static float klsp_pt_seg(float px, float pz, const klsp_seg *s) {
    float dx = s->x1 - s->x0, dz = s->z1 - s->z0, l2 = dx * dx + dz * dz;
    float t = l2 > 1e-12f ? ((px - s->x0) * dx + (pz - s->z0) * dz) / l2 : 0.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(px - (s->x0 + t * dx), pz - (s->z0 + t * dz));
}
static klsp_rect klsp_fit_rect(const klsp_seg *s, int n, float hx, float hz, float zw, float zd);
// 1 when the left normal (-dz, dx) of the wall (start -> end) points away from (cx, cz): the side a wall with Z down has
// its plane normal on. Flipping such a wall (start <-> end, normal and tangent negated) points it inward, and for a convex
// ring in ascending angle order the walls then chain end -> start like the synthetic ones.
static int klsp_wall_outward(const klsp_seg *s, float cx, float cz) {
    float dx = s->x1 - s->x0, dz = s->z1 - s->z0;
    return -dz * (cx - (s->x0 + s->x1) * 0.5f) + dx * (cz - (s->z0 + s->z1) * 0.5f) < 0.0f;
}
// 1 when the n (4..64) walls, in angle order around the room, form a valid room: every end point finite, every wall at least
// KLSP_MIN_WALL long, each wall touching the next (the last the first) within tol with its ends in either order, and the
// ring -- corners = midpoint of the closest ends of neighbours -- convex (a few degrees of slack, so a wall split in two
// pieces is fine), of positive area (the angle order) >= KLSP_MIN_AREA, in a box (turned to the longest wall) at least
// KLSP_MIN_SIDE on both sides. So NaN, degenerate, collinear, duplicated and thin rings are rejected, and so is any
// non-convex room (an L is never "complete": the synthetic box stays, fitted around its real walls).
static int klsp_ring_closed(const klsp_seg *s, int n, float tol) {
    if (n < 4 || n > 64) return 0;
    float px[64] = {0}, pz[64] = {0}, mx = 0, mz = 0;
    for (int k = 0; k < n; k++) {
        const klsp_seg *a = &s[k], *b = &s[(k + 1) % n];
        if (!isfinite(a->x0) || !isfinite(a->z0) || !isfinite(a->x1) || !isfinite(a->z1)) return 0;
        if (!(hypotf(a->x1 - a->x0, a->z1 - a->z0) >= KLSP_MIN_WALL)) return 0;
        if (!isfinite(b->x0) || !isfinite(b->z0) || !isfinite(b->x1) || !isfinite(b->z1)) return 0;
        float m = fminf(fminf(klsp_pt_seg(a->x0, a->z0, b), klsp_pt_seg(a->x1, a->z1, b)),
                        fminf(klsp_pt_seg(b->x0, b->z0, a), klsp_pt_seg(b->x1, b->z1, a)));
        if (!(m <= tol)) return 0;
        float ax[2] = {a->x0, a->x1}, az[2] = {a->z0, a->z1}, bx[2] = {b->x0, b->x1}, bz[2] = {b->z0, b->z1}, best = 1e30f;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++) {
                float d = hypotf(ax[i] - bx[j], az[i] - bz[j]);
                if (d < best) { best = d; px[k] = (ax[i] + bx[j]) * 0.5f; pz[k] = (az[i] + bz[j]) * 0.5f; }
            }
        mx += px[k]; mz += pz[k];
    }
    float area = 0;
    for (int k = 0; k < n; k++) {
        int j = (k + 1) % n, l = (k + 2) % n;
        area += px[k] * pz[j] - px[j] * pz[k];
        float ux = px[j] - px[k], uz = pz[j] - pz[k], vx = px[l] - px[j], vz = pz[l] - pz[j];
        if (ux * vz - uz * vx < -0.15f * hypotf(ux, uz) * hypotf(vx, vz)) return 0;
    }
    if (!(area * 0.5f >= KLSP_MIN_AREA)) return 0;
    klsp_rect b = klsp_fit_rect(s, n, mx / n, mz / n, 0.0f, 0.0f);
    return b.w >= KLSP_MIN_SIDE && b.d >= KLSP_MIN_SIDE;
}
// Smallest box, turned to the longest wall, that holds every wall end point and the zone (zw x zd, same turn) around
// the head (hx, hz). With no walls: the zone itself, not turned.
static klsp_rect klsp_fit_rect(const klsp_seg *s, int n, float hx, float hz, float zw, float zd) {
    klsp_rect r = {0, 0, 0, 0, 0}, zr = {hx, hz, zw, zd, 0};
    float best = -1.0f;
    for (int k = 0; k < n; k++) {
        float l = hypotf(s[k].x1 - s[k].x0, s[k].z1 - s[k].z0);
        if (l > best) { best = l; r.yaw = atan2f(s[k].z1 - s[k].z0, s[k].x1 - s[k].x0); }
    }
    zr.yaw = r.yaw;
    float c = cosf(r.yaw), sn = sinf(r.yaw), lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (int i = 0; i < 2 * n + 4; i++) {
        float x, z, u, v;
        int j = i - 2 * n;
        if (j < 0) { x = i % 2 ? s[i / 2].x1 : s[i / 2].x0; z = i % 2 ? s[i / 2].z1 : s[i / 2].z0; }
        else klsp_world(&zr, (j & 1 ? 0.5f : -0.5f) * zw, (j & 2 ? 0.5f : -0.5f) * zd, &x, &z);
        u = c * x + sn * z; v = -sn * x + c * z;
        lo[0] = fminf(lo[0], u); hi[0] = fmaxf(hi[0], u); lo[1] = fminf(lo[1], v); hi[1] = fmaxf(hi[1], v);
    }
    r.w = hi[0] - lo[0]; r.d = hi[1] - lo[1];
    float mu = (lo[0] + hi[0]) * 0.5f, mv = (lo[1] + hi[1]) * 0.5f;
    r.cx = c * mu - sn * mv; r.cz = sn * mu + c * mv;
    return r;
}
// KL-TEST-END closedroom

// A real wall plane as a footprint (tangent = plane X axis, length = w: ARKit frame as kl_spatial.h promises); 0 when it is
// not a usable wall (not a real wall plane, narrower than KLSP_MIN_WALL, or not finite).
static int real_wall_seg(const ent *p, klsp_seg *s) {
    if (p->kind != KLSP_PLANE || is_synth(p->id) || p->label != KLSP_LBL_WALL || !(p->w >= KLSP_MIN_WALL)) return 0;
    float hw = p->w * 0.5f;
    *s = (klsp_seg){p->m[12] - p->m[0] * hw, p->m[14] - p->m[2] * hw, p->m[12] + p->m[0] * hw, p->m[14] + p->m[2] * hw};
    return isfinite(s->x0) && isfinite(s->z0) && isfinite(s->x1) && isfinite(s->z1);
}
// 1 when the real (ARKit) planes alone hold a floor, a ceiling and a valid closed room of walls (klsp_ring_closed): only
// then do they replace the synthetic room.
static int real_set_complete_locked(void) {
    typedef struct { float ang; klsp_seg s; } rw;
    rw w[64];
    int nw = 0, fl = 0, ce = 0;
    float cx = 0, cz = 0;
    for (uint32_t i = 0; i < g_n; i++) {
        const ent *p = &g_e[i];
        if (p->kind != KLSP_PLANE || is_synth(p->id)) continue;
        fl += p->label == KLSP_LBL_FLOOR; ce += p->label == KLSP_LBL_CEILING;
        if (nw >= 64 || !real_wall_seg(p, &w[nw].s)) continue;
        cx += p->m[12]; cz += p->m[14]; nw++;
    }
    if (!fl || !ce || nw < 4) return 0;
    cx /= nw; cz /= nw;
    for (int k = 0; k < nw; k++) {                     // insertion sort by angle around the walls' centroid
        w[k].ang = atan2f((w[k].s.z0 + w[k].s.z1) * 0.5f - cz, (w[k].s.x0 + w[k].s.x1) * 0.5f - cx);
        for (int j = k; j > 0 && w[j].ang < w[j - 1].ang; j--) { rw t = w[j]; w[j] = w[j - 1]; w[j - 1] = t; }
    }
    klsp_seg ring[64];
    for (int k = 0; k < nw; k++) ring[k] = w[k].s;
    return klsp_ring_closed(ring, nw, 0.35f);
}
// Turns every real wall to face inward, like the synthetic ones, before a complete real set replaces them: a wall whose
// normal (plane Y) points out of the room is rolled 180 deg about its local Z (down, kl_spatial.h): tangent and normal
// negated, so start <-> end swap and the walls chain end -> start. Idempotent; the polygon's X follows the roll.
static void real_walls_inward_locked(void) {
    klsp_seg s;
    float cx = 0, cz = 0;
    int n = 0;
    for (uint32_t i = 0; i < g_n; i++) if (real_wall_seg(&g_e[i], &s)) { cx += g_e[i].m[12]; cz += g_e[i].m[14]; n++; }
    if (!n) return;
    cx /= n; cz /= n;
    for (uint32_t i = 0; i < g_n; i++) {
        ent *p = &g_e[i];
        if (!real_wall_seg(p, &s) || !klsp_wall_outward(&s, cx, cz)) continue;
        for (int c = 0; c < 7; c++) if (c != 3) p->m[c] = -p->m[c];                 // columns X and Y
        for (int k = 0; k < p->npoly; k++) p->poly[2 * k] = -p->poly[2 * k];
    }
}

static ent *upsert_locked(int kind, kl_uuid id) {
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELAXED);
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
    g_cap_known = 1;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [spatial] available: planes=%d mesh=%d room=%d anchors=%d\n",
            planes_ok != 0, mesh_ok != 0, room_ok != 0, anchors_ok != 0);
}
int kl_spatial_available_known(void) {
    pthread_mutex_lock(&g_mu);
    int k = g_cap_known;
    pthread_mutex_unlock(&g_mu);
    return k;
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
    static int lg, nfloor;
    static float floor_logged = NAN;
    int log_floor = 0;
    pthread_mutex_lock(&g_mu);
    ent *e = find_locked(id);
    if (removed) { if (e && e->kind == KLSP_PLANE) del_locked(e); pthread_mutex_unlock(&g_mu); return; }
    e = upsert_locked(KLSP_PLANE, id);
    if (e) {
        memcpy(e->m, m4, sizeof e->m);
        e->w = w; e->h = h; e->label = label; e->tracked = 1;
        free(e->poly); e->poly = NULL; e->npoly = 0;
        if (poly2d && npoly >= 3 && npoly <= KLSP_POLY_MAX) {
            e->poly = (float *)malloc((size_t)npoly * 2 * sizeof(float));
            if (e->poly) { memcpy(e->poly, poly2d, (size_t)npoly * 2 * sizeof(float)); e->npoly = npoly; }
        }
        // A complete real set (floor, ceiling, closed walls) replaces the synthetic room, its walls turned inward first;
        // last, as the purge moves entries.
        if (!is_synth(id) && real_set_complete_locked()) { real_walls_inward_locked(); purge_synth_locked(); }
        // [spatial] floor plane: the first real floor and each new floor height (5 cm), 10 lines at most
        if (label == KLSP_LBL_FLOOR && !is_synth(id) && nfloor < 10 && !(fabsf(m4[13] - floor_logged) < 0.05f)) {
            floor_logged = m4[13]; nfloor++; log_floor = 1;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (log_floor)
        fprintf(stderr, "  [spatial] floor plane y=%.3f %.2fx%.2f (ARKit world = head_display_y's frame: ~0 = the visionOS origin is this floor)\n",
                (double)m4[13], (double)w, (double)h);
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
    if (real_set_complete_locked()) purge_synth_locked();   // a complete real room replaces the synthetic one; fragments do not
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
// Planes are handed out FLOOR, CEILING, WALL, the rest, OTHER last (arrival order within each): the room's own order, no other
// effect on the game (HLVR's "Unknown classification" was the label count including the NUL, not the OTHER plane). Handles are given in listing order
// on first sight and never change (kl_spatial_handle_for), so a later reordering renumbers nothing. KL-TEST t_plane_order.
static int plane_rank(int label) {
    return label == KLSP_LBL_FLOOR ? 0 : label == KLSP_LBL_CEILING ? 1 : label == KLSP_LBL_WALL ? 2 : label == KLSP_LBL_OTHER ? 4 : 3;
}
typedef struct { float ang; kl_uuid id; } wall_ang;
static int wall_cmp(const void *a, const void *b) {
    float x = ((const wall_ang *)a)->ang, y = ((const wall_ang *)b)->ang;
    return x < y ? -1 : x > y;
}
typedef struct { kl_uuid id; uint64_t key; } mem_ord_t;
static int mem_cmp(const void *a, const void *b) {
    uint64_t x = ((const mem_ord_t *)a)->key, y = ((const mem_ord_t *)b)->key;
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
    mem_ord_t *mo = (mem_ord_t *)calloc(g_n ? g_n : 1, sizeof(mem_ord_t));
    if (!mo) { free(wa); return 0; }
    int has_synth = 0;
    for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_PLANE && is_synth(g_e[i].id)) has_synth = 1;
    for (uint32_t i = 0; i < g_n; i++) {
        ent *p = &g_e[i];
        if (p->kind != KLSP_PLANE) continue;
        if (!is_synth(p->id) && has_synth && (p->label == KLSP_LBL_FLOOR || p->label == KLSP_LBL_CEILING || p->label == KLSP_LBL_WALL)) continue;
        if (restrict_to_room && !is_synth(p->id)) {
            int in = 0;
            for (uint32_t k = 0; k < room->nmem; k++) if (uuid_eq(room->mem[k], p->id)) { in = 1; break; }
            if (!in) continue;
        }
        mo[cnt].id = p->id; mo[cnt].key = ((uint64_t)plane_rank(p->label) << 32) | (1u + i);   // arrival order within the category
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
    // Container order: the chosen floor and ceiling first, then the rest of their kind, the walls in the room's order, the other planes,
    // OTHER last (plane_rank).
    for (uint32_t c = 0; c < cnt; c++) {
        if (uuid_eq(mo[c].id, fl) || uuid_eq(mo[c].id, ce)) mo[c].key &= ~(uint64_t)0xffffffffu;
        for (uint32_t k = 0; k < nw; k++) if (uuid_eq(mo[c].id, wa[k].id)) mo[c].key = (2ull << 32) | (1u + k);
    }
    qsort(mo, cnt, sizeof(mem_ord_t), mem_cmp);
    if (members) for (uint32_t c = 0; c < cnt && c < mcap; c++) members[c] = mo[c].id;
    free(mo);
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
    if (kind == KLSP_PLANE) {
        for (int r = 0; r < 5; r++)
            for (uint32_t k = 0; k < g_n; k++)
                if (g_e[k].kind == KLSP_PLANE && plane_rank(g_e[k].label) == r && i-- == 0) return &g_e[k];
        return NULL;
    }
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
            float cx = 0, cz = 0, fy = 1e30f; int n = 0, syn = 0;
            for (uint32_t k = 0; k < g_n; k++) syn |= g_e[k].kind == KLSP_PLANE && is_synth(g_e[k].id);
            for (uint32_t k = 0; k < g_n; k++) if (g_e[k].kind == KLSP_PLANE && (!syn || is_synth(g_e[k].id))) {     // one source: the synthetic set when there is one
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

int kl_spatial_is_synthetic(kl_uuid id) { return is_synth(id); }

// ---- footprint helpers: convex hull of what ARKit measured (guardian default) and the polygons of the synthetic room ----
#define KLSP_MAX_WALLS 64        // = what klsp_ring_closed accepts
#define KLSP_ZONE_PTS 256        // points of the user's zone kept (klsp_zone_points)
typedef struct { float x, z; } klsp_pt;
static int klsp_pt_cmp(const void *a, const void *b) {
    const klsp_pt *p = (const klsp_pt *)a, *q = (const klsp_pt *)b;
    return p->x < q->x ? -1 : p->x > q->x ? 1 : p->z < q->z ? -1 : p->z > q->z;
}
static float klsp_cross(const klsp_pt *o, const klsp_pt *a, const klsp_pt *b) {
    return (a->x - o->x) * (b->z - o->z) - (a->z - o->z) * (b->x - o->x);
}
// Convex hull (Andrew's monotone chain) of the n points, sorted in place; out holds 2n + 2. Counter-clockwise in (x, z) with x
// right and z up (positive area, ascending angle: the walls' order) -- 0 when fewer than 3 points leave an area.
static int klsp_hull(klsp_pt *p, int n, klsp_pt *out) {
    if (n < 3) return 0;
    qsort(p, (size_t)n, sizeof *p, klsp_pt_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && klsp_cross(&out[k - 2], &out[k - 1], &p[i]) <= 0) k--;
        out[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && klsp_cross(&out[k - 2], &out[k - 1], &p[i]) <= 0) k--;
        out[k++] = p[i];
    }
    return k - 1 >= 3 ? k - 1 : 0;
}
// Fuses the short edges of the hull h (n vertices): while an edge is under KLSP_MIN_WALL, a vertex lies within 3 cm of the line
// of its neighbours or there are more than KLSP_MAX_WALLS vertices, the vertex that costs least area goes (a subset of a convex
// polygon's vertices is convex). Stops at 3 vertices; returns the new count.
static int klsp_hull_fuse(klsp_pt *h, int n) {
    while (n > 3) {
        int best = -1;
        float bc = 1e30f;
        for (int i = 0; i < n; i++) {
            const klsp_pt *a = &h[(i + n - 1) % n], *b = &h[i], *c = &h[(i + 1) % n];
            float cr = fabsf(klsp_cross(a, b, c));         // twice the area of the triangle a, b, c
            int bad = hypotf(b->x - a->x, b->z - a->z) < KLSP_MIN_WALL || hypotf(c->x - b->x, c->z - b->z) < KLSP_MIN_WALL
                      || cr < 0.03f * hypotf(c->x - a->x, c->z - a->z) || n > KLSP_MAX_WALLS;
            if (bad && cr < bc) { bc = cr; best = i; }
        }
        if (best < 0) break;
        memmove(&h[best], &h[best + 1], (size_t)(n - best - 1) * sizeof *h);
        n--;
    }
    return n;
}
// What the built room was made from: a change in any of these rebuilds it (kl_spatial_synth_room). bb = bounding box of the
// points in 5 cm units (x0, z0, x1, z1); built = the source (KLSP_SRC_*); fy, h = floor and height in 5 cm units; nwall = walls built;
// zhash = hash (mm) of the drawn zone's polygon, 0 for the other sources (a zone redrawn inside the same box is another room).
typedef struct { int nplanes, nmem, nmesh, bb[4], built, fy, h, nwall, zhash; } klsp_sig;
enum { KLSP_SRC_FALLBACK = 0, KLSP_SRC_OUTLINE = 1, KLSP_SRC_GUARDIAN = 2, KLSP_SRC_REAL = 3 };      // the order of the log names below
static klsp_sig g_synth_sig;
static int klsp_sig_significant(const klsp_sig *a, const klsp_sig *b);
// The footprint of the room from measured data only (guest space): corners of the real FLOOR / CEILING / WALL planes -- the
// members of the first RoomAnchor that lists planes, else all of them -- and the vertices of the scene meshes that lie between
// ylo and yhi and within 0.5 m of those planes' box, rounded to 2 cm, as a fused convex hull h (room for 2 * points + 2). Without
// such planes the mesh alone is not used. Returns the vertex count, or 0 when the walls would be fewer than 4, the ring is not
// a valid room (klsp_ring_closed) or the head (hx, hz) lies outside it. *npts = points used; *sig = what they were. Caller
// holds g_mu.
static int built_footprint_locked(float ylo, float yhi, float hx, float hz, klsp_pt *h, int *npts, klsp_sig *sig) {
    const ent *room = NULL;
    size_t cap = 0;
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_e[i].kind == KLSP_ROOM && !room && g_e[i].nmem) room = &g_e[i];
        cap += g_e[i].kind == KLSP_PLANE ? 4 : g_e[i].kind == KLSP_MESH ? g_e[i].nv : 0;
    }
    memset(sig, 0, sizeof *sig);
    *npts = 0;
    klsp_pt *p = (klsp_pt *)malloc((cap ? cap : 1) * sizeof *p);
    if (!p) return 0;
    int n = 0;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    sig->nmem = room ? (int)room->nmem : 0;
    for (uint32_t i = 0; i < g_n; i++) {
        const ent *e = &g_e[i];
        if (e->kind != KLSP_PLANE || is_synth(e->id) || !(e->label == KLSP_LBL_FLOOR || e->label == KLSP_LBL_CEILING || e->label == KLSP_LBL_WALL)) continue;
        if (room) {
            int in = 0;
            for (uint32_t k = 0; k < room->nmem; k++) if (uuid_eq(room->mem[k], e->id)) { in = 1; break; }
            if (!in) continue;
        }
        float g[16];
        to_guest_locked(e->m, g);
        for (int c = 0; c < 4; c++) {                       // the rectangle's corners (a wall's are its two ends, twice)
            float sx = (c & 1 ? 0.5f : -0.5f) * e->w, sz = (c & 2 ? 0.5f : -0.5f) * e->h;
            float x = g[12] + g[0] * sx + g[8] * sz, z = g[14] + g[2] * sx + g[10] * sz;
            if (!isfinite(x) || !isfinite(z)) continue;
            p[n++] = (klsp_pt){roundf(x * 50.0f) / 50.0f, roundf(z * 50.0f) / 50.0f};
            lo[0] = fminf(lo[0], x); hi[0] = fmaxf(hi[0], x); lo[1] = fminf(lo[1], z); hi[1] = fmaxf(hi[1], z);
        }
        sig->nplanes++;
    }
    for (uint32_t i = 0; n > 0 && i < g_n; i++) {
        const ent *e = &g_e[i];
        float g[16];
        if (e->kind != KLSP_MESH) continue;
        to_guest_locked(e->m, g);
        for (uint32_t k = 0; k < e->nv; k++) {
            const float *v = e->v + 3 * (size_t)k;
            float x = g[0] * v[0] + g[4] * v[1] + g[8] * v[2] + g[12];
            float y = g[1] * v[0] + g[5] * v[1] + g[9] * v[2] + g[13];
            float z = g[2] * v[0] + g[6] * v[1] + g[10] * v[2] + g[14];
            if (!isfinite(x) || !isfinite(z) || y < ylo || y > yhi) continue;
            if (x < lo[0] - 0.5f || x > hi[0] + 0.5f || z < lo[1] - 0.5f || z > hi[1] + 0.5f) continue;     // far geometry is not this room
            p[n++] = (klsp_pt){roundf(x * 50.0f) / 50.0f, roundf(z * 50.0f) / 50.0f};
            sig->nmesh++;
        }
    }
    if (n > 0) {
        float bl[2] = {1e30f, 1e30f}, bh[2] = {-1e30f, -1e30f};
        for (int i = 0; i < n; i++) {
            bl[0] = fminf(bl[0], p[i].x); bh[0] = fmaxf(bh[0], p[i].x); bl[1] = fminf(bl[1], p[i].z); bh[1] = fmaxf(bh[1], p[i].z);
        }
        sig->bb[0] = (int)lroundf(bl[0] * 20.0f); sig->bb[1] = (int)lroundf(bl[1] * 20.0f);
        sig->bb[2] = (int)lroundf(bh[0] * 20.0f); sig->bb[3] = (int)lroundf(bh[1] * 20.0f);
    }
    klsp_pt *hull = (klsp_pt *)malloc(((size_t)n * 2 + 2) * sizeof *hull);
    int nh = hull ? klsp_hull(p, n, hull) : 0;
    free(p);
    if (nh) nh = klsp_hull_fuse(hull, nh);
    klsp_seg sg[KLSP_MAX_WALLS];
    if (nh >= 4 && nh <= KLSP_MAX_WALLS) {
        for (int k = 0; k < nh; k++) sg[k] = (klsp_seg){hull[k].x, hull[k].z, hull[(k + 1) % nh].x, hull[(k + 1) % nh].z};
        if (!klsp_ring_closed(sg, nh, 0.35f)) nh = 0;
        klsp_pt head = {hx, hz};
        for (int k = 0; nh && k < nh; k++)                  // the player must stand inside the walls (5 cm slack)
            if (klsp_cross(&hull[k], &hull[(k + 1) % nh], &head) < -0.05f * hypotf(sg[k].x1 - sg[k].x0, sg[k].z1 - sg[k].z0)) nh = 0;
    } else nh = 0;
    if (nh) memcpy(h, hull, (size_t)nh * sizeof *h);
    free(hull);
    *npts = n; sig->built = nh > 0;
    return nh;
}

// 1 when the n hull vertices h (counter-clockwise), as walls from floor fy up H, are a room the Quest accepts (kl_guardian: V1
// counts, wall quads in a closed ring, ceiling above the floor; plus a real outline: finite, area >= 0.01 m2, 1.5-6 m high).
static int klsp_hull_room_valid(const klsp_pt *h, int n, float fy, float H) {
    float q[KLSP_MAX_WALLS * 12];
    if (n < 3 || n > KLSP_MAX_WALLS) return 0;
    for (int k = 0; k < n; k++) {
        const klsp_pt *a = &h[k], *b = &h[(k + 1) % n];
        float *w = q + 12 * k;
        w[0] = a->x; w[1] = fy;     w[2] = a->z;  w[3] = a->x; w[4] = fy + H; w[5] = a->z;
        w[6] = b->x; w[7] = fy + H; w[8] = b->z;  w[9] = b->x; w[10] = fy;    w[11] = b->z;
    }
    return kl_guardian_hull_ok(&h[0].x, n, H) && kl_guardian_room_v1_valid(n, 1, 1, fy, fy + H) && kl_guardian_roombox_valid(q, n, fy, fy + H);
}

// ---- the footprint of the room handed to a game: real walls, else the user's zone (never an invented size) ----
// 1 when polygon p (n >= 3) has two non-adjacent edges that cross.
static int klsp_self_cross(const klsp_pt *p, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 2; j < n; j++) {
            if (i == 0 && j == n - 1) continue;                                   // neighbours through the closing edge
            const klsp_pt *a = &p[i], *b = &p[(i + 1) % n], *c = &p[j], *d = &p[(j + 1) % n];
            if (klsp_cross(a, b, c) * klsp_cross(a, b, d) < 0 && klsp_cross(c, d, a) * klsp_cross(c, d, b) < 0) return 1;
        }
    return 0;
}
static float klsp_area2(const klsp_pt *p, int n) {          // twice the signed area: > 0 = counter-clockwise in (x, z)
    float a = 0;
    for (int k = 0; k < n; k++) a += p[k].x * p[(k + 1) % n].z - p[(k + 1) % n].x * p[k].z;
    return a;
}
// 1 when (x, z) lies in polygon p (n vertices) or within 5 cm of its outline.
static int klsp_inside(const klsp_pt *p, int n, float x, float z) {
    int in = 0;
    for (int k = 0; k < n; k++) {
        const klsp_pt *a = &p[k], *b = &p[(k + 1) % n];
        klsp_seg s = {a->x, a->z, b->x, b->z};
        if (klsp_pt_seg(x, z, &s) <= 0.05f) return 1;
        if ((a->z > z) != (b->z > z) && x < a->x + (z - a->z) * (b->x - a->x) / (b->z - a->z)) in ^= 1;
    }
    return in;
}
// Makes p (n vertices, any simple polygon -- convex or not) a room footprint: counter-clockwise, short / collinear / surplus
// vertices fused (klsp_hull_fuse), at least 3 walls (V1 accepts 3), no crossing edges, area >= KLSP_MIN_AREA. Returns the new
// vertex count, 0 when it is not one.
static int klsp_poly_prepare(klsp_pt *p, int n) {
    if (n < 3) return 0;
    for (int k = 0; k < n; k++) if (!isfinite(p[k].x) || !isfinite(p[k].z)) return 0;
    if (klsp_area2(p, n) < 0)
        for (int a = 0, b = n - 1; a < b; a++, b--) { klsp_pt t = p[a]; p[a] = p[b]; p[b] = t; }
    n = klsp_hull_fuse(p, n);
    if (n < 3 || n > KLSP_MAX_WALLS || klsp_self_cross(p, n) || !(klsp_area2(p, n) * 0.5f >= KLSP_MIN_AREA)) return 0;
    return n;
}
// The player's drawn zone as a room footprint. klsp_hull_fuse assumes a convex polygon, so a hand-drawn L would lose its reflex corners:
// here only what cannot be a wall goes -- duplicate points (under 1 cm), points on the straight line of their neighbours (3 cm), and
// edges under KLSP_MIN_WALL, whose two ends become their midpoint (the shape and its concavity stay) -- then, over KLSP_MAX_WALLS vertices,
// the shortest edges the same way. Counter-clockwise on return. A zone that still crosses itself, has under 3 vertices or under
// KLSP_MIN_AREA is refused and logged ("[room] drawn zone rejected: reason=..."). Returns the vertex count, 0 when refused.
static int klsp_zone_prepare(klsp_pt *p, int n) {
    const char *why = NULL;
    for (int k = 0; k < n; k++) if (!isfinite(p[k].x) || !isfinite(p[k].z)) { why = "not-finite"; break; }
    if (!why && n >= 3 && klsp_area2(p, n) < 0)
        for (int a = 0, b = n - 1; a < b; a++, b--) { klsp_pt t = p[a]; p[a] = p[b]; p[b] = t; }
    for (int guard = 0; !why && n > 3 && guard < 4 * KLSP_ZONE_PTS; guard++) {
        int drop = -1, shortest = -1;
        float sl = 1e30f;
        for (int i = 0; i < n && drop < 0; i++) {
            const klsp_pt *a = &p[(i + n - 1) % n], *b = &p[i], *c = &p[(i + 1) % n];
            float ab = hypotf(b->x - a->x, b->z - a->z), ac = hypotf(c->x - a->x, c->z - a->z);
            if (ab < 0.01f || ac < 0.01f || fabsf(klsp_cross(a, b, c)) < 0.03f * ac) drop = i;   // duplicate / spike / straight through
        }
        if (drop >= 0) { memmove(&p[drop], &p[drop + 1], (size_t)(n - drop - 1) * sizeof *p); n--; continue; }
        for (int i = 0; i < n; i++) {
            float l = hypotf(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].z - p[i].z);
            if (l < sl) { sl = l; shortest = i; }
        }
        if (sl >= KLSP_MIN_WALL && n <= KLSP_MAX_WALLS) break;
        int j = (shortest + 1) % n;                                                       // merge the shortest edge into its midpoint
        p[shortest] = (klsp_pt){(p[shortest].x + p[j].x) * 0.5f, (p[shortest].z + p[j].z) * 0.5f};
        memmove(&p[j], &p[j + 1], (size_t)(n - j - 1) * sizeof *p);
        n--;
    }
    if (!why && n < 3) why = "too-few-vertices";
    if (!why && klsp_self_cross(p, n)) why = "self-crossing";
    if (!why && !(klsp_area2(p, n) * 0.5f >= KLSP_MIN_AREA)) why = "area";
    if (why) { fprintf(stderr, "  [room] drawn zone rejected: reason=%s\n", why); return 0; }
    return n;
}
// Intersection of the lines of walls a and b, when they are not nearly parallel (under 6 degrees).
static int klsp_line_isect(const klsp_seg *a, const klsp_seg *b, float *x, float *z) {
    float ax = a->x1 - a->x0, az = a->z1 - a->z0, bx = b->x1 - b->x0, bz = b->z1 - b->z0, den = ax * bz - az * bx;
    if (!(fabsf(den) > 0.1f * hypotf(ax, az) * hypotf(bx, bz))) return 0;
    float t = ((b->x0 - a->x0) * bz - (b->z0 - a->z0) * bx) / den;
    *x = a->x0 + ax * t; *z = a->z0 + az * t;
    return 1;
}

// ---- the outline from the real walls: what a wall list holds besides the room's own walls ----
#define KLSP_OUT_MAX 128         // real walls looked at; more are dropped (logged)
static float klsp_seg_len(const klsp_seg *s) { return hypotf(s->x1 - s->x0, s->z1 - s->z0); }
// Distance between two wall segments (0 when they cross).
static float klsp_seg_dist(const klsp_seg *a, const klsp_seg *b) {
    klsp_pt p0 = {a->x0, a->z0}, p1 = {a->x1, a->z1}, q0 = {b->x0, b->z0}, q1 = {b->x1, b->z1};
    if (klsp_cross(&p0, &p1, &q0) * klsp_cross(&p0, &p1, &q1) < 0 && klsp_cross(&q0, &q1, &p0) * klsp_cross(&q0, &q1, &p1) < 0) return 0.0f;
    return fminf(fminf(klsp_pt_seg(a->x0, a->z0, b), klsp_pt_seg(a->x1, a->z1, b)),
                 fminf(klsp_pt_seg(b->x0, b->z0, a), klsp_pt_seg(b->x1, b->z1, a)));
}
// 1 when wall b is the same wall as a (a duplicate, or a piece of it): lines within 15 cm and 8 degrees, overlapping or at most
// 35 cm apart along it. Then a becomes the one segment that spans both (same direction).
static int klsp_wall_merge(klsp_seg *a, const klsp_seg *b) {
    float ax = a->x1 - a->x0, az = a->z1 - a->z0, la = hypotf(ax, az), bx = b->x1 - b->x0, bz = b->z1 - b->z0, lb = hypotf(bx, bz);
    if (!(fabsf(ax * bz - az * bx) <= 0.14f * la * lb)) return 0;
    float ux = ax / la, uz = az / la;
    if (fabsf(-uz * (b->x0 - a->x0) + ux * (b->z0 - a->z0)) > 0.15f || fabsf(-uz * (b->x1 - a->x0) + ux * (b->z1 - a->z0)) > 0.15f) return 0;
    float s0 = ux * (b->x0 - a->x0) + uz * (b->z0 - a->z0), s1 = ux * (b->x1 - a->x0) + uz * (b->z1 - a->z0);
    float lo = fminf(fminf(s0, s1), 0.0f), hi = fmaxf(fmaxf(s0, s1), la);
    if (fminf(s0, s1) > la + 0.35f || fmaxf(s0, s1) < -0.35f) return 0;
    *a = (klsp_seg){a->x0 + ux * lo, a->z0 + uz * lo, a->x0 + ux * hi, a->z0 + uz * hi};
    return 1;
}
// 1 when wall i of the n is on the outside of the set: a ray from beside its middle, along one of its two normals, meets no other wall.
// A partition has other walls on both sides of it; the walls of a room, one side open to the outside (or to its missing side).
static int klsp_wall_outer(const klsp_seg *s, int n, int i) {
    float l = klsp_seg_len(&s[i]), mx = (s[i].x0 + s[i].x1) * 0.5f, mz = (s[i].z0 + s[i].z1) * 0.5f;
    float nx = -(s[i].z1 - s[i].z0) / l, nz = (s[i].x1 - s[i].x0) / l;
    for (int sg = -1; sg <= 1; sg += 2) {
        float dx = (float)sg * nx, dz = (float)sg * nz, ox = mx + dx * 0.05f, oz = mz + dz * 0.05f;
        int hit = 0;
        for (int k = 0; k < n && !hit; k++) {
            if (k == i) continue;
            float ex = s[k].x1 - s[k].x0, ez = s[k].z1 - s[k].z0, den = dx * ez - dz * ex;
            if (!(fabsf(den) > 1e-6f)) continue;
            float px = s[k].x0 - ox, pz = s[k].z0 - oz, t = (px * ez - pz * ex) / den, u = (px * dz - pz * dx) / den;
            hit = t > 0 && u >= 0 && u <= 1;
        }
        if (!hit) return 1;
    }
    return 0;
}
// Distance from (x, z) to the outline of polygon p (n vertices).
static float klsp_edge_dist(const klsp_pt *p, int n, float x, float z) {
    float d = 1e30f;
    for (int k = 0; k < n; k++) {
        klsp_seg e = {p[k].x, p[k].z, p[(k + 1) % n].x, p[(k + 1) % n].z};
        d = fminf(d, klsp_pt_seg(x, z, &e));
    }
    return d;
}
// The ring of the n (>= 3) outer walls w: chained end to start from the longest, always to the nearest free end; where two
// neighbours meet (ends within 0.35 m, lines crossing there) the corner is where their lines cross, otherwise the ends are joined
// by a straight closure -- what a missing side becomes. Then it must be a real outline: every edge of the polygon covers at least
// half a real wall (sum of the overlaps along it) or is a closure between two wall ends, closures are at most half of the perimeter
// and no wall lies inside the polygon (midpoint inside, over 0.3 m from the border). Returns the vertex count, 0 when not.
static int klsp_wall_ring(const klsp_seg *w, int n, klsp_pt *out) {
    klsp_seg a[KLSP_OUT_MAX];
    int used[KLSP_OUT_MAX] = {0}, first = 0, m = 0;
    for (int k = 1; k < n; k++) if (klsp_seg_len(&w[k]) > klsp_seg_len(&w[first])) first = k;
    a[0] = w[first]; used[first] = 1;
    for (int c = 1; c < n; c++) {                               // the free wall with an end nearest to the tail of the chain
        int bk = -1, bf = 0;
        float bd = 1e30f;
        for (int k = 0; k < n; k++) {
            if (used[k]) continue;
            for (int f = 0; f < 2; f++) {
                float d = hypotf((f ? w[k].x1 : w[k].x0) - a[c - 1].x1, (f ? w[k].z1 : w[k].z0) - a[c - 1].z1);
                if (d < bd) { bd = d; bk = k; bf = f; }
            }
        }
        used[bk] = 1;
        a[c] = bf ? (klsp_seg){w[bk].x1, w[bk].z1, w[bk].x0, w[bk].z0} : w[bk];
    }
    for (int c = 0; c < n; c++) {
        const klsp_seg *p = &a[c], *q = &a[(c + 1) % n];
        float ix, iz, gap = hypotf(q->x0 - p->x1, q->z0 - p->z1);
        if (gap <= 0.35f && klsp_line_isect(p, q, &ix, &iz) && hypotf(ix - p->x1, iz - p->z1) <= 0.35f && hypotf(ix - q->x0, iz - q->z0) <= 0.35f)
            out[m++] = (klsp_pt){ix, iz};
        else if (gap <= KLSP_MIN_WALL) out[m++] = (klsp_pt){(p->x1 + q->x0) * 0.5f, (p->z1 + q->z0) * 0.5f};
        else { out[m++] = (klsp_pt){p->x1, p->z1}; out[m++] = (klsp_pt){q->x0, q->z0}; }
    }
    m = klsp_poly_prepare(out, m);
    if (!m) return 0;
    float perim = 0, closure = 0;
    for (int k = 0; k < m; k++) {
        const klsp_pt *e0 = &out[k], *e1 = &out[(k + 1) % m];
        float el = hypotf(e1->x - e0->x, e1->z - e0->z), ux = (e1->x - e0->x) / el, uz = (e1->z - e0->z) / el, cover = 0;
        perim += el;
        for (int j = 0; j < n; j++) {                           // how much of this edge the real walls lie along
            float wl = klsp_seg_len(&w[j]), cr = ux * (w[j].z1 - w[j].z0) - uz * (w[j].x1 - w[j].x0);
            if (!(fabsf(cr) <= 0.26f * wl)) continue;           // not within 15 degrees of the edge
            if (fabsf(-uz * (w[j].x0 - e0->x) + ux * (w[j].z0 - e0->z)) > 0.35f || fabsf(-uz * (w[j].x1 - e0->x) + ux * (w[j].z1 - e0->z)) > 0.35f) continue;
            float s0 = ux * (w[j].x0 - e0->x) + uz * (w[j].z0 - e0->z), s1 = ux * (w[j].x1 - e0->x) + uz * (w[j].z1 - e0->z);
            float ov = fminf(fmaxf(s0, s1), el) - fmaxf(fminf(s0, s1), 0.0f);
            if (ov > 0) cover += ov;
        }
        if (cover >= 0.5f * el) continue;
        int near0 = 0, near1 = 0;                               // not a wall: it must be a closure between two wall ends
        for (int j = 0; j < n; j++)
            for (int f = 0; f < 2; f++) {
                float x = f ? w[j].x1 : w[j].x0, z = f ? w[j].z1 : w[j].z0;
                near0 |= hypotf(x - e0->x, z - e0->z) <= 0.35f; near1 |= hypotf(x - e1->x, z - e1->z) <= 0.35f;
            }
        if (!near0 || !near1) return 0;
        closure += el;
    }
    if (closure > 0.5f * perim) return 0;
    for (int j = 0; j < n; j++) {
        float mx = (w[j].x0 + w[j].x1) * 0.5f, mz = (w[j].z0 + w[j].z1) * 0.5f;
        if (klsp_inside(out, m, mx, mz) && klsp_edge_dist(out, m, mx, mz) > 0.3f) return 0;     // a partition inside the ring
    }
    return m;
}
// The outline of the room from its real WALL planes only (guest space): the walls of the first RoomAnchor that lists planes, else
// every real wall, each at least KLSP_MIN_WALL wide, at their measured extent. Duplicated / overlapping pieces of one wall are merged;
// walls are grouped by touching (within 0.35 m) and the groups tried from the biggest: those with a wall of another group (another
// room) in them never mix. In a group only the outer walls count (klsp_wall_outer -- partitions and furniture edges left out) and
// they make the ring of klsp_wall_ring. No hull, no margin: an L stays an L. With head_check the player (hx, hz) must stand inside the
// walls (the first build only: a live rebuild keeps what the game has while he walks about). Returns the vertex count, 0 when
// there is no outline (fewer than 3 walls, or the ring does not validate: the next source). out holds KLSP_ZONE_PTS. Caller holds g_mu.
static int outline_from_walls_locked(klsp_pt *out, float hx, float hz, int head_check) {
    klsp_seg s[KLSP_OUT_MAX];
    const ent *room = NULL;
    int n = 0, dropped = 0;
    for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_ROOM && g_e[i].nmem) { room = &g_e[i]; break; }
    for (uint32_t i = 0; i < g_n; i++) {
        const ent *e = &g_e[i];
        if (e->kind != KLSP_PLANE || is_synth(e->id) || e->label != KLSP_LBL_WALL || !(e->w >= KLSP_MIN_WALL)) continue;
        if (room) {
            int in = 0;
            for (uint32_t k = 0; k < room->nmem && !in; k++) in = uuid_eq(room->mem[k], e->id);
            if (!in) continue;
        }
        float g[16], hw = e->w * 0.5f;
        to_guest_locked(e->m, g);
        klsp_seg q = {g[12] - g[0] * hw, g[14] - g[2] * hw, g[12] + g[0] * hw, g[14] + g[2] * hw};
        if (!(isfinite(q.x0) && isfinite(q.z0) && isfinite(q.x1) && isfinite(q.z1))) continue;
        if (n < KLSP_OUT_MAX) s[n++] = q; else dropped++;
    }
    if (dropped) fprintf(stderr, "  [room] outline: %d real walls, only the first %d used\n", n + dropped, n);
    for (int i = 0; i < n; i++)                                 // duplicated / overlapping pieces of one wall -> one
        for (int j = i + 1; j < n; ) {
            if (klsp_wall_merge(&s[i], &s[j])) { s[j] = s[--n]; j = i + 1; } else j++;
        }
    if (n < 3) return 0;
    int grp[KLSP_OUT_MAX], done[KLSP_OUT_MAX] = {0};
    for (int i = 0; i < n; i++) grp[i] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (klsp_seg_dist(&s[i], &s[j]) <= 0.35f) {
                int a = grp[i], b = grp[j];
                while (grp[a] != a) a = grp[a];
                while (grp[b] != b) b = grp[b];
                grp[b] = a;
            }
    for (int i = 0; i < n; i++) { int a = i; while (grp[a] != a) a = grp[a]; grp[i] = a; }
    for (;;) {                                                  // the biggest group not tried yet
        int best = -1, bc = 2;
        for (int g = 0; g < n; g++) {
            int c = 0;
            if (grp[g] != g || done[g]) continue;
            for (int i = 0; i < n; i++) c += grp[i] == g;
            if (c > bc) { bc = c; best = g; }
        }
        if (best < 0) return 0;
        done[best] = 1;
        klsp_seg all[KLSP_OUT_MAX], w[KLSP_OUT_MAX];
        int na = 0, nw = 0;
        for (int i = 0; i < n; i++) if (grp[i] == best) all[na++] = s[i];
        for (int i = 0; i < na; i++) if (klsp_wall_outer(all, na, i)) w[nw++] = all[i];
        if (nw < 3) continue;
        int m = klsp_wall_ring(w, nw, out);
        if (m && (!head_check || klsp_inside(out, m, hx, hz))) return m;
    }
}
// The user's confirmed zone (kl_guardian: the user's roomscale boundary, guest space) as at most KLSP_ZONE_PTS points, thinned
// evenly when it has more. 0 when there is none (nothing set, the default taken from the room, or a stationary circle).
static int klsp_zone_points(klsp_pt *z) {
    if (kl_guardian_boundary_source() != KLG_BSRC_USER || kl_guardian_get_mode() != KLG_MODE_ROOMSCALE) return 0;
    int n = kl_guardian_get_boundary(NULL, 0);
    if (n < 3) return 0;
    float *xz = (float *)malloc((size_t)n * 2 * sizeof *xz);
    if (!xz) return 0;
    n = kl_guardian_get_boundary(xz, n);
    int step = (n + KLSP_ZONE_PTS - 1) / KLSP_ZONE_PTS, m = 0;
    for (int k = 0; k < n && m < KLSP_ZONE_PTS; k += step) z[m++] = (klsp_pt){xz[2 * k], xz[2 * k + 1]};
    free(xz);
    return m;
}
// Height of the room above its floor fy: the real ceiling plane (cey, guest y) when it is 1.5-6 m up; else the highest scene-mesh
// point inside the footprint p (n vertices); else 2.4 m, the one invented figure left. *src = 1 ceiling, 2 mesh, 0 default. Caller
// holds g_mu.
static float klsp_room_height_locked(float fy, float cey, const klsp_pt *p, int n, int *src) {
    *src = 0;
    if (cey > -1e29f && cey - fy >= 1.5f && cey - fy <= 6.0f) { *src = 1; return cey - fy; }
    float top = -1e30f;
    for (uint32_t i = 0; i < g_n; i++) {
        const ent *e = &g_e[i];
        float g[16];
        if (e->kind != KLSP_MESH) continue;
        to_guest_locked(e->m, g);
        for (uint32_t k = 0; k < e->nv; k++) {
            const float *v = e->v + 3 * (size_t)k;
            float y = g[1] * v[0] + g[5] * v[1] + g[9] * v[2] + g[13];
            if (!(y > fy + 1.0f && y <= fy + 6.0f && y > top)) continue;       // below 1 m is furniture, above 6 m is not this room
            float x = g[0] * v[0] + g[4] * v[1] + g[8] * v[2] + g[12], z = g[2] * v[0] + g[6] * v[1] + g[10] * v[2] + g[14];
            if (isfinite(x) && isfinite(z) && klsp_inside(p, n, x, z)) top = y;
        }
    }
    if (top - fy >= 1.5f) { *src = 2; return top - fy; }
    return 2.4f;
}

// ---- the room handed to a game when the real planes are not a complete room (Space Setup must still end with a closed one) ----
// Source, first that applies (logged as "[room] source=..."): real (a complete real set: nothing is added, returns 0) ->
// guardian (the zone the player DREW with the controller: kl_guardian, user source, roomscale, >= 3 vertices; one wall per edge, edges
// under 0.3 m fused, no test of where his head is) -> outline (outline_from_walls_locked: the measured walls, a missing side closed
// by a straight wall) -> fallback (no drawn zone, no usable wall: the old box, zw x zd + 0.5 m per side around the head (hx, hz),
// turned and grown to hold partial walls by klsp_fit_rect; 2.5 m high). A stationary circle is never a room. The first two have no
// margin and no box: the polygon is floor and ceiling (non-convex allowed), one wall per edge, ceiling at the real ceiling or the
// mesh (klsp_room_height_locked). The floor is the guardian's (the user's confirmed one) when there is one, else the real floor plane.
// Planes use the ARKit frame (normal +Y; walls: local Z points down so Meta +Y is up, see kl_spatial.h).
// Returns 1 when a synthetic room is present now, 0 when a complete real set exists (it wins, nothing is added).
// keep != NULL (live update): a synthetic room already built from `keep` stays as it is while the new input does not differ
// significantly from it -- no silent change of geometry for jitter or a few mesh points.
static int g_src_logged = -1;                                          // the source of the last "[room] source=" line
static int synth_room_impl(float hx, float hz, float zw, float zd, klsp_sig *out, const klsp_sig *keep) {
    static const char *const k_src[] = {"fallback", "outline", "guardian", "real"}, *const k_hsrc[] = {"default", "ceiling", "mesh"};
    const float M = 0.5f, oy = kl_ovrp_guest_origin_y();
    memset(out, 0, sizeof *out);
    float H = 2.5f, gfy = 0.0f, rfy = 1e30f, cey = -1e30f;
    int gsrc = kl_guardian_get_floor(&gfy);            // the guardian's floor (visionOS world) when there is one
    float fy = gfy - oy;
    klsp_pt zone[KLSP_ZONE_PTS], poly[KLSP_ZONE_PTS];
    int nz = klsp_zone_points(zone);                   // before g_mu: the guardian has its own lock
    klsp_seg ws[64];
    int nws = 0;
    pthread_mutex_lock(&g_mu);
    const ent *room = NULL;
    int synth = 0, have_room = 0, nplanes = 0;
    for (uint32_t i = 0; i < g_n; i++) if (g_e[i].kind == KLSP_ROOM && g_e[i].nmem) { room = &g_e[i]; break; }
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_e[i].kind == KLSP_ROOM) have_room = 1;
        if (g_e[i].kind != KLSP_PLANE) continue;
        if (is_synth(g_e[i].id)) { synth++; continue; }
        float g[16], hw = g_e[i].w * 0.5f;
        to_guest_locked(g_e[i].m, g);
        if (g_e[i].label == KLSP_LBL_FLOOR && g[13] < rfy) rfy = g[13];
        if (g_e[i].label == KLSP_LBL_CEILING && g[13] > cey) cey = g[13];
        if (g_e[i].label == KLSP_LBL_WALL && nws < 64) ws[nws++] = (klsp_seg){g[12] - g[0] * hw, g[14] - g[2] * hw, g[12] + g[0] * hw, g[14] + g[2] * hw};
        if (g_e[i].label == KLSP_LBL_FLOOR || g_e[i].label == KLSP_LBL_CEILING || g_e[i].label == KLSP_LBL_WALL) {
            int in = !room;
            for (uint32_t k = 0; room && k < room->nmem && !in; k++) in = uuid_eq(room->mem[k], g_e[i].id);
            nplanes += in;
        }
    }
    if (real_set_complete_locked()) {
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < nws; k++) {
            lo[0] = fminf(lo[0], fminf(ws[k].x0, ws[k].x1)); hi[0] = fmaxf(hi[0], fmaxf(ws[k].x0, ws[k].x1));
            lo[1] = fminf(lo[1], fminf(ws[k].z0, ws[k].z1)); hi[1] = fmaxf(hi[1], fmaxf(ws[k].z0, ws[k].z1));
        }
        int first = g_src_logged != KLSP_SRC_REAL;
        g_src_logged = KLSP_SRC_REAL;
        pthread_mutex_unlock(&g_mu);
        if (first) fprintf(stderr, "  [room] source=real dims=%.2fx%.2f h=%.2f nwalls=%d\n", hi[0] - lo[0], hi[1] - lo[1], cey > -1e29f ? cey - (rfy < 1e29f ? rfy : fy) : 0.0f, nws);
        return 0;
    }
    if (rfy < 1e29f && !gsrc) fy = rfy;
    klsp_rect r;
    klsp_seg sg[KLSP_MAX_WALLS];
    klsp_sig sig;
    memset(&sig, 0, sizeof sig);
    sig.nplanes = nplanes; sig.nmem = room ? (int)room->nmem : 0;
    int np = 0, src = KLSP_SRC_FALLBACK, hsrc = 0, nwall = 4;
    float hb = H;
    // The player must stand inside the outline when it is first chosen; a live rebuild of an outline the game already has does not ask
    // again (he walks about), whereas anything else being rebuilt live starts from the first-build rule.
    int head_check = !(keep && keep->built == KLSP_SRC_OUTLINE);
    static const int k_order[2] = {KLSP_SRC_GUARDIAN, KLSP_SRC_OUTLINE};
    for (int i = 0; i < 2 && !np; i++) {
        int s = k_order[i], n = 0;
        if (s == KLSP_SRC_OUTLINE) {
            n = outline_from_walls_locked(poly, hx, hz, head_check);
        } else if (nz >= 3) {                                          // the player's drawn zone: no head test, it is his
            memcpy(poly, zone, (size_t)nz * sizeof *poly);
            n = klsp_zone_prepare(poly, nz);
        }
        if (n) {
            hb = klsp_room_height_locked(fy, cey, poly, n, &hsrc);
            if (!klsp_hull_room_valid(poly, n, fy, hb)) n = 0;         // not a V1 room: the next source
        }
        if (n) { np = n; src = s; }
    }
    if (np) {
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < np; k++) {
            sg[k] = (klsp_seg){poly[k].x, poly[k].z, poly[(k + 1) % np].x, poly[(k + 1) % np].z};
            lo[0] = fminf(lo[0], poly[k].x); hi[0] = fmaxf(hi[0], poly[k].x); lo[1] = fminf(lo[1], poly[k].z); hi[1] = fmaxf(hi[1], poly[k].z);
        }
        nwall = np; H = hb;
        r = (klsp_rect){(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, hi[0] - lo[0], hi[1] - lo[1], 0.0f};
        sig.bb[0] = (int)lroundf(lo[0] * 20.0f); sig.bb[1] = (int)lroundf(lo[1] * 20.0f);
        sig.bb[2] = (int)lroundf(hi[0] * 20.0f); sig.bb[3] = (int)lroundf(hi[1] * 20.0f);
        sig.h = (int)lroundf(H * 20.0f);
    } else {
        r = klsp_fit_rect(ws, nws, hx, hz, zw + 2 * M, zd + 2 * M);
        klsp_box_walls(&r, sg);
    }
    sig.built = src; sig.fy = (int)lroundf(fy * 20.0f); sig.nwall = nwall;
    if (src == KLSP_SRC_GUARDIAN) {                                    // exact: the player drew it, no ARKit jitter in it
        uint32_t h = 2166136261u;
        for (int k = 0; k < np; k++) {
            h = (h ^ (uint32_t)lroundf(poly[k].x * 1000.0f)) * 16777619u;
            h = (h ^ (uint32_t)lroundf(poly[k].z * 1000.0f)) * 16777619u;
        }
        sig.zhash = (int)h;
    }
    if (synth && synth == (int)g_synth_n && (!memcmp(&sig, &g_synth_sig, sizeof sig) || (keep && !klsp_sig_significant(&sig, keep)))) { *out = sig; pthread_mutex_unlock(&g_mu); return 1; }   // same input: keep it
    purge_synth_locked();                              // new or changed data: rebuild (same ids, one source at a time, under this lock)
    float c = cosf(r.yaw), sn = sinf(r.yaw);
    for (int k = 0; k < 2 + nwall; k++) {
        float g[16] = {0};
        g[15] = 1;
        int label; float w = r.w, h = H;
        if (k < 2) {                                   // 0 floor (normal up), 1 ceiling (normal down)
            label = k ? KLSP_LBL_CEILING : KLSP_LBL_FLOOR; h = r.d;
            g[0] = c; g[2] = sn; g[5] = k ? -1.0f : 1.0f; g[8] = k ? sn : -sn; g[10] = k ? -c : c;
            g[12] = r.cx; g[13] = fy + (k ? H : 0.0f); g[14] = r.cz;
        } else {
            label = KLSP_LBL_WALL;
            const klsp_seg *q = &sg[k - 2];
            float dx = q->x1 - q->x0, dz = q->z1 - q->z0;
            w = hypotf(dx, dz);
            g[0] = dx / w; g[2] = dz / w;              // X = tangent
            g[4] = -dz / w; g[6] = dx / w;             // Y = inward normal (left of the tangent, as the walls run in ascending angle)
            g[9] = -1.0f;                              // Z = down
            g[12] = (q->x0 + q->x1) * 0.5f; g[13] = fy + H * 0.5f; g[14] = (q->z0 + q->z1) * 0.5f;
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
        float *pl = (np && k < 2) ? (float *)malloc((size_t)np * 2 * sizeof(float)) : NULL;
        if (pl) {                                      // floor / ceiling = the polygon, counter-clockwise in plane XY (Meta y = -local Z)
            for (int j = 0; j < np; j++) {
                const klsp_pt *q = &poly[k ? j : np - 1 - j];
                pl[2 * j] = q->x - r.cx; pl[2 * j + 1] = (k ? 1.0f : -1.0f) * (q->z - r.cz);
            }
            free(e->poly); e->poly = pl; e->npoly = np;
        }
    }
    g_synth_n = (uint32_t)(2 + nwall); g_synth_sig = sig; *out = sig;
    g_src_logged = src;
    pthread_mutex_unlock(&g_mu);
    if (src == KLSP_SRC_OUTLINE) kl_guardian_default_from_room(&poly[0].x, np);     // the outline is the default play space (a user one wins)
    fprintf(stderr, "  [spatial] synthetic room %.2fx%.2f turn=%.2f centre=%.2f,%.2f floor_y=%.2f partial_walls=%d\n",
            r.w, r.d, r.yaw, r.cx, r.cz, fy, nws);
    fprintf(stderr, "  [room] source=%s dims=%.2fx%.2f h=%.2f nwalls=%d hsrc=%s floor_y=%.2f src=%s\n", k_src[src], r.w, r.d, H, nwall,
            np ? k_hsrc[hsrc] : "fixed", fy, src == KLSP_SRC_GUARDIAN ? "zone" : np ? (have_room ? "roomanchor" : "planes") : "none");
    return 1;
}

// What the game was last handed (kl_spatial_synth_room), and what the live update compares against: a real complete set (synth 0) or a
// synthetic room built from `sig`. Guarded by g_mu.
static struct { int valid, synth; klsp_sig sig; } g_given;
int kl_spatial_synth_room(float hx, float hz, float zw, float zd) {
    klsp_sig sig;
    int r = synth_room_impl(hx, hz, zw, zd, &sig, NULL);
    pthread_mutex_lock(&g_mu);
    g_given.valid = 1; g_given.synth = r; g_given.sig = sig;
    pthread_mutex_unlock(&g_mu);
    return r;
}

// ---- live room: the scene keeps being captured after the game has its room ----
// A change that matters (the set of walls / floor / ceiling, the footprint by >= 10 cm, the scene-mesh points inside it by > 25 %) is
// what a new room is; a table, ARKit jitter or a few mesh triangles are not. The user's floor is only read (synth_room_impl), never set.
static int klsp_sig_significant(const klsp_sig *a, const klsp_sig *b) {
    if (a->built != b->built || a->nplanes != b->nplanes || a->nmem != b->nmem) return 1;
    for (int i = 0; i < 4; i++) if (abs(a->bb[i] - b->bb[i]) >= 2) return 1;          // bb is in 5 cm units
    if (abs(a->fy - b->fy) >= 2 || abs(a->h - b->h) >= 2) return 1;
    if (a->nwall != b->nwall || a->zhash != b->zhash) return 1;                   // the floor or the height moved by >= 10 cm
    int dm = abs(a->nmesh - b->nmesh), top = a->nmesh > b->nmesh ? a->nmesh : b->nmesh;
    return dm > 64 && dm * 4 > top;
}
static void (*g_room_cb)(void);
static uint32_t g_room_gen, g_live_gen, g_live_ggen;
static int g_live_gen_valid;
static uint64_t g_live_ns;
void kl_spatial_set_room_listener(void (*fn)(void)) {
    pthread_mutex_lock(&g_mu);
    g_room_cb = fn;
    pthread_mutex_unlock(&g_mu);
}
uint32_t kl_spatial_room_generation(void) { return __atomic_load_n(&g_room_gen, __ATOMIC_RELAXED); }
uint32_t kl_spatial_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }
int kl_spatial_live_update_at(float hx, float hz, float zw, float zd, uint64_t now_ns) {
    if (!isfinite(hx) || !isfinite(hz)) return 0;
    uint32_t gen = kl_spatial_generation(), ggen = kl_guardian_generation();    // the store or the guardian (a zone redrawn) changed
    pthread_mutex_lock(&g_mu);
    int run = g_given.valid && !(g_live_gen_valid && gen == g_live_gen && ggen == g_live_ggen) && (!g_live_ns || now_ns >= g_live_ns + 2000000000ull);
    if (run) g_live_ns = now_ns;
    pthread_mutex_unlock(&g_mu);
    if (!run) return 0;
    klsp_sig given, sig;
    pthread_mutex_lock(&g_mu);
    given = g_given.sig;
    pthread_mutex_unlock(&g_mu);
    int r = synth_room_impl(hx, hz, zw, zd, &sig, &given);
    uint32_t after = kl_spatial_generation();
    void (*cb)(void) = NULL;
    pthread_mutex_lock(&g_mu);
    g_live_gen = after; g_live_ggen = ggen; g_live_gen_valid = 1;
    int changed = r != g_given.synth || (r && klsp_sig_significant(&sig, &g_given.sig));
    if (changed) {
        g_given.synth = r; g_given.sig = sig;
        __atomic_add_fetch(&g_room_gen, 1, __ATOMIC_RELAXED);
        cb = g_room_cb;
    }
    pthread_mutex_unlock(&g_mu);
    if (changed) fprintf(stderr, "  [spatial] room changed (%s, planes=%d h=%.2f): rebuilt, game notified\n", r ? "synthetic" : "real set", sig.nplanes, (double)sig.h * 0.05);
    if (cb) cb();                                       // outside g_mu: the listener may query the store
    return changed;
}
int kl_spatial_live_update(float hx, float hz, float zw, float zd) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return kl_spatial_live_update_at(hx, hz, zw, zd, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

// The guardian's default play space from the room ARKit has NOW, without a game asking for the scene (kl_spatial_synth_room does the
// same when one does): the footprint of the measured planes / scene mesh around guest (hx, hz) goes to kl_guardian_default_from_room
// (a user boundary wins; nothing changes when it does not fit 1.15 m). At most once per 2 s (the build walks every scene-mesh vertex).
// Returns kl_guardian_default_from_room's answer, 0 when rate-limited or when no valid footprint exists.
static uint64_t g_gd_last_ns;
int kl_spatial_update_guardian_default(float hx, float hz) {
    if (!isfinite(hx) || !isfinite(hz)) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, prev = __atomic_load_n(&g_gd_last_ns, __ATOMIC_RELAXED);
    if (prev && now - prev < 2000000000ull) return 0;
    __atomic_store_n(&g_gd_last_ns, now, __ATOMIC_RELAXED);
    const float oy = kl_ovrp_guest_origin_y();
    float gfy = 0.0f, rfy = 1e30f, cey = -1e30f;
    int gsrc = kl_guardian_get_floor(&gfy);            // same floor / ceiling rules as kl_spatial_synth_room
    float fy = gfy - oy;
    klsp_pt hull[KLSP_MAX_WALLS];
    klsp_sig sig;
    int npts = 0;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_e[i].kind != KLSP_PLANE || is_synth(g_e[i].id)) continue;
        float g[16];
        to_guest_locked(g_e[i].m, g);
        if (g_e[i].label == KLSP_LBL_FLOOR && g[13] < rfy) rfy = g[13];
        if (g_e[i].label == KLSP_LBL_CEILING && g[13] > cey) cey = g[13];
    }
    if (rfy < 1e29f && !gsrc) fy = rfy;
    float hb = cey > -1e29f && cey - fy >= 1.5f && cey - fy <= 6.0f ? cey - fy : 2.4f;
    int nh = built_footprint_locked(fy - 0.3f, fy + hb + 0.3f, hx, hz, hull, &npts, &sig);
    if (nh && !klsp_hull_room_valid(hull, nh, fy, hb)) nh = 0;
    pthread_mutex_unlock(&g_mu);
    return nh ? kl_guardian_default_from_room(&hull[0].x, nh) : 0;   // outside g_mu: the guardian has its own locks
}
