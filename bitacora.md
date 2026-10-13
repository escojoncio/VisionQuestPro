# Bitácora — VisionQuestPro

Historial completo de sesiones anteriores: `git log -p bitacora.md` (compactada el 2026-10-13 tras el log 49).

## Renombrado VPEngine → VPKEngine (2026-10-13)
- Todas las menciones, rutas e identificadores pasan a VPKEngine (repo `escojoncio/VPKEngine`, checkout `vpkengine/`).
  Datos antiguos del visor (carpetas VPEngine, ajustes, certificado, packs `vpengine.vpgame`) se traspasan solos
  (`VPKLegacy` en VPKEngine `platform/visionos/Sources/VPGamePack.swift`).
- `app/Sources/VQLaunch.swift`: ajuste `vq.vpkengine` con respaldo de la clave antigua `vq.vpengine`.

## Logs 50–52 (2026-10-13, primera build INLINE) → plan siguiente build
Inline funciona como diseño (100 % frames resueltos, jitter exacto mixed=0, sin native_stuck) pero EMPEORA fps:
- **Serialización**: hook TAA antes del `QueueWaitIdle` de `kl_vulkan_frame_done` (kl_vulkan.c ~3564/3579) → frame =
  CPU + GPU juego + TAA. TAA ≈14,7 ms (Batman 2383x1912x2) / 15–15,5 ms (AoT 2347x1912x2). Batman ~90→~37 fps,
  AoT partida 25–28 fps, menú 43–45. `gpu=` mide gpuEnd−gpuStart del CB client (puede incluir la espera del event).
- **Borroso**: MSAA 2x nativo forzado a 1x; historial hw≈0,84 + CR 5 taps remuestreado cada frame (cabeza en VR);
  reconstrucción gaussiana σ≈0,47 px; RCAS 0,4 débil; near 0,05 por defecto vs 0,1 del frustum (paralaje x2);
  ojo 2383x1912 ampliado ~2,6x bilineal por el compositor.
- **pollInline**: >1 s sin gancho (cargas, gpu.hold) → ruta async (lee el override = TAA de TAA, `core` usado desde
  dos hilos) → saltos/parpadeos. Debe quedarse inline una vez visto el gancho.
- **AoT cosas faltantes** (no TAA): 18× VPKEngine `vk.err compute "internal error: vector"` (out_of_range en
  `gpu/shader/spirv/src/lower.cpp`) → dispatch_skipped 3–15/frame (skinning/partículas). Vídeo: `kl_mediandk.c` sin
  surface entrega getOutputBuffer NULL/size 0 y getOutputFormat sin color-format/stride → 280 dequeued, 0 rendered.
  SpaceWarp: depth del pase MV = handle salvaje 0x100c88430 → MV a cero (moving 0 %).
- **AC rosa** (no TAA): skybox de vídeo nunca OFF (ojo nunca compuesto); capas overlay no escritas compuestas (18–22,
  25, 33–37) porque VPVulkan no exporta `vkGetMTLTextureMVK` (klvk_overlay_mtl_texture y sombras muertos) y desde
  d574e25 `KL_VK_LAYER_NOCLEAR` ya no limpia → memoria sin inicializar = rosa. AC no está colgado (llega al menú y
  a una 2.ª carga; sesión de 26 s); Wwise sigue sin bancos (`main.207706…obb` ausente, en.lang no buscado en obb).
- Comunes: `gpu.shader.lib hit=0` (0 % caché MSL → tirones 1–1,7 s), ASW apagado (26 fps sin reproyección
  posicional), memoria 5921/6144 MiB en AC (imágenes viejas de ojo/MV/depth no liberadas).
