// libandroid, second half. See kl_android.h.
//
// What a Quest 3 (Horizon OS, Android 12L) answers, served for what this runtime
// can stand behind, and the error Android returns for everything else:
//   AConfiguration    the headset's own configuration: xhdpi, landscape, no
//                     touch/keyboard/nav, SDK 32, the system locale.
//   AChoreographer    a vsync clock at the display frequency, delivering to the
//                     looper of the thread that asked, as the framework does.
//   AThermal          the visor's real thermal state (kl_host_thermal_state).
//   APerformanceHint  sessions are accepted and the hints are no-ops — which is
//                     what the API allows when the system has no use for them.
//   AStorageManager   OBB images cannot be loop-mounted here; every mount reports
//                     Android's own "could not mount" through the callback.
//   ASharedMemory     anonymous mappable files.
//   AInput*/ANativeActivity: the full accessor list, for a queue that never
//                     carries an event (see kl_ndk.c).
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kl_android.h"
#include "kl_ndk.h"
#include "klepton.h"
#include "kl_jni.h"
#include "kl_ovrp.h"

static int64_t kla_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ============================================================ AConfiguration
// ResTable_config's fields as the NDK getters/setters expose them. A freshly
// created configuration is all-ANY (zero), like Android's; AConfiguration_
// fromAssetManager fills it with the headset's current configuration.
typedef struct {
    int32_t mcc, mnc;
    char    lang[2], country[2];
    int32_t orientation, touchscreen, density, keyboard, navigation;
    int32_t keys_hidden, nav_hidden, sdk, screen_size, screen_long;
    int32_t ui_type, ui_night, sw_dp, w_dp, h_dp;
    int32_t layout_dir, screen_round, color_mode, gender;
} kla_config;

_Static_assert(sizeof(kla_config) <= 128, "kla_config must fit the 128 bytes the old blob reserved");
#define KLA_ACONFIG_BYTES 128

// ACONFIGURATION_* constants that matter for the values below.
enum {
    ACONF_ORIENTATION_LAND = 2, ACONF_TOUCHSCREEN_NOTOUCH = 1, ACONF_KEYBOARD_NOKEYS = 1,
    ACONF_NAVIGATION_NONAV = 1, ACONF_KEYSHIDDEN_NO = 1, ACONF_NAVHIDDEN_NO = 1,
    ACONF_SCREENSIZE_XLARGE = 4, ACONF_SCREENLONG_NO = 1, ACONF_UI_MODE_TYPE_NORMAL = 1,
    ACONF_UI_MODE_NIGHT_NO = 1, ACONF_LAYOUTDIR_LTR = 1, ACONF_SCREENROUND_NO = 1,
    ACONF_WIDE_COLOR_GAMUT_NO = 1, ACONF_HDR_NO = 4,
};

// The Quest 3, from the same panel constants the Java side reports (Display and
// DisplayMetrics in jni/kl_jni_display.c): 1832x1920 at xhdpi, so 916x960 dp,
// which Android classes xlarge and not "long". Orientation is landscape (the
// headset's apps run landscape), the device has no touch/keys/nav, SDK is 32,
// the colour gamut is sRGB, and the locale is the system's.
#define KLA_QUEST_W_PX 1832
#define KLA_QUEST_H_PX 1920
#define KLA_QUEST_DPI  320
static void kla_fill_quest(kla_config *c) {
    memset(c, 0, sizeof *c);
    char lang[16] = "", country[16] = "";
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    c->lang[0] = lang[0]; c->lang[1] = lang[0] && lang[1] ? lang[1] : 0;
    c->country[0] = country[0]; c->country[1] = country[0] && country[1] ? country[1] : 0;
    c->orientation = ACONF_ORIENTATION_LAND;
    c->touchscreen = ACONF_TOUCHSCREEN_NOTOUCH;
    c->density = KLA_QUEST_DPI;
    c->keyboard = ACONF_KEYBOARD_NOKEYS;
    c->navigation = ACONF_NAVIGATION_NONAV;
    c->keys_hidden = ACONF_KEYSHIDDEN_NO;
    c->nav_hidden = ACONF_NAVHIDDEN_NO;
    c->sdk = 32;
    c->screen_size = ACONF_SCREENSIZE_XLARGE;
    c->screen_long = ACONF_SCREENLONG_NO;
    c->ui_type = ACONF_UI_MODE_TYPE_NORMAL;
    c->ui_night = ACONF_UI_MODE_NIGHT_NO;
    c->w_dp = KLA_QUEST_W_PX * 160 / KLA_QUEST_DPI;
    c->h_dp = KLA_QUEST_H_PX * 160 / KLA_QUEST_DPI;
    c->sw_dp = c->w_dp < c->h_dp ? c->w_dp : c->h_dp;
    c->layout_dir = ACONF_LAYOUTDIR_LTR;
    c->screen_round = ACONF_SCREENROUND_NO;
    c->color_mode = ACONF_WIDE_COLOR_GAMUT_NO | ACONF_HDR_NO;
}

static void *kla_AConfiguration_new(void) { return calloc(1, KLA_ACONFIG_BYTES); }
static void  kla_AConfiguration_delete(void *c) { free(c); }
static void  kla_AConfiguration_fromAssetManager(void *c, void *mgr) {
    (void)mgr;                          // one asset manager, one device: the headset's configuration
    if (c) { memset(c, 0, KLA_ACONFIG_BYTES); kla_fill_quest(c); }
}
static void kla_AConfiguration_copy(void *dst, const void *src) {
    if (dst && src) memcpy(dst, src, KLA_ACONFIG_BYTES);
}

// Getters and setters, one pair per field. The NDK passes ints, so the setters
// store what they are given (including values outside the enum, as Android does).
#define KLA_GS(Name, field) \
    static int32_t kla_AConfiguration_get##Name(const void *c) { return c ? ((const kla_config *)c)->field : 0; } \
    static void    kla_AConfiguration_set##Name(void *c, int32_t v) { if (c) ((kla_config *)c)->field = v; }
KLA_GS(Mcc, mcc)
KLA_GS(Mnc, mnc)
KLA_GS(Orientation, orientation)
KLA_GS(Touchscreen, touchscreen)
KLA_GS(Density, density)
KLA_GS(Keyboard, keyboard)
KLA_GS(Navigation, navigation)
KLA_GS(KeysHidden, keys_hidden)
KLA_GS(NavHidden, nav_hidden)
KLA_GS(SdkVersion, sdk)
KLA_GS(ScreenSize, screen_size)
KLA_GS(ScreenLong, screen_long)
KLA_GS(UiModeType, ui_type)
KLA_GS(UiModeNight, ui_night)
KLA_GS(SmallestScreenWidthDp, sw_dp)
KLA_GS(ScreenWidthDp, w_dp)
KLA_GS(ScreenHeightDp, h_dp)
KLA_GS(LayoutDirection, layout_dir)
KLA_GS(ScreenRound, screen_round)
KLA_GS(ColorMode, color_mode)
KLA_GS(GrammaticalGender, gender)

