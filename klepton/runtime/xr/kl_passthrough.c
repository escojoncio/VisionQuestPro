// kl_passthrough — see kl_passthrough.h.
#include "kl_passthrough.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int g_present;      // last frame's verdict from a shim
static atomic_int g_unpremul;
static atomic_int g_force;        // the host asked for passthrough (guardian screen), whatever the guest submits

// [pt-sum]: one line every 5 s of frames submitted (same cadence as [sp-sum]; own clock, t = seconds since the first
// frame). Counts are for the 5 s window just closed; `layer` is the last passthrough layer id seen (-1 none yet),
// `active` the verdict of the last frame. Capped at PT_SUM_LINES (1 h) like the other permanent counters.
//   [pt-sum] t=<s>s layer=<id> active=<0|1> frames=<EndFrames> present=<with a passthrough layer> underlay=<n> overlay=<n>
//            premul=<n> straight=<n>      (premul + straight = frames; underlay + overlay = present; each is its own counter)
#define PT_SUM_EVERY_NS 5000000000ull
#define PT_SUM_LINES 720
static atomic_uint g_sum_premul, g_sum_straight, g_sum_under, g_sum_over;
static atomic_int g_sum_layer = -1;
static atomic_ullong g_sum_t0, g_sum_due;
static atomic_uint g_sum_lines;

static void pt_sum_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long now = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    unsigned long long due = atomic_load(&g_sum_due);
    if (!due) {
        unsigned long long zero = 0;
        if (atomic_compare_exchange_strong(&g_sum_due, &zero, now + PT_SUM_EVERY_NS)) atomic_store(&g_sum_t0, now);
        return;
    }
    if (now < due || !atomic_compare_exchange_strong(&g_sum_due, &due, now + PT_SUM_EVERY_NS)) return;
    unsigned premul = atomic_exchange(&g_sum_premul, 0), straight = atomic_exchange(&g_sum_straight, 0);
    unsigned under = atomic_exchange(&g_sum_under, 0), over = atomic_exchange(&g_sum_over, 0);
    if (atomic_fetch_add(&g_sum_lines, 1) >= PT_SUM_LINES) return;
    fprintf(stderr, "[pt-sum] t=%.0fs layer=%d active=%d frames=%u present=%u underlay=%u overlay=%u premul=%u straight=%u\n",
            (double)(now - atomic_load(&g_sum_t0)) / 1e9, atomic_load(&g_sum_layer), atomic_load(&g_present),
            premul + straight, under + over, under, over, premul, straight);
}

int kl_passthrough_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("KL_PASSTHROUGH");
        mode = 0;
        if (e && (!strcmp(e, "off") || !strcmp(e, "0"))) mode = 1;
        else if (e && (!strcmp(e, "on") || !strcmp(e, "1"))) mode = 2;
        fprintf(stderr, "[pt] mode %s (KL_PASSTHROUGH=%s)\n",
                mode == 0 ? "auto" : mode == 1 ? "off (always VR)" : "on (always mixed)",
                e ? e : "unset");
    }
    return mode;
}

void kl_passthrough_note_frame(int present, int layer, int underlay, int unpremultiplied) {
    present = present != 0;
    atomic_store(&g_unpremul, unpremultiplied != 0);
    atomic_fetch_add(unpremultiplied ? &g_sum_straight : &g_sum_premul, 1);
    if (present) {
        atomic_fetch_add(underlay ? &g_sum_under : &g_sum_over, 1);
        atomic_store(&g_sum_layer, layer);
    }
    int was = atomic_exchange(&g_present, present);
    if (was != present) {
        if (present)
            fprintf(stderr, "[pt] passthrough ON (layer %d, %s, %s alpha)\n", layer,
                    underlay ? "underlay" : "overlay/blend-mode",
                    unpremultiplied ? "straight" : "premultiplied");
        else
            fprintf(stderr, "[pt] passthrough OFF\n");
    }
    pt_sum_tick();
}

void kl_passthrough_force(int on) {
    int was = atomic_exchange(&g_force, on != 0);
    if (was != (on != 0)) fprintf(stderr, "[pt] passthrough forced %s by the host\n", on ? "ON" : "OFF");
}

int kl_passthrough_forced(void) { return atomic_load(&g_force); }

int kl_passthrough_active(void) {
    if (atomic_load(&g_force)) return 1;       // wins over KL_PASSTHROUGH=off: the guardian screen cannot be shown in black
    switch (kl_passthrough_mode()) {
    case 1: return 0;
    case 2: return 1;
    default: return atomic_load(&g_present);
    }
}

int kl_passthrough_unpremultiplied(void) { return atomic_load(&g_unpremul); }
