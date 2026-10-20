# Bitácora — VisionQuestPro

Estado vigente y compacto. Historial: `git log -p bitacora.md`.

## 1. Estado actual (2026-10-20)

- **Proyecto**: app visionOS que ejecuta juegos de Quest (APK + datos descomprimidos en `QUEST/Juegos`) sin Quest. Base Klepton (MIT, `klepton/`): cada `.so` ARM64 se envuelve en un dylib firmado y se carga con `dlopen`; bionic, JNI, NDK, OVRPlugin y OpenXR propios; sin traducción de CPU. Solo offline.
- **Gráficos**: VPKEngine (`VPVulkan.framework`: Vulkan/SPIR-V → Metal; incluye vpgles = GLES) y VPKTAA nativo. Compositor propio en Swift (`KleptonCompositor`).
- **Última build en verde**: run CI 39402167780, commit "Fin de frame segmentado apagado por defecto" (`fd25cf4`), con Gate. IPA en el release `visionos`, asset `VisionQuestPro.ipa`. **VPKEngine en main `44c9785`**.
- **Verificado en visor (por logs)**: AC Nexus (log 69: resolución, tiempos del compositor, congelación del guest), Batman (logs 66/68: fin de frame, coste del compositor), Demeo (aborts JNI de `b83ac15`). Medidas del compositor y causas de crash anteriores a esta build.
- **NO verificado en visor** (todo lo posterior a esos logs): puerta blanda solo para el ojo marcado, blend por capa, diagnósticos del log 73, flush de caches en hilo aparte, freno de encoders de AC, cuadro gris de Demeo, IAP offline, slider de resolución, backfill opaco, JNI completo, `take_rgba` con VTPixelTransfer, háptica por mano, ALooper/sensores/AInputQueue, RCAS fusionado + máscara UI, foveado propio, anillo TAA de 4 slots, sello de caché por tag del traductor, pipelines asíncronos.

## 2. Estado por juego

| Juego | Último problema → causa | Hecho / pendiente |
|---|---|---|
| AC Nexus (Unity/Vulkan, Wwise) | Muro de memoria (jetsam ~8191 MiB) por ~39.000 blit encoders (~39k regiones IOAccelerator pequeñas en ~4 s) | Presupuesto de encoders con contrapresión (en VPKEngine). Sin confirmar en visor |
| | Resolución 2976→3840: el plugin del guest ensancha el desc (unión de conos canteados) y AC pide textureScale 1,3 encima | `klovrp_fill_eye_desc` ya no multiplica por el escalado del juego; ensanche aprendido; slider (ver 4/5). Sin confirmar |
| | «Se ve la habitación» durante la carga | Backfill negro opaco en TODO frame sin passthrough + trazas `[pt] immersion style requested` y `[cp] slow frame`. **Causa sin confirmar** (alfa 0 refutado: sin passthrough el clear es alfa 1 y los shaders fuerzan alfa 1). Ojo: `[pt] mode auto` = `KL_PASSTHROUGH` sin definir |
| | Warmup 22 s → 4,5 s | Sello de caché por `vpvk_translator_tag` (ya no por hash de fuentes) + pipelines asíncronos |
| | Congelación del guest (serial parado, hasta 22 s) | >3,0 s sin serial nuevo y `staleInARow>=8` → backfill (`guest frozen`) |
| | Cuadrados rosas | Causa sin identificar. No son capacidades Vulkan ni `-force-vulkan`; candidato: capa Quad/head 512x512 clear-only; bisecar con `[prov]`. `[magenta]` (1 shader de error de Unity) no lo prueba |
| | `OVRInput connected=0x0` toda la sesión | No bloquea; pulsaciones llegan por otra vía |
| Demeo (Unity OpenXR + vpgles) | SIGABRT por `GetPrimitiveArrayCritical` (slot 222) → JNI completo implementado | Sin confirmar |
| | `([F)V` decodificado como float escalar (`klj_arr+0x4`, `klhv_stex_getMatrix`) | Arreglado: todo tipo con `[` es referencia; `klj_as_object` protegido |
| | Abort en `ovr_IAP_GetViewerPurchases*` (DurableCache, AppStateGDPR) | Fallo OFFLINE honesto. Sin confirmar |
| | Cuadro gris `Texture must have width greater than 0` | Causa: no se emitía INFO_OUTPUT_FORMAT_CHANGED en modo superficie → ahora sí. **Pendiente de confirmar** |
| | Vídeo de logos | Advertencia: NO era EXTERNAL_OES. Riesgo vivo: hilo del juego bloqueado esperando el primer frame de vídeo. AR (passthrough/anclas/planos/profundidad ARKit) implementado, sin probar |
| Batman (Vulkan) | Congelaciones de 40 s | Pipelines asíncronos aunque el perfil Vulkan completo esté activo |
| | Pase vacío MSAA | Fusionado (<1 ms real) |
| | fps limitados por GPU | Fin de frame segmentado/pipelining no dio fps y metió judder (~10 % de presentaciones tardías, +6 ms de edad de pose) → APAGADO por defecto |
| | Compositor 4–6 ms por present a 100 Hz con calidad 1.0 | Calidad elegida por el usuario: no tocar. Medido con `[cp] gpu` + desglose por pase |
| | Memoria: 847 MB `tex_msaa` | Ver pendiente del corte Feedback (sec. 7) |
| AoT (Unity/Vulkan, IL2CPP) | GPU al límite con MSAA x2 a 2976x1912 → visionOS impone 50 Hz (`HALF RATE`); rosa/1 s en tutoriales 2D | `KL_CP_HALFRATE_RECOVER`; `gpu.sec`/`[cp] gpu`. Verificar fps |
| | Crash si `nativeLowMemory` se entrega fuera del hilo del juego | Solo en el hilo del juego |
| SuperHOT (Unity, vpgles) | Negro (`SetupEyeTexture2` solo ANGLE); háptica amplitud 0 | Rama `kl_gles_legacy_eye_texture` + `vpgles_wrap_image_as`; háptica global. Sin reprobar |
| Hellsplit (UE4) | Ojos separados en cargas; háptica por `SetControllerHapticsAmplitudeEnvelope` rechazada | Drop de EndFrame4 sin capa de ojo tras haber tenido una (`ever_eye_layer`); envolvente implementada; sensores/Settings UE4. Sin reprobar |
| TRIANGLE STRATEGY (UE4, Vulkan; GLES por manifiesto) | Deadlock en `klb_pthread_rwlock_wrlock`; bloqueo tras destruir passthrough; SIGSEGV en slowLog | rwlocks propios (mutex+cond); `[snap] game thread silent` a 10/30 s. Sin reprobar |
| Blade & Sorcery, ITR2, Deadpool, DGH | Sin problemas registrados (B&S: solo `vr_splash` como carátula) | Probar y registrar |