// AConfiguration_getLanguage/getCountry fill EXACTLY two bytes and write no
// terminator: callers pass a char[2] on the stack. The setters read two bytes.
static void kla_AConfiguration_getLanguage(const void *c, char *out) {
    if (!out) return;
    out[0] = c ? ((const kla_config *)c)->lang[0] : 0;
    out[1] = c ? ((const kla_config *)c)->lang[1] : 0;
}
static void kla_AConfiguration_getCountry(const void *c, char *out) {
    if (!out) return;
    out[0] = c ? ((const kla_config *)c)->country[0] : 0;
    out[1] = c ? ((const kla_config *)c)->country[1] : 0;
}
static void kla_AConfiguration_setLanguage(void *c, const char *l) {
    if (!c) return;
    ((kla_config *)c)->lang[0] = l ? l[0] : 0;
    ((kla_config *)c)->lang[1] = l && l[0] ? l[1] : 0;
}
static void kla_AConfiguration_setCountry(void *c, const char *l) {
    if (!c) return;
    ((kla_config *)c)->country[0] = l ? l[0] : 0;
    ((kla_config *)c)->country[1] = l && l[0] ? l[1] : 0;
}

// Which fields differ, as the ACONFIGURATION_* change mask.
static int32_t kla_AConfiguration_diff(const void *a, const void *b) {
    const kla_config *x = a, *y = b;
    if (!x || !y) return 0;
    int32_t d = 0;
    if (x->mcc != y->mcc) d |= 0x1;
    if (x->mnc != y->mnc) d |= 0x2;
    if (memcmp(x->lang, y->lang, 2) || memcmp(x->country, y->country, 2)) d |= 0x4;
    if (x->touchscreen != y->touchscreen) d |= 0x8;
    if (x->keyboard != y->keyboard) d |= 0x10;
    if (x->keys_hidden != y->keys_hidden || x->nav_hidden != y->nav_hidden) d |= 0x20;
    if (x->navigation != y->navigation) d |= 0x40;
    if (x->orientation != y->orientation) d |= 0x80;
    if (x->density != y->density) d |= 0x100;
    if (x->screen_size != y->screen_size || x->w_dp != y->w_dp || x->h_dp != y->h_dp) d |= 0x200;
    if (x->sdk != y->sdk) d |= 0x400;
    if (x->screen_long != y->screen_long) d |= 0x800;
    if (x->ui_type != y->ui_type || x->ui_night != y->ui_night) d |= 0x1000;
    if (x->sw_dp != y->sw_dp) d |= 0x2000;
    if (x->layout_dir != y->layout_dir) d |= 0x4000;
    if (x->screen_round != y->screen_round) d |= 0x8000;
    if (x->color_mode != y->color_mode) d |= 0x10000;
    if (x->gender != y->gender) d |= 0x20000;
    return d;
}

// Resource selection. `match`: can `base` (a resource's qualifiers) be used on a
// device configured as `req`? Every qualifier it specifies must agree, and size
// qualifiers are minimums. `isBetterThan`: of two candidates, does `test` fit
// `req` more specifically than `base`? Specificity is the count of qualifiers
// that were specified and agree, with density as the tie-break (closer wins).
static int kla_field_ok(int32_t base, int32_t req) { return !base || base == req; }
static int32_t kla_AConfiguration_match(const void *b, const void *r) {
    const kla_config *base = b, *req = r;
    if (!base || !req) return 0;
    if (!kla_field_ok(base->mcc, req->mcc) || !kla_field_ok(base->mnc, req->mnc)) return 0;
    if (base->lang[0] && memcmp(base->lang, req->lang, 2)) return 0;
    if (base->country[0] && memcmp(base->country, req->country, 2)) return 0;
    if (!kla_field_ok(base->orientation, req->orientation) ||
        !kla_field_ok(base->touchscreen, req->touchscreen) ||
        !kla_field_ok(base->keyboard, req->keyboard) ||
        !kla_field_ok(base->navigation, req->navigation) ||
        !kla_field_ok(base->keys_hidden, req->keys_hidden) ||
        !kla_field_ok(base->nav_hidden, req->nav_hidden) ||
        !kla_field_ok(base->screen_long, req->screen_long) ||
        !kla_field_ok(base->ui_type, req->ui_type) ||
        !kla_field_ok(base->ui_night, req->ui_night) ||
        !kla_field_ok(base->layout_dir, req->layout_dir) ||
        !kla_field_ok(base->screen_round, req->screen_round)) return 0;
    if (base->screen_size > req->screen_size) return 0;
    if (base->sdk > req->sdk) return 0;
    if (base->sw_dp > req->sw_dp || base->w_dp > req->w_dp || base->h_dp > req->h_dp) return 0;
    return 1;
}
static int kla_specificity(const kla_config *c, const kla_config *req) {
    int s = 0;
    s += c->mcc && c->mcc == req->mcc;       s += c->mnc && c->mnc == req->mnc;
    s += c->lang[0] != 0;                    s += c->country[0] != 0;
    s += c->orientation != 0;                s += c->touchscreen != 0;
    s += c->keyboard != 0;                   s += c->navigation != 0;
    s += c->screen_size != 0;                s += c->screen_long != 0;
    s += c->ui_type != 0;                    s += c->ui_night != 0;
    s += c->sw_dp != 0;                      s += c->w_dp != 0 || c->h_dp != 0;
    s += c->layout_dir != 0;                 s += c->sdk != 0;
    return s;
}
static int32_t kla_AConfiguration_isBetterThan(const void *b, const void *t, const void *r) {
    const kla_config *base = b, *test = t, *req = r;
    if (!base || !test || !req) return 0;
    if (!r) return 0;
    int sb = kla_specificity(base, req), st = kla_specificity(test, req);
    if (st != sb) return st > sb;
    if (req->density) {                       // closer density, preferring the larger one when equidistant
        int db = abs(base->density ? base->density - req->density : 0);
        int dt = abs(test->density ? test->density - req->density : 0);
        return dt < db;
    }
    return 0;
}

