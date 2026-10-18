# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-18; detalle de cada paso en los commits
«Integrar paso-aN» y en las ramas `paso-aN` de GitHub).

## EMPEZAR AQUÍ — 2026-10-19: lote nocturno sin probar (build lanzada)
Estado: cambios en el árbol de trabajo (25 ficheros, +1017/-48) sobre fc2f217; VPKEngine con cambios propios (11 ficheros, +770/-64). Nada verificado en el visor.

### Cambios (visionquestpro)
- Orden de envío de quads: ya en 5cf8922 (`KleptonCompositor.swift`, `KL_OVERLAY_SUBMIT_ORDER` def. 1).
- HUD en las ramas del loading skybox: `KleptonCompositor.swift`.
- Fondo negro `drawBlackBackfill` cuando no hay ojo ni vídeo y no hay passthrough: `KleptonCompositor.swift`.
- Ojo reasignado retenido hasta su primera escritura (pases/copias/clears); tope 3 s; la textura vieja no se libera antes de 4 s: `KleptonCompositor.swift`.
- Logs de pausa/invalidación del LayerRenderer (`[cp] layer paused/invalidated`): `KleptonCompositor.swift`, `KleptonApp.swift`.
- Procedencia ampliada (todos los adjuntos/resolve, dynamic rendering): `gfx/kl_vulkan.c`.
- APIs gráficas por manifiesto (`android.hardware.vulkan.version`, `glEsVersion`); `-force-vulkan` solo si el juego trae Vulkan: `VQGames.swift`, `VQLaunch.swift`, `VQLauncher.swift` (aviso «APIs del juego»).
- Cierre limpio del motor (UnityPlayer.kill / killProcess / System.exit vía JNI, mensaje en el launcher, cierre robusto del espacio): `jni/kl_jni_sysinfo.c`, `kl_jni.h`, `KleptonApp.swift`, `kl_app.c/h`.
- Diagnóstico de bloqueos: `[snap] game thread silent` a 10/30 s (`diag/kl_sample.c/h`); registro de esperas por hilo `kl_wait.h` (`libc/kl_pthread.c`, `kl_libc.c`, `kl_shim.c`, `kl_va_handlers.c`, `kl_bionic_extra2.c`); peticiones Oculus pendientes (`xr/kl_ovrplat.c/h`, `kl_ovrp.c`, `kl_openxr.c`); `[gles] ruta:` y log EGL/GL (`gfx/kl_gles.c/h`, `kl_egl.c`).
- Carátulas de tienda: ya en fc2f217.

### Cambios (VPKEngine, /home/claude/vpkengine, sin commit)
- Memoria nueva a cero perezosa (`vk.zeroinit`): `gpu/vulkan/src/entry.cpp`, `metal_backend.mm`.
- MSL reutilizado con receta (registro 'F' + id del traductor; log `gpu.shader.msl cached=`): `recipes.cpp/h`, `pipelines.cpp`.
- Corte entre encoders cada 256 draws (`VPVK_MAX_DRAWS_PER_CB`): `commands.cpp/h`.
- `gpu.slowcb` si CB > 8 ms; cola Metal de 256 CB: `metal_backend.mm`.
- Tests: `gpu/vulkan/tests/vk_tests.cpp`.

### Causas medidas
- Cuadrados rosas: capas OVROverlay copiadas de imágenes nunca escritas (memoria Metal reciclada; Android entrega ceros) → zero-init perezoso + ojo retenido.
- Corte del inmersivo en AC: ShaderWarmup (~5 s retraduciendo 3392 shaders con receta + frame de 199-543 ms de GPU) → MSL reutilizado, corte cada 256 draws.
- Demeo: `-force-vulkan` sin Vulkan en el build + kill no-op → SIGTRAP → APIs por manifiesto + cierre limpio.
- TRIANGLE STRATEGY (primer juego GLES): hilo de juego de Unreal bloqueado en carga de nivel tras destruir el passthrough; causa por confirmar con `[snap]`. El cambio mixed→full funciona.

