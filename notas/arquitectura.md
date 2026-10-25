# Arquitectura y ficheros clave

## Runtime (`klepton/runtime/`)
Fuentes en `klepton/Makefile` (`RUNTIME_SHIP`/`RUNTIME_JNI` → `libklepton.a`; Xcode no lista fuentes sueltas).
- `libc/`: `kl_libc.c` (memoria/presión, redirección `*/Content/Paks/*.lang` al obb), `kl_pthread.c` (rwlocks propios mutex+cond), `kl_shim.c` (símbolos, `kl_shim_known`, `kl_android_log_write`), `kl_bionic_extra*.c` (~800 funciones; `extra4` = zlib), `kl_va_thunks.S` (variádicos JNI AAPCS64), `kl_wait.h` (esperas por hilo).
- `kl_jni.c` + `jni/kl_jni_*.c`: JNI sintético (net/Settings, window, ue4, hostvideo, display, looper, services, softinput, fmod, bridge, android). `kl_fault.c`: cabecera de crash. `kl_x18.c` (`kl_x18_site_span`). `kl_guestpatch.c` (parches por build con `expect`).
- `guest/`: `kl_ndk.c` (ALooper, sensores, AInputQueue, AHardwareBuffer), drivers por motor (`kl_slink`, `kl_gles3jni`, `kl_native`, `kl_sdl2`, `kl_ue4`, `kl_jkxr`, `kl_driver`), `kl_nativeactivity.c` (`kl_na_low_memory` → `callbacks->onLowMemory`), `kl_android.c`, `kl_exe.c` (CTS).
- `xr/`: OVRPlugin `kl_ovrp*.c` (`_render/_input/_misc/_spatial`, thunks x8 `kl_ovrp_sret2.S`, `kl_ovrp_eyesize.h` funciones puras `kles_*`), OpenXR `kl_openxr*.c`, `kl_ovrplat.c` (OVRPlatform/IAP), `kl_spatial.c`, `kl_envdepth.c`.
- `gfx/`: `kl_vulkan.c` (puente a VPVulkan: trim, GPU lost, fin de frame, TAA/RCAS/máscara UI, marca del ojo `klvk_mark_eye_image` → `vpvk_mark_eye_image`), `kl_egl.c`/`kl_gles.c` (ruta vpgles; `kl_gles_mark_eye_texture`), `kl_reproject.c` (composición con RCAS fusionado).
- `media/` (mediandk con `VTPixelTransferSession` y contrapresión `klm_bp_*`, opensl con resampleo lineal, aaudio, camera), `diag/kl_sample.c`.
- `klepton/tools/klepton_ld.c`: pool de veneers medido sobre `site_lo/site_hi` (sitios x18), no sobre todo `.text`.

## App Swift (`app/Sources/`)
`VQLog` (sesiones archivadas en `Documents/Registros/sesiones/<fecha>_<juego>_<commit>__*`, conserva 8, comparte 6), `VQFolder`, `VQGames`, `VQPrepare` (`converterVersion`), `VQLaunch` (ajustes → `KL_*`/`VPVK_*`, sellos `firstrun.v<N>.<tag>` y `aot.<tag>.r<recetas>` con `translatorTag()`), `VQLauncher`, `VQCover` (carpeta → OculusDB → `vr_splash` → icono; reintento 1 h), `VQApiScan`, `VQConformance*`. `app/Native/`: `vq_apk.c`, `vq_config.c`, `vq_scan.c`.

## Visor (`klepton/visionos/Sources/`)
`KleptonCompositor` (compositor, RCAS/uiMask, GPU lost, backfill, blend por capa `blendPipeline(for:base:)`, medición por pase), `KleptonTAA` (VPKTAA, anillo de salida 4), `KleptonControllers` (mandos + CoreHaptics por mano), `KleptonChroma` (passthrough/inmersión), `KleptonSpatial`/`KleptonEnvDepth` (ARKit), `KleptonApp` (`Lifecycle.flushEngineCaches(then:)`), `kl_app.c` (heartbeat `[hb]`, symlink corto `<Documents>/.f` a Partidas para UE4 con ruta >96).