### Decisión clave: portar el VPKTAA de PTVisionPro
`VPKTAACore.swift` es una reimplementación propia, NO el `vpktaa.frag` de PTVisionPro (afinado en 50 builds:
`patches/0001-visionos-menus.patch` → `shaders/vpktaa.frag`: reconstrucción 3x3 que ya afila (sharpness 0–1, usuario 1.0),
KEEP_STILL 0.93 / KEEP_MOVING 0.80 por velocidad, SIGMA_FLAT 1.25 / SIGMA_EDGE 0.85 (caja más estrecha en bordes),
Compress/Expand reversible por brillo, historial CR 5 taps con distancia en alfa (sin textura dist), DISOCCLUSION 0.05,
capa de partículas). En PT corre dentro del render del juego (HDR, antes de post/UI, matriz de la cámara del motor,
sustituye al resolve de upscale, imagen 50 % + foveado). Aquí: imagen final LDR con UI, cámara deducida, 9–19 Mpx sin
foveado, sharp 0.4. Paso 0 del plan: portar su algoritmo y constantes a VPKTAACore (MSL compute), resolver solo la
zona nítida del foveado, capturar la view/proyección que el juego pide a OVRPlugin/OpenXR.
### Plan (orden)
1. Quitar la serialización: sin `QueueWaitIdle` por frame; serial publicado al completar en GPU (MTLSharedEvent /
   completion del tail de encodeNative), 2 frames en vuelo.
2. Ajuste «AA: MSAA del juego | VPKTAA» (por defecto MSAA del juego si lo trae; sin forzar 1x).
3. VPKTAA ≤3 ms: historial RG11B10, bilineal, sin textura dist, depth 1 tap, solo zona nítida del foveado, RCAS en el
   compositor; medir con MTLCounterSampleBuffer. Near por defecto = el de GetNodeFrustum2.
4. pollInline estable + `core` serializado. 5. AC: exportar `vkGetMTLTextureMVK` en VPVulkan, compositor salta capas no
   escritas, clear diferido. 6. VPKEngine compute out_of_range (volcar SPIR-V fallido). 7. Vídeo sin surface. 8. Caché MSL.

## VPKTAA a nivel de render — modo INLINE (2026-10-13, sin build aún)
Sustituye a la ruta asíncrona del compositor (que queda solo con `KL_VPKTAA_INLINE=0`, guests GLES y OpenXR).
Causas de lo visto en logs 46–49: resolve en cola propia compitiendo con el juego (59 % raw-fallback = alterna
AA/sin AA) y jitter por frustum no emparejable (el juego lee `ovrp_GetNodeFrustum2` ~5/frame) → borroso.
- **VPKEngine** (`gpu/vulkan`, `gpu/metal2`): jitter aplicado al TRADUCIR cada envío (`commands.cpp` `taaEyePass`/
  `Translator::jitter`): `viewport.x/y += jx/jy` (px del target, +y abajo) en draws con depth test/write dentro de
  pases de ojo (con depth y tamaño == ojo; multiview también si mide 50–100 % del ojo); excluidos pantalla completa
  (no indexado, ≤6 vértices, sin VB) y draws sin depth (cuentan skipped). Profundidad de pases de ojo forzada a
  Store. API C (`vpvk/config.h`): `vpvk_taa_set_jitter`, `vpvk_taa_get_applied` (tag, jx/jy, target, jittered/
  skipped/submits/mixed; resetea), `vpvk_native_submit(fn,user)` (cmd buffer en la cola del guest tras todo lo
  enviado, incl. frames en gpu.hold: toma `submitMutex`; fences del frontier + MTLEvent signal/wait alrededor del
  callback; ocupa slot in-flight → `vkQueueWaitIdle` lo espera), `vpvk_native_device`. Env `VPVK_TAA_JITTER=0`.
  Log `vk.taa set …`, `vk.taa jitter submit=…` (1.º y cada 1800), `vk.native submit ok=…`.
