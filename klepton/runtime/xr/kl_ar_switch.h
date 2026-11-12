// kl_ar_switch.h - diagnostic switches that turn off one piece of the spatial AR, to bisect in a single build which one breaks a
// game (passthrough, text, ray). Default: every switch off = every piece on = the behaviour without this file.
//   KL_NO_OVRM_BRIDGE=1   ovrp_UnityOpenXR_OnInstanceCreate / OnInstanceDestroy are success no-ops again (kl_ovrp.c)
//   KL_NO_ANCHOR_BRIDGE=1 no create hook: the ovrp anchor-create result is not delivered on the OpenXR queue (kl_openxr_spatial.c)
//   KL_NO_XR_DISCOVERY=1  XR_META_spatial_entity_discovery / room_mesh / semantic_label not announced, their entry points
//                         answer FEATURE_UNSUPPORTED (kl_openxr_spatial.c)
//   KL_NO_SCENE_RESCAN=1  no launch rescan, no L3+R3 rescan, no calib-mesh.bin read or written (kl_spatial.c)
// Each is read once (getenv, cached per translation unit); only the value "1" turns a piece off. The first time a switch acts it
// logs "[ar-switch] <NAME>=1 (pieza desactivada)".
#ifndef KL_AR_SWITCH_H
#define KL_AR_SWITCH_H
#include <stdio.h>
#include <stdlib.h>

enum { KL_ARSW_OVRM_BRIDGE, KL_ARSW_ANCHOR_BRIDGE, KL_ARSW_XR_DISCOVERY, KL_ARSW_SCENE_RESCAN, KL_ARSW_COUNT };

// 1 = the piece is disabled by its switch (logs once), 0 = it runs as always.
static inline int kl_ar_switch_off(int sw) {
    static const char *const name[KL_ARSW_COUNT] = { "KL_NO_OVRM_BRIDGE", "KL_NO_ANCHOR_BRIDGE", "KL_NO_XR_DISCOVERY", "KL_NO_SCENE_RESCAN" };
    static int state[KL_ARSW_COUNT];    // 0 unread, 1 piece on, 2 piece off
    static int logged[KL_ARSW_COUNT];
    if (sw < 0 || sw >= KL_ARSW_COUNT) return 0;
    int s = __atomic_load_n(&state[sw], __ATOMIC_RELAXED);
    if (!s) {
        const char *v = getenv(name[sw]);
        s = (v && v[0] == '1' && !v[1]) ? 2 : 1;
        __atomic_store_n(&state[sw], s, __ATOMIC_RELAXED);
    }
    if (s == 2 && !__atomic_exchange_n(&logged[sw], 1, __ATOMIC_RELAXED))
        fprintf(stderr, "  [ar-switch] %s=1 (pieza desactivada)\n", name[sw]);
    return s == 2;
}

#endif // KL_AR_SWITCH_H