### Qué mirar en el siguiente log
`vk.zeroinit`; ausencia de capas magenta; `gpu.shader.msl cached=`; `gpu.slowcb`; `[cp] backfill`; `eye stage N … held back/published`; `[cp] layer paused/invalidated`; `[gles] ruta:`; `[snap] game thread silent`; «APIs del juego».

### Pendiente para la siguiente build
- Pausa con la Corona. Hoy: background → LayerRenderer `.invalidated` en ~5 ms → `kl_app_guest_stop` → `_exit(0)`; guest de una sola ejecución; `ovrp_GetAppHasInputFocus` siempre 1.
- Diseño: no parar el guest al invalidarse por Corona; foco perdido al juego (ovrp InputFocus/VrFocus, estados OpenXR, onPause/onResume por JNI); cortar envíos de GPU en pausa; launcher con «Reanudar» que reabre el espacio y reengancha un compositor nuevo a las texturas existentes.
- Primer paso: experimento con `KL_EXIT_ON_BACKGROUND=0` y `guest_stop` condicionado para ver si el proceso sobrevive.
- Otros: OVRInput `connected=0x0` en AC (no bloquea); regulador de GPU del guest; rosa/1 s en tutoriales 2D de AoT (debería ir con zero-init).

## 2026-10-18 (noche): primera prueba en el visor (AoT + AC) → 5 frentes; BUILD SUCCEEDED 3e7a30c (run 39156514064, VPKEngine c920e54), sin probar
Logs: `…-fin-Attack_on_Titan…` y `…-fin-Assassins_Creed_Nexus_VR` (19:55-20:03). Hallazgos y cambios:
1. **Audio AC roto** (514 «Voice Starvation», `[aaudio] burst 256`): `klaa_guest_wwise` (kl_aaudio.c) miraba
   `access(<g_native_lib_dir>/libAkSoundEngine.so)`, pero en la app `tree/lib` nunca tiene `.so` (VQPrepare extrae a
   `work/`, convierte a dylib y borra el ELF); antes lo tapaba `kl_is_ac_nexus`. Arreglo 861f148: `kl_find_image`
   (registro del loader, kl_dl.c), solo se cachea el 1 → burst 1024 para cualquier guest con Wwise.
2. **AC «pillado en el menú»** = aviso de UbiServices (esperado: `ovr_User_GetAccessToken` → error por
   `KL_PLAT_TOKEN_ERROR`, 0 intentos de red) que no se puede cerrar porque **AC no ve mandos**: `[mprobe] OVRInput:
   connected=0x0` toda la sesión (AoT `0x3`). AC: OVRPlugin 1.88, `ovrp_GetControllerState4` (mask 3 y 0xfffffff, nuestro
   relleno da Connected=3); AoT: 1.89, `GetControllerState6`. Causa sin confirmar; sospecha: identidad Quest 3 (headset 11)
   con OVRPlugin 1.88. Diagnóstico 861f148: `[ctrl]` primeras 24 llamadas por versión de GetControllerState*; logs de
   `ovrp_GetVersion*/GetNativeSDKVersion*`; mprobe añade `active= loadedXRDevice= ovrpVersion=` (loadedXRDevice
   probablemente `?`: suele ser campo, no propiedad).
3. **Carga larga «de Ubisoft»**: no es red (0 DNS/connect); es el vídeo `Ubisoft-Logo-With-Sound.mp4` mientras AC carga
   datos (19:55:36→19:56:35, memoria 1,7→4,9 GB).
4. **Rosa global**: `[magenta] fragment module … (1,0,1,1)` en AC (2 pipelines, pase de ojo 4x) y AoT (4, 2x) = shader de
   error de Unity. Hipótesis: `maxVertexOutputComponents/maxFragmentInputComponents` 124 < 128 (`interpolators32`).
   VPKEngine c920e54: 128 con `VPVK_REPORT_GEOM=1` (juegos), `vk.caps` volcado, `vk.err what=varyings`. Ajuste nuevo
   «Perfil Vulkan completo (experimental)» (`vq.vkFullProfile` → `VPVK_EXTRA_EXTENSIONS=1` en juegos) para A/B.
