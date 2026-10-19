// kl_openxr_ext.c - Lote 5: OpenXR core + non-spatial extensions. Registered with
// kl_openxr.c through kl_openxr_ext_register (see kl_openxr_priv.h).
//
// SERVED AND ADVERTISED
//   XR_EXT_debug_utils (v5)     functional messenger table; submitted messages are
//                               delivered to the registered callbacks and mirrored
//                               to the log. Names/labels are bookkeeping only.
//   XR_EXT_thermal_query (v2)   level/headroom from kl_openxr_ext_set_thermal_state
//                               (Swift pushes ProcessInfo.thermalState); never fed
//                               => NORMAL with the spec's "unavailable" (-1.0f),
//                               the same "no sensor, nominal" stance as
//                               ovrp_GetSystemBatteryTemperature2 in kl_ovrp.c.
// SERVED, NOT ADVERTISED (answers a vendor/loader probe with the "nothing here" value)
//   xrStructureTypeToString2KHR, xrNegotiateLoaderRuntimeInterface,
//   xrGetAudio{Input,Output}DeviceGuidOculus, xrEnumerateExternalCamerasOCULUS,
//   xrEnumerateViveTrackerPathsHTCX, xrGetStationaryReferenceSpaceIdEXTX2,
//   xrGetFoveationEyeTrackedStateMETA (foveation is always off: flags 0).
// REFUSED BY DESIGN (never advertised; every name answers
//   XR_ERROR_FUNCTION_UNSUPPORTED, logged once): conformance_automation,
//   render_model_EXT, body/eye/face tracking, simultaneous hands+controllers,
//   desktop graphics requirements, Win32 time, loader-side API-layer entry points,
//   and the META-internal feature fidelity / tile hint / PCM sample rate.
//
// 100 % offline: nothing here touches the network.
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "kl_openxr_priv.h"

// Spec result codes not in kl_openxr_priv.h.
enum {
    KX_VALIDATION_FAILURE = -1, KX_INITIALIZATION_FAILED = -6,
    KX_LIMIT_REACHED = -10, KX_HANDLE_INVALID = -12,
    KX_SIZE_INSUFFICIENT = -11,
};
typedef uint32_t XrBool32;

// ---------------------------------------------------------------- helpers
static void kx_note(const char *fn, const char *what) {   // one log line per fn
    static const char *seen[48];
    static int n;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (seen[i] == fn) { pthread_mutex_unlock(&mu); return; }
    if (n < 48) seen[n++] = fn;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "  [xr] %s -> %s (logged once)\n", fn, what);
}

static int kx_inst_ok(void *h) {
    if (!h) return 0;
    return h == klxr_current_instance();
}
static int kx_sess_ok(void *h) { return h && h == klxr_current_session(); }

// Two-call idiom for enumerations that always have 0 entries.
static XrResult kx_empty_enum(uint32_t capacity, uint32_t *count_out) {
    (void)capacity;
    if (!count_out) return KX_VALIDATION_FAILURE;
    *count_out = 0;
    return KLXR_R_SUCCESS;
}

// =================================================================
// XR_EXT_debug_utils
// =================================================================
enum {
    KX_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO = 1000019000,
    KX_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA = 1000019001,
    KX_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO = 1000019002,
    KX_TYPE_DEBUG_UTILS_LABEL = 1000019003,
};
enum {   // XrDebugUtilsMessageSeverityFlagsEXT / TypeFlagsEXT (XrFlags64)
    KX_SEV_VERBOSE = 0x1, KX_SEV_INFO = 0x10, KX_SEV_WARNING = 0x100, KX_SEV_ERROR = 0x1000,
    KX_SEV_ALL = 0x1111,
    KX_MSG_GENERAL = 0x1, KX_MSG_VALIDATION = 0x2, KX_MSG_PERFORMANCE = 0x4,
    KX_MSG_CONFORMANCE = 0x8, KX_MSG_ALL = 0xF,
};
typedef uint32_t (*kx_dbg_cb)(uint64_t severity, uint64_t types, const void *data, void *user);

typedef struct { int32_t type; const void *next; uint64_t severities, types;
                 kx_dbg_cb callback; void *user; } kx_msgr_create_info;
typedef struct { int32_t type; const void *next; int32_t objectType; uint64_t objectHandle;
                 const char *objectName; } kx_object_name_info;