- **Klepton C**: `kl_vulkan.c/.h` `kl_vulkan_taa_inline` (KL_VPKTAA=1, `KL_VPKTAA_INLINE`≠0, `vpvk_native_submit`
  presente), `kl_vulkan_set_taa_hook` (llamado en `kl_vulkan_frame_done` tras `klvk_ed_frame_done` y ANTES del
  QueueWaitIdle, solo si `kl_vulkan_taa_frame_record` (kl_ovrp EndFrame4) dice que el registro de este frame está en
  ese stage), `kl_vulkan_native_submit/device`, `kl_vulkan_eye_color_mtl` (ojo VIVO), `kl_vulkan_taa_publish`
  (override en `kl_glfb_set_eye_mtl_override`, aplicado tras la espera y antes del serial; se borra al empezar
  EndFrame4 del stage, al cambiar la imagen viva/tamaño o al liberar), `kl_vulkan_taa_applied`. Jitter del frame
  siguiente al final de frame_done: Halton(2,3)x8 −0,5 con tamaño del ojo, SOLO si este frame se resolvió y publicó
  (si no, jitter apagado: un frame jitterizado sin resolver solo titila); `KL_VPKTAA_JITTER=0`. Profundidad viva sin
  snapshot, MSAA incluida (vkCreateImage: depth MS del tamaño del ojo con SAMPLED, sin TRANSIENT). `kl_ovrp.c`:
  frustum sin jitter en inline (jitter_mode 0). Logs `[taa-inline] …`.
- **Swift**: `KleptonTAA.swift` `pollInline()` registra el gancho; inline solo mientras el gancho se llama (<1 s, 3 s de
  gracia; si no, ruta async). `inlineFrame/inlineResolve` (hilo del guest): jitter aplicado → tangentes de VPKTAACore
  (`dx=−s.x(l+r)/(crop.x·w)`, `dy=+s.y(t+b)/(crop.y·h)` con flipY; s = j·crop·ojo/target), salida 2DArray 2 capas por
  stage en A/B (`stageOuts[stage*2+k]`, cruce de colas sin hazards), solo rgba8/bgra8 sRGB, espacial solo sin
  jitter aplicado. `VPKTAACore.swift`: variante MSL `VPKTAA_DEPTH_MS=1` con `depth2d_ms_array` (muestra más cercana
  según revZ, texture(8)). `KleptonCompositor.swift` `prepareTAA`: en inline no llama a submit/acquire.
### Comprobar en el próximo log (Batman, VPKTAA on)
- `[taa-inline] ON …`, `resolve hook registered`, `jitter ON: Halton … for WxH`; VPKEngine `vk.taa jitter … jittered>0`
  y `vk.native submit ok=1`. `encodeNative` ya va en 3 command buffers ordenados con MTLEvent entre CBs; si aun así
  se congela, buscar `gpu.err what=native_stuck … head= client= tail=` (dice qué parte no terminó).
- `[taa] stats (30 s, inline)`: resolved ≈ frames, `gpu=` ms reales, `draws jittered` > 0 y skipped razonable,
  `mixed` ≈ 0, depthValid alto (si ≈0: depth descartada), `unresolved:` vacío. Comparar fps con VPKTAA off.
- Con MSAA del juego on: `depth … (ms=N)` > 0 y sin `REFUSED: no 2D-array view`.

## Log 44 AoT (2026-10-13, sin build aún) — cuelgue en teardown de vídeo
- Causa: Unity `AndroidVideoMedia` para su `VideoDecoderLooper` con `msg.getTarget().getLooper().quit()` + `Thread.join()`. `Message.getTarget()` no tenía binding (SIMULATED → null), el looper nunca salía y `join()` bloqueaba el hilo principal del guest: frames congelados en serial 506 (`stale in a row`), PCM a cero.
- `klepton/runtime/jni/kl_jni_looper.c`: nuevos `klj_Message_getTarget` (devuelve `m->target`) y `klj_Handler_getLooper` (`h->looper` o el main looper); bindings `Message.getTarget()Landroid/os/Handler;` y `Handler.getLooper()Landroid/os/Looper;`.
### Comprobar en el próximo log
- Tras `Message.sendToTarget(what=0)` debe salir `HandlerThread "VideoDecoderLooper" looper exited` y no `Thread.join() ... still waiting`; serials del compositor siguen subiendo.