5. **50 Hz pegado** (AoT 20:02:35→fin): tras picos de GPU del juego (63 ms) y ventanas con 6-9 `past deadline`, el sistema
   presenta cada 20,02 ms con 0 retrasos (medio ritmo impuesto por visionOS; térmico «fair»). a707196 + revisión:
   `[cp] minimumFrameRepeatCount N at start` / `changed A -> B (by the system)`; `KL_CP_HALFRATE_RECOVER` (def. 1):
   6 ventanas con mediana 1,8-2,2 periodos y 0 retrasos → `minimumFrameRepeatCount = 0` (`[cp] HALF RATE …`) o
   `HALF RATE held by the system` si ya era 0; fin `HALF RATE over after X s`; resumen al cerrar.
6. Carátulas: ver «Pendiente conocido».

### 2026-10-18 (noche, 2.º log AC, perfil Vulkan completo)
- Audio AC OK (1 Voice Starvation, burst 1024). Pulsaciones llegan (`[input]` A/X/gatillos), `[ctrl] GetControllerState4 -> Connected=0x3`,
  pero `OVRInput connected=0x0 active=0x0` (ovrpVersion 1.88.0; OVRManager OnDestroy al cambiar de escena).
- Rosa sigue con perfil completo (vk.caps 1.3, 128, geom, int16, vertex stores) y sin `-force-vulkan` (usuario): NO son capacidades
  ni el Vulkan forzado. Son objetos que en Quest son invisibles (no son mandos), visibles desde el arranque y pegados a las manos.
- Regresión: panel de calibración de mandos de AC no aparece (salen sus botones); con los parches de AC sí. Causa probable: b609321
  quitó la composición de quads en orden de envío (era comportamiento de Quest, no muleta). Restaurada para TODOS
  (`KL_OVERLAY_SUBMIT_ORDER` def. 1, KleptonCompositor.swift). Efecto menor: el HUD de FPS hereda `overlayOrderState`.
- Basura gráfica ~1 s en la carga: sin causa. Limpieza síncrona de capas descartada en revisión (stall en el hilo del juego;
  el compositor ya limpia capas no escritas con `unwrittenLayerReady`).
- Pendiente rosa: ¿salía con los parches de AC? Si no, bisecar los parches retirados (b495801: 2 etapas, pose fresca,
  SpaceWarp spin, MSAA/ojo, overlay texl) con `KL_AC_NEXUS_PATCHES=1`.

### 2026-10-18 (noche, 3.er log AC) y carátulas
- Con quads en orden de envío (5cf8922) vuelve la imagen de calibración de mandos y se entra al juego. `isHmdPresent=1`,
  OVRManager vivo, `OVRInput connected=0x0` sigue (el juego lee mandos por otra vía; no bloquea).
- `[magenta]` (f200a95 añade estado): 2 pipelines con writeMask 0xf, blend 0, depth test/write, op 6 → se crean en
  `ShaderWarmupManager.WarmUpShaders` (Unity siempre calienta su shader de error); no prueba que sean los cuadrados.
  Muchos cuadrados rosas en el 3.er log, uno sigue la mirada: candidato capa #5 Quad/head 512x512 a 0,5 m (fuente
  «clear-only pass») ahora visible por el orden de envío. Siguiente: log con «capturas de diagnóstico».
- Corrupción ~1 s + corte del inmersivo al iniciar: sin cierre de espacio en el log (lo hace visionOS; probable espera
  del sistema por fotogramas tardíos en la carga). Sin investigar.
- Carátulas: los APK NO traen la 16:9 (solo app_icon 48-192 px; `assets/vr_splash.png` = logo: AC/B&S 3840x2160, Batman
  2048x435, AoT ninguno; sin android:banner). `VQCover.swift`: cadena carpeta del juego → `cover-store.*` (OculusDB
  `oculusdb.rui2015.me/api/v1/search/<package>`, `imageLink` o imágenes 16:9 de la descripción; aspecto 1.6-1.95; ≤8 MB;
  marcador `cover-store.none` `absent` sin reintento / `net` 7 días) → `cover-splash.*` (vr_splash 16:9) → `cover-apk.*`.
  Ajuste «Descargar carátulas de la tienda de Meta» (`vq.storeCovers`, def. sí). Refresco por `vqCoverUpdated`. Sin
  probar: OculusDB no verificado byte a byte (CDN no accesible desde aquí); log «Carátula de tienda: … guardada».