typedef struct { int32_t type; const void *next; const char *labelName; } kx_label;
typedef struct { int32_t type; const void *next; const char *messageId, *functionName, *message;
                 uint32_t objectCount; const kx_object_name_info *objects;
                 uint32_t sessionLabelCount; const kx_label *sessionLabels; } kx_cb_data;

#define KX_MSGR_MAX 16
#define KX_NAMES_MAX 64
#define KX_LOG_MSG_MAX 64
static struct { int used; uint64_t sev, types; kx_dbg_cb cb; void *user; } g_msgr[KX_MSGR_MAX];
static struct { int used; uint64_t handle; int32_t objType; char name[64]; } g_names[KX_NAMES_MAX];
static unsigned g_label_depth, g_logged_msgs;
static pthread_mutex_t g_dbg_mu = PTHREAD_MUTEX_INITIALIZER;

static XrResult kx_CreateDebugUtilsMessengerEXT(void *instance, const kx_msgr_create_info *ci,
                                                void **out) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    if (!ci || !out || ci->type != KX_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO ||
        !ci->callback || !ci->severities || (ci->severities & ~(uint64_t)KX_SEV_ALL) ||
        !ci->types || (ci->types & ~(uint64_t)KX_MSG_ALL))
        return KX_VALIDATION_FAILURE;
    XrResult rc = KX_LIMIT_REACHED;
    pthread_mutex_lock(&g_dbg_mu);
    for (int i = 0; i < KX_MSGR_MAX; i++) if (!g_msgr[i].used) {
        g_msgr[i].used = 1; g_msgr[i].sev = ci->severities; g_msgr[i].types = ci->types;
        g_msgr[i].cb = ci->callback; g_msgr[i].user = ci->user;
        *out = &g_msgr[i];
        rc = KLXR_R_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&g_dbg_mu);
    if (rc == KLXR_R_SUCCESS)
        kx_note("xrCreateDebugUtilsMessengerEXT", "messenger registered (callbacks fed by xrSubmitDebugUtilsMessageEXT)");
    return rc;
}

static XrResult kx_DestroyDebugUtilsMessengerEXT(void *h) {
    XrResult rc = KX_HANDLE_INVALID;
    pthread_mutex_lock(&g_dbg_mu);
    for (int i = 0; i < KX_MSGR_MAX; i++) if (h == (void *)&g_msgr[i] && g_msgr[i].used) {
        memset(&g_msgr[i], 0, sizeof g_msgr[i]);
        rc = KLXR_R_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&g_dbg_mu);
    return rc;
}

static const char *kx_sev_name(uint64_t s) {
    return (s & KX_SEV_ERROR) ? "ERROR" : (s & KX_SEV_WARNING) ? "WARN"
         : (s & KX_SEV_INFO) ? "INFO" : "VERBOSE";
}

static XrResult kx_SubmitDebugUtilsMessageEXT(void *instance, uint64_t severity, uint64_t types,
                                              const kx_cb_data *data) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    // Exactly one severity bit and a non-empty valid type set (spec valid usage).
    if (!data || data->type != KX_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA ||
        !severity || (severity & (severity - 1)) || (severity & ~(uint64_t)KX_SEV_ALL) ||
        !types || (types & ~(uint64_t)KX_MSG_ALL) || !data->message)
        return KX_VALIDATION_FAILURE;
    struct { kx_dbg_cb cb; void *user; } todo[KX_MSGR_MAX];
    int n = 0, log_it;
    pthread_mutex_lock(&g_dbg_mu);
    for (int i = 0; i < KX_MSGR_MAX; i++)
        if (g_msgr[i].used && (g_msgr[i].sev & severity) && (g_msgr[i].types & types)) {
            todo[n].cb = g_msgr[i].cb; todo[n].user = g_msgr[i].user; n++;
        }
    log_it = g_logged_msgs < KX_LOG_MSG_MAX ? (int)++g_logged_msgs : (g_logged_msgs == KX_LOG_MSG_MAX ? -1 : 0);
    if (log_it == -1) g_logged_msgs++;
    pthread_mutex_unlock(&g_dbg_mu);
    if (log_it > 0)
        fprintf(stderr, "  [xr] debug_utils %s: [%s] %s\n", kx_sev_name(severity),
                data->functionName ? data->functionName : "-", data->message);
    else if (log_it == -1)
        fprintf(stderr, "  [xr] debug_utils: %d messages logged, further ones go to callbacks only\n",
                KX_LOG_MSG_MAX);
    for (int i = 0; i < n; i++) todo[i].cb(severity, types, data, todo[i].user);   // return value is advisory
    return KLXR_R_SUCCESS;
}