## Logs 44–46 (2026-10-13): AC Nexus no pasa de la carga + pose EyeLevel → build siguiente
- AC (45): entitlement OK y luego `ovr_User_GetLoggedInUser -> 0` → `E/Unity: An unknown error occurred. Request
  failed.` → se queda en Background-Looping. La lista por target (`zix`…) nunca casaba en VisionQuestPro (target =
  nombre de carpeta). `kl_ovrplat.c` `klplat_user_default`: también ON si el paquete empieza por `com.ubisoft.`
  (`kl_jni_guest_package`). Idiomas: `*.lang` → AssetType `language_pack`, `GetLanguage` = la entrada, Tag = nombre
  sin extensión, EnglishName/NativeName por tabla (`KL_PLAT_LANGPACKS=0` lo apaga).
- AC Wwise: `Setting package path: /sdcard/Android/obb/com.Ubisoft.ACNexusVR` → `Package path couldn't be set` →
  Init.bnk/BNK_* AK_FileNotFound, `Unable to load audio wwise package: en.lang`; `obb MISS main.207706.…obb`.
  Causa no localizada (sin op fallida visible). `kl_libc.c`: `klb_statfs` ahora mapea la ruta (`kl_guest_path`;
  antes statfs de /sdcard/… daba ENOENT); `kl_fs_trace` registra SIEMPRE (máx. 24) operaciones fallidas bajo
  `/Android/obb/` (`[fs] … FAILED … (obb diag n/24)`).
- Pose (todos los OVRPlugin): con origen EyeLevel (0) se servía la Y absoluta del visor (head Y=1.02–1.04 con
  origin=0) cuando en Quest es ≈0 → cámara una altura de ojos por encima del rig y quads world-locked del juego (y≈0)
  compuestos en el suelo. `kl_ovrp.c`: `kl_ovrp_guest_origin_y()` (altura medida si origin=0, cabeza publicada y ya
  hubo lectura OVRPlugin; 0 para OpenXR); `klovrp_step_head`/`klovrp_step_hand` restan; `begin_frame_impl` registra
  en ese espacio; RelativePose(1|2) con cur=0 → y=−altura. `kl_reproject.c` (def. débil = 0): suma el offset a la
  posición del render pose (paralaje) y a quads no head-locked. `KL_OVRP_EYELEVEL_ZERO=0` = antiguo.
- AC (45) habitación visible / destellos morados: `[pt] mode auto` y nunca `[pt] passthrough ON`, ojo y skybox con
  alfa forzado 1, imágenes de capa nuevas ya se limpian (`klvk_image_clear_black`) y overlays sin escribir se saltan
  (`kl_vulkan_layer_written`). Pero Unity/UE4 abrían el espacio en `.mixed` (`kl_app_target_wants_full` solo
  native/GLES3JNI/SDL2) → cualquier píxel/frame no opaco enseña la habitación. `visionos/Sources/kl_app.c`: también
  `KL_GUEST_UNITY`/`KL_GUEST_UE4` → `.full`; pasa a `.mixed` en vivo si el juego pide passthrough (`KL_FULL=0` revierte).
  Geometría morada: único error GPU del log = `gpu.err f=845 arg_overflow=2563 skipped_draws` (L7375, frame de
  `ShaderWarmupManager.PopulatePSOCache`) → pendiente VPKEngine (desbordamiento del argument buffer); quads del juego
  en EyeLevel ahora a la altura de ojos (antes en el suelo).
### Comprobar en el próximo log
- AC: `[plat] ovr_User_GetLoggedInUser -> request N` (no «-> 0»), sin «Request failed», `[plat] language pack: en.lang`,
  líneas `[fs] … (obb diag)` alrededor de «Package path couldn't be set», y si pasa del Background-Looping.
