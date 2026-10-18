// kl_jni_sysinfo.c — the small android.os / java.util classes engines query for
// a device report: SystemClock, Process, PowerManager (+WakeLock), BatteryManager
// extras, Build statics, Debug and the rest of java.util.Locale.
//
// Everything here is the Quest's, not a phone's: the screen is always on, there
// is no power-saver mode, wake locks are honoured by existing (the visor does not
// sleep while a game runs), and the thermal state is the visor's own.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_android.h"
#include "kl_jni_int.h"

#define KLJ_BOOL(x) ((klj_val){.j = (x) ? 1u : 0u})

// ---- SystemClock ----
// elapsedRealtime counts suspend; uptime does not. The visor does not suspend a
// running game, so both read the same monotonic clock apart from their units.
static int64_t klj_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static klj_val klj_SC_elapsedMs(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)(klj_mono_ns() / 1000000)};
}
static klj_val klj_SC_elapsedNs(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)klj_mono_ns()};
}
static klj_val klj_SC_threadMs(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000)};
}
static klj_val klj_SC_sleep(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s;
    int64_t ms = n > 0 ? (int64_t)a[0].j : 0;
    if (ms > 0) {
        struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
        nanosleep(&ts, NULL);
    }
    return (klj_val){.j = 0};
}
static klj_val klj_SC_currentNetworkTimeMillis(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    // Android throws DateTimeException when no NTP time was fetched; offline, none was.
    klj_throw_new("java/time/DateTimeException", "Missing NTP fix");
    return (klj_val){.j = 0};
}

// ---- Process ----
#define KLJ_APP_UID 10123      /* a normal app uid (u0_a123); synthetic, constant */
static klj_val klj_Proc_myPid(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint32_t)getpid()};
}
static klj_val klj_Proc_myUid(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = KLJ_APP_UID};
}
static klj_val klj_Proc_true(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return KLJ_BOOL(1);
}
static klj_val klj_Proc_zero(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_Proc_cpuMs(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000)};
}

// ---- PowerManager ----
static klj_val klj_PM_interactive(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return KLJ_BOOL(1);        // a worn visor running a game: the "screen" is on
}
static klj_val klj_PM_false(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return KLJ_BOOL(0);
}
static klj_val klj_PM_thermal(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint32_t)kl_android_thermal_status()};
}
static klj_val klj_PM_headroom(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.d = NAN};    // "unavailable", as on a device without the HAL
}
static klj_val klj_PM_wakeLockSupported(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s;
    int lvl = n > 0 ? (int)(int32_t)a[0].j : 0;
    return KLJ_BOOL(lvl == 1 || lvl == 6 || lvl == 10 || lvl == 26 || lvl == 32 || lvl == 64 || lvl == 128);
}

// WakeLock: held state is per object, kept as a count when reference counted.
typedef struct { int held; } klj_wakelock;
static klj_wakelock g_wl[16];
static unsigned g_wl_n;
static klj_wakelock *klj_wl_of(void *self) {
    static void *objs[16];
    for (unsigned i = 0; i < g_wl_n; i++) if (objs[i] == self) return &g_wl[i];
    if (g_wl_n >= 16) return &g_wl[15];
    objs[g_wl_n] = self;
    return &g_wl[g_wl_n++];
}
static klj_val klj_PM_newWakeLock(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s;
    const char *tag = n > 1 ? klj_str(a[1].l) : NULL;
    void *wl = kl_jni_new_object("android/os/PowerManager$WakeLock");
    if (wl) klj_as_object(wl)->pinned = 1;
    KLJ_LOG("PowerManager.newWakeLock(level %d, \"%s\")", n > 0 ? (int)a[0].j : 0, tag ? tag : "");
    return (klj_val){.l = wl};
}
static klj_val klj_WL_acquire(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    klj_wl_of(s)->held++;
    return (klj_val){.j = 0};
}
static klj_val klj_WL_release(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    klj_wakelock *w = klj_wl_of(s);
    // Reference-counted release of an unheld lock throws RuntimeException on Android.
    if (w->held <= 0) { klj_throw_new("java/lang/RuntimeException", "WakeLock under-locked"); return (klj_val){.j = 0}; }
    w->held--;
    return (klj_val){.j = 0};
}
static klj_val klj_WL_isHeld(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    return KLJ_BOOL(klj_wl_of(s)->held > 0);
}

// ---- BatteryManager extras ----
static klj_val klj_BM_longProp(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)INT64_MIN};   // Long.MIN_VALUE: property not exposed
}
static klj_val klj_BM_minus1(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)-1ll};
}

// ---- Build statics ----
static klj_val klj_Build_getSerial(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("unknown")};
}
static klj_val klj_Build_getRadioVersion(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("")};
}
static klj_val klj_Build_getFingerprintedPartitions(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.l = klj_new_list(NULL, 0)};
}