static XrResult kx_SetDebugUtilsObjectNameEXT(void *instance, const kx_object_name_info *info) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    if (!info || info->type != KX_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO || !info->objectHandle)
        return KX_VALIDATION_FAILURE;
    pthread_mutex_lock(&g_dbg_mu);
    int slot = -1;
    for (int i = 0; i < KX_NAMES_MAX; i++) {
        if (g_names[i].used && g_names[i].handle == info->objectHandle &&
            g_names[i].objType == info->objectType) { slot = i; break; }
        if (!g_names[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {   // table full => the name is dropped; it is diagnostic only
        if (info->objectName && *info->objectName) {
            g_names[slot].used = 1; g_names[slot].handle = info->objectHandle;
            g_names[slot].objType = info->objectType;
            snprintf(g_names[slot].name, sizeof g_names[slot].name, "%s", info->objectName);
        } else {
            memset(&g_names[slot], 0, sizeof g_names[slot]);   // empty name clears
        }
    }
    pthread_mutex_unlock(&g_dbg_mu);
    return KLXR_R_SUCCESS;
}

static XrResult kx_SessionBeginDebugUtilsLabelRegionEXT(void *session, const kx_label *l) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    if (!l || l->type != KX_TYPE_DEBUG_UTILS_LABEL || !l->labelName) return KX_VALIDATION_FAILURE;
    pthread_mutex_lock(&g_dbg_mu); g_label_depth++; pthread_mutex_unlock(&g_dbg_mu);
    return KLXR_R_SUCCESS;
}
static XrResult kx_SessionEndDebugUtilsLabelRegionEXT(void *session) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    pthread_mutex_lock(&g_dbg_mu); if (g_label_depth) g_label_depth--; pthread_mutex_unlock(&g_dbg_mu);
    return KLXR_R_SUCCESS;
}
static XrResult kx_SessionInsertDebugUtilsLabelEXT(void *session, const kx_label *l) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    if (!l || l->type != KX_TYPE_DEBUG_UTILS_LABEL || !l->labelName) return KX_VALIDATION_FAILURE;
    return KLXR_R_SUCCESS;
}

// =================================================================
// XR_EXT_thermal_query (with XR_EXT_performance_settings, already served)
// =================================================================
enum { KX_PERF_DOMAIN_CPU = 1, KX_PERF_DOMAIN_GPU = 2, KX_PERF_SUBDOMAIN_THERMAL = 3,
       KX_NOTIF_NORMAL = 0, KX_NOTIF_WARNING = 25, KX_NOTIF_IMPAIRED = 75,
       KX_TYPE_EVENT_DATA_PERF_SETTINGS = 1000015000 };
#define KX_TEMP_UNAVAILABLE (-1.0f)       // XR_THERMAL_TEMPERATURE_UNAVAILABLE_EXT

static int g_thermal = -1;                // -1 never fed, else NSProcessInfoThermalState 0..3
static pthread_mutex_t g_th_mu = PTHREAD_MUTEX_INITIALIZER;

static int kx_thermal_level(int st) { return st >= 3 ? KX_NOTIF_IMPAIRED : st == 2 ? KX_NOTIF_WARNING : KX_NOTIF_NORMAL; }

// Fed from Swift: 0 nominal, 1 fair, 2 serious, 3 critical (ProcessInfo.thermalState).
// Emits XrEventDataPerfSettingsEXT (thermal sub-domain, CPU and GPU) when the level changes.
void kl_openxr_ext_set_thermal_state(int state) {
    if (state < 0) state = 0;
    if (state > 3) state = 3;
    pthread_mutex_lock(&g_th_mu);
    int from = kx_thermal_level(g_thermal < 0 ? 0 : g_thermal), to = kx_thermal_level(state);
    g_thermal = state;
    pthread_mutex_unlock(&g_th_mu);
    if (from != to && klxr_current_session()) {
        for (int d = KX_PERF_DOMAIN_CPU; d <= KX_PERF_DOMAIN_GPU; d++) {
            int32_t ev[4] = { d, KX_PERF_SUBDOMAIN_THERMAL, from, to };   // after the 16-byte header
            klxr_push_event(KX_TYPE_EVENT_DATA_PERF_SETTINGS, ev, sizeof ev);
        }
    }
}

