# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-15; lo cerrado queda en una línea con referencia).

## SIGUIENTE PASO OBLIGATORIO — emulación perfecta de Quest 3 (antes de cualquier otra tarea)
Objetivo: que ningún juego note diferencia con un Quest 3. Hacerlo en este orden:
1. **GPU idéntica a la Adreno 740 del Quest 3.** VPKEngine debe declarar exactamente lo que declara la Adreno
   (features, límites, extensiones, formatos) y cumplirlo. Herramienta: Vulkan CTS dentro de la app hasta 0 fallos en
   lo que la Adreno soporta (base: Pass 8 567 / Fail 898 / NotSupported 26 622, perfil rápida). Incluye MSAA
   transitorio `memoryless` en VPKEngine (como la memoria en chip del Adreno: sin RAM) y, hecho eso, volver a 4x en AC.
2. **Runtime del Quest completo (OVRPlugin/OpenXR).** OpenXR CTS pasando en el visor; tiempos (predicted display time),
   poses, capas y SpaceWarp como el sistema real.
3. **Android completo.** Todas las APIs de sistema/NDK que piden los juegos: usar el «Escáner de APIs» por juego
   (`VQApiScan.swift`) y cerrar lo que marque SIN IMPLEMENTAR (p. ej. AMediaDataSource_setGetAvailableSize/newUri).
4. **Compositor.** Ritmo de panel fijo (100 Hz) y reproyección con profundidad por píxel (no solo giro).
5. **Rendimiento.** Shaders precompilados sin tirones y paso a la traducción anticipada (AOT) de VPKEngine.
6. **OpenGL ES** (tras el paso 1, con el CTS de Vulkan a 0 fallos): capa GLES/EGL propia encima del Vulkan de VPKEngine
   (`libGLESv2/v3` + `libEGL` del guest → llamadas Vulkan; GLSL ES → SPIR-V → traductor existente), código 100 %
   propio, sin ANGLE. Así los juegos GLES heredan perfil Adreno, MSAA, VPKTAA, foveado y caché de shaders. Validar con
   el CTS de GLES (dEQP-GLES2/3/31, ya incluido en VK-GL-CTS). Quitar el bloqueo «solo Vulkan» del launcher al terminar.
7. **Limpieza de muletas de AC** (al cerrar pasos 2 y 4): ver entrada (20).
Todos estos pasos son OBLIGATORIOS y van en este orden.

## DESPUÉS de los pasos obligatorios — SSAO propio en VPKEngine
- Pase de oclusión ambiental en pantalla propio, a partir del buffer de profundidad del juego (el mismo que captura
  VPKTAA), aplicado a la imagen antes de enviarla al compositor. Genérico para todos los juegos.
- **Toggle en Ajustes** (por juego, desactivado por defecto): un juego que ya trae su propio SSAO/AO no debe recibirlo
  dos veces. Opcional: detectar AO propio del juego (p. ej. renderer feature SSAO de URP / pases AO conocidos de Unreal)
  y avisar en la tarjeta del juego.
- Objetivo: el menor coste en ms posible (VR estéreo, 2 ojos a alta resolución). Pautas: media (o cuarto de)
  resolución, profundidad reconstruida a lineal una sola vez y compartida con VPKTAA, pocas muestras + ruido azul
  y acumulación temporal reutilizando la historia/vectores de VPKTAA, desenfoque bilateral separable por profundidad,
  upsample guiado por profundidad, ambos ojos en un único dispatch (array de capas), solo dentro de la zona nítida del
  foveado (periferia barata o sin AO). Medir ms GPU por pase en el log.
- Origen: Attack on Titan VR: Unbreakable (URP 2021.3) no trae el shader SSAO de URP en la build, así que no se puede
  activar editando sus assets.
Lo que hace falta del usuario: logs de juegos variados (cada uno destapa huecos distintos); tandas periódicas del
Vulkan CTS desde la app; el informe de capacidades (vulkaninfo) de un Quest 3 real para comparar con datos exactos
(vulkan.gpuinfo.org bloquea la descarga automática).

