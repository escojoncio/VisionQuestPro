// t_ovrp_render.c - host test of kl_ovrp_render.c's eye-layer queries (stubs for kl_ovrp.c).
// clang -I.. -I../.. -o /tmp/t_ovrp_render tests/t_ovrp_render.c -lm && /tmp/t_ovrp_render
#include "../kl_ovrp_render.c"
#include <assert.h>

static int g_eye_id, g_eye_w, g_eye_h;   // the live eye layer the stubs report (0 = none)

void kl_ovrp_hit(const char *n) { (void)n; }
static const kl_ovrp_entry *g_tab; static size_t g_ntab;
void kl_ovrp_register(const kl_ovrp_entry *t, size_t n, const char *const *r) { (void)r; g_tab = t; g_ntab = n; }
static void *find(const char *name) {         // what kl_ovrp_sym hands the guest for a registered name
    for (size_t i = 0; i < g_ntab; i++) if (!strcmp(g_tab[i].name, name)) return g_tab[i].fn;
    return NULL;
}
void *kl_ovrp_sym(const char *n) { (void)n; return 0; }
void *kl_openxr_sym(const char *n) { (void)n; return 0; }
int kl_ovrp_is_initialized(void) { return 1; }
int kl_ovrp_multiview(void) { return 0; }
int kl_ovrp_stage_count(void) { return 3; }
void kl_ovrp_eye_texture_size(int *w, int *h) { *w = 2000; *h = 2200; }
int kl_ovrp_active_eye_layer_size(int *w, int *h) {
    if (!g_eye_id) return 0;
    if (w) *w = g_eye_w;
    if (h) *h = g_eye_h;
    return 1;
}
int kl_ovrp_active_eye_layer_id(void) { return g_eye_id; }
// Not reached by this test; the linker still wants them.
int kl_ovrp_battery_level(void) { return 100; }
int kl_ovrp_battery_charging(void) { return 0; }
void kl_ovrp_eye_local(int e, float o[3], float r[4]) { (void)e; (void)o; (void)r; }
void kl_ovrp_eye_view(int e, float *px, float *py, float *pz, float *qx, float *qy, float *qz, float *qw, float t[4]) {
    (void)e; (void)px; (void)py; (void)pz; (void)qx; (void)qy; (void)qz; (void)qw; (void)t;
}
float kl_ovrp_frustum_near(void) { return 0.1f; }
void kl_ovrp_guest_head(int s, kl_ovrp_pose7 *o) { (void)s; memset(o, 0, sizeof *o); o->qw = 1; }
int kl_ovrp_head_motion(float *v, float *a) { (void)v; (void)a; return 0; }
float klovrp_measured_eye_height(void) { return 1.6f; }
void klovrp_GetEyePose_entry(void) {} void klovrp_GetEyeVelocity_entry(void) {}
void klovrp_GetEyeAcceleration_entry(void) {} void klovrp_GetTrackingCalibratedOrigin_entry(void) {}

