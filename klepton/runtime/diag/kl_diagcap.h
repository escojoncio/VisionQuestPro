// In-game trigger for VPKEngine's frame capture (vpvk_capture_request in VPVulkan, vpcap.h): press a button
// combination (by default the instant it goes down; optionally hold it for a few seconds) and the next frame is
// captured. No 2D UI is involved, so it works inside the immersive space. The app turns it on per game
// (KL_DIAGCAP=1, with VPVK_CAPTURE=1 so VPKEngine tracks objects from the start); the combination and the hold time
// come from KL_DIAGCAP_BTN / KL_DIAGCAP_HOLD_MS (default 0 = edge-triggered on the press).
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

// DEBOUNCE: after a fire, no new one for this long, so one press (contact bounce, a combination that flickers) is one request.
enum { KL_DIAGCAP_HOLD_MIN_MS = 0, KL_DIAGCAP_HOLD_MAX_MS = 30000, KL_DIAGCAP_DEBOUNCE_MS = 300 };

// The hold detector alone (no clock, no I/O): `need[h]` are the raw buttons hand h must hold (0 = hand not needed).
typedef struct {
    uint32_t need[2];
    uint32_t hold_ms;
    uint32_t cur[2];       // last button word seen per hand
    int      active;       // the combination is held
    int      fired;        // already fired in this hold: needs a release first
    uint64_t since_ms;     // when the combination became held
    int      has_fired;    // a fire happened (last_fire_ms is valid)
    uint64_t last_fire_ms; // when it fired last, for the debounce
} kl_diagcap;

void kl_diagcap_init(kl_diagcap *c, int mode, uint32_t hold_ms);
// A button word of `hand` at `now_ms`. Returns 1 once per press, when the combination has been held for hold_ms
// (hold_ms == 0: at the very sample where it goes down); a release re-arms it, but not within the debounce time.
int  kl_diagcap_step(kl_diagcap *c, int hand, uint32_t buttons, uint64_t now_ms);

// kl_ovrp_set_controller_input: every button word of both hands. No-op unless KL_DIAGCAP=1. On a fire:
// vpvk_capture_request(1, NULL) (dlsym), the log line "[cap] requested rc=<n>" and a haptic pulse on both hands.
void kl_diagcap_input(int hand, uint32_t buttons);

#endif
