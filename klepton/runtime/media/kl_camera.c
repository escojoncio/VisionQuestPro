// libcamera2ndk.so as a device without cameras. See kl_camera.h.
#include "kl_camera.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// camera_status_t, values from the NDK's NdkCameraError.h.
enum {
    KC_OK                          = 0,
    KC_ERROR_BASE                  = -10000,
    KC_ERROR_INVALID_PARAMETER     = -10002,
    KC_ERROR_CAMERA_DISCONNECTED   = -10003,
    KC_ERROR_NOT_ENOUGH_MEMORY     = -10004,
    KC_ERROR_METADATA_NOT_FOUND    = -10005,
    KC_ERROR_CAMERA_DEVICE         = -10006,
    KC_ERROR_CAMERA_SERVICE        = -10007,
    KC_ERROR_SESSION_CLOSED        = -10008,
    KC_ERROR_INVALID_OPERATION     = -10009,
};

// ACameraIdList as the NDK declares it.
typedef struct { int numCameras; const char **cameraIds; } kc_idlist;

static kc_idlist g_empty_list = { 0, NULL };
static char      g_manager_dummy;

// Once per function, as the other media doors do: the first call is the news.
#define ONCE(name) do { static int s_; if (!s_) { s_ = 1; \
    fprintf(stderr, "  [camera] %s (no cameras on this device)\n", name); } } while (0)

static void *kc_ACameraManager_create(void) { ONCE("ACameraManager_create"); return &g_manager_dummy; }
static void kc_ACameraManager_delete(void *m) { ONCE("ACameraManager_delete"); (void)m; }

static int kc_ACameraManager_getCameraIdList(void *m, kc_idlist **out) {
    ONCE("ACameraManager_getCameraIdList"); (void)m;
    if (!out) return KC_ERROR_INVALID_PARAMETER;
    *out = &g_empty_list;
    return KC_OK;
}
static void kc_ACameraManager_deleteCameraIdList(kc_idlist *l) { ONCE("ACameraManager_deleteCameraIdList"); (void)l; }

static int kc_ACameraManager_getCameraCharacteristics(void *m, const char *id, void **out) {
    ONCE("ACameraManager_getCameraCharacteristics"); (void)m; (void)id;
    if (out) *out = NULL;
    return KC_ERROR_INVALID_PARAMETER;
}
static int kc_ACameraManager_openCamera(void *m, const char *id, void *cb, void **dev) {
    ONCE("ACameraManager_openCamera"); (void)m; (void)id; (void)cb;
    if (dev) *dev = NULL;
    return KC_ERROR_CAMERA_DISCONNECTED;
}
static int kc_ACameraManager_registerAvailabilityCallback(void *m, const void *cb) {
    ONCE("ACameraManager_registerAvailabilityCallback"); (void)m; (void)cb; return KC_OK;
}
static int kc_ACameraManager_unregisterAvailabilityCallback(void *m, const void *cb) {
    ONCE("ACameraManager_unregisterAvailabilityCallback"); (void)m; (void)cb; return KC_OK;
}

static void kc_ACameraMetadata_free(void *md) { ONCE("ACameraMetadata_free"); (void)md; }
static int kc_ACameraMetadata_getConstEntry(const void *md, uint32_t tag, void *entry) {
    ONCE("ACameraMetadata_getConstEntry"); (void)md; (void)tag; (void)entry;
    return KC_ERROR_METADATA_NOT_FOUND;
}

static int kc_ACameraDevice_close(void *d) { ONCE("ACameraDevice_close"); (void)d; return KC_OK; }
static const char *kc_ACameraDevice_getId(const void *d) { ONCE("ACameraDevice_getId"); (void)d; return NULL; }
static int kc_ACameraDevice_createCaptureRequest(const void *d, int tmpl, void **req) {
    ONCE("ACameraDevice_createCaptureRequest"); (void)d; (void)tmpl;
    if (req) *req = NULL;
    return KC_ERROR_CAMERA_DISCONNECTED;
}
static int kc_ACameraDevice_createCaptureSession(void *d, const void *outs, const void *cb, void **sess) {
    ONCE("ACameraDevice_createCaptureSession"); (void)d; (void)outs; (void)cb;
    if (sess) *sess = NULL;
    return KC_ERROR_CAMERA_DISCONNECTED;
}

