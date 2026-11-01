// Host test of kl_ovrp.c's KL-TEST ovrpfallback block (cut out at build time): what the generic
// "unimplemented" stub answers for a name nobody classified. A float/struct/out-param entry point
// that lands there must read as "unsupported" (-1004), never as a success with garbage out-params;
// capability predicates and pointer getters answer 0 (no / NULL).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ovrpfallback.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define UNSUP ((uint64_t)(int64_t)-1004)

int main(void) {
    // Unknown names, any shape: -1004 (ovrpFailure_Unsupported).
    CHECK(klovrp_fallback_value("ovrp_SomethingNewFromAFutureSdk") == UNSUP, "unknown name -> -1004");
    CHECK(klovrp_fallback_value("ovrp_GetUserEyeHeight") == UNSUP, "float getter nobody implemented -> -1004");
    CHECK(klovrp_fallback_value("ovrp_GetDominantHand") == UNSUP, "Result+out getter nobody implemented -> -1004");
    CHECK(klovrp_fallback_value("ovrp_Media_GetMrcActivationMode") == UNSUP, "Media getter -> -1004");
    // Predicates (Get/Is/Has/Should + ...Supported/Enabled/...), with or without an ABI digit: 0.
    CHECK(klovrp_fallback_value("ovrp_GetFooSupported") == 0, "predicate -> 0");
    CHECK(klovrp_fallback_value("ovrp_IsFooEnabled2") == 0, "predicate + ABI digit -> 0");
    CHECK(klovrp_fallback_value("ovrp_HasBarAvailable") == 0, "Has...Available -> 0");
    CHECK(klovrp_fallback_value("ovrp_SetFooEnabled") == UNSUP, "a setter is not a predicate");
    // Pointer returns: NULL, never a negative number called as a function.
    CHECK(klovrp_fallback_value("ovrp_HookGetInstanceProcAddr") == 0, "Hook/ProcAddr -> NULL");
    CHECK(klovrp_fallback_value("ovrp_GetNativeSDKPointer") == 0, "Get...Pointer -> NULL");
    CHECK(klovrp_fallback_value("ovrp_GetFooPtr") == 0, "Get...Ptr -> NULL");
    CHECK(klovrp_fallback_value("ovrp_SetFooPtr") == UNSUP, "only a getter returns the pointer");
    if (!fails) puts("t_ovrp_fallback: ok");
    return fails ? 1 : 0;
}