## 3. Arquitectura y ficheros clave

**Runtime** (`klepton/runtime/`; fuentes en `klepton/Makefile` `RUNTIME_SHIP`/`RUNTIME_JNI` → `libklepton.a`; Xcode no lista fuentes sueltas):
- `libc/`: `kl_libc.c` (memoria/presión), `kl_pthread.c` (rwlocks), `kl_shim.c` (símbolos, `kl_shim_known`), `kl_bionic_extra*.c` (~800 funciones; `extra4` = zlib), `kl_va_thunks.S` (variádicos JNI AAPCS64), `kl_wait.h` (esperas por hilo).
- `kl_jni.c` + `jni/kl_jni_*.c`: JNI sintético (net/Settings, window, ue4, hostvideo, display, looper, services, softinput, fmod, bridge). `kl_fault.c`: cabecera de crash.
- `guest/`: `kl_ndk.c` (ALooper, sensores, AInputQueue, AHardwareBuffer) y drivers por motor (`kl_slink`, `kl_gles3jni`, `kl_native`, `kl_sdl2`, `kl_ue4`, `kl_jkxr`, `kl_driver`), `kl_android.c`, `kl_exe.c` (CTS).
- `xr/`: OVRPlugin `kl_ovrp*.c` (`_render/_input/_misc/_spatial`, thunks x8 `kl_ovrp_sret2.S`, `kl_ovrp_eyesize.h`), OpenXR `kl_openxr*.c`, `kl_ovrplat.c` (OVRPlatform/IAP), `kl_spatial.c`, `kl_envdepth.c`.
- `gfx/`: `kl_vulkan.c` (puente a VPVulkan: trim, GPU lost, fin de frame, TAA/RCAS/máscara UI), `kl_egl.c`/`kl_gles.c` (ruta vpgles), `kl_reproject.c` (composición con RCAS fusionado).
- `media/` (mediandk, opensl, aaudio, camera), `diag/kl_sample.c`.

**App Swift** (`app/Sources/`): `VQLog`, `VQFolder`, `VQGames`, `VQPrepare`, `VQLaunch` (ajustes → `KL_*`/`VPVK_*`, sello de primera ejecución), `VQLauncher`, `VQCover`, `VQApiScan`, `VQConformance*`; `app/Native/` `vq_apk.c`, `vq_config.c`, `vq_scan.c`. **Visor** (`klepton/visionos/Sources/`): `KleptonCompositor` (compositor, RCAS/uiMask, GPU lost, backfill, medición), `KleptonTAA` (VPKTAA, anillo de salida), `KleptonControllers` (mandos + CoreHaptics), `KleptonChroma` (passthrough/inmersión), `KleptonSpatial`/`KleptonEnvDepth` (ARKit), `KleptonApp`, `kl_app.c` (heartbeat `[hb]`).

**CI**: `.github/workflows/visionos.yml` (solo con `[build]` en el commit o manual): VPVulkan (caché por hash de `vpkengine/gpu/**`) → MoltenVK 1.4.2 → `tools/ci/syntax-xros.sh` → `tools/ci/host-tests.sh` → `libklepton.a` → app (`xcodebuild`) → volcado de API de CompositorServices (`compositor-api.txt`) → **Gate** → `scripts/ci-publish-log.sh` → release `visionos`. Syntax/tests no cortan el job (para que la app informe sus errores); el Gate sí falla y no hay IPA. Conformidad: `conformance.yml` (Vulkan CTS 1.4.6.2 `rapida`, OpenXR CTS 1.1.58; app `VQConformance*`, `KL_GUEST_EXE`).

**Datos de plataforma**: dylibs sin firmar en `QUEST/Cache/<j>/dylibs`, firmadas en `Library/Application Support/VisionQuestPro/Signed/<j>/`; logs en `QUEST/Registros/<fecha>-fin-<juego>` (+ `anteriores/`); centinela `Registros/sesion-en-curso.txt` + MetricKit; anclas en `QUEST/Partidas/<juego>/anclas.json`; APIs Meta en `QUEST/Cache/<juego>/meta-api.txt`.

**`klepton/tools/ovrp_abi.py`**: compara firmas `kl_ovrp*` con el desensamblado de `libOVRPlugin.so` (9 versiones SDK 1.59–1.117). Vigente: 2 diferencias, ambos falsos positivos (GetEyeFrustum devuelve HFA v0..v3; GetFloat devuelve float en s0, la herramienta lo clasifica BOOL).

**ABI OVRPlugin verificada (no revertir)**:
- x8/sret: GetNodePose/Velocity/Acceleration(2), GetTrackerPose, GetEyePose…, GetTrackingCalibratedOrigin (28 B tipo Posef; velocidad/aceleración en Position, orientación identidad).
- GetEyeFrustum HFA4 {near,far,fovX,fovY}; GetControllerState3 = `ovrpResult f(mask,out64)`; State4/5/6 ponen a cero 96/100/108 B; GetTimeInSeconds = `ovrpResult f(double*)`.
- GetCaps/GetStatus `ovrpBool`; GetCaps2/GetStatus2 con máscara por valor (bit0 = running, sin enum verificado).
- GetEyeLayerRecommendedResolution(out): un solo arg.
- `ovrp_PollEvent` acota la escritura por versión SDK (`klovrp_pollevent_cap`, 4..3996 B).
- `ovrp_Get*Ptr/Pointer` y nombres con ProcAddr/Hook devuelven 0, nunca -1004 (Unity lo usa como `xrGetInstanceProcAddr`).
- Teclado (4 funciones 1.81–1.104) rechazado con -1004; `ovrKeyValuePair` struct de 40 B por valor/x8.

