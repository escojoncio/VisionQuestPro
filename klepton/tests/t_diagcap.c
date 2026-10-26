// In-game capture trigger (runtime/diag/kl_diagcap.c): the hold detector, and the dlsym call into VPKEngine.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kl_diagcap.h"
#include "kl_ovrp.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

// Stand-ins: VPKEngine's capture API (found with dlsym(RTLD_DEFAULT), exported by the executable) and the haptics seam.
static int req_calls, req_rc = 1, req_frames = -1, req_pathnull;
int vpvk_capture_request(uint32_t frames, const char *path) { req_calls++; req_frames = (int)frames; req_pathnull = path == NULL; return req_rc; }
static int hap_calls, hap_hands;
static float hap_amp[2], hap_secs[2];
void kl_ovrp_haptics_apply(int hand, float amplitude, float seconds) {
    hap_calls++; hap_hands |= 1 << hand; hap_amp[hand] = amplitude; hap_secs[hand] = seconds;
}

#define LS KL_OVRP_RAW_LTHUMBSTICK
#define RS KL_OVRP_RAW_RTHUMBSTICK

int main(void) {
    kl_diagcap c;

    // Left stick, 3 s: nothing before, one fire at 3 s, none while it stays down, again after a release.
    kl_diagcap_init(&c, KL_DIAGCAP_STICK_L, 3000);
    CHECK(c.need[0] == LS && c.need[1] == 0, "left stick mask");
    CHECK(!kl_diagcap_step(&c, 0, 0, 0), "idle");
    CHECK(!kl_diagcap_step(&c, 0, LS, 1000), "press starts the timer");
    CHECK(!kl_diagcap_step(&c, 0, LS, 3999), "just short of 3 s");
    CHECK(kl_diagcap_step(&c, 0, LS, 4000), "fires at 3 s");
    CHECK(!kl_diagcap_step(&c, 0, LS, 4010), "once per hold");
    CHECK(!kl_diagcap_step(&c, 0, LS, 60000), "still once much later");
    CHECK(!kl_diagcap_step(&c, 0, 0, 61000), "release");
    CHECK(!kl_diagcap_step(&c, 0, LS, 62000), "second hold starts");
    CHECK(kl_diagcap_step(&c, 0, LS, 65000), "second hold fires");

    // A release in the middle restarts the count; other buttons held with it do not matter.
    kl_diagcap_init(&c, KL_DIAGCAP_STICK_L, 3000);
    kl_diagcap_step(&c, 0, LS, 0);
    kl_diagcap_step(&c, 0, 0, 2000);
    CHECK(!kl_diagcap_step(&c, 0, LS, 2500), "restart after release");
    CHECK(!kl_diagcap_step(&c, 0, LS, 5000), "2.5 s since the restart");
    CHECK(kl_diagcap_step(&c, 0, LS | KL_OVRP_RAW_X | KL_OVRP_RAW_LINDEX_TRIGGER, 5500), "extra buttons are ignored");

    // The click is hand specific: the right stick's bit on the left hand's word, or the left stick on the right
    // hand's, is not it; the other hand's traffic does not disturb the hold.
    kl_diagcap_init(&c, KL_DIAGCAP_STICK_L, 3000);
    kl_diagcap_step(&c, 0, RS, 0);
    CHECK(!kl_diagcap_step(&c, 0, RS, 5000), "right stick bit on the left hand");
    kl_diagcap_step(&c, 0, LS, 6000);
    kl_diagcap_step(&c, 1, RS, 7000);
    kl_diagcap_step(&c, 1, 0, 8000);
    CHECK(kl_diagcap_step(&c, 0, LS, 9000), "right hand words do not disturb");

    kl_diagcap_init(&c, KL_DIAGCAP_STICK_R, 3000);
    CHECK(c.need[0] == 0 && c.need[1] == RS, "right stick mask");
    kl_diagcap_step(&c, 1, RS, 0);
    CHECK(!kl_diagcap_step(&c, 0, LS, 2000), "left word ignored in right mode");
    CHECK(kl_diagcap_step(&c, 1, RS, 3000), "right stick fires");

    // Both sticks: the count starts when the second one goes down; letting go of either one stops it.
    kl_diagcap_init(&c, KL_DIAGCAP_STICKS, 3000);
    kl_diagcap_step(&c, 0, LS, 0);
    CHECK(!kl_diagcap_step(&c, 0, LS, 5000), "one stick is not enough");
    kl_diagcap_step(&c, 1, RS, 6000);
    CHECK(!kl_diagcap_step(&c, 1, RS, 8999), "3 s from the second press");
    CHECK(kl_diagcap_step(&c, 1, RS, 9000), "both sticks fire");
    kl_diagcap_step(&c, 0, 0, 9500);
    kl_diagcap_step(&c, 0, LS, 10000);
    CHECK(!kl_diagcap_step(&c, 1, RS, 12000), "re-armed only after a release, 2 s in");
    CHECK(kl_diagcap_step(&c, 1, RS, 13000), "fires again");

    // Both index triggers (the hand-tracking pinches).
    kl_diagcap_init(&c, KL_DIAGCAP_TRIGGERS, 3000);
    kl_diagcap_step(&c, 0, KL_OVRP_RAW_LINDEX_TRIGGER, 0);
    kl_diagcap_step(&c, 1, KL_OVRP_RAW_RINDEX_TRIGGER, 0);
    CHECK(kl_diagcap_step(&c, 1, KL_OVRP_RAW_RINDEX_TRIGGER, 3000), "both triggers fire");

    // Hold time limits; unknown mode falls back to the left stick; a bad hand is ignored.
    kl_diagcap_init(&c, 99, 1);
    CHECK(c.hold_ms == KL_DIAGCAP_HOLD_MIN_MS && c.need[0] == LS && c.need[1] == 0, "min hold, default mode");
    kl_diagcap_init(&c, 0, 99999999);
    CHECK(c.hold_ms == KL_DIAGCAP_HOLD_MAX_MS, "max hold");
    CHECK(!kl_diagcap_step(&c, 2, LS, 0) && !kl_diagcap_step(&c, -1, LS, 0), "hand out of range");

    // kl_diagcap_input decides once, on its first call, from the environment: each case runs in its own process.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {            // on: both sticks, 500 ms, the real clock and the dlsym call
        setenv("KL_DIAGCAP", "1", 1); setenv("KL_DIAGCAP_BTN", "2", 1); setenv("KL_DIAGCAP_HOLD_MS", "500", 1);
        kl_diagcap_input(0, LS);
        kl_diagcap_input(1, RS);
        CHECK(req_calls == 0, "child: not yet");
        usleep(650000);
        kl_diagcap_input(1, RS);
        CHECK(req_calls == 1 && req_frames == 1 && req_pathnull, "child: vpvk_capture_request(1, NULL)");
        CHECK(hap_calls == 2 && hap_hands == 3 && hap_amp[0] == 0.8f && hap_secs[1] == 0.3f, "child: strong short pulse on both hands");
        usleep(100000);
        kl_diagcap_input(1, RS);
        CHECK(req_calls == 1, "child: once per hold");
        kl_diagcap_input(1, 0);
        req_rc = 0; hap_calls = 0;
        kl_diagcap_input(1, RS);
        usleep(650000);
        kl_diagcap_input(1, RS);
        CHECK(req_calls == 2 && hap_calls == 2 && hap_amp[0] == 0.3f && hap_secs[0] == 0.8f, "child: rc 0 gives the weak long pulse");
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "on case (child)");

    // Off unless KL_DIAGCAP=1.
    unsetenv("KL_DIAGCAP");
    for (int i = 0; i < 3; i++) kl_diagcap_input(0, LS);
    usleep(700000);
    kl_diagcap_input(0, LS);
    CHECK(req_calls == 0 && hap_calls == 0, "off by default");
    return fails ? 1 : (printf("t_diagcap ok\n"), 0);
}