// ---- Debug ----
static klj_val klj_Debug_nativeHeap(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// ---- java.util.Locale ----
typedef struct { const char *code, *name, *iso3; } klj_langname;
static const klj_langname g_langs[] = {
    {"en", "English", "eng"}, {"es", "Spanish", "spa"}, {"de", "German", "deu"},
    {"fr", "French", "fra"}, {"it", "Italian", "ita"}, {"pt", "Portuguese", "por"},
    {"ja", "Japanese", "jpn"}, {"ko", "Korean", "kor"}, {"zh", "Chinese", "zho"},
    {"ru", "Russian", "rus"}, {"nl", "Dutch", "nld"}, {"pl", "Polish", "pol"},
};
static const klj_langname *klj_lang(const char *c) {
    for (size_t i = 0; i < sizeof g_langs / sizeof *g_langs; i++)
        if (!strcmp(g_langs[i].code, c)) return &g_langs[i];
    return NULL;
}
typedef struct { const char *code, *name, *iso3; } klj_ctryname;
static const klj_ctryname g_ctrys[] = {
    {"US", "United States", "USA"}, {"GB", "United Kingdom", "GBR"}, {"ES", "Spain", "ESP"},
    {"DE", "Germany", "DEU"}, {"FR", "France", "FRA"}, {"IT", "Italy", "ITA"},
    {"JP", "Japan", "JPN"}, {"KR", "South Korea", "KOR"}, {"CN", "China", "CHN"},
    {"BR", "Brazil", "BRA"}, {"MX", "Mexico", "MEX"}, {"CA", "Canada", "CAN"},
};
static const klj_ctryname *klj_ctry(const char *c) {
    for (size_t i = 0; i < sizeof g_ctrys / sizeof *g_ctrys; i++)
        if (!strcmp(g_ctrys[i].code, c)) return &g_ctrys[i];
    return NULL;
}
static void klj_lc(char *l, size_t ls, char *c, size_t cs) { kl_jni_locale_parts(l, ls, c, cs); }

static klj_val klj_Loc_displayLanguage(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16]; klj_lc(l, sizeof l, c, sizeof c);
    const klj_langname *x = klj_lang(l);
    return (klj_val){.l = kl_jni_new_string(x ? x->name : l)};
}
static klj_val klj_Loc_displayCountry(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16]; klj_lc(l, sizeof l, c, sizeof c);
    const klj_ctryname *x = klj_ctry(c);
    return (klj_val){.l = kl_jni_new_string(x ? x->name : c)};
}
static klj_val klj_Loc_displayName(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16], buf[96]; klj_lc(l, sizeof l, c, sizeof c);
    const klj_langname *x = klj_lang(l);
    const klj_ctryname *y = klj_ctry(c);
    if (*c) snprintf(buf, sizeof buf, "%s (%s)", x ? x->name : l, y ? y->name : c);
    else    snprintf(buf, sizeof buf, "%s", x ? x->name : l);
    return (klj_val){.l = kl_jni_new_string(buf)};
}
static klj_val klj_Loc_toString(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16], buf[40]; klj_lc(l, sizeof l, c, sizeof c);
    if (*c) snprintf(buf, sizeof buf, "%s_%s", l, c); else snprintf(buf, sizeof buf, "%s", l);
    return (klj_val){.l = kl_jni_new_string(buf)};
}
static klj_val klj_Loc_iso3Language(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16]; klj_lc(l, sizeof l, c, sizeof c);
    const klj_langname *x = klj_lang(l);
    return (klj_val){.l = kl_jni_new_string(x ? x->iso3 : l)};
}
static klj_val klj_Loc_iso3Country(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    char l[16], c[16]; klj_lc(l, sizeof l, c, sizeof c);
    const klj_ctryname *y = klj_ctry(c);
    return (klj_val){.l = kl_jni_new_string(y ? y->iso3 : c)};
}
static klj_val klj_Loc_empty(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("")};
}
static klj_val klj_Loc_noop(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    return (klj_val){.j = 0};
}

