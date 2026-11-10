// Pure rules of the guest low-memory notification and of the [mem] telemetry throttle. No platform calls, so
// tests/t_lowmem.c runs the shipping code on Linux. Users: libc/kl_libc.c (kl_mem_pressure_poll, kl_mem_telemetry),
// jni/kl_jni_android.c (ActivityManager.getMemoryInfo).
#ifndef KL_MEMRULE_H
#define KL_MEMRULE_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// The guest (Unity Application.lowMemory -> UnloadUnusedAssets, UE trim) answers a notification by dropping streaming
// pools and LODs, and the geometry it drops is not always back. So it is told only what is true: the OS's own memory
// pressure, or REAL use (phys_footprint) of the REAL limit (footprint + os_proc_available_memory). Percentages of the
// real limit; each tier re-arms only below its CLEAR value.
#define KL_LOWMEM_AT_DEFAULT          80
#define KL_LOWMEM_CLEAR_DEFAULT       70
#define KL_LOWMEM_CRIT_DEFAULT        90
#define KL_LOWMEM_CRIT_CLEAR_DEFAULT  80
// Seconds-to-the-ceiling of the footprint slope trigger (KL_LOWMEM_RATE_S). 0 = off: a steep climb is not pressure (a
// level load climbs fast and settles), so the guest is not told for it unless the variable asks for it.
#define KL_LOWMEM_RATE_S_DEFAULT      0
// Same slope rule for the HOST-side trim only (KL_TRIM_RATE_S): retired images, VPKEngine's trim, graveyard drain. It never
// marks a guest notification pending. 0 = off.
#define KL_TRIM_RATE_S_DEFAULT        6
// Real-limit headroom floor in MiB (KL_LOWMEM_FREE_MB); a negative value is ignored.
#define KL_LOWMEM_FREE_MB_DEFAULT     384

// Percentages from the environment are clamped to 0..100 (an `at` above 100 would wrap 100 - at).
static inline int kl_lowmem_pct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
// KL_LOWMEM_FREE_MB -> bytes; negative = ignored (default).
static inline uint64_t kl_lowmem_floor_bytes(int mb) {
    return (uint64_t)(mb < 0 ? KL_LOWMEM_FREE_MB_DEFAULT : mb) << 20;
}

typedef struct { int warn_hot, warn_cold, crit_hot, crit_cold; const char *why; } kl_lowmem_eval;

// Levels for `used` bytes of footprint against `limit` (0 = unknown) with `avail` bytes of OS headroom (0 = unknown).
// `floor_b`: headroom at or below which the OS is about to end the process (critical, reason "headroom").
// why = "headroom" or "footprint"; the caller refines it.
static inline void kl_lowmem_eval_levels(uint64_t used, uint64_t avail, uint64_t limit, int at, int clear, int crit,
                                         int crit_clear, uint64_t floor_b, kl_lowmem_eval *o) {
    at = kl_lowmem_pct(at); clear = kl_lowmem_pct(clear); crit = kl_lowmem_pct(crit); crit_clear = kl_lowmem_pct(crit_clear);
    int pct = (limit && used) ? (int)((used * 100) / limit) : -1;
    o->warn_hot = 0; o->warn_cold = 1; o->crit_hot = 0; o->crit_cold = 1;
    o->why = "footprint";
    if (avail) {
        uint64_t clearf = floor_b + (floor_b >> 1);      // re-arm at 1.5x the floor
        if (avail <= floor_b) { o->crit_hot = 1; o->why = "headroom"; }
        if (avail < clearf) { o->warn_cold = 0; o->crit_cold = 0; }
    }
    if (pct >= 0) {
        if (pct >= at) o->warn_hot = 1;
        if (pct >= clear) o->warn_cold = 0;
        if (pct >= crit) o->crit_hot = 1;
        if (pct >= crit_clear) o->crit_cold = 0;
    }
    if (o->crit_hot) o->warn_hot = 1;   // never reach critical without the warning having fired
}

// ActivityManager.MemoryInfo.threshold: the free bytes below which lowMemory reads true, from the first tier.
static inline uint64_t kl_lowmem_threshold(uint64_t total, int at) {
    at = kl_lowmem_pct(at);
    return at > 0 ? total / 100 * (uint64_t)(100 - at) : 0;
}

// ActivityManager.getMemoryInfo().lowMemory: true exactly when the notification rule is hot. `at` <= 0 = notifications off.
static inline int kl_lowmem_is_low(uint64_t used, uint64_t avail, uint64_t limit, int at, int clear, int crit,
                                   int crit_clear, uint64_t floor_b) {
    if (at <= 0) return 0;
    kl_lowmem_eval ev;
    kl_lowmem_eval_levels(used, avail, limit, at, clear, crit, crit_clear, floor_b, &ev);
    return ev.warn_hot || ev.crit_hot;
}