## 4. Cambios de los últimos lotes (19–20 oct)

**Análisis de AC Nexus (log 73): texto del tutorial, combate, diagnósticos (sin build, sin verificar en visor)**
- Marca del ojo: `kl_vulkan.c` `klvk_mark_eye_image` llama a `vpvk_mark_eye_image` (dlsym en `g_vpvk.mark_eye_image`) en `kl_vulkan_layer_image` solo para la capa de ojo; log `[vk] eye image … vpvk_mark_eye_image -> 1` (`-1` = VPKEngine sin export → todos los pases duros). Las capas de panel nunca se marcan.
- Blend de capas (`kl_ovrp.c` `klovrp_overlay_blend`, `kl_ovrp.h` `blend_on/src/dst`; `KleptonCompositor.swift` `blendPipeline(for:base:)`): ABI (DWARF de `libOculusXRPlugin`): `ovrpBlendFactor` Zero 0, One 1, SrcAlpha 2, OneMinusSrcAlpha 3, DstAlpha 4, OneMinusDstAlpha 5. **No existe flag de alfa premultiplicado** en `ovrpLayerFlag_*` ni `ovrpLayerSubmitFlag_*` (hay `InverseAlpha` y `IgnoreSourceAlpha`, sin implementar): premultiplicado se expresa como One/OneMinusSrcAlpha en los factores. Solo se leen los factores (+0xb0/+0xb4/+0xb8) cuando `klovrp_quad_size_off == 0xbc` (en cabeceras antiguas +0xb0 es el tamaño del quad) y con valores 0..5; Swift construye (y cachea) un pipeline por par con los factores RGB (alfa sin tocar); sin factores = comportamiento anterior. Log una vez por capa: `[ovrp] layer N blend: …` y `blend: …` en `[cp] compositing overlay layer`. `KL_OVRP_BLEND_FACTORS=0` lo apaga. **Hipótesis secundaria refutada para AC Nexus**: usa `ovrp_EnqueueSubmitLayer2`, que no lleva factores; el desensamblado de la 1.5.x del plugin rellena el submit interno con has_blend=0, src=2, dst=3 (SrcAlpha/OneMinusSrcAlpha = el blend actual). El cambio solo afecta a guests que envían por `EndFrame4` con la cabecera nueva.
- Aviso de ABI sin tocar: el DWARF de `libOculusXRPlugin` da `ovrpLayerSubmitFlag_HeadLocked=1, Octilinear=2, ReverseZ=4, NoDepth=8, InverseAlpha=16`, mientras `KLOVRP_SUBMIT_HEAD_LOCKED = 0x2` y el comentario dicen bit 1 = HeadLocked y bit 0 = NoDepth «en todos los OVRPlugin del corpus». Comprobar cuál es cierta para AC antes de fiarse de `head_locked`.
- Diagnóstico para la próxima partida (todo con tope): `[ctrl] vel hand L|R:` a 2 Hz con |v|, |av| publicados, pico de 0,5 s y lecturas (`klovrp_vel_diag`, tope 4000; `KL_OVRP_VEL_DIAG=0`); `[ctrl] vel swing #N` por golpe (|v|>1,5 m/s hasta <1,0; tope 300). `[unity-kw] P/tag: texto` copia de las líneas de logcat con checkpoint/respawn/death/die/fail/mission/tutorial/combat al inicio de palabra (INFO+ o tag Unity; `kl_log_keyword_copy` en `kl_va_handlers.c`, llamado desde `__android_log_print` y `__android_log_write`; `KL_LOG_KW_MAX`, 1500). Con logger propio del guest (`__android_log_set_logger`), `kl_shim.c` `kl_android_log_write` copia ahora siempre a nuestro log las E/W/I (`KL_LOG_CUSTOM_MAX`, 3000); `__android_log_print` ya imprimía siempre. `[prov] overlay layer N: last 600 published frames: A with content written … B without` (`klovrp_overlay_content_stats`; usa `kl_vulkan_layer_write_events`, solo escrituras a la imagen propia de la capa: una capa dibujada desde el texL del juego no cuenta). `[ctrl] menu transition #N: overlays A -> B, last Connected handed out=0x…, input calls since the last transition: …` (cuando el nº de capas no-ojo pasa de/a 0, también por la ruta `Enqueue`) y `[mprobe fN] OVRInput at menu transition … connected= active=` (sin `KL_PROBE_INPUT`, tras `KL_PROBE_FROM`, tope 60). Hallazgo previo: `OVRInput.connected=0x0` pero `GetControllerState4` devuelve `Connected=0x3` y nadie llama a `ovrp_GetConnectedControllers*`.
- `vpvk_flush_caches`: solo se llamaba en la salida `_exit` de `immersiveEnded`, de forma síncrona en el hilo principal. Ahora `Lifecycle.flushEngineCaches(then:)` (`KleptonApp.swift`) lo ejecuta en un hilo utility y se llama: al terminar el espacio inmersivo con el lanzador reabierto, al pasar a background con el espacio abierto, y antes de las dos salidas (`_exit(0)` / `exit(0)`, que ahora se hacen desde ese hilo tras el guardado). El motor ya no guarda el archivo de pipelines en cada compilación.