// ============================================================ AChoreographer
// One instance per looper thread. A frame callback is one-shot and runs on the
// NEXT vsync after it was posted (a callback posted from inside a callback waits
// for the following frame); a delayed one additionally waits out its delay. The
// vsync itself is a host thread ticking at the display frequency the compositor
// reports (the same seam the Java Choreographer's frame clock reads); it only
// WAKES the owning looper — the callbacks run on the guest's own thread, inside
// its ALooper_poll*, which is where Android runs them.
enum { CH_FRAME32 = 0, CH_FRAME64, CH_VSYNC };
#define CH_MAX_Q 32
#define CH_MAX_RR 8
typedef struct ch_cb { int kind; void *fn; void *data; int64_t due_ns; uint64_t posted_tick; } ch_cb;
typedef struct kla_choreo {
    pthread_mutex_t lock;
    void           *looper;
    ch_cb           q[CH_MAX_Q];
    int             nq;
    struct { void *fn; void *data; } rr[CH_MAX_RR];
    int             nrr;
    int             rr_pending;
    struct kla_choreo *next;
} kla_choreo;

static pthread_key_t   g_ch_key;
static pthread_once_t  g_ch_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_ch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ch_wake = PTHREAD_COND_INITIALIZER;
static kla_choreo     *g_ch_list;
static int             g_ch_ticker;
static uint64_t        g_ch_tick;          // vsync counter
static int64_t         g_ch_vsync_ns;      // time of the latest vsync
static int64_t         g_ch_period_ns = 13888889;   // 72 Hz until the first tick reads the seam
static int             g_ch_active;        // callbacks outstanding anywhere
static void kla_ch_key(void) { pthread_key_create(&g_ch_key, NULL); }

static double kla_display_hz(void) {
    float hz = kl_ovrp_display_frequency();
    return (hz >= 30.0f && hz <= 240.0f) ? (double)hz : 72.0;
}

static void *kla_ch_ticker(void *arg) {
    (void)arg;
    int64_t next = kla_now_ns();
    double last_hz = 0;
    for (;;) {
        pthread_mutex_lock(&g_ch_lock);
        while (!g_ch_active) {                    // nobody is waiting for a frame: sleep until someone posts
            pthread_cond_wait(&g_ch_wake, &g_ch_lock);
            next = kla_now_ns();
        }
        pthread_mutex_unlock(&g_ch_lock);

        double hz = kla_display_hz();
        int64_t period = (int64_t)(1e9 / hz);
        next += period;
        int64_t now = kla_now_ns();
        if (next < now) next = now + period;      // fell behind (suspended): resync rather than burst
        if (next > now) {
            struct timespec d = { (time_t)((next - now) / 1000000000LL), (long)((next - now) % 1000000000LL) };
            nanosleep(&d, NULL);
        }
        int rate_changed = last_hz != 0 && hz != last_hz;
        last_hz = hz;

        __atomic_store_n(&g_ch_period_ns, period, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ch_vsync_ns, next, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_ch_tick, 1, __ATOMIC_RELEASE);

        pthread_mutex_lock(&g_ch_lock);
        for (kla_choreo *c = g_ch_list; c; c = c->next) {
            pthread_mutex_lock(&c->lock);
            int wake = 0;
            for (int i = 0; i < c->nq; i++)
                if (c->q[i].due_ns <= next) wake = 1;
            if (rate_changed && c->nrr) { c->rr_pending = 1; wake = 1; }
            void *lp = c->looper;
            pthread_mutex_unlock(&c->lock);
            if (wake) kl_ndk_looper_wake(lp);
        }
        pthread_mutex_unlock(&g_ch_lock);
    }
    return NULL;
}

static void kla_ch_activate(int delta) {
    pthread_mutex_lock(&g_ch_lock);
    g_ch_active += delta;
    if (g_ch_active < 0) g_ch_active = 0;
    if (g_ch_active && !g_ch_ticker) {
        pthread_t th;
        if (pthread_create(&th, NULL, kla_ch_ticker, NULL) == 0) { pthread_detach(th); g_ch_ticker = 1; }
    }
    if (g_ch_active) pthread_cond_signal(&g_ch_wake);
    pthread_mutex_unlock(&g_ch_lock);
}

static void *kla_AChoreographer_getInstance(void) {
    pthread_once(&g_ch_once, kla_ch_key);
    kla_choreo *c = pthread_getspecific(g_ch_key);
    if (c) return c;
    // Android: the Choreographer lives on a Looper. A thread without one gets none.
    void *lp = kl_ndk_looper_current();
    if (!lp) return NULL;
    c = calloc(1, sizeof *c);
    if (!c) return NULL;
    pthread_mutex_init(&c->lock, NULL);
    c->looper = lp;
    pthread_mutex_lock(&g_ch_lock);
    c->next = g_ch_list; g_ch_list = c;
    pthread_mutex_unlock(&g_ch_lock);
    pthread_setspecific(g_ch_key, c);
    return c;
}

static void kla_ch_post(kla_choreo *c, int kind, void *fn, void *data, int64_t delay_ms) {
    if (!c || !fn) return;
    pthread_mutex_lock(&c->lock);
    if (c->nq < CH_MAX_Q) {
        c->q[c->nq++] = (ch_cb){kind, fn, data,
                                kla_now_ns() + (delay_ms > 0 ? delay_ms * 1000000LL : 0),
                                __atomic_load_n(&g_ch_tick, __ATOMIC_ACQUIRE)};
        pthread_mutex_unlock(&c->lock);
        kla_ch_activate(+1);
        return;
    }
    pthread_mutex_unlock(&c->lock);
    static int said;
    if (!said++) fprintf(stderr, "  [ndk] AChoreographer: %d callbacks already pending, dropping one\n", CH_MAX_Q);
}
static void kla_AChoreographer_postFrameCallback(void *c, void *cb, void *d)   { kla_ch_post(c, CH_FRAME32, cb, d, 0); }
static void kla_AChoreographer_postFrameCallbackDelayed(void *c, void *cb, void *d, long ms) { kla_ch_post(c, CH_FRAME32, cb, d, ms); }
static void kla_AChoreographer_postFrameCallback64(void *c, void *cb, void *d) { kla_ch_post(c, CH_FRAME64, cb, d, 0); }
static void kla_AChoreographer_postFrameCallbackDelayed64(void *c, void *cb, void *d, uint32_t ms) { kla_ch_post(c, CH_FRAME64, cb, d, ms); }
static void kla_AChoreographer_postVsyncCallback(void *c, void *cb, void *d)   { kla_ch_post(c, CH_VSYNC, cb, d, 0); }

static void kla_AChoreographer_registerRefreshRateCallback(void *ch, void *cb, void *d) {
    kla_choreo *c = ch;
    if (!c || !cb) return;
    pthread_mutex_lock(&c->lock);
    int dup = 0;
    for (int i = 0; i < c->nrr; i++) if (c->rr[i].fn == cb && c->rr[i].data == d) dup = 1;
    if (!dup && c->nrr < CH_MAX_RR) { c->rr[c->nrr].fn = cb; c->rr[c->nrr].data = d; c->nrr++; kla_ch_activate(+1); }
    pthread_mutex_unlock(&c->lock);
}
static void kla_AChoreographer_unregisterRefreshRateCallback(void *ch, void *cb, void *d) {
    kla_choreo *c = ch;
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->nrr; i++)
        if (c->rr[i].fn == cb && c->rr[i].data == d) {
            c->rr[i] = c->rr[--c->nrr];
            kla_ch_activate(-1);
            break;
        }
    pthread_mutex_unlock(&c->lock);
}

