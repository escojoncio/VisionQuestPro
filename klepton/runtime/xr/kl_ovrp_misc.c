// kl_ovrp_misc.c - Lote 6: ovrp miscellany (device cameras, MRC / external camera,
// keyboards, controller render models, telemetry, KTX2, MetaWandAuth, futures).
//
// Registered with kl_ovrp.c through kl_ovrp_misc_register (recipe in kl_ovrp_priv.h).
//
// Policy: this host is fully offline and has no camera, MRC, keyboard or render
// model service. Two answers only, and neither dereferences an argument:
//
//  * ACCEPT (k_tab): returns 0 with nothing written. Used for telemetry, QPL,
//    unified events and metadata (accepted and dropped, nothing leaves the
//    device), for setters/overrides of absent hardware, for Close/Unregister/
//    Cancel/Destroy, and for the "Has/Is/Does..." predicates of absent hardware:
//    0 is "false" when the guest reads the v1 `ovrpBool f(...)` shape and
//    "success, out left at the caller's zero" when it reads the `ovrpResult
//    f(..., ovrpBool *out)` shape, so a predicate is "no" under either ABI.
//    EXCEPT where the real build's ABI is known (tools/ovrp_abi.py over nine
//    OVRPlugin versions): the capability predicates below the OK() list are
//    ovrpResult f(T *out) with a 4-byte out through x0 in every build, so they
//    write their 0 ("no") instead of leaving it to the caller's zeroing.
//  * REFUSE (k_refused): ovrpFailure_Unsupported (-1004), logged once, by name,
//    through the shared handler. Used for everything that would have to hand
//    back a device, a frame, a pose, a model, a token or a handle.
//
// Out-params are NOT written by the remaining OK()/refusal entries: their
// signatures are not verified (a guessed `out` that is really an enum or a size
// would be a wild write). A guest that pre-zeroes its locals (the C# marshaller
// does) reads the untouched value as "none / empty / false". The functions
// whose out-pointer the disassembly confirms are written explicitly below.
//
// Not simulated, on purpose (same line as kl_ovrplat.h: never forge ownership):
// MetaWandAuth_Authenticate / GetAccessToken are refused, HasAccessToken and
// IsAuthenticating say no, DeclareUser records nothing and is not a session.
//
// KTX2: ovrp_Ktx* is Meta's glTF/KTX2 loader for controller render models. Render
// models are unavailable on this host (ovrp_GetRenderModel* refused), so nothing
// can hand a KTX2 blob to it; and the useful case, Basis Universal supercompression
// (ETC1S/UASTC), needs a transcoder that is not in the tree. A header-only parser
// would implement a loader whose only caller is unreachable and whose Transcode
// would be refused anyway, so the whole family is refused, except KtxDestroy
// (accepted: destroying the handle that was never given is a no-op).
//
// Events: ovrp_RegisterOpenXREventHandler is refused rather than accepted: the
// callback is fed by the OpenXR event pump, and kl_openxr.c exposes no hook to
// call it, so accepting would promise events that never arrive. Shutdown-handler
// registration is accepted (a shutdown event simply never fires). Futures:
// XR_EXT_future is not served here, so no future exists to poll (PollFuture
// refused) or cancel (CancelFuture accepted).
#include "kl_ovrp_priv.h"

#define OK(f) static ovrpResult klm_##f(void) { return kl_ovrp_ok("ovrp_" #f); }

OK(CloseCameraDevice)
OK(HasCameraDeviceOpened)
OK(HasCameraDeviceOpened2)
OK(IsCameraDeviceAvailable2)
OK(DoesCameraDeviceSupportDepth)
OK(IsCameraDeviceColorFrameAvailable)
OK(IsCameraDeviceColorFrameAvailable2)
OK(IsCameraDeviceDepthFrameAvailable)
OK(SetCameraDeviceDepthSensingMode)
OK(SetCameraDevicePreferredColorFrameSize)
OK(SetCameraDevicePreferredDepthQuality)
OK(OverrideExternalCameraFov)
OK(OverrideExternalCameraStaticPose)
OK(ResetDefaultExternalCamera)
OK(SetDefaultExternalCamera)
OK(Media_SetMrcFrameSize)
OK(SetKeyboardOverlayPose)
OK(SetKeyboardOverlayUV)
OK(TelemetryGetMetadata)
OK(TelemetrySetMetadata)
OK(TelemetrySetMetadataBool)
OK(TelemetrySetMetadataDouble)
OK(TelemetrySetMetadataFloat)
OK(TelemetrySetMetadataInt)
OK(AddCustomMetadata)
OK(QplMarkerAnnotationVariant)
OK(QplMarkerPointData)
OK(QplMarkerStartForJoin)
OK(SendUnifiedEvent)
OK(SendUnifiedEventV2)
OK(SendUnifiedEventV3)
OK(SendUnifiedEventV4)
OK(DeclareUser)
OK(MetaWandAuth_HasAccessToken)
OK(MetaWandAuth_IsAuthenticating)
OK(MetaWandAuth_Logout)
OK(MetaWandAuth_Stop)
OK(UnregisterOpenXREventHandler)
OK(RegisterShutdownEventHandler)
OK(UnregisterShutdownEventHandler)
OK(CancelFuture)
OK(KtxDestroy)

