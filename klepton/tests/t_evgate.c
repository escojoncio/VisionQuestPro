// Host test of the event-delivery gate (runtime/xr/kl_evgate.h): a completion event never reaches the guest in the
// drain of the frame that requested it, nor before the device's latency.
//   make evgate        (or tools/ci/host-tests.sh)
#include <stdio.h>
#include "xr/kl_evgate.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    // Requested before this frame's drain (MRUK.Awake): drains closed so far = 7.
    kl_evgate g = kl_evgate_stamp(7, 1000);
    CHECK(!kl_evgate_due(g, 7, 1000), "same frame, first poll of the drain");
    CHECK(!kl_evgate_due(g, 7, 5000), "same drain however late (a frame that takes seconds)");
    CHECK(!kl_evgate_due(g, 8, 1000 + KL_EVGATE_LATENCY_MS - 1), "next drain but before the latency (several drains a frame)");
    CHECK(kl_evgate_due(g, 8, 1000 + KL_EVGATE_LATENCY_MS), "next drain, latency passed");
    CHECK(kl_evgate_due(g, 20, 9000), "any later drain");

    // Requested from inside a drain (the handler of the previous completion): not in the same loop.
    kl_evgate h = kl_evgate_stamp(8, 2000);
    CHECK(!kl_evgate_due(h, 8, 2000 + 10 * KL_EVGATE_LATENCY_MS), "next iteration of the same drain");
    CHECK(kl_evgate_due(h, 9, 2000 + KL_EVGATE_LATENCY_MS), "the drain after it");

    if (fails) { printf("t_evgate: %d FAILED\n", fails); return 1; }
    printf("t_evgate: ok\n");
    return 0;
}
