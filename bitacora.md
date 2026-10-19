# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md`. Este fichero es el estado vigente (compactado el 2026-10-19).

## 1. Estado actual (2026-10-19)

**Proyecto**: app visionOS que ejecuta juegos de Quest (APK + datos descomprimidos en `QUEST/Juegos`) sin Quest. Base Klepton (MIT, copia `ce74582` en `klepton/`): cada `.so` ARM64 se envuelve en un dylib firmado y se carga con `dlopen`; bionic, JNI, NDK, OVRPlugin y OpenXR son propios. Sin traducción de CPU. Gráficos: VPKEngine (`VPVulkan.framework`, Vulkan/SPIR-V → Metal; incluye `gpu/gles` = vpgles) y VPKTAA nativo. Solo offline.

**Última build exitosa**: run CI 39310806744 en verde con Gate (commit b83ac15 sobre a8b07fc). IPA: release `visionos` del repo, asset `VisionQuestPro.ipa`

**Compila y pasa tests (CI)**: `-fsyntax-only` de todo `RUNTIME_SHIP` con SDK xros, tests host (VPKEngine `gpu/` cmake+ctest, `gpu/metal2` para xros, `pred_frame_offset_test`, `t_ovrp_input`, `spatial_test`, `t_ptw` con ASan/UBSan), `libklepton.a`, app Swift (BUILD SUCCEEDED).
**NO probado en el visor: TODO el último lote** (sección 3): memoria/trim, RCAS fusionado + máscara UI, háptica, ALooper, vídeo (SurfaceTexture), sensores, AInputQueue, UE4/Settings, anillo TAA de 4 slots, tamaño de ojo guardado, GPU lost, `[cp] gpu`. Lo que sí se probó en visor es anterior a este lote.

### Estado por juego (último problema conocido → qué se hizo)
| Juego | Último problema | Hecho / pendiente |
|---|---|---|
| AC Nexus (Unity/Vulkan, Wwise) | Jetsam a ~6,5–8,2 GiB en la carga síncrona (pico de memoria gráfica sin atribuir); bajones; flashes/basura ~1 s en carga; `OVRInput connected=0x0` toda la sesión (no bloquea; pulsaciones llegan por otra vía); cuadrados rosas (causa sin identificar; `[magenta]` 2 pipelines del warmup de Unity no lo prueban); corte del inmersivo en ShaderWarmup (3392 shaders) | Audio arreglado (burst 1024 con Wwise vía `kl_find_image`); zero-init perezoso + ojo retenido; MSL reutilizado y corte cada 256 draws; límite de memoria 8192 MiB y avisos 65/80 %; caché de 512 archivos de PSO; quads en orden de envío (calibración de mandos visible). Verificar en visor |
| AoT (Unity/Vulkan, IL2CPP) | Bajones: GPU al límite con MSAA x2 a 2976x1912 → visionOS fija 50 Hz (`HALF RATE`); rosa/1 s en tutoriales 2D; crash si `nativeLowMemory` se entrega fuera del hilo del juego (IL2CPP sin x18) | Aviso de memoria solo en el hilo del juego; `KL_CP_HALFRATE_RECOVER`; instrumentación `gpu.sec`/`[cp] gpu`. Verificar framerate |
| Demeo (Unity OpenXR + GLES/vpgles) | Atascado en el vídeo de logos | Arreglos: hook OpenXR (`UnityOpenXR_HookGetInstanceProcAddr` pass-through; antes SIGKILL CODESIGNING por -1004), ruta de imagen vpgles en OpenXR, APIs por manifiesto + cierre limpio. Vídeo: SurfaceTexture real drenado en el hilo del juego + wake de ALooper arreglado. Riesgo: hilo del juego bloqueado esperando el primer frame de vídeo. En AR (passthrough/anclas/planos/profundidad ARKit): implementado, sin probar |
| SuperHOT (Unity, vpgles) | Negro (`SetupEyeTexture2` solo ANGLE); `SetControllerVibration` casi siempre amplitud 0 | Rama vpgles `kl_gles_legacy_eye_texture` + `vpgles_wrap_image_as`; háptica global. Sin reprobar |
| Hellsplit (UE4) | Ojos separados en cargas; háptica por `SetControllerHapticsAmplitudeEnvelope` (×1259) rechazada | `drop` de EndFrame4 sin capa de ojo tras haber tenido una (`ever_eye_layer`); envolvente implementada; sensores/Settings UE4. Sin reprobar |
| TRIANGLE STRATEGY (UE4, Vulkan; primer juego GLES por manifiesto) | Deadlock en `klb_pthread_rwlock_wrlock`; hilo de juego bloqueado en carga tras destruir passthrough; SIGSEGV en slowLog (VPKEngine a316f28) | rwlocks propios (mutex+cond, mapa por dirección); `[snap] game thread silent` a 10/30 s para diagnosticar. Sin reprobar |
| Batman | Sin entradas de problema; solo: `assets/vr_splash.png` 2048x435 (carátula) | Ninguno pendiente conocido |
| Blade & Sorcery (B&S) | Solo: `vr_splash.png` 3840x2160 (carátula) | Ninguno pendiente conocido |
| ITR2, Deadpool, DGH | Sin datos en la bitácora anterior | Sin estado; probar y registrar |

