// Eye texture size from the app's «Resolución» slider (KL_OVRP_EYE_BASE=quest3,
// KL_OVRP_EYE_SCALE=percent/100). Pure arithmetic, no state: kl_ovrp.c uses
// it, tests/kl_ovrp_eyesize_test.c checks it.
//
// 100 % = the Quest 3 per-eye pixel budget (2064x2208 = 4.56 Mpx, the recommendation of the
// real headset). 200 % = the Vision Pro panel's own density (3660x3200 = 11.7 Mpx). Linear per
// axis between those two anchors (below 100 %: linear through 0). Both anchors are reshaped to
// the display's aspect (square pixels, same shape as the frustum), so one factor serves both axes.
//
// The value is the RECOMMENDED eye size, fixed once before the guest starts (kl_ovrp.c). The
// layer desc is derived from it by kles_desc_size; the runtime accepts any layer / swapchain size
// the guest then builds and only measures the plugin widening once (kles_freeze_widen).
#ifndef KL_OVRP_EYESIZE_H
#define KL_OVRP_EYESIZE_H

#include <math.h>

#define KLES_Q3_W      2064.0f
#define KLES_Q3_H      2208.0f
#define KLES_AVP_W     3660.0f
#define KLES_AVP_H     3200.0f
#define KLES_PCT_MIN   50.0f
#define KLES_PCT_MAX   200.0f
#define KLES_CAP_DFLT  4096

// Linear factor per axis over the Quest 3 size for a percentage (clamped to 50..200).
static inline float kles_pct_factor(float pct) {
    if (!(pct >= KLES_PCT_MIN)) pct = (pct == pct) ? KLES_PCT_MIN : 100.0f;
    if (pct > KLES_PCT_MAX) pct = KLES_PCT_MAX;
    if (pct <= 100.0f) return pct / 100.0f;
    float f_avp = sqrtf((KLES_AVP_W * KLES_AVP_H) / (KLES_Q3_W * KLES_Q3_H));
    return 1.0f + (pct - 100.0f) / 100.0f * (f_avp - 1.0f);
}

// Final per-eye size for a percentage at the display's aspect (w/h), the longest axis capped at
// `cap` (<= 0: no cap) keeping the aspect. Returns 0 and leaves w/h alone on a bad aspect.
static inline int kles_target_size(float aspect, float pct, int cap, int *w, int *h) {
    if (!(aspect > 0.1f && aspect < 10.0f) || !w || !h) return 0;
    float h0 = sqrtf((KLES_Q3_W * KLES_Q3_H) / aspect);
    float f = kles_pct_factor(pct);
    int th = (int)(h0 * f + 0.5f);
    int tw = (int)(h0 * aspect * f + 0.5f);
    if (cap > 0 && (tw > cap || th > cap)) {
        float k = (float)cap / (float)(tw > th ? tw : th);
        tw = (int)((float)tw * k + 0.5f);
        th = (int)((float)th * k + 0.5f);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    *w = tw; *h = th;
    return 1;
}

// The eye layer desc the guest is handed. The value is the FINAL eye size: the guest's own texture
// scale above 1 (Unity's eyeTextureResolutionScale, e.g. 1.3 in AC Nexus) is cancelled, one below 1
// (the game asking for fewer pixels) is kept, and the plugin widening frozen once per run
// (kles_freeze_widen) is divided out. The longest axis is capped at `cap` (<= 0: no cap) keeping
// the aspect. Inputs only (recommended, the guest's scale, the frozen widening): nothing the guest
// built after an earlier desc moves it, so there is no feedback loop.
static inline void kles_desc_size(int tw, int th, float game_scale, float widen_w, float widen_h,
                                  int cap, int *w, int *h) {
    float eff = (game_scale > 0.0f && game_scale < 1.0f) ? game_scale : 1.0f;
    if (!(widen_w >= 1.0f)) widen_w = 1.0f;
    if (!(widen_h >= 1.0f)) widen_h = 1.0f;
    int dw = (int)((float)tw * eff / widen_w + 0.5f);
    int dh = (int)((float)th * eff / widen_h + 0.5f);
    if (cap > 0 && (dw > cap || dh > cap)) {
        float k = (float)cap / (float)(dw > dh ? dw : dh);
        dw = (int)((float)dw * k + 0.5f);
        dh = (int)((float)dh * k + 0.5f);
    }
    *w = dw < 1 ? 1 : dw;
    *h = dh < 1 ? 1 : dh;
}

// The guest's plugin may widen the layer it builds from our desc (the union of the two canted
// cones). Measured ONCE per run: the first eye layer that is a widening (> 1.02 on at least one
// axis, 0.98..1.6 on both) freezes it in *ww / *wh (an axis at or under 1.02 stays 1.0) and sets
// *frozen; it is never re-learned, so at most one rebuild follows and the desc is then a fixed
// point. A layer at ~1.0 (no widening yet, or a plugin that never widens) stays provisional:
// widen = 1 keeps being used and a later widened layer can still freeze. A ratio outside the range
// is the guest choosing its own size: nothing is frozen. Returns 1 when it froze now.
static inline int kles_freeze_widen(int fill_w, int fill_h, int setup_w, int setup_h,
                                    int *frozen, float *ww, float *wh) {
    if (*frozen || fill_w <= 0 || fill_h <= 0 || setup_w <= 0 || setup_h <= 0) return 0;
    float rw = (float)setup_w / (float)fill_w, rh = (float)setup_h / (float)fill_h;
    if (!(rw >= 0.98f && rw <= 1.6f && rh >= 0.98f && rh <= 1.6f)) return 0;
    if (!(rw > 1.02f || rh > 1.02f)) return 0;
    *ww = rw > 1.02f ? rw : 1.0f;
    *wh = rh > 1.02f ? rh : 1.0f;
    *frozen = 1;
    return 1;
}

// OpenXR: the same freeze-once for the guest's own scale over the recommendation. The first
// eye-shaped swapchain (same factor on both axes within 3 %) that is > 1.02 and <= 2 times what we
// recommended freezes that scale; from then on the recommendation is the slider divided by it.
// *scale starts at 1 and is never re-learned. rec = the recommendation we answered, sw/sh = created.
// Returns 1 when it froze now.
static inline int kles_freeze_xr_scale(int rec_w, int rec_h, int sw, int sh, int *frozen, float *scale) {
    if (*frozen || rec_w <= 0 || rec_h <= 0 || sw <= 0 || sh <= 0) return 0;
    float rw = (float)sw / (float)rec_w, rh = (float)sh / (float)rec_h;
    if (fabsf(rw - rh) > 0.03f * rh) return 0;
    if (!(rw > 1.02f && rw <= 2.0f)) return 0;
    *scale = rw;
    *frozen = 1;
    return 1;
}

// The recommendation to answer an OpenXR guest with, given its frozen scale (>= 1).
static inline void kles_xr_recommended(int tw, int th, float xr_scale, int *w, int *h) {
    if (!(xr_scale >= 1.0f)) xr_scale = 1.0f;
    int rw = (int)((float)tw / xr_scale + 0.5f), rh = (int)((float)th / xr_scale + 0.5f);
    *w = rw < 1 ? 1 : rw;
    *h = rh < 1 ? 1 : rh;
}

#endif
