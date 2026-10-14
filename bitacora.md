# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-14; lo cerrado queda en una línea con referencia).

## EMPEZAR AQUÍ — 2026-10-14 noche
- Vulkan CTS rapida COMPLETO (log 61, 36087/36087): 81,0 % de lo soportado; 1513 Fail, 7 Crash, 28101 NotSupported.
  Fallos por subgrupo: renderpasses ~500 (formats.* clear/load/input attachments), pipeline.stencil 136,
  copy_and_blit 90, image_clearing 57, pipeline.image/blend/sampler ~95, draw.basic/multiple_clears ~50,
  glsl.texture_functions offsets/query ~110, image.mutable 51.
- Build con VPKEngine `b71ed3f` (sin probar): copias depth/stencil por aspecto, clears en quads (rects, capas,
  aspectos, área parcial, load ops depth≠stencil), clear colors por formato entero/float.
- OpenXR CTS (log 61): 0 tests en 2 s porque no existía `/sdcard/files` (no podía abrir `openxr_cts.xml`). Arreglo:
  `kl_guest_mkdirs()` (kl_native.c, declarado en klepton.h) crea `/sdcard/files` antes de onCreate y en
  getFilesDir/getExternalFilesDir. Progreso visible: `kl_openxr_status_log/_text` (kl_openxr.c) cuenta secciones
  (`--durations yes`), aserciones `FAILED:`, frames y el resumen final; liblog lo alimenta (kl_va_handlers.c,
  kl_shim.c) y el compositor lo pinta en el HUD con `KL_STATUS_HUD=1` (KleptonCompositor.swift; la app lo pone solo
  para el OpenXR CTS).
- Siguiente (orden): extensiones (timeline semaphores, synchronization2, dynamic_rendering anunciada,
  depth_stencil_resolve, sampler_mirror_clamp_to_edge, copy_commands2, vertexPipelineStoresAndAtomics, multi_draw),
  D24 depth en copias, fallos de glsl texture offsets/query, stencil/blend/sampler. Para causas reales hacen falta
  los .qpa (`QUEST/Partidas/Vulkan CTS (Khronos)/cts`).

## Robustez del CTS (2026-10-14, motivo: log 59 bloqueado en `048-draw-001` tras `Caused_GPU_Hang_Error`)
- VPKEngine (`gpu/metal2`, `gpu/vulkan`; detalle en su bitácora 2026-10-14 (2)): ninguna espera a la GPU es infinita.
  Dispositivo perdido (`gpu.err what=device_lost`) con revoked/ignored/removed siempre, cualquier fallo no-memoryless
  si `VPVK_DEVICE_LOST_STRICT=1`, o 20 s esperando sin compleciones (pausas de la app excluidas). Perdido ⇒ sin trabajo
  nuevo, esperas/submit = `VK_ERROR_DEVICE_LOST`, backend filtrado si quedan CB colgados (`what=device_lost_leak`).
- Fences precisas (antes vaciaban toda la GPU): `Fence::frames` + `waitFrames`; timeout respetado (0 = sondeo).
  Afecta a juegos (menos serialización CPU/GPU; vigilar regresiones). `klvk_shadow_pump` ya sondea de verdad.
- `VPVK_PREWARM=0/1` por entorno (la llamada `vpvk_config_set_prewarm` llegaba tarde: el CTS precalentaba ~1700
  pipelines por vkCreateDevice). App: según «Precompilar shaders»; CTS = 0 y sin `VQPrewarm.run`.
- Klepton `kl_vulkan.c`: envoltorios guest WaitForFences/GetFenceStatus/DeviceWaitIdle/QueueWaitIdle/QueueSubmit →
  `klvk_lost`; con `KL_GUEST_KIND=exe` salta al punto de recuperación (`KL_RECOVER_DEVICE_LOST`=1001, kl_fault.h).