## EMPEZAR AQUÍ — 2026-10-16 (20): perfil Adreno bloque 2 + AC a 4x — SIN BUILD (pendiente visto bueno)
- VPKEngine (push): `1495086` sin BC, ASTC no cuadrados (`FormatInfo::blockHeight`), R4G4B4A4=ABGR4, sin
  EXT_metal_objects visible; `1e6d988` shaderInt16 (16 bits en registros de 32), shaderDrawParameters (buffer 29
  `VpDrawParams`), maintenance2/3, storage_buffer_storage_class, driver_properties QUALCOMM, Vulkan11/12/13 structs,
  extensiones 1.2/1.3 fuera de VPVK_EXTRA_EXTENSIONS (menos EXT_multi_draw); `kTranslatorVersion`=8 (retraduce
  recetas); `f836aeb` host_query_reset; `1e483b2` arreglos de revisión (baseVertex no indexado, spec int16, ext16).
- NO declarado (el traductor no lo cumple): samplerYcbcrConversion, variablePointers, 16/8-bit storage, apiVersion 1.3
  (faltan maintenance4, BDA, memory model…), B4G4R4A4. MSL nuevo sin compilar en Apple: vigilar errores de pipeline.
- VisionQuestPro: AC vuelve a MSAA 4x (`KL_VK_MAX_SAMPLES` def. 0 para todos); ojo 3840 de AC solo si se pide 2x
  explícito (kl_ovrp.c). Vigilar memoria en AC.
- Parches solo-AC (`kl_is_ac_nexus()`) a revisar al cerrar pasos 2 y 4: probar AC con cada uno apagado y quitar los
  que ya no hagan falta. Muletas de tiempos/compositor: `KL_NEXUS_FRAME_BUDGET`, `KL_GUEST_FRESH_POSE`, 2 etapas
  (`kl_ovrp_stage_count`), `KL_OVERLAY_TEXL`=0, `KL_OVERLAY_SUBMIT_ORDER`. Se quedan (no son muletas): token offline
  `KL_PLAT_TOKEN_ERROR`, burst Wwise. Arreglos globales (clock_getres, publicar tras GPU…) son emulación correcta.

## 2026-10-15 (19): paso 1 (GPU = Adreno 740) iniciado
- VPKEngine, 4 commits locales (pendientes de push): `ced6226` memoria LAZILY_ALLOCATED sin RAM (tipo 3 sin host,
  `DeviceMemory::lazy`, commitment 0; la ruta MSAA memoryless/MultisampleResolve ya existía en metal2); `ad18998`
  STORAGE_IMAGE_ATOMIC/STORAGE_TEXEL_BUFFER_ATOMIC en R32_UINT/SINT (`MTLTextureUsageShaderAtomic`, sin probar en
  visionOS); `e626ce4` bitácora VPKEngine; `5099bb3` driverVersion 512.744.0 (bit 31), `textureCompressionBC=FALSE`,
  `vertexPipelineStoresAndAtomics=TRUE` fijo (entry.cpp:324, ya sin la puerta del CTS).
- Revisión adversarial: sin bloqueantes. Pendiente: test de store/atomic en VS; BC1/3/7 siguen anunciados en
  formatFeatures (physical_device.cpp:208-241) → quitar SAMPLED_IMAGE para forzar ruta ETC2/ASTC como el Adreno
  (rompe test vk_tests.cpp:405, ajustarlo).
- Fallos CTS de la lista: la mayoría ya resueltos en (8)/(12) según arneses en Linux; confirmar con CTS nuevo en el
  visor. Queda B4G4R4A4_UNORM_PACK16 (obligatorio; Metal solo ABGR4 → swizzle).
- Perfil Adreno comparado: `docs/perfil-adreno740.md` (copiado de progreso). Siguientes P0: añadir lo que obliga
  VP_ANDROID_baseline_2022 (shaderInt16, shaderDrawParameters, KHR_maintenance2/3, storage_buffer_storage_class,
  samplerYcbcrConversion, variablePointers), quitar `EXT_metal_objects` del listado al guest, apiVersion 1.3 con
  structs VULKAN_1_1/1_2/1_3_FEATURES, ASTC no cuadrado, centralizar en VPKEngine y dejar `klvk_patch_features` no-op.
  Falta vulkaninfo real de un Quest 3 para cerrar valores [B]/[C].