## 2. Arquitectura y ficheros clave

**Runtime Klepton** (`klepton/runtime/`, fuentes listadas en `klepton/Makefile` `RUNTIME_SHIP`/`RUNTIME_JNI` → `libklepton.a`; Xcode no lista fuentes sueltas):
- `libc/`: bionic propio (`kl_libc.c` memoria/presión, `kl_pthread.c` rwlocks, `kl_shim.c` resolución de símbolos y `kl_shim_known`, `kl_bionic_extra*.c` ~800 funciones, `kl_bionic_extra4.c` zlib); `kl_wait.h` registro de esperas por hilo.
- `kl_jni.c` + `jni/kl_jni_*.c`: JNI sintético (sysinfo, net/Settings, window, ue4, hostvideo, services, softinput, fmod, bridge).
- `guest/`: NDK (`kl_ndk.c`: ALooper, sensores, AInputQueue, AHardwareBuffer) y drivers por motor: `kl_slink`, `kl_gles3jni`, `kl_native`, `kl_sdl2`, `kl_ue4`, `kl_jkxr`, `kl_driver`; `kl_exe.c` (guest ejecutable para CTS).
- `xr/`: OVRPlugin `kl_ovrp*.c` (`_render`, `_input`, `_misc`, `_spatial`, thunks x8 en `kl_ovrp_sret2.S`, registro `kl_ovrp_register`/`kl_ovrp_priv.h`), OpenXR `kl_openxr*.c` (`_ext`, `_spatial`, `_vendor`, `kl_openxr_priv.h`), `kl_ovrplat.c` (OVRPlatform), `kl_spatial.c`, `kl_envdepth.c`.
- `gfx/`: `kl_vulkan.c` (puente a VPVulkan/MoltenVK, trim, GPU lost, TAA/RCAS/máscara UI), `kl_egl.c`/`kl_gles.c` (EGL/GLES, ruta vpgles), `kl_reproject.c` (+`_ptw.c`; shader de composición con RCAS fusionado).
- `media/` (mediandk, opensl, aaudio, camera), `diag/` (`kl_sample.c` snapshots), `kl_fault.c`.

**App Swift**: `app/Sources/` `VQLog`, `VQFolder`, `VQGames`, `VQPrepare`, `VQLaunch` (ajustes → variables `KL_*`/`VPVK_*`, identidad Quest 3), `VQLauncher`, `VQCover`, `VQApiScan`, `VQConformance*`; `app/Native/vq_apk.c`, `vq_config.c` (→ `vpvk_config_set_*`), `vq_scan.c`. Visor: `klepton/visionos/Sources/` `KleptonCompositor` (compositor, RCAS/uiMask, GPU lost, capas), `KleptonTAA` (VPKTAA nativo, anillo de salida), `KleptonControllers` (mandos + háptica CoreHaptics), `KleptonSpatial`/`KleptonEnvDepth` (ARKit), `KleptonApp`, `kl_app.c` (heartbeat `[hb]`). Lanzamiento por `VQLaunch`.

**CI**: `.github/workflows/visionos.yml` (solo `[build]` en el commit o manual): VPVulkan (caché por hash de `vpkengine/gpu/**`) → MoltenVK 1.4.2 → `tools/ci/syntax-xros.sh` → `tools/ci/host-tests.sh` → `libklepton.a` → app (`xcodebuild`) → **Gate** (falla si syntax/tests fallaron; no hay IPA) → `scripts/ci-publish-log.sh` (log a rama `ci-logs`, fichero `visionos.txt`) → release `visionos`. Los pasos de syntax/tests no cortan el job para que la app informe también sus errores.
**`klepton/tools/ovrp_abi.py`**: compara las firmas de `kl_ovrp*` con el desensamblado de `libOVRPlugin.so` real (9 versiones SDK 1.59–1.117, ref 1.117; lee también los `kl_ovrp*.S`). Resultado vigente: 36 graves → 2, ambos falsos positivos verificados (GetEyeFrustum devuelve HFA v0..v3; GetFloat devuelve float en s0, la herramienta los clasifica BOOL).

**Datos de plataforma**: dylibs sin firmar en `QUEST/Cache/<j>/dylibs`, firmadas en `Library/Application Support/VisionQuestPro/Signed/<j>/`; logs en `QUEST/Registros/<fecha>-fin-<juego>`; centinela `Registros/sesion-en-curso.txt` + MetricKit; anclas en `QUEST/Partidas/<juego>/anclas.json`; inventario de APIs Meta en `QUEST/Cache/<juego>/meta-api.txt`.