- Unity/UE4 en `.full`: no se ve la habitación; `[pt] immersion -> mixed` solo si el juego pide passthrough.
- Juegos EyeLevel: quads del menú a la altura de los ojos; `GetTrackingTransformRelativePose origin=1 (cur=0)` y≈−altura.

## Logs 47–49 (2026-10-13) → build siguiente
### VPKTAA en Batman (log 49): «no suaviza» + framerate
- Evidencia `[taa] stats` (30 s): resolves=1040, **raw-fallback=1472** (solo ~41 % de los frames del juego salen con
  TAA; el resto es el frame crudo → alterna AA/sin AA), gpu=22,2 ms/resolve (máx 34), juego `gpu.time` ≈13,4 ms medio
  a ~84 fps → GPU saturada: el resolve en su cola compite con el juego (Metal en visionOS no expone prioridad de cola).
- Jitter: el juego (Unity, `libOculusXRPlugin.so`) relee `ovrp_GetNodeFrustum2` (x30627, ~5/frame) y el probe da
  jitter ON, pero los votos (used,prev,next)=1,69M/2,03M/1,52M ≈ 34/40/26 % → jitter NO probado (con jitter bien
  emparejado «used» domina). Sin jitter real el TAA solo difumina/titila. Frustum real near=0.100 (se usa 0.05 por
  defecto; solo afecta paralaje de traslación).
- MSAA 2x del juego forzado a 1x (`vk.msaa forced 1x (was 2x)`) para tener depth 1x → se perdió el AA nativo.
- `VPKTAACore.swift`: `Params.pad` → `spatial` (float, mismo tamaño, 304 B); `VPKTAACore.spatial` (0..1). En
  `vpktaa_resolve`, tras la reconstrucción: AA espacial tipo FXAA (test de borde FXAA 3.11 sobre el 3x3 del tile,
  mezcla con el vecino a través del borde, 0,25–0,5), antes del blend temporal (que lo estabiliza). Sin lecturas
  extra de memoria (usa el tile).
- `KleptonTAA.swift`: `KL_VPKTAA_SPATIAL`=auto|0|1 (auto: ON hasta que «used» ≥50 % de votos; log `spatial=on/off`);
  lag de jitter auto ahora cambia si alt es el máximo y ≥1,15× used (antes ≥2× y ≥50 %: nunca saltaba);
  stats añade `WARN: TAA shown on only N pct…` cuando raw-fallback > resolves.
### Pendiente / decisión
- Estándar en móvil TBDR (Quest/Apple): MSAA en tile memory ≈ gratis. Para juegos con MSAA nativo y GPU saturada,
  lo recomendable es NO forzar 1x y dejar VPKTAA apagado (o ajuste en la app «AA: MSAA del juego | VPKTAA»); requiere
  tocar el forzado `VPVK_FORCE_1X`/kl_vulkan y Ajustes (no hecho aquí). Mismo patrón que AoT (lento con VPKTAA).
- Comprobar en el próximo log: `jitter votes … -> AUTO jitter lag -1` y si después «used» supera 50 %
  (`jitter proven`); `raw-fallback` vs `resolves`; comparar fps con VPKTAA OFF.

