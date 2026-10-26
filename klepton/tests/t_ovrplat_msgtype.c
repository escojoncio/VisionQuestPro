// ovrMessageType_ToString and the family test of kl_ovrplat.c (KL-TEST msgtype / owns blocks, cut out at build time).
// Maestro's libUE4 imports ovrMessageType_ToString; kl_ovrplat_owns did not claim the prefix, so it stayed unresolved.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned g_hits;
static void plat_hit(const char *name) { (void)name; g_hits++; }

#include "msgtype.inc"
#include "owns.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    CHECK(kl_ovrplat_owns("ovrMessageType_ToString"), "ovrMessageType_ToString is claimed");
    CHECK(kl_ovrplat_owns("ovr_PopMessage") && kl_ovrplat_owns("ovrID_FromString") &&
          kl_ovrplat_owns("ovrKeyValuePair_makeString"), "old families still claimed");
    CHECK(!kl_ovrplat_owns("ovrp_GetInitialized") && !kl_ovrplat_owns("ovrMessage") && !kl_ovrplat_owns(NULL),
          "ovrp_* and others are not claimed");

    const char *s = klplat_MessageType_ToString(0x436F345D);          // User_GetLoggedInUser
    CHECK(s && !strcmp(s, "User_GetLoggedInUser"), "known type -> its name");
    s = klplat_MessageType_ToString(0xDEADBEEFu);
    CHECK(s && !strcmp(s, "Unknown"), "unknown type -> \"Unknown\"");
    s = klplat_MessageType_ToString(0);
    CHECK(s && !strcmp(s, "Unknown"), "0 -> \"Unknown\"");
    CHECK(g_hits == 3, "each call is counted");
    if (fails) return 1;
    printf("t_ovrplat_msgtype ok\n");
    return 0;
}