**ABI OVRPlugin verificada (no revertir)**: x8/sret para GetNodePose/Velocity/Acceleration(2), GetTrackerPose, GetEyePose/..., GetTrackingCalibratedOrigin (28 B tipo Posef; velocidad/aceleración en Position, orientación identidad). GetEyeFrustum HFA4 {near,far,fovX,fovY}; GetControllerState3 = ovrpResult f(mask,out64); GetControllerState4/5/6 ponen a cero 96/100/108 B; GetTimeInSeconds = ovrpResult f(double*); GetCaps/GetStatus ovrpBool, GetCaps2/GetStatus2 con máscara por valor (bit0 = running, sin enum verificado); GetEyeLayerRecommendedResolution(out) un solo arg; `ovrp_PollEvent` acota la escritura por versión SDK (`klovrp_pollevent_cap`, 4..3996 B); `ovrp_Get*Ptr/Pointer` y nombres con ProcAddr/Hook devuelven 0, nunca -1004 (Unity lo usa como `xrGetInstanceProcAddr`); teclado (4 funciones 1.81-1.104) rechazado -1004; ovrKeyValuePair struct de 40 B por valor/x8.

## 3. Cambios del último lote (a8b07fc / b83ac15)

**Memoria**
- Aviso de memoria baja DECIDIDO en segundo plano (`kl_mem_pressure_poll`, timer 100 ms) y ENTREGADO en el hilo del juego (`kl_mem_pressure_deliver()` en el pump de `kl_slink`(x2), `kl_gles3jni`, `kl_native`, `kl_sdl2`, `kl_ue4`, `kl_jkxr`, cada 10 ticks; `kl_driver_frame` ya lo hacía; `exchange` de `g_lowmem_pending` evita doble entrega): `guest/*.c`.
- `kl_mem_total` = 8192 MiB (`KL_MEM_TOTAL_MB` revierte); avisos sobre el límite real (footprint+avail) al 65 % y crítico 80 %; histéresis (rearma <70 %): `libc/kl_libc.c`.
- Trim propio `kl_vulkan_trim` (VPKEngine `vpvk_trim_memory`, nivel 2 si critical, 1 s por nivel) también con guests GLES sobre vpgles; retired OpenXR images solo con guest Vulkan: `gfx/kl_vulkan.c`.
- Breakdown en `[hb]` (`kl_mem_breakdown`: ledger_tag_graphics/media/net/purg, fp_peak, lim_rem), `[hb] jump +N MiB` (>200 MiB/tick), `[hb] blocked`, `kl_mem_regions` por user_tag: `libc/kl_libc.c`, `visionos/Sources/kl_app.c`.
- Anillo de salida de VPKTAA 5→4 slots y se suelta el anillo viejo antes de crear el nuevo: `KleptonTAA.swift`.

**RCAS y compositor**
- RCAS fusionado en `kl_reproject_f` (uniforms `rcas_con`/`rcasFlags`/rect; bit 1 = hay máscara UI; `c = mix(rcas, original, mask)`), `uiMask0/uiMask1` texture 2/3 (`texture2d_array`), `uiMaskDummy` 1x1 donde no hay: `gfx/kl_reproject.c/.h`, `KleptonCompositor.swift`; `kl_vulkan_ui_mask_texture` en `kl_vulkan.c`.
- RCAS como pase independiente 0–100 % (global y por juego, `KL_RCAS`/`VPVK_RCAS`, 0 = sin pase) en todos los modos de AA: `VQLaunch.swift`, `vq_config.c`, `kl_vulkan.c` (`kl_vulkan_rcas_on`, frames `rcas_only`), `KleptonTAA.swift`.
- Medición `[cp] gpu p50/p95/max` del compositor (completion handler); GPU lost → ojo en negro (`taaHide`) + cierre con aviso (`kl_vulkan_gpu_lost`, `kl_jni_request_quit`); tamaño de ojo final guardado (`vq.eyeSizeFinal`, clave = env BASE|SCALE|MAX) y empujado antes de arrancar el guest: `KleptonCompositor.swift`, `VQLaunch.swift`.

**Háptica por mano**: motores CoreHaptics por mano en cola serie `vq.haptics` con reintento/backoff, watchdog 0,4 s, identidad en `stoppedHandler`; perfil autodetectado (`HapticProfile.detect`): `KleptonControllers.swift`. Anillo 4096, `SetControllerHapticsAmplitudeEnvelope/Haptics2/LocalizedVibration`, `XR_FB_haptic_amplitude_envelope`/`_pcm`: `xr/kl_ovrp.c`, `kl_ovrp_input.c`, `kl_openxr.c`, `kl_openxr_ext.c`.