**JNI / media**
- `kl_jni.c`: GetPrimitiveArrayCritical/Release, GetStringCritical/Region, GetStringUTFRegion, MonitorEnter/Exit (mutex recursivo), GetObjectRefType, GetSuperclass, IsAssignableFrom, DefineClass, AllocObject, ToReflected*, CallNonvirtual<T>MethodV, Get/Set campos Byte/Char/Short/Double + SetStatic*. `klj_unimpl` imprime hilo e imagen+offset del llamador.
- `kl_va_thunks.S` + `kl_jni.c`: 10 `CallNonvirtual<T>Method` variádicos. Ya no queda ningún slot JNIEnv >3 en `klj_unimpl`.
- `kl_jni.c`: `klj_decode_args`/`_a` tratan todo `[…` como referencia; `klj_as_object` (<0x10000 → NULL, log tope 20); `klj_arr_data(a, need)` NULL-safe, aplicado en `jni/kl_jni_{net,hostvideo,display,softinput}.c`.
- `media/kl_mediandk.c`: `take_rgba` convierte formatos no 32BGRA con `VTPixelTransferSession` (buffer BGRA reutilizado, liberado en `AImageReader_delete`/abandon); `klm_cpu_announce` emite INFO_OUTPUT_FORMAT_CHANGED también en modo superficie (retiene primer frame `held_pb`, devuelve -2, `kl_media_surface_set_size`); `getOutputFormat` en superficie con width/height/crop/rotation/color-format 0x7F000789; `AMediaCodec_stop` libera `held_pb`.
- `xr/kl_ovrplat.c`: toda la familia `ovr_IAP_GetViewerPurchases*` + `GetNextPurchaseArrayPage` → fallo OFFLINE (request id + completion IsError=1; DeltaAsync sin tipo conocido → request 0).
- `kl_fault.c` + `VQLog.swift`: cabecera `== klepton-crash: build <KL_BUILD_COMMIT>, pid, session start ==`; `archiveStaleCrashLogs()` mueve crashes viejos a `anteriores/` (10 por carpeta), llamado desde `VQLauncher.play`; MetricKit a `Registros/metrickit-<ts>.json`; `shareable` incluye `crashAttachments`.

**Fin de frame / compositor**
- `gfx/kl_vulkan.c`: fin de frame segmentado (anillo de 4 fences, `vkQueueSubmit` vacío por frame, serial al completarse en GPU, `klvk_frame_fmax`), `kl_vulkan_frame_pending_serial()`; `xr/kl_ovrp.c` publicación diferida (`pubq_*`, `klovrp_pub_poll`). **APAGADO por defecto** (`KL_VK_FRAME_ASYNC=1` lo activa). Ruta async antigua y cmdbuf nativo eliminados.
- `KleptonCompositor.swift`: frame congelado (`staleHide`, `KL_CP_STALE_HIDE_S`=3,0 s) → backfill; temporización por pase (`MTLCounterSampleBuffer`, `KL_CP_PASSTIME=0` lo apaga); backfill negro opaco (z 0,0001) en todo frame sin passthrough; hilo `.userInteractive`; `[cp] slow frame`; `[cp] slack`; GPU lost → ojo negro + cierre.
- `kl_reproject.c`: RCAS fusionado en `kl_reproject_f`, salta las 4 taps donde la máscara UI vale 1.
- RCAS 0–100 % global/por juego (`KL_RCAS`/`VPVK_RCAS`) en todos los modos de AA. VPKTAA nativo único (Vulkan y vpgles, ovrp y OpenXR). Foveado propio cableado a VPKEngine (M5 solo en M5).
- Frecuencia efectiva (`kl_ovrp_guest_display_frequency()`) leída por Display/OpenXR/Choreographer/looper/ue4: `jni/kl_jni_display.c`, `xr/kl_openxr.c`, `guest/kl_android.c`, `jni/kl_jni_looper.c`.

**Resolución** (`xr/kl_ovrp_eyesize.h`, `kl_ovrp.c`, `kl_openxr.c`, `VQLaunch.swift`, `VQLauncher.swift`)
- Funciones puras `kles_*`; `KL_OVRP_EYE_BASE=quest3` + `KL_OVRP_EYE_SCALE`=%/100; `VQLaunch` fija render_scale = 1 (`929fe4f`).
- Ensanche del plugin aprendido en `klovrp_SetupLayer` (`g_widen_w/h`); OpenXR aprende la escala del guest (`g_xr_game_scale`).
- `VQSettings.resolutionPct` (`vq.resPct`) y `VQGameRes` por juego; `VQResolutionSlider`; migración de claves antiguas.

**Memoria / diagnóstico**
- `kl_libc.c`: aviso de memoria decidido en segundo plano y entregado en el hilo del juego; `kl_mem_total` 8192 MiB (`KL_MEM_TOTAL_MB`), avisos 65 %/80 % (histéresis <70 %); presión por pendiente cada 100 ms (avail/pendiente < ~6 s → trim propio + `kl_vulkan_trim` + `vpvk_trim_memory`); `/proc/self/maps` sintético; volcado IOAccelerator; `host_statistics64`; estadística por fichero.
- `kl_vulkan.c`: agregado de `vkCreateImage`, `kl_vulkan_diag`, `kl_vulkan_idle_drain`; `kl_sample.c` hilos calientes; `kl_app.c` latidos `[hb]`/`[hbm]`/`[vkagg]`/`[mtl]`, sonda de entitlements.
- `KleptonTAA.swift`: anillo de salida 4 slots.
- `VQLaunch.swift`: `translatorTag()` (`vpvk_translator_tag`, recortado a 32) en los sellos `firstrun.v<N>.<tag>` y `aot.<tag>.r<recetas>`; la primera ejecución se rehace una vez tras instalar.

**Otros**: háptica por mano CoreHaptics autodetectada (`KleptonControllers.swift`, `kl_ovrp_input.c`); ALooper con wake interno distinto del real (`kl_ndk.c`); sensores derivados de la pose de cabeza; AInputQueue real; vídeo con SurfaceTexture real drenado en el hilo del juego; GL ausente → stub que devuelve 0 (`kl_egl.c`).

**Tests de host** (todos en `tools/ci/host-tests.sh`): `t_frame_fence`, `t_jni_sig`, `t_memdiag`, `t_media_announce`, `t_ptw`, `kl_ovrp_eyesize_test`, `pred_frame_offset_test`, `t_ovrp_input`, `spatial_test`, y VPKEngine `gpu/` (cmake+ctest, `gpu/metal2` para xros).
- **Registros por sesión (2026-10-20, `app/Sources/VQLog.swift`, sin compilar/probar en visor)**: causa de que el zip no trajera AC: `shareable` solo daba app.txt + UN app-anterior.txt + UN klepton-boot.log, y `start()` rota app.txt en CADA arranque; tras la sesión de AC (pid 2167, 12:43:57, MetricKit CPU 12:44-12:46) hubo dos arranques sin juego (13:06:51, 13:09:36) que empujaron su app.txt fuera de app-anterior. Arreglo: `archivePreviousSession()` (en `start()`, antes de rotar) copia app.txt (+ klepton-boot.log y klepton-crash.log escritos después del inicio de ese proceso) a `Documents/Registros/sesiones/<fecha>_<juego>_<commit>__{app.txt,klepton-boot.log,klepton-crash.log}` solo si hubo `Arranque:`; recorta >6 MB (2 MB cabeza + 4 MB cola; el completo sigue en Documents/QUEST); conserva 8, comparte las 6 últimas desde una caché (`refreshSessionFiles`), sin escanear en cada render. `shareable` omite el boot/crash vivo si su copia archivada va en la lista; `saveToQuest` usa `liveFiles` (completos). Etiqueta de `savePreviousSessionToQuest` lleva juego_commit. Pendiente: confirmar en visor; la copia a QUEST de arranque sigue a los 10 s (si la app sale antes, la hace `play()` del siguiente juego).