static XrResult kx_ThermalGetTemperatureTrendEXT(void *session, int32_t domain, int32_t *level,
                                                 float *headroom, float *slope) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    if ((domain != KX_PERF_DOMAIN_CPU && domain != KX_PERF_DOMAIN_GPU) || !level || !headroom || !slope)
        return KX_VALIDATION_FAILURE;
    pthread_mutex_lock(&g_th_mu); int st = g_thermal; pthread_mutex_unlock(&g_th_mu);
    *level = kx_thermal_level(st < 0 ? 0 : st);
    // No temperature sensor is presented; only the coarse OS state is known.
    *headroom = st < 0 ? KX_TEMP_UNAVAILABLE : st == 0 ? 1.0f : st == 1 ? 0.5f : 0.0f;
    *slope = KX_TEMP_UNAVAILABLE;
    kx_note("xrThermalGetTemperatureTrendEXT", "OS thermal state only (no sensor); slope unavailable");
    return KLXR_R_SUCCESS;
}

// =================================================================
// Core / loader / vendor probes served with "nothing here"
// =================================================================
// XR_KHR_extended_struct_name_size: buffer is XR_MAX_STRUCTURE_NAME_SIZE_EXTENDED_KHR (256).
// Same form as kl_openxr.c's xrStructureTypeToString (the number is enough to look up).
static XrResult kx_StructureTypeToString2KHR(void *instance, int32_t value, char *buffer) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    if (!buffer) return KX_VALIDATION_FAILURE;
    snprintf(buffer, 256, "XR_TYPE_%d", value);
    return KLXR_R_SUCCESS;
}

// XrNegotiateLoaderInfo / XrNegotiateRuntimeRequest (openxr/loader_interfaces).
typedef struct { int32_t structType; uint32_t structVersion; size_t structSize;
                 uint32_t minInterfaceVersion, maxInterfaceVersion;
                 uint64_t minApiVersion, maxApiVersion; } kx_neg_loader_info;
typedef struct { int32_t structType; uint32_t structVersion; size_t structSize;
                 uint32_t runtimeInterfaceVersion; uint64_t runtimeApiVersion;
                 void *getInstanceProcAddr; } kx_neg_runtime_req;
enum { KX_LOADER_INFO = 1, KX_RUNTIME_REQUEST = 3 };
#define KX_API_1_1 ((1ull << 48) | (1ull << 32))

static XrResult kx_NegotiateLoaderRuntimeInterface(const kx_neg_loader_info *li, kx_neg_runtime_req *rq) {
    if (!li || !rq) return KX_VALIDATION_FAILURE;
    if (li->structType != KX_LOADER_INFO || li->structVersion != 1 || li->structSize != sizeof *li ||
        rq->structType != KX_RUNTIME_REQUEST || rq->structVersion != 1 || rq->structSize != sizeof *rq)
        return KX_INITIALIZATION_FAILED;
    if (li->minInterfaceVersion > 1 || li->maxInterfaceVersion < 1) return KX_INITIALIZATION_FAILED;
    if ((li->minApiVersion >> 48) != 1 || li->minApiVersion > KX_API_1_1) return KX_INITIALIZATION_FAILED;
    void *gipa = kl_openxr_lookup("xrGetInstanceProcAddr");
    if (!gipa) return KX_INITIALIZATION_FAILED;
    rq->runtimeInterfaceVersion = 1;
    rq->runtimeApiVersion = KX_API_1_1;
    rq->getInstanceProcAddr = gipa;
    kx_note("xrNegotiateLoaderRuntimeInterface", "interface v1, API 1.1, our xrGetInstanceProcAddr");
    return KLXR_R_SUCCESS;
}

// XR_OCULUS_audio_device_guid: wchar_t[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS = 128]; empty = system default.
static XrResult kx_GetAudioDeviceGuidOculus(void *instance, wchar_t *buf) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    if (!buf) return KX_VALIDATION_FAILURE;
    buf[0] = 0;
    return KLXR_R_SUCCESS;
}
static XrResult kx_EnumerateExternalCamerasOCULUS(void *session, uint32_t cap, uint32_t *count, void *cams) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    (void)cams;
    return kx_empty_enum(cap, count);
}
static XrResult kx_EnumerateViveTrackerPathsHTCX(void *instance, uint32_t cap, uint32_t *count, void *paths) {
    if (!kx_inst_ok(instance)) return KX_HANDLE_INVALID;
    (void)paths;
    return kx_empty_enum(cap, count);
}
// XrUuidEXT: 16 bytes. One fixed id for the single stationary space.
static XrResult kx_GetStationaryReferenceSpaceIdEXTX2(void *session, uint8_t *uuid) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    if (!uuid) return KX_VALIDATION_FAILURE;
    static const uint8_t ID[16] = { 0x4b, 0x6c, 0x65, 0x70, 0x74, 0x6f, 0x6e, 0x58,
                                    0x52, 0x2d, 0x53, 0x54, 0x41, 0x54, 0x00, 0x01 };
    memcpy(uuid, ID, 16);
    return KLXR_R_SUCCESS;
}