// ovrpResult f(T *out), the out pointer being the only argument (1.117: RESULT,
// x0 ptr [w 0..4], -1001 on NULL): this host has none of these capabilities, so
// the answer is 0 / false / none.
static ovrpResult klm_GetEnvironmentDepthSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetEnvironmentDepthSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetEnvironmentDepthHandRemovalSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetEnvironmentDepthHandRemovalSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetBodyTrackingEnabled(int32_t *o) { kl_ovrp_hit("ovrp_GetBodyTrackingEnabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetBodyTrackingSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetBodyTrackingSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTrackingEnabled(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTrackingEnabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTrackingSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTrackingSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTrackingVisemesEnabled(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTrackingVisemesEnabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTrackingVisemesSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTrackingVisemesSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTracking2Enabled(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTracking2Enabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFaceTracking2Supported(int32_t *o) { kl_ovrp_hit("ovrp_GetFaceTracking2Supported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetReorientHMDOnControllerRecenter(int32_t *o) { kl_ovrp_hit("ovrp_GetReorientHMDOnControllerRecenter"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetFullBodyTrackingEnabled(int32_t *o) { kl_ovrp_hit("ovrp_GetFullBodyTrackingEnabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetAppHasSystemOverlayPresent(int32_t *o) { kl_ovrp_hit("ovrp_GetAppHasSystemOverlayPresent"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetBoundaryVisible2(int32_t *o) { kl_ovrp_hit("ovrp_GetBoundaryVisible2"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetLocalDimming(int32_t *o) { kl_ovrp_hit("ovrp_GetLocalDimming"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetSuggestedCpuPerformanceLevel(int32_t *o) { kl_ovrp_hit("ovrp_GetSuggestedCpuPerformanceLevel"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetSuggestedGpuPerformanceLevel(int32_t *o) { kl_ovrp_hit("ovrp_GetSuggestedGpuPerformanceLevel"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetGPUUtilSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetGPUUtilSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetExternalCameraCount(int32_t *o) { kl_ovrp_hit("ovrp_GetExternalCameraCount"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetTiledMultiResDynamic(int32_t *o) { kl_ovrp_hit("ovrp_GetTiledMultiResDynamic"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetTiledMultiResSupported(int32_t *o) { kl_ovrp_hit("ovrp_GetTiledMultiResSupported"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_IsMrcEnabled(int32_t *o) { kl_ovrp_hit("ovrp_Media_IsMrcEnabled"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_IsMrcActivated(int32_t *o) { kl_ovrp_hit("ovrp_Media_IsMrcActivated"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_UseMrcDebugCamera(int32_t *o) { kl_ovrp_hit("ovrp_Media_UseMrcDebugCamera"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_GetInitialized(int32_t *o) { kl_ovrp_hit("ovrp_Media_GetInitialized"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }

// ovrpResult f(ovrpBool *out) / f(enum *out) with the out pointer the only argument (1.94: x0 ptr
// forwarded to a virtual call, RESULT): a success the caller reads back must have written. Absent
// hardware, so 0: not eye-tracked, input buffer type 0 (Memory), not casting.
static ovrpResult klm_GetFoveationEyeTracked(int32_t *o) { kl_ovrp_hit("ovrp_GetFoveationEyeTracked"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_GetMrcInputVideoBufferType(int32_t *o) { kl_ovrp_hit("ovrp_Media_GetMrcInputVideoBufferType"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_Media_IsCastingToRemoteClient(int32_t *o) { kl_ovrp_hit("ovrp_Media_IsCastingToRemoteClient"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
// ovrpResult f(int cameraId, ovrpBool *useOverridden) (1.94: x0 i32, x1 ptr): no external camera, so
// nothing is overridden.
static ovrpResult klm_GetUseOverriddenExternalCameraFov(int camera, int32_t *o) { (void)camera; kl_ovrp_hit("ovrp_GetUseOverriddenExternalCameraFov"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }
static ovrpResult klm_GetUseOverriddenExternalCameraStaticPose(int camera, int32_t *o) { (void)camera; kl_ovrp_hit("ovrp_GetUseOverriddenExternalCameraStaticPose"); if (!o) return KLOVRP_INVALID_PARAM; *o = 0; return KLOVRP_SUCCESS; }

// ovrpResult ovrp_TelemetryCreateMetadataHandle(int *out) (1.117 only) and
// ovrpResult ovrp_QplCreateMarkerHandle(const char *name, int *out): a handle the
// guest passes back to calls that drop their data, so any non-zero value will do.
static ovrpResult klm_TelemetryCreateMetadataHandle(int32_t *o) {
    kl_ovrp_hit("ovrp_TelemetryCreateMetadataHandle");
    if (!o) return KLOVRP_INVALID_PARAM;
    *o = 1;
    return KLOVRP_SUCCESS;
}
static ovrpResult klm_QplCreateMarkerHandle(const char *name, int32_t *o) {
    (void)name;
    kl_ovrp_hit("ovrp_QplCreateMarkerHandle");
    if (!o) return KLOVRP_INVALID_PARAM;
    *o = 1;
    return KLOVRP_SUCCESS;
}
// ovrpResult ovrp_GetNativeSDKPointer2(void **out): 8 bytes through x0. There is
// no native SDK here (libvrapi is never loaded): NULL.
static ovrpResult klm_GetNativeSDKPointer2(void **o) {
    kl_ovrp_hit("ovrp_GetNativeSDKPointer2");
    if (!o) return KLOVRP_INVALID_PARAM;
    *o = NULL;
    return KLOVRP_SUCCESS;
}

#define E(f) {"ovrp_" #f, (void *)klm_##f}
static const kl_ovrp_entry k_tab[] = {
    E(GetEnvironmentDepthSupported),
    E(GetEnvironmentDepthHandRemovalSupported),
    E(GetFoveationEyeTracked),
    E(Media_GetMrcInputVideoBufferType),
    E(GetBodyTrackingEnabled),
    E(GetBodyTrackingSupported),
    E(GetFaceTrackingEnabled),
    E(GetFaceTrackingSupported),
    E(GetFaceTrackingVisemesEnabled),
    E(GetFaceTrackingVisemesSupported),
    E(GetFaceTracking2Enabled),
    E(GetFaceTracking2Supported),
    E(GetReorientHMDOnControllerRecenter),
    E(GetFullBodyTrackingEnabled),
    E(GetAppHasSystemOverlayPresent),
    E(GetBoundaryVisible2),
    E(GetLocalDimming),
    E(GetSuggestedCpuPerformanceLevel),
    E(GetSuggestedGpuPerformanceLevel),
    E(GetGPUUtilSupported),
    E(GetExternalCameraCount),
    E(GetTiledMultiResDynamic),
    E(GetTiledMultiResSupported),
    E(Media_IsMrcEnabled),
    E(Media_IsMrcActivated),
    E(Media_UseMrcDebugCamera),
    E(Media_GetInitialized),

    E(CloseCameraDevice), E(HasCameraDeviceOpened), E(HasCameraDeviceOpened2),
    E(IsCameraDeviceAvailable2), E(DoesCameraDeviceSupportDepth),
    E(IsCameraDeviceColorFrameAvailable), E(IsCameraDeviceColorFrameAvailable2),
    E(IsCameraDeviceDepthFrameAvailable), E(SetCameraDeviceDepthSensingMode),
    E(SetCameraDevicePreferredColorFrameSize), E(SetCameraDevicePreferredDepthQuality),
    E(GetUseOverriddenExternalCameraFov), E(GetUseOverriddenExternalCameraStaticPose),
    E(OverrideExternalCameraFov), E(OverrideExternalCameraStaticPose),
    E(ResetDefaultExternalCamera), E(SetDefaultExternalCamera),
    E(Media_IsCastingToRemoteClient), E(Media_SetMrcFrameSize), E(SetKeyboardOverlayPose),
    E(SetKeyboardOverlayUV), E(TelemetryCreateMetadataHandle), E(QplCreateMarkerHandle),
    E(GetNativeSDKPointer2), E(TelemetryGetMetadata),
    E(TelemetrySetMetadata), E(TelemetrySetMetadataBool), E(TelemetrySetMetadataDouble),
    E(TelemetrySetMetadataFloat), E(TelemetrySetMetadataInt), E(AddCustomMetadata),
    E(QplMarkerAnnotationVariant), E(QplMarkerPointData), E(QplMarkerStartForJoin),
    E(SendUnifiedEvent), E(SendUnifiedEventV2), E(SendUnifiedEventV3), E(SendUnifiedEventV4),
    E(DeclareUser), E(MetaWandAuth_HasAccessToken), E(MetaWandAuth_IsAuthenticating),
    E(MetaWandAuth_Logout), E(MetaWandAuth_Stop), E(UnregisterOpenXREventHandler),
    E(RegisterShutdownEventHandler), E(UnregisterShutdownEventHandler), E(CancelFuture),
    E(KtxDestroy),
};
#undef E

static const char *const k_refused[] = {
    "ovrp_EnumerateAllCameraDevices", "ovrp_EnumerateAvailableCameraDevices",
    "ovrp_OpenCameraDevice", "ovrp_GetCameraDeviceColorFrameBgraPixels",
    "ovrp_GetCameraDeviceColorFrameSize", "ovrp_GetCameraDeviceDepthConfidencePixels",
    "ovrp_GetCameraDeviceDepthFramePixels", "ovrp_GetCameraDeviceDepthFrameSize",
    "ovrp_GetCameraDeviceDepthSensingMode", "ovrp_GetCameraDeviceIntrinsicsParameters",
    "ovrp_GetCameraDevicePreferredDepthQuality", "ovrp_ConvertPoseToCameraSpace",
    "ovrp_GetExternalCameraCalibrationRawPose", "ovrp_GetExternalCameraExtrinsics",
    "ovrp_GetExternalCameraIntrinsics", "ovrp_GetExternalCameraName",
    "ovrp_GetExternalCameraPose", "ovrp_Media_CreateCustomCameraAnchor",
    "ovrp_Media_EncodeMrcFrame", "ovrp_Media_EncodeMrcFrameDualTexturesWithPoseTime",
    "ovrp_Media_EncodeMrcFrameWithDualTextures", "ovrp_Media_EncodeMrcFrameWithPoseTime",
    "ovrp_Media_EnumerateCameraAnchorHandles", "ovrp_Media_GetCameraAnchorHandle",
    "ovrp_Media_GetCameraAnchorName", "ovrp_Media_GetCameraAnchorType",
    "ovrp_Media_GetCameraMinMaxDistance", "ovrp_Media_GetCurrentCameraAnchorHandle",
    "ovrp_Media_GetCustomCameraAnchorPose", "ovrp_Media_GetMrcAudioSampleRate",
    "ovrp_Media_GetMrcFrameImageFlipped", "ovrp_Media_GetMrcFrameInverseAlpha",
    "ovrp_Media_GetMrcFrameSize", "ovrp_Media_GetPlatformCameraMode", "ovrp_Media_SyncMrcFrame",
    // The first four exist only in OVRPlugin 1.81 .. 1.104 (absent from 1.59, 1.110
    // and 1.117). All four are ovrpResult in every build that has them
    // (RESULT{-1002}, arguments only forwarded to a virtual call), so -1004 with
    // nothing written is the coherent refusal; no keyboard exists on this host.
    "ovrp_GetSystemKeyboardDescription", "ovrp_GetKeyboardState", "ovrp_StartKeyboardTracking",
    "ovrp_StopKeyboardTracking", "ovrp_CreateVirtualKeyboard", "ovrp_DestroyVirtualKeyboard",
    "ovrp_CreateVirtualKeyboardSpace", "ovrp_SuggestVirtualKeyboardLocation",
    "ovrp_GetVirtualKeyboardScale", "ovrp_SetVirtualKeyboardModelVisibility",
    "ovrp_GetVirtualKeyboardModelAnimationStates", "ovrp_GetVirtualKeyboardDirtyTextures",
    "ovrp_GetVirtualKeyboardTextureData", "ovrp_ChangeVirtualKeyboardTextContext",
    "ovrp_SendVirtualKeyboardInput", "ovrp_GetRenderModelPaths",
    "ovrp_GetRenderModelProperties", "ovrp_GetRenderModelProperties2", "ovrp_LoadRenderModel",
    "ovrp_MetaWandAuth_Authenticate", "ovrp_MetaWandAuth_GetAccessToken",
    "ovrp_KtxLoadFromMemory", "ovrp_KtxGetTextureData", "ovrp_KtxNeedsTranscoding",
    "ovrp_KtxTextureHeight", "ovrp_KtxTextureSize", "ovrp_KtxTextureWidth", "ovrp_KtxTranscode",
    "ovrp_RegisterOpenXREventHandler", "ovrp_PollFuture",
    // Telemetry consent (OVRPlugin 1.110 only, absent from 1.89 / 1.94): no arguments of their own,
    // ovrpResult only (the engine's bool as 0 / -1000, not buffer getters). Success would mean "there is
    // text / a notification to show", and no consent flow exists on this host, so -1004.
    // (ovrp_GetUnifiedConsent is not here: it returns the enum BY VALUE.)
    "ovrp_ShouldShowTelemetryNotification", "ovrp_GetConsentSettingsChangeText",
    "ovrp_GetConsentTitle", "ovrp_GetConsentMarkdownText",
    "ovrp_GetConsentNotificationMarkdownText", "ovrp_GetConsentTitleText",
    "ovrp_GetConsentNotificationText", "ovrp_GetConsentText", NULL,
};

void kl_ovrp_misc_register(void) {
    kl_ovrp_register(k_tab, sizeof k_tab / sizeof k_tab[0], k_refused);
}
