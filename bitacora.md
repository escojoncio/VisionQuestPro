# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-18; detalle de cada paso en los commits
«Integrar paso-aN» y en las ramas `paso-aN` de GitHub).

## EMPEZAR AQUÍ — 2026-10-18: pasos 1-7 implementados e integrados — primera build pendiente
Rama `integ` en VisionQuestPro y en VPKEngine (las dos con todo unido). Antes de la build: fusionar `integ` → `main`
en VPKEngine (la CI de esta app clona VPKEngine `main`) y luego aquí, commit con `[build]`.
Nada de hoy se ha compilado para visionOS: Swift, `.m/.mm`, MSL, `.S` y el C con cabeceras de Darwin solo están
revisados a mano (VPKEngine 203/203 tests en Linux; arneses C de Klepton en Linux OK). Esperar errores de compilación
en la primera build: leer el log de CI (rama `ci-logs`) y corregir antes de cualquier prueba.

### Qué cambia por DEFECTO (sin tocar ajustes)
- 100 % sin conexión para todos los juegos: `KL_NET_OFFLINE` def. 1 (kl_shim.c getaddrinfo/connect/sendto/sendmsg,
  kl_jni_android.c ConnectivityManager); solo loopback (127.x, ::1, localhost) y AF_UNIX. `KL_PLAT_TOKEN_ERROR` def. 1
  (ovr_User_GetAccessToken → error: AC no entra en UbiServices) y usuario Meta offline para todos (`klplat_user_default`=1).
- Sin trato por juego: `kl_is_ac_nexus` eliminado; muletas AC borradas (`KL_NEXUS_FRAME_BUDGET`, `KL_GUEST_FRESH_POSE`,
  2 etapas, ojo 3840, `KL_OVERLAY_SUBMIT_ORDER`); `KL_OVERLAY_TEXL` y 3 etapas para todos. Riesgo: memoria de AC (8,1 GB).
- Identidad Quest 3 coherente con `KL_QUEST3` (lo pone la app): OVRPlugin headset 11 «Meta Quest 3»
  (`KL_OVRP_QUEST3_ID` def. 1), OpenXR systemName «Meta Quest 3», JNI SDK 32. Mandos siguen como Touch de Quest 2.
- Compositor: ritmo de panel encajado a 120/100/96/90 Hz (±6 %) (`KL_CP_PANEL_SNAP=0` lo quita, `KL_CP_PANEL_HZ` fuerza).
- VPKEngine: perfil Adreno nuevo tras la puerta del CTS (`VPVK_EXTRA_EXTENSIONS=1`); juegos ven apiVersion 1.1 y
  shaderInt16/shaderDrawParameters/sync2/dynamic rendering/timeline a 0 (perfil anterior al 16/10). Diagnóstico de
  cuelgue: encoders >2048 draws partidos en trozos de 256 (`VPVK_HANG_SPLIT`), no si hay adjuntos memoryless;
  límite de bucles anidados (traductor v13, recetas se retraducen solas).
- Android: ~800 funciones nuevas de bionic/NDK (kl_bionic_extra*.c, kl_pthread.c, kl_android.c, mediandk, AAudio,
  OpenSL, JNI kl_jni_sysinfo.c); escáner de APIs mira la tabla real; sensor: solo giroscopio de cabeza real.
- Eye tracking: `ovrp_Start/StopEyeTracking` → Unsupported.

### Ajustes nuevos en la app (todos apagados por defecto)
- «VPKTAA nativo» (`vq.vpktaaNative` → `VPVK_TAA_NATIVE`): pase TAA dentro de VPKEngine, jitter Halton propio por
  viewport, Catmull-Rom, varianza YCoCg, desoclusión, RCAS; máscara de UI (R8 en slot 7, `VPVK_TAA_UI_MASK` def. 1):
  la UI/2D dentro del ojo no recibe jitter ni historia. Log `vk.taa native gpu_ms`, `vk.taa uimask draws_3d= draws_ui=`.
- «Shaders AOT (experimental)» (`vq.aot` → `VPVK_AOT=1`): compila todo en la preparación a MTLBinaryArchive; variantes
  nuevas en segundo plano sin bloquear (`VP_HOLD_MS=0`). Log `gpu.pso.aot archived= hot= hot_ms_total=`.