// AChoreographerFrameCallbackData: one preferred frame timeline per frame. The
// vsync id counts frames; the expected presentation is one refresh after the
// vsync and the deadline the refresh period itself, the same shape
// Display.getPresentationDeadlineNanos reports on the Java side.
typedef struct { int64_t frame_time; int64_t period; uint64_t vsync_id; } kla_frame_data;
static int64_t kla_FCD_getFrameTimeNanos(const kla_frame_data *d) { return d ? d->frame_time : 0; }
static size_t  kla_FCD_getFrameTimelinesLength(const kla_frame_data *d) { return d ? 1 : 0; }
static size_t  kla_FCD_getPreferredFrameTimelineIndex(const kla_frame_data *d) { (void)d; return 0; }
static int64_t kla_FCD_getFrameTimelineVsyncId(const kla_frame_data *d, size_t i) {
    return d && i == 0 ? (int64_t)d->vsync_id : 0;
}
static int64_t kla_FCD_getFrameTimelineExpectedPresentationTimeNanos(const kla_frame_data *d, size_t i) {
    return d && i == 0 ? d->frame_time + d->period : 0;
}
static int64_t kla_FCD_getFrameTimelineDeadlineNanos(const kla_frame_data *d, size_t i) {
    return d && i == 0 ? d->frame_time + d->period : 0;
}


int kl_android_choreo_dispatch(void) {
    pthread_once(&g_ch_once, kla_ch_key);
    kla_choreo *c = pthread_getspecific(g_ch_key);
    if (!c) return 0;
    uint64_t tick = __atomic_load_n(&g_ch_tick, __ATOMIC_ACQUIRE);
    int64_t vs = __atomic_load_n(&g_ch_vsync_ns, __ATOMIC_ACQUIRE);
    int64_t period = __atomic_load_n(&g_ch_period_ns, __ATOMIC_RELAXED);
    if (!tick) return 0;
    int ran = 0;

    // Refresh-rate callbacks first: the new period is what the frame callbacks
    // that follow are paced against.
    pthread_mutex_lock(&c->lock);
    int rr = c->rr_pending;
    c->rr_pending = 0;
    void *rrf[CH_MAX_RR], *rrd[CH_MAX_RR];
    int nrr = rr ? c->nrr : 0;
    for (int i = 0; i < nrr; i++) { rrf[i] = c->rr[i].fn; rrd[i] = c->rr[i].data; }
    pthread_mutex_unlock(&c->lock);
    for (int i = 0; i < nrr; i++) {
        ((void (*)(int64_t, void *))rrf[i])(period, rrd[i]);
        ran++;
    }

    for (;;) {
        ch_cb cb;
        int found = 0;
        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < c->nq; i++) {
            // Due by time AND posted before the vsync being delivered: a callback
            // posted from inside a callback must wait for the next frame.
            if (c->q[i].due_ns <= vs && c->q[i].posted_tick < tick) {
                cb = c->q[i];
                c->q[i] = c->q[--c->nq];
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&c->lock);
        if (!found) break;
        kla_ch_activate(-1);
        switch (cb.kind) {
        case CH_FRAME32:
        case CH_FRAME64:
            ((void (*)(int64_t, void *))cb.fn)(vs, cb.data);
            break;
        default: {
            kla_frame_data fd = { vs, period, tick };
            ((void (*)(const kla_frame_data *, void *))cb.fn)(&fd, cb.data);
        }
        }
        ran++;
    }
    return ran;
}

// ================================================================== AThermal
// The visor's own thermal state. kl_host_thermal_state() (media/kl_thermal.m) is
// NSProcessInfo.thermalState; a build without it answers "none", which is also
// what a cool device says. -1 is the NDK's error value.
extern int kl_host_thermal_state(void) __attribute__((weak));
enum { ATHERMAL_STATUS_ERROR = -1, ATHERMAL_STATUS_NONE = 0, ATHERMAL_STATUS_LIGHT = 1,
       ATHERMAL_STATUS_MODERATE = 2, ATHERMAL_STATUS_SEVERE = 3, ATHERMAL_STATUS_CRITICAL = 4 };

static int kla_thermal_status(void) {
    if (!kl_host_thermal_state) return ATHERMAL_STATUS_NONE;
    switch (kl_host_thermal_state()) {          // NSProcessInfoThermalState
    case 0:  return ATHERMAL_STATUS_NONE;       // nominal
    case 1:  return ATHERMAL_STATUS_LIGHT;      // fair
    case 2:  return ATHERMAL_STATUS_SEVERE;     // serious: performance is visibly reduced
    case 3:  return ATHERMAL_STATUS_CRITICAL;   // critical
    default: return ATHERMAL_STATUS_ERROR;
    }
}

typedef struct { int unused; } kla_thermal_mgr;
static kla_thermal_mgr g_thermal_mgr;
typedef void (*kla_thermal_cb)(void *data, int status);
static struct { kla_thermal_cb fn; void *data; } g_thermal_l[8];
static int             g_thermal_n, g_thermal_thread;
static pthread_mutex_t g_thermal_lock = PTHREAD_MUTEX_INITIALIZER;

static void *kla_AThermal_acquireManager(void) { return &g_thermal_mgr; }
static void  kla_AThermal_releaseManager(void *m) { (void)m; }
static int   kla_AThermal_getCurrentThermalStatus(void *m) {
    return m ? kla_thermal_status() : ATHERMAL_STATUS_ERROR;
}