// XrFoveationEyeTrackedStateMETA: type, next, XrVector2f foveationCenter[2], XrFlags64 flags.
// Foveation is always off and there is no eye tracking: flags 0 (not valid), centre mid-eye.
enum { KX_TYPE_FOVEATION_EYE_TRACKED_STATE_META = 1000200001 };
static XrResult kx_GetFoveationEyeTrackedStateMETA(void *session, void *state) {
    if (!kx_sess_ok(session)) return KX_HANDLE_INVALID;
    struct { int32_t type; void *next; float center[4]; uint64_t flags; } *s = state;
    if (!s || s->type != KX_TYPE_FOVEATION_EYE_TRACKED_STATE_META) return KX_VALIDATION_FAILURE;
    s->center[0] = s->center[1] = s->center[2] = s->center[3] = 0.5f;
    s->flags = 0;
    kx_note("xrGetFoveationEyeTrackedStateMETA", "flags 0 (foveation off, no eye tracking)");
    return KLXR_R_SUCCESS;
}

// =================================================================
// registration
// =================================================================
static const klxr_row_def k_rows[] = {
    {"xrCreateDebugUtilsMessengerEXT",  (void *)kx_CreateDebugUtilsMessengerEXT},
    {"xrDestroyDebugUtilsMessengerEXT", (void *)kx_DestroyDebugUtilsMessengerEXT},
    {"xrSubmitDebugUtilsMessageEXT",    (void *)kx_SubmitDebugUtilsMessageEXT},
    {"xrSetDebugUtilsObjectNameEXT",    (void *)kx_SetDebugUtilsObjectNameEXT},
    {"xrSessionBeginDebugUtilsLabelRegionEXT", (void *)kx_SessionBeginDebugUtilsLabelRegionEXT},
    {"xrSessionEndDebugUtilsLabelRegionEXT",   (void *)kx_SessionEndDebugUtilsLabelRegionEXT},
    {"xrSessionInsertDebugUtilsLabelEXT",      (void *)kx_SessionInsertDebugUtilsLabelEXT},
    {"xrThermalGetTemperatureTrendEXT", (void *)kx_ThermalGetTemperatureTrendEXT},
    {"xrStructureTypeToString2KHR",     (void *)kx_StructureTypeToString2KHR},
    {"xrNegotiateLoaderRuntimeInterface", (void *)kx_NegotiateLoaderRuntimeInterface},
    {"xrGetAudioInputDeviceGuidOculus",  (void *)kx_GetAudioDeviceGuidOculus},
    {"xrGetAudioOutputDeviceGuidOculus", (void *)kx_GetAudioDeviceGuidOculus},
    {"xrEnumerateExternalCamerasOCULUS", (void *)kx_EnumerateExternalCamerasOCULUS},
    {"xrEnumerateViveTrackerPathsHTCX",  (void *)kx_EnumerateViveTrackerPathsHTCX},
    {"xrGetStationaryReferenceSpaceIdEXTX2", (void *)kx_GetStationaryReferenceSpaceIdEXTX2},
    {"xrGetFoveationEyeTrackedStateMETA", (void *)kx_GetFoveationEyeTrackedStateMETA},
};

static const char *const k_debug_fns[] = {
    "xrCreateDebugUtilsMessengerEXT", "xrDestroyDebugUtilsMessengerEXT",
    "xrSubmitDebugUtilsMessageEXT", "xrSetDebugUtilsObjectNameEXT",
    "xrSessionBeginDebugUtilsLabelRegionEXT", "xrSessionEndDebugUtilsLabelRegionEXT",
    "xrSessionInsertDebugUtilsLabelEXT", NULL};
static const char *const k_thermal_fns[] = {"xrThermalGetTemperatureTrendEXT", NULL};

