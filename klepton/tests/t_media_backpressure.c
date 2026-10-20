// Input backpressure of kl_mediandk.c (klm_bp_admit / klm_bp_stalled_us, cut out at build time).
// Models the failure that froze Demeo's intro: a guest that reads its whole file at once, a
// decoder that is much faster than the guest's 1x playback, and a decoded-frame ring of 16 that
// used to drop the oldest frames. With the cap nothing may ever be dropped.
#include <stdint.h>
#include <stdio.h>

#include "backpressure.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

#define RING 16
int main(void) {
    // Pure decisions.
    CHECK(klm_bp_admit(0, 8, 12, 0, 3000000), "empty pipeline admits");
    CHECK(klm_bp_admit(7, 8, 12, 0, 3000000), "below the cap admits");
    CHECK(!klm_bp_admit(8, 8, 12, 0, 3000000), "at the cap refuses");
    CHECK(!klm_bp_admit(8, 8, 12, 2999999, 3000000), "a short stall does not forgive");
    CHECK(klm_bp_admit(8, 8, 12, 3000000, 3000000), "a long stall forgives");
    CHECK(!klm_bp_admit(12, 8, 12, 9000000, 3000000), "never past the hard cap");
    CHECK(klm_bp_admit(100, 0, 12, 0, 3000000), "soft 0 disables the cap");

    uint64_t sig = 0; int64_t since = 0;
    CHECK(klm_bp_stalled_us(&sig, &since, 5, 1000) == 0, "first sample starts the clock");
    CHECK(klm_bp_stalled_us(&sig, &since, 5, 4000) == 3000, "same signature: time since it");
    CHECK(klm_bp_stalled_us(&sig, &since, 6, 5000) == 0, "progress resets the stall");

    // Flood model: 300-frame clip, guest feeds as fast as it is admitted, decoder emits a frame
    // per ms, guest consumes (dequeues+releases) one frame every 33 ms. Frames in flight =
    // in decoder + ring; the ring must never overflow and every frame must arrive in order.
    int fed = 0, in_vt = 0, ring = 0, consumed = 0, dropped = 0, last = -1, order_ok = 1;
    int q[RING * 4], qh = 0, qt = 0, maxflight = 0, refused = 0;
    uint64_t progress = 0; uint64_t sig2 = 0; int64_t since2 = 0;
    for (int64_t t = 1; consumed < 300 && t < 20000000; t++) {
        if (t % 1 == 0 && fed < 300) {              // guest tries to feed every ms
            int inflight = in_vt + ring;
            int64_t st = klm_bp_stalled_us(&sig2, &since2, progress, t * 1000);
            if (klm_bp_admit(inflight, 8, 12, st, 3000000)) { in_vt++; fed++; q[qt++ % (RING * 4)] = fed; }
            else refused++;
        }
        if (t % 2 == 0 && in_vt > 0) {              // decoder: one frame per 2 ms
            in_vt--;
            if (ring == RING) { dropped++; ring--; }
            ring++; progress++;
        }
        if (t % 33 == 0 && ring > 0) {              // guest: 1x playback
            int f = q[qh++ % (RING * 4)];
            ring--; consumed++; progress++;
            if (f < last) order_ok = 0;
            last = f;
        }
        if (in_vt + ring > maxflight) maxflight = in_vt + ring;
    }
    CHECK(consumed == 300, "every frame is played");
    CHECK(dropped == 0, "the ring never drops a frame");
    CHECK(order_ok, "in order");
    CHECK(maxflight <= 8, "in flight never passes the cap");
    CHECK(refused > 0, "the flood was actually held back");
    if (fails) return 1;
    printf("t_media_backpressure ok\n");
    return 0;
}
