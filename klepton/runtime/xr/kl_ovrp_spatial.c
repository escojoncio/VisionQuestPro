// kl_ovrp_spatial.c - Lote 3: ovrp espacio (anclas / escena / planos / raycast) sobre kl_spatial.
//
// The OVRPlugin face of the spatial store (kl_spatial.c, fed by ARKit through
// KleptonSpatial.swift). Registered with kl_ovrp.c through
// kl_ovrp_spatial_register (recipe in kl_ovrp_priv.h).
//
// ---------------------------------------------------------------------------
// ABI notes (no OVR_Plugin headers are available; this is the OVRPlugin.cs
// P/Invoke shape, i.e. what a Unity guest really passes)
// ---------------------------------------------------------------------------
//  * Everything here returns ovrpResult (0 = success). None is a v1 ovrpBool.
//  * A space is `ref UInt64 space`: a POINTER to the handle. The decoder U()
//    also accepts the handle by value: our handles are < 4 GB, no arm64 user
//    pointer is, so the two cannot be confused (see kl_spatial.c).
//  * ovrpUuid is 16 raw bytes (a .NET Guid is marshalled byte for byte).
//  * Async calls return a requestId; the result arrives as an event through
//    kl_ovrp_push_event, visible only from the NEXT ovrp_PollEvent2.
//  * Event layouts are the natural C layout of the OVRPlugin.cs structs
//    (EventDataSpatialAnchorCreateComplete {u64 requestId; i32 result; u64 space;
//    uuid}, ...). The ovrpEventType numbers are the OVRPlugin.cs values as
//    recalled; the Discovery ones are a GUESS (marked below).
#include "kl_ovrp_priv.h"
#include "kl_spatial.h"
#include "kl_envdepth.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ONCE(msg) do { static int _o; if (!_o) { _o = 1; fprintf(stderr, "  [ovrp-spatial] %s\n", (msg)); } } while (0)

// Permanent traces, counted against the shared diagnostic budget (kl_ovrp_diag_ok); nothing per frame.
//   [sp-call] fn=<ovrp_*> n=<k> <key args/outs> ret=<ovrpResult>   the first SP_PER_FN calls of each function, and again at
//                                                                   the 100th, 1000th and 10000th (outside the budget: 3 lines/fn)
//   LocateSpace/LocateSpace2 add: space=0x.. ok=0|1 why=none|space|pose|cached (ok=0 for cached) kind=<KLSP_*> flags=0x.. p=(x,y,z) q=(x,y,z,w) origin=<arg>
//                                 (why=cached: the plane left the live registry, the last pose given for the handle is returned, flags 0x3)
//   [sp-loc-bad] space=0x.. kind=<KLSP_*> why=space|pose   the first SP_LOCBAD_MAX distinct handles whose locate failed (outside the budget)
//   [sp-ev] push|polled type=<ovrpEventType> req=<requestId> ...   every event pushed / handed to the guest (SP_PER_EV per type)
// A function's body is sp_<Name> (always inlined, so the frame the guest-caller lookup in kl_ovrp_hit walks is the
// wrapper's, as before); the table entry klovrp_<Name> calls it and logs the arguments and the code returned.
#define SP_PER_FN 20
#define SP_PER_EV 50
#define SP_REPEAT(k) ((k) == 100 || (k) == 1000 || (k) == 10000)
#define SP_IMPL static inline __attribute__((always_inline)) ovrpResult
#define ULL(x) ((unsigned long long)(x))
#define SPT(fn, ret, ...) do { \
        static unsigned _c; \
        static sp_stat _s; \
        int32_t _r = (int32_t)(ret); \
        unsigned _k = __atomic_add_fetch(&_c, 1, __ATOMIC_RELAXED); \
        sp_note(&_s, fn, _r); \
        if ((_k <= SP_PER_FN && kl_ovrp_diag_ok()) || SP_REPEAT(_k)) sp_emit(fn, _k, _r, __VA_ARGS__); \
    } while (0)
static void sp_emit(const char *fn, unsigned k, int32_t ret, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void sp_emit(const char *fn, unsigned k, int32_t ret, const char *fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "  [sp-call] fn=%s n=%u %s ret=%d\n", fn, k, b, (int)ret);
}

// ---- [sp-sum]: periodic summary, outside the [sp-call]/[sp-ev] budgets ---------------------------------------
// The per-call traces stop after SP_PER_FN calls per function and KL_OVRP_DIAG_LINES lines in all, so a long MR
// session goes dark. Every SPT call site also bumps a counter and keeps the last return value; every 5 s ONE line is
// written (not counted in the budget):  [sp-sum] t=<s>s Fn=<calls>/<lastret> ... Poll=<PollEvent calls> evN=<events
// of type N delivered> ...  (calls and events are totals since the start; only what changed since the last line is
// listed, except Poll, which is always there: a line with Poll alone = no spatial call and no event in that window,
// no line at all = the guest stopped polling).
// loc_ok= loc_bad_space= loc_bad_pose= loc_cached= are the Locate results of THIS window only (reset at every line), listed when any is not 0.
// A line longer than SP_SUM_LINE is split in several lines of the same instant. The Poll-alone lines (heartbeat) stop at
// SP_SUM_HEARTBEATS per process (1 h, like [pt-sum]); the lines that carry changes are not counted.
#define SP_SUM_EVERY_NS 5000000000ull
#define SP_SUM_HEARTBEATS 720
#define SP_SUM_LINE 900
#define SP_SUM_MAXFN 96
#define SP_SUM_MAXEV 129                   // event types 0..127, [128] = anything else
typedef struct { const char *name; unsigned calls, rep; int32_t last; int reg; } sp_stat;

// KL-TEST-BEGIN spsum
// One "Fn=calls/lastret" token; "ovrp_" is dropped from the name to keep the line short.
static void sp_sum_tok_fn(char *buf, size_t cap, const char *name, unsigned calls, int32_t last) {
    if (strncmp(name, "ovrp_", 5) == 0) name += 5;
    snprintf(buf, cap, "%s=%u/%d", name, calls, (int)last);
}
// Joins the tokens as "[sp-sum] t=<t>s tok tok ..." and hands each finished line (no newline) to emit; a line
// is closed before it would pass `maxlen` characters and the next one repeats the prefix. A token is never cut.
static void sp_sum_build(double t, const char *const *tok, unsigned n, size_t maxlen,
                         void (*emit)(const char *line, void *ctx), void *ctx) {
    char line[1024], pre[48];
    int pl = snprintf(pre, sizeof pre, "[sp-sum] t=%.0fs", t);
    size_t cap = maxlen < sizeof line - 1 ? maxlen : sizeof line - 1;
    size_t len = (size_t)pl;
    unsigned in_line = 0;
    memcpy(line, pre, len + 1);
    for (unsigned i = 0; i < n; i++) {
        size_t tl = strlen(tok[i]);
        if (in_line && len + 1 + tl > cap) {
            emit(line, ctx);
            memcpy(line, pre, (size_t)pl + 1);
            len = (size_t)pl;
            in_line = 0;
        }
        if (len + 1 + tl > sizeof line - 1) continue;      // a single token larger than the buffer: dropped
        line[len++] = ' ';
        memcpy(line + len, tok[i], tl + 1);
        len += tl;
        in_line++;
    }
    if (in_line) emit(line, ctx);
}
// KL-TEST-END spsum

static sp_stat *volatile g_sp_reg[SP_SUM_MAXFN];
static unsigned g_sp_nreg;
static unsigned g_sp_poll, g_sp_ev[SP_SUM_MAXEV], g_sp_ev_rep[SP_SUM_MAXEV];
static unsigned g_sp_loc[4];                               // window counters: ok, bad space, bad pose, cached
static uint64_t g_sp_t0, g_sp_due;
static void (*g_sp_sink)(const char *line);                // tests; NULL = stderr

static void sp_sum_line(const char *line, void *ctx) {
    (void)ctx;
    if (g_sp_sink) g_sp_sink(line); else fprintf(stderr, "%s\n", line);
}

