// Host test of kl_fault.c's KL-TEST fqonce block (cut out at build time): the ForceQuit dump the guest triggers
// (AndroidThunkJava_ForceQuit, ignored) goes to the normal log only (fd 2), marked [forcequit], once per session,
// and never opens the crash file (klepton-crash.log is for a real crash). The block references no crash-file code:
// klf_open_crash is deliberately NOT defined here, so a version of it that opened the file would not link.
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static char out[4096];
static size_t outn;
static void klf_emit_fd2(const char *b, size_t n) { memcpy(out + outn, b, n); outn += n; out[outn] = 0; }
static int ring_calls;
static void kl_log_ring_dump(void (*emit)(const char *, size_t)) {
    ring_calls++;
    emit("[fatal] last guest errors:\n", 27);
    emit("  E/UE4: Fatal error\n", 21);
}
#include "fqonce.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    kl_fault_guest_errors_once();
    CHECK(!strncmp(out, "[forcequit] ", 12), "dump starts with the [forcequit] mark");
    CHECK(strstr(out, "[fatal] last guest errors:\n  E/UE4: Fatal error\n") != NULL, "the ring dump follows");
    size_t n1 = outn;
    kl_fault_guest_errors_once();
    CHECK(outn == n1 && ring_calls == 1, "once per session");
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
