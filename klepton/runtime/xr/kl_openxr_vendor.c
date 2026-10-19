// kl_openxr_vendor.c - Lote 6: refusal table for non-Meta vendor extensions
// (ML, BD, MSFT, HTC, ANDROID, VARJO, QCOM, MNDX, ALMALENCE). Registered with
// kl_openxr.c through kl_openxr_vendor_register (see kl_openxr_priv.h).
//
// None of these extensions is advertised in xrEnumerateInstanceExtensionProperties,
// so a conforming app never asks for them; the names below are what a title (or
// the engine plugin it embeds) resolves speculatively through xrGetInstanceProcAddr.
// Each one gets a row that answers XR_ERROR_FUNCTION_UNSUPPORTED, logged once and
// counted, and no argument is read (the signatures are irrelevant). A name that
// another batch or the core table already serves is left alone by
// klxr_register_refused. Nothing here touches the network or an account.
#include "kl_openxr_priv.h"

static const char *const k_ml[] = {
    "xrAllocateWorldMeshBufferML", "xrCreateExportedLocalizationMapML",
    "xrCreateFacialExpressionClientML", "xrCreateMarkerDetectorML", "xrCreateMarkerSpaceML",
    "xrCreateSpaceFromCoordinateFrameUIDML", "xrCreateSpatialAnchorsAsyncML",
    "xrCreateSpatialAnchorsCompleteML", "xrCreateSpatialAnchorsStorageML",
    "xrCreateWorldMeshDetectorML", "xrDeleteSpatialAnchorsAsyncML",
    "xrDeleteSpatialAnchorsCompleteML", "xrDestroyExportedLocalizationMapML",
    "xrDestroyFacialExpressionClientML", "xrDestroyMarkerDetectorML",
    "xrDestroySpatialAnchorsStorageML", "xrDestroyWorldMeshDetectorML",
    "xrEnableLocalizationEventsML", "xrEnableUserCalibrationEventsML",
    "xrFreeWorldMeshBufferML", "xrGetExportedLocalizationMapDataML",
    "xrGetFacialExpressionBlendShapePropertiesML", "xrGetMarkerDetectorStateML",
    "xrGetMarkerLengthML", "xrGetMarkerNumberML", "xrGetMarkerReprojectionErrorML",
    "xrGetMarkerStringML", "xrGetMarkersML", "xrGetSpatialAnchorStateML",
    "xrGetWorldMeshBufferRecommendSizeML", "xrImportLocalizationMapML",
    "xrPublishSpatialAnchorsAsyncML", "xrPublishSpatialAnchorsCompleteML",
    "xrQueryLocalizationMapsML", "xrQuerySpatialAnchorsAsyncML",
    "xrQuerySpatialAnchorsCompleteML", "xrRequestMapLocalizationML",
    "xrRequestWorldMeshAsyncML", "xrRequestWorldMeshCompleteML",
    "xrRequestWorldMeshStateAsyncML", "xrRequestWorldMeshStateCompleteML",
    "xrSetSystemNotificationsML", "xrSnapshotMarkerDetectorML",
    "xrUpdateSpatialAnchorsExpirationAsyncML", "xrUpdateSpatialAnchorsExpirationCompleteML", NULL,
};