**ALooper / NDK** (`guest/kl_ndk.c`): wake interno (`addFd/removeFd`, byte 'i') distinto del `ALooper_wake` real (byte 'w'); `kl_looper_poll` con deadline (reconstruye el set y sigue esperando el resto del timeout; timeout 0 → TIMEOUT; solo 'w' devuelve `ALOOPER_POLL_WAKE`); poller de input único (`g_poller_mu`, `poller_started`, `detachLooper` hace join).
**Vídeo**: SurfaceTexture/Surface reales drenados en el hilo del juego, `onFrameAvailable` desde el hilo del reader, `[vtrace]` (`KLM_VTRACE_MAX`): `jni/kl_jni_hostvideo.c`, `media/kl_mediandk.c`.
**Sensores/Input/HWBuffer** (`kl_ndk.c`): acel/giro/gravedad/rotation vector derivados de la pose de cabeza (mundo ENU = (x,−z,y)); AInputQueue real + puente mandos Touch→gamepad; `AHardwareBuffer_{to,from}HardwareBuffer`, `send/recvHandleToUnixSocket`.
**UE4/Settings** (`jni/kl_jni_ue4.c`, `kl_jni_net.c`, `kl_jni_window.c`, `kl_jni.c`): `nativeHandleSensorEvents([F[F[F[F)`, refresh rate vía ovrp, fuentes en `/data/data/<pkg>/cache/fonts`, clipboard (UIPasteboard), teclado virtual; tabla única Settings.Secure/Global/System; `device_name` "Meta Quest 3|2"; WindowMetrics; `hasSystemFeature` (audio, mic, sensores, vr.*, wifi, bluetooth).
**Símbolos**: zlib (`get_crc_table`, `crc32_z`, `uncompress2`, `deflate*/inflate*`…) en `libc/kl_bionic_extra4.c`; `ovrKeyValuePair_make*` y `kl_ovrplat_owns/known` en `xr/kl_ovrplat.c`; `kl_shim_known` por familia (xr/SL/ovr/egl) en `libc/kl_shim.c`.
**EGL/GLES sin abort**: GL ausente → stub que devuelve 0 + `[egl] absent GL function called, returning 0: NAME` y lista al cierre (`kl_egl_report_absent`); `kl_gles_report_unresolved` con `pthread_mutex_trylock` (apto para señal) y `g_miss_reported`: `gfx/kl_egl.c`, `kl_gles.c`.
**CI**: Gate + `syntax-xros.sh` + `host-tests.sh` (gpu_host con `-k` para listar todos los errores): `visionos.yml`, `tools/ci/*`.
**Arreglos de CI de macOS**: `lgamma_r`/`lgammaf_r` con prototipos explícitos (math.h de Darwin los oculta tras `_REENTRANT`); el paso del runtime no corta el job para que `xcodebuild` saque sus errores; recetas de `build/xros|xrsim/libklepton.a` compilan todo y fallan al final antes del `ar`.

## 4. Decisiones de diseño vigentes

- Offline total: `KL_NET_OFFLINE`=1 (solo loopback y AF_UNIX), `KL_PLAT_TOKEN_ERROR`=1 (AC no entra en UbiServices; su aviso no se puede cerrar y es esperado), usuario Meta offline. Sin cuentas ni servicios.
- Todo global, sin condiciones por juego (`kl_is_ac_nexus` y muletas de AC eliminados). Excepción: ajustes por juego del usuario (AA, resolución, anisótropo, RCAS).
- VPKTAA es opcional y único = nativo de VPKEngine (Vulkan y vpgles, ovrp y OpenXR; sin cobertura en ANGLE/MoltenVK). El MSAA del juego se mantiene por defecto (modo Automático: VPKTAA solo si no hay MSAA detectado).
- `nativeLowMemory` nunca fuera del hilo del juego: crash de AoT con IL2CPP en hilo no registrado (x18 shadow 0), un attach JNI no lo arregla. No se llama desde `kl_jni_drain_ui_tasks` (reentrante vía `executeMainThreadJobs`). Con el juego bloqueado solo recorta `kl_vulkan_trim`.
- `ovrp_GetEyeLayerRecommendedResolution` devuelve -1004 sin capa de ojo viva (si no, Unity usa resolución dinámica con viewport reducido y deforma el 3D de AC).
- GLES: ES 3.1, no 3.2.
- Identidad Quest 3 coherente con `KL_QUEST3` (OVRPlugin headset 11, OpenXR systemName, JNI SDK 32); mandos como Touch de Quest 2. Eye tracking → Unsupported.
- Háptica autodetectada por mando, sin ajuste de intensidad (clave `vq.hapticsGain` borrada).
- Quads de capas en orden de envío (`KL_OVERLAY_SUBMIT_ORDER` def. 1): sin él no aparece el panel de calibración de AC.
- Foveado de Quest siempre apagado; foveado propio opt-in (Apagado por defecto), M5 solo en M5; exige VPKTAA nativo, declina con MSAA.
- ASW desactivable (por defecto activo, `KL_CP_ASW`/PTW solo por variable); ritmo de panel encajado a 120/100/96/90 Hz (`KL_CP_PANEL_SNAP=0`).
- Valores mágicos: burst de audio 1024 con Wwise (256 da «Voice Starvation»); `kMaxArchiveFiles` 512 (AC genera 4101 PSO; con 64 ficheros evicta y recompila cada arranque); encoders >2048 draws partidos en trozos de 256 (`VPVK_MAX_DRAWS_PER_CB`); anillo TAA 4 (con 3 un pinned antiguo deja sin slot; si sale `no free output` volver a 5); máscara UI R8 en slot 7.
- Datos de juegos (APK, logs, assets) nunca en el repo. Pruebas solo con juegos propios del usuario.