**OpenXR LOCAL / vídeo / passthrough (20 oct, sin build ni verificar en visor)**
- `xr/kl_openxr.c` `klxr_local_origin_y`: LOCAL = tracking space elevado por la altura medida de la cabeza (`kl_ovrp_measured_eye_height_or0`, nuevo en `kl_ovrp.c/.h`; 0 hasta medirse); se latchea la primera vez y al cambiar >=10 mm (recenter/postura) emite `XrEventDataReferenceSpaceChangePending` (tipo 40; tambien en el primer latch si ya se sirvio LOCAL con 0). STAGE/LOCAL_FLOOR/VIEW sin cambios; un guest que fijó origen ovrp conserva `kl_ovrp_eye_height()`. `KL_XR_LOCAL_LEGACY=1` = LOCAL==STAGE. Autotest `kl_openxr_space_selftest` ajustado.
- Passthrough: `xrGetSystemProperties` rellena `XrSystemPassthroughPropertiesFB` (1000118000, supports=1) y `...Properties2FB` (1000118006, capabilities=0x3) en `kl_openxr_spatial_system_props`; `XR_FB_passthrough` activa `ext_sysprops`. Pendiente: confirmar en Demeo que Unity ya no dice "doesn't support XR_FB_passthrough" y sale `[pt] passthrough ON`.
- Traza `[xr-ctrl]` 1 Hz (60 s, `KL_XR_AIMGRIP_TRACE[_S]`): pose publicada (Apple .grip + Sense) vs grip/aim entregados, pitch añadido, separación aim-grip. Solo diagnóstico.
- Vídeo: contrapresión en `AMediaCodec_dequeueInputBuffer` (`kl_mediandk.c`, `klm_bp_*`): TRY_AGAIN mientras en_VT + ring + slots + held >= `KL_MEDIA_INFLIGHT` (8); tras 3 s sin progreso admite hasta 12 (< ring 16). `kl_vtdec_flow/finish` (nuevo); EOS hace FinishDelayedFrames y solo entrega el buffer EOS con VT vacío. `[media] vstat` 1/s (hilo por códec) con contadores de vtdec/slots/reader/take_rgba/GL (`kl_jni_hostvideo.c` registra su texto); topes de 3 de `take_rgba` y `[video] tex<-frame` eliminados. Test `t_media_backpressure` (`make mediabp`, en host-tests.sh).
- `kl_jni_hostvideo.c`: fallback EXTERNAL_OES->2D ahora libera el nombre (`glDeleteTextures`) antes de bindearlo como 2D. El `INVALID_OPERATION (BindTexture) 0xDE1` del log 71 NO venía de nuestro fallback (no hay línea "falling back"): es Unity bindeando 2D un nombre que subimos como EXTERNAL.
- Pendiente: `kl_mediandk_report` antes de los `_exit` (`klb__exit` en `kl_libc.c`, `KleptonApp.swift:171`), fuera de los ficheros asignados.

## 5. Decisiones de diseño vigentes

- Todo global, sin condiciones por juego (muletas de AC eliminadas). Excepción: ajustes por juego del usuario (AA, resolución, anisótropo, RCAS).
- Solo offline: `KL_NET_OFFLINE`=1 (loopback y AF_UNIX), `KL_PLAT_TOKEN_ERROR`=1 (AC no entra en UbiServices; su aviso es esperado), usuario Meta offline. Sin cuentas ni servicios.
- IAP/compras: solo fallo OFFLINE honesto, nunca compra/entitlement; el resto del DRM (ConsumePurchase, LaunchCheckoutFlow, Entitlement salvo `GetIsViewerEntitled`, AssetFile Download*) sigue abortando.
- MSAA del juego mantenido por defecto; modo Automático: VPKTAA solo si no se detecta MSAA. VPKTAA = nativo de VPKEngine (sin cobertura en ANGLE/MoltenVK).
- `nativeLowMemory` solo en el hilo del juego (AoT/IL2CPP cae sin x18; un attach JNI no lo arregla); no desde `kl_jni_drain_ui_tasks` (reentrante). Con el juego bloqueado solo recorta `kl_vulkan_trim`.
- `ovrp_GetEyeLayerRecommendedResolution` → -1004 sin capa de ojo viva (si no, Unity usa resolución dinámica con viewport reducido y deforma el 3D de AC).
- Slider de resolución = tamaño FINAL del ojo: 100 % = Quest 3 (2064x2208, 4,56 Mpx), 200 % = densidad del Vision Pro (3660x3200, 11,7 Mpx), 50–200 %, tope 4096. El usuario no quiere tocar el tamaño de imagen salvo con ese slider (render_scale = 1).
- Pipelining de fin de frame apagado por defecto: no da fps en juegos limitados por GPU y causa judder.
- Calidad del compositor 1.0 elegida por el usuario: no tocar.
- No hay API pública en visionOS 26.5 (CompositorServices/RealityKit) para elegir 90/96/100 Hz (solo `minimumFrameRepeatCount` como divisor y `maxRenderQuality`): controlador dinámico de panel descartado; no decir al juego 90 con el panel a 100 (judder). Ritmo encajado a 120/100/96/90 (`KL_CP_PANEL_SNAP=0`); la predicción de pose OVRP usa el panel a propósito.
- GLES ES 3.1, no 3.2. Identidad Quest 3 coherente (`KL_QUEST3`: OVRPlugin headset 11, OpenXR systemName, JNI SDK 32, `device_name` "Meta Quest 3"); mandos como Touch de Quest 2; eye tracking Unsupported.
- Háptica autodetectada por mando, sin ajuste de intensidad.
- Foveado de Quest siempre apagado; foveado propio opt-in (apagado por defecto), M5 solo en M5, exige VPKTAA nativo, declina con MSAA.
- ASW desactivable (activo por defecto; `KL_CP_ASW`/`KL_CP_PTW` por variable).
- Frame congelado: umbral 3,0 s (con 1,0 s las cargas de 1–3 s y los juegos <1 fps parpadeaban).
- Valores mágicos: burst de audio 1024 con Wwise (256 da «Voice Starvation»); `kMaxArchiveFiles` 512 (AC genera 4101 PSO; con 64 evicta y recompila cada arranque); encoders >2048 draws partidos en trozos de 256 (`VPVK_MAX_DRAWS_PER_CB`); anillo TAA 4 (con 3 un pinned antiguo deja sin slot; si sale `no free output`, volver a 5); máscara UI R8 en slot 7; quads de capas en orden de envío (`KL_OVERLAY_SUBMIT_ORDER`=1; sin él no aparece el panel de calibración de AC); profundidad `.store` obligatoria en el composite (visionOS reproyecta con ella); `KL_EXIT_ON_BACKGROUND` (hoy background → `_exit(0)`).
- Advertencias de hipótesis refutadas: H0 de memoria (VkDeviceMemory host-visible envuelto en MTLBuffer noCopy) NO es la causa del salto de AC; alfa 0 del backfill NO explica «se ve la habitación»; el vídeo de Demeo NO era EXTERNAL_OES; `native_stuck` venía del cmdbuf nativo (`encodeNative`), no del solape.
- Datos de juegos (APK, logs, assets) nunca en el repo. Pruebas solo con juegos propios del usuario.

