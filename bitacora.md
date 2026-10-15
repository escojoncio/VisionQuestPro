# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-14; lo cerrado queda en una línea con referencia).

## 2026-10-14 (6) — CAUSA de «no arranca»: restauración de escena, no el código
- Informe .ips (xrOS 27.0 24M362): `SIGTRAP` en SwiftUI `AppSceneDelegate.makeSceneHostWindow(restorationSceneItemID:
  restorationData:…)` desde `scene(_:willConnectTo:)`, antes de cualquier código propio. El sistema intentaba restaurar
  la sesión guardada tras el OpenXR CTS (log 61, muerto por memoria con el espacio inmersivo abierto); persistía al
  instalar encima. Arreglo: `.restorationBehavior(.disabled)` en la `Window` principal (KleptonApp.swift). Remedio sin
  build: eliminar la app del todo y reinstalar.
- Los cambios de (4) (621d102..6fe05ab) y VPKEngine 1f246f1 están reintroducidos íntegros; la reversión intermedia
  (6476c93, VPKEngine 07c79f3) fue innecesaria.

## EMPEZAR AQUÍ — 2026-10-15 (13): fin de frame asíncrono, pose de render real, geometry para Unity — SIN PROBAR
- Resultado de (12) (Registros 5/6, log AoT completo 11:52–11:53): temblor al girar sigue; AoT 50–75 fps (pocos a
  90) con ~7 ms de GPU; compositor sano (cadencia 11,11 ms, 0 negros nuevos); sin gpu.err; VPKTAA off (MSAA 2x).
  Ring de poses: el hilo principal hace Update(Render,F) DOS veces por frame, la 2.ª DESPUÉS de BeginFrame4(F) en
  el hilo de render; GetNodePoseState3 vio frameIndex=0 en sus lecturas → el anillo no sirve para las lecturas.
  AC: el log no llegó (el visor se satura al iniciar AC y hay que reiniciar); capturas AC siguen 100 % magenta.
  Corrupción del borde derecho en frame 2: no reapareció en esta tanda.
- `kl_ovrp.c`: el registro del frame sigue la ÚLTIMA lectura de cabeza/ojos (nodos 0,1,2,9, step Render) hecha en
  el hilo de BeginFrame4 (`g_render_tid`, `g_pend_rr`) entre BeginFrame4 y EndFrame4. Log cada 300 frames:
  `[ovrp] render-thread pose vs BeginFrame record: worst/mean/… differed` (= error que había antes).
  `KL_OVRP_RENDER_READ=0` lo desactiva. Si no sale la línea, el hilo de render no lee poses (ver otra vía).
- `kl_vulkan.c` fin de frame ASÍNCRONO (`KL_VK_FRAME_ASYNC`, def. 1): en vez de vkQueueWaitIdle por frame (CPU y
  GPU nunca se solapaban), `kl_vulkan_native_submit` con un CB vacío en la cola de VPKEngine y
  `addCompletedHandler:` (objc_msgSend + bloque C) que avanza el serial; máx. 3 en vuelo (si no, espera como antes).
  No con VPKTAA inline ni con capturas KL_VK_OUT. Log: `frame end: ASYNC …` una vez. Riesgo a vigilar: overlays
  muestreados antes de que acabe su copia (parpadeo de capas).
- VPKEngine `628c3b4`: `VPVK_REPORT_GEOM=1` reporta geometryShader/tessellationShader + límites tipo Adreno.
  Hipótesis magenta AC: Unity descarta subshaders target 4.6/5.0 o `require geometry` → shader de error. La app la
  pone en `applyToEnvironment` (juegos) y `VQConformance` la quita. Si un pipeline trae etapa geom/tess real:
  `vk.err what=stage` en el log.
- Pendiente: log de AC (se copia como «sesion-anterior» al lanzar el siguiente juego); saturación al iniciar AC
  (memoria: foot 4,4 GB a 1,5 min en log z9); voces AC «Voice Starvation» (streaming Wwise); HUD AoT.