// Arm / re-arm hysteresis over the two tiers (flags shared by the 100 ms timer, the frame-loop poll and the OS source).
// Returns 0 = nothing to tell, 1 = warning, 2 = critical; *why is refined for the log / pending reason.
static inline int kl_lowmem_step(_Atomic int *armed, _Atomic int *crit_armed, const kl_lowmem_eval *ev, const char **why) {
    int fire = 0;
    if (ev->crit_hot && atomic_exchange(crit_armed, 0)) {
        atomic_store(armed, 0);
        fire = 2;
        if (!strcmp(*why, "footprint")) *why = "footprint critical";
    } else if (ev->warn_hot && atomic_exchange(armed, 0)) {
        fire = 1;
        *why = "footprint warning";
    }
    if (!ev->crit_hot && ev->crit_cold) atomic_store(crit_armed, 1);
    if (!ev->warn_hot && ev->warn_cold) atomic_store(armed, 1);
    return fire;
}

// Footprint-slope trigger. `ttl` = seconds to the ceiling (<0 = footprint not rising / no sample), `slope` bytes/s.
// trim: host-side trim only (KL_TRIM_RATE_S); warn: also tell the guest (KL_LOWMEM_RATE_S). Each 0 = off.
typedef struct { int trim, warn; } kl_rate_dec;
static inline kl_rate_dec kl_rate_decide(double ttl, uint64_t slope, uint64_t min_slope, int trim_rate_s, int warn_rate_s) {
    kl_rate_dec d = {0, 0};
    if (ttl < 0 || slope < min_slope) return d;
    d.trim = trim_rate_s > 0 && ttl < (double)trim_rate_s;
    d.warn = warn_rate_s > 0 && ttl < (double)warn_rate_s;
    return d;
}

// [mem] telemetry throttle: one line per period; the first `cap` are unconditional, afterwards only when the footprint
// moved more than 10 % from the last printed one. Returns 1 when a line is due (and counts it).
typedef struct { int started; unsigned lines; uint64_t last_ms, last_foot; } kl_tele_state;
static inline int kl_tele_due(kl_tele_state *s, uint64_t now_ms, uint64_t foot, unsigned cap, uint64_t period_ms) {
    if (s->started && now_ms - s->last_ms < period_ms) return 0;
    s->started = 1;
    s->last_ms = now_ms;
    if (s->lines >= cap) {
        uint64_t d = foot > s->last_foot ? foot - s->last_foot : s->last_foot - foot;
        if (!s->last_foot || d * 10 <= s->last_foot) return 0;
    }
    s->lines++;
    s->last_foot = foot;
    return 1;
}

// Texture budget (VPKEngine vpvk_texture_budget): the largest side of the plain sampled textures the engine creates from now on,
// from the footprint as a share of the real limit. 0 = no cap. A Quest carries a title through its texture peaks with swap
// (4 GiB of zram on a Quest 3) and no per-process line; here GPU memory cannot be swapped and the process ends at the limit, so
// past half of it new textures above 4096 lose their largest mip level(s), past 70 % above 2048, past 85 % above 1024.
static inline unsigned kl_texbudget_cap(uint64_t used, uint64_t limit) {
    if (!limit) return 0;
    uint64_t pct = used * 100 / limit;
    return pct >= 85 ? 1024 : pct >= 70 ? 2048 : pct >= 50 ? 4096 : 0;
}
// The cap in force after asking for `want`: it only tightens within a run, so two textures of one family (a streamed-in copy
// and the one it replaces) never disagree on which levels exist in the direction that would leave a level unwritten.
static inline unsigned kl_texbudget_next(unsigned cur, unsigned want) {
    return !want ? cur : (!cur || want < cur) ? want : cur;
}

// Value of `key` in a "k=v k=v ..." line (the key must start the line or follow a space); 0 when absent or not a number.
static inline uint64_t kl_kv_u64(const char *line, const char *key) {
    size_t kn = strlen(key);
    for (const char *p = line; p && *p;) {
        if ((p == line || p[-1] == ' ') && !strncmp(p, key, kn) && p[kn] == '=') {
            uint64_t v = 0;
            for (const char *q = p + kn + 1; *q >= '0' && *q <= '9'; q++) v = v * 10 + (uint64_t)(*q - '0');
            return v;
        }
        p = strchr(p, ' ');
        if (p) p++;
    }
    return 0;
}

#endif