## 5. Diagnóstico

Logs: `QUEST/Registros/` (también compartibles desde la app); línea 1 = hash de build. CI: `git fetch origin ci-logs` o API `repos/<owner>/VisionQuestPro/contents/visionos.txt?ref=ci-logs` (primero errores, luego las últimas 150 líneas); `gh run view --log` da 403.

| Etiqueta | Significado |
|---|---|
| `[cp] gpu p50/p95/max … (N cmdbufs, rcas X)` | Tiempo GPU del compositor; `gpu.sec` (p50/p95/max/over10) = lo mismo de VPKEngine por segundo |
| `gpu.sec` / `gpu lost` | Si hay GPU lost: ojo negro y cierre con mensaje en el launcher |
| `msaa_released` | Etiqueta de VPKEngine sobre el MSAA del juego liberado (memoria tile); si aparece `tile_rejected>0` en mem stats, el MSAA 4x no cabe |
| `[mem] threshold`, `[mem] OS pressure`, `[mem] trim (…)` | Aviso 65/80 %, presión del SO, recorte (`VPKEngine released N MiB`) |
| `[hb]`, `[hb] jump`, `[hb] blocked` | Heartbeat con breakdown de memoria; salto >200 MiB/tick; hilo bloqueado |
| `[snap] game thread silent` | Hilo del juego sin actividad a 10/30 s (volcado de esperas por hilo, `kl_wait.h`) |
| `[vtrace tid=…]` | Traza de vídeo/JNI: FindClass, llamadas Java sin emulación (`NO-OP game Java void`) |
| `gles.diag` / `gles.warn` (VPKEngine), `[gles] ruta:` | Errores GL del guest (INVALID_ENUM first at…) y ruta elegida |
| `[egl] absent GL function called, returning 0: NAME` | Función GL no resuelta; lista final al cierre |
| `[cp] RCAS fused: UI mask … in use` / `no UI mask for eye N` / `UI mask WxH is not the eye texture's` | Estado de la máscara UI; si falla el MSL: `[cp] composite pipeline failed` |
| `[taa-inline] eye AxB -> CxD: output ring rebuilt`, `no free output` | Anillo TAA |
| `[cp] haptics: hand N engine up … locality X`, `FIRST REAL PULSE`, `[ovrp] haptics: first real pulse`, `SetControllerHapticsAmplitudeEnvelope … (struct order …)` | Háptica; si el orden sale `Count,Duration` corregir la doc; ajustes: `KL_HAPTICS_LOCALITY=all|handles`, `KL_HAPTICS_FLOOR`, `KL_HAPTICS_MIN` |
| `[cp] HALF RATE …`, `minimumFrameRepeatCount` | Medio ritmo impuesto por el sistema / recuperación |
| `[cp] layer paused/invalidated`, `[cp] backfill`, `eye stage N … held back/published`, `[zero] compositor leyó imagen pendiente` | Ciclo de capas/ojos |
| `vk.zeroinit`, `gpu.shader.msl cached=`, `gpu.slowcb`, `gpu.hang`, `gpu.prewarm ar_hits/ar_misses`, `vk.taa native gpu_ms`, `vk.caps`, `vk.err` | VPKEngine |
| `[magenta]`, `[prov] layer N … no write seen` | Shader de error de Unity (siempre calienta uno), procedencia de capas |

## 6. Pendientes y riesgos abiertos (por prioridad)