## 2026-10-15 (12): poses por índice de frame (temblor al girar), registros completos — probado: sigue temblor
- Resultado de (11) (capturas 11:25–11:29): AoT CON imagen y en español (HUD: juego 55 fps / visor 90 Hz, VPKTAA off);
  AoT log de z7: fps del juego mayormente 90, con caídas a 35–85. AC: capas OVROverlay 100 % magenta (255,0,255)
  en la captura del compositor, incluida la capa 3 (Quad head-locked 1024², «own image (guest wrote it)», escrita por
  vkCmdCopyImage desde RGBA8 1024²) → la FUENTE ya es magenta. Hipótesis: OVROverlay hace Graphics.Blit con
  material «Oculus/Texture2D Blit» a un RT temporal y luego CopyTexture → shader de error de Unity. Pendiente la
  captura de la fuente (`vk_layerNN_src.png`).
- Usuario: imagen temblorosa al girar la cabeza (quieta OK), todos los juegos. Causa (kl_ovrp.c): Unity tiene dos
  hilos; WaitToBeginFrame(N+1)+Update(Render) del hilo principal llegan ENTRE BeginFrame4(N) y EndFrame4(N) del de
  render; `g_step_head` era una sola muestra compartida e ignoraba el frameIndex de Update2/3 y de
  GetNodePoseState3(step, frameIndex, node) → frame N podía dibujarse con la pose de N+1 mientras el registro de
  timewarp llevaba otra → error ∝ velocidad angular. Arreglo: anillo `g_fring[16]` por frameIndex: primera muestra
  Render de cada frame fijada en Update2/3, GetNodePoseState3(Render,F) responde de ella (t_req_frame), y
  BeginFrame4(F) registra esa misma. Protecciones: índice repetido en BeginFrame → anillo off; índice que retrocede →
  anillo limpio; solo índices en [begin-4, begin+8]. `KL_OVRP_FRAME_RING=0` = comportamiento anterior. Log: 24
  líneas `[ovrp] Update(Render) frameIndex=… tid=… -> pinned|kept` y 12 `GetNodePoseState3(Render) frameIndex=…`.
- Registros: la copia «sesion-anterior» se hacía a los 10 s de abrir la app, DENTRO de la sesión nueva si se lanzaba
  un juego rápido (las carpetas traían 10 s de la sesión nueva). Ahora `VQLauncher.play` llama
  `VQLog.savePreviousSessionToQuest()` antes de preparar el juego. `saveToQuest` omite klepton-crash.log si es >60 s
  más viejo que el boot log (el fault de Wwise de las 09:55 se copiaba en todas las carpetas).
- `kl_vulkan_capture_layers`: con «capturas de diagnóstico» (KL_CAPTURE=1) captura capas + fuentes 2 veces en
  QUEST/Registros (vk_layerNN.png / vk_layerNN_src.png; log: centro y nº de píxeles magenta).

## 2026-10-15 (11): SpaceWarp siempre servido, QoS RT, latencia adaptativa, idioma — probado: AoT con imagen
- Resultado de (10): AC ya no crashea (audio suena, x18 OK). AoT SIN IMAGEN: con KL_VPKTAA=0 se rechazaba
  GetLayerTextureSpaceWarp → libOculusXRPlugin CreateEyeTextureStage (+0x137ec) llama CreateMotionVectorTexture si
  el SETTING global del juego (OculusSettings SpaceWarp, `ldrh [x8,#0x28]`) está activo, NO por el desc → falla la
  etapa → `ovrp_DestroyLayer(1)` tras 1 frame → Unity sin destino XR (`Missing resolve surface`, `Not inside a
  Renderpass`). Arreglo: `klovrp_asw_on()` = `KL_VPKTAA_ASW` (def. 1), independiente de VPKTAA (kl_ovrp.c).