// Listeners are called when the status changes, and once on registration with
// the current status (what PowerManager does). Android calls them on a binder
// thread; ours is a guest-ready thread that polls the host state twice a second.
static void *kla_thermal_main(void *arg) {
    (void)arg;
    int last = -2;
    for (;;) {
        usleep(500000);
        int st = kla_thermal_status();
        pthread_mutex_lock(&g_thermal_lock);
        int n = g_thermal_n;
        kla_thermal_cb fn[8]; void *d[8];
        for (int i = 0; i < n; i++) { fn[i] = g_thermal_l[i].fn; d[i] = g_thermal_l[i].data; }
        pthread_mutex_unlock(&g_thermal_lock);
        if (st != last && st != ATHERMAL_STATUS_ERROR) {
            for (int i = 0; i < n; i++) fn[i](d[i], st);
            last = st;
        }
    }
    return NULL;
}
static int kla_AThermal_registerThermalStatusListener(void *m, kla_thermal_cb cb, void *data) {
    if (!m || !cb) return -EINVAL;
    pthread_mutex_lock(&g_thermal_lock);
    for (int i = 0; i < g_thermal_n; i++)
        if (g_thermal_l[i].fn == cb && g_thermal_l[i].data == data) {
            pthread_mutex_unlock(&g_thermal_lock);
            return -EINVAL;                      // already registered
        }
    if (g_thermal_n == 8) { pthread_mutex_unlock(&g_thermal_lock); return -ENOMEM; }
    g_thermal_l[g_thermal_n].fn = cb; g_thermal_l[g_thermal_n].data = data; g_thermal_n++;
    int start = !g_thermal_thread;
    g_thermal_thread = 1;
    pthread_mutex_unlock(&g_thermal_lock);
    if (start) {
        pthread_t th;
        if (kl_guest_thread_create(&th, 0, kla_thermal_main, NULL) == 0) pthread_detach(th);
    }
    return 0;
}
static int kla_AThermal_unregisterThermalStatusListener(void *m, kla_thermal_cb cb, void *data) {
    if (!m || !cb) return -EINVAL;
    pthread_mutex_lock(&g_thermal_lock);
    for (int i = 0; i < g_thermal_n; i++)
        if (g_thermal_l[i].fn == cb && g_thermal_l[i].data == data) {
            g_thermal_l[i] = g_thermal_l[--g_thermal_n];
            pthread_mutex_unlock(&g_thermal_lock);
            return 0;
        }
    pthread_mutex_unlock(&g_thermal_lock);
    return -EINVAL;                              // never registered
}
// A headroom needs a thermal model of the device; the host exposes only the
// coarse state, and a number invented from it would steer the guest's quality
// wrongly. NaN is the NDK's "not supported" (also for forecasts over 60 s).
static float kla_AThermal_getThermalHeadroom(void *m, int forecast_seconds) {
    (void)m; (void)forecast_seconds;
    return NAN;
}

// ============================================================ APerformanceHint
// Sessions are accepted and the hints change nothing: the NDK documents the
// system as free to ignore them. Validation is Android's: at least one thread,
// a positive target, positive durations.
typedef struct { int64_t target_ns; int prefer_power; } kla_perf_session;
static int g_perf_mgr;
static void *kla_APerformanceHint_getManager(void) { return &g_perf_mgr; }
static int64_t kla_APerformanceHint_getPreferredUpdateRateNanos(void *m) {
    if (!m) return -1;
    return (int64_t)(1e9 / kla_display_hz());       // one display period
}
static void *kla_APerformanceHint_createSession(void *m, const int32_t *tids, size_t n, int64_t target) {
    if (!m || !tids || n == 0 || target <= 0) return NULL;
    kla_perf_session *s = calloc(1, sizeof *s);
    if (s) s->target_ns = target;
    return s;
}
static int kla_APerformanceHint_updateTargetWorkDuration(void *s, int64_t target) {
    if (!s || target < 0) return -EINVAL;
    ((kla_perf_session *)s)->target_ns = target;
    return 0;
}
static int kla_APerformanceHint_reportActualWorkDuration(void *s, int64_t actual) {
    if (!s || actual <= 0) return -EINVAL;
    return 0;
}
static void kla_APerformanceHint_closeSession(void *s) { free(s); }
static int kla_APerformanceHint_setThreads(void *s, const int32_t *tids, size_t n) {
    if (!s || !tids || n == 0) return -EINVAL;
    return 0;
}
static int kla_APerformanceHint_setPreferPowerEfficiency(void *s, int enabled) {
    if (!s) return -EINVAL;
    ((kla_perf_session *)s)->prefer_power = enabled != 0;
    return 0;
}

// ============================================================ AStorageManager
// OBB images are loop-mounted by vold on Android. There is no vold here and
// nothing to mount an image with, so a mount reports the framework's own failure
// (ERROR_COULD_NOT_MOUNT) through the callback — asynchronously, as it would
// arrive — and nothing is ever mounted. A Quest game's .obb is a ZIP expansion
// file read directly; mountObb is for the older "jobb" image format.
enum { AOBB_STATE_MOUNTED = 1, AOBB_STATE_UNMOUNTED = 2, AOBB_STATE_ERROR_INTERNAL = 20,
       AOBB_STATE_ERROR_COULD_NOT_MOUNT = 21, AOBB_STATE_ERROR_COULD_NOT_UNMOUNT = 22,
       AOBB_STATE_ERROR_NOT_MOUNTED = 23, AOBB_STATE_ERROR_ALREADY_MOUNTED = 24,
       AOBB_STATE_ERROR_PERMISSION_DENIED = 25 };
typedef void (*kla_obb_cb)(const char *filename, int32_t state, void *data);
typedef struct { kla_obb_cb cb; char *name; int32_t state; void *data; } kla_obb_job;
static int g_storage_mgr;

static void *kla_obb_deliver(void *arg) {
    kla_obb_job *j = arg;
    if (j->cb) j->cb(j->name, j->state, j->data);
    free(j->name); free(j);
    return NULL;
}
static void kla_obb_post(kla_obb_cb cb, const char *name, int32_t state, void *data) {
    if (!cb) return;
    kla_obb_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->cb = cb; j->name = strdup(name ? name : ""); j->state = state; j->data = data;
    pthread_t th;
    if (kl_guest_thread_create(&th, 0, kla_obb_deliver, j) == 0) pthread_detach(th);
    else { free(j->name); free(j); }
}
static void *kla_AStorageManager_new(void) { return &g_storage_mgr; }
static void  kla_AStorageManager_delete(void *m) { (void)m; }
static void kla_AStorageManager_mountObb(void *m, const char *file, const char *key,
                                         kla_obb_cb cb, void *data) {
    (void)m; (void)key;
    struct stat st;
    int exists = file && stat(file, &st) == 0 && S_ISREG(st.st_mode);
    fprintf(stderr, "  [ndk] AStorageManager_mountObb(%s): %s\n", file ? file : "(null)",
            exists ? "OBB images cannot be mounted here — reporting COULD_NOT_MOUNT"
                   : "no such file — reporting COULD_NOT_MOUNT");
    kla_obb_post(cb, file, AOBB_STATE_ERROR_COULD_NOT_MOUNT, data);
}
static void kla_AStorageManager_unmountObb(void *m, const char *file, int force,
                                           kla_obb_cb cb, void *data) {
    (void)m; (void)force;
    kla_obb_post(cb, file, AOBB_STATE_ERROR_NOT_MOUNTED, data);   // nothing was ever mounted
}
static int kla_AStorageManager_isObbMounted(void *m, const char *file) { (void)m; (void)file; return 0; }
static const char *kla_AStorageManager_getMountedObbPath(void *m, const char *file) { (void)m; (void)file; return NULL; }