const klj_binding klj_bind_sysinfo[] = {
    {"android/os/SystemClock", "elapsedRealtime",      "()J", klj_SC_elapsedMs},
    {"android/os/SystemClock", "elapsedRealtimeNanos", "()J", klj_SC_elapsedNs},
    {"android/os/SystemClock", "uptimeMillis",         "()J", klj_SC_elapsedMs},
    {"android/os/SystemClock", "uptimeNanos",          "()J", klj_SC_elapsedNs},
    {"android/os/SystemClock", "currentThreadTimeMillis", "()J", klj_SC_threadMs},
    {"android/os/SystemClock", "currentThreadTimeMicro",  "()J", klj_SC_threadMs},
    {"android/os/SystemClock", "sleep",                "(J)V", klj_SC_sleep},
    {"android/os/SystemClock", "currentNetworkTimeMillis", "()J", klj_SC_currentNetworkTimeMillis},

    {"android/os/Process", "myPid",          "()I", klj_Proc_myPid},
    {"android/os/Process", "myUid",          "()I", klj_Proc_myUid},
    {"android/os/Process", "is64Bit",        "()Z", klj_Proc_true},
    {"android/os/Process", "supportsProcesses", "()Z", klj_Proc_true},
    {"android/os/Process", "isApplicationUid", "(I)Z", klj_Proc_true},
    {"android/os/Process", "getThreadPriority", "(I)I", klj_Proc_zero},
    {"android/os/Process", "getElapsedCpuTime", "()J", klj_Proc_cpuMs},

    {"android/os/PowerManager", "isInteractive",     "()Z", klj_PM_interactive},
    {"android/os/PowerManager", "isScreenOn",        "()Z", klj_PM_interactive},
    {"android/os/PowerManager", "isPowerSaveMode",   "()Z", klj_PM_false},
    {"android/os/PowerManager", "isDeviceIdleMode",  "()Z", klj_PM_false},
    {"android/os/PowerManager", "isLightDeviceIdleMode", "()Z", klj_PM_false},
    {"android/os/PowerManager", "isIgnoringBatteryOptimizations", "(Ljava/lang/String;)Z", klj_PM_false},
    {"android/os/PowerManager", "isWakeLockLevelSupported", "(I)Z", klj_PM_wakeLockSupported},
    {"android/os/PowerManager", "getCurrentThermalStatus", "()I", klj_PM_thermal},
    {"android/os/PowerManager", "getThermalHeadroom", "(I)F", klj_PM_headroom},
    {"android/os/PowerManager", "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;", klj_PM_newWakeLock},
    {"android/os/PowerManager$WakeLock", "acquire",  "()V",  klj_WL_acquire},
    {"android/os/PowerManager$WakeLock", "acquire",  "(J)V", klj_WL_acquire},
    {"android/os/PowerManager$WakeLock", "release",  "()V",  klj_WL_release},
    {"android/os/PowerManager$WakeLock", "release",  "(I)V", klj_WL_release},
    {"android/os/PowerManager$WakeLock", "isHeld",   "()Z",  klj_WL_isHeld},
    {"android/os/PowerManager$WakeLock", "setReferenceCounted", "(Z)V", klj_Loc_noop},

    {"android/os/BatteryManager", "getLongProperty", "(I)J", klj_BM_longProp},
    {"android/os/BatteryManager", "computeChargeTimeRemaining", "()J", klj_BM_minus1},

    {"android/os/Build", "getSerial",       "()Ljava/lang/String;", klj_Build_getSerial},
    {"android/os/Build", "getRadioVersion", "()Ljava/lang/String;", klj_Build_getRadioVersion},
    {"android/os/Build", "getFingerprintedPartitions", "()Ljava/util/List;", klj_Build_getFingerprintedPartitions},

    {"android/os/Debug", "isDebuggerConnected", "()Z", klj_PM_false},
    {"android/os/Debug", "waitingForDebugger",  "()Z", klj_PM_false},
    {"android/os/Debug", "getNativeHeapSize",          "()J", klj_Debug_nativeHeap},
    {"android/os/Debug", "getNativeHeapAllocatedSize", "()J", klj_Debug_nativeHeap},
    {"android/os/Debug", "getNativeHeapFreeSize",      "()J", klj_Debug_nativeHeap},

    {"java/util/Locale", "getDisplayLanguage", "()Ljava/lang/String;", klj_Loc_displayLanguage},
    {"java/util/Locale", "getDisplayCountry",  "()Ljava/lang/String;", klj_Loc_displayCountry},
    {"java/util/Locale", "getDisplayName",     "()Ljava/lang/String;", klj_Loc_displayName},
    {"java/util/Locale", "toString",           "()Ljava/lang/String;", klj_Loc_toString},
    {"java/util/Locale", "getISO3Language",    "()Ljava/lang/String;", klj_Loc_iso3Language},
    {"java/util/Locale", "getISO3Country",     "()Ljava/lang/String;", klj_Loc_iso3Country},
    {"java/util/Locale", "getVariant",         "()Ljava/lang/String;", klj_Loc_empty},
    {"java/util/Locale", "getScript",          "()Ljava/lang/String;", klj_Loc_empty},
    {"java/util/Locale", "setDefault",         "(Ljava/util/Locale;)V", klj_Loc_noop},
    {NULL, NULL, NULL, NULL},
};