### AC Nexus (log 48): crash ovr_User_GetImageUrl + flashes tras el vídeo (sin build)
- **Crash** `fatal: unimplemented Oculus Platform entry point 'ovr_User_GetImageUrl'` (tras GetLoggedInUser, usuario offline Ubisoft).
  - `klepton/runtime/xr/kl_ovrplat.c` `klplat_called`: ya NO aborta; respuesta neutra por nombre (`plat_name_is_string`: último segmento `Get…Url/Uri/String/Name/Path/Token/Text/Title/Description/Locale/Tag/Hash/Key/Metadata/ApiName/Json/Email/OculusID/SessionId/DisplayName/Deeplink(Message)/Presence/Label/Sku` → `""`; resto → 0 = request "no se pudo hacer" / ID 0 / false / NULL). Log `[plat] unimplemented X -> neutral …` una vez por nombre. `KL_PLAT_STRICT=1` recupera el abort. `plat_is_drm` sigue antes y sigue abortando.
  - Getters User explícitos: `ovr_User_GetImageUrl/SmallImageUrl/Presence/PresenceDeeplinkMessage/PresenceDestinationApiName/PresenceLobbySessionId/PresenceMatchSessionId/InviteToken` → `""`, `GetPresenceStatus` → 0 (`klplat_User_str_empty`/`klplat_User_zero` en `g_plat_impl`).
- **Flashes ≤1 s**: frontera de frame del set de overlays Enqueue. Capa 2 (Equirect estática) llega siempre con `frame=0`, capas 3/4 con `frame=N`; `klovrp_EnqueueSubmitLayer2` hacía flush al cambiar frameIndex → cada frame publicaba {2} y luego {3,4}; el compositor (90 Hz, otro hilo) pillaba uno u otro → quad head-locked 3 (cover a 0,5 m) y capa 2 parpadeaban. Evidencia: census alterna `overlay set (2): #3 #4` / `(1): #2` en cada frame (log 48 l.1875–2178). Descartados: gpu.err (1, blit 1x1 al arrancar), draws_skipped=0, skybox nunca OFF.
  - `klepton/runtime/xr/kl_ovrp.c`: `klovrp_enq_endframe()` (al inicio de `klovrp_EndFrame4`) publica el set completo; tras ver EndFrame4 el frameIndex ya no parte el set (solo id repetido como red de seguridad). Mutex `g_enq_mu` en submit/flush. Guests sin EndFrame4: heurística antigua.
- Mirar en log siguiente: `[ovrp] overlay set boundary = ovrp_EndFrame4`, census estable `overlay set (3 layer(s)): #2 … #3 … #4`, `[plat] unimplemented … -> neutral`, sin fatal de plataforma.
- Observado: el juego deja de enviar frames hasta ~2 s en cargas (`207 stale in a row`); es el propio juego cargando.

### AoT 47: carga infinita al entrar en partida (música sí, imagen congelada)
- Evidencia: serial del compositor congelado en 10300 desde 10:50:08 (`5770 stale in a row`), `vk.submit` se para en
  n=10312, sin más JNI/Unity. Últimas líneas del guest: `ovrp_EnqueueSetupLayer2 -> layer 2, Quad, 2048x512` (quad
  de la pantalla de carga) y `first-call ovrp_GetLayerTexturePtr` desde libil2cpp (hilo de juego); NUNCA sale
  `GetLayerTexture2: layer 2 …` → el hilo de juego quedó dentro de `kl_vulkan_layer_image` → `klvk_image_alloc` →
  `klvk_image_clear_black` (QueueSubmit + WaitForFences en el hilo de juego) mientras el hilo de render de Unity hacía
  su vkQueueSubmit. vkQueueSubmit exige sincronización externa por cola; nuestros submits internos no la tenían.
- `klepton/runtime/gfx/kl_vulkan.c`: `g_qsub_mu` + `klvk_qsubmit()`; todos los `d->QueueSubmit` internos pasan por él
  y `klvk_QueueSubmit` (el del guest) toma el mismo lock solo alrededor de `real()` (no del shadow pump ni de esperas).
  Capas no-ojo (`kl_vulkan_layer_image` con key != ojo) ya no hacen el clear síncrono (`t_klvk_no_clear`; el
  compositor ya salta capas no escritas). `KL_VK_LAYER_NOCLEAR=0` lo reactiva.