// AObbScanner: an OBB image ends with a footer — package version, flags, salt and
// the package name, closed by (footer size, signature 0x01059983). A ZIP-style
// expansion OBB has none, so the scanner answers NULL, as Android's does.
typedef struct { char *package; int32_t version, flags; } kla_obbinfo;
static uint32_t kla_le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void *kla_AObbScanner_getObbInfo(const char *filename) {
    if (!filename) return NULL;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL;
    kla_obbinfo *info = NULL;
    struct stat st;
    unsigned char tag[8];
    if (fstat(fd, &st) == 0 && st.st_size >= 33 &&
        pread(fd, tag, 8, st.st_size - 8) == 8 && kla_le32(tag + 4) == 0x01059983u) {
        uint32_t fsize = kla_le32(tag);
        if (fsize >= 25 && fsize <= 32768 && (off_t)(fsize + 8) <= st.st_size) {
            unsigned char *buf = malloc(fsize);
            if (buf && pread(fd, buf, fsize, st.st_size - 8 - fsize) == (ssize_t)fsize &&
                kla_le32(buf) == 1 /* signature version */) {
                uint32_t nlen = kla_le32(buf + 20);
                if (nlen > 0 && 24 + nlen <= fsize) {
                    info = calloc(1, sizeof *info);
                    if (info) {
                        info->version = (int32_t)kla_le32(buf + 4);
                        info->flags = (int32_t)kla_le32(buf + 8);
                        info->package = strndup((const char *)buf + 24, nlen);
                        if (!info->package) { free(info); info = NULL; }
                    }
                }
            }
            free(buf);
        }
    }
    close(fd);
    return info;
}
static void kla_AObbInfo_delete(void *i) { kla_obbinfo *o = i; if (o) { free(o->package); free(o); } }
static const char *kla_AObbInfo_getPackageName(const void *i) { return i ? ((const kla_obbinfo *)i)->package : NULL; }
static int32_t kla_AObbInfo_getVersion(const void *i) { return i ? ((const kla_obbinfo *)i)->version : 0; }
static int32_t kla_AObbInfo_getFlags(const void *i) { return i ? ((const kla_obbinfo *)i)->flags : 0; }

// ============================================================== ASharedMemory
// An anonymous file the guest can mmap and share. The pages are a temp file
// unlinked at creation: Darwin has no memfd, and shm_open is refused to a
// sandboxed app. setProt keeps Android's one-way rule — a mapping permission can
// be narrowed, never widened — but cannot be enforced against mmap here.
#define KLA_SHM_MAX 64
static struct { int fd; int prot; } g_shm[KLA_SHM_MAX];
static pthread_mutex_t g_shm_lock = PTHREAD_MUTEX_INITIALIZER;
static int kla_ASharedMemory_create(const char *name, size_t size) {
    (void)name;
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/klepton-shm-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) != 0) { int e = errno; close(fd); errno = e; return -1; }
    pthread_mutex_lock(&g_shm_lock);
    for (int i = 0; i < KLA_SHM_MAX; i++)
        if (!g_shm[i].fd) { g_shm[i].fd = fd; g_shm[i].prot = 7; break; }   // PROT_READ|WRITE|EXEC
    pthread_mutex_unlock(&g_shm_lock);
    return fd;
}
static size_t kla_ASharedMemory_getSize(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? (size_t)st.st_size : 0;      // 0 for a bad fd, as Android
}
static int kla_ASharedMemory_setProt(int fd, int prot) {
    if (prot & ~7) { errno = EINVAL; return -1; }
    int rc = 0;
    pthread_mutex_lock(&g_shm_lock);
    int known = 0;
    for (int i = 0; i < KLA_SHM_MAX; i++)
        if (g_shm[i].fd == fd) {
            known = 1;
            if (prot & ~g_shm[i].prot) { errno = EINVAL; rc = -1; }   // widening is refused
            else g_shm[i].prot = prot;
            break;
        }
    pthread_mutex_unlock(&g_shm_lock);
    if (!known) { errno = EBADF; return -1; }
    return rc;
}

// ================================================================== AInput*
// The accessor list for events of an input queue that never carries one (input
// reaches a VR guest through OpenXR/OVRPlugin). Unreachable without an event, so
// zeros; they exist so a guest built against native_app_glue binds by name.
#define KLA_I32_0(sym) static int32_t kla_##sym(const void *ev) { (void)ev; return 0; }
#define KLA_I64_0(sym) static int64_t kla_##sym(const void *ev) { (void)ev; return 0; }
#define KLA_F_0(sym)   static float   kla_##sym(const void *ev) { (void)ev; return 0.0f; }
#define KLA_I32_IDX(sym) static int32_t kla_##sym(const void *ev, size_t i) { (void)ev; (void)i; return 0; }
#define KLA_F_IDX(sym)   static float   kla_##sym(const void *ev, size_t i) { (void)ev; (void)i; return 0.0f; }
KLA_I32_0(AInputEvent_getDeviceId)   KLA_I32_0(AInputEvent_getSource)
KLA_I32_0(AKeyEvent_getFlags)        KLA_I32_0(AKeyEvent_getScanCode)
KLA_I32_0(AKeyEvent_getMetaState)    KLA_I32_0(AKeyEvent_getRepeatCount)
KLA_I64_0(AKeyEvent_getDownTime)     KLA_I64_0(AKeyEvent_getEventTime)
KLA_I32_0(AMotionEvent_getAction)    KLA_I32_0(AMotionEvent_getFlags)
KLA_I32_0(AMotionEvent_getMetaState) KLA_I32_0(AMotionEvent_getButtonState)
KLA_I32_0(AMotionEvent_getEdgeFlags) KLA_I32_0(AMotionEvent_getActionButton)
KLA_I32_0(AMotionEvent_getClassification)
KLA_I64_0(AMotionEvent_getDownTime)  KLA_I64_0(AMotionEvent_getEventTime)
KLA_F_0(AMotionEvent_getXOffset)     KLA_F_0(AMotionEvent_getYOffset)
KLA_F_0(AMotionEvent_getXPrecision)  KLA_F_0(AMotionEvent_getYPrecision)
static size_t kla_AMotionEvent_getPointerCount(const void *ev) { (void)ev; return 0; }
static size_t kla_AMotionEvent_getHistorySize(const void *ev) { (void)ev; return 0; }
KLA_I32_IDX(AMotionEvent_getPointerId) KLA_I32_IDX(AMotionEvent_getToolType)
KLA_F_IDX(AMotionEvent_getX)         KLA_F_IDX(AMotionEvent_getY)
KLA_F_IDX(AMotionEvent_getPressure)  KLA_F_IDX(AMotionEvent_getSize)
KLA_F_IDX(AMotionEvent_getTouchMajor) KLA_F_IDX(AMotionEvent_getTouchMinor)
KLA_F_IDX(AMotionEvent_getToolMajor) KLA_F_IDX(AMotionEvent_getToolMinor)
KLA_F_IDX(AMotionEvent_getOrientation)
static float kla_AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t i) {
    (void)ev; (void)axis; (void)i; return 0.0f;
}
static int64_t kla_AMotionEvent_getHistoricalEventTime(const void *ev, size_t h) { (void)ev; (void)h; return 0; }
#define KLA_HIST(sym) static float kla_##sym(const void *ev, size_t p, size_t h) { (void)ev; (void)p; (void)h; return 0.0f; }
KLA_HIST(AMotionEvent_getHistoricalRawX) KLA_HIST(AMotionEvent_getHistoricalRawY)
KLA_HIST(AMotionEvent_getHistoricalX)    KLA_HIST(AMotionEvent_getHistoricalY)
KLA_HIST(AMotionEvent_getHistoricalPressure) KLA_HIST(AMotionEvent_getHistoricalSize)
KLA_HIST(AMotionEvent_getHistoricalTouchMajor) KLA_HIST(AMotionEvent_getHistoricalTouchMinor)
KLA_HIST(AMotionEvent_getHistoricalToolMajor)  KLA_HIST(AMotionEvent_getHistoricalToolMinor)
KLA_HIST(AMotionEvent_getHistoricalOrientation)
static float kla_AMotionEvent_getHistoricalAxisValue(const void *ev, int32_t axis, size_t p, size_t h) {
    (void)ev; (void)axis; (void)p; (void)h; return 0.0f;
}
static int32_t kla_AInputQueue_hasEvents(void *q) { (void)q; return 0; }
// A Java KeyEvent/MotionEvent has no native event behind it here.
static void *kla_AInputEvent_fromJava(void *env, void *obj) { (void)env; (void)obj; return NULL; }
static void *kla_AInputQueue_fromJava(void *env, void *obj) { (void)env; (void)obj; return NULL; }
static void  kla_AInputEvent_release(const void *ev) { (void)ev; }