- AC audio «petardea»: 334× Wwise `Voice Starvation`. `kl_pthread.c`: hilo guest creado con SCHED_FIFO/RR →
  QOS USER_INTERACTIVE en el trampolín (`KL_GUEST_RT_QOS`); `pthread_setschedparam` FIFO/RR → `kl_pthread_boost_qos`.
  `kl_audio.c`: objetivo de llenado +10 ms por underrun nuevo (máx. 120, `KL_AUDIO_ADAPT`); la línea periódica
  `[au] guest PCM so far` ahora incluye underruns y objetivo.
- Cuadrados magenta de AC = capas OVROverlay Quad 1875x1500 flags 0x205 que siguen a cada mano (#33/#34, luego
  #43/#44); NUNCA escritas por el guest (sin «written by the guest») → el compositor muestra el texL del guest.
  Diagnóstico añadido: compositor registra de qué peldaño sale cada overlay (own/texL/fallback, tamaño, formato,
  flags); `kl_vulkan_capture_layers` captura también la textura fuente (`vk_layerNN_src.png`) y loguea píxel
  central + nº de píxeles magenta. Requiere «capturas de diagnóstico» activado.
- Botón PS: el censo de elementos Sense en visionOS solo tiene "Button Menu" (Create/Options); el PS no se expone
  (el sistema lo reserva). Sonda `probeHomeButtonAPI` (KleptonControllers.swift) lista selectores home/system/gesture
  de las clases GameController en el visor real (el SDK de la CI es XROS 26.5).
- Idioma: AC usa LanguagePack/AssetFile de Meta (voces descargables; en la carpeta solo hay en.lang). Texto
  interno: `VQLanguage.matchCode` busca en global-metadata.dat identificadores con el nombre inglés del idioma
  (WwiseLanguageSpanish, MexicanSpanish…; «Spanish» a secas = enum de Unity, no cuenta). Ajuste nuevo «Idioma del
  juego»: Automático / Español (España) es_ES / Español (México) es_MX / Inglés (VQSettings.gameLanguage, vq.gameLang).
- Shims nuevos (kl_shim.c): __poll_chk, __pread_chk, __pwrite_chk, fread_unlocked, sched_getparam, sched_getcpu, tgkill.
- Auditoría ABI heurística ovrp (args leídos por la export real vs nuestras firmas, OVRPlugin 1.88/1.89): solo
  SpaceWarp (ya arreglado) era real; resto falsos positivos del parser.

## 2026-10-15 (10): x18 en código GCC (Wwise) + ABI de SpaceWarp — probado: AC arranca con audio
- Resultado de (9) (logs 2026-10-15 09:55 AC / 10:02 AoT): AC ya pasa del Wwise (bancos cargan) y muere en el 1.er
  frame de audio: SIGSEGV 0x0, `ldr q4,[x18]` veneered en libResonanceAudio+0x1f40c tras `adr x18,+0x1f5f0`
  (+0x1f3fc) REFUSADO (KLX_PC_ADR) → ranura TSD = 0. AoT: parpadeo + rayas rojas al inicio.
- Herramienta: arnés Linux de `kl_x18_emit` (scratchpad, compila kl_x18.c con stubs de libkern/mach y
  `tp=0`) sobre todas las .so de AC y AoT: antes 6 refusados (5 `adr x18` en libAkSoundEngine/libResonanceAudio,
  1 `sub sp,x7,x18` VLA en libAkSoundEngine+0x1b6e58) y sitios reales tomados por datos junto a pools literales
  GCC (libAkSoundEngine+0x1da450/0x1da460, libAkConvolutionReverb+0xae48), y 3 palabras de pool (`ldr s,lit`)
  PARCHEADAS como si fueran código (libAkSoundEngine+0x262be4/0x262de0/0x264548, constantes de Opus). Ahora:
  0 refusados en ambos juegos, esos pools = datos.
- `kl_x18.c`: `klx_lit_map` (bit k: destino de `adr` 4 palabras / `ldr literal` su tamaño → no cuenta como
  evidencia; bit n+k: destino de `ldr literal` → dato seguro; `adr` nunca marca dato seguro por jump tables).
  `kl_x18_is_data` (TLS rewriters) sigue sin mapa (coste por sitio). Veneers nuevos: `adr x18` y `ldr w18/x18/
  ldrsw x18, lit` (adrp+add desde el veneer → str ranura); `add/sub sp, xN, x18[,ext]` y `add/sub sp, x18, #imm`
  (b = sp viejo, sp = nuevo, recarga a/b vía b). `KLX_VEN_MAX_INSN` 8→12. `VQPrepare.converterVersion` kld-3
  (todas las librerías se retraducen la 1.ª vez).
- `kl_ovrp.c` `ovrp_GetLayerTextureSpaceWarp`: la export real tiene 7 args (x3..x6 punteros, todos obligatorios;
  libOVRPlugin 1.89 +0x16efc0). libOculusXRPlugin (MISMO binario en AC y AoT) lee motion=[x3] y depth=[x5]
  (CreateMotionVectorTexture +0x13c28). Escribíamos depth en x4 → [sp+0x20] basura = &global del plugin
  +0x53534 → «wild handle 0x…03534» D24S8 sustituido por scratch 4096² en AMBOS juegos. Ahora depth en x5, x4/x6=0.
- `VQLaunch.swift`: AA auto con MSAA ya detectado (`vq.aaDetected` > 1: AC 4x, AoT 2x) → VPKTAA off desde el
  arranque (KL_VPKTAA=0) → SpaceWarp no se sirve, el proveedor apaga AppSW (sin pase de MV por frame).
- Probar: AC (audio y que pase del primer frame de Wwise; capas rosas), AoT (parpadeo/rayas). Buscar en log:
  ausencia de `wild handle`, `[asw]` sin GetLayerTextureSpaceWarp en juegos MSAA, faults `[x18]`.
- Rosa de AC sigue sin causa: capas 2/3 OVROverlay. Capa 2 = vkCmdBlitImage desde ASTC_6x6_SRGB 2x2 →
  `gpu.err blit_unsupported out_of_range` (VPKEngine anuncia BLIT_SRC para ASTC? revisar format features);
  capa 3 = vkCmdCopyImage desde RGBA8 1024². Siguiente: procedencia de la textura fuente.

## 2026-10-15 (9): build con VPKEngine 935aa1b (arreglos del Vulkan CTS completo)
- Base medida (CTS rapida completo, cts_4+cts_5, logs 62–63): 36 087 casos, Pass 8 567, Fail 898, NotSupported 26 622.
- VPKEngine `bbc2791`+`935aa1b` (detalle en su bitácora «2026-10-15 (12)»): imágenes LINEAR con píxeles en su memoria
  (casi todo draw.*), multiview clears por vista, DONT_CARE con área parcial, store D/S por aspecto, resolve MSAA
  entero/parcial, input attachments MS, autodependencias, formato del VkBufferView en el traductor (texel buffers),
  OpCompositeInsert con operandos corregidos, shadow+grad, frexp/ldexp exactos, mipLodBias, clip distance en FS,
  bucles >8192, timeline wait-before-signal, aliasing de imágenes, cube array en load/store, etc.
  `kTranslatorVersion` 7 (los juegos retraducen shaders una vez).
- Klepton (esta build):
  - `kl_pthread.c`: mapa de mutex con TUMBAS (destroy dejaba key 0 y cortaba cadenas → dos mutex host para un mutex
    guest → `mutex lock failed: Invalid argument`, OpenXR CTS log 62, sesión 38). Inserción/destroy serializados
    (`g_mtx_insert_mu`), hash de 18 bits (antes 15: solo usaba 32k de 262k ranuras), `mtx_host` acotado con
    fallback, log `[klb] pthread_mutex_lock(...) failed`.
  - `kl_openxr.c`: capa `XR_APILAYER_KHRONOS_runtime_conformance` (si `libXrApiLayer_runtime_conformance.so` es
    cargable): EnumerateApiLayerProperties/ExtensionProperties(layer), negociación, `createApiLayerInstance` con
    `next` = runtime, trampolines arm64 por fila (`kl_xr_slots`, `klxr_guest_fn`) que `klxr_layer_route` redirige a
    la capa; `unroute` en DestroyInstance. Juegos (sin la .so): punteros directos, capas desconocidas ignoradas.
  - `kl_ndk.c`: AAssetManager_open/openDir con rutas absolutas (`kl_asset_resolve` → `kl_guest_path`; si no existe,
    `assets/` como antes) → bancos de Wwise (AC Nexus). Stubs AInputQueue/AKeyEvent/AMotionEvent.
  - libc: `klb_openat` pasaba AT_FDCWD de Linux (−100) a Darwin (openat relativos = EBADF). Nuevos: fstatat,
    faccessat, mkdirat, unlinkat, renameat, readlink(at), fchmodat, fchownat, linkat, symlinkat, rmdir, remove,
    truncate, chmod/chown/lchown, link/symlink, mkfifo, creat, utime/utimes/utimensat/futimens, pathconf/fpathconf,
    __realpath_chk/__getcwd_chk, ftw/nftw, glob/globfree, fstatfs, __openat_2, freopen, nextafterf, __libc_init
    (+ alias *64). `kl_libc_table.h`: quitadas las 25 KL_FWD que tapaban los shims.
  - Futex reescrito (`kl_futex_impl`): lista de waiters por bucket, WAKE devuelve los despertados, REQUEUE/CMP_REQUEUE,
    WAKE_OP con signo, plazo monotónico. `kl_futex_dump()` en el `[snap]`: por dirección valor, waiters, último WAKE.
- Probar: (1) Vulkan CTS con «Borrar resultados»; (2) OpenXR CTS (debe salir «API layer … active» y ya no el aborto
  de la sesión 38); (3) AC Nexus/AoT/Batman: audio Wwise, congelado (`[futex]` en el `[snap]`), capas rosas.
- Pendiente conocido: pSampleMask del pipeline; multisample.misc entero; linkage ivec4→ivec2; mipmap projected;
  rasterization *_point; capa Vulkan del conformance layer (no cargamos capas Vulkan); CTS de OpenXR con trampolines
  solo si la .so se detecta antes de relocalizar (si sale «API layer routed 0», revisar orden).

## 2026-10-14 (8) — resultados del CTS completo (base de lo anterior)
- Ver git log -p bitacora.md (lista de grupos 1–6).

## 2026-10-14 (7) — resultados tras df34dd2 (SIN build todavía; esperar al Vulkan CTS completo)
- Vulkan CTS (cts_4): 88/106 tandas en 3 min, 30 607 casos, 0 Crash/Timeout/DeviceLost; Pass 8 363, Fail 859 (90,7 %),
  NotSupported 21 385. Sin archivo binario el CTS va ~30x más rápido. Sonda `[xpc]` OK. Faltan tandas 088–105.
- OpenXR CTS (log 62): STATIC_IMAGE ya pasa (único FAILED = capa de conformidad). Aborto final:
  `libc++abi … std::system_error: mutex lock failed: Invalid argument` en un `xrCreateSession` (38.ª sesión) — mismo
  aborto que el log 22; sin backtrace (el latido también se para desde 22:44:15, igual que en el log 61). Pendiente.
- `kl_mem trim (system critical)`: 253 imágenes retiradas seguían en cola → el temporizador solo se reprogramaba con
  entradas quick. Cambio local (kl_vulkan.c): `klvk_retire_image_ex` programa el temporizador; este se reprograma
  mientras quede algo; las reemplazadas caducan también a 4×`KL_VK_FREE_AFTER_S` sin fines de frame.

## 2026-10-14 (4) — cambios tras los logs 60/61
- Vulkan CTS (log 60, `cts_3`): sesión cortada por tiempo en la tanda 35/106 (12 645/36 087 casos; 4 348 Pass,
  297 Fail = 93,6 % de los soportados, 8 000 NotSupported). Faltan las tandas 035–105: seguir sin borrar resultados.
- Causa común de muchos fallos: desde 2 s tras arrancar, conexiones NUEVAS a servicios del sistema fallaban
  (`MTLCompilerService … Connection init failed at lookup with error 3`: 17 pipelines, librería de blits → ~90
  `blit_unsupported …library_failed`, 118 `archive_save`). Al pulsar Compartir, la hoja del sistema abortó la app
  (`Could not cast _NSXPCDistantObject → _EXDiscoveryServiceProtocol`, mismo síntoma). Causa raíz SIN identificar.
  - VPKEngine (11): reintentos/backoff de compilación, sin cachear fallos por compilador inaccesible.
  - CTS sin archivo binario: `VQLaunch` no pone `VP_PIPELINE_CACHE_DIR` si `isConformance` (A/B: si sigue
    cayendo, no era el archivo).
  - Diagnóstico: `VQCompilerProbe` (VQLog.swift) compila una librería única cada 15 s → `[xpc] compilador Metal
    OK|INACCESIBLE` en cambios y cada 5 min; latido `[hb] … ports=N fds=M` cada 64 ticks (`kl_ipc_counts`,
    kl_app.c). Siguiente: cruzar el momento del primer INACCESIBLE con ports/fds (¿fuga de puertos por VkDevice?).
  - Registros sin hoja de Compartir: `VQLog.saveToQuest` → `QUEST/Registros/<fecha>-<etiqueta>/` (últimas 12):
    al arrancar si hay boot log nuevo (`sesion-anterior`), al final del resumen del CTS (`conformidad`) y a mano.
    `VQShareLogs.swift` = `VQSaveLogsButton` (solo en Ajustes > Registros, junto al ShareLink). Los `ShareLink` vuelven a
    ser EXACTAMENTE los de a1df14a: 621d102/aae1aea/cefc682 no arrancaban en visionOS 27.2 (Menu con ShareLink en el
    toolbar del launcher, y luego un 4.º botón junto a él → overflow = menú). NO tocar el toolbar del launcher.
  - Arranque = a1df14a salvo la copia de la sesión anterior (+10 s, segundo plano). `VQCompilerProbe.start()` solo al
    lanzar un juego/test (VQLaunch).
- `dEQP-VK.info.platform` (Crash, signal 11): `createPlatform()` de deqp pasaba a `Platform` una referencia a un
  `NativeActivity` de la pila. `patch_deqp.py` lo hace `static`. Requiere APK nuevo (`conformance.yml`, target vulkan).
- OpenXR CTS (log 61): muerto por presión de memoria del sistema tras `MaxLayers-noninteractive` (16 capas 4766×1912:
  0,7→3,5 GB en 4 s; la cola de retiro solo se vaciaba en fin de frame/otra destrucción, ~30 s después).
  - `kl_vulkan.c`: `klvk_retire_schedule` (dispatch_after_f 0,6 s, cola global utility) vacía las imágenes `quick`;
    `kl_vulkan_trim()` (desde `kl_mem_pressure_fire`, kl_libc.c) las libera ya (`klvk_retire_pump_ex(d,1)`).
  - `kl_openxr.c`: swapchain `STATIC_IMAGE` = 1 imagen (antes 3 y solo 1 adquirible → CALL_ORDER_INVALID en
    «Swapchains / Non-default create flags»).
  - El XML del CTS se escribe al final: tras un cierre, los resultados solo están en el log (`V/OpenXR_Conformance`).
- Pendiente Vulkan (tras medir otra vez): image_clearing 1–2 canales/enteros, multiview clears, texel buffers,
  different_signedness, aliasing, shader_access secundarios, clears en quad (`gpu.clear`), multiview+occlusion,
  queryLod, 16-bit storage; capa `XR_APILAYER_KHRONOS_runtime_conformance` (el loader propio no carga capas).
- Resuelto antes (detalle en git): OpenXR swapchains liberados/FIFO/orden de frames/estados (sesión (3)), VPKEngine (10).

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