## 6. Diagnóstico

Logs en `QUEST/Registros/` (compartibles desde la app); línea 1 = hash de build. **Log de CI**: rama `ci-logs`, fichero `visionos.txt`, por `git fetch origin ci-logs` o API `repos/<owner>/<repo>/contents/visionos.txt?ref=ci-logs` (primero errores, luego las últimas 150 líneas). `gh run view --log` da 403. `compositor-api.txt` va embebido en `visionos.txt`.

| Etiqueta | Significado |
|---|---|
| `[hb]`, `[hb] jump +N MiB`, `[hb] blocked` | Heartbeat con breakdown de memoria (ledger gráfico/media/net/purg, fp_peak, lim_rem); salto >200 MiB/tick; hilo bloqueado |
| `[hbm]`, `[vkagg]`, `[mtl]` | Cadencia rápida tras el salto de memoria: ledger por regiones, agregado de `vkCreateImage` (por formato/mips), estado de Metal (`g_held`, cementerio) |
| `[mtl] cut_enc` / `chunk_wait` | VPKEngine: corte de encoders por presupuesto y espera de contrapresión (muro de encoders de AC) |
| `[mem] threshold`, `[mem] OS pressure`, `[mem] trim (…)` | Aviso 65/80 %, presión del SO, recorte (`VPKEngine released N MiB`) |
| `msaa_released`, `tile_rejected>0` | MSAA del juego liberado; con `tile_rejected>0` el MSAA 4x no cabe |
| `gpu.bigframe` | VPKEngine: frame con trabajo GPU anómalo (usar con `gpu.enc ls=`) |
| `gpu.enc ls=` | VPKEngine: encoders por frame y último pase/largo de encoder |
| `gpu.stall_warn` | GPU sin progreso (precursor de `gpu.hang`) |
| `gpu.cbtime` | Tiempo GPU por command buffer |
| `gpu.sec`, `gpu.slowcb`, `gpu.hang`, `gpu lost` | Tiempos por segundo; GPU lost → ojo negro y cierre con mensaje |
| `vk.boot_summary`, `vk.pipeline_skip`, `vk.pipeline_wait what=…` | Arranque de VPVulkan; pipelines asíncronos omitidos/esperados |
| `vk.zeroinit`, `gpu.shader.msl cached=`, `gpu.prewarm ar_hits/ar_misses`, `vk.taa native gpu_ms`, `vk.caps`, `vk.err` | Otros de VPKEngine |
| `[frame-end] mode=pipelined|sync|resolved …` (cada 600 frames) y `[frame-end] fence wait … waited N ms` (>500 ms) | Estado del fin de frame: allowance, in-flight, esperas, latencia submit→completion; `[ovrp] frame end waited` = espera del hilo de render (≈0–3 ms si va segmentado) |
| `[cp] gpu p50/p95/max … (N cmdbufs, rcas X) | pass frag/vert … rt WxH` | Tiempo GPU del compositor y por pase (frag << cmdbuf = solape con el juego); `[cp] pass timing`, `per-pass GPU timing unavailable` |
| `[cp] slack min/mean A/B ms fps_guest N` | Holgura de plazo tras commit (negativo = fuera de plazo) y fps reales del juego |
| `[cp] slow frame: … (poses+publish, drawables+encode, commit)` | Present retrasado y dónde (máx. 40) |
| `[cp] stale -> backfill: …`, `[cp] fresh again: …` | Frame congelado >3 s / recuperación |
| `[pt] immersion style requested: .full|.mixed` | Estilo de inmersión pedido (`.mixed` en AC = causa de «se ve la habitación») |
| `[pt] mode auto`, `[pt] passthrough ON` | Passthrough por frame (`kl_passthrough_note_frame`) |
| `[cp] HALF RATE …`, `minimumFrameRepeatCount` | Medio ritmo impuesto por el sistema / recuperación |
| `[cp] layer paused/invalidated`, `[cp] backfill`, `eye stage N … held back/published`, `[zero] compositor leyó imagen pendiente`, `[prov] layer N … no write seen` | Ciclo de capas/ojos y procedencia |
| `[cp] RCAS fused: UI mask … in use` / `no UI mask for eye N` / `UI mask WxH is not the eye texture's`; `[cp] composite pipeline failed` | Máscara UI / fallo del MSL |
| `[taa-inline] eye AxB -> CxD: output ring rebuilt`, `no free output` | Anillo TAA |
| `[ovrp] eye desc … for a final`, `plugin widening`, `[xr] swapchain … is …x the recommended` | Resolución: tamaño final, ensanche aprendido, escala OpenXR |
| `[media] take_rgba #N: 'fourcc' WxH via … (OSStatus), xfer ms, total ms` | Conversión de vídeo (3 primeras y 16 primeros fallos); `[media] surface codec: INFO_OUTPUT_FORMAT_CHANGED` antes del primer frame |
| `[vtrace tid=…]` (`KLM_VTRACE_MAX`), `NO-OP game Java void` | Traza JNI/vídeo, llamadas Java sin emulación |
| `[jni] klj_as_object: handle … (caller …)` | Handle inválido (tope 20); `klj_unimpl` imprime hilo y caller |
| `== klepton-crash: build <commit>, pid, session start ==` | Cabecera del crash; los anteriores van a `Registros/anteriores/` |
| `[snap] game thread silent` | Hilo del juego sin actividad a 10/30 s (esperas por hilo) |
| `[cp] haptics: hand N engine up …`, `FIRST REAL PULSE`, `SetControllerHapticsAmplitudeEnvelope … (struct order …)` | Háptica; si el orden sale `Count,Duration`, corregir; `KL_HAPTICS_LOCALITY=all|handles`, `KL_HAPTICS_FLOOR`, `KL_HAPTICS_MIN` |
| `gles.diag`/`gles.warn`, `[gles] ruta:`, `[egl] absent GL function called, returning 0: NAME` | Errores GL del guest, ruta elegida, funciones GL ausentes |
| `[magenta]` | Shader de error de Unity (siempre calienta uno) |
| `[ctrl] vel hand`, `[ctrl] vel swing`, `[ctrl] menu transition`, `[mprobe fN] OVRInput at menu transition` | Velocidad de mandos publicada al juego, golpes (>1,5 m/s), llamadas de input y `connected` de OVRInput al abrir/cerrar menús |
| `[unity-kw]`, `[prov] overlay layer N: last 600 …` | Líneas de logcat de estado de partida; frames con/sin escritura por capa overlay |
| `[ovrp] layer N blend:`, `[vk] eye image … vpvk_mark_eye_image`, `vk.eye_image`, `vk.pipeline_skip … skipped_by_dest{…}` | Blend por capa; marca de imágenes de ojo; clase de destino de los draws saltados |