int main(void) {
    int32_t id = -1;
    assert(r_GetEyeFovLayerId(NULL) == KLOVRP_INVALID_PARAM);
    // No eye layer yet: success, id 0 ("no id").
    assert(r_GetEyeFovLayerId(&id) == KLOVRP_SUCCESS && id == 0);
    // The guest set up its eye layer (id 7): that is the id the plugin reports.
    g_eye_id = 7; g_eye_w = 1800; g_eye_h = 1900;
    id = -1;
    assert(r_GetEyeFovLayerId(&id) == KLOVRP_SUCCESS && id == 7);
    // The recommended-resolution decision is untouched: fixed size once an eye layer is live...
    ovrpSizei sz = { 0, 0 };
    assert(r_GetEyeLayerRecommendedResolution(&sz) == KLOVRP_SUCCESS && sz.w == 2000 && sz.h == 2200);
    // ...and -1004 with nothing written while none is.
    g_eye_id = 0; sz.w = sz.h = -5;
    assert(r_GetEyeLayerRecommendedResolution(&sz) == KLOVRP_UNSUPPORTED && sz.w == -5 && sz.h == -5);
    // Legacy float getters, reached by name as the guest reaches them: the value is in s0 (a C float
    // return), set values are echoed back, a non-positive or non-finite set is ignored.
    kl_ovrp_render_register();
    float (*eh)(void) = (float (*)(void))find("ovrp_GetUserEyeHeight");
    int32_t (*seh)(float) = (int32_t (*)(float))find("ovrp_SetUserEyeHeight");
    float (*ed)(void) = (float (*)(void))find("ovrp_GetUserEyeDepth");
    int32_t (*sed)(float) = (int32_t (*)(float))find("ovrp_SetUserEyeDepth");
    assert(eh && seh && ed && sed);
    assert(eh() == 1.6f);                                   // never set: the measured eye height
    assert(seh(1.675f) == 1 && eh() == 1.675f);
    assert(seh(-1.0f) == 1 && seh(NAN) == 1 && eh() == 1.675f);
    assert(fabsf(ed() - 0.0805f) < 1e-6f);                  // Quest default neck-eye depth
    assert(sed(0.09f) == 1 && fabsf(ed() - 0.09f) < 1e-6f);
    // ovrpBool ovrp_SetEyeOcclusionMeshEnabled(ovrpBool): was the shared "success" 0 = FALSE, which a
    // caller reads as "refused". Accepted (no mesh exists, ovrp_GetEyeOcclusionMeshEnabled stays 0).
    int32_t (*sem)(int) = (int32_t (*)(int))find("ovrp_SetEyeOcclusionMeshEnabled");
    int32_t (*gem)(void) = (int32_t (*)(void))find("ovrp_GetEyeOcclusionMeshEnabled");
    assert(sem && gem && sem(1) == KLOVRP_TRUE && gem() == 0);
    // ovrpBool ovrp_SetEyeTextureArrayEnabled(ovrpBool): BOOL in OVRPlugin 1.89, 1.94 and 1.110 alike;
    // the shared "success" 0 read as FALSE (refused).
    int32_t (*seta)(int) = (int32_t (*)(int))find("ovrp_SetEyeTextureArrayEnabled");
    assert(seta && seta(1) == KLOVRP_TRUE && seta(0) == KLOVRP_TRUE);
    // ovrp_PreInitialize2 is a tail call into PreInitialize5 in all three builds: an ovrpResult, 0 = success.
    ovrpResult (*pre2)(void) = (ovrpResult (*)(void))find("ovrp_PreInitialize2");
    assert(pre2 && pre2() == KLOVRP_SUCCESS);
    // float ovrp_GetFloat(int key): the value in s0 (a C float return), echoing SetFloat; unknown key 0.0.
    float (*gf)(int) = (float (*)(int))find("ovrp_GetFloat");
    int32_t (*sf)(int, float) = (int32_t (*)(int, float))find("ovrp_SetFloat");
    assert(gf && sf && gf(7) == 0.0f && sf(7, 2.5f) == KLOVRP_TRUE && gf(7) == 2.5f);
    // ovrpResult ovrp_GetNativeOpenXRHandles(uint64_t *xrInstance, uint64_t *xrSession): TWO pointers in
    // every build. A third register is the caller's stale x2 and must never be written through.
    ovrpResult (*gh)(void *, void *, void *) = (ovrpResult (*)(void *, void *, void *))find("ovrp_GetNativeOpenXRHandles");
    assert(gh);
    uint64_t hi = 0x1111, hs = 0x2222, stale = 0x3333;
    assert(gh(&hi, &hs, &stale) == KLOVRP_UNSUPPORTED);
    assert(hi == 0 && hs == 0 && stale == 0x3333);
    puts("t_ovrp_render: ok");
    return 0;
}