### Pruebas pedidas con la build nueva
- AoT y AC por defecto: audio de AC (sin Voice Starvation, burst 1024), `[magenta]` (¿desaparecen?), `vk.caps`,
  `vk.err what=varyings`, líneas `[cp] HALF RATE`/`minimumFrameRepeatCount`.
- AC con «Perfil Quest 3» apagado: ¿`connected=0x3`? Si sí → identidad Quest 3 + OVRPlugin < 1.89 (arreglo general).
- Si sigue el rosa: repetir con «Perfil Vulkan completo».
- Si el medio ritmo sigue con repeatCount 0 → regulador del juego (como AVP) para no provocar retrasos del compositor.

## 2026-10-18 (tarde): pasos 1-7 compilados — BUILD SUCCEEDED, IPA publicada
- Run 39150059344 (merge 12090b2) falló solo en `kl_bionic_extra.c`: `lgamma_r`/`lgammaf_r` no declarados (math.h de
  Darwin los esconde tras `_REENTRANT`; libSystem los exporta). Arreglo 210c12b: prototipos explícitos tras los includes.
- 210c12b también: recetas `build/xros|xrsim/libklepton.a` del `klepton/Makefile` compilan todos los `.c` y fallan al
  final (`rc=0 … || rc=1`) antes del `ar`; paso del runtime en `visionos.yml` no corta el job (`|| echo ::error::`) para
  que `xcodebuild` saque también sus errores en la misma pasada. Sin `libklepton.a` el enlace falla → no hay release.
- Run 39150906473 (210c12b): runtime + VPVulkan (con `taa_native_metal.mm`, gles) + app → BUILD SUCCEEDED; release
  `visionos` actualizada. Avisos revisados, ninguno es bug: `kl_ovrp.c:2798` indentación (el `n++` va fuera del if a
  propósito), `strchrnul` visionOS 2.4+ (target 26), `-Wcomma` en `vq_harvest.c`, sin usar en `gpu/gles/glsl`.
- Siguiente: «Orden de pruebas en el visor» desde el punto 1 con la IPA de 210c12b; logs a `QUEST/Registros`.
VPKEngine `main` = e6c3f3b.

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
  gl_PointCoord fijo; window surfaces planas sin conectar; GLboolean sin extensión garantizada (teórico);
  `kl_gles_sym` NULL cae a la pasarela GL vieja (estados mezclados).
- Android: AObb sin montaje real; AImage solo YUV_420_888; OpenSL sin efectos/3D/MIDI; posix_spawn/exec/SysV ENOSYS;
  inotify sin IN_ACCESS/OPEN/CLOSE. Sin validar en Darwin: kl_bionic_extra2/4.c (campos internos de FILE), thunks `.S`.
- VPKEngine (arreglado 498bc9f/e6c3f3b, `taa_native_metal.mm` sin compilar): fase de jitter solo si se codificó,
  handler con `shared_ptr<Stats>`, `*WithCount` con min(n,1), requisitos de memoria sin handle nulo, uiMask bajo
  submitMutex; EGL BAD_ACCESS y destrucción diferida de contexto/surface. Queda: hilo que muere sin soltar su contexto.
- Carátulas: en el launcher solo salen los iconos; la carátula que trae cada juego de Quest no se muestra.
  `VQCover.swift` busca: imagen en la carpeta del juego (cover/banner/poster o la mayor) → en el APK solo
  `res/**/{app_banner,banner,tv_banner,app_icon,ic_launcher,icon,…}.{png,jpg,webp}` → cae al icono. No mira `assets/`,
  OBB ni nombres ofuscados por aapt2 (`res/xx.png`). Pendiente: localizar en un APK real dónde va la carátula
  (listar el zip y buscar imágenes anchas) y añadir esa ruta con prioridad sobre el icono; invalidar `cover-apk.*`
  ya cacheados (hoy guardan el icono y tapan la carátula aunque se corrija la búsqueda).
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

