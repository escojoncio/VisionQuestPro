// runtime/diag/kl_logcap.h: the cap rule behind the [sp-life] / [life] / [net] / [text] lines.
// gcc -Wall -Wextra -Iruntime/diag tests/t_logcap.c -o /tmp/t_logcap
#include <stdio.h>
#include "kl_logcap.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    // first 3, then 1 in 10
    unsigned w = 0;
    for (unsigned n = 1; n <= 100; n++) w += (unsigned)kl_logcap_pass(n, 3, 10);
    CHECK(w == 3 + 10, "first 3 plus every 10th of 100 (10,20,...,100) = 13");
    CHECK(kl_logcap_pass(1, 3, 10) && kl_logcap_pass(3, 3, 10) && !kl_logcap_pass(4, 3, 10), "the first window ends at `first`");
    CHECK(kl_logcap_pass(10, 3, 10) && !kl_logcap_pass(11, 3, 10) && kl_logcap_pass(20, 3, 10), "then multiples of `every`");
    CHECK(!kl_logcap_pass(0, 3, 10), "occurrence 0 is never written");
    // every == 0: no periodic line
    w = 0;
    for (unsigned n = 1; n <= 1000; n++) w += (unsigned)kl_logcap_pass(n, 5, 0);
    CHECK(w == 5, "every=0: only the first 5 of 1000");
    CHECK(!kl_logcap_pass(1, 0, 0) && kl_logcap_pass(7, 0, 7), "first=0 writes only the periodic ones");

    // hit(): counter + number
    unsigned c = 0, n = 0, wrote = 0;
    for (int i = 0; i < 50; i++) { if (kl_logcap_hit(&c, 2, 25, &n)) wrote++; }
    CHECK(c == 50 && n == 50 && wrote == 2 + 2, "hit over 50: 1, 2, 25, 50");
    CHECK(kl_logcap_hit(&c, 2, 25, NULL) == 0 && c == 51, "NULL nout accepted, 51st not written");

    // overdue(): once per request, after the limit, never on a backwards clock
    const uint64_t S = 1000000000ull;
    CHECK(!kl_logcap_overdue(4 * S, 0, 5 * S, 0), "4 s old: not yet");
    CHECK(kl_logcap_overdue(5 * S, 0, 5 * S, 0), "exactly 5 s: due");
    CHECK(kl_logcap_overdue(60 * S, 10 * S, 5 * S, 0), "long overdue: due");
    CHECK(!kl_logcap_overdue(60 * S, 10 * S, 5 * S, 1), "already reported: never again");
    CHECK(!kl_logcap_overdue(3 * S, 10 * S, 5 * S, 0), "now < t0 (clock went back): not due, no underflow");

    if (fails) { printf("t_logcap: %d FAILED\n", fails); return 1; }
    printf("t_logcap: all passed\n");
    return 0;
}