- `klepton/visionos/Sources/kl_app.c` (heartbeat): vigilante de bloqueo. Si el serial OVRPlugin no avanza 8 s en
  `frame pump` (tras >30 frames), `kl_sample_snapshot` de todos los hilos (`[snap] guest threads at guest frame stall`),
  y otra a 40 s. `KL_STALL_SNAP=0` lo apaga. Sirve para cualquier juego OVRPlugin (no OpenXR).
### Rendimiento (AoT 47; mismo patrón que Batman)
- Menú: ~33 fps de guest (631 submits/20 s), `gpu_ms` p50 22 / p90 27 ms con solo ~204 draws; pase principal
  RGBA8Srgb 2976x1912x2 = 11.6 ms. En partida 15–25 ms.
- **Cuello principal común: VPKTAA** (`[taa] stats … async queue gpu=18.8–24.6 ms/frame (max 40)`): misma GPU que el
  juego → ~40–45 ms de GPU por frame del guest. Pendiente (dueño de KleptonTAA/VPKTAACore): debería costar 1–3 ms;
  perfilar sus pases, bajar a la resolución de salida o desactivarlo para A/B.
- Compilación de shaders: `gpu.shader.lib hit=0 miss=1440` con índice de 658 entradas cargado → 0 % de aciertos
  (1092 MSL compilados, 25 s, máx. 766 ms); primer run de build nueva puede invalidar → comprobar en un 2.º arranque
  con la misma build. Al entrar en partida `gpu.pso ms=7178 max 886/2814`, `gpu.hold 1504 ms timeouts=1`,
  `missing_pipelines=12` (tirón de carga, no fps sostenido).
- `gpu.err archive_save pipelines-38.metalar entries=84 Failed_to_generate_machO: cannot pack` (VPKEngine; los split
  de 23/33 entradas sí se guardan) y 18× `vk.err shader compute "internal error: vector"` (emisor MSL de VPKEngine;
  `dispatch_skipped=6`/frame). Ambos en VPKEngine, no en este repo.
- Ojo 2976x1912: ovrp recomienda 2383x1912 (base original) y Unity aplica su escala ~1.25 en ancho.
### Comprobar en el próximo log (AoT 47 / rendimiento)
- AoT: tras `SetupLayer -> layer 2, Quad` debe salir `GetLayerTexture2: layer 2 …` y el serial sigue subiendo.
- Si vuelve a congelarse: bloque `[snap] guest threads at guest frame stall` → hilo y función donde está parado.
- `gpu.shader.lib hit=` > 0 en el segundo arranque con la misma build.

## Resumen logs 24–43 (detalle en git)
- VPKTAA (ruta async, ya sustituida por INLINE): core reutilizable `VPKTAACore.swift` (API en su cabecera: color
  2DArray, tangentes sin jitter, jitter, crop/cropOff, pose del ojo, depth `VPKTAADepthInput`, MV `VPKTAAMotionInput`);
  reproyección por profundidad, MV AppSW, desoclusión, clip YCoCg, RCAS fusionado, AA espacial; profundidad elegida
  en `kl_vulkan.c` (`klvk_ed_*`, prioridad pase-ojo > SpaceWarp > tamaño), reversed-Z por clear/censo; env
  `KL_VPKTAA_NEAR/FAR/REVZ/MV/MV_SCALE/MV_SIGN/MV_MAP/DEPTH_MAP/DISOCC`.
- SpaceWarp: flags 0x300 (AC/AoT) con `GetLayerTextureSpaceWarp`; MV size en el desc del ojo; Batman (0x1000) no la usa.
- Overlays/compositor: rect de textura y color por capa, `SetColorScaleAndOffset` (fundidos), origen abajo (bit 3),
  imagen propia de capa si el juego la escribe, OVROverlay con `VkImage*` en celda estable, vídeos 180° SBS por capa
  Equirect, skybox de carga solo con vídeo vivo, HUD FPS, capturas de diagnóstico, OpenXR color scale/bias.