- «Caché de shaders en QUEST» (`vq.psoInQuest`): `QUEST/Cache/<j>/Pipelines`, sobrevive a reinstalar.
- «OpenGL ES con VPKEngine (experimental)» (`vq.glesVPK` → `KL_GLES_VPK=1`, exige VPKEngine + VPVulkan): GLSL ES
  propio → SPIR-V → traductor; capa GL/EGL en VPKEngine `gpu/gles` (vpgles_* en VPVulkan); puente `kl_gles.c`.
- Por juego: resolución (`vq.resMode/resBase/resScale.<aaKey>` → `KL_OVRP_EYE_BASE/SCALE`) y anisótropo
  (`vq.aniso.<aaKey>`: Global/Del juego/2-16x → `VPVK_FORCE_ANISO`).

### Puertas solo por variable (para probar a mano; candidatas a ajuste si funcionan)
- `KL_CP_PTW=1` reproyección posicional con profundidad por píxel (malla 96², `KL_CP_PTW_GRID`, `_DILATE`, `_FAR`);
  log `[cp] PTW gpu:`. Profundidad de ojo con `KL_VPKTAA=1` o `KL_CP_PTW=1`; MSAA resuelta (`KL_DEPTH_MS_RESOLVE` def. 1).
- `KL_CP_ASW=1` (exige PTW; anulado por el ajuste «ASW desactivado», activo por defecto): extrapolación con MV de
  SpaceWarp; signo fijo (`KL_CP_ASW_SIGN`), pendiente compartir el voto de signo de VPKTAA.
- `KL_OVRP_PRED_FRAMEIDX=1` (predicción por frameIndex + extrapolación de pose), `KL_OVRP_LATENCY=1`
  (GetAppLatencyTimings2), `KL_XR_STRICT_LAYERS=1` (solo lo enciende el CTS de OpenXR).

### Orden de pruebas en el visor (tras compilar)
1. AoT y AC con todo por defecto: que nada empeore (offline, 3 etapas, identidad Quest 3, perfil Vulkan reducido).
   AC: ¿sigue el cuelgue del warmup? Leer `gpu.hang draw …`, `chunks_done`, `gpu-hang-*.metal` en QUEST/Registros.
   Si AC usa MSAA memoryless no se parte: repetir con MSAA desactivado.
2. Vulkan CTS (perfil rápida) con `VPVK_EXTRA_EXTENSIONS=1`: grupos que validan lo tras la puerta —
   int16 `spirv_assembly.*int16*`, `*16bit_storage*`, `draw.shader_draw_parameters.*`, vertex stores
   `robustness.* binding_model.* memory_model.*`, `api.private_data.*`, `memory.requirements.*`, `*imageless*`,
   `subgroups.size_control.*`, `synchronization2.*`, `dynamic_rendering.*`, `synchronization.timeline_semaphore.*`,
   `ycbcr.*`, `api.info.*`, `api.image_format_properties`. Abrir la puerta a juegos por grupos limpios.
3. VPKTAA nativo en AoT (sin MSAA): nitidez, `draws_ui`, ms GPU. Luego PTW y ASW.
4. AOT + caché en QUEST: segunda partida sin `gpu.pso.hot`.
5. OpenXR CTS 1.1.58. 6. Un juego GLES con el ajuste GLES. 7. dEQP-GLES (listas en `conformance/gles/caselists.py`;
   falta compilar gles2/3/31 en `conformance.yml` y el runner EGL).

### Pendiente conocido
- Rosa de AC: causa sin identificar (no son vertex stores). Comparar lo que Unity consulta con el perfil Adreno real y
  `[prov] layer 3 … no write seen`. Falta vulkaninfo JSON real de Quest 3 (vulkan.gpuinfo.org, descarga manual).
- Flags SpaceWarp y orden de LatencyTimings sin contrastar con `OVR_Plugin_Types.h` (no accesible): usar el
  `libOVRPlugin.so` de un APK del usuario (desensamblar).
- VPKEngine: salida de fragmento int/snorm y blits 1D sin auditar; ycbcr sin 10/16 bit ni formato externo
  (AHardwareBuffer); no viable en Metal: variablePointers completo, LogicOp/PatchControlPoints dinámicos,
  availability/visibility, BDA capture/replay, inline uniform update-after-bind. EDS × máscara de UI no combinan.
- GLES: sin transform feedback, SSBO/imágenes/atómicos, geometry/tess, program binary; occlusion query = 1;
  gl_PointCoord fijo; window surfaces planas sin conectar; DestroyContext con contexto actual en otro hilo (UAF);
  MakeCurrent permite el mismo contexto en dos hilos; GLboolean sin extensión garantizada (teórico);
  `kl_gles_sym` NULL cae a la pasarela GL vieja (estados mezclados).
