// t_ovrp_misc.c - host test of kl_ovrp_misc.c: an ovrpResult success answer must not leave the
// caller's out-parameter as it found it (stub for kl_ovrp.c). Also kl_ovrp.c's KL-TEST ovrpinit
// blocks with the g_ovrp_impl rows that name them (both cut out at build time): ovrp_GetInitialized,
// and its KL-TEST ovrpbridge blocks: what the OVRPlugin path leaves alone under Unity's OpenXR bridge.
// awk '/KL-TEST-BEGIN ovrpinit/{p=1;next} /KL-TEST-END ovrpinit/{p=0} p' kl_ovrp.c > /tmp/ovrpinit.inc
// awk '/KL-TEST-BEGIN ovrpbridge/{p=1;next} /KL-TEST-END ovrpbridge/{p=0} p' kl_ovrp.c > /tmp/ovrpbridge.inc
// awk '/KL-TEST-BEGIN ptscan/{p=1;next} /KL-TEST-END ptscan/{p=0} p' kl_ovrp.c > /tmp/ptscan.inc
// awk '/^    [{]"ovrp_(GetInitialized|Initialize7|Shutdown2?|GetNativeXrApiType|UnityOpenXR_OnInstance(Create|Destroy))", /' kl_ovrp.c > /tmp/ovrpinit_tab.inc
// clang -I.. -I../.. -I/tmp -o /tmp/t_ovrp_misc tests/t_ovrp_misc.c && /tmp/t_ovrp_misc
#include "../kl_ovrp_misc.c"
#include <assert.h>
#include <string.h>

// kl_ovrp.c's init state, resolved as kl_ovrp_sym does for a name kl_ovrp.c implements itself:
// g_ovrp_impl is searched before the registered tables and the shared answers.
#define OVRP_SUCCESS 0
#define OVRP_TRUE    1
#define OVRP_FAIL_INVALID_PARAM (-1001)
static void ovrp_hit(const char *n) { (void)n; }
#include "ovrpinit.inc"
// What the ovrpbridge blocks read from the rest of kl_ovrp.c: the OVRPlugin guest origin (EyeLevel:
// the eye height) and the measured eye height.
float klovrp_guest_origin_y(void) { return 1.25f; }
float klovrp_measured_eye_height(void) { return 1.5f; }
// ...the layer desc (only its shape is read) and the allocator ovrp_EnqueueSetupLayer2 delegates to...
#define OVRP_FAIL_UNSUPPORTED (-1004)
typedef struct { int shape; } ovrp_layer_desc_eyefov;
static int n_setup;
static uint64_t klovrp_SetupLayer(void *device, const ovrp_layer_desc_eyefov *desc, int *layer_id) {
    (void)device; (void)desc;
    *layer_id = 40 + ++n_setup;
    return OVRP_SUCCESS;
}
#include "ptscan.inc"
#include "ovrpbridge.inc"
// ...and the eye-height re-latch behind ovrp_RecenterTrackingOrigin*.
static int n_recenter;
static void klovrp_recenter_now(const char *why) { (void)why; n_recenter++; }
static const struct { const char *name; void *fn; } k_impl[] = {
#include "ovrpinit_tab.inc"
};
static void *find_impl(const char *name) {
    for (size_t i = 0; i < sizeof k_impl / sizeof k_impl[0]; i++) if (!strcmp(k_impl[i].name, name)) return k_impl[i].fn;
    return NULL;
}

void kl_ovrp_hit(const char *n) { (void)n; }
ovrpResult kl_ovrp_ok(const char *n) { (void)n; return 0; }
static const kl_ovrp_entry *g_tab; static size_t g_ntab;
void kl_ovrp_register(const kl_ovrp_entry *t, size_t n, const char *const *r) { (void)r; g_tab = t; g_ntab = n; }
static void *find(const char *name) {         // what kl_ovrp_sym hands the guest for a registered name
    for (size_t i = 0; i < g_ntab; i++) if (!strcmp(g_tab[i].name, name)) return g_tab[i].fn;
    return NULL;
}
static int refused(const char *name) {
    for (const char *const *r = k_refused; *r; r++) if (!strcmp(*r, name)) return 1;
    return 0;
}