1. **Verificar en visor todo el lote** (sección 3). Mirar primero: framerate (bajones de AC y AoT: `[cp] gpu`, `gpu.sec`), flashes de AC, vídeo de Demeo (riesgo: hilo del juego bloqueado esperando el primer frame; `[vtrace]`), `[mem] trim` con guest GLES, `RCAS fused … in use` con HUD.
2. Pico de memoria del driver en la carga de AC (~1,8 GB/s no contabilizado por Metal hasta ~8,19 GiB): sin atribuir. Con el log decidir recortes (etapas del ojo 3→2, historial TAA, prewarm sin owner); evaluar entitlement increased-memory-limit.
3. Pausa con la Corona: hoy background → LayerRenderer `.invalidated` → `kl_app_guest_stop` → `_exit(0)`; `ovrp_GetAppHasInputFocus` siempre 1. Diseño: no parar el guest, foco perdido al juego (ovrp InputFocus/VrFocus, estados OpenXR, onPause/onResume JNI), cortar envíos GPU, «Reanudar». Primer paso: experimento con `KL_EXIT_ON_BACKGROUND=0`.
4. Rosa de AC: no son capacidades Vulkan ni `-force-vulkan`; objetos invisibles en Quest, pegados a las manos. Candidato: capa #5 Quad/head 512x512 (clear-only). Bisecar con `[prov]`; falta vulkaninfo real de Quest 3.
5. `onInputQueueCreated`: `kl_ndk_input_queue` sin llamador (`kl_nativeactivity.c`); conectarlo.
6. Verificar la firma/orden de `nativeHandleSensorEvents` en UE4 (tilt, rate, gravity, accel=0).
7. Callbacks de ALooper: se ignora su valor de retorno (¿desregistrar si 0?).
8. Sensores: mezcla de pose en vivo con velocidad del frame fijado.
9. Riesgos del TAA/RCAS: HDR saturate; lag de 1 frame de la máscara UI (es la del último envío del guest); anillo 4 (ver `no free output`); guests ANGLE/sin VPKEngine sin máscara; ojo foveado de otro tamaño sin máscara; EDS × máscara UI no combinan.
10. Demeo en AR: política ARKit por manifiesto (escena solo con `USE_SCENE`, anclas con `USE_ANCHOR_API`); extensiones de escena/planos/profundidad se evalúan una vez en el primer enumerate y requieren ARKit autorizado; orientación del bind de manos y eventos discovery 104/105 sin verificar.
11. Foveado propio sin probar en visor (M2/M5; reenviar tangentes con `vpvk_config_set_foveation_eyes`; mapa identidad 16384², coste fold/expand sin medir).
12. OVRPlugin: flags SpaceWarp y orden de LatencyTimings sin contrastar (usar `libOVRPlugin.so` de un APK propio); fov de frustums v0 asumido en grados; `ovrp_SetControllerHapticsParametric`, `GetControllerParametricProperties`, `TriggerVibrationAction` rechazadas.
13. Carátulas: los APK no traen la 16:9 (solo `app_icon` y `vr_splash`); `VQCover.swift` usa carpeta → OculusDB (`oculusdb.rui2015.me/api/v1/search/<package>`, no verificado byte a byte) → `vr_splash` → icono. Marcadores de caché v4.
14. GLES (vpgles): sin transform feedback, SSBO/imágenes/atómicos, geometry/tess, program binary; occlusion query = 1; `gl_PointCoord` fijo; detección de MSAA solo en Vulkan; `kl_gles_sym` NULL cae a la pasarela vieja. `pollAADetection` limitado a Vulkan.
15. Android: AObb sin montaje real; AImage solo YUV_420_888; OpenSL sin efectos/3D/MIDI; posix_spawn/exec/SysV ENOSYS; inotify sin IN_ACCESS/OPEN/CLOSE; sin acel. lineal ni magnetómetro; `kl_egl.c`/`kl_vulkan.c` necesitan consulta pura `*_known` para el escáner; sin validar en Darwin `kl_bionic_extra2/4.c` (campos de FILE) y thunks `.S`.
16. VPKEngine: salida de fragmento int/snorm y blits 1D sin auditar; ycbcr sin 10/16 bit; no viable en Metal: variablePointers completo, LogicOp/PatchControlPoints dinámicos, availability/visibility, BDA capture/replay. Hilo que muere sin soltar su contexto GL.
17. Conformidad: CI `conformance.yml` (Vulkan CTS 1.4.6.2 `rapida` ~36k casos, OpenXR CTS 1.1.58; releases `conformance`/`conformance-openxr`; app `VQConformance*`, resultados en `QUEST/Partidas/<test>/cts/`; `KL_GUEST_EXE`, `KL_EXE_MAX_RECOVERIES`=50). Referencia: 96,9 % de 6551 casos (antes del perfil 1.3). Falta: dEQP-GLES (gles2/3/31 en CI y runner EGL), grupos CTS tras `VPVK_EXTRA_EXTENSIONS=1`.
18. Futuro: SSAO propio en VPKEngine (media resolución, toggle por juego, apagado). `PTW`/`ASW` por variable (`KL_CP_PTW`, `KL_CP_ASW`) pendientes de ajuste. Sin usar en `gpu/gles/glsl` y avisos de compilación revisados: ninguno es bug.
19. Limpieza: `kl_ovrp.c` sondeo de jitter de frustum inerte; avisos de formato `%llu` en `kl_egl.c`.