// Refused by design: no state to inject into (conformance), no data on this host (body/eye/face),
// not a GLES/Vulkan-to-guest session (desktop graphics), not Windows, or not ours (layer side).
static const char *const k_conf_fns[] = {
    "xrSetInputDeviceActiveEXT", "xrSetInputDeviceStateBoolEXT", "xrSetInputDeviceStateFloatEXT",
    "xrSetInputDeviceStateVector2fEXT", "xrSetInputDeviceLocationEXT",
    "xrSetInputDeviceVelocityUNITY", NULL};
static const char *const k_rendermodel_fns[] = {
    "xrCreateRenderModelEXT", "xrDestroyRenderModelEXT", "xrGetRenderModelPropertiesEXT",
    "xrCreateRenderModelSpaceEXT", "xrCreateRenderModelAssetEXT", "xrDestroyRenderModelAssetEXT",
    "xrGetRenderModelAssetDataEXT", "xrGetRenderModelAssetPropertiesEXT",
    "xrGetRenderModelStateEXT", "xrGetRenderModelPoseTopLevelUserPathEXT",
    "xrEnumerateRenderModelSubactionPathsEXT", "xrEnumerateInteractionRenderModelIdsEXT", NULL};
static const char *const k_body_fns[] = {
    "xrCreateBodyTrackerFB", "xrDestroyBodyTrackerFB", "xrGetBodySkeletonFB", "xrLocateBodyJointsFB",
    "xrRequestBodyTrackingFidelityMETA", "xrSuggestBodyTrackingCalibrationOverrideMETA",
    "xrResetBodyTrackingCalibrationMETA", NULL};
static const char *const k_eye_fns[] = {
    "xrCreateEyeTrackerFB", "xrDestroyEyeTrackerFB", "xrGetEyeGazesFB", NULL};
static const char *const k_face_fns[] = {
    "xrCreateFaceTrackerFB", "xrDestroyFaceTrackerFB", "xrGetFaceExpressionWeightsFB",
    "xrCreateFaceTracker2FB", "xrDestroyFaceTracker2FB", "xrGetFaceExpressionWeights2FB", NULL};
static const char *const k_simul_fns[] = {
    "xrPauseSimultaneousHandsAndControllersTrackingMETA",
    "xrResumeSimultaneousHandsAndControllersTrackingMETA",
    "xrPauseSimultaneousHandsControllersTrackingMETAX1",
    "xrResumeSimultaneousHandsControllersTrackingMETAX1", NULL};
static const char *const k_meta_fns[] = {
    "xrSetFeatureEnabledMETA", "xrSetFeatureFidelityMETA", "xrGetFeatureFidelityStateMETA",
    "xrGetRecommendedLayerResolutionMETA", "xrSetTilePropertiesHintMETA",
    "xrGetDeviceSampleRateFB", NULL};
static const char *const k_core_fns[] = {
    "xrConvertTimeToWin32PerformanceCounterKHR", "xrConvertWin32PerformanceCounterToTimeKHR",
    "xrGetD3D11GraphicsRequirementsKHR", "xrGetD3D12GraphicsRequirementsKHR",
    "xrGetOpenGLGraphicsRequirementsKHR", "xrGetMetalGraphicsRequirementsKHR",
    "xrHapticParametricGetPropertiesEXTX1", "xrNegotiateLoaderApiLayerInterface",
    "xrCreateApiLayerInstance", NULL};

void kl_openxr_ext_register(void) {
    klxr_register_rows(k_rows, sizeof k_rows / sizeof k_rows[0]);
    klxr_register_extension("XR_EXT_debug_utils", 5, NULL, k_debug_fns);
    klxr_register_extension("XR_EXT_thermal_query", 2, NULL, k_thermal_fns);

    klxr_register_refused("XR_EXT_conformance_automation", k_conf_fns);
    klxr_register_refused("XR_EXT_render_model", k_rendermodel_fns);
    klxr_register_refused("XR_FB_body_tracking/META_body_tracking", k_body_fns);
    klxr_register_refused("XR_FB_eye_tracking_social", k_eye_fns);
    klxr_register_refused("XR_FB_face_tracking(2)", k_face_fns);
    klxr_register_refused("XR_META_simultaneous_hands_and_controllers", k_simul_fns);
    klxr_register_refused("XR_META (feature fidelity, tile hint, PCM rate, rec. resolution)", k_meta_fns);
    klxr_register_refused("core/KHR (Win32, D3D, GL, Metal, layer entry points)", k_core_fns);
}