## Datos de plataforma
dylibs sin firmar en `QUEST/Cache/<j>/dylibs`, firmadas en `Library/Application Support/VisionQuestPro/Signed/<j>/`; logs en `QUEST/Registros/<fecha>-fin-<juego>` (+ `anteriores/`); centinela `Registros/sesion-en-curso.txt` + MetricKit; anclas `QUEST/Partidas/<juego>/anclas.json`; APIs Meta `QUEST/Cache/<juego>/meta-api.txt`.
- Nombre de target en la app = nombre de la carpeta ("Resident Evil 4"), no el corto. Lo keyed por `kl_driver_target_name()` ("olar","wrath2","twd2","cs1","hl2"…) NO se activa en la app: revisar esos usos (pendiente).

## ABI OVRPlugin verificada (no revertir)
- `klepton/tools/ovrp_abi.py`: compara `kl_ovrp*` con `libOVRPlugin.so` (9 versiones SDK 1.59–1.117). 2 diferencias vigentes, ambas falsos positivos (GetEyeFrustum HFA v0..v3; GetFloat en s0).
- x8/sret: GetNodePose/Velocity/Acceleration(2), GetTrackerPose, GetEyePose…, GetTrackingCalibratedOrigin (28 B Posef).
- GetEyeFrustum HFA4 {near,far,fovX,fovY}; GetControllerState3 = `ovrpResult f(mask,out64)`; State4/5/6 ponen a cero 96/100/108 B; GetTimeInSeconds = `ovrpResult f(double*)`.
- GetCaps/GetStatus `ovrpBool`; GetCaps2/GetStatus2 con máscara por valor (bit0 = running).
- GetEyeLayerRecommendedResolution(out): un arg; → -1004 sin capa de ojo viva.
- `ovrp_PollEvent` acota la escritura por versión SDK (`klovrp_pollevent_cap`, 4..3996 B).
- `ovrp_Get*Ptr/Pointer` y nombres con ProcAddr/Hook devuelven 0, nunca -1004.
- Teclado (4 funciones 1.81–1.104) → -1004; `ovrKeyValuePair` 40 B por valor/x8.
- `ovrpBlendFactor` Zero 0, One 1, SrcAlpha 2, OneMinusSrcAlpha 3, DstAlpha 4, OneMinusDstAlpha 5; sin flag de premultiplicado. Factores leídos (+0xb0/+0xb4/+0xb8) solo si `klovrp_quad_size_off == 0xbc`. `KL_OVRP_BLEND_FACTORS=0` lo apaga.
- Aviso: DWARF de `libOculusXRPlugin` da `ovrpLayerSubmitFlag_HeadLocked=1, Octilinear=2, ReverseZ=4, NoDepth=8, InverseAlpha=16`; `KLOVRP_SUBMIT_HEAD_LOCKED = 0x2` dice lo contrario. Sin contrastar.

## Resolución
- Slider = tamaño FINAL del ojo: 100 % = Quest 3 (2064x2208), 200 % = densidad Vision Pro (3660x3200), 50–200 %, tope 4096; render_scale = 1. `KL_OVRP_EYE_BASE=quest3`, `KL_OVRP_EYE_SCALE`.
- Ensanche del plugin aprendido en `klovrp_SetupLayer` (`g_widen_w/h`); OpenXR aprende la escala del guest (`g_xr_game_scale`).
- Ojos colapsados (Thrill 2: ambos ojos a 0,0 3552x2258 sin sub-rect): `klovrp_note_collapsed_rects()` en EndFrame4 → `g_unify_detected`; `klovrp_eye_tan()` da la unión a todos. `KL_OVRP_UNIFY_FRUSTUM=0/1`.
- OpenXR LOCAL = STAGE elevado por la altura medida de la cabeza (`klxr_local_origin_y`); `KL_XR_LOCAL_LEGACY=1` = LOCAL==STAGE.