## 7. Cómo compilar, probar y publicar

- Build: el commit debe llevar `[build]` en el mensaje (o lanzar `visionos` a mano) → IPA en el release `visionos`. Solo una build completa tras revisar; no lanzar builds por cambios sueltos.
- VPKEngine se clona de `main` en CI (sparse `runtime` + `gpu`): hay que pushear los cambios de VPKEngine ANTES de lanzar la build. La caché de VPVulkan se invalida por hash de `vpkengine/gpu/**`.
- Local (sin Mac): `clang -fsyntax-only` con las flags de `XROS_CFLAGS` solo sirve para ficheros sin cabeceras Darwin; el resto se valida en CI (`syntax-xros.sh`, `host-tests.sh`). Swift/.mm solo se compilan en CI.
- Tras un fallo: leer `visionos.txt` en `ci-logs` (ver sección 5).
- Identidad de commits: `Claude <noreply@anthropic.com>`. Ningún dato personal en el repo.
- Bitácora: actualizar tras cada función, bug o build.

## 8. Lote JNI completo + take_rgba (sin build; pendiente de verificar en visor)

Causa (log Demeo b83ac15): SIGABRT por `GetPrimitiveArrayCritical` (slot 222) → `klj_unimpl`; antes, `kl_media_surface_take_rgba` no convertía el formato nativo '&8v0' del decodificador con ventana.

- `klepton/runtime/kl_jni.c`: implementados y registrados en `klj_build_tables` GetPrimitiveArrayCritical/Release (puntero directo, isCopy=0), GetStringCritical/Release (copia UTF-16), GetStringRegion, GetStringUTFRegion (lanzan StringIndexOutOfBounds), MonitorEnter/Exit (mutex recursivo por objeto, lista con `users`; Exit sin Enter devuelve -1), GetObjectRefType, GetSuperclass, IsAssignableFrom, DefineClass (NoClassDefFoundError), AllocObject, ToReflectedMethod/Field, CallNonvirtual<T>MethodV, y Get/Set de campos Byte/Char/Short/Double (instancia y estáticos) + SetStatic* completos. `klj_unimpl` ahora imprime hilo e imagen+offset del llamador (`kl_addr_image(__builtin_return_address(0))` tomado en el stub).
- Siguen en `klj_unimpl` solo los 10 `CallNonvirtual<T>Method` variádicos planos (requieren thunk `.S` en `kl_va_thunks.S`) y los slots reservados 0-3.
- `klepton/runtime/media/kl_mediandk.c`: `take_rgba` convierte todo lo que no sea 32BGRA con un `VTPixelTransferSession` del reader a un CVPixelBuffer 32BGRA reutilizado (IOSurface), y luego usa la copia BGRA→RGBA existente; si falla y el frame es NV12 cae a la ruta CPU. Sesión y buffer se liberan en `klm_AImageReader_delete` y en abandon/ruta abandoned de `take_rgba` (`klm_reader_free_xfer`); `delete` también libra `r->rgba`. Diagnóstico `[media] take_rgba #N: 'fourcc' WxH via … (OSStatus), xfer ms, total ms` en las 3 primeras llamadas y en los primeros 16 fallos.
- Verificado: clang -fsyntax-only de kl_jni.c sin errores nuevos; test host (gcc ASan+UBSan, stubs) de GetPrimitiveArrayCritical, monitores, regiones de string, RefType y que ningún slot >3 sea NULL. NO verificado: la compilación de kl_mediandk.c (headers Darwin), VTPixelTransfer con '&8v0' real, coste en 4K.
- Pendiente: mirar en el log de Demeo `[media] take_rgba #1` (¿via VTPixelTransfer->BGRA, OSStatus 0?, ms) y que no vuelva el abort en slot 222.

## 9. Compositor: política de frame congelado, desglose por pase, registros (sin build; pendiente de verificar en visor)

Medido (log AC): `[cp] gpu` 4-5 ms p50 / 7 p95 / 9 max sobre 10 ms de periodo con el juego a 100 fps; con el juego congelado (serial 638) el compositor reproyectó 22 s el mismo frame ("513 stale in a row", pose de hasta 8,7 s).