// ============================================================ ANativeActivity
static void kla_ANativeActivity_hideSoftInput(void *act, unsigned flags) {
    (void)act; (void)flags;      // there is no soft keyboard to hide (see showSoftInput in kl_ndk.c)
}
static void kla_ANativeActivity_setWindowFlags(void *act, uint32_t add, uint32_t remove) {
    (void)act;
    static int said;
    if (!said++)
        fprintf(stderr, "  [ndk] ANativeActivity_setWindowFlags(+0x%x, -0x%x) — recorded, not applied: "
                        "the headset's window is the guest's only window\n", add, remove);
}

// ================================================================== dispatch
static const struct { const char *name; void *fn; } g_android[] = {
#define N(sym, fn) {sym, (void *)(fn)}
#define NC(Name) N("AConfiguration_" #Name, kla_AConfiguration_##Name)
    NC(new), NC(delete), NC(fromAssetManager), NC(copy),
    NC(getMcc), NC(setMcc), NC(getMnc), NC(setMnc),
    NC(getLanguage), NC(setLanguage), NC(getCountry), NC(setCountry),
    NC(getOrientation), NC(setOrientation), NC(getTouchscreen), NC(setTouchscreen),
    NC(getDensity), NC(setDensity), NC(getKeyboard), NC(setKeyboard),
    NC(getNavigation), NC(setNavigation), NC(getKeysHidden), NC(setKeysHidden),
    NC(getNavHidden), NC(setNavHidden), NC(getSdkVersion), NC(setSdkVersion),
    NC(getScreenSize), NC(setScreenSize), NC(getScreenLong), NC(setScreenLong),
    NC(getUiModeType), NC(setUiModeType), NC(getUiModeNight), NC(setUiModeNight),
    NC(getScreenWidthDp), NC(setScreenWidthDp), NC(getScreenHeightDp), NC(setScreenHeightDp),
    NC(getSmallestScreenWidthDp), NC(setSmallestScreenWidthDp),
    NC(getLayoutDirection), NC(setLayoutDirection), NC(getScreenRound), NC(setScreenRound),
    NC(getColorMode), NC(setColorMode), NC(getGrammaticalGender), NC(setGrammaticalGender),
    NC(diff), NC(match), NC(isBetterThan),
#undef NC
    N("AChoreographer_getInstance",                  kla_AChoreographer_getInstance),
    N("AChoreographer_postFrameCallback",            kla_AChoreographer_postFrameCallback),
    N("AChoreographer_postFrameCallbackDelayed",     kla_AChoreographer_postFrameCallbackDelayed),
    N("AChoreographer_postFrameCallback64",          kla_AChoreographer_postFrameCallback64),
    N("AChoreographer_postFrameCallbackDelayed64",   kla_AChoreographer_postFrameCallbackDelayed64),
    N("AChoreographer_postVsyncCallback",            kla_AChoreographer_postVsyncCallback),
    N("AChoreographer_registerRefreshRateCallback",  kla_AChoreographer_registerRefreshRateCallback),
    N("AChoreographer_unregisterRefreshRateCallback", kla_AChoreographer_unregisterRefreshRateCallback),
    N("AChoreographerFrameCallbackData_getFrameTimeNanos", kla_FCD_getFrameTimeNanos),
    N("AChoreographerFrameCallbackData_getFrameTimelinesLength", kla_FCD_getFrameTimelinesLength),
    N("AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex", kla_FCD_getPreferredFrameTimelineIndex),
    N("AChoreographerFrameCallbackData_getFrameTimelineVsyncId", kla_FCD_getFrameTimelineVsyncId),
    N("AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos", kla_FCD_getFrameTimelineExpectedPresentationTimeNanos),
    N("AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos", kla_FCD_getFrameTimelineDeadlineNanos),

    N("AThermal_acquireManager",                kla_AThermal_acquireManager),
    N("AThermal_releaseManager",                kla_AThermal_releaseManager),
    N("AThermal_getCurrentThermalStatus",       kla_AThermal_getCurrentThermalStatus),
    N("AThermal_registerThermalStatusListener", kla_AThermal_registerThermalStatusListener),
    N("AThermal_unregisterThermalStatusListener", kla_AThermal_unregisterThermalStatusListener),
    N("AThermal_getThermalHeadroom",            kla_AThermal_getThermalHeadroom),

    N("APerformanceHint_getManager",               kla_APerformanceHint_getManager),
    N("APerformanceHint_getPreferredUpdateRateNanos", kla_APerformanceHint_getPreferredUpdateRateNanos),
    N("APerformanceHint_createSession",            kla_APerformanceHint_createSession),
    N("APerformanceHint_updateTargetWorkDuration", kla_APerformanceHint_updateTargetWorkDuration),
    N("APerformanceHint_reportActualWorkDuration", kla_APerformanceHint_reportActualWorkDuration),
    N("APerformanceHint_closeSession",             kla_APerformanceHint_closeSession),
    N("APerformanceHint_setThreads",               kla_APerformanceHint_setThreads),
    N("APerformanceHint_setPreferPowerEfficiency", kla_APerformanceHint_setPreferPowerEfficiency),

    N("AStorageManager_new",             kla_AStorageManager_new),
    N("AStorageManager_delete",          kla_AStorageManager_delete),
    N("AStorageManager_mountObb",        kla_AStorageManager_mountObb),
    N("AStorageManager_unmountObb",      kla_AStorageManager_unmountObb),
    N("AStorageManager_isObbMounted",    kla_AStorageManager_isObbMounted),
    N("AStorageManager_getMountedObbPath", kla_AStorageManager_getMountedObbPath),
    N("AObbScanner_getObbInfo",          kla_AObbScanner_getObbInfo),
    N("AObbInfo_delete",                 kla_AObbInfo_delete),
    N("AObbInfo_getPackageName",         kla_AObbInfo_getPackageName),
    N("AObbInfo_getVersion",             kla_AObbInfo_getVersion),
    N("AObbInfo_getFlags",               kla_AObbInfo_getFlags),

    N("ASharedMemory_create",  kla_ASharedMemory_create),
    N("ASharedMemory_getSize", kla_ASharedMemory_getSize),
    N("ASharedMemory_setProt", kla_ASharedMemory_setProt),

    N("AInputEvent_getDeviceId", kla_AInputEvent_getDeviceId), N("AInputEvent_getSource", kla_AInputEvent_getSource),
    N("AInputEvent_release", kla_AInputEvent_release),         N("AInputEvent_fromJava", kla_AInputEvent_fromJava),
    N("AInputQueue_fromJava", kla_AInputQueue_fromJava),       N("AInputQueue_hasEvents", kla_AInputQueue_hasEvents),
    N("AKeyEvent_getFlags", kla_AKeyEvent_getFlags),           N("AKeyEvent_getScanCode", kla_AKeyEvent_getScanCode),
    N("AKeyEvent_getMetaState", kla_AKeyEvent_getMetaState),   N("AKeyEvent_getRepeatCount", kla_AKeyEvent_getRepeatCount),
    N("AKeyEvent_getDownTime", kla_AKeyEvent_getDownTime),     N("AKeyEvent_getEventTime", kla_AKeyEvent_getEventTime),
    N("AMotionEvent_getAction", kla_AMotionEvent_getAction),   N("AMotionEvent_getFlags", kla_AMotionEvent_getFlags),
    N("AMotionEvent_getMetaState", kla_AMotionEvent_getMetaState), N("AMotionEvent_getButtonState", kla_AMotionEvent_getButtonState),
    N("AMotionEvent_getEdgeFlags", kla_AMotionEvent_getEdgeFlags), N("AMotionEvent_getActionButton", kla_AMotionEvent_getActionButton),
    N("AMotionEvent_getClassification", kla_AMotionEvent_getClassification),
    N("AMotionEvent_getDownTime", kla_AMotionEvent_getDownTime), N("AMotionEvent_getEventTime", kla_AMotionEvent_getEventTime),
    N("AMotionEvent_getXOffset", kla_AMotionEvent_getXOffset), N("AMotionEvent_getYOffset", kla_AMotionEvent_getYOffset),
    N("AMotionEvent_getXPrecision", kla_AMotionEvent_getXPrecision), N("AMotionEvent_getYPrecision", kla_AMotionEvent_getYPrecision),
    N("AMotionEvent_getPointerCount", kla_AMotionEvent_getPointerCount), N("AMotionEvent_getHistorySize", kla_AMotionEvent_getHistorySize),
    N("AMotionEvent_getPointerId", kla_AMotionEvent_getPointerId), N("AMotionEvent_getToolType", kla_AMotionEvent_getToolType),
    N("AMotionEvent_getX", kla_AMotionEvent_getX), N("AMotionEvent_getY", kla_AMotionEvent_getY),
    N("AMotionEvent_getPressure", kla_AMotionEvent_getPressure), N("AMotionEvent_getSize", kla_AMotionEvent_getSize),
    N("AMotionEvent_getTouchMajor", kla_AMotionEvent_getTouchMajor), N("AMotionEvent_getTouchMinor", kla_AMotionEvent_getTouchMinor),
    N("AMotionEvent_getToolMajor", kla_AMotionEvent_getToolMajor), N("AMotionEvent_getToolMinor", kla_AMotionEvent_getToolMinor),
    N("AMotionEvent_getOrientation", kla_AMotionEvent_getOrientation), N("AMotionEvent_getAxisValue", kla_AMotionEvent_getAxisValue),
    N("AMotionEvent_getHistoricalEventTime", kla_AMotionEvent_getHistoricalEventTime),
    N("AMotionEvent_getHistoricalRawX", kla_AMotionEvent_getHistoricalRawX), N("AMotionEvent_getHistoricalRawY", kla_AMotionEvent_getHistoricalRawY),
    N("AMotionEvent_getHistoricalX", kla_AMotionEvent_getHistoricalX), N("AMotionEvent_getHistoricalY", kla_AMotionEvent_getHistoricalY),
    N("AMotionEvent_getHistoricalPressure", kla_AMotionEvent_getHistoricalPressure), N("AMotionEvent_getHistoricalSize", kla_AMotionEvent_getHistoricalSize),
    N("AMotionEvent_getHistoricalTouchMajor", kla_AMotionEvent_getHistoricalTouchMajor), N("AMotionEvent_getHistoricalTouchMinor", kla_AMotionEvent_getHistoricalTouchMinor),
    N("AMotionEvent_getHistoricalToolMajor", kla_AMotionEvent_getHistoricalToolMajor), N("AMotionEvent_getHistoricalToolMinor", kla_AMotionEvent_getHistoricalToolMinor),
    N("AMotionEvent_getHistoricalOrientation", kla_AMotionEvent_getHistoricalOrientation), N("AMotionEvent_getHistoricalAxisValue", kla_AMotionEvent_getHistoricalAxisValue),

    N("ANativeActivity_hideSoftInput",  kla_ANativeActivity_hideSoftInput),
    N("ANativeActivity_setWindowFlags", kla_ANativeActivity_setWindowFlags),
#undef N
};

void *kl_android_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_android / sizeof g_android[0]; i++)
        if (strcmp(g_android[i].name, name) == 0) return g_android[i].fn;
    return NULL;
}