## Fix crash general + carátulas
- VPKEngine: fix de capturas colgadas en completion handlers Metal (crash de TRIANGLE y probablemente SIGBUS de AC). Auditado runtime C de 650083e (eye hold, kl_wait, ledger, rings, fb_map): sin escrituras fuera de rango.
- `app/Sources/VQCover.swift`: imagen de carpeta solo si el nombre contiene cover/banner/poster/caratula; marcadores `<unix> <absent|net> v2` (reintento 3/7 días, antiguos ignorados); log por candidato; rechazo html/json/text; máx 16 MB; downscale >1920 px u >8 MB a JPEG 0.85; borra caché de otra extensión.
- Pendiente: si AC sigue cayendo probar VPVK_MAX_DRAWS_PER_CB=0, VPVK_ZERO_NEW_IMAGES=0, KL_SNAP_GAME_STALL_S=0. Pausa con la corona en la siguiente build.

## Lote 2026-10-19: memoria, rwlocks, carátulas, diagnóstico de cierres
- Diagnóstico de los 3 cierres (build fc4492c): AC = jetsam a ~6.5 GB (aviso de memoria nunca saltaba); TRIANGLE = deadlock en `klb_pthread_rwlock_wrlock` (no crash); Demeo = muerte silenciosa a los 3 s tras cargar libUnityARFoundationMeta (sin fault ni presión de memoria; causa desconocida).
- `klepton/runtime/libc/kl_libc.c`: `kl_mem_pressure_poll` avisa si avail<=384 MB o footprint>=80 % de 6144 MB; histéresis `g_lowmem_armed` (rearma <70 % y avail>=1.5×floor); logs `[mem] threshold …` y `[mem] OS pressure …`.
- `klepton/runtime/libc/kl_pthread.c`: eliminado SYNC_TABLE; rwlocks por dirección (`g_rwl_map` 65536, tombstones), implementación propia mutex+cond (preferencia lectores bionic, kind 2 prefer-writer, EDEADLK/EBUSY/EPERM), destroy EBUSY si tomado; `kl_pthread_report` vuelca `-- rwlock holders --`.
- `app/Sources/VQCover.swift`: ficha `/api/v1/id/<id>` (formato no verificado: el log lista sus claves; ajustar `detailImages` si difiere), candidatos apaisados primero, 401/403 de URL firmada no marca net, fallback a cuadrada, marcadores v3.
- `app/Sources/VQLog.swift` (+VQLaunch/VQLauncher/VQConformanceView, KleptonApp.swift): centinela `Registros/sesion-en-curso.txt` (VQSession) y MetricKit `VQMetrics` volcando MXCrashDiagnostic al log.
- Pendiente: verificar en visor AC (memoria), TRIANGLE (rwlock), Demeo (MetricKit/.ips); pausa con corona.

## Fix Demeo: hook OpenXR de OVRPlugin
- .ips 00:48:38 (Demeo): SIGKILL CODESIGNING, pc=x8=-1004 llamado desde libUnityOpenXR+0xef73c. `ovrp_UnityOpenXR_HookGetInstanceProcAddr` caía en `klovrp_called` (-1004) y Unity lo usaba como `xrGetInstanceProcAddr`.
- `klepton/runtime/xr/kl_ovrp.c`: `klovrp_UnityOpenXR_HookGetInstanceProcAddr` pass-through del argumento (en `g_ovrp_impl`); `ovrp_UnityOpenXR_*` (SetClientVersion, On*Instance*/Session*/AppSpaceChange) y `ovrp_AllowVisibilityMask` → `g_ovrp_result_ok`; `klovrp_called` devuelve 0 (no -1004) si el nombre contiene ProcAddr/Hook o es `ovrp_Get*Ptr/Pointer`.
- .ips 00:23:52 (TRIANGLE, build 650083e): SIGSEGV 0x34 en slowLog → ya corregido en VPKEngine a316f28.
