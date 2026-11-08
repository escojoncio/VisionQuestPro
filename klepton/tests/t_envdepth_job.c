// Host test of the envdepth job timeout (kl_envdepth.c, KL-TEST envdepthjob block): a depth job whose command buffer
// never completes is given up after KLED_JOB_TIMEOUT_S, and its late job_done must not clear the flag of the job
// that replaced it (job sequence number).
// Build: tools/ci/host-tests.sh (ASan + UBSan); the block is extracted into envdepthjob.inc.
#include <stdint.h>
#include <stdio.h>

#include "envdepthjob.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    const double T = KLED_JOB_TIMEOUT_S;
    // Expiry: only a job in flight, only once the timeout has fully passed.
    CHECK(!kled_job_expired(0, 10.0, 99.0, T), "nothing in flight never expires");
    CHECK(!kled_job_expired(1, 10.0, 10.0, T), "just begun: not expired");
    CHECK(!kled_job_expired(1, 10.0, 10.0 + T - 0.001, T), "just short of the timeout: not expired");
    CHECK(kled_job_expired(1, 10.0, 10.0 + T, T), "at the timeout: expired");
    CHECK(kled_job_expired(1, 10.0, 600.0, T), "long past: expired");
    CHECK(T > 0.1 && T <= 2.0, "timeout is long enough for a real render and short enough to recover");

    // Current job: a finish of the job in flight clears it, nothing else does.
    CHECK(kled_job_is_current(1, 7, 7), "the job in flight is current");
    CHECK(!kled_job_is_current(0, 7, 7), "same seq but nothing in flight (already timed out): not current");
    CHECK(!kled_job_is_current(1, 7, 8), "an older job while a newer one is in flight: not current");
    CHECK(!kled_job_is_current(0, 7, 8), "an older job, nothing in flight: not current");

    // Sequence: job 1 hangs, times out, job 2 starts; job 1 finishing late must leave job 2's flag alone.
    uint32_t seq = 0; int rendering = 0; double began = 0;
    seq = 1; rendering = 1; began = 0.0;                                 // job 1 begins
    CHECK(!kled_job_expired(rendering, began, 0.5 * T, T), "job 1 still waited for at half the timeout");
    CHECK(kled_job_expired(rendering, began, 1.2 * T, T), "job 1 given up past the timeout");
    rendering = 0;                                                       // job_begin releases the flag
    seq = 2; rendering = 1; began = 1.2 * T;                               // job 2 begins
    CHECK(!kled_job_is_current(rendering, 1, seq), "job 1 finishing late is not current");
    CHECK(rendering == 1, "job 1's late finish left job 2 in flight");
    CHECK(kled_job_is_current(rendering, 2, seq), "job 2 finishing is current");

    // job_done decision: current job; late job whose slot is still S_LOST; late job whose slot is not S_LOST.
    CHECK(kled_done_action(1, 7, 7, 0) == KLED_DONE_CURRENT, "the job in flight finishes as current");
    CHECK(kled_done_action(1, 7, 7, 1) == KLED_DONE_CURRENT, "the current job is current whatever its slot says");
    CHECK(kled_done_action(1, 1, 2, 1) == KLED_DONE_FREE_LOST, "old job, slot S_LOST, newer job in flight: slot freed");
    CHECK(kled_done_action(0, 1, 1, 1) == KLED_DONE_FREE_LOST, "timed-out job, nothing in flight, slot S_LOST: slot freed");
    CHECK(kled_done_action(1, 1, 2, 0) == KLED_DONE_IGNORE, "old job, slot reused (not S_LOST): left alone");
    CHECK(kled_done_action(0, 1, 2, 0) == KLED_DONE_IGNORE, "old job, nothing in flight, slot not S_LOST: left alone");
    return fails ? 1 : (printf("t_envdepth_job ok\n"), 0);
}