- `klepton/visionos/Sources/KleptonCompositor.swift`:
  - Frame congelado (global): `staleHide` = serial sin cambiar `KL_CP_STALE_HIDE_S` (3,0) s de reloj y `staleInARow >= 8`. Con él: sin ojo (`alloc = nil`), `projLayerCount = 0`, sin overlays, y entra la ruta existente `drawBlackBackfill` (o el skybox de carga si el vídeo de carga está activo). Un serial nuevo lo apaga. Logs una vez por cambio: `[cp] stale -> backfill: …` / `[cp] fresh again: …`; motivo de backfill `guest frozen`.
  - Desglose por pase: ring de 4 `MTLCounterSampleBuffer` (timestamp, `.atStageBoundary`), 4 muestras por pase (inicio/fin vertex y fragment), solo el primer pase de cada frame. Se añade a la línea `[cp] gpu`: ` | pass frag p50/max … vert p50 … ms (N samples) rt WxH layered|per-eye foveated|uniform`. Una línea `[cp] pass timing: X ns/tick; first pass …` valida la unidad (se asume CPU timestamp en ns). `KL_CP_PASSTIME=0` lo apaga; sin soporte, `[cp] per-pass GPU timing unavailable`.
  - Estructura del composite (sin cambios, ya mínima): 1 cmdbuf, 1 pase de render con ambos ojos (vertex amplification, `renderTargetArrayLength=2`), 1 draw de rejilla, rate map del drawable aplicado, RCAS+máscara UI fusionados en `kl_reproject_f`, sin MSAA, sin blits/cómputo (salvo KL_CAPTURE). Profundidad `.store` obligatoria (visionOS reproyecta con ella).
- `klepton/runtime/gfx/kl_reproject.c` (`kl_reproject_f`): la máscara UI se lee antes y, donde vale 1.0, se saltan las 4 taps RCAS (`mix(s,c,1)==c`: misma imagen).
- `klepton/runtime/gfx/kl_vulkan.c`: `klvk_slow_wait_note` registra `[frame-end] … waited N ms (gpu backlog M ms, serial S)` cuando el `QueueWaitIdle` de fin de frame (ambas ramas) pasa de 500 ms; tope 40 líneas.
- `app/Sources/VQLog.swift`: `shareable` incluye `crashAttachments` (`gpu-hang-*.metal`, `metrickit-*.json`, `*.ips` de Documents/Registros y QUEST/Registros); `VQMetrics.didReceive` escribe cada payload MetricKit en `Registros/metrickit-<ts>.json`. No existe zip: el botón compartir pasa la lista de ficheros a `ShareLink` y `saveToQuest` los copia a QUEST/Registros/<fecha>.
- Verificado: `clang -fsyntax-only` (XROS_CFLAGS sin SDK) de `kl_vulkan.c` sin errores nuevos; `t_ptw` con gcc ASan+UBSan ok. NO verificado: Swift (no compila aquí), `kl_reproject.c` (requiere simd/Darwin), MSL en dispositivo, soporte de counters en visionOS, unidad ns de `sampleTimestamps`.
- Pendiente: en el próximo log mirar `[cp] gpu … | pass frag` (frag p50 vs cmdbuf p50: si frag << cmdbuf el resto es solapamiento con el juego), `[cp] stale -> backfill`, `[frame-end] … waited`.

- Revisión adversarial (sin build): `resolveCounterRange` ahora con `NSRange(location:length:)` (Range<Int> no compila); `withUnsafeBytes` con tipo explícito; `ptReserve` libera el slot huérfano; umbral de frame congelado 1,0 → 3,0 s (cargas de 1-3 s y juegos a <1 fps parpadeaban negro/frame); `VQLog.crashAttachments` lista solo la carpeta local en el acto y QUEST/Registros desde caché refrescada en segundo plano (30 s, `refreshQuestAttachments`), porque `shareable` se evalúa en cada render de SwiftUI.

## 10. Sello de caché por versión del traductor (sin build; pendiente de verificar en visor)

- `app/Sources/VQLaunch.swift`: nuevo `translatorTag()` (símbolo `vpvk_translator_tag` de VPVulkan, con respaldo al viejo `vpmsl_translator_fingerprint`); `firstRun` y `aot` lo usan en el sello (`firstrun.v<N>.<tag>`, `aot.<tag>.r<recetas>`). Antes el sello seguía el hash de las fuentes del traductor: cada lote de VPKEngine rehacía la primera ejecución/AOT y retraducía todo (AC Nexus: 2432 etapas, 4,1 s en el hilo del juego). Ahora el tag (`b<base>.l<lower>.e<emit>.d<driver>`) cambia solo cuando cambia la salida del traductor (ver bitácora de VPKEngine §8). Los sellos existentes (`firstrun.v<N>.<hash>`) quedan obsoletos una vez: se rehace la preparación del primer arranque tras instalar esta build.
- `translatorTag()` recorta a 32 caracteres (el respaldo `vpmsl_translator_fingerprint` es un hash largo; el tag nuevo cabe entero).
- El runtime (`klepton/runtime/gfx/kl_vulkan.c`) no interviene: la creación asíncrona de pipelines vive en VPVulkan (`VPVK_ASYNC_PIPELINES`, por defecto activa; `VPVK_EXTRA_EXTENSIONS=1` la desactiva). Diagnóstico nuevo en el log del visor: `vk.boot_summary`, `vk.pipeline_skip`, `vk.pipeline_wait what=…`.