- Klepton `kl_exe.c`: caso abandonado = `DeviceLost` | `Timeout` | `Crash`. Vigilante por caso
  (`KL_EXE_CASE_TIMEOUT_S`=240 s sin escribir el .qpa) → SIGABRT al hilo del test (backtrace en el informe) → Timeout;
  sin desenrollar en 60 s → `_exit(0)`; main() tras recuperación sin escribir log en 240 s → `_exit(0)`; huecos >5 s
  del bucle (app suspendida) no cuentan. No se usa `--deqp-watchdog` (30 s/iteración, aborta desde otro hilo).
- App `VQConformance.swift`: resumen con colgados/perdidos y «No soportados por motivo» (top 40, números → N, se
  quita solo la ubicación `at x.cpp:N`). `VPVK_DEVICE_LOST_STRICT=1` solo en el CTS (`unsetenv` para juegos).
- Riesgo: cada DeviceLost filtra su VkDevice (cientos de MB); lo para el límite de memoria entre tandas.

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

## Juegos: problemas abiertos (AC Nexus / AoT; última prueba logs 53–55, builds `de2bf58` + VPKEngine `166f0b1`)
- Rosa = capas OVROverlay, no el ojo (log 55, capturas): `eye0/1` negros; 26/33 capas 255,0,255. Las rosas son las que
  el juego rellena con `vkCmdCopyImage` desde textura (fmt 43/37); las de RT dibujado = negro. Siguiente: loguear el
  src de esos `vkCmdCopyImage` (`klvk_CmdCopyImage`) y cómo se rellenó (CopyBufferToImage vs pase).
- Congelado: serial del compositor parado (624–632) tras `ShaderWarmupManager.WarmUpShaders` (~1700 pipelines).
  `[snap]`: los 24 hilos guest en `kl_futex_impl` ← `libunity.so+0xb8fe50` (FUTEX_WAIT con timeout relativo);
  nadie en vk. Hipótesis: (1) despertar perdido en `kl_futex_impl`; (2) hilo que debía señalar murió;
  (3) respuesta de Platform/AssetFile que no llega. Siguiente: leer `kl_futex_impl` y añadir al `[snap]` contador por
  dirección (addr, val, waiters, último WAKE). El error memoryless del log 53 NO era la causa (fue al ir a segundo plano).
- Caché de shaders: `gpu.shader.lib hit=0 miss=3488` con `gpu.pso.recipe loaded pipelines=3169` → la caché MSL no acierta.
- Audio AC (Wwise 2019.2): bancos en `Master.pck` + `en.lang` (no hay `main.*.obb`). Wwise abre con
  `AAssetManager_open` y RUTA ABSOLUTA (`/sdcard/Android/obb/…/Init.bnk` MISS). Arreglo genérico pendiente en
  `kl_AAssetManager_open`/`openDir` (`klepton/runtime/guest/kl_ndk.c`): si empieza por '/', `kl_guest_path` y abrir ese
  fichero. Probable causa también de «Package path couldn't be set» (C# de Ubisoft).
- Glitch del visor (log 55): footprint 3,5→4,0 GB mientras se guardaban capturas con el juego congelado.
- libc sin shim (genérico): fstatat(64), faccessat, open64, openat64, __openat_2, freopen, readlink(at), mkdirat,
  rmdir, unlinkat, renameat, statfs64, statvfs64, truncate, chmod, utime/utimes/utimensat, nftw/ftw, glob, creat,
  remove, pathconf, __realpath_chk, __getcwd_chk, fchdir. Con `kl_guest_path` + conversión de structs y de constantes
  *at (AT_FDCWD −100/−2; AT_SYMLINK_NOFOLLOW 0x100/0x20; AT_REMOVEDIR 0x200/0x80; AT_EACCESS 0x200/0x10).
- Otros: juegos solo GLES bloqueados («solo Vulkan»); aborto final `mutex lock failed: Invalid argument` (log 22);
  Boundary «no configurado»; VPKTAA inline sin probar en un juego sin MSAA; tirones: precompilar todo antes de jugar.

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
