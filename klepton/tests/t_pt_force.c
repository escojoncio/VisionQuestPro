// kl_passthrough_force: the guardian screen asks for passthrough whatever the guest (not started) submits.
// clang -Wall -Iruntime/xr tests/t_pt_force.c runtime/xr/kl_passthrough.c -o /tmp/t_pt_force && /tmp/t_pt_force
#include <stdio.h>
#include "kl_passthrough.h"

static int fails;
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    CK(kl_passthrough_mode() == 0);                  // KL_PASSTHROUGH unset: auto
    CK(kl_passthrough_active() == 0);                // no frame from a guest yet
    CK(kl_passthrough_forced() == 0);
    kl_passthrough_force(1);                         // guardian screen opens
    CK(kl_passthrough_forced() == 1);
    CK(kl_passthrough_active() == 1);
    kl_passthrough_note_frame(0, -1, 0, 0);          // a frame without a passthrough layer does not undo it
    CK(kl_passthrough_active() == 1);
    kl_passthrough_force(0);                         // guardian ends: back to what the guest says
    CK(kl_passthrough_forced() == 0);
    CK(kl_passthrough_active() == 0);
    kl_passthrough_note_frame(1, 3, 1, 0);
    CK(kl_passthrough_active() == 1);                // the guest's own request still works
    kl_passthrough_force(1);
    kl_passthrough_force(7);                         // any non-zero is a force
    CK(kl_passthrough_forced() == 1);
    kl_passthrough_note_frame(0, -1, 0, 0);
    kl_passthrough_force(0);
    CK(kl_passthrough_active() == 0);
    printf(fails ? "t_pt_force: %d FAILED\n" : "t_pt_force: all passed\n", fails);
    return fails != 0;
}