static const char *const k_bd[] = {
    "xrCaptureSceneAsyncBD", "xrCaptureSceneCompleteBD", "xrCreateAnchorSpaceBD",
    "xrCreateBodyTrackerBD", "xrCreateFaceTrackerBD", "xrCreateSenseDataProviderBD",
    "xrCreateSoundFieldBD", "xrCreateSoundObjectBD", "xrCreateSoundObstacleBD",
    "xrCreateSoundObstacleMaterialBD", "xrCreateSpatialAnchorAsyncBD",
    "xrCreateSpatialAnchorCompleteBD", "xrCreateSpatialAudioRendererBD",
    "xrCreateSpatialEntityAnchorBD", "xrDestroyAnchorBD", "xrDestroyBodyTrackerBD",
    "xrDestroyFaceTrackerBD", "xrDestroySenseDataProviderBD", "xrDestroySenseDataSnapshotBD",
    "xrDestroySoundFieldBD", "xrDestroySoundObjectBD", "xrDestroySoundObstacleBD",
    "xrDestroySoundObstacleMaterialBD", "xrDestroySpatialAudioRendererBD",
    "xrDownloadSharedSpatialAnchorAsyncBD", "xrDownloadSharedSpatialAnchorCompleteBD",
    "xrEndAudioPeriodBD", "xrEnumerateFacialSimulationModesBD",
    "xrEnumerateSpatialEntityComponentTypesBD", "xrEnumerateSupportedAudioSampleRateBD",
    "xrGetAnchorUuidBD", "xrGetFacialSimulationDataBD", "xrGetFacialSimulationModeBD",
    "xrGetQueriedSenseDataBD", "xrGetSenseDataProviderStateBD",
    "xrGetSpatialEntityComponentDataBD", "xrGetSpatialEntityUuidBD", "xrLocateBodyJointsBD",
    "xrPersistSpatialAnchorAsyncBD", "xrPersistSpatialAnchorCompleteBD",
    "xrQueryFramesPerBufferRangeBD", "xrQuerySenseDataAsyncBD", "xrQuerySenseDataCompleteBD",
    "xrSetFacialSimulationModeBD", "xrShareSpatialAnchorAsyncBD",
    "xrShareSpatialAnchorCompleteBD", "xrStartSenseDataProviderAsyncBD",
    "xrStartSenseDataProviderCompleteBD", "xrStopSenseDataProviderBD",
    "xrSubmitSoundFieldBufferBD", "xrSubmitSoundObjectBufferBD",
    "xrUnpersistSpatialAnchorAsyncBD", "xrUnpersistSpatialAnchorCompleteBD",
    "xrUpdateSoundFieldConfigBD", "xrUpdateSoundObjectConfigBD",
    "xrUpdateSoundObstacleConfigBD", "xrUpdateSoundObstacleMaterialConfigBD",
    "xrWaitAudioPeriodBD", NULL,
};

static const char *const k_msft[] = {
    "xrClearSpatialAnchorStoreMSFT", "xrComputeNewSceneMSFT", "xrCreateHandMeshSpaceMSFT",
    "xrCreateSceneMSFT", "xrCreateSceneObserverMSFT",
    "xrCreateSpatialAnchorFromPerceptionAnchorMSFT",
    "xrCreateSpatialAnchorFromPersistedNameMSFT", "xrCreateSpatialAnchorMSFT",
    "xrCreateSpatialAnchorSpaceMSFT", "xrCreateSpatialAnchorStoreConnectionMSFT",
    "xrCreateSpatialGraphNodeSpaceMSFT", "xrDeserializeSceneMSFT", "xrDestroySceneMSFT",
    "xrDestroySceneObserverMSFT", "xrDestroySpatialAnchorMSFT",
    "xrDestroySpatialAnchorStoreConnectionMSFT", "xrDestroySpatialGraphNodeBindingMSFT",
    "xrEnumeratePersistedSpatialAnchorNamesMSFT", "xrEnumerateReprojectionModesMSFT",
    "xrEnumerateSceneComputeFeaturesMSFT", "xrGetControllerModelKeyMSFT",
    "xrGetControllerModelPropertiesMSFT", "xrGetControllerModelStateMSFT",
    "xrGetSceneComponentsMSFT", "xrGetSceneComputeStateMSFT",
    "xrGetSceneMarkerDecodedStringMSFT", "xrGetSceneMarkerRawDataMSFT",
    "xrGetSceneMeshBuffersMSFT", "xrGetSerializedSceneFragmentDataMSFT",
    "xrGetSpatialGraphNodeBindingPropertiesMSFT", "xrLoadControllerModelMSFT",
    "xrLocateSceneComponentsMSFT", "xrPersistSpatialAnchorMSFT",
    "xrTryCreateSpatialGraphStaticNodeBindingMSFT",
    "xrTryGetPerceptionAnchorFromSpatialAnchorMSFT", "xrUnpersistSpatialAnchorMSFT",
    "xrUpdateHandMeshMSFT", NULL,
};

static const char *const k_htc[] = {
    "xrApplyFoveationHTC", "xrCreateBodyTrackerHTC", "xrCreateFacialTrackerHTC",
    "xrCreatePassthroughHTC", "xrCreateSpatialAnchorHTC", "xrDestroyBodyTrackerHTC",
    "xrDestroyFacialTrackerHTC", "xrDestroyPassthroughHTC", "xrGetBodySkeletonHTC",
    "xrGetFacialExpressionsHTC", "xrGetSpatialAnchorNameHTC", "xrLocateBodyJointsHTC", NULL,
};

