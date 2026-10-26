// In-game trigger for VPKEngine's frame capture (vpvk_capture_request in VPVulkan, vpcap.h): hold a button
// combination for a few seconds and the next frame is captured. No 2D UI is involved, so it works inside the
// immersive space. The app turns it on per game (KL_DIAGCAP=1, with VPVK_CAPTURE=1 so VPKEngine tracks objects from
// the start); the combination and the hold time come from KL_DIAGCAP_BTN / KL_DIAGCAP_HOLD_MS.
#ifndef KL_DIAGCAP_H
#define KL_DIAGCAP_H

#include <stdint.h>

// KL_DIAGCAP_BTN values.
enum {
    KL_DIAGCAP_STICK_L  = 0,   // left thumbstick click
    KL_DIAGCAP_STICK_R  = 1,   // right thumbstick click
    KL_DIAGCAP_STICKS   = 2,   // both thumbstick clicks together
    KL_DIAGCAP_TRIGGERS = 3,   // both index triggers (hand tracking: both pinches)
};

enum { KL_DIAGCAP_HOLD_MIN_MS = 500, KL_DIAGCAP_HOLD_MAX_MS = 30000 };

// The hold detector alone (no clock, no I/O): `need[h]` are the raw buttons hand h must hold (0 = hand not needed).
typedef struct {
    uint32_t need[2];
    uint32_t hold_ms;
    uint32_t cur[2];       // last button word seen per hand
    int      active;       // the combination is held
    int      fired;        // already fired in this hold: needs a release first
    uint64_t since_ms;     // when the combination became held
} kl_diagcap;

void kl_diagcap_init(kl_diagcap *c, int mode, uint32_t hold_ms);
// A button word of `hand` at `now_ms`. Returns 1 once per hold, when the combination has been held for hold_ms.
int  kl_diagcap_step(kl_diagcap *c, int hand, uint32_t buttons, uint64_t now_ms);

// kl_ovrp_set_controller_input: every button word of both hands. No-op unless KL_DIAGCAP=1. On a fire:
// vpvk_capture_request(1, NULL) (dlsym), the log line "[cap] requested rc=<n>" and a haptic pulse on both hands.
void kl_diagcap_input(int hand, uint32_t buttons);

#endif