static int kc_ACameraCaptureSession_close(void *s) { ONCE("ACameraCaptureSession_close"); (void)s; return KC_OK; }
static int kc_ACameraCaptureSession_setRepeatingRequest(void *s, void *cb, int n, void **reqs, int *seq) {
    ONCE("ACameraCaptureSession_setRepeatingRequest"); (void)s; (void)cb; (void)n; (void)reqs;
    if (seq) *seq = -1;
    return KC_ERROR_SESSION_CLOSED;
}

static int kc_ACameraOutputTarget_create(void *win, void **out) {
    ONCE("ACameraOutputTarget_create"); (void)win;
    if (out) *out = NULL;
    return KC_ERROR_INVALID_PARAMETER;
}
static int kc_ACaptureRequest_addTarget(void *req, const void *tgt) {
    ONCE("ACaptureRequest_addTarget"); (void)req; (void)tgt; return KC_ERROR_INVALID_PARAMETER;
}

static int kc_ACaptureSessionOutputContainer_create(void **out) {
    ONCE("ACaptureSessionOutputContainer_create");
    if (out) *out = NULL;
    return KC_ERROR_NOT_ENOUGH_MEMORY;
}
static int kc_ACaptureSessionOutputContainer_add(void *c, const void *o) {
    ONCE("ACaptureSessionOutputContainer_add"); (void)c; (void)o; return KC_ERROR_INVALID_PARAMETER;
}
static void kc_ACaptureSessionOutputContainer_free(void *c) { ONCE("ACaptureSessionOutputContainer_free"); (void)c; }
static int kc_ACaptureSessionOutput_create(void *win, void **out) {
    ONCE("ACaptureSessionOutput_create"); (void)win;
    if (out) *out = NULL;
    return KC_ERROR_INVALID_PARAMETER;
}
static void kc_ACaptureSessionOutput_free(void *o) { ONCE("ACaptureSessionOutput_free"); (void)o; }

// There is no Java Surface behind a camera window.
static void *kc_ANativeWindow_toSurface(void *env, void *win) {
    ONCE("ANativeWindow_toSurface"); (void)env; (void)win; return NULL;
}

#define C(sym) {#sym, (void *)kc_##sym}
static const struct { const char *name; void *fn; } g_camera[] = {
    C(ACameraManager_create), C(ACameraManager_delete),
    C(ACameraManager_getCameraIdList), C(ACameraManager_deleteCameraIdList),
    C(ACameraManager_getCameraCharacteristics), C(ACameraManager_openCamera),
    C(ACameraManager_registerAvailabilityCallback), C(ACameraManager_unregisterAvailabilityCallback),
    C(ACameraMetadata_free), C(ACameraMetadata_getConstEntry),
    C(ACameraDevice_close), C(ACameraDevice_getId),
    C(ACameraDevice_createCaptureRequest), C(ACameraDevice_createCaptureSession),
    C(ACameraCaptureSession_close), C(ACameraCaptureSession_setRepeatingRequest),
    C(ACameraOutputTarget_create), C(ACaptureRequest_addTarget),
    C(ACaptureSessionOutputContainer_create), C(ACaptureSessionOutputContainer_add),
    C(ACaptureSessionOutputContainer_free),
    C(ACaptureSessionOutput_create), C(ACaptureSessionOutput_free),
    C(ANativeWindow_toSurface),
};

void *kl_camera_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_camera / sizeof g_camera[0]; i++)
        if (!strcmp(g_camera[i].name, name)) return g_camera[i].fn;
    return NULL;
}