## 7. Pendientes y riesgos abiertos (por prioridad)

1. **Verificar en visor** el lote completo. Primero: freno de encoders de AC (`[mtl] cut_enc/chunk_wait`, `[hb]`, ¿sigue el jetsam?), cuadro gris de Demeo (`INFO_OUTPUT_FORMAT_CHANGED`, `take_rgba #1`, que no vuelva el abort en slot 222 ni `klj_arr` en el crash), slider de resolución (`for a final`), fps de AC/AoT (`[cp] gpu`, `gpu.sec`).
2. Causa de «se ve la habitación» en AC: mirar `[pt] immersion style requested` y `[cp] slow frame` en la congelación.
3. Pico de memoria de AC en carga (~1,8 GB/s sin atribuir): dueño de las ~39k regiones sin identificar (candidatos: mips ASTC 8x8 de 512², cmdbufs planificados tarde, compresor). Opciones: etapas del ojo 3→2, historial TAA, prewarm sin owner; evaluar entitlement increased-memory-limit.
4. Hazard de copias por rect en VPKEngine (`VPVK_COPY_RECT_HAZARD`, apagado).
5. Batman: corte Feedback por profundidad MSAA muestreada (847 MB `tex_msaa`).
6. Coste del compositor (4–6 ms/present a 100 Hz); mirar `pass frag` vs cmdbuf.
7. `ovr_IAP_GetViewerPurchasesDeltaAsync` devuelve request 0 (sin tipo de mensaje conocido en SDK v74).
8. `klj_arr(x)->data` sin protección en `jni/kl_jni_{android,bridge,services,ue4,window}.c`: migrar a `klj_arr_data`.
9. La primera capa de AC sale hasta ~25 % más ancha que lo elegido hasta que el juego la reconstruye; en OpenXR la primera swapchain conserva el escalado del juego. Persistir por juego el ensanche/escala aprendidos y empujarlos antes de arrancar. El 200 % (≈3821x3065 × ensanche) pesa mucho en memoria.
10. Fin de frame segmentado (si se reactiva): con S=3 stages el compositor puede leer el stage anterior mientras la GPU escribe j+1 (`KL_OVRP_STAGES=4` lo cierra); el submit vacío incrementa `d->submits`; `KL_OVRP_PUBLISH_AFTER_GPU=0` publica antes de tiempo; shadow copy de overlays hasta 1 frame por delante.
11. Pausa con la Corona: hoy background → `.invalidated` → `kl_app_guest_stop` → `_exit(0)`; `ovrp_GetAppHasInputFocus` siempre 1. Diseño: no parar el guest, foco perdido al juego (ovrp InputFocus/VrFocus, estados OpenXR, onPause/onResume), cortar envíos GPU, «Reanudar». Primer paso: experimento `KL_EXIT_ON_BACKGROUND=0`.
12. `onInputQueueCreated`: `kl_ndk_input_queue` sin llamador (`kl_nativeactivity.c`). Verificar firma/orden de `nativeHandleSensorEvents` en UE4. Callbacks de ALooper: se ignora su retorno. Sensores: mezcla de pose en vivo con velocidad del frame fijado.
13. TAA/RCAS: HDR saturate; lag de 1 frame de la máscara UI; guests ANGLE/sin VPKEngine sin máscara; ojo foveado de otro tamaño sin máscara; EDS × máscara UI no combinan. Foveado propio sin probar (M2/M5; `vpvk_config_set_foveation_eyes`; mapa identidad 16384², coste fold/expand sin medir).
14. Demeo en AR: política ARKit por manifiesto (escena con `USE_SCENE`, anclas con `USE_ANCHOR_API`); extensiones de escena/planos/profundidad se evalúan una vez en el primer enumerate y exigen ARKit autorizado; bind de manos y eventos discovery 104/105 sin verificar.
15. OVRPlugin: flags SpaceWarp y orden de LatencyTimings sin contrastar (usar `libOVRPlugin.so` de un APK propio); fov de frustums v0 asumido en grados; rechazadas `SetControllerHapticsParametric`, `GetControllerParametricProperties`, `TriggerVibrationAction`.
16. Carátulas: los APK solo traen `app_icon` y `vr_splash`; `VQCover.swift` usa carpeta → OculusDB (`oculusdb.rui2015.me/api/v1/search/<package>`, no verificado byte a byte) → `vr_splash` → icono; marcadores de caché v4.
17. GLES (vpgles): sin transform feedback, SSBO/imágenes/atómicos, geometry/tess, program binary; occlusion query = 1; `gl_PointCoord` fijo; detección de MSAA solo en Vulkan. Android: AObb sin montaje real; AImage solo YUV_420_888; OpenSL sin efectos/3D/MIDI; posix_spawn/exec/SysV ENOSYS; inotify sin IN_ACCESS/OPEN/CLOSE; sin acel. lineal ni magnetómetro. Sin validar en Darwin: `kl_bionic_extra2/4.c` (campos de FILE) y thunks `.S`.
18. VPKEngine: salida de fragmento int/snorm y blits 1D sin auditar; ycbcr sin 10/16 bit; no viable en Metal: variablePointers completo, LogicOp/PatchControlPoints dinámicos, availability/visibility, BDA capture/replay; hilo que muere sin soltar su contexto GL.
19. Conformidad: referencia 96,9 % de 6551 casos (antes del perfil 1.3). Falta dEQP-GLES (gles2/3/31, runner EGL) y grupos CTS tras `VPVK_EXTRA_EXTENSIONS=1`.
20. Futuro: SSAO propio en VPKEngine (media resolución, por juego, apagado). Limpieza: sondeo de jitter de frustum inerte en `kl_ovrp.c`; avisos `%llu` en `kl_egl.c`.

