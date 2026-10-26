// In-game trigger for VPKEngine's frame capture; see kl_diagcap.h.
#include "kl_diagcap.h"

#include <dlfcn.h>
#include <stdio.h>
#include <time.h>

#include "kl_env.h"
#include "kl_ovrp.h"

void kl_diagcap_init(kl_diagcap *c, int mode, uint32_t hold_ms) {
    *c = (kl_diagcap){0};
    switch (mode) {
    case KL_DIAGCAP_STICK_R:  c->need[1] = KL_OVRP_RAW_RTHUMBSTICK; break;
    case KL_DIAGCAP_STICKS:   c->need[0] = KL_OVRP_RAW_LTHUMBSTICK; c->need[1] = KL_OVRP_RAW_RTHUMBSTICK; break;
    case KL_DIAGCAP_TRIGGERS: c->need[0] = KL_OVRP_RAW_LINDEX_TRIGGER; c->need[1] = KL_OVRP_RAW_RINDEX_TRIGGER; break;
    default:                  c->need[0] = KL_OVRP_RAW_LTHUMBSTICK; break;
    }
    c->hold_ms = hold_ms < KL_DIAGCAP_HOLD_MIN_MS ? KL_DIAGCAP_HOLD_MIN_MS
               : hold_ms > KL_DIAGCAP_HOLD_MAX_MS ? KL_DIAGCAP_HOLD_MAX_MS : hold_ms;
}

int kl_diagcap_step(kl_diagcap *c, int hand, uint32_t buttons, uint64_t now_ms) {
    if ((unsigned)hand > 1) return 0;
    c->cur[hand] = buttons;
    int held = 1;
    for (int h = 0; h < 2; h++)
        if (c->need[h] && (c->cur[h] & c->need[h]) != c->need[h]) held = 0;
    if (!held) { c->active = 0; c->fired = 0; return 0; }
    if (!c->active) { c->active = 1; c->since_ms = now_ms; return 0; }
    if (c->fired || now_ms < c->since_ms + c->hold_ms) return 0;
    c->fired = 1;
    return 1;
}

static void fire(void) {
    int (*request)(uint32_t, const char *) = (int (*)(uint32_t, const char *))dlsym(RTLD_DEFAULT, "vpvk_capture_request");
    // rc: 1 queued, 0 a capture is already pending/running, -1 tracking off, -2 this driver has no capture API.
    int rc = request ? request(1, NULL) : -2;
    fprintf(stderr, "[cap] requested rc=%d (%s)\n", rc, rc == 1 ? "queued" : rc == 0 ? "already pending"
            : rc == -1 ? "tracking off" : "vpvk_capture_request not in this driver");
    // A short strong pulse = queued; a long weak one = it did not start.
    for (int h = 0; h < 2; h++) kl_ovrp_haptics_apply(h, rc == 1 ? 0.8f : 0.3f, rc == 1 ? 0.3f : 0.8f);
}

void kl_diagcap_input(int hand, uint32_t buttons) {
    static int on = -1;
    static kl_diagcap c;
    if (on < 0) {
        on = kl_env_on("KL_DIAGCAP", 0);
        if (on) {
            kl_diagcap_init(&c, kl_env_int("KL_DIAGCAP_BTN", KL_DIAGCAP_STICK_L), kl_env_uint("KL_DIAGCAP_HOLD_MS", 3000));
            fprintf(stderr, "[cap] trigger on: buttons %#x/%#x held %u ms\n", c.need[0], c.need[1], c.hold_ms);
        }
    }
    if (!on) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (kl_diagcap_step(&c, hand, buttons, (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u)) fire();
}