- Android: AObb sin montaje real; AImage solo YUV_420_888; OpenSL sin efectos/3D/MIDI; posix_spawn/exec/SysV ENOSYS;
  inotify sin IN_ACCESS/OPEN/CLOSE. Sin validar en Darwin: kl_bionic_extra2/4.c (campos internos de FILE), thunks `.S`.
- Menores VPKEngine: jitter consume fase aunque falle el encode; handler de taa_native_metal capturando `Impl*` al
  cerrar; `vkCmdSet{Viewport,Scissor}WithCount` ignoran el contador.
- Después de los pasos: SSAO propio en VPKEngine (media resolución, profundidad compartida con VPKTAA, toggle por
  juego desactivado por defecto; origen: AoT no trae el SSAO de URP).

## Conformidad (Vulkan CTS / OpenXR CTS como guest dentro de la app)
- CI `conformance.yml` (Linux, manual o `[conformance]`): VK-GL-CTS `vulkan-cts-1.4.6.2`, `DEQP_ANDROID_EXE=ON`, parche
  `conformance/vulkan/patch_deqp.py` (exporta `vq_exe_main`), listas `conformance/vulkan/caselists.py` (perfil
  `rapida` ~36k casos, tandas de 400; `completa` con `VQ_CTS_FULL=1`) → release `conformance` (`VulkanCTS.apk`).
  OpenXR CTS 1.1.58 → release `conformance-openxr`.
- Klepton `KL_GUEST_EXE` (`runtime/guest/kl_exe.c`): main() en hilo guest por línea de `KL_EXE_ARGS_FILE`; recuperación
  por caso con sigsetjmp, máx. `KL_EXE_MAX_RECOVERIES`=50.
- App: `VQConformance.swift` (resultados en `QUEST/Partidas/<test>/cts/`), Ajustes > Conformidad. Args deqp:
  `--quiet`, `--deqp-log-flush=enable`. OpenXR: `KL_INTENT_EXTRAS`; `prepareOpenXR` pone `KL_XR_STRICT_LAYERS=1`.
- Última referencia: log 58, 6551 casos, 96,9 % de los soportados (antes del perfil 1.3).

## Resuelto (referencia; detalle en git)
- 2026-10-15/16: temblor al girar (etapa publicada tras la GPU, `KL_OVRP_PUBLISH_AFTER_GPU`), clock_getres Linux,
  AMediaDataSource, panel 50→100 Hz, una pose por frame, registros al cerrar (`QUEST/Registros/<fecha>-fin-<juego>`),
  carátulas (`VQCover.swift`), `[prov]` procedencia de capas.
- 2026-10-13 y anteriores: AA por juego, VPKTAA inline, SDK Quest offline (485 `ovrp_*`, OpenXR 1.1), Vulkan en Unity
  con spoof Adreno 740, audio, mandos Sense (PS = botones Meta), foveado Quest siempre apagado.

## Arquitectura (referencia)
- Base Klepton (MIT, copia `ce74582` en `klepton/`, ver `THIRD-PARTY-NOTICES.md`): cada `.so` ARM64 se envuelve en un
  dylib firmado y se carga con `dlopen`; bionic/JNI/OVRPlugin/OpenXR propios. Sin traducción de CPU. Fuentes del
  runtime: `klepton/Makefile` (`RUNTIME_SHIP`, `RUNTIME_JNI`) → `libklepton.a`; Xcode no lista fuentes sueltas.
- Gráficos: VPKEngine `VPVulkan.framework` (`scripts/build-vpvulkan.sh`, caché por hash de `vpkengine/gpu/**`; la CI
  clona VPKEngine `main`), incluye `gpu/gles` (vpgles). Usa `gpu/metal2`.
- Firma: dylibs sin firmar en `QUEST/Cache/<j>/dylibs`; firmadas en `Library/Application Support/VisionQuestPro/Signed/<j>/`.
- App (`app/Sources/`): `VQLog`, `VQFolder`, `VQGames`, `VQPrepare`, `VQLaunch` (+ `VQSettings`, `VQGameRes`,
  `VQGameAniso`), `VQLauncher`, `VQConformance*`, `VQApiScan`, `VQCover`; `app/Native/vq_apk.c`, `vq_config.c`, `vq_scan.c`.
- CI `visionos.yml`: solo `[build]` o manual; release `visionos` (`VisionQuestPro.ipa`); log de CI en rama `ci-logs`
  (`visionos.txt`; `gh run view --log-failed` da 403 desde aquí).