- Sistema/JNI: Object.toString/hashCode/equals, NO-OP de métodos void del juego, excepciones JNI por hilo, pthread
  attr/sched, OpenSL volume/feeder, AAsset 64 bits, índice genérico de la carpeta del juego (obb), imágenes nuevas a
  negro, audio 40 ms, MSAA del juego configurable (`vq.gameMSAA`).
- Rendimiento Batman resuelto en log 26 (~90 fps; VPKEngine locales en slots por variable, `gpu.hold`).

## Resuelto en logs 8–22 (referencia breve; detalle en git)
- Vulkan en Batman: spoof Adreno 740 / driverVersion bit 31, `ovrp_GetDisplayAdapterId2` → NULL (log 7–8),
  `-force-vulkan` vía extra `unity` del Intent, AssetFile StatusByName como petición fallida.
- Sistema: StatFs, ELF TLS (TLSDESC), liblog completo, libdl namespaces, AAudio MMap, OpenSL PCM_EX float y
  GetConfiguration, AudioManager.getDevices/AudioDeviceInfo → audio OK desde log 17.
- Gráficos VPKEngine: point_size, SubpassData como Texture y D2Array con multiview, VkComponentMapping → swizzle
  (fuentes OK log 22), pipelines fuera del mutex + MTLBinaryArchive persistente.
- XR: zNear/zFar, Time de poses, RelativePose EyeLevel con altura medida por el Vision Pro, LateLatching off,
  postura auto/sentado/de pie, recentrado con PS derecho largo, PS izq = Start, toques capacitivos, manos ocultas,
  idioma del sistema (`Locale.preferredLanguages`), versión OVRPlugin leída del APK.
- Foveado Quest siempre apagado (ovrp, OpenXR FB/META, extensiones Vulkan FDM/VRS ocultas).
- Pendiente antiguo aún abierto: aborto final `mutex lock failed: Invalid argument` (log 22); Boundary «no configurado».

## Arquitectura (referencia)
- Base Klepton (MIT, copia `ce74582` en `klepton/`, ver `THIRD-PARTY-NOTICES.md`): cada `.so` ARM64 se envuelve en un
  dylib firmado (`tools/klepton_ld.c` en proceso, `klepton_ld_run`) y se carga con `dlopen`; relocs ELF propias;
  bionic/JNI/OVRPlugin/OpenXR propios. Sin traducción de CPU.
- Gráficos: VPKEngine VPVulkan.framework (`scripts/build-vpvulkan.sh`, cacheado por hash de `vpkengine/gpu/**`) en lugar de
  MoltenVK (`VQSettings.useVPKEngine`, `KL_MVK_DYLIB`). VisionQuestPro usa `gpu/metal2`.
- Firma: conversión sin firmar en `QUEST/Cache/<j>/dylibs`; copias firmadas en
  `Library/Application Support/VisionQuestPro/Signed/<j>/` (`VQPrepare`, `VQSigner`, variantes req4k/req16k/plain).
- App (`app/Sources/`): `VQLog`, `VQFolder` (QUEST: Juegos/Cache/Partidas), `VQGames`, `VQPrepare`, `VQLaunch`
  (+ `VQSettings`, todos los ajustes en la app), `VQLauncher`; `app/Native/vq_apk.c` (ZIP + AXML), `vq_config.c`
  (reenvío `vpvk_config_set_*`).
- CI `.github/workflows/visionos.yml`: solo `[build]` o manual; release `visionos` (`VisionQuestPro.ipa`, bundle
  `com.kdt.livecontainer`); logs de CI en rama `ci-logs` (`visionos.txt`).

## Batman: Arkham Shadow (com.camouflaj.manta, solo estudio)
- Unity 2022.3.45 IL2CPP, Vulkan multiview, SRP propio «Manta» (derivado de HDRP), OculusXRPlugin → libOVRPlugin
  1.98; Wwise (OpenSL); `supportedDevices=quest3+`. x18 usado como registro normal (Klepton lo parchea).