## 8. Cómo compilar, probar y publicar

- **Build**: el mensaje del commit debe llevar `[build]` (o lanzar `visionos` a mano) → IPA en el release `visionos`. Solo una build completa tras revisar; no lanzar builds por cambios sueltos. Al entregarla, dar el enlace de descarga.
- **VPKEngine** se clona de `main` en CI (sparse `runtime` + `gpu`): pushear VPKEngine ANTES de lanzar la build. La caché de VPVulkan se invalida por hash de `vpkengine/gpu/**`.
- **Gate**: falla si `syntax-xros.sh` (`-fsyntax-only` de todo `RUNTIME_SHIP` con SDK xros) o `host-tests.sh` (todos los tests de host de la sec. 4, con ASan/UBSan; `gpu_host` con `-k` para listar todos los errores) fallan. Con Gate rojo no hay IPA. Si macOS falla: `lgamma_r`/`lgammaf_r` llevan prototipos explícitos (math.h de Darwin los oculta tras `_REENTRANT`).
- **Local (Linux, sin Mac)**: `clang -fsyntax-only` con `XROS_CFLAGS` solo sirve para ficheros sin cabeceras Darwin; usar un shim de Darwin (stubs de CommonCrypto/CoreVideo/pthread) y distinguir errores previos del shim. Swift/.mm solo compilan en CI. `tools/ci/host-tests.sh` no corre completo en este Linux (falta `/gpu` y `vulkan.h`); tests sueltos con `make jnisig|memdiag|mediaannounce|eyesize` y `CC=gcc`.
- **Tras un fallo**: leer `visionos.txt` en `ci-logs` (sec. 6).
- **Identidad de commit**: `Claude <noreply@anthropic.com>`. Ningún dato personal ni owner en el repo.
- **Bitácora**: actualizar tras cada función, bug o build.

- OpenXR: la swapchain de color de la capa de proyeccion se marca como imagen de ojo (kl_vulkan_xr_image_mark_eye -> vpvk_mark_eye_image). Fallback 2D de hostvideo: solo borra el nombre del guest si el bind 2D falla. EOS de vtdec: FinishDelayedFrames + WaitForAsynchronousFrames. Pendiente en visor: Steam Link/CTS con LOCAL elevado; samplerExternalOES con hostvideo.

- Build en verde: run CI 39419833988 (commit 4b29253, motor ed942c7), IPA en el release `visionos`. Sin probar en visor.

- Demeo (run 39419833988, mandos/manos): [xr-ctrl] en reposo, con manos (sin joints OpenXR: el tracker se rechaza y Demeo ve mandos Touch + avatares de mano por la pose grip) dio grip publicado 38/34° y aim entregado 38/34° sobre X (+37/-37 netos = 0), es decir rayos 35-40° HACIA ARRIBA desde manos a 23 cm bajo los ojos; origen aim = grip (0 mm). El par +37/-37 es de Sense (+37 deshace el hiltPitch -37 que el frontend mete a Sense; -37 ≈ grip-a-aim propio -34,8), no de manos. Cambio: `klxr_hand_aim` (kl_openxr.c) usa para el AIM de una mano con joints publicados el rayo hombro→pellizco (`kl_ovrp_hand_aim_pose` en kl_ovrp_input.c, misma lógica que `PointerPose` de OVRPlugin, `hand_ray` compartido); grip y Sense intactos; `KL_XR_AIM_HANDS=0` restaura el aim antiguo; el panel (`g_tune_live`) sigue mandando. Test: `t_ovrp_input` (aim hacia delante y abajo, origen = pellizco, sin joints = 0).
- Passthrough en Demeo (mismo log): `xrCreatePassthroughFB/LayerFB` OK, `[pt] passthrough ON (layer 0, underlay, premultiplied alpha)`, `.mixed` pedido y el compositor conserva el alfa del ojo (`kl_chroma_apply` bit 2, clear alfa 0) → fondo negro = alfa 1 en el ojo del juego; causa NO demostrable con este log (no se registraba blend ni flags). Añadido `[pt] xrEndFrame: environment blend mode … layer N: type flags (blend-texture-source-alpha, unpremultiplied) alpha-blend-FB src/dst` al cambiar (`klxr_endframe_layers_diag`). Pendiente: con ese log decidir si Demeo pide `BLEND_TEXTURE_SOURCE_ALPHA`/`XrCompositionLayerAlphaBlendFB` y qué alfa escribe.
