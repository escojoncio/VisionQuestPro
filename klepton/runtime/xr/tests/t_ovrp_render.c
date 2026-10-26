// t_ovrp_render.c - host test of kl_ovrp_render.c's eye-layer queries (stubs for kl_ovrp.c).
// clang -I.. -I../.. -o /tmp/t_ovrp_render tests/t_ovrp_render.c -lm && /tmp/t_ovrp_render
#include "../kl_ovrp_render.c"
#include <assert.h>

static int g_eye_id, g_eye_w, g_eye_h;   // the live eye layer the stubs report (0 = none)

void kl_ovrp_hit(const char *n) { (void)n; }
void kl_ovrp_register(const kl_ovrp_entry *t, size_t n, const char *const *r) { (void)t; (void)n; (void)r; }
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
    puts("t_ovrp_render: ok");
    return 0;
}