- Volver a MSAA 4x en AC (`KL_VK_MAX_SAMPLES`) ahora que la MSAA transitoria no gasta RAM.

## 2026-10-15 (18): crash AMediaDataSource, panel 50→100 Hz, vertex SSBO (rosa), token offline — build 18
- Resultado de (17) (build 505b8dc, AC 21:24–21:32, primera vez que el juego arranca): 0 `Scheduler::dispatch` (reloj
  OK); `[nexus] SpaceWarp spin 18.89 ms -> 0`; memoria 3,9 GB; rosa sigue (cover quad + menús); fin por
  `[klepton] fatal: called unresolved import 'AMediaDataSource_new'` (VideoPlayer con fuente propia).
- `kl_mediandk.c`: AMediaDataSource_* + `AMediaExtractor_setDataSourceCustom` (vuelca la fuente a
  TMPDIR/klepton-media-ds-*.mp4 y abre con kl_avdemux; `spooled` → unlink al borrar; close() del guest al borrar).
- Display: `display rate SETTLED at 50.028 Hz` en panel de 100 Hz (frames de arranque mostrados 2 veces) → al juego
  se le dijo 50 Hz. `KleptonCompositor.swift`: `unheldPanelPeriod` en el latch de arranque (<55 Hz y x2/x3 de
  90/96/100/120 → canónico; `KL_CP_UNHOLD_RATE`), y re-latch hacia arriba en `cadenceSummary` solo desde <85 Hz,
  ≥25 % de huecos cortos (`cadenceShort`) y 6 ventanas (~3 s; la ventana es de 0,5 s).
- Rosa: VPKEngine reporta `vertexPipelineStoresAndAtomics=0` (atado a VPVK_EXTRA_EXTENSIONS, entry.cpp:325); Adreno y
  MoltenVK reportan 1 → Unity descarta subshaders con StructuredBuffer en vértice → shader de error.
  `kl_vulkan.c` `klvk_patch_features` (Features y Features2): 1 para todos (`KL_VK_VTX_SSBO`, def. 1);
  `KL_VK_IMG_NOFMT=1` (def. 0) reporta también storage image sin formato. Log: `features to the guest: …`.
  Escrituras de vértice en el traductor sin validar (CTS) — vigilar `vk.err`.
- UbiServices offline ~50 s (reintentos DNS de login con el token Oculus): `kl_ovrplat.c` `ovr_User_GetAccessToken`
  → completion de ERROR para AC (`KL_PLAT_TOKEN_ERROR`, latch solo con paquete conocido).
- Log `[cp] thermal state: nominal|fair|SERIOUS|CRITICAL` (al cambiar y cada 30 s), KleptonCompositor.swift.
- Pendiente: MSAA memoryless en VPKEngine (MSAA transitorio sin RAM, como en Adreno) y volver a 4x en AC; paneles AC
  girados 180° (`KL_OVERLAY_FLIP_X/Y`); idioma voces AC (menú del juego).

## 2026-10-15 (16)-(17): etapa publicada tras la GPU (temblor al girar), clock_getres Linux (AC) — build 17
- Resultado de (15) (Registros 11/12, build 8f080bf, AoT 20:21 a 100 Hz): censo `answered head vs pinned pose worst
  0,06–0,07°` → las poses YA son una por frame; el temblor seguía → la causa NO eran las poses. Compositor sano
  (cadencia 10,00 ms). Registros `fin-<juego>` funcionan.