int main(void) {
    kl_ovrp_misc_register();
    const int32_t SENT = 0x5A5A5A5A;
    int32_t o;
    // ovrpResult f(T *out): success AND the out written (garbage in a pre-set slot must not survive).
    static const char *const one_out[] = {
        "ovrp_GetFoveationEyeTracked", "ovrp_Media_GetMrcInputVideoBufferType",
        "ovrp_Media_IsCastingToRemoteClient",
    };
    for (size_t i = 0; i < sizeof one_out / sizeof one_out[0]; i++) {
        ovrpResult (*f)(int32_t *) = (ovrpResult (*)(int32_t *))find(one_out[i]);
        assert(f && "registered");
        o = SENT;
        assert(f(&o) == KLOVRP_SUCCESS && o == 0);
        assert(f(NULL) == KLOVRP_INVALID_PARAM);
    }
    // ovrpResult f(int cameraId, ovrpBool *out)
    static const char *const cam_out[] = {
        "ovrp_GetUseOverriddenExternalCameraFov", "ovrp_GetUseOverriddenExternalCameraStaticPose",
    };
    for (size_t i = 0; i < sizeof cam_out / sizeof cam_out[0]; i++) {
        ovrpResult (*f)(int, int32_t *) = (ovrpResult (*)(int, int32_t *))find(cam_out[i]);
        assert(f && "registered");
        o = SENT;
        assert(f(0, &o) == KLOVRP_SUCCESS && o == 0);
        assert(f(0, NULL) == KLOVRP_INVALID_PARAM);
    }
    // No-out entries keep the plain success answer.
    ovrpResult (*ok)(void) = (ovrpResult (*)(void))find("ovrp_AddCustomMetadata");
    assert(ok && ok() == KLOVRP_SUCCESS);
    // A refusal is by name and nothing is registered for it.
    assert(refused("ovrp_GetRenderModelPaths") && !find("ovrp_GetRenderModelPaths"));
    // The telemetry-consent text/notification getters take caller buffers (1.110 only: ovrpResult,
    // arguments forwarded to the engine): a success that leaves them untouched is a lie, so refused.
    static const char *const consent[] = {
        "ovrp_ShouldShowTelemetryNotification", "ovrp_GetConsentSettingsChangeText",
        "ovrp_GetConsentTitle", "ovrp_GetConsentMarkdownText",
        "ovrp_GetConsentNotificationMarkdownText",
    };
    for (size_t i = 0; i < sizeof consent / sizeof consent[0]; i++)
        assert(refused(consent[i]) && !find(consent[i]));
    // Unity's OpenXR bridge never calls ovrp_Initialize*: the instance notifications are the init
    // state ovrp_GetInitialized reports (OVRManager does not initialise while it reads 0).
    uint64_t (*inited)(void) = (uint64_t (*)(void))find_impl("ovrp_GetInitialized");
    uint64_t (*created)(uint64_t) = (uint64_t (*)(uint64_t))find_impl("ovrp_UnityOpenXR_OnInstanceCreate");
    uint64_t (*destroyed)(uint64_t) = (uint64_t (*)(uint64_t))find_impl("ovrp_UnityOpenXR_OnInstanceDestroy");
    assert(inited && created && destroyed && "own implementations in g_ovrp_impl");
    assert(inited() == 0);
    assert(created(1) == 0 && inited() == 1);
    assert(destroyed(1) == 0 && inited() == 0);
    // A null instance: failure, nothing initialised, no bridge.
    assert((int32_t)created(0) < 0 && inited() == 0 && !klovrp_bridge());
    // ovrp_Initialize* / ovrp_Shutdown* mixed with the notifications. The real plugin (1.103) has ONE
    // singleton behind ovrp_GetInitialized: ovrp_Shutdown2 clears it whoever created it, and its
    // OnInstanceDestroy calls ovrp_Shutdown2 itself. The bridge is the XR instance's and outlives a
    // Shutdown: only OnInstanceDestroy ends it.
    uint64_t (*init7)(void) = (uint64_t (*)(void))find_impl("ovrp_Initialize7");
    uint64_t (*shut)(void) = (uint64_t (*)(void))find_impl("ovrp_Shutdown");
    uint64_t (*shut2)(void) = (uint64_t (*)(void))find_impl("ovrp_Shutdown2");
    assert(init7 && shut && shut2 && "own implementations in g_ovrp_impl");
    assert(init7() == 0 && inited() == 1 && !klovrp_bridge());        // the legacy way in is never the bridge
    assert((int32_t)created(0) < 0 && inited() == 1 && !klovrp_bridge());   // a refused Create leaves a running plugin alone
    assert(shut() == OVRP_TRUE && inited() == 0);
    assert(created(1) == 0 && inited() == 1 && klovrp_bridge());
    assert(shut2() == 0 && inited() == 0 && klovrp_bridge());         // Shutdown2 after Create: down, still the bridge
    assert(init7() == 0 && inited() == 1 && klovrp_bridge());         // Initialize* under the bridge
    assert(shut() == OVRP_TRUE && inited() == 0 && klovrp_bridge());
    assert(destroyed(1) == 0 && inited() == 0 && !klovrp_bridge());
    assert(init7() == 0 && created(1) == 0 && inited() == 1 && klovrp_bridge());   // Create over Initialize...
    assert(destroyed(1) == 0 && inited() == 0 && !klovrp_bridge());   // ...and Destroy takes both down

    // Under the bridge the OVRPlugin path is inert for what it shares with kl_openxr.c.
    uint64_t (*apitype)(int *) = (uint64_t (*)(int *))find_impl("ovrp_GetNativeXrApiType");
    int api = -1;
    assert(apitype && apitype(&api) == 0 && api == 1);                // no bridge: as before
    assert(kl_ovrp_guest_origin_y() == 0.0f);                         // no OVRPlugin node read yet
    assert(created(1) == 0);
    assert(apitype(&api) == 0 && api == 3);                           // ovrpXrApi OpenXR, as the real plugin answers
    klovrp_space_mark();                                              // OVRManager reads a node pose
    assert(g_ovrp_space_used == 0 && kl_ovrp_guest_origin_y() == 0.0f);
    g_tracking_origin = 1;                                            // OVRManager: ovrp_SetTrackingOriginType(FloorLevel)
    assert(kl_ovrp_tracking_origin() == 0 && kl_ovrp_eye_height() == 0.0f);   // the two inputs of klxr_local_origin_y
    assert(destroyed(1) == 0);
    assert(g_ovrp_space_used == 0 && kl_ovrp_guest_origin_y() == 0.0f);       // nothing was latched meanwhile
    assert(kl_ovrp_tracking_origin() == 1 && kl_ovrp_eye_height() == 1.5f);
    // An OVRPlugin guest (no bridge) is unchanged...
    klovrp_space_mark();
    assert(g_ovrp_space_used == 1 && kl_ovrp_guest_origin_y() == 1.25f);
    // ...and a node read made outside the bridge does not leak into it.
    assert(created(1) == 0 && kl_ovrp_guest_origin_y() == 0.0f);
    assert(destroyed(1) == 0 && kl_ovrp_guest_origin_y() == 1.25f);

    // ovrp_RecenterTrackingOrigin*: the eye height it re-latches is OpenXR's LOCAL origin under the bridge.
    assert(klovrp_RecenterTrackingOrigin2(0) == 0 && n_recenter == 1);        // no bridge: as before
    assert(klovrp_RecenterTrackingOrigin(0) == 0 && n_recenter == 2);
    assert(created(1) == 0);
    assert(klovrp_RecenterTrackingOrigin2(0) == 0 && klovrp_RecenterTrackingOrigin(0) == 0);
    assert(n_recenter == 2);                                          // acknowledged, nothing re-latched
    assert(destroyed(1) == 0);
    assert(klovrp_RecenterTrackingOrigin2(0) == 0 && n_recenter == 3);

    // ovrp_EnqueueSetupLayer2: under the bridge an overlay gets no layer (nothing would composite it), so
    // OVROverlay keeps the object's own renderer; the OVRPassthroughLayer (state only) still gets its id.
    ovrp_layer_desc_eyefov quad = { 0 }, equirect = { 5 }, recon = { 7 }, surf = { 8 };
    int lid = -7;
    assert(klovrp_EnqueueSetupLayer2(&quad, 0, &lid) == 0 && lid == 41 && n_setup == 1);     // no bridge: as before
    assert(created(1) == 0);
    lid = -7;
    assert((int32_t)klovrp_EnqueueSetupLayer2(&quad, 0, &lid) < 0 && lid == -7 && n_setup == 1);       // refused: no slot, id untouched
    assert((int32_t)klovrp_EnqueueSetupLayer2(&equirect, 0, &lid) < 0 && lid == -7 && n_setup == 1);
    assert(klovrp_EnqueueSetupLayer2(&recon, 0, &lid) == 0 && lid == 42 && n_setup == 2);
    assert(klovrp_EnqueueSetupLayer2(&surf, 0, &lid) == 0 && lid == 43 && n_setup == 3);
    assert(destroyed(1) == 0);
    assert(klovrp_EnqueueSetupLayer2(&equirect, 0, &lid) == 0 && lid == 44 && n_setup == 4);
    puts("t_ovrp_misc: ok");
    return 0;
}
