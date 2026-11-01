// t_ovrp_misc.c - host test of kl_ovrp_misc.c: an ovrpResult success answer must not leave the
// caller's out-parameter as it found it (stub for kl_ovrp.c).
// clang -I.. -I../.. -o /tmp/t_ovrp_misc tests/t_ovrp_misc.c && /tmp/t_ovrp_misc
#include "../kl_ovrp_misc.c"
#include <assert.h>
#include <string.h>

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
    puts("t_ovrp_misc: ok");
    return 0;
}