// The tick: emits the line when 5 s have passed since the last one and something changed. now_ns is any monotonic
// clock; the first call only fixes t0. Safe from any thread (one caller wins each window).
void kl_ovrp_sp_sum_tick_at(uint64_t now_ns) {
    uint64_t due = __atomic_load_n(&g_sp_due, __ATOMIC_ACQUIRE);
    if (!due) {
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(&g_sp_due, &zero, now_ns + SP_SUM_EVERY_NS, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            g_sp_t0 = now_ns;
        return;
    }
    if (now_ns < due || !__atomic_compare_exchange_n(&g_sp_due, &due, now_ns + SP_SUM_EVERY_NS, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    static char tokb[SP_SUM_MAXFN + SP_SUM_MAXEV + 4][56];
    const char *tok[SP_SUM_MAXFN + SP_SUM_MAXEV + 4];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;  // tokb is shared; the window is already won, this is only the buffer
    pthread_mutex_lock(&mu);
    unsigned n = 0;
    unsigned nreg = __atomic_load_n(&g_sp_nreg, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < nreg && i < SP_SUM_MAXFN; i++) {
        sp_stat *s = g_sp_reg[i];
        if (!s) continue;
        unsigned c = __atomic_load_n(&s->calls, __ATOMIC_RELAXED);
        if (c == s->rep) continue;
        s->rep = c;
        sp_sum_tok_fn(tokb[n], sizeof tokb[n], s->name, c, __atomic_load_n(&s->last, __ATOMIC_RELAXED));
        tok[n] = tokb[n]; n++;
    }
    {
        static const char *const nm[4] = {"loc_ok", "loc_bad_space", "loc_bad_pose", "loc_cached"};
        unsigned lc[4], any = 0;
        for (int i = 0; i < 4; i++) { lc[i] = __atomic_exchange_n(&g_sp_loc[i], 0u, __ATOMIC_RELAXED); any |= lc[i]; }
        if (any) for (int i = 0; i < 4; i++) { snprintf(tokb[n], sizeof tokb[n], "%s=%u", nm[i], lc[i]); tok[n] = tokb[n]; n++; }
    }
    unsigned evfirst = n;
    for (unsigned t = 0; t < SP_SUM_MAXEV; t++) {
        unsigned c = __atomic_load_n(&g_sp_ev[t], __ATOMIC_RELAXED);
        if (c == g_sp_ev_rep[t]) continue;
        g_sp_ev_rep[t] = c;
        if (t == SP_SUM_MAXEV - 1) snprintf(tokb[n], sizeof tokb[n], "evOther=%u", c);
        else snprintf(tokb[n], sizeof tokb[n], "ev%u=%u", t, c);
        tok[n] = tokb[n]; n++;
    }
    static unsigned heartbeats;  // under mu
    if (n == 0 && heartbeats++ >= SP_SUM_HEARTBEATS) {  // nothing changed and the heartbeat budget is spent
        pthread_mutex_unlock(&mu);
        return;
    }
    // Poll goes first among the events so a split line still carries it.
    snprintf(tokb[n], sizeof tokb[n], "Poll=%u", __atomic_load_n(&g_sp_poll, __ATOMIC_RELAXED));
    tok[n] = tokb[n];
    const char *poll = tok[n];
    for (unsigned i = n; i > evfirst; i--) tok[i] = tok[i - 1];
    tok[evfirst] = poll;
    n++;
    sp_sum_build((double)(now_ns - g_sp_t0) / 1e9, tok, n, SP_SUM_LINE, sp_sum_line, NULL);
    pthread_mutex_unlock(&mu);
}
static void sp_sum_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    kl_ovrp_sp_sum_tick_at((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}
// Called by every SPT call site: count + last return, and register the site on its first call.
static void sp_note(sp_stat *s, const char *name, int32_t ret) {
    __atomic_add_fetch(&s->calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->last, ret, __ATOMIC_RELAXED);
    int zero = 0;
    if (__atomic_compare_exchange_n(&s->reg, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s->name = name;
        unsigned i = __atomic_fetch_add(&g_sp_nreg, 1, __ATOMIC_ACQ_REL);
        if (i < SP_SUM_MAXFN) __atomic_store_n(&g_sp_reg[i], s, __ATOMIC_RELEASE);
    }
    sp_sum_tick();
}
// ovrp_PollEvent / ovrp_PollEvent2 (kl_ovrp.c): one call, and the type of the event it delivered (-1 = queue empty).
void kl_ovrp_sp_note_poll(int32_t type) {
    __atomic_add_fetch(&g_sp_poll, 1, __ATOMIC_RELAXED);
    if (type >= 0) __atomic_add_fetch(&g_sp_ev[type < 128 ? type : 128], 1, __ATOMIC_RELAXED);
    sp_sum_tick();
}
void kl_ovrp_sp_sum_set_sink(void (*sink)(const char *line)) { g_sp_sink = sink; }

// ---- ovrp constants ---------------------------------------------------------
enum {
    EV_ANCHOR_CREATE   = 49,
    EV_SET_STATUS      = 50,
    EV_QUERY_RESULTS   = 51,
    EV_QUERY_COMPLETE  = 52,
    EV_SAVE_COMPLETE   = 53,
    EV_ERASE_COMPLETE  = 54,
    EV_LIST_SAVE       = 57,
    EV_SCENE_CAPTURE   = 100,
    // UNVERIFIED guesses (XR_META_spatial_entity_discovery has no ovrp header here).
    EV_DISC_RESULTS    = 104,
    EV_DISC_COMPLETE   = 105,
};
enum {
    C_LOCATABLE = 0, C_STORABLE = 1, C_SHARABLE = 2, C_BOUNDED2D = 3, C_BOUNDED3D = 4,
    C_LABELS = 5, C_ROOMLAYOUT = 6, C_CONTAINER = 7, C_TRIMESH = 1000269000, C_TRIMESH_ALT = 8,
};
#define STORAGE_LOCAL 1                   // ovrpSpaceStorageLocation: 0 Invalid, 1 Local, 2 Cloud (as XR_SPACE_STORAGE_LOCATION_LOCAL_FB)
#define IS_LOCAL_LOC(l) ((l) == STORAGE_LOCAL || (l) == 0)  // loc 0 (Invalid / unfilled struct) is served from the local store as before; only Cloud (2) is refused
#define FILTER_IDS 1
#define FILTER_COMPONENTS 2
#define OVRP_APP_SPACE 0x0A55ACEull       // ovrp_GetAppSpace handle (never a kl_spatial handle)

// ---- argument decoding --------------------------------------------------------
#define PTR_MIN 0x100000000ull
static int isptr(uint64_t a) { return a >= PTR_MIN; }
static uint64_t U(uint64_t a) { return isptr(a) ? *(const uint64_t *)(uintptr_t)a : a; }
static uint64_t sp_h(uint64_t a) { return a ? U(a) : 0; }                     // a space handle as logged
static uint64_t sp_out(uint64_t a2, uint64_t a3) {                              // SaveSpaces: the requestId it wrote
    uint64_t o = isptr(a2) ? a2 : (isptr(a3) ? a3 : 0);
    return o ? *(const uint64_t *)(uintptr_t)o : 0;
}
static int space_of(uint64_t arg, kl_uuid *id, int *kind) {
    if (!arg) return 0;
    int k = 0;
    if (!kl_spatial_uuid_for_handle(U(arg), id, &k) || !k) return 0;
    if (kind) *kind = k;
    return 1;
}

// ---- wire structs (natural layout == OVRPlugin.cs sequential) ----------------
typedef struct { int32_t baseTracking; ovrpPosef pose; double time; } ci_anchor;      // 40
typedef struct { uint64_t requestId; int32_t result; int32_t _p; uint64_t space; kl_uuid uuid; } ev_create;      // 40
typedef struct { uint64_t requestId; int32_t result; int32_t _p; uint64_t space; kl_uuid uuid;
                 int32_t component; int32_t enabled; } ev_status;                                               // 48
typedef struct { uint64_t requestId; } ev_qres;                                                                 // 8
typedef struct { uint64_t requestId; int32_t result; int32_t _p; } ev_qdone;                                    // 16
typedef struct { uint64_t requestId; uint64_t space; int32_t result; kl_uuid uuid; } ev_save;                   // 40
typedef struct { uint64_t requestId; int32_t result; kl_uuid uuid; int32_t location; } ev_erase;                // 32
typedef struct { uint64_t requestId; int32_t result; int32_t _p; } ev_simple;                                   // 16
typedef struct { uint64_t space; kl_uuid uuid; } qresult;                                                       // 24
// OVRPlugin.SpaceQueryInfo: the filters are INLINE arrays (SpaceFilterInfoIds {Guid Ids[1024]; int NumIds},
// SpaceFilterInfoComponents {int Components[16]; int NumComponents}), Ids at offset 28, 16488 bytes in all.
#define QINFO_MAX_IDS   1024
#define QINFO_MAX_COMPS 16
typedef struct {
    int32_t QueryType, MaxQuerySpaces; double Timeout;
    int32_t Location, ActionType, FilterType;
    kl_uuid Ids[QINFO_MAX_IDS]; int32_t NumIds;
    int32_t Components[QINFO_MAX_COMPS]; int32_t NumComponents;
} qinfo;
_Static_assert(__builtin_offsetof(qinfo, Ids) == 28 && __builtin_offsetof(qinfo, NumIds) == 16412 &&
               __builtin_offsetof(qinfo, NumComponents) == 16480 && sizeof(qinfo) == 16488, "OVRPlugin.SpaceQueryInfo layout");
typedef struct { int32_t cap, count; void *data; } cap_list;       // {capacityInput, countOutput, ptr}
typedef struct { kl_uuid floor, ceiling; int32_t wcap, wcount; kl_uuid *walls; } room_layout;
typedef struct { float x, y, w, h; } rectf;
typedef struct { float x, y, z, w, h, d; } boundsf;
typedef struct { uint64_t flags; ovrpPosef pose; } space_location;                                             // 40

// Every event starts with its u64 requestId; the i32 result follows it, except EV_QUERY_RESULTS (no result) and
// EV_SAVE_COMPLETE (after the space handle).
static void push(int type, const void *p, size_t n) {
    int rc = kl_ovrp_push_event(type, p, n);
    static unsigned cnt[128];
    if (__atomic_add_fetch(&cnt[type & 127], 1, __ATOMIC_RELAXED) <= SP_PER_EV && kl_ovrp_diag_ok()) {
        uint64_t req = 0;
        int32_t res = 0;
        size_t ro = type == EV_SAVE_COMPLETE ? 16 : 8;
        if (n >= 8) memcpy(&req, p, 8);
        int has = type != EV_QUERY_RESULTS && n >= ro + 4;
        if (has) memcpy(&res, (const char *)p + ro, 4);
        fprintf(stderr, "  [sp-ev] push type=%d req=%llu result=%d queued=%d\n", type, ULL(req), has ? (int)res : 0, rc == 0);
    }
    if (rc != 0) ONCE("event queue full: a spatial event was dropped");
}

// ---- matrices <-> poses -----------------------------------------------------
static void m_to_pose(const float *m, ovrpPosef *p) {
    float t = m[0] + m[5] + m[10], x, y, z, w;
    if (t > 0) { float s = sqrtf(t + 1.0f) * 2; w = 0.25f * s; x = (m[6] - m[9]) / s; y = (m[8] - m[2]) / s; z = (m[1] - m[4]) / s; }
    else if (m[0] > m[5] && m[0] > m[10]) { float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2; w = (m[6] - m[9]) / s; x = 0.25f * s; y = (m[4] + m[1]) / s; z = (m[8] + m[2]) / s; }
    else if (m[5] > m[10]) { float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2; w = (m[8] - m[2]) / s; x = (m[4] + m[1]) / s; y = 0.25f * s; z = (m[9] + m[6]) / s; }
    else { float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2; w = (m[1] - m[4]) / s; x = (m[8] + m[2]) / s; y = (m[9] + m[6]) / s; z = 0.25f * s; }
    p->Orientation.x = x; p->Orientation.y = y; p->Orientation.z = z; p->Orientation.w = w;
    p->Position.x = m[12]; p->Position.y = m[13]; p->Position.z = m[14];
}
static void pose_to_m(const ovrpPosef *p, float *m) {
    float x = p->Orientation.x, y = p->Orientation.y, z = p->Orientation.z, w = p->Orientation.w;
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n < 1e-6f) { x = y = z = 0; w = 1; } else { x /= n; y /= n; z /= n; w /= n; }
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);     m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);     m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p->Position.x; m[13] = p->Position.y; m[14] = p->Position.z; m[15] = 1;
}

// ---- component / label tables -------------------------------------------------
static int label_has_volume(int l) { return l == KLSP_LBL_TABLE || l == KLSP_LBL_COUCH; }
static const char *label_name(int l) {
    switch (l) {
        case KLSP_LBL_FLOOR: return "FLOOR";
        case KLSP_LBL_CEILING: return "CEILING";
        case KLSP_LBL_WALL: return "WALL_FACE";
        case KLSP_LBL_TABLE: return "TABLE";
        case KLSP_LBL_COUCH: return "COUCH";
        case KLSP_LBL_DOOR: return "DOOR_FRAME";
        case KLSP_LBL_WINDOW: return "WINDOW_FRAME";
        default: return "OTHER";
    }
}
static int plane_label(kl_uuid id) {
    int l = KLSP_LBL_OTHER;
    kl_spatial_plane_info(id, NULL, NULL, &l, 0, NULL, NULL);
    return l;
}
// Components an entity supports; returns the count (<= 8).
static int comps_of(kl_uuid id, int kind, int32_t out[8]) {
    int n = 0;
    out[n++] = C_LOCATABLE;
    switch (kind) {
        case KLSP_ANCHOR: out[n++] = C_STORABLE; break;
        case KLSP_ROOM: out[n++] = C_ROOMLAYOUT; out[n++] = C_CONTAINER; break;
        case KLSP_MESH: out[n++] = C_TRIMESH; break;
        case KLSP_PLANE:
            out[n++] = C_BOUNDED2D; out[n++] = C_LABELS;
            if (label_has_volume(plane_label(id))) out[n++] = C_BOUNDED3D;
            break;
        default: break;
    }
    return n;
}
static int comp_supported(kl_uuid id, int kind, int32_t type) {
    int32_t c[8];
    int n = comps_of(id, kind, c);
    if (type == C_TRIMESH_ALT) type = C_TRIMESH;
    for (int i = 0; i < n; i++) if (c[i] == type) return 1;
    return 0;
}
// kl_spatial hands planes out in the ARKit anchor frame (normal +Y); the OVR/Meta plane has its
// rectangle in XY with normal +Z: Rx(-90deg) on the right (same as kl_openxr_spatial.c plane_to_meta).
static void plane_to_meta(float m[16]) {
    float r[16], o[16] = {1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * o[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(m, r, sizeof r);
}
static int pose_of(kl_uuid id, int kind, float m[16]) {
    uint32_t n = kl_spatial_count(kind);
    for (uint32_t i = 0; i < n; i++) {
        kl_uuid u;
        if (kl_spatial_get(kind, i, &u, m) && !memcmp(u.b, id.b, 16)) {
            if (kind == KLSP_PLANE) plane_to_meta(m);
            return 1;
        }
    }
    return 0;
}
// Depth of a volume plane (table/couch): distance down to the lowest floor, else a default.
static float volume_depth(int label, const float *plane_m) {
    float fy = 1e30f;
    uint32_t n = kl_spatial_count(KLSP_PLANE);
    for (uint32_t i = 0; i < n; i++) {
        kl_uuid u; float m[16];
        if (kl_spatial_get(KLSP_PLANE, i, &u, m) && plane_label(u) == KLSP_LBL_FLOOR && m[13] < fy) fy = m[13];
    }
    float d = fy < 1e29f ? plane_m[13] - fy : (label == KLSP_LBL_COUCH ? 0.45f : 0.75f);
    return d < 0.1f ? 0.1f : (d > 1.5f ? 1.5f : d);
}

// ---- pending async requests ----------------------------------------------------
enum { P_CREATE = 1, P_ERASE = 2 };
typedef struct { uint64_t req; int kind; int32_t location; } fpend;
static fpend g_pend[64];
static unsigned g_npend;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void pend_add(uint64_t req, int kind, int32_t loc) {
    pthread_mutex_lock(&g_mu);
    if (g_npend == 64) g_pend[0] = g_pend[--g_npend];
    g_pend[g_npend].req = req; g_pend[g_npend].kind = kind; g_pend[g_npend].location = loc; g_npend++;
    pthread_mutex_unlock(&g_mu);
}
static int pend_take(uint64_t req, fpend *out) {
    int r = 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < g_npend; i++) if (g_pend[i].req == req) { *out = g_pend[i]; g_pend[i] = g_pend[--g_npend]; r = 1; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
static int g_listening;
static void on_done(uint64_t req, int ok, kl_uuid id) {
    fpend p;
    if (!pend_take(req, &p)) return;               // not ours (xr facade / fire-and-forget)
    if (p.kind == P_CREATE) {
        ev_create e; memset(&e, 0, sizeof e);
        e.requestId = req; e.result = ok ? KLOVRP_SUCCESS : KLOVRP_OPERATION_FAILED;
        if (ok) { e.space = kl_spatial_handle_for(id); e.uuid = id; }
        push(EV_ANCHOR_CREATE, &e, sizeof e);
    } else {
        ev_erase e; memset(&e, 0, sizeof e);
        e.requestId = req; e.result = ok ? KLOVRP_SUCCESS : KLOVRP_OPERATION_FAILED; e.uuid = id; e.location = p.location;
        push(EV_ERASE_COMPLETE, &e, sizeof e);
    }
}
static void ensure_listener(void) {
    if (!g_listening) { g_listening = 1; kl_spatial_add_listener(on_done); }
}

// ---- spaces: anchors ---------------------------------------------------------------
SP_IMPL sp_CreateSpatialAnchor(const ci_anchor *info, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_CreateSpatialAnchor");
    if (!info || !req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    if (!kl_spatial_available(KLSP_CAP_ANCHORS)) { ONCE("CreateSpatialAnchor: anchors unavailable (no world sensing)"); return KLOVRP_UNSUPPORTED; }
    ensure_listener();
    float m[16];
    pose_to_m(&info->pose, m);
    uint64_t req = kl_spatial_new_request();
    pend_add(req, P_CREATE, 0);
    if (kl_spatial_create_anchor(req, m) != 0) { fpend p; pend_take(req, &p); return KLOVRP_OPERATION_FAILED; }
    *req_out = req;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_CreateSpatialAnchor(const ci_anchor *info, uint64_t *req_out) {
    ovrpResult r = sp_CreateSpatialAnchor(info, req_out);
    SPT("ovrp_CreateSpatialAnchor", r, "req=%llu", ULL(req_out ? *req_out : 0));
    return r;
}

// Last good pose per handle. A scene capture is a snapshot on a Quest: a handle already returned stays valid, but ARKit
// retires planes from the live registry, and the game keeps locating the handles it holds. Bounded; a handle past the
// limit is just not remembered (zeros, as before). Destroy/Erase of a handle drops its entry. Handles are never reused, so an entry never goes stale for another space.
#define SP_LASTPOSE_MAX 8192                  // = the handle limit of kl_spatial.c (KLSP_MAX_HANDLES)
static struct { uint64_t h; ovrpPosef p; } g_lp[SP_LASTPOSE_MAX];
static unsigned g_nlp;
static pthread_mutex_t g_lp_mu = PTHREAD_MUTEX_INITIALIZER;
static void lp_put(uint64_t h, const ovrpPosef *p) {
    pthread_mutex_lock(&g_lp_mu);
    unsigned i = 0;
    while (i < g_nlp && g_lp[i].h != h) i++;
    if (i < SP_LASTPOSE_MAX) { g_lp[i].h = h; g_lp[i].p = *p; if (i == g_nlp) g_nlp++; }
    pthread_mutex_unlock(&g_lp_mu);
}
static int lp_get(uint64_t h, ovrpPosef *p) {
    int found = 0;
    pthread_mutex_lock(&g_lp_mu);
    for (unsigned i = 0; i < g_nlp; i++) if (g_lp[i].h == h) { *p = g_lp[i].p; found = 1; break; }
    pthread_mutex_unlock(&g_lp_mu);
    return found;
}
static void lp_del(uint64_t h) {
    pthread_mutex_lock(&g_lp_mu);
    for (unsigned i = 0; i < g_nlp; i++) if (g_lp[i].h == h) { g_lp[i] = g_lp[--g_nlp]; break; }
    pthread_mutex_unlock(&g_lp_mu);
}
static void cs_clear(uint64_t space);
SP_IMPL sp_DestroySpace(uint64_t space) {
    kl_ovrp_hit("ovrp_DestroySpace");
    lp_del(sp_h(space));                                           // also for a plane ARKit already retired: the game let go of it
    kl_uuid id; int kind = 0;
    if (!space_of(space, &id, &kind)) return KLOVRP_SUCCESS;       // already gone: nothing to free
    cs_clear(kl_spatial_handle_for(id));
    // The handle is a stable name for a uuid; there is nothing to release. An
    // anchor that was never saved dies with its handle (Quest: anchors are
    // ephemeral until SaveSpace); a saved one stays until EraseSpace.
    if (kind == KLSP_ANCHOR && !kl_spatial_anchor_saved(id)) {
        ensure_listener();
        (void)kl_spatial_remove_anchor(kl_spatial_new_request(), id);
    }
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_DestroySpace(uint64_t space) {
    ovrpResult r = sp_DestroySpace(space);
    SPT("ovrp_DestroySpace", r, "space=0x%llx", ULL(sp_h(space)));
    return r;
}

static void save_one_event(uint64_t req, kl_uuid id, int ok) {
    ev_save e; memset(&e, 0, sizeof e);
    e.requestId = req; e.uuid = id; e.space = kl_spatial_handle_for(id);
    e.result = ok ? KLOVRP_SUCCESS : KLOVRP_UNSUPPORTED;
    push(EV_SAVE_COMPLETE, &e, sizeof e);
}
static int save_space(kl_uuid id, int kind, int32_t location) {
    if (kind != KLSP_ANCHOR || !IS_LOCAL_LOC(location)) return 0;
    kl_spatial_anchor_set_saved(id, 1);          // WorldAnchors persist by themselves; this records "this game's"
    return 1;
}
SP_IMPL sp_SaveSpace(uint64_t space, int32_t location, int32_t mode, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_SaveSpace");
    (void)mode;
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind)) return KLOVRP_INVALID_PARAM;
    uint64_t req = kl_spatial_new_request();
    save_one_event(req, id, save_space(id, kind, location));
    *req_out = req;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_SaveSpace(uint64_t space, int32_t location, int32_t mode, uint64_t *req_out) {
    ovrpResult r = sp_SaveSpace(space, location, mode, req_out);
    SPT("ovrp_SaveSpace", r, "space=0x%llx loc=%d mode=%d req=%llu", ULL(sp_h(space)), location, mode, ULL(req_out ? *req_out : 0));
    return r;
}
static ovrpResult save_list(const uint64_t *spaces, uint32_t n, int32_t location, uint64_t *req_out) {
    if (!spaces || !req_out || n > 1024) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    uint64_t req = kl_spatial_new_request();
    int ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        kl_uuid id; int kind;
        if (!space_of((uint64_t)(uintptr_t)&spaces[i], &id, &kind) || !save_space(id, kind, location)) ok = 0;
    }
    ev_simple e; memset(&e, 0, sizeof e);
    e.requestId = req; e.result = ok ? KLOVRP_SUCCESS : KLOVRP_UNSUPPORTED;
    push(EV_LIST_SAVE, &e, sizeof e);
    *req_out = req;
    return KLOVRP_SUCCESS;
}
// ovrp_SaveSpaceList(spaces*, n, location, out requestId)
SP_IMPL sp_SaveSpaceList(const uint64_t *spaces, uint32_t n, int32_t location, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_SaveSpaceList");
    return save_list(spaces, n, location, req_out);
}
static ovrpResult klovrp_SaveSpaceList(const uint64_t *spaces, uint32_t n, int32_t location, uint64_t *req_out) {
    ovrpResult r = sp_SaveSpaceList(spaces, n, location, req_out);
    SPT("ovrp_SaveSpaceList", r, "n=%u loc=%d req=%llu", n, location, ULL(req_out ? *req_out : 0));
    return r;
}
// ovrp_SaveSpaces(n, spaces*, [location,] out requestId): the out pointer is the last
// pointer-sized argument (the optional location is a small int, never a pointer).
SP_IMPL sp_SaveSpaces(uint32_t n, const uint64_t *spaces, uint64_t a2, uint64_t a3) {
    kl_ovrp_hit("ovrp_SaveSpaces");
    // ovrp_SaveSpaces(uint n, ulong *spaces, out ulong requestId): a2 is the out pointer; a3 is read only when a2 is a
    // location (a stray a3 that looks like a pointer must not be written).
    uint64_t *out = isptr(a2) ? (uint64_t *)(uintptr_t)a2 : (isptr(a3) ? (uint64_t *)(uintptr_t)a3 : NULL);
    int32_t loc = isptr(a2) ? STORAGE_LOCAL : (int32_t)a2;
    return save_list(spaces, n, loc, out);
}
static ovrpResult klovrp_SaveSpaces(uint32_t n, const uint64_t *spaces, uint64_t a2, uint64_t a3) {
    ovrpResult r = sp_SaveSpaces(n, spaces, a2, a3);
    SPT("ovrp_SaveSpaces", r, "n=%u req=%llu", n, ULL(sp_out(a2, a3)));
    return r;
}

static ovrpResult erase_one(kl_uuid id, int kind, int32_t location, uint64_t req) {
    ensure_listener();
    if (kind != KLSP_ANCHOR || !IS_LOCAL_LOC(location)) {
        ev_erase e; memset(&e, 0, sizeof e);
        e.requestId = req; e.result = KLOVRP_UNSUPPORTED; e.uuid = id; e.location = location;
        push(EV_ERASE_COMPLETE, &e, sizeof e);
        return KLOVRP_SUCCESS;
    }
    pend_add(req, P_ERASE, location);
    if (kl_spatial_remove_anchor(req, id) != 0) {
        fpend p; pend_take(req, &p);
        ev_erase e; memset(&e, 0, sizeof e);
        e.requestId = req; e.result = KLOVRP_OPERATION_FAILED; e.uuid = id; e.location = location;
        push(EV_ERASE_COMPLETE, &e, sizeof e);
    }
    return KLOVRP_SUCCESS;
}
SP_IMPL sp_EraseSpace(uint64_t space, int32_t location, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_EraseSpace");
    lp_del(sp_h(space));
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind)) return KLOVRP_INVALID_PARAM;
    uint64_t req = kl_spatial_new_request();
    *req_out = req;
    cs_clear(kl_spatial_handle_for(id));
    return erase_one(id, kind, location, req);
}
static ovrpResult klovrp_EraseSpace(uint64_t space, int32_t location, uint64_t *req_out) {
    ovrpResult r = sp_EraseSpace(space, location, req_out);
    SPT("ovrp_EraseSpace", r, "space=0x%llx loc=%d req=%llu", ULL(sp_h(space)), location, ULL(req_out ? *req_out : 0));
    return r;
}
// ovrp_EraseSpaces(spaceCount, spaces*, uuidCount, uuids*, out requestId). The older
// shape (n, spaces*, uuids*, location, out) is recognised by a pointer in a2.
// One request, one completion event (for the first target).
SP_IMPL sp_EraseSpaces(uint32_t ns, const uint64_t *spaces, uint64_t a2, uint64_t a3, uint64_t a4) {
    kl_ovrp_hit("ovrp_EraseSpaces");
    uint64_t *out = (uint64_t *)(uintptr_t)a4;
    if (!isptr(a4)) return KLOVRP_INVALID_PARAM;
    uint32_t nu = 0; const kl_uuid *uu = NULL; int32_t loc = STORAGE_LOCAL;
    if (isptr(a2)) { uu = (const kl_uuid *)(uintptr_t)a2; nu = 0; loc = (int32_t)a3; }       // old shape: uuid count unknown
    else { nu = (uint32_t)a2; uu = isptr(a3) ? (const kl_uuid *)(uintptr_t)a3 : NULL; }
    if (ns > 1024 || nu > 1024) return KLOVRP_INVALID_PARAM;
    *out = 0;
    for (uint32_t i = 0; spaces && i < ns; i++) lp_del(sp_h((uint64_t)(uintptr_t)&spaces[i]));
    uint64_t req = kl_spatial_new_request();
    kl_uuid first; int kind = 0, have = 0;
    for (uint32_t i = 0; i < ns && !have && spaces; i++)
        if (space_of((uint64_t)(uintptr_t)&spaces[i], &first, &kind)) have = 1;
    if (!have && nu && uu) { first = uu[0]; kind = kl_spatial_kind_of(first); have = 1; }
    if (!have) return KLOVRP_INVALID_PARAM;
    // Remaining targets: fire and forget (the request's single event names the first).
    ensure_listener();
    for (uint32_t i = 0; i < ns && spaces; i++) {
        kl_uuid id; int k;
        if (!space_of((uint64_t)(uintptr_t)&spaces[i], &id, &k) || k != KLSP_ANCHOR || !memcmp(id.b, first.b, 16)) continue;
        (void)kl_spatial_remove_anchor(kl_spatial_new_request(), id);
    }
    for (uint32_t i = 0; i < nu && uu; i++)
        if (memcmp(uu[i].b, first.b, 16) && kl_spatial_kind_of(uu[i]) == KLSP_ANCHOR)
            (void)kl_spatial_remove_anchor(kl_spatial_new_request(), uu[i]);
    *out = req;
    return erase_one(first, kind, loc, req);
}
static ovrpResult klovrp_EraseSpaces(uint32_t ns, const uint64_t *spaces, uint64_t a2, uint64_t a3, uint64_t a4) {
    ovrpResult r = sp_EraseSpaces(ns, spaces, a2, a3, a4);
    SPT("ovrp_EraseSpaces", r, "spaces=%u req=%llu", ns, ULL(isptr(a4) ? *(const uint64_t *)(uintptr_t)a4 : 0));
    return r;
}

// ---- spaces: locate / identity ------------------------------------------------------
// ovrp_LocateSpace(ref Posef location, ref UInt64 space, TrackingOrigin). The plan's
// order (space, origin, out) is accepted too: there the first argument is not a pointer.
// What one locate did, for the [sp-call] line and the [sp-sum] / [sp-loc-bad] counters. why: 0 none, 1 space, 2 pose, 3 cached.
typedef struct { uint64_t h, flags; int ok, why, kind; int32_t origin; ovrpPosef p; } sp_loc;
static const char *const sp_why_s[4] = {"none", "space", "pose", "cached"};
#define SP_LOC_FMT "space=0x%llx ok=%d why=%s kind=%d flags=0x%llx p=(%.2f,%.2f,%.2f) q=(%.3f,%.3f,%.3f,%.3f) origin=%d"
#define SP_LOC_ARGS(l) ULL((l).h), (l).ok, sp_why_s[(l).why], (l).kind, ULL((l).flags), \
        (l).p.Position.x, (l).p.Position.y, (l).p.Position.z, \
        (l).p.Orientation.x, (l).p.Orientation.y, (l).p.Orientation.z, (l).p.Orientation.w, (int)(l).origin
#define SP_LOCBAD_MAX 10
// A failed locate: counted, and the first SP_LOCBAD_MAX distinct handles get one [sp-loc-bad] line (not budgeted).
static void sp_loc_bad(const sp_loc *li) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t seen[SP_LOCBAD_MAX];
    static unsigned n;
    __atomic_add_fetch(&g_sp_loc[li->why], 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&n, __ATOMIC_RELAXED) >= SP_LOCBAD_MAX) return;
    pthread_mutex_lock(&mu);
    unsigned i = 0;
    while (i < n && seen[i] != li->h) i++;
    int fresh = i == n && n < SP_LOCBAD_MAX;
    if (fresh) { seen[n] = li->h; __atomic_store_n(&n, n + 1, __ATOMIC_RELAXED); }
    pthread_mutex_unlock(&mu);
    if (fresh) fprintf(stderr, "  [sp-loc-bad] space=0x%llx kind=%d why=%s\n", ULL(li->h), li->kind, sp_why_s[li->why]);
}
static ovrpResult locate(uint64_t a0, uint64_t a1, uint64_t a2, int v2, sp_loc *li) {
    uint64_t out, space;
    memset(li, 0, sizeof *li);
    if (isptr(a0)) { out = a0; space = a1; li->origin = (int32_t)a2; } else { space = a0; out = a2; li->origin = (int32_t)a1; }
    li->h = sp_h(space);
    if (!out || !space) return KLOVRP_INVALID_PARAM;
    kl_uuid id; int kind = 0;
    float m[16];
    space_location *loc = (space_location *)(uintptr_t)out;
    int sok = space_of(space, &id, &kind);
    if (!sok || !pose_of(id, kind, m)) {
        li->why = sok ? 2 : 1; li->kind = kind;
        ovrpPosef cp;
        if (lp_get(li->h, &cp)) {                       // known handle, plane gone from the registry: its last pose, not tracked
            li->why = 3; li->p = cp;                    // ok stays 0: a substituted value, not a live locate
            if (v2) { loc->flags = 0x3ull; loc->pose = cp; li->flags = loc->flags; }
            else *(ovrpPosef *)(uintptr_t)out = cp;
            __atomic_add_fetch(&g_sp_loc[3], 1, __ATOMIC_RELAXED);
            return KLOVRP_SUCCESS;
        }
        sp_loc_bad(li);
        if (v2) { memset(loc, 0, sizeof *loc); return KLOVRP_SUCCESS; }
        return KLOVRP_OPERATION_FAILED;
    }
    int tracked = kind != KLSP_ANCHOR || kl_spatial_anchor_tracked(id);
    ovrpPosef p; m_to_pose(m, &p);
    if (v2) { loc->flags = tracked ? 0xFull : 0x3ull; loc->pose = p; }
    else *(ovrpPosef *)(uintptr_t)out = p;
    if (kind != KLSP_ANCHOR) lp_put(li->h, &p);           // anchors are not remembered: a destroyed/erased one keeps failing
    li->ok = 1; li->kind = kind; li->p = p;
    if (v2) li->flags = loc->flags;
    __atomic_add_fetch(&g_sp_loc[0], 1, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_LocateSpace(uint64_t a0, uint64_t a1, uint64_t a2) {
    kl_ovrp_hit("ovrp_LocateSpace");
    sp_loc li;
    ovrpResult r = locate(a0, a1, a2, 0, &li);
    SPT("ovrp_LocateSpace", r, SP_LOC_FMT, SP_LOC_ARGS(li));
    return r;
}
static ovrpResult klovrp_LocateSpace2(uint64_t a0, uint64_t a1, uint64_t a2) {
    kl_ovrp_hit("ovrp_LocateSpace2");
    sp_loc li;
    ovrpResult r = locate(a0, a1, a2, 1, &li);
    SPT("ovrp_LocateSpace2", r, SP_LOC_FMT, SP_LOC_ARGS(li));
    return r;
}
SP_IMPL sp_GetSpaceUuid(uint64_t space, kl_uuid *out) {
    kl_ovrp_hit("ovrp_GetSpaceUuid");
    if (!out) return KLOVRP_INVALID_PARAM;
    kl_uuid id;
    if (!space_of(space, &id, NULL)) { memset(out, 0, sizeof *out); return KLOVRP_INVALID_PARAM; }
    *out = id;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceUuid(uint64_t space, kl_uuid *out) {
    ovrpResult r = sp_GetSpaceUuid(space, out);
    SPT("ovrp_GetSpaceUuid", r, "space=0x%llx", ULL(sp_h(space)));
    return r;
}

// Space users: opaque, no accounts (no network).
static uint64_t g_users[16];
static ovrpResult klovrp_CreateSpaceUser(uint64_t user_id, uint64_t *out) {
    kl_ovrp_hit("ovrp_CreateSpaceUser");
    if (!out) return KLOVRP_INVALID_PARAM;
    uint64_t uid = U(user_id);
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!g_users[i] && slot < 0) slot = i;
    if (slot >= 0) g_users[slot] = uid ? uid : 1;
    pthread_mutex_unlock(&g_mu);
    if (slot < 0) { *out = 0; return KLOVRP_OPERATION_FAILED; }
    *out = 0x5000 + (uint64_t)slot;           // < 4 GB, outside the kl_spatial handle range
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceUserId(uint64_t user, uint64_t *out) {
    kl_ovrp_hit("ovrp_GetSpaceUserId");
    if (!out) return KLOVRP_INVALID_PARAM;
    uint64_t h = U(user);
    *out = 0;
    if (h < 0x5000 || h >= 0x5010) return KLOVRP_INVALID_PARAM;
    pthread_mutex_lock(&g_mu); *out = g_users[h - 0x5000]; pthread_mutex_unlock(&g_mu);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_DestroySpaceUser(uint64_t user) {
    kl_ovrp_hit("ovrp_DestroySpaceUser");
    uint64_t h = U(user);
    if (h >= 0x5000 && h < 0x5010) { pthread_mutex_lock(&g_mu); g_users[h - 0x5000] = 0; pthread_mutex_unlock(&g_mu); }
    return KLOVRP_SUCCESS;
}

// ---- components -------------------------------------------------------------------
SP_IMPL sp_EnumerateSpaceSupportedComponents(uint64_t space, uint32_t cap, uint32_t *count, int32_t *types) {
    kl_ovrp_hit("ovrp_EnumerateSpaceSupportedComponents");
    if (!count) return KLOVRP_INVALID_PARAM;
    *count = 0;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind)) return KLOVRP_INVALID_PARAM;
    int32_t c[8];
    int n = comps_of(id, kind, c);
    *count = (uint32_t)n;
    if (cap && types) for (int i = 0; i < n && (uint32_t)i < cap; i++) types[i] = c[i];
    if (cap && (uint32_t)n > cap) *count = cap;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_EnumerateSpaceSupportedComponents(uint64_t space, uint32_t cap, uint32_t *count, int32_t *types) {
    ovrpResult r = sp_EnumerateSpaceSupportedComponents(space, cap, count, types);
    SPT("ovrp_EnumerateSpaceSupportedComponents", r, "space=0x%llx cap=%u count=%u", ULL(sp_h(space)), cap, count ? *count : 0u);
    return r;
}
// Requested enabled state per (space handle, component), set by ovrp_SetSpaceComponentStatus. Ring of 256.
#define CS_MAX 256
static struct { uint64_t space; int32_t type; int32_t on; } g_cs[CS_MAX];
static unsigned g_cs_n, g_cs_next;
static void cs_clear(uint64_t space) {
    if (!space) return;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < g_cs_n; ) {
        if (g_cs[i].space == space) g_cs[i] = g_cs[--g_cs_n]; else i++;
    }
    if (g_cs_next >= g_cs_n) g_cs_next = 0;
    pthread_mutex_unlock(&g_mu);
}
static int cs_get(uint64_t space, int32_t type, int32_t *on) {
    int r = 0;
    if (!space) return 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < g_cs_n; i++) if (g_cs[i].space == space && g_cs[i].type == type) { *on = g_cs[i].on; r = 1; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
static void cs_set(uint64_t space, int32_t type, int32_t on) {
    if (!space) return;
    pthread_mutex_lock(&g_mu);
    unsigned i;
    for (i = 0; i < g_cs_n; i++) if (g_cs[i].space == space && g_cs[i].type == type) break;
    if (i == g_cs_n) { if (g_cs_n < CS_MAX) g_cs_n++; else i = g_cs_next++ % CS_MAX; if (i >= g_cs_n) i = g_cs_n - 1; }
    g_cs[i].space = space; g_cs[i].type = type; g_cs[i].on = on;
    pthread_mutex_unlock(&g_mu);
}
SP_IMPL sp_GetSpaceComponentStatus(uint64_t space, int32_t type, ovrpBool *enabled, ovrpBool *pending) {
    kl_ovrp_hit("ovrp_GetSpaceComponentStatus");
    if (enabled) *enabled = 0;
    if (pending) *pending = 0;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind)) return KLOVRP_INVALID_PARAM;
    int32_t on = 0;
    if (enabled) *enabled = comp_supported(id, kind, type) ? (cs_get(kl_spatial_handle_for(id), type, &on) ? on : 1) : 0;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceComponentStatus(uint64_t space, int32_t type, ovrpBool *enabled, ovrpBool *pending) {
    ovrpResult r = sp_GetSpaceComponentStatus(space, type, enabled, pending);
    SPT("ovrp_GetSpaceComponentStatus", r, "space=0x%llx comp=%d enabled=%d", ULL(sp_h(space)), type, enabled ? (int)*enabled : -1);
    return r;
}
// (space, type, enable, double timeout, out requestId): the double travels in d0.
SP_IMPL sp_SetSpaceComponentStatus(uint64_t space, int32_t type, ovrpBool enable, double timeout, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_SetSpaceComponentStatus");
    (void)timeout;
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind)) return KLOVRP_INVALID_PARAM;
    uint64_t req = kl_spatial_new_request();
    int sup = comp_supported(id, kind, type);
    ev_status e; memset(&e, 0, sizeof e);
    e.requestId = req; e.space = kl_spatial_handle_for(id); e.uuid = id; e.component = type;
    e.result = sup ? KLOVRP_SUCCESS : KLOVRP_UNSUPPORTED;
    if (sup) cs_set(e.space, type, enable ? 1 : 0);
    e.enabled = sup && enable ? 1 : 0;
    push(EV_SET_STATUS, &e, sizeof e);
    *req_out = req;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_SetSpaceComponentStatus(uint64_t space, int32_t type, ovrpBool enable, double timeout, uint64_t *req_out) {
    ovrpResult r = sp_SetSpaceComponentStatus(space, type, enable, timeout, req_out);
    SPT("ovrp_SetSpaceComponentStatus", r, "space=0x%llx comp=%d enable=%d req=%llu", ULL(sp_h(space)), type, (int)enable, ULL(req_out ? *req_out : 0));
    return r;
}

// ---- queries ------------------------------------------------------------------------
#define QRES_MAX 2048
#define QSLOTS 8
typedef struct { uint64_t req; uint32_t n; qresult *r; } qslot;
static qslot g_q[QSLOTS];
static unsigned g_qnext;
// Results held for request `req`, or -1 when none is stored (a trace helper).
static int sp_qcount(uint64_t req) {
    int n = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < QSLOTS; i++) if (g_q[i].req == req && g_q[i].r) n = (int)g_q[i].n;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int in_ids(kl_uuid id, const kl_uuid *ids, int n) {
    for (int i = 0; i < n; i++) if (!memcmp(ids[i].b, id.b, 16)) return 1;
    return 0;
}
static int has_all(kl_uuid id, int kind, const int32_t *want, int nw) {
    for (int i = 0; i < nw; i++) if (!comp_supported(id, kind, want[i])) return 0;
    return 1;
}
// Runs a query on the snapshot; stores the result under `req`. ids/want may be NULL.
static void run_query(uint64_t req, int maxn, const kl_uuid *ids, int nids, const int32_t *want, int nw, int32_t location) {
    qresult *r = (qresult *)calloc(QRES_MAX, sizeof(qresult));
    uint32_t n = 0;
    if (maxn <= 0 || maxn > QRES_MAX) maxn = QRES_MAX;
    static const int kinds[4] = {KLSP_ANCHOR, KLSP_ROOM, KLSP_PLANE, KLSP_MESH};
    if (r && IS_LOCAL_LOC(location))
        for (int ki = 0; ki < 4; ki++) {
            int kind = kinds[ki];
            uint32_t cnt = kl_spatial_count(kind);
            for (uint32_t i = 0; i < cnt && n < (uint32_t)maxn; i++) {
                kl_uuid id;
                if (!kl_spatial_get(kind, i, &id, NULL)) continue;
                if (nids) { if (!in_ids(id, ids, nids)) continue; }
                else {
                    if (kind == KLSP_MESH && !(nw && has_all(id, kind, want, nw))) continue;   // meshes only when asked for
                    if (kind == KLSP_ANCHOR && !kl_spatial_anchor_saved(id)) continue;          // storage queries see saved anchors
                    if (nw && !has_all(id, kind, want, nw)) continue;
                }
                r[n].space = kl_spatial_handle_for(id); r[n].uuid = id; n++;
            }
        }
    pthread_mutex_lock(&g_mu);
    qslot *s = &g_q[g_qnext++ % QSLOTS];
    free(s->r);
    s->req = req; s->n = n; s->r = r;
    pthread_mutex_unlock(&g_mu);
}
static void query_events(uint64_t req) {
    ev_qres a = {req};
    push(EV_QUERY_RESULTS, &a, sizeof a);
    ev_qdone b; memset(&b, 0, sizeof b);
    b.requestId = req; b.result = KLOVRP_SUCCESS;
    push(EV_QUERY_COMPLETE, &b, sizeof b);
}
static int plausible_array(const void *p, int n) { return p && isptr((uint64_t)(uintptr_t)p) && n > 0 && n <= 4096; }

SP_IMPL sp_QuerySpaces(const qinfo *q, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_QuerySpaces");
    if (!q || !req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    const kl_uuid *ids = NULL; int nids = 0; const int32_t *want = NULL; int nw = 0;
    if (q->FilterType == FILTER_IDS && q->NumIds > 0 && q->NumIds <= QINFO_MAX_IDS) { ids = q->Ids; nids = q->NumIds; }
    else if (q->FilterType == FILTER_COMPONENTS && q->NumComponents > 0 && q->NumComponents <= QINFO_MAX_COMPS) { want = q->Components; nw = q->NumComponents; }
    uint64_t req = kl_spatial_new_request();
    run_query(req, q->MaxQuerySpaces, ids, nids, want, nw, q->Location);
    *req_out = req;
    query_events(req);
    return KLOVRP_SUCCESS;
}
// Filter actually requested by a QuerySpaces/QuerySpaces2 call, plus the first 64 bytes of the info struct
// (QuerySpaces2: only its sizeof; first SP_QDUMP calls of each function): what the guest asked for, as opposed to what we answered.
#define SP_QDUMP 6
static void sp_qlog(const char *fn, unsigned *cnt, const void *info, int len, int ft, const void *items, int n, int xt, int xn) {
    if (__atomic_add_fetch(cnt, 1, __ATOMIC_RELAXED) > SP_QDUMP || !kl_ovrp_diag_ok() || !info) return;
    char c[96] = "";
    if (ft == FILTER_COMPONENTS && plausible_array(items, n))
        for (int i = 0, o = 0; i < n && i < 8 && o < (int)sizeof c - 12; i++) o += snprintf(c + o, sizeof c - (size_t)o, "%s%d", i ? "," : "", ((const int32_t *)items)[i]);
    fprintf(stderr, "  [sp-call] fn=%s q#%u filterType=%d %s=%d%s%s excludeFilterType=%d exclude_n=%d\n", fn, *cnt, ft,
            ft == FILTER_IDS ? "ids" : "comps", n, c[0] ? " types=" : "", c, xt, xn);
    const unsigned char *b = (const unsigned char *)info;
    char h[64 * 3 + 1] = "";
    for (int i = 0; i < len && i < 64; i++) snprintf(h + i * 3, 4, "%02x ", b[i]);
    fprintf(stderr, "  [sp-call] fn=%s q#%u info64=%s\n", fn, *cnt, h);
}
static ovrpResult klovrp_QuerySpaces(const qinfo *q, uint64_t *req_out) {
    ovrpResult r = sp_QuerySpaces(q, req_out);
    static unsigned qn;
    if (q) sp_qlog("ovrp_QuerySpaces", &qn, q, 64, q->FilterType, q->FilterType == FILTER_IDS ? (const void *)q->Ids : (const void *)q->Components,
                   q->FilterType == FILTER_IDS ? q->NumIds : q->NumComponents, -1, -1);   // exclude fields: not in this layout (see info64)
    SPT("ovrp_QuerySpaces", r, "req=%llu max=%d filter=%d loc=%d results=%d", ULL(req_out ? *req_out : 0), q ? q->MaxQuerySpaces : -1, q ? q->FilterType : -1, q ? q->Location : -1, req_out ? sp_qcount(*req_out) : -1);
    return r;
}
// SpaceQueryInfo2 {QueryType, Max, Timeout, Location, Action, Filter*, ExcludeFilter*}: the
// filter structs are not documented here; `{i32 type; i32 count; void *items}` is tried and
// anything implausible falls back to "everything".
typedef struct { int32_t QueryType, MaxQuerySpaces; double Timeout; int32_t Location, ActionType; void *Filter; void *Exclude; } qinfo2;
typedef struct { int32_t type, count; void *items; } qfilter2;
SP_IMPL sp_QuerySpaces2(const qinfo2 *q, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_QuerySpaces2");
    if (!q || !req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    const kl_uuid *ids = NULL; int nids = 0; const int32_t *want = NULL; int nw = 0;
    if (isptr((uint64_t)(uintptr_t)q->Filter)) {
        const qfilter2 *f = (const qfilter2 *)q->Filter;
        if (f->type == FILTER_IDS && plausible_array(f->items, f->count)) { ids = (const kl_uuid *)f->items; nids = f->count; }
        else if (f->type == FILTER_COMPONENTS && plausible_array(f->items, f->count)) { want = (const int32_t *)f->items; nw = f->count; }
        else ONCE("QuerySpaces2: unrecognised filter layout, answering with everything");
    }
    uint64_t req = kl_spatial_new_request();
    run_query(req, q->MaxQuerySpaces, ids, nids, want, nw, q->Location);
    *req_out = req;
    query_events(req);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_QuerySpaces2(const qinfo2 *q, uint64_t *req_out) {
    ovrpResult r = sp_QuerySpaces2(q, req_out);
    static unsigned qn;
    if (q) {
        const qfilter2 *f = isptr((uint64_t)(uintptr_t)q->Filter) ? (const qfilter2 *)q->Filter : NULL;
        const qfilter2 *x = isptr((uint64_t)(uintptr_t)q->Exclude) ? (const qfilter2 *)q->Exclude : NULL;
        sp_qlog("ovrp_QuerySpaces2", &qn, q, (int)sizeof *q, f ? f->type : -1, f ? f->items : NULL, f ? f->count : -1, x ? x->type : -1, x ? x->count : -1);
    }
    SPT("ovrp_QuerySpaces2", r, "req=%llu max=%d loc=%d results=%d", ULL(req_out ? *req_out : 0), q ? q->MaxQuerySpaces : -1, q ? q->Location : -1, req_out ? sp_qcount(*req_out) : -1);
    return r;
}
static ovrpResult retrieve(uint64_t req_arg, int32_t cap, int32_t *count, qresult *out) {
    if (!count) return KLOVRP_INVALID_PARAM;
    uint64_t req = U(req_arg);
    *count = 0;
    pthread_mutex_lock(&g_mu);
    qslot *s = NULL;
    for (int i = 0; i < QSLOTS; i++) if (g_q[i].req == req && g_q[i].r) s = &g_q[i];
    if (!s) { pthread_mutex_unlock(&g_mu); return KLOVRP_INVALID_PARAM; }
    if (cap <= 0 || !out) *count = (int32_t)s->n;
    else { uint32_t n = s->n < (uint32_t)cap ? s->n : (uint32_t)cap; memcpy(out, s->r, n * sizeof(qresult)); *count = (int32_t)n; }
    pthread_mutex_unlock(&g_mu);
    return KLOVRP_SUCCESS;
}
SP_IMPL sp_RetrieveSpaceQueryResults(uint64_t req, int32_t cap, int32_t *count, qresult *out) {
    kl_ovrp_hit("ovrp_RetrieveSpaceQueryResults");
    return retrieve(req, cap, count, out);
}
static ovrpResult klovrp_RetrieveSpaceQueryResults(uint64_t req, int32_t cap, int32_t *count, qresult *out) {
    ovrpResult r = sp_RetrieveSpaceQueryResults(req, cap, count, out);
    SPT("ovrp_RetrieveSpaceQueryResults", r, "req=%llu cap=%d count=%d", ULL(U(req)), cap, count ? *count : -1);
    return r;
}
// Discovery (XR_META_spatial_entity_discovery): same store, filters ignored.
// ovrp_DiscoverSpaces(in SpaceDiscoveryInfo {uint NumFilters; Header** Filters}, out ulong requestId): two arguments (a
// third one read as the out pointer is a stray register: the guest never got its id, and 8 bytes went to a wild address).
typedef struct { uint32_t n; uint32_t _p; const void *const *filters; } dinfo;
SP_IMPL sp_DiscoverSpaces(const dinfo *info, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_DiscoverSpaces");
    (void)info;
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    uint64_t req = kl_spatial_new_request();
    run_query(req, QRES_MAX, NULL, 0, NULL, 0, STORAGE_LOCAL);
    *req_out = req;
    ev_qres a = {req};
    push(EV_DISC_RESULTS, &a, sizeof a);
    ev_qdone b; memset(&b, 0, sizeof b);
    b.requestId = req; b.result = KLOVRP_SUCCESS;
    push(EV_DISC_COMPLETE, &b, sizeof b);
    ONCE("DiscoverSpaces: event type numbers are unverified guesses");
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_DiscoverSpaces(const dinfo *info, uint64_t *req_out) {
    ovrpResult r = sp_DiscoverSpaces(info, req_out);
    SPT("ovrp_DiscoverSpaces", r, "filters=%d req=%llu results=%d", info ? (int)info->n : -1, ULL(req_out ? *req_out : 0),
        req_out ? sp_qcount(*req_out) : -1);
    return r;
}
// ovrp_RetrieveSpaceDiscoveryResults(ulong requestId, ref SpaceDiscoveryResults): capacity 0 / no array asks the count.
typedef struct { uint32_t cap, count; qresult *results; } dresults;
SP_IMPL sp_RetrieveSpaceDiscoveryResults(uint64_t req, dresults *res) {
    kl_ovrp_hit("ovrp_RetrieveSpaceDiscoveryResults");
    if (!res) return KLOVRP_INVALID_PARAM;
    int32_t n = 0;
    ovrpResult r = retrieve(req, res->results ? (int32_t)res->cap : 0, &n, res->results);
    res->count = (uint32_t)n;
    return r;
}
static ovrpResult klovrp_RetrieveSpaceDiscoveryResults(uint64_t req, dresults *res) {
    ovrpResult r = sp_RetrieveSpaceDiscoveryResults(req, res);
    SPT("ovrp_RetrieveSpaceDiscoveryResults", r, "req=%llu cap=%d count=%d", ULL(req), res ? (int)res->cap : -1,
        res ? (int)res->count : -1);
    return r;
}

// ---- per-space data -------------------------------------------------------------
SP_IMPL sp_GetSpaceBoundingBox2D(uint64_t space, rectf *out) {
    kl_ovrp_hit("ovrp_GetSpaceBoundingBox2D");
    if (!out) return KLOVRP_INVALID_PARAM;
    memset(out, 0, sizeof *out);
    kl_uuid id; int kind; float w, h;
    if (!space_of(space, &id, &kind) || kind != KLSP_PLANE || !kl_spatial_plane_info(id, &w, &h, NULL, 0, NULL, NULL))
        return KLOVRP_INVALID_PARAM;
    out->x = -w * 0.5f; out->y = -h * 0.5f; out->w = w; out->h = h;     // offset = the corner, as XrRect2Df
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceBoundingBox2D(uint64_t space, rectf *out) {
    ovrpResult r = sp_GetSpaceBoundingBox2D(space, out);
    SPT("ovrp_GetSpaceBoundingBox2D", r, "space=0x%llx w=%.3f h=%.3f", ULL(sp_h(space)), out ? out->w : 0.0f, out ? out->h : 0.0f);
    return r;
}
SP_IMPL sp_GetSpaceBoundingBox3D(uint64_t space, boundsf *out) {
    kl_ovrp_hit("ovrp_GetSpaceBoundingBox3D");
    if (!out) return KLOVRP_INVALID_PARAM;
    memset(out, 0, sizeof *out);
    kl_uuid id; int kind; float w, h; int label;
    if (!space_of(space, &id, &kind) || kind != KLSP_PLANE || !kl_spatial_plane_info(id, &w, &h, &label, 0, NULL, NULL))
        return KLOVRP_INVALID_PARAM;
    float depth = 0;
    if (label_has_volume(label)) { float m[16]; if (pose_of(id, kind, m)) depth = volume_depth(label, m); }
    // The volume hangs below the plane: the plane is its +Z face.
    out->x = -w * 0.5f; out->y = -h * 0.5f; out->z = -depth; out->w = w; out->h = h; out->d = depth;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceBoundingBox3D(uint64_t space, boundsf *out) {
    ovrpResult r = sp_GetSpaceBoundingBox3D(space, out);
    SPT("ovrp_GetSpaceBoundingBox3D", r, "space=0x%llx w=%.3f h=%.3f d=%.3f", ULL(sp_h(space)), out ? out->w : 0.0f, out ? out->h : 0.0f, out ? out->d : 0.0f);
    return r;
}
SP_IMPL sp_GetSpaceSemanticLabels(uint64_t space, cap_list *out) {
    kl_ovrp_hit("ovrp_GetSpaceSemanticLabels");
    if (!out) return KLOVRP_INVALID_PARAM;
    kl_uuid id; int kind;
    if (!space_of(space, &id, &kind) || kind != KLSP_PLANE) { out->count = 0; return KLOVRP_INVALID_PARAM; }
    const char *s = label_name(plane_label(id));
    int n = (int)strlen(s) + 1;                                           // with the NUL, as the OpenXR twin
    if (out->cap > 0 && out->data) { int c = out->cap < n ? out->cap : n; memcpy(out->data, s, (size_t)c); ((char *)out->data)[c - 1] = 0; out->count = c; }
    else out->count = n;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceSemanticLabels(uint64_t space, cap_list *out) {
    ovrpResult r = sp_GetSpaceSemanticLabels(space, out);
    SPT("ovrp_GetSpaceSemanticLabels", r, "space=0x%llx cap=%d count=%d", ULL(sp_h(space)), out ? out->cap : -1, out ? out->count : -1);
    return r;
}
SP_IMPL sp_GetSpaceBoundary2D(uint64_t space, cap_list *out) {
    kl_ovrp_hit("ovrp_GetSpaceBoundary2D");
    if (!out) return KLOVRP_INVALID_PARAM;
    kl_uuid id; int kind; uint32_t np = 0;
    if (!space_of(space, &id, &kind) || kind != KLSP_PLANE) { out->count = 0; return KLOVRP_INVALID_PARAM; }
    if (out->cap > 0 && out->data) {
        kl_spatial_plane_info(id, NULL, NULL, NULL, (uint32_t)out->cap, (float *)out->data, &np);   // ovrpVector2f == 2 floats
        out->count = (int32_t)(np < (uint32_t)out->cap ? np : (uint32_t)out->cap);
    } else {
        kl_spatial_plane_info(id, NULL, NULL, NULL, 0, NULL, &np);
        out->count = (int32_t)np;
    }
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceBoundary2D(uint64_t space, cap_list *out) {
    ovrpResult r = sp_GetSpaceBoundary2D(space, out);
    SPT("ovrp_GetSpaceBoundary2D", r, "space=0x%llx cap=%d count=%d", ULL(sp_h(space)), out ? out->cap : -1, out ? out->count : -1);
    return r;
}
SP_IMPL sp_GetSpaceRoomLayout(uint64_t space, room_layout *out) {
    kl_ovrp_hit("ovrp_GetSpaceRoomLayout");
    if (!out) return KLOVRP_INVALID_PARAM;
    kl_uuid id; int kind; uint32_t nw = 0;
    if (!space_of(space, &id, &kind) || kind != KLSP_ROOM) return KLOVRP_INVALID_PARAM;
    uint32_t cap = (out->wcap > 0 && out->walls) ? (uint32_t)out->wcap : 0;
    if (!kl_spatial_room_info(id, &out->floor, &out->ceiling, cap, cap ? out->walls : NULL, &nw, 0, NULL, NULL)) return KLOVRP_OPERATION_FAILED;
    out->wcount = (int32_t)(cap && nw > cap ? cap : nw);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceRoomLayout(uint64_t space, room_layout *out) {
    ovrpResult r = sp_GetSpaceRoomLayout(space, out);
    SPT("ovrp_GetSpaceRoomLayout", r, "space=0x%llx wcap=%d wcount=%d", ULL(sp_h(space)), out ? out->wcap : -1, out ? out->wcount : -1);
    return r;
}
SP_IMPL sp_GetSpaceContainer(uint64_t space, cap_list *out) {
    kl_ovrp_hit("ovrp_GetSpaceContainer");
    if (!out) return KLOVRP_INVALID_PARAM;
    kl_uuid id; int kind; uint32_t nm = 0;
    if (!space_of(space, &id, &kind) || kind != KLSP_ROOM) { out->count = 0; return KLOVRP_INVALID_PARAM; }
    uint32_t cap = (out->cap > 0 && out->data) ? (uint32_t)out->cap : 0;
    if (!kl_spatial_room_info(id, NULL, NULL, 0, NULL, NULL, cap, cap ? (kl_uuid *)out->data : NULL, &nm)) return KLOVRP_OPERATION_FAILED;
    out->count = (int32_t)(cap && nm > cap ? cap : nm);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceContainer(uint64_t space, cap_list *out) {
    ovrpResult r = sp_GetSpaceContainer(space, out);
    SPT("ovrp_GetSpaceContainer", r, "space=0x%llx cap=%d count=%d", ULL(sp_h(space)), out ? out->cap : -1, out ? out->count : -1);
    return r;
}
// ovrp_GetSpaceTriangleMesh(space, vertexCap, out vertexCount, vertices*, indexCap, out indexCount, indices*)
// (the OpenXR-style two-call shape; indices are triangle corners, 3 per triangle).
SP_IMPL sp_GetSpaceTriangleMesh(uint64_t space, int32_t vcap, int32_t *vcount, float *verts,
                                              int32_t icap, int32_t *icount, uint32_t *indices) {
    kl_ovrp_hit("ovrp_GetSpaceTriangleMesh");
    if (vcount) *vcount = 0;
    if (icount) *icount = 0;
    kl_uuid id; int kind; uint32_t nv = 0, nt = 0;
    if (!space_of(space, &id, &kind) || kind != KLSP_MESH) return KLOVRP_INVALID_PARAM;
    uint32_t vc = (vcap > 0 && verts) ? (uint32_t)vcap : 0, tc = (icap > 0 && indices) ? (uint32_t)icap / 3u : 0;
    if (!kl_spatial_mesh_info(id, vc, vc ? verts : NULL, &nv, tc, tc ? indices : NULL, &nt)) return KLOVRP_OPERATION_FAILED;
    if (vcount) *vcount = (int32_t)(vc && nv > vc ? vc : nv);
    if (icount) *icount = (int32_t)(tc && nt > tc ? tc * 3u : nt * 3u);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetSpaceTriangleMesh(uint64_t space, int32_t vcap, int32_t *vcount, float *verts,
                                              int32_t icap, int32_t *icount, uint32_t *indices) {
    ovrpResult r = sp_GetSpaceTriangleMesh(space, vcap, vcount, verts, icap, icount, indices);
    SPT("ovrp_GetSpaceTriangleMesh", r, "space=0x%llx vcap=%d vcount=%d icap=%d icount=%d", ULL(sp_h(space)), vcap, vcount ? *vcount : -1, icap, icount ? *icount : -1);
    return r;
}

// ---- scene capture ------------------------------------------------------------------
// Diagnostics, once per capture: every room as the game reads it (layout floor / ceiling / walls, container members with label,
// size and polygon points) and the planes an unfiltered query also returns that no room holds. Positions are guest space.
static uint64_t dump_h(kl_uuid id) { static const kl_uuid z; return memcmp(&id, &z, sizeof id) ? kl_spatial_handle_for(id) : 0; }
static void room_dump(void) {
    enum { MW = 64, MM = 256 };
    kl_uuid w[MW], mem[MM];                        // ~5 KB of the capture thread's 128 KB stack
    uint32_t nr = kl_spatial_count(KLSP_ROOM), np = kl_spatial_count(KLSP_PLANE), inroom = 0;
    for (uint32_t r = 0; r < nr && r < 4; r++) {
        kl_uuid id, fl, ce; float m[16]; uint32_t nw = 0, nm = 0;
        if (!kl_spatial_get(KLSP_ROOM, r, &id, m) || !kl_spatial_room_info(id, &fl, &ce, MW, w, &nw, MM, mem, &nm)) continue;
        fprintf(stderr, "  [room-dump] room 0x%llx%s floor 0x%llx ceiling 0x%llx walls=%u members=%u\n", ULL(dump_h(id)),
                kl_spatial_is_synthetic(id) ? " (synthetic)" : "", ULL(dump_h(fl)), ULL(dump_h(ce)), nw, nm);
        for (uint32_t k = 0; k < nm && k < MM; k++) {
            float pw = 0, ph = 0; int lbl = KLSP_LBL_OTHER; uint32_t npl = 0, wall = 0;
            kl_spatial_plane_info(mem[k], &pw, &ph, &lbl, 0, NULL, &npl);
            for (uint32_t j = 0; j < nw && j < MW; j++) if (!memcmp(&w[j], &mem[k], sizeof w[j])) wall = j + 1;
            fprintf(stderr, "  [room-dump]   0x%llx %s %.2fx%.2f poly=%u%s%s\n", ULL(kl_spatial_handle_for(mem[k])), label_name(lbl), pw, ph, npl,
                    wall ? " layout_wall" : "", kl_spatial_is_synthetic(mem[k]) ? " synthetic" : "");
        }
        inroom = nm < MM ? nm : MM;
    }
    for (uint32_t i = 0; i < np; i++) {            // what an unfiltered query adds (against the last room's members)
        kl_uuid id; float m[16];
        if (!kl_spatial_get(KLSP_PLANE, i, &id, m)) continue;
        int found = 0;
        for (uint32_t k = 0; k < inroom && !found; k++) found = !memcmp(&mem[k], &id, sizeof id);
        if (found) continue;
        float pw = 0, ph = 0; int lbl = KLSP_LBL_OTHER;
        kl_spatial_plane_info(id, &pw, &ph, &lbl, 0, NULL, NULL);
        fprintf(stderr, "  [room-dump]   outside any room: 0x%llx %s %.2fx%.2f at %.2f,%.2f,%.2f\n", ULL(kl_spatial_handle_for(id)),
                label_name(lbl), pw, ph, m[12], m[13], m[14]);
    }
}
// Quest opens its scanning app. visionOS scans on its own, so success is immediate and the
// completion event waits for data (planes or a room), then a short settle so the first
// query sees more than one wall; or a timeout, with whatever there is.
static void *capture_thread(void *arg) {
    uint64_t req = (uint64_t)(uintptr_t)arg;
    const int step_ms = 100, settle_ms = 1500;
    int timeout_ms = 8000;
    const char *tv = getenv("KL_SPATIAL_CAPTURE_TIMEOUT_MS");      // test hook; default unchanged
    if (tv && atoi(tv) > 0) timeout_ms = atoi(tv);
    int waited = 0;
    while (waited < timeout_ms && kl_spatial_count(KLSP_PLANE) == 0 && kl_spatial_count(KLSP_ROOM) == 0
           && (kl_spatial_available(KLSP_CAP_PLANES) || kl_spatial_available(KLSP_CAP_ROOM))) {
        usleep(step_ms * 1000); waited += step_ms;
    }
    if (kl_spatial_count(KLSP_PLANE) > 0) usleep(settle_ms * 1000);
    // Quest's Space Setup always ends with a CLOSED room: without a real room or a complete real set, hand out a synthetic
    // box (size of the play area) around the head, completing any partial walls.
    int synth;
    {
        float hp[3], hq[4], zw = 3.0f, zd = 3.0f;
        kl_ovrp_get_head_pose(&hp[0], &hp[1], &hp[2], &hq[0], &hq[1], &hq[2], &hq[3]);
        if (!isfinite(hp[0]) || !isfinite(hp[2])) hp[0] = hp[2] = 0.0f;       // a NaN/Inf head would poison the box fit
        if (!kl_ovrp_boundary(&zw, &zd)) zw = zd = 3.0f;
        synth = kl_spatial_synth_room(hp[0], hp[2], zw, zd);
        if (synth) fprintf(stderr, "  [sp] capture synthetic room\n");
    }
    ev_simple e; memset(&e, 0, sizeof e);
    e.requestId = req;
    e.result = (synth || kl_spatial_available(KLSP_CAP_PLANES) || kl_spatial_available(KLSP_CAP_ROOM) || kl_spatial_count(KLSP_PLANE))
                   ? KLOVRP_SUCCESS : KLOVRP_UNSUPPORTED;
    push(EV_SCENE_CAPTURE, &e, sizeof e);
    fprintf(stderr, "  [ovrp-spatial] scene capture %llu complete after %d ms: %u planes, %u rooms, %u meshes\n",
            (unsigned long long)req, waited, kl_spatial_count(KLSP_PLANE), kl_spatial_count(KLSP_ROOM), kl_spatial_count(KLSP_MESH));
    room_dump();
    return NULL;
}
SP_IMPL sp_RequestSceneCapture(const void *request, uint64_t *req_out) {
    kl_ovrp_hit("ovrp_RequestSceneCapture");
    (void)request;
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    uint64_t req = kl_spatial_new_request();
    pthread_t th; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    int rc = pthread_create(&th, &at, capture_thread, (void *)(uintptr_t)req);
    pthread_attr_destroy(&at);
    if (rc != 0) {                                  // no thread: complete now with what there is
        ev_simple e; memset(&e, 0, sizeof e);
        e.requestId = req; e.result = KLOVRP_SUCCESS;
        push(EV_SCENE_CAPTURE, &e, sizeof e);
    }
    *req_out = req;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_RequestSceneCapture(const void *request, uint64_t *req_out) {
    ovrpResult r = sp_RequestSceneCapture(request, req_out);
    SPT("ovrp_RequestSceneCapture", r, "req=%llu", ULL(req_out ? *req_out : 0));
    return r;
}

// ---- app space / stationary reference ---------------------------------------------------
static ovrpResult klovrp_GetAppSpace(uint64_t *out) {
    kl_ovrp_hit("ovrp_GetAppSpace");
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = OVRP_APP_SPACE;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetStationaryReferenceSpaceId(kl_uuid *out) {
    kl_ovrp_hit("ovrp_GetStationaryReferenceSpaceId");
    if (!out) return KLOVRP_INVALID_PARAM;
    static const kl_uuid u = {{'K','L','-','S','T','A','T','I','O','N','A','R','Y','-','0','1'}};
    *out = u;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_UnityOpenXR_OnAppSpaceChange2(void) {
    return kl_ovrp_ok("ovrp_UnityOpenXR_OnAppSpaceChange2");
}

// ---- environment raycast (CPU over the ARKit mesh / planes) --------------------------------
static ovrpResult klovrp_GetEnvironmentRaycastSupported(ovrpBool *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentRaycastSupported");
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = (kl_spatial_available(KLSP_CAP_MESH) || kl_spatial_available(KLSP_CAP_PLANES)) ? 1 : 0;
    return KLOVRP_SUCCESS;
}
#define RAYCASTER_BASE 0x6000ull
static int g_raycasters;
static ovrpResult klovrp_CreateEnvironmentRaycasterAsync(uint64_t *req_out) {
    kl_ovrp_hit("ovrp_CreateEnvironmentRaycasterAsync");
    if (!req_out) return KLOVRP_INVALID_PARAM;
    *req_out = 0;
    if (!kl_spatial_available(KLSP_CAP_MESH) && !kl_spatial_available(KLSP_CAP_PLANES)) return KLOVRP_UNSUPPORTED;
    *req_out = kl_spatial_new_request();
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_CreateEnvironmentRaycasterComplete(uint64_t req, uint64_t *out) {
    kl_ovrp_hit("ovrp_CreateEnvironmentRaycasterComplete");
    (void)req;
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = RAYCASTER_BASE + (uint64_t)__atomic_add_fetch(&g_raycasters, 1, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_DestroyEnvironmentRaycaster(uint64_t rc) {
    kl_ovrp_hit("ovrp_DestroyEnvironmentRaycaster");
    (void)rc;
    return KLOVRP_SUCCESS;
}
typedef struct { float origin[3], dir[3], max_dist; } ray_info;
typedef struct { ovrpPosef pose; ovrpVector3f normal; int32_t status; } ray_hit;
// (raycaster, info{origin,dir,maxDist}*, out hit{pose,normal,status}*): the layouts are the plan's guess.
static ovrpResult klovrp_PerformEnvironmentRaycast(uint64_t rc, const ray_info *info, ray_hit *out) {
    kl_ovrp_hit("ovrp_PerformEnvironmentRaycast");
    (void)rc;
    if (!info || !out) return KLOVRP_INVALID_PARAM;
    memset(out, 0, sizeof *out);
    out->pose.Orientation.w = 1;
    float md = info->max_dist > 0 ? info->max_dist : 10.0f, pos[3], nrm[3];
    kl_uuid hid;
    if (!kl_spatial_raycast(info->origin, info->dir, md, pos, nrm, &hid)) return KLOVRP_SUCCESS;     // status 0: no hit
    // Orientation turning +Z onto the normal (shortest arc).
    float c[3] = {-nrm[1], nrm[0], 0}, dot = nrm[2], l = sqrtf(c[0] * c[0] + c[1] * c[1]);
    ovrpQuatf q = {0, 0, 0, 1};
    if (dot < -0.9999f) { q.x = 1; q.w = 0; }
    else if (l > 1e-6f) { float w = 1.0f + dot, n = sqrtf(c[0] * c[0] + c[1] * c[1] + w * w); q.x = c[0] / n; q.y = c[1] / n; q.w = w / n; }
    out->pose.Orientation = q;
    out->pose.Position.x = pos[0]; out->pose.Position.y = pos[1]; out->pose.Position.z = pos[2];
    out->normal.x = nrm[0]; out->normal.y = nrm[1]; out->normal.z = nrm[2];
    out->status = 1;
    return KLOVRP_SUCCESS;
}

// ---- capabilities this host does not have -----------------------------------------------------
static ovrpResult klovrp_PredFalse(const char *name, ovrpBool *out) {
    kl_ovrp_hit(name);
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = 0;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetDynamicObjectKeyboardSupported(ovrpBool *o) { return klovrp_PredFalse("ovrp_GetDynamicObjectKeyboardSupported", o); }
static ovrpResult klovrp_GetDynamicObjectTrackerSupported(ovrpBool *o) { return klovrp_PredFalse("ovrp_GetDynamicObjectTrackerSupported", o); }
static ovrpResult klovrp_GetMarkerTrackingSupported(ovrpBool *o) { return klovrp_PredFalse("ovrp_GetMarkerTrackingSupported", o); }
// ovrpPassthroughPreferences { flags }: no passthrough-only mode on this host.
static ovrpResult klovrp_GetPassthroughPreferences(int32_t *out) {
    kl_ovrp_hit("ovrp_GetPassthroughPreferences");
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = 0;
    return KLOVRP_SUCCESS;
}

// ---- environment depth (kl_envdepth: the ARKit scene mesh rasterised per eye) -----------------
// Shapes read off the real libOVRPlugin's entry points (aarch64):
//   Supported / HandRemovalSupported (int *out); Initialize(int handRemoval);
//   Destroy / Start / Stop (); TextureDesc(desc *) = 24 bytes {w, h, 1, 1, 3, 6} (the tail is the
//   constant the plugin itself returns); StageCount(int *); Texture(stage, eye, uint64 *) - one
//   array texture for both eyes; SetHandRemoval(int); FrameDesc(eye, desc *) = 88 bytes:
//     0 i32 isValid | 8,16 two doubles (0 in the real plugin) | 24 i32 swapchain index |
//     28 pose {quat xyzw, pos xyz} | 56 tangents up, down, left, right | 72 nearZ | 76 farZ | 80 double 0.0078125
//   The real FrameDesc acquires a new image on the eye-0 call and serves the same frame to eye 1;
//   so does this.
static ovrpResult klovrp_GetEnvironmentDepthSupported(ovrpBool *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthSupported");
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = kl_envdepth_supported();
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_InitializeEnvironmentDepth(int32_t hand_removal) {
    kl_ovrp_hit("ovrp_InitializeEnvironmentDepth");
    if (kl_envdepth_create(hand_removal) != 0) return KLOVRP_UNSUPPORTED;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_DestroyEnvironmentDepth(void) {
    kl_ovrp_hit("ovrp_DestroyEnvironmentDepth");
    kl_envdepth_destroy();
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_StartEnvironmentDepth(void) {
    kl_ovrp_hit("ovrp_StartEnvironmentDepth");
    return kl_envdepth_start() == 0 ? KLOVRP_SUCCESS : KLOVRP_NOT_INITIALIZED;
}
static ovrpResult klovrp_StopEnvironmentDepth(void) {
    kl_ovrp_hit("ovrp_StopEnvironmentDepth");
    kl_envdepth_stop();
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_SetEnvironmentDepthHandRemoval(int32_t on) {
    kl_ovrp_hit("ovrp_SetEnvironmentDepthHandRemoval");
    kl_envdepth_set_hand_removal(on & 1);
    return KLOVRP_SUCCESS;
}
// The images exist once the provider does; a GL guest needs its context current to make them,
// so this is asked at every query until it succeeds.
static int envdepth_images(void) {
    if (!kl_envdepth_created()) return 0;
    return kl_envdepth_alloc() > 0;
}
static ovrpResult klovrp_GetEnvironmentDepthTextureDesc(int32_t *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthTextureDesc");
    if (!out) return KLOVRP_INVALID_PARAM;
    if (!envdepth_images()) return KLOVRP_NOT_INITIALIZED;
    out[0] = KLED_W; out[1] = KLED_H; out[2] = 1; out[3] = 1; out[4] = 3; out[5] = 6;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetEnvironmentDepthTextureStageCount(int32_t *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthTextureStageCount");
    if (!out) return KLOVRP_INVALID_PARAM;
    if (!envdepth_images()) return KLOVRP_NOT_INITIALIZED;
    *out = KLED_IMAGES;
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetEnvironmentDepthTexture(int32_t stage, int32_t eye, uint64_t *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthTexture");
    (void)eye;
    if (!out) return KLOVRP_INVALID_PARAM;
    if (!envdepth_images()) return KLOVRP_NOT_INITIALIZED;
    if (stage < 0 || stage >= KLED_IMAGES) return KLOVRP_INVALID_PARAM;
    *out = kl_envdepth_image(stage);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetEnvironmentDepthFrameDesc(int32_t eye, void *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthFrameDesc");
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static kl_envdepth_frame cache;
    static int have;
    if (!out) return KLOVRP_INVALID_PARAM;
    if (eye < 0) eye = 0;
    if (eye > 1) eye = 1;
    unsigned char *o = (unsigned char *)out;
    memset(o, 0, 88);
    pthread_mutex_lock(&mu);
    if (eye == 0 || !have) {
        int r = kl_envdepth_acquire(&cache);
        have = r > 0;
    }
    if (have) {
        const kl_envdepth_frame *f = &cache;
        int32_t valid = 1, index = f->index;
        float near_z = f->near_z, far_z = f->far_z;
        float tan4[4] = { f->tan[eye][2], f->tan[eye][3], f->tan[eye][0], f->tan[eye][1] };   // up, down, left, right
        const float *p = f->pose[eye];
        float pq[7] = { p[3], p[4], p[5], p[6], p[0], p[1], p[2] };                             // quat, then position
        double k = 0.0078125;
        memcpy(o + 0, &valid, 4);
        memcpy(o + 24, &index, 4);
        memcpy(o + 28, pq, sizeof pq);
        memcpy(o + 56, tan4, sizeof tan4);
        memcpy(o + 72, &near_z, 4);
        memcpy(o + 76, &far_z, 4);
        memcpy(o + 80, &k, 8);
    }
    pthread_mutex_unlock(&mu);
    return KLOVRP_SUCCESS;
}
static ovrpResult klovrp_GetEnvironmentDepthHandRemovalSupported(ovrpBool *out) {
    kl_ovrp_hit("ovrp_GetEnvironmentDepthHandRemovalSupported");
    if (!out) return KLOVRP_INVALID_PARAM;
    *out = kl_envdepth_supported();      // the mesh never contains hands, so "removed" is always true
    return KLOVRP_SUCCESS;
}

static const kl_ovrp_entry k_tab[] = {
    {"ovrp_CreateSpatialAnchor", (void *)klovrp_CreateSpatialAnchor},
    {"ovrp_DestroySpace", (void *)klovrp_DestroySpace},
    {"ovrp_SaveSpace", (void *)klovrp_SaveSpace},
    {"ovrp_SaveSpaces", (void *)klovrp_SaveSpaces},
    {"ovrp_SaveSpaceList", (void *)klovrp_SaveSpaceList},
    {"ovrp_EraseSpace", (void *)klovrp_EraseSpace},
    {"ovrp_EraseSpaces", (void *)klovrp_EraseSpaces},
    {"ovrp_LocateSpace", (void *)klovrp_LocateSpace},
    {"ovrp_LocateSpace2", (void *)klovrp_LocateSpace2},
    {"ovrp_GetSpaceUuid", (void *)klovrp_GetSpaceUuid},
    {"ovrp_GetSpaceUserId", (void *)klovrp_GetSpaceUserId},
    {"ovrp_CreateSpaceUser", (void *)klovrp_CreateSpaceUser},
    {"ovrp_DestroySpaceUser", (void *)klovrp_DestroySpaceUser},
    {"ovrp_EnumerateSpaceSupportedComponents", (void *)klovrp_EnumerateSpaceSupportedComponents},
    {"ovrp_GetSpaceComponentStatus", (void *)klovrp_GetSpaceComponentStatus},
    {"ovrp_SetSpaceComponentStatus", (void *)klovrp_SetSpaceComponentStatus},
    {"ovrp_QuerySpaces", (void *)klovrp_QuerySpaces},
    {"ovrp_QuerySpaces2", (void *)klovrp_QuerySpaces2},
    {"ovrp_RetrieveSpaceQueryResults", (void *)klovrp_RetrieveSpaceQueryResults},
    {"ovrp_DiscoverSpaces", (void *)klovrp_DiscoverSpaces},
    {"ovrp_RetrieveSpaceDiscoveryResults", (void *)klovrp_RetrieveSpaceDiscoveryResults},
    {"ovrp_GetSpaceContainer", (void *)klovrp_GetSpaceContainer},
    {"ovrp_GetSpaceBoundingBox2D", (void *)klovrp_GetSpaceBoundingBox2D},
    {"ovrp_GetSpaceBoundingBox3D", (void *)klovrp_GetSpaceBoundingBox3D},
    {"ovrp_GetSpaceSemanticLabels", (void *)klovrp_GetSpaceSemanticLabels},
    {"ovrp_GetSpaceBoundary2D", (void *)klovrp_GetSpaceBoundary2D},
    {"ovrp_GetSpaceRoomLayout", (void *)klovrp_GetSpaceRoomLayout},
    {"ovrp_GetSpaceTriangleMesh", (void *)klovrp_GetSpaceTriangleMesh},
    {"ovrp_RequestSceneCapture", (void *)klovrp_RequestSceneCapture},
    {"ovrp_GetAppSpace", (void *)klovrp_GetAppSpace},
    {"ovrp_GetStationaryReferenceSpaceId", (void *)klovrp_GetStationaryReferenceSpaceId},
    {"ovrp_UnityOpenXR_OnAppSpaceChange2", (void *)klovrp_UnityOpenXR_OnAppSpaceChange2},
    {"ovrp_GetEnvironmentRaycastSupported", (void *)klovrp_GetEnvironmentRaycastSupported},
    {"ovrp_CreateEnvironmentRaycasterAsync", (void *)klovrp_CreateEnvironmentRaycasterAsync},
    {"ovrp_CreateEnvironmentRaycasterComplete", (void *)klovrp_CreateEnvironmentRaycasterComplete},
    {"ovrp_DestroyEnvironmentRaycaster", (void *)klovrp_DestroyEnvironmentRaycaster},
    {"ovrp_PerformEnvironmentRaycast", (void *)klovrp_PerformEnvironmentRaycast},
    {"ovrp_GetDynamicObjectKeyboardSupported", (void *)klovrp_GetDynamicObjectKeyboardSupported},
    {"ovrp_GetDynamicObjectTrackerSupported", (void *)klovrp_GetDynamicObjectTrackerSupported},
    {"ovrp_GetMarkerTrackingSupported", (void *)klovrp_GetMarkerTrackingSupported},
    {"ovrp_GetPassthroughPreferences", (void *)klovrp_GetPassthroughPreferences},
    {"ovrp_GetEnvironmentDepthSupported", (void *)klovrp_GetEnvironmentDepthSupported},
    {"ovrp_GetEnvironmentDepthHandRemovalSupported", (void *)klovrp_GetEnvironmentDepthHandRemovalSupported},
    {"ovrp_InitializeEnvironmentDepth", (void *)klovrp_InitializeEnvironmentDepth},
    {"ovrp_DestroyEnvironmentDepth", (void *)klovrp_DestroyEnvironmentDepth},
    {"ovrp_StartEnvironmentDepth", (void *)klovrp_StartEnvironmentDepth},
    {"ovrp_StopEnvironmentDepth", (void *)klovrp_StopEnvironmentDepth},
    {"ovrp_GetEnvironmentDepthTextureDesc", (void *)klovrp_GetEnvironmentDepthTextureDesc},
    {"ovrp_GetEnvironmentDepthTextureStageCount", (void *)klovrp_GetEnvironmentDepthTextureStageCount},
    {"ovrp_GetEnvironmentDepthTexture", (void *)klovrp_GetEnvironmentDepthTexture},
    {"ovrp_SetEnvironmentDepthHandRemoval", (void *)klovrp_SetEnvironmentDepthHandRemoval},
    {"ovrp_GetEnvironmentDepthFrameDesc", (void *)klovrp_GetEnvironmentDepthFrameDesc},
};

// No network / accounts, no QR or dynamic-object tracking on visionOS for an app (environment depth is
// served from the scene mesh: kl_envdepth).
static const char *const k_refused[] = {
    "ovrp_ShareSpaces", "ovrp_ShareSpaces2",
    "ovrp_StartColocationDiscovery", "ovrp_StopColocationDiscovery",
    "ovrp_StartColocationAdvertisement", "ovrp_StopColocationAdvertisement",
    "ovrp_CreateDynamicObjectTracker", "ovrp_DestroyDynamicObjectTracker",
    "ovrp_SetDynamicObjectTrackedClasses", "ovrp_GetSpaceDynamicObjectData",
    "ovrp_CreateMarkerTrackerAsync", "ovrp_CreateMarkerTrackerComplete",
    "ovrp_DestroyMarkerTracker", "ovrp_GetSpaceMarkerPayload",
    "ovrp_GetSpaceRoomMesh", "ovrp_GetSpaceRoomFaceIndices",
    NULL,
};

void kl_ovrp_spatial_register(void) {
    kl_ovrp_register(k_tab, sizeof k_tab / sizeof k_tab[0], k_refused);
}
