// kl_passthrough — see kl_passthrough.h.
#include "kl_passthrough.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_present;      // last frame's verdict from a shim
static atomic_int g_unpremul;

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
    int was = atomic_exchange(&g_present, present);
    if (was != present) {
        if (present)
            fprintf(stderr, "[pt] passthrough ON (layer %d, %s, %s alpha)\n", layer,
                    underlay ? "underlay" : "overlay/blend-mode",
                    unpremultiplied ? "straight" : "premultiplied");
        else
            fprintf(stderr, "[pt] passthrough OFF\n");
    }
}

int kl_passthrough_active(void) {
    switch (kl_passthrough_mode()) {
    case 1: return 0;
    case 2: return 1;
    default: return atomic_load(&g_present);
    }
}

int kl_passthrough_unpremultiplied(void) { return atomic_load(&g_unpremul); }