- CAUSA del temblor (también visto en Arkham Shadow, «flicker when I
  look around»): `klovrp_end_frame_impl` publicaba `last_complete` ANTES de que `kl_vulkan_frame_done` esperara a
  la GPU (~12 ms en AoT = un periodo entero) → el compositor reproyectaba con la pose del frame N una textura que
  aún tenía (parcial o totalmente) el frame N-3. Arreglo `kl_ovrp.c`: `t_defer_complete` (EndFrame4, guest Vulkan,
  `KL_OVRP_PUBLISH_AFTER_GPU`, def. 1) → `deferred_complete` + `r[stage].complete=0`; `klovrp_publish_deferred()` tras
  `kl_vulkan_frame_done` (y al final de EndFrame4). Escaneos de VPKTAA en EndFrame4 aceptan la etapa diferida. Log
  cada 600: `[ovrp] frame end waited for the GPU X ms mean … stage published AFTER the wait`.
- AC ralentizaciones: 4464× `Scheduler::dispatch (1255 ms)` en 30 s. Causa: UbiServices usa
  1e9/clock_getres(MONOTONIC); Darwin da 1000 ns → reloj 1000x rápido. `klb_clock_getres` (kl_libc.c) responde 1 ns
  para ids 0/1/4/7 a TODOS los juegos (`KL_CLOCK_RES_LINUX`, def. 1).
- AC magenta: `[prov] layer 3 source …: no write seen` (el quad de portada se copia de una textura que nada escribió).
  Probado y DESCARTADO limpiar a cero (la tabla de procedencia es un anillo con pérdidas y no cubre resolve/compute/
  dynamic rendering → borraría contenido legítimo). Queda solo el diagnóstico `[prov]`.
- (17) Arreglos de AC — gate AC = `kl_is_ac_nexus()` (kl_jni_services.c, por
  paquete com.Ubisoft.ACNexusVR; `klj_guest_package` ahora con mutex y sin fijar el fallback antes de tener manifest):
  - AC: `KL_NEXUS_FRAME_BUDGET` (kl_mprobe.c, desde `kl_driver_frame`: pone a 0 `_nexusFrameTicksSpaceWarp` de
    NexusUtils.PhaseSyncHack vía il2cpp; valores 150000–260000), `KL_GUEST_FRESH_POSE` (kl_app.c), `KL_VK_MAX_SAMPLES`=2
    (límites + sampleCounts por formato), 2 etapas de ojo (`kl_ovrp_stage_count`), tope de ojo 3840 si MSAA≤2 y 2 etapas,
    `KL_OVERLAY_TEXL`=0, `KL_OVERLAY_SUBMIT_ORDER` (Swift; escribe profundidad solo si writeDepth).
  - Todos: `KL_OVRP_ENQ_OWNS_OVERLAYS` (EndFrame4 vacío no borra overlays encolados; contador solo con capas no-ojo),
    AAudio burst 256 (Wwise 1024: AC o libAkSoundEngine.so; `KL_AAUDIO_WWISE_BURST`), `KL_FS_REL_SDCARD` (rutas
    relativas "sdcard/…" como absolutas mientras el guest no haga chdir).
  - Ya equivalentes: idiomas (.lang listados; la voz en español se elige en el menú del juego), SpaceWarp, texptr,
    soft null-class, injectActivityJava, red offline, vídeo de carga.
  - Fork, no portado: `klovrp_enq_age` (ruta EndFrame legada), usuarios offline por app id de otras tiendas,
    `KL_OVERLAY_FLIP_X/Y` (los paneles de AC podrían salir girados 180°; comprobar).

## 2026-10-15 (15): una sola pose por frame para TODAS las lecturas; registros al acabar el juego — SIN PROBAR
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
- AC magenta, dato nuevo (log 12:42): la capa 3 (quad de portada, head-locked 1024²) ya era 100 % magenta a las
  12:42:06 y el PRIMER módulo de error de Unity se creó a las 12:42:19 → ese magenta NO viene del shader de error.
  Añadido `[prov]` (kl_vulkan.c, `klvk_prov_*`): procedencia de la imagen fuente de cada capa (último escritor:
  render pass + pipelines + fs MAGENTA/no, upload, clear [MAGENTA si (1,0,1)], blit, copia). Se imprime en el
  `written by the guest` de cada capa. Siguiente: leer `[prov] layer N source …` en el log de AC.
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