static const char *const k_android[] = {
    "xrAddTrackableImageDatabaseANDROID", "xrCreateAnchorSpaceANDROID",
    "xrCreateDeviceAnchorPersistenceANDROID", "xrCreateFaceTrackerANDROID",
    "xrCreatePassthroughLayerANDROID", "xrCreatePersistedAnchorSpaceANDROID",
    "xrCreateSceneMeshSnapshotANDROID", "xrCreateSceneMeshingTrackerANDROID",
    "xrCreateSpatialAnchorSpaceANDROID", "xrCreateSpatialAnchorSpaceFromIdANDROID",
    "xrCreateSpatialRaycastSnapshotANDROID", "xrCreateTrackableImageDatabaseAsyncANDROID",
    "xrCreateTrackableImageDatabaseCompleteANDROID", "xrCreateTrackableTrackerANDROID",
    "xrDestroyDeviceAnchorPersistenceANDROID", "xrDestroyFaceTrackerANDROID",
    "xrDestroyPassthroughLayerANDROID", "xrDestroySceneMeshSnapshotANDROID",
    "xrDestroySceneMeshingTrackerANDROID", "xrDestroyTrackableImageDatabaseANDROID",
    "xrDestroyTrackableTrackerANDROID", "xrEnumeratePerformanceMetricsCounterPathsANDROID",
    "xrEnumeratePersistedAnchorsANDROID", "xrEnumerateRaycastSupportedTrackableTypesANDROID",
    "xrEnumerateSpatialAnchorAttachableComponentsANDROID",
    "xrEnumerateSupportedAnchorTrackableTypesANDROID",
    "xrEnumerateSupportedPersistenceAnchorTypesANDROID",
    "xrEnumerateSupportedSemanticLabelSetsANDROID", "xrEnumerateSupportedTrackableTypesANDROID",
    "xrGetAllSubmeshStatesANDROID", "xrGetAllTrackablesANDROID",
    "xrGetAnchorPersistStateANDROID", "xrGetFaceCalibrationStateANDROID",
    "xrGetFaceStateANDROID", "xrGetPassthroughCameraStateANDROID",
    "xrGetPerformanceMetricsStateANDROID", "xrGetSubmeshDataANDROID",
    "xrGetTrackableImageANDROID", "xrGetTrackableMarkerANDROID", "xrGetTrackableObjectANDROID",
    "xrGetTrackablePlaneANDROID", "xrGetTrackableQrCodeANDROID", "xrPersistAnchorANDROID",
    "xrQueryPerformanceMetricsCounterANDROID", "xrRaycastANDROID",
    "xrRemoveTrackableImageDatabaseANDROID", "xrSetPassthroughLayerMeshANDROID",
    "xrSetPerformanceMetricsStateANDROID", "xrShareAnchorANDROID", "xrUnpersistAnchorANDROID",
    "xrUnshareAnchorANDROID", NULL,
};

static const char *const k_varjo[] = {
    "xrCreateMarkerSpaceVARJO", "xrGetMarkerSizeVARJO", "xrSetEnvironmentDepthEstimationVARJO",
    "xrSetMarkerTrackingPredictionVARJO", "xrSetMarkerTrackingTimeoutVARJO",
    "xrSetMarkerTrackingVARJO", "xrSetViewOffsetVARJO", NULL,
};

static const char *const k_qcom[] = {
    "xrGetHandGestureQCOM", "xrSetTrackingOptimizationSettingsHintQCOM", NULL,
};

static const char *const k_mndx[] = {
    "xrApplyForceFeedbackCurlMNDX", NULL,
};

static const char *const k_almalence[] = {
    "xrSetDigitalLensControlALMALENCE", NULL,
};

void kl_openxr_vendor_register(void) {
    klxr_register_refused("XR_ML", k_ml);
    klxr_register_refused("XR_BD", k_bd);
    klxr_register_refused("XR_MSFT", k_msft);
    klxr_register_refused("XR_HTC", k_htc);
    klxr_register_refused("XR_ANDROID", k_android);
    klxr_register_refused("XR_VARJO", k_varjo);
    klxr_register_refused("XR_QCOM", k_qcom);
    klxr_register_refused("XR_MNDX", k_mndx);
    klxr_register_refused("XR_ALMALENCE", k_almalence);
}
