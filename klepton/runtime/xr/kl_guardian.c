// kl_guardian.c - the Guardian core behind kl_guardian.h (own code; rules and numbers from informe_fw_guardian.md).
#include "kl_guardian.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

typedef struct { float w, d, cx, cz, yaw, cell; } rect_t;   // cell = grid step the sides were counted in

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;        // state below
static pthread_mutex_t g_calc_mu = PTHREAD_MUTEX_INITIALIZER;   // g_tmp / g_grid; taken before g_mu
static float g_floor_y, g_pref_y = NAN;
static int g_floor_src, g_mode, g_bsrc, g_n, g_configured;
static float g_radius = KLG_DEFAULT_RADIUS;
static float g_pts[KLG_MAX_POINTS * 2];
static rect_t g_rect;
static uint32_t g_gen;

// ---- floor ----
static int floor_set_locked(float y, int src) {
    if (!(fabsf(y) <= 10.0f)) return 0;                       // |y| <= 10 m keeps every caller's int mm math safe
    if (y == g_floor_y && src == g_floor_src) return 0;
    g_floor_y = y; g_floor_src = src; g_gen++;
    return 1;
}
int kl_guardian_set_floor(float y, int source) {
    if (source < KLG_FLOOR_NONE || source > KLG_FLOOR_MANUAL) return 0;
    if (source != KLG_FLOOR_NONE && !(fabsf(y) <= 10.0f)) return 0;      // also rejects NaN / inf
    pthread_mutex_lock(&g_mu);
    if (source == KLG_FLOOR_AUTO && (g_floor_src == KLG_FLOOR_MANUAL                 // a manual floor is only replaced by clear / manual
                                     || (g_floor_src == KLG_FLOOR_AUTO && fabsf(y - g_floor_y) < 0.01f))) {   // ARKit jitter: no new generation
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    int r = floor_set_locked(source == KLG_FLOOR_NONE ? 0.0f : y, source);
    pthread_mutex_unlock(&g_mu);
    return r;
}
int kl_guardian_get_floor(float *y) {
    pthread_mutex_lock(&g_mu);
    int s = g_floor_src;
    if (y) *y = s ? g_floor_y : 0.0f;
    pthread_mutex_unlock(&g_mu);
    return s;
}
void kl_guardian_floor_confirm(void) {
    pthread_mutex_lock(&g_mu);
    if (g_floor_src) g_pref_y = g_floor_y;
    pthread_mutex_unlock(&g_mu);
}
float kl_guardian_get_user_preferred_floor(void) {
    pthread_mutex_lock(&g_mu);
    float y = g_pref_y;
    pthread_mutex_unlock(&g_mu);
    return y;
}
int kl_guardian_floor_plausible(float floorY, float headY, int from_slam) {
    if (!isfinite(floorY) || !isfinite(headY)) return 0;
    float d = floorY - headY, eps = 1e-5f;
    return d >= -2.5f - eps && d <= (from_slam ? -0.7f : 0.0f) + eps;
}
float kl_guardian_floor_nudge(float stickY, float headY, float controllerY) {
    pthread_mutex_lock(&g_mu);
    float f = g_floor_y;
    if (!isfinite(headY)) { pthread_mutex_unlock(&g_mu); return f; }
    if (!g_floor_src) f = headY - 1.15f;
    int hand = isfinite(controllerY);
    if (hand && controllerY < f) f = controllerY;
    if (isfinite(stickY) && fabsf(stickY) >= 0.1f) {
        f += stickY > 0 ? 0.006f : -0.006f;
        if (hand) f = fminf(fmaxf(f, controllerY - 1.0f), controllerY + 1.0f);
    }
    if (f > headY - 0.1524f) f = headY - 0.1524f;
    floor_set_locked(f, KLG_FLOOR_MANUAL);
    f = g_floor_y;
    pthread_mutex_unlock(&g_mu);
    return f;
}
float kl_guardian_floor_lower_to(float y) {
    pthread_mutex_lock(&g_mu);
    if (isfinite(y) && g_floor_src && y < g_floor_y) floor_set_locked(y, KLG_FLOOR_MANUAL);
    float f = g_floor_y;
    pthread_mutex_unlock(&g_mu);
    return f;
}

static int g_grab;
int kl_guardian_floor_grab(int pressed, float handY, float headY) {
    pthread_mutex_lock(&g_mu);
    g_grab = pressed != 0;
    if (g_grab && isfinite(handY) && isfinite(headY))
        floor_set_locked(fminf(fmaxf(handY, headY - 2.5f), headY - 0.1524f), KLG_FLOOR_MANUAL);   // up or down; never above head - 0.1524
    int r = g_grab;
    pthread_mutex_unlock(&g_mu);
    return r;
}
int kl_guardian_floor_grabbing(void) {
    pthread_mutex_lock(&g_mu);
    int r = g_grab;
    pthread_mutex_unlock(&g_mu);
    return r;
}

// ---- largest oriented rectangle inside a polygon ----
// For each turn (5 degree steps over 90) the polygon is rasterised (even-odd, cell centres) into a bitmap and the largest
// all-inside block of cells is found with the histogram stack method. Cells are 2 cm up to a 7.6 m room, then bigger (the
// grid is at most 400 x 400); the answer is a count of cells, so it errs under by at most a cell.
#define GN 400
static uint8_t g_grid[GN * GN];
static float g_tmp[KLG_MAX_POINTS * 2];

static void best_block(int nx, int nz, int *bw, int *bd, int *bi, int *bj) {   // largest block: w cols x d rows, ends at col bi-1... start (bi, bj)
    uint16_t h[GN + 1];
    int st[GN + 2];
    int best = 0;
    memset(h, 0, sizeof h);
    *bw = *bd = *bi = *bj = 0;
    for (int j = 0; j < nz; j++) {
        const uint8_t *row = g_grid + (size_t)j * nx;
        for (int i = 0; i < nx; i++) h[i] = row[i] ? (uint16_t)(h[i] + 1) : 0;
        h[nx] = 0;
        int sp = 0;
        for (int i = 0; i <= nx; i++) {
            while (sp > 0 && h[st[sp - 1]] >= h[i]) {
                int hh = h[st[--sp]], left = sp ? st[sp - 1] + 1 : 0, w = i - left;
                if (hh * w > best) { best = hh * w; *bw = w; *bd = hh; *bi = left; *bj = j - hh + 1; }
            }
            st[sp++] = i;
        }
    }
}
static rect_t inscribed(const float *p, int n) {
    rect_t best = {0, 0, 0, 0, 0, 0};
    float best_area = 0;
    for (int a = 0; a < 90; a += 5) {
        float rad = (float)a * 0.017453292f, c = cosf(rad), s = sinf(rad);
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (int k = 0; k < n; k++) {
            float u = c * p[2 * k] + s * p[2 * k + 1], v = -s * p[2 * k] + c * p[2 * k + 1];
            x0 = fminf(x0, u); x1 = fmaxf(x1, u); z0 = fminf(z0, v); z1 = fmaxf(z1, v);
        }
        float cell = fmaxf(0.02f, fmaxf(x1 - x0, z1 - z0) / 380.0f);
        int nx = (int)ceilf((x1 - x0) / cell) + 1, nz = (int)ceilf((z1 - z0) / cell) + 1;
        if (nx > GN) nx = GN;
        if (nz > GN) nz = GN;
        if (nx < 1) nx = 1;
        if (nz < 1) nz = 1;
        memset(g_grid, 0, (size_t)nx * nz);
        for (int k = 0; k < n; k++) {
            int m = (k + 1) % n;
            float au = c * p[2 * k] + s * p[2 * k + 1], av = -s * p[2 * k] + c * p[2 * k + 1];
            float bu = c * p[2 * m] + s * p[2 * m + 1], bv = -s * p[2 * m] + c * p[2 * m + 1];
            if (av == bv) continue;
            float lo = fminf(av, bv), hi = fmaxf(av, bv);
            int j0 = (int)ceilf((lo - z0) / cell - 0.5f), j1 = (int)ceilf((hi - z0) / cell - 0.5f) - 1;   // centre in [lo, hi)
            if (j0 < 0) j0 = 0;
            if (j1 > nz - 1) j1 = nz - 1;
            for (int j = j0; j <= j1; j++) {
                float zc = z0 + ((float)j + 0.5f) * cell, u = au + (zc - av) * (bu - au) / (bv - av);
                int i = (int)ceilf((u - x0) / cell - 0.5f);
                if (i < 0) i = 0;
                if (i < nx) g_grid[(size_t)j * nx + i] ^= 1;
            }
        }
        for (int j = 0; j < nz; j++) {
            uint8_t run = 0;
            for (int i = 0; i < nx; i++) { run ^= g_grid[(size_t)j * nx + i]; g_grid[(size_t)j * nx + i] = run; }
        }
        int bw, bd, bi, bj;
        best_block(nx, nz, &bw, &bd, &bi, &bj);
        float area = (float)bw * (float)bd * cell * cell;
        if (area > best_area) {
            float cu = x0 + ((float)bi + (float)bw * 0.5f) * cell, cv = z0 + ((float)bj + (float)bd * 0.5f) * cell;
            best_area = area;
            best = (rect_t){(float)bw * cell, (float)bd * cell, c * cu - s * cv, s * cu + c * cv, rad, cell};
        }
    }
    return best;
}
// A side is a count of cell centres inside the polygon, so an exact 1.15 m room measures one cell short: allow that cell.
static int rect_ok(const rect_t *r) { return fminf(r->w, r->d) >= KLG_MIN_SIDE - r->cell - 1e-4f && r->w > 0 && r->d > 0; }

// ---- boundary ----
static float clamp_radius(float r) { return isfinite(r) && r >= 0.3f && r <= 10.0f ? r : KLG_DEFAULT_RADIUS; }
// Copies p[0..n) to g_tmp counter-clockwise (more than KLG_MAX_POINTS: evenly spaced subset); returns the stored point count,
// 0 when a coordinate is not finite or beyond 1e4 m, or the area is ~0. Caller holds g_calc_mu.
static int load_tmp(const float *p, int n) {
    double area = 0;
    int cnt = n > KLG_MAX_POINTS ? KLG_MAX_POINTS : n;
    for (int k = 0; k < cnt; k++) {
        int a = n > KLG_MAX_POINTS ? (int)((long long)k * n / KLG_MAX_POINTS) : k;
        int b = n > KLG_MAX_POINTS ? (int)((long long)((k + 1) % cnt) * n / KLG_MAX_POINTS) : (k + 1) % cnt;
        if (!(fabsf(p[2 * a]) <= 1e4f) || !(fabsf(p[2 * a + 1]) <= 1e4f)) return 0;      // also rejects NaN / inf
        area += (double)p[2 * a] * p[2 * b + 1] - (double)p[2 * b] * p[2 * a + 1];
    }
    if (!(fabs(area) > 2e-4)) return 0;
    for (int k = 0; k < cnt; k++) {
        int s = area > 0 ? k : cnt - 1 - k;
        if (n > KLG_MAX_POINTS) s = (int)((long long)s * n / KLG_MAX_POINTS);
        g_tmp[2 * k] = p[2 * s]; g_tmp[2 * k + 1] = p[2 * s + 1];
    }
    return cnt;
}
static int commit_locked(int n, int mode, float radius, int bsrc, const rect_t *r) {   // radius <= 0 keeps the current one
    if (!(radius > 0)) radius = g_radius;
    if (g_configured && g_n == n && g_mode == mode && g_radius == radius && g_bsrc == bsrc
        && (n == 0 || !memcmp(g_pts, g_tmp, (size_t)n * 2 * sizeof(float)))) return 0;
    g_n = n; g_mode = mode; g_radius = radius; g_bsrc = bsrc; g_configured = 1;
    if (n) memcpy(g_pts, g_tmp, (size_t)n * 2 * sizeof(float));
    g_rect = r ? *r : (rect_t){0, 0, 0, 0, 0};
    g_gen++;
    return 1;
}
int kl_guardian_set_boundary(const float *points, int n, int mode, float radius) {
    if (mode != KLG_MODE_ROOMSCALE && mode != KLG_MODE_STATIONARY) return -1;
    radius = radius > 0 ? clamp_radius(radius) : 0.0f;
    pthread_mutex_lock(&g_calc_mu);
    int r;
    if (mode == KLG_MODE_STATIONARY) {
        pthread_mutex_lock(&g_mu);
        commit_locked(0, mode, radius, KLG_BSRC_USER, NULL);
        pthread_mutex_unlock(&g_mu);
        r = 1;
    } else if (!points || n < 3 || !(n = load_tmp(points, n))) {
        r = -1;
    } else {
        rect_t rc = inscribed(g_tmp, n);
        pthread_mutex_lock(&g_mu);
        commit_locked(n, mode, radius, KLG_BSRC_USER, &rc);
        pthread_mutex_unlock(&g_mu);
        r = rect_ok(&rc) ? 1 : 0;
    }
    pthread_mutex_unlock(&g_calc_mu);
    return r;
}
void kl_guardian_set_stationary_radius(float radius) {
    pthread_mutex_lock(&g_mu);
    float r = clamp_radius(radius);
    if (r != g_radius) { g_radius = r; g_gen++; }
    pthread_mutex_unlock(&g_mu);
}
void kl_guardian_clear(void) {
    pthread_mutex_lock(&g_mu);
    if (g_configured) { g_configured = 0; g_n = 0; g_bsrc = KLG_BSRC_NONE; g_mode = 0; g_rect = (rect_t){0, 0, 0, 0, 0}; g_gen++; }
    pthread_mutex_unlock(&g_mu);
}
int kl_guardian_default_from_room(const float *xz, int n) {
    if (!xz) return 0;
    if (n < 3) return 0;
    pthread_mutex_lock(&g_mu);
    int user = g_bsrc == KLG_BSRC_USER;
    pthread_mutex_unlock(&g_mu);
    if (user) return 0;
    pthread_mutex_lock(&g_calc_mu);
    int r = 0;
    if ((n = load_tmp(xz, n)) != 0) {
        rect_t rc = inscribed(g_tmp, n);
        if (rect_ok(&rc)) {
            pthread_mutex_lock(&g_mu);
            if (g_bsrc != KLG_BSRC_USER) { commit_locked(n, KLG_MODE_ROOMSCALE, 0.0f, KLG_BSRC_ROOM, &rc); r = 1; }
            pthread_mutex_unlock(&g_mu);
        }
    }
    pthread_mutex_unlock(&g_calc_mu);
    return r;
}
int kl_guardian_configured(void) { pthread_mutex_lock(&g_mu); int c = g_configured; pthread_mutex_unlock(&g_mu); return c; }
int kl_guardian_boundary_source(void) { pthread_mutex_lock(&g_mu); int c = g_configured ? g_bsrc : 0; pthread_mutex_unlock(&g_mu); return c; }
int kl_guardian_get_mode(void) { pthread_mutex_lock(&g_mu); int c = g_mode; pthread_mutex_unlock(&g_mu); return c; }
float kl_guardian_get_radius(void) { pthread_mutex_lock(&g_mu); float c = g_radius; pthread_mutex_unlock(&g_mu); return c; }
uint32_t kl_guardian_generation(void) { pthread_mutex_lock(&g_mu); uint32_t c = g_gen; pthread_mutex_unlock(&g_mu); return c; }
int kl_guardian_get_boundary(float *xz, int cap) {
    pthread_mutex_lock(&g_mu);
    int n = g_configured ? g_n : 0;
    if (xz && n > 0) {
        if (cap < n) n = cap > 0 ? cap : 0;
        memcpy(xz, g_pts, (size_t)n * 2 * sizeof(float));
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
int kl_guardian_playspace(float *w, float *d, float *cx, float *cz, float *yaw) {
    pthread_mutex_lock(&g_mu);
    rect_t r = g_configured && g_mode == KLG_MODE_ROOMSCALE && g_n ? g_rect : (rect_t){0, 0, 0, 0, 0};
    pthread_mutex_unlock(&g_mu);
    if (w) *w = r.w;
    if (d) *d = r.d;
    if (cx) *cx = r.cx;
    if (cz) *cz = r.cz;
    if (yaw) *yaw = r.yaw;
    return rect_ok(&r);
}
int kl_guardian_playspace_ok(void) { return kl_guardian_playspace(NULL, NULL, NULL, NULL, NULL); }
int kl_guardian_effective(int *circle, float *w, float *d) {
    pthread_mutex_lock(&g_mu);
    int on = g_configured, rs = on && g_mode == KLG_MODE_ROOMSCALE && g_n && rect_ok(&g_rect);
    rect_t r = g_rect;
    float dia = g_radius * 2.0f;
    pthread_mutex_unlock(&g_mu);
    if (!on) return 0;
    if (circle) *circle = !rs;
    if (w) *w = rs ? r.w : dia;
    if (d) *d = rs ? r.d : dia;
    return 1;
}
int kl_guardian_polygon_test(float x, float z, float *dist, float *px, float *pz, float *nx, float *nz) {
    pthread_mutex_lock(&g_mu);
    if (!(g_configured && g_mode == KLG_MODE_ROOMSCALE && g_n && rect_ok(&g_rect))) { pthread_mutex_unlock(&g_mu); return 0; }
    float bd = 1e30f, bx = 0, bz = 0, bnx = 0, bnz = 0;
    int inside = 0;
    for (int k = 0; k < g_n; k++) {
        int m = (k + 1) % g_n;
        float ax = g_pts[2 * k], az = g_pts[2 * k + 1], ex = g_pts[2 * m] - ax, ez = g_pts[2 * m + 1] - az;
        if ((az > z) != (g_pts[2 * m + 1] > z) && x < ax + (z - az) * ex / ez) inside ^= 1;
        float l2 = ex * ex + ez * ez, t = l2 > 1e-12f ? ((x - ax) * ex + (z - az) * ez) / l2 : 0.0f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float qx = ax + t * ex, qz = az + t * ez, dd = hypotf(x - qx, z - qz);
        if (dd < bd) {
            float l = sqrtf(l2);
            bd = dd; bx = qx; bz = qz;
            bnx = l > 1e-9f ? -ez / l : 0.0f; bnz = l > 1e-9f ? ex / l : 0.0f;       // left of a counter-clockwise edge = inward
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (dist) *dist = inside ? bd : -bd;
    if (px) *px = bx;
    if (pz) *pz = bz;
    if (nx) *nx = bnx;
    if (nz) *nz = bnz;
    return 1;
}

// ---- room rules ----
int kl_guardian_room_v1_valid(int outer_walls, int floors, int ceilings, float floorY, float ceilingY) {
    return outer_walls >= 3 && floors >= 1 && ceilings >= 1 && isfinite(floorY) && isfinite(ceilingY) && ceilingY > floorY;
}
int kl_guardian_hull_ok(const float *xz, int n, float H) {
    if (!xz || n < 3 || !(H >= 1.5f && H <= 6.0f)) return 0;
    double area = 0;
    for (int k = 0; k < n; k++) {
        int m = (k + 1) % n;
        if (!(fabsf(xz[2 * k]) <= 1e4f) || !(fabsf(xz[2 * k + 1]) <= 1e4f)) return 0;
        area += (double)xz[2 * k] * xz[2 * m + 1] - (double)xz[2 * m] * xz[2 * k + 1];
    }
    return fabs(area) * 0.5 >= 0.01;
}
int kl_guardian_ring_closed(const float *q, int nwalls, float tol) {
    if (!q || nwalls < 3) return 0;
    for (int i = 0; i < nwalls; i++) {
        const float *a = q + 12 * i, *b = q + 12 * ((i + 1) % nwalls);
        for (int ax = 0; ax < 3; ax++) {
            if (!(fabsf(a[9 + ax] - b[ax]) <= tol) || !(fabsf(a[6 + ax] - b[3 + ax]) <= tol)) return 0;
        }
    }
    return 1;
}
int kl_guardian_roombox_vertices_ok(int nverts) { return nverts >= 12 && nverts % 4 == 0; }
int kl_guardian_roombox_valid(const float *quads, int nwalls, float floorY, float ceilingY) {
    return kl_guardian_roombox_vertices_ok(nwalls * 4) && isfinite(floorY) && isfinite(ceilingY) && ceilingY > floorY
           && kl_guardian_ring_closed(quads, nwalls, 0.05f);
}

// ---- play area (inscribed rectangle) vs outer polygon ----
int kl_guardian_playarea_corners(float xz[8]) {
    float w, d, cx, cz, yaw;
    if (!kl_guardian_playspace(&w, &d, &cx, &cz, &yaw)) return 0;
    float c = cosf(yaw), s = sinf(yaw), hw = w * 0.5f, hd = d * 0.5f;
    const float k[4][2] = {{-hw, -hd}, {hw, -hd}, {hw, hd}, {-hw, hd}};      // (u, v) axes: (c, s) and (-s, c)
    for (int i = 0; i < 4; i++) { xz[2 * i] = cx + c * k[i][0] - s * k[i][1]; xz[2 * i + 1] = cz + s * k[i][0] + c * k[i][1]; }
    return 1;
}
int kl_guardian_playarea_test(float x, float z, float *dist, float *px, float *pz, float *nx, float *nz) {
    float w, d, cx, cz, yaw;
    if (!kl_guardian_playspace(&w, &d, &cx, &cz, &yaw)) return 0;
    float c = cosf(yaw), s = sinf(yaw), hw = w * 0.5f, hd = d * 0.5f;
    float lu = c * (x - cx) + s * (z - cz), lv = -s * (x - cx) + c * (z - cz);
    float du = hw - fabsf(lu), dv = hd - fabsf(lv), su = lu < 0 ? -1.0f : 1.0f, sv = lv < 0 ? -1.0f : 1.0f;
    float pu, pv, nu = 0, nv = 0, dd;
    if (du < dv) { pu = su * hw; pv = lv; nu = -su; dd = du; }
    else { pu = lu; pv = sv * hd; nv = -sv; dd = dv; }
    if (dist) *dist = dd;
    if (px) *px = cx + c * pu - s * pv;
    if (pz) *pz = cz + s * pu + c * pv;
    if (nx) *nx = c * nu - s * nv;
    if (nz) *nz = s * nu + c * nv;
    return 1;
}
int kl_guardian_outer_size(float *w, float *d) {
    pthread_mutex_lock(&g_mu);
    int ok = g_configured && g_mode == KLG_MODE_ROOMSCALE && g_n && rect_ok(&g_rect);
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (int k = 0; ok && k < g_n; k++) {
        x0 = fminf(x0, g_pts[2 * k]); x1 = fmaxf(x1, g_pts[2 * k]); z0 = fminf(z0, g_pts[2 * k + 1]); z1 = fmaxf(z1, g_pts[2 * k + 1]);
    }
    pthread_mutex_unlock(&g_mu);
    if (!ok) return 0;
    if (w) *w = x1 - x0;
    if (d) *d = z1 - z0;
    return 1;
}
