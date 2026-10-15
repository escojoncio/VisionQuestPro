# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-15; lo cerrado queda en una línea con referencia).

## EMPEZAR AQUÍ — 2026-10-15 (15): una sola pose por frame para TODAS las lecturas; registros al acabar el juego — SIN PROBAR
- Resultado de (14) (Registros 9/10, builds e56af9c/c29ae8e): PEOR al girar («los ojos ni se sincronizan»). El log lo
  mide: `frame record -> last Update(Render)` worst 2–7°, mean 0,7–1,2° girando, 22° en tirones = lo que el registro se
  separó de la pose dibujada → Unity dibuja con la PRIMERA lectura del frame (el display provider lee los ojos justo tras
  el primer Update), NO con la segunda. `KL_OVRP_POSE_LAST` def. 0 (variante «última» solo para A/B por env); ajuste
  «Pose del frame» eliminado de la app. HUD AoT: salió `overlays enqueued on thread X, EndFrame4 on Y` (arreglo activo;
  sin veredicto del usuario). AC: `[magenta] fragment module … returns the constant (1,0,1,1)` ×2 y 4 pipelines
  (renderPass 0x13d764e40, 2 samples) → Unity SÍ sustituye un shader por el de error; falta saber cuál/por qué.
- Cambio (15) `kl_ovrp.c`: el segundo Update(Render,F) ya no refresca `g_step_head` con una lectura nueva: reusa la
  pose fijada de F. Las lecturas Render sin frame fijado/vivo (frameIndex 0 del C#, otros hilos) responden la del
  frame más nuevo fijado (`g_fring_newest`) en vez de `g_step_head` fresco. BeginFrame4(F) sin Update previo fija F.
  Censo cada 300 frames: `[ovrp] Render reads over 300 frames: … answered head vs pinned pose worst X deg`
  (DEBE ser 0,00; si no, bug del latch). Si el temblor sigue con 0,00, las poses NO son la causa: mirar compositor
  (kl_reproject / anchor / cadencia) — la matemática de kl_reproject_build se revisó en papel y es correcta.
- Registros: `VQLog.saveSessionEnd` al cerrar el espacio inmersivo (`Lifecycle.immersiveEnded`): carpeta
  `QUEST/Registros/<fecha>-fin-<juego>`, en segundo plano (espera ≤20 s solo antes de `_exit`). La copia al reabrir
  (`savePreviousSessionToQuest`) queda para crashes: 12 reintentos ×5 s al arrancar, 1 intento al lanzar, se detiene
  si ya se lanzó un juego; motivo en app.txt si falla.
- Pendiente: shader magenta de AC (qué shader: registrar nombre/keywords del material? Unity no lo dice en Vulkan; opción:
  volcar las features que Unity consulta — `vkGetPhysicalDeviceFeatures/Properties` — y comparar con Adreno 740);
  saturación al iniciar AC; voces «Voice Starvation»; idioma AC (`Language set: English(US)` con es_ES).

## 2026-10-15 (14) — cerrado en (15)
- Pose «última» (revertida), overlays por hilo (`g_enq_tid`, `g_enq_idle_endframes`), detector magenta
  (`klvk_spirv_const_magenta_fs`), `KL_VK_FRAME_ASYNC` def. 0 (AC: `native_stuck` + aborto en VPKEngine).

## 2026-10-15 (13) — cerrado en (14)
- Fin de frame ASÍNCRONO (`KL_VK_FRAME_ASYNC`, def. 1; `frame end: ASYNC` en el log, funcionando); lectura del hilo de
  render (`KL_OVRP_RENDER_READ`, nunca dispara en AoT); VPKEngine `628c3b4` `VPVK_REPORT_GEOM=1` (no arregla AC).

## 2026-10-15 (9)–(12) — resumen (detalle en git)
- (12) anillo de poses por frameIndex `g_fring` (`KL_OVRP_FRAME_RING`); capturas de capas + fuentes (`vk_layerNN*.png`).
- (11) SpaceWarp siempre servido (`KL_VPKTAA_ASW`); QoS RT hilos guest; audio adaptativo; ajuste «Idioma del juego»
  (`vq.gameLang`); sonda botón PS (visionOS no expone el PS); shims libc varios.
- (10) x18: veneers `adr/ldr literal/add-sub sp` con x18, pools GCC como datos (`klx_lit_map`, converter kld-3);
  `ovrp_GetLayerTextureSpaceWarp` 7 args (depth en x5); VPKTAA off desde el arranque en juegos MSAA.
- (9) VPKEngine 935aa1b (CTS); mutex con tumbas; capa de conformidad OpenXR; AAsset con rutas absolutas (Wwise);
  shims libc *at; futex reescrito con `kl_futex_dump` en `[snap]`.
- 2026-10-14 (6): `.restorationBehavior(.disabled)` en la ventana principal (no arrancaba al restaurar escena).
  NO tocar el toolbar del launcher (ShareLink como en a1df14a).
- 2026-10-14 (4)/(7)/(8) y «Robustez del CTS» (device lost, vigilantes por caso, fences precisas, cola de retiro,
  `VQCompilerProbe`, registros a QUEST/Registros): ver `git log -p bitacora.md`.

## Conformidad (Vulkan CTS / OpenXR CTS como guest dentro de la app)
- CI `conformance.yml` (Linux, manual o `[conformance]`): VK-GL-CTS `vulkan-cts-1.4.6.2`, `DEQP_ANDROID_EXE=ON`, parche
  `conformance/vulkan/patch_deqp.py` (exporta `vq_exe_main`), listas `conformance/vulkan/caselists.py` (perfil
  `rapida` ~36k casos, CRC32 estable, tandas de 400; `completa` con `VQ_CTS_FULL=1`) → release `conformance`
  (`VulkanCTS.apk`, meta-data `com.visionquestpro.guest=exe`). OpenXR CTS 1.1.58 → release `conformance-openxr`.
- Klepton `KL_GUEST_EXE` (`runtime/guest/kl_exe.c`): main() en hilo guest de 64 MB por línea de `KL_EXE_ARGS_FILE`
  (TAB); `KL_EXE_BUDGET`/`KL_EXE_MEM_MB`/`KL_EXE_STOP_FILE`/`KL_EXE_PROGRESS`; recuperación por caso con sigsetjmp
  (`kl_fault_set_recovery`), reanuda en `<log>.r<k>.txt/.qpa`, máx. `KL_EXE_MAX_RECOVERIES`=50.
- App: `VQConformance.swift` (tandas desde `assets/caselists/index.txt`, resultados en `QUEST/Partidas/<test>/cts/`,
  caso abierto = Crash y se salta), `VQConformanceView.swift` (progreso, Parar; Ajustes > Conformidad). Args deqp:
  `--quiet` (no `--deqp-quiet`), `--deqp-log-flush=enable`. OpenXR: `KL_INTENT_EXTRAS` (graphicsPlugin=Vulkan2…).
- Escáner de APIs (`VQApiScan.swift`, `app/Native/vq_scan.c`): log «Escáner de APIs» por lanzamiento, caché
  `Cache/<j>/apiscan-raw.txt`.
- Sesiones: log 56 fesetround & co. (resuelto); log 57 aborto 1D comprimida (resuelto en VPKEngine) y `[imgfmt]`
  limitado; log 58: 6551 casos, 96,9 % de los soportados; log 59: bloqueo en draw (resuelto arriba, sin probar).
- Pendiente (por orden):
  1-2. HECHO: caso de draw identificado (OpArrayLength) y OpArrayLength implementado (VPKEngine `a0b39c4`).
  3. Fails conocidos (104): texel buffers sint/snorm/16-bit, fragment_shader_output int/snorm, blits 1D (1D como 2D),
     device_init (multiview/shader_draw_parameters → FEATURE_NOT_PRESENT si no se anuncian), format_properties
     (A1R5G5B5, A8B8G8R8_SNORM, R32G32B32 vertex), image_format_properties sampleCounts>1 sin color attachment,
     protected memory props, submit_wait_many_semaphores.
  4. NotSupported: implementar según «No soportados por motivo», con objetivo el perfil Adreno 740 (no todo).
  5. OpenXR CTS en el visor.

## Juegos: problemas abiertos (resto)
- Congelado tras `ShaderWarmupManager.WarmUpShaders` (AC, logs 53–55): `[futex]` en el `[snap]` para diagnosticar.
- Caché de shaders: `gpu.shader.lib hit` bajo frente a `gpu.pso.recipe loaded` → la caché MSL no acierta del todo.
- Juegos solo GLES bloqueados («solo Vulkan»); Boundary «no configurado»; VPKTAA inline sin probar en juego sin MSAA;
  tirones: precompilar todo antes de jugar.

## Resuelto (referencia; detalle en git)
- 2026-10-13: AA por juego (`KL_AA_MODE`, `VPVK_MSAA`, detección `vq.aaDetected.<key>`), VPKTAA inline sin
  `QueueWaitIdle` (core «pt-port v1»), vídeo sin surface (NV12 CPU), SDK Quest offline (Platform tipado, 485 `ovrp_*`,
  boundary virtual 3×3 m, OpenXR 105 funciones + 1.1), «Diagnóstico de ficheros» (`KL_TRACE_FS`).
- Logs 44–52: VPKTAA inline, QueueSubmit serializado (`g_qsub_mu`), vigilante `[snap]`, usuario offline Ubisoft,
  idiomas `*.lang`, frontera de overlays `ovrp_EndFrame4`, pose EyeLevel con altura medida.
- Logs 24–43: VPKTAA core (`VPKTAACore.swift`: profundidad, MV de SpaceWarp, desoclusión, YCoCg, RCAS), SpaceWarp
  0x300, compositor de overlays (rects, color scale, Equirect 180° SBS, HUD FPS, capturas), JNI/OpenSL/AAsset varios;
  rendimiento ~90 fps (log 26).
- Logs 8–22: Vulkan en Unity (spoof Adreno 740), TLS ELF, liblog, libdl, AAudio/OpenSL (audio OK log 17), SubpassData,
  swizzle, MTLBinaryArchive, poses/postura/recentrado, mandos Sense (PS = botones Meta), manos ocultas, idioma del
  sistema, foveado Quest siempre apagado.

## Arquitectura (referencia)
- Base Klepton (MIT, copia `ce74582` en `klepton/`, ver `THIRD-PARTY-NOTICES.md`): cada `.so` ARM64 se envuelve en un
  dylib firmado (`tools/klepton_ld.c`, `klepton_ld_run`) y se carga con `dlopen`; bionic/JNI/OVRPlugin/OpenXR propios.
  Sin traducción de CPU.
- Gráficos: VPKEngine `VPVulkan.framework` (`scripts/build-vpvulkan.sh`, caché por hash de `vpkengine/gpu/**`; la CI
  clona VPKEngine `main`) en lugar de MoltenVK (`VQSettings.useVPKEngine`, `KL_MVK_DYLIB`). Usa `gpu/metal2`.
- Firma: conversión sin firmar en `QUEST/Cache/<j>/dylibs`; firmadas en
  `Library/Application Support/VisionQuestPro/Signed/<j>/` (`VQPrepare`, `VQSigner`).
- App (`app/Sources/`): `VQLog`, `VQFolder`, `VQGames`, `VQPrepare`, `VQLaunch` (+ `VQSettings`), `VQLauncher`,
  `VQConformance*`; `app/Native/vq_apk.c` (ZIP + AXML), `vq_config.c` (reenvío `vpvk_config_set_*`).
- CI `visionos.yml`: solo `[build]` o manual; release `visionos` (`VisionQuestPro.ipa`, bundle `com.kdt.livecontainer`);
  log de CI en rama `ci-logs` (`visionos.txt`; `gh run view --log-failed` da 403 desde aquí).
