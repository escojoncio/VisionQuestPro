// Frame boundary of gfx/kl_bg.c: kl_frame_boundary() forwards to VPKEngine's vpvk_frame_boundary (dlsym, once) one call per
// guest frame; a guest using both routes (ovrp + OpenXR) gets one call per frame; a driver without the symbol costs nothing.
// Built twice: with the stand-in (default) and with -DNO_SYM (an older engine).
#include <stdio.h>
#include <unistd.h>

#include "kl_bg.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

#ifndef NO_SYM
// Stand-in for VPKEngine's C API, which kl_bg.c finds with dlsym(RTLD_DEFAULT) (exported by the executable).
static int calls;
void vpvk_frame_boundary(void) { calls++; }
#endif

int main(void) {
#ifdef NO_SYM
    for (int i = 0; i < 100; i++) { kl_frame_boundary(KL_FRAME_OVRP); kl_frame_boundary(KL_FRAME_XR); }
    puts("ok: no vpvk_frame_boundary in the engine, nothing called");
#else
    // One route: one call per frame.
    for (int i = 0; i < 10; i++) kl_frame_boundary(KL_FRAME_OVRP);
    CHECK(calls == 10, "ovrp alone: one call per frame");

    // Both routes in the same frame (either order): the owner (ovrp) keeps the boundary, xr is ignored.
    for (int i = 0; i < 5; i++) { kl_frame_boundary(KL_FRAME_OVRP); kl_frame_boundary(KL_FRAME_XR); }
    CHECK(calls == 15, "ovrp then xr in each frame: one call per frame");
    for (int i = 0; i < 5; i++) { kl_frame_boundary(KL_FRAME_XR); kl_frame_boundary(KL_FRAME_OVRP); }
    CHECK(calls == 20, "xr then ovrp in each frame: one call per frame");

    // The owner went quiet (the guest switched route): the other takes over after the stale window.
    usleep(600000);
    for (int i = 0; i < 4; i++) kl_frame_boundary(KL_FRAME_XR);
    CHECK(calls == 24, "xr takes over once ovrp stopped calling");
    kl_frame_boundary(KL_FRAME_OVRP);
    CHECK(calls == 24, "ovrp ignored while xr owns the boundary");
#endif
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    puts("ok");
    return 0;
}
