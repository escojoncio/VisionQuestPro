# Bitácora — VisionQuestPro

Historial completo de sesiones anteriores: `git log -p bitacora.md` (compactada el 2026-10-12 tras el log 25).

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
  `ShaderWarmupManager.PopulatePSOCache`) → pendiente VPEngine (desbordamiento del argument buffer); quads del juego
  en EyeLevel ahora a la altura de ojos (antes en el suelo).
### Comprobar en el próximo log
- AC: `[plat] ovr_User_GetLoggedInUser -> request N` (no «-> 0»), sin «Request failed», `[plat] language pack: en.lang`,
  líneas `[fs] … (obb diag)` alrededor de «Package path couldn't be set», y si pasa del Background-Looping.
- Unity/UE4 en `.full`: no se ve la habitación; `[pt] immersion -> mixed` solo si el juego pide passthrough.
- Juegos EyeLevel: quads del menú a la altura de los ojos; `GetTrackingTransformRelativePose origin=1 (cur=0)` y≈−altura.

## VPKTAA: voto de jitter, RCAS fusionado, fallback sin TAA (2026-10-13, sin build)
Log 46 (Batman): entradas OK (jitter ON, depth D24S8 live s0/s1 revZ=1, near 0.05 por defecto, sin MV), depthValid 53 %,
disocc 0,5 %, rejected 6,5 %; pero resolves ≈30/s con juego ≈50 fps, gpu=21,6 ms/resolve (compartiendo GPU) → saltos.
- `VPKTAACore.swift`: `VPKTAAEyeInput.jitterAlt` (offset anterior/siguiente); `Params` + `shiftMain`/`shiftAlt`
  (304 B); voto de emparejamiento de jitter en píxeles stat (bordes, estáticos) → `stats[11..13]`,
  `VPKTAAStats.jitterVotes`. RCAS FUSIONADO en `vpktaa_resolve` (grupo 16x16 posee 14x14, anillo de 1 px en
  threadgroup `rt[256]`); kernel `vpktaa_rcas` eliminado; sumas de stats saltadas con `simd_any(statPx)`.
- `KleptonTAA.swift`: `pairedJitter` deduce k Halton del registro y aplica `jitterLag`
  (`KL_VPKTAA_JITTER_LAG`=auto|-1|0|1; auto cambia con ≥300 votos, alt ≥50 % y ≥2× usado, invalida historial);
  `acquire(for:newestSerial:)` devuelve nil si el resolve va ≥2 frames por detrás → se muestra el frame del juego
  sin TAA; stats: `raw-fallback=N frames`, `jitter votes (used,prev,next)=… lag=N`.
- `KleptonCompositor.swift` `prepareTAA`: pasa `rendered.serial` a `acquire`.
- Sin tocar: copia de depth en kl_vulkan (el guest comparte una depth entre stages; el resolve asíncrono la leería
  ya sobrescrita). Resolución: ya resuelve a la del juego (tamaño del color), no a la del compositor.
Pendiente: verificar en log `gpu=` ms y `resolves=` vs fps del juego, `raw-fallback`, votos de jitter; si el coste
sigue alto con GPU compartida, valorar media cadencia del juego o bajar resolución; near real de Batman (0,1?).

## VPKTAA completo (2026-10-13, sin build aún) — profundidad universal + MV AppSW + reproyección por píxel
Módulo reutilizable (también para el port Vulkan de Alyx): `klepton/visionos/Sources/VPKTAACore.swift` (solo
Metal+simd). Adaptador Klepton: `KleptonTAA.swift`. API del core (doc en la cabecera del fichero):
`VPKTAAEyeInput{color(2DArray), slice, flipY, tangents l r t b SIN jitter, jitter (tangentes), crop/cropOff,
orientation+position (cámara del ojo, mundo, m), depth: VPKTAADepthInput?{texture, slice, reversedZ, near, far(0=∞),
mapsCrop}, motion: VPKTAAMotionInput?{texture, slice, scale (texel→Δcrop-uv, cur−prev, signo incluido), mapsCrop}}`;
`encode(cmd:eye:input:output:)` (output = vista rgba8Unorm de la salida sRGB), `takeStats(eye:)`, `invalidate()`.
- Shader (`vpktaa_resolve`, tg 16x16): tile 18x18 half en threadgroup (1 lectura por texel); pesos 3x3 de
  reconstrucción calculados en CPU (el desplazamiento del jitter es constante por frame: `shiftInt` + fracción);
  profundidad: la más cercana de centro+4 diagonales → distancia lineal (reversed: `n·f/(n+d(f−n))`, f=0 → `n/d`;
  estándar análogo; d=0 rev / d=1 std = cielo) → P=dir·dist → vista previa (`rot`=Rprev⁻¹Rcur, `trans`=Rprev⁻¹(Tcur−Tprev),
  ojo = cabeza·`kl_ovrp_eye_local`) → uv previa; MV de objeto aplicado antes (cuv − mv·scale, muestreado en el texel de
  profundidad más cercana); sin profundidad/cielo = solo rotación. Historial CR 5 taps rgba16f + distancia previa r16f
  (gather 2x2; desoclusión si ninguno a < `KL_VPKTAA_DISOCC`=0.1 relativo → peso 0); clip YCoCg (γ 1,25 jitter /1,0);
  −25 % de historial a ≥32 px/frame; Karis; RCAS aparte (o escritura sRGB directa si nitidez 0). Votación de signo MV
  (4 variantes ±x±y en píxeles de estadística que se mueven).
- `kl_vulkan.c/.h` (solo con `KL_VPKTAA=1`, `KL_VPKTAA_DEPTH=0` lo apaga): tablas depth-images (`klvk_depth_info_*`,
  vkCreateImage), render passes (`klvk_rpd_*`: índice depth + loadOp CLEAR), framebuffers (`klvk_fbd_*`); en
  vkCreateImage las depth del tamaño del ojo (1x o forzadas 1x por `VPVK_FORCE_1X`) ganan SAMPLED y pierden TRANSIENT;
  storeOp depth DONT_CARE→STORE en vkCreateRenderPass/2 y vkCmdBeginRendering; `klvk_ed_note_pass`/`klvk_ed_candidate`
  en cada begin: 1 = pase con la imagen ojo adjunta, 3 = depth de tamaño ojo (último gana); 2 = depth SpaceWarp si se
  escribió ese frame. Prioridad 1 > 2 > 3 (1 es resolución completa y mismo layout). `klvk_ed_frame_done` (en
  `kl_vulkan_frame_done`, ANTES del QueueWaitIdle): registra por stage/ojo; si la depth es compartida entre stages la
  copia (`klvk_ed_snapshot`, vkCmdCopyImage aspecto DEPTH en la cola del juego) a una imagen propia por stage.
  Reversed-Z: clear del pase ojo (0.0 → reversed) o censo de depthCompareOp en vkCreateGraphicsPipelines.
  API: `kl_vulkan_eye_depth_get(stage, eye, want_tag, &kl_vulkan_eye_depth)`, `kl_vulkan_eye_depth_mtl(...)`,
  `kl_vulkan_eye_depth_tag(stage, serial)`. Mapa vista→imagen ahora en anillo (antes dejaba de registrar a las 8192).
- `kl_ovrp.c/.h`: EndFrame4 etiqueta la depth con el serial del registro archivado (0 si se descartó);
  `kl_ovrp_eye_local(eye, off, rot)`; `kl_ovrp_render_pose.depth_near/depth_far` (desde el submit si
  `KL_VPKTAA_NEARFAR_OFF=0x…`); volcado `[asw]` ampliado a +0x128 y cada 1800 frames. En el volcado del log 43:
  Fov[2] (Up Down Left Right) en +0x104..+0x123; +0x124 = 1e7 (¿FarZ?); near no localizado → default.
- `KleptonTAA.submit(newest:wait:sources:)`: elige el registro más nuevo cuya depth ya está etiquetada (tras el idle
  del juego ⇒ el ojo también está completo); si ninguno en `KL_VPKTAA_DEPTH_WAIT_MS`=25 ms, el más nuevo sin depth
  (`depth-late`). MV si `rendered.asw & KL_OVRP_ASW_MV`. Mapeo depth/MV: textura del tamaño del color = su layout; otro
  tamaño = cubre el rect crop del ojo (MV AppSW 1549x1243 para crop 3029x2485 de 3840x2485). Rechaza MV si el aspecto
  difiere > 5 %.
- Env: `KL_VPKTAA_NEAR`=0.05, `KL_VPKTAA_FAR`=0 (∞), `KL_VPKTAA_REVZ`=0/1 (forzar), `KL_VPKTAA_MV`=0 (sin MV),
  `KL_VPKTAA_MV_SCALE`=0.5, `KL_VPKTAA_MV_SIGN`=auto|1|-1 (auto: empieza +x+y y cambia si un signo gana ≥55 % de ≥200
  votos), `KL_VPKTAA_MV_YFLIP=1`, `KL_VPKTAA_MV_MAP`/`KL_VPKTAA_DEPTH_MAP`=full|crop, `KL_VPKTAA_DISOCC`,
  `KL_VPKTAA_DEPTH_COPY=0`, `KL_VPKTAA_DEPTH_WAIT_MS`.
- Supuestos: Unity/Unreal Vulkan = reversed-Z; near 0,05 m (para la prueba de desoclusión relativa el near se cancela;
  solo afecta al paralaje de la traslación); MV AppSW = solo objetos, NDC cur−prev (signo verificable con los votos).
### Comprobar en el próximo log
- `[taa-depth] eye-sized depth image … usage` y `[taa-depth] frame N stage S: eye depth = eye-pass … live|copied …
  reversedZ=1`; si sale `no eye depth found`, mirar cuántos pases tenían la imagen ojo.
- `[taa] inputs … eye0 depth=eye-pass live D24S8 … revZ=1(clear 0.000) near=0.050(default) far=inf mv=live …`
  (o `REFUSED: …`). `[taa] stats`: gpu ms (objetivo ≤3), depthValid %, disocc %, depth-late, mean|mv|, votos MV.
- AC: volcado `[asw] … +0x128` cada 1800 frames para localizar NearZ/FarZ.
- Limitaciones: locomoción artificial (el rig se mueve, la pose de tracking no) → la reproyección por cámara falla y lo
  absorben desoclusión + clip; MSAA del juego activo (sin forzar 1x) → depth multisample no usable (se registra);
  OpenXR XR_FB_space_warp / GLES sin depth; RCAS sigue en un segundo dispatch.

## Log 43 AC Nexus (2026-10-13, VPKTAA on) → build siguiente
- Vídeos: el juego los pone en una capa Equirect con Surface Android (desc flag 0x80, texL=0, capas 4/23/31/38/40)
  y declara la matriz de rect: ojo L `src=[0 0 0.5 1] sb=[1 1 -0.25 0]` → SBS, destino u 0.25..0.75 = 180° delante.
  Ficheros: Ubisoft-Logo 5760x2880, Background-with-Initialisation-Loop 2880x2880, Background-Looping 1920x1920
  (todos SBS 180). El skybox los pintaba mono con `hscale=2` (frame entero en 180° → imagen doble, sin estéreo).
  `KleptonCompositor.swift`: `SkyboxLayout`, `skyboxLayoutFromLayer` (d=(src−b)/s → hspan/vspan, rect por ojo
  pasado a origen arriba-izq., yaw del centro + pose de la capa), `skyboxLayoutFromFrame` (nombre _LR/_SBS/_TB/_OU/
  180/360, si no aspecto vía `HostVideoPlayer.layout`), `drawLoadingSkybox` en modo angular por ojo
  (`KL_EQ_LEGACY=1` = antiguo). `encodeHostVideo` usa también el rect de la capa Equirect. Log
  `[cp] loading skybox layout: … from layer N: L=[…] R=[…] span 180x180 deg`.
- Ojo nunca compuesto en todo el log (skybox ON desde el frame 0, nunca OFF): el disparador era «Equirect vivo» y AC
  casi no destruye capas. `kl_ovrp.c` `kl_loadvid_recount`: solo Equirect de vídeo (flag 0x80 o
  `GetLayerAndroidSurfaceObject` pedido) ENVIADO en los últimos `KL_LOADVID_GRACE`=45 EndFrame4 (setup cuenta);
  `kl_loadvid_tick` en EndFrame4; `submit_tick` en EnqueueSubmitLayer2 y en la lista de EndFrame4.
  `KL_LOADVID_ANY_EQ=1` = regla antigua. Log `[ovrp] loading-video skybox ON/OFF (…)`.
- Proyección simétrica (Quest 3): viewport ojo0 `0,0 2347x1912`, ojo1 `629,0` de 2976 (78,9 %). El composite ya
  recorta por ojo, pero VPKTAA solo usaba el tamaño → ojo derecho resuelto con 629 px de desfase (historial mal
  reproyectado, franja derecha sin TAA). `KleptonTAA.swift`: `Params.cropOff`, `cuv=(uv−off)/crop`, muestreo
  fuente/historial con offset, reset al cambiar el rect, crop solo si `viewport_of` = textura; dispatch limitado al
  rect (+1 texel de margen, buffer(2) `org`) → ~21 % menos coste (TAA estaba en 20,3 ms/frame a 3840x2485).
- Diagnóstico: `klovrp_enq_census` (`[ovrp] overlay set (N): #id Forma WxH@(x y z) f… notex rect;` al cambiar el
  conjunto, máx. 40). Capas 6/9/10/16 de AC se componían en ~(0,0,0) m: comprobar con el censo.
- Sin fallos de traductor VPEngine en este log (sin `internal error`, arg_overflow ni draws saltados; solo
  `missing_pipelines=4487` en el frame de warmup f=846 y `archive_save` partido OK). MV depth de SpaceWarp: en la
  1.ª capa ojo Unity crea vistas D24S8 sobre el handle salvaje 0x1041bf078 (scratch), «0 with depth written».
### Comprobar en el próximo log
- `[cp] loading skybox layout` con `from layer`, span 180x180, L/R mitades; vídeo en estéreo delante.
- `[ovrp] loading-video skybox OFF` al pasar a partida y `[cp] first render viewport read`; `[taa] eye 1 renders
  into an offset rect` y `[taa] stats` gpu < 20 ms. Pendiente si en el menú faltan punteros/manos: el ojo no se
  dibuja sobre el skybox (haría falta mezcla alfa tipo underlay; mirar captura del ojo con «Capturas»).

## Logs 39–41 (2026-10-13) → build siguiente
- SpaceWarp: AC y AoT llevan `flags 0x300` = bit 8 SpaceWarpDataAllocation + bit 9 SpaceWarpDedicatedDepth y SÍ llaman
  `GetLayerTextureSpaceWarp`; Batman (0x1000) no lo usa (SetSpaceWarp forzado sin efecto, inocuo). La fase 1 servía MV
  con desc MV size 0x0 → `RenderTexture.Create failed`, Unity renderizaba 2 de cada 3 frames en la imagen MV/scratch
  (parpadeo). `kl_ovrp.c`: `KLOVRP_LAYERFLAG_SPACEWARP_ALLOC=(1<<8)|(1<<12)`, `_DEPTH=(1<<9)|(1<<13)`; rechazo si el
  desc no lleva MV size (`KL_VPKTAA_ASW_NOSIZE=1` sirve igual). `kl_vulkan.c` `klvk_eye_fb_note` (diag stages eye).
- Overlays: bit 3 `TextureOriginAtBottomLeft` (0x8) → `origin_top_left=0` en Vulkan
  (`klovrp_overlay_origin_top_left`, `KL_OVRP_LAYER_BL=0`). Murciélago de Batman y quad de AC al derecho.
- AC (39): abort `GetObjectClass(NULL)` desde libubiservices (Activity del puente Java). `kl_jni.c`: soft null-class
  por defecto (`KL_JNI_SOFT_NULLCLASS=0`), receptor NULL en método no-Object de Object → Activity real.
  `kl_shim.c`: `__memchr_chk`, `__fread_chk`, `__assert`. Vídeo de carga: `kl_jni_services.c` guarda el vídeo pedido
  (`kl_loadingvideo_request_gen/name`); compositor lo reproduce con sonido (bucle si el nombre contiene «Loop»),
  retiene 4 texturas de frame; al cerrar el skybox se olvida la petición.
- AoT (40): audio entrecortado: feeder OpenSL hacía pop antes de copiar. `kl_opensl.c`: peek → write → retire
  (`clears`), `GetState.count` incluye el buffer en curso. Pendiente VPEngine: `internal error: vector` en un compute
  (vk.comp14…347) y `gpu.err arg_overflow=16090 skipped_draws` en f=348.
- Pendiente VPKTAA: fase 2 con MV reales (AC/AoT); vía universal por profundidad desde VPEngine (Batman); bajar el
  coste GPU del TAA (12,7 ms/frame en cola compartida).

## Logs 36–38 (2026-10-13) → build siguiente
- AoT (36) y AC (37) abortaban: tras `throw FileNotFoundException(patch obb)` Unity llama `Object.toString()` →
  «no host implementation». `kl_jni_android.c`: `klj_Object_toString` (String → sí mismo; throwable →
  "pkg.Clase: msg"; resto "pkg.Clase@hash"), `klj_Object_hashCode` (String: polinomio 31 de Java; resto puntero),
  `klj_Object_equals` (identidad). Fallback global vía `klj_find_binding("java/lang/Object", …)`.
- AC (37): índice OK: `<juego>/com.Ubisoft.ACNexusVR/` con start0/1/2, en.lang, *.climb, bundles → obb dir correcto.
- Batman (38, VPKTAA on, force_1x=1): `[asw]` desc con MotionVectorTextureSize 1192x956 pero
  `GetLayerTextureSpaceWarp` nunca llamado; submit flags 0x24 siempre; bits=0 en 2400 frames. Cadencia: 3 frames
  tarde en todo el log. TAA gpu 12.7 ms/frame (cola compartida con el juego), rejected 18 %.
  `kl_ovrp.c` `klovrp_asw_frame`: con VPKTAA y flag 0x1000, desde el frame 90 llama una vez a la export del guest
  `SetSpaceWarp`/`SetAppSpaceWarp`(1) (`KL_VPKTAA_FORCE_SW=0` lo evita). Sin limitar a media cadencia.
### Comprobar en el próximo log
- `[asw] guest export …` y si aparecen `GetLayerTextureSpaceWarp #n`, escrituras MV/depth y flags de submit nuevos.
- AC: pasa de la carga; AoT: `[sl]` volume, sin fault CRI, `[snap]` si se cierra el display.

## Logs 33–35 (2026-10-13): AoT crash, AC carga infinita, VPKTAA → build siguiente
- Batman (35): compositor 100 Hz sin fallos (TAA asíncrono OK), 1 sola gpu.hold (108 ms). VPKTAA ciego (sin MV/depth):
  rejected 10–30 %, borroso; juego GPU 14 ms (~50 fps). Usuario prefiere MSAA del juego.
  App: `gameMSAA` (`vq.gameMSAA`, defecto ON, `force_single_sample = gameMSAA ? 0 : 1`), VPKTAA defecto OFF
  (`vq.vpktaa4`), textos «experimental».
- SpaceWarp fase 1 (solo `KL_VPKTAA=1`, `KL_VPKTAA_ASW=0` lo apaga): Batman pide `flags=0x1000`
  (SpaceWarpDataAllocation), `mv=2/7` (RGBA16F / D24_S8), `depth=10` (None) pero nunca llamaba
  `GetLayerTextureSpaceWarp`: el descriptor iba con MotionVectorTextureSize 0. `kl_ovrp.c`: `klovrp_fill_eye_desc`
  rellena mv size = ojo / `KL_VPKTAA_MV_DIV` (2); `klovrp_GetLayerTextureSpaceWarp` (Vulkan) entrega MV + depth por
  fase (2 capas multiview); `klovrp_asw_frame` (EndFrame4) vuelca flags y +0xa8..+0x130 del submit del ojo.
  `kl_ovrp_render_pose.asw/asw_submit_flags`. `kl_vulkan.c`: tabla ASW, `kl_vulkan_spacewarp_mtl`,
  `kl_vulkan_spacewarp_writes`. Capturas `aswmv*`. Numeración formatos depth hipótesis (D16=6 … None=10).
  Fase 2 pendiente: offsets de escala/offset MV, delta pose, near/far del volcado; TAA con reproyección por píxel
  (MV dilatado 3x3 por profundidad más cercana), desoclusión por profundidad, fallback rotación.
  No cubierto: OpenXR `XR_FB_space_warp` (Unreal) ni GLES.
- AC Nexus (34): `FileNotFoundException …/Android/obb/com.Ubisoft.ACNexusVR/start0` ×4317 (obb dir = raíz del juego).
  `VQLaunch.swift`: obb dir = `<juego>/<paquete>` si existe; `kl_jni_android.c` `klj_obb_descend_package` +
  `klj_obb_list`; `kl_libc.c` índice genérico de la carpeta del juego (`kl_gd_find`: fallo en
  `<ext>/Android/obb/<pkg>/…` → busca por sufijo de ruta, nombre suelto permitido; en `Android/data/<pkg>/…` sin
  nombre suelto; caché 256), logs `[path] game folder …` y `obb MISS`. Destellos morados: compositor pintaba el ojo
  viejo mientras el vídeo de carga no tenía frame → ahora negro + overlays (`loadingVideo != nil`).
- AoT (33): SIGSEGV 0x0 en libcri_ware_unity+0xbeed8 tras `GetInterface: unsupported` (SL_IID_VOLUME).
  `kl_opensl.c` SLVolumeItf completo (ganancia en feeder), `*out=NULL`, más IIDs; `kl_ndk.c` AAsset_seek,
  openFileDescriptor64, getRemainingLength(64) (off_t 64 bits), isAllocated, AAssetDir_rewind. JNI: excepciones
  reales por hilo (`klj_exc_set` pin/unpin, `KL_JNI_EXCEPTIONS=0`), ZipFile.<init> lanza FileNotFound/Zip/NPE.
  `kl_sample_snapshot` al primer DestroyDistortionWindow (`KL_OVRP_TEARDOWN_SNAP=0`).
### Comprobar en el próximo log
- `grep "\[asw\]"` (Batman con VPKTAA); `[path] game folder` / `obb MISS` (AC); `[sl]` volume y `[snap]` (AoT).

## Log 32 (2026-10-13): VPKTAA lento / a saltos → build siguiente
- Log 32: vpktaa=1 force_1x=1, base «compositor» (eye 3072x2465 vs 2290x2400 del juego), gpu.time media 19 ms
  (895/1252 > 14 ms → juego ~50 fps), cadencia compositor 12–17 ms con 9–16 frames fuera de plazo por medio segundo:
  el TAA (2x7,6 Mpx, resolve + RCAS) iba DENTRO del command buffer del compositor. rcas=1.0 (nitidez máxima).
- `KleptonTAA.swift`: cola Metal propia. `submit` (guest frame nuevo, 1 trabajo en vuelo, espera el fence GL si lo hay
  vía `taaWait`), 3 slots de salida rgba8 sRGB (escritos por vista rgba8Unorm + `enc_srgb`), `acquire(for: cmd)` fija
  el slot hasta que acaba el composite, `invalidate` + `generation`. El compositor muestra el último resolve TERMINADO
  con SU pose (`taaPose` sustituye a `rendered` en `encodeViews`) → nunca pierde plazo por el TAA.
  Stats: muestreo 1/64 (antes desbordaba uint32: «rejected=132.9»), `NSLog("%@")` (el «%» rompía el formato),
  `gpu=… ms/frame` medido en la cola TAA.
- HUD FPS (`KL_FPS_HUD`, ajuste «Mostrar FPS», `vq.showFps`): panel head-locked (overlay pipeline, depth always,
  `srgb_decode=0`): fps juego (serial), Hz visor, resolución de render, VPKTAA on/off; textura CoreText cada 0,5 s.
- MSAA del juego apagado siempre (`force_single_sample=1`, quitado `dropGameAA`). Aviso de cadencia con n ≥ 15.
- Ajustes: textos explicativos de «Intensidad del suavizado» (peso de historial 0,5–0,92) y «Nitidez» (RCAS).
- `kl_pthread.c` inheritsched con numeración bionic (EXPLICIT 0, INHERIT 1).

## Logs 29–31 (2026-10-13): crashes de AC Nexus y Attack on Titan → build siguiente
- AC Nexus (log 30): abort JNI en `LoadingScreen.startUpdatingLoadingScreen(String,Context,Surface,II)V`.
  `jni/kl_jni_services.c` `klj_LoadingScreen_startUpdating` (junto a `stopUpdatingLoadingScreen`): registra ruta,
  surface, capa (`kl_hostvideo_surface_layer`) y w×h; no hace nada más.
- Genérico `kl_jni.c` `klj_game_void_noop` (en `klj_call_common`, antes del abort): método `V` (no `<init>`) de clase
  que no es del framework (`android/ java/ javax/ dalvik/ kotlin/ kotlinx/ com/android/ androidx/
  com/google/android/`) → no-op con `[jni] NO-OP game Java void …` una vez por método. `KL_JNI_STRICT=1` restaura
  el abort. GetMethodID/GetStaticMethodID nunca fallan (`klj_want`), así que solo hacía falta en la llamada.
- Attack on Titan (log 31): `pthread_attr_setschedpolicy` sin resolver (libcri_ware_unity). `libc/kl_pthread.c`:
  attr get/set de stacksize, detachstate, stack, guardsize, schedpolicy, schedparam, scope (PROCESS→95), inheritsched
  (flags bionic 0x4/0x8) + `pthread_setschedprio`; `sched_get_priority_max/min` con semántica Linux (FIFO/RR 1..99,
  OTHER/BATCH/IDLE 0, resto EINVAL). Exportados en `kl_shim.c` (X()/E()); quitados los `KL_FWD` de
  `kl_libc_table.h` (generado: si se regenera, comprobar que no vuelven).
- `kl_image.c` `report_missing` al final de `bind_dynamic` (ELF y dylib): `[dl] <lib>: N unresolved import(s): …`
  (máx. 48 nombres) → todos los símbolos que faltan en una sola ejecución.
- `xr/kl_ovrp.c`: `ovrp_log_chain` (recorre x29 con límites de la pila del hilo, hasta 6 retornos de guest + método
  IL2CPP) en `DestroyDistortionWindow*` (vía `klovrp_ok`) y `DestroyLayer` (8 primeras); `ovrp_log_ret` para
  WaitToBeginFrame/BeginFrame4/EndFrame4 (5 primeras y cualquier fallo).
- Hipótesis AoT pantalla tras frame 0: nuestras respuestas son éxito (Wait/Begin/End devuelven 0); el proveedor
  Oculus (+0x1500c/+0x1509c) para el display tras el primer EndFrame4 sin pedir ShouldQuit/Recreate (Update3 y
  GetAppShouldQuit2 nunca llamados). Probable parada del subsistema desde C# (XR Management / script de arranque)
  tras el `NullReferenceException` (~l.1518) o el error de GUID del OBB (`got ''`, patch OBB ausente).
### Comprobar en el próximo log
- AC Nexus: línea `LoadingScreen.startUpdatingLoadingScreen …` y que sigue; lista de `NO-OP game Java void`.
- AoT: sin fatal de pthread; líneas `[dl] …unresolved import(s)`; `guest chain:` de DestroyDistortionWindow2 /
  DestroyLayer (¿método IL2CPP?) y `[ovrp] … #N ->`.

## Batman logs 27/28 (2026-10-12) → build siguiente
- Log 27: VPKTAA on + MSAA forzado 1x → todo rosa (ojo sin dibujar, arreglo en VPEngine `forcedResolveCopy`); compositor
  a ~58 Hz con VPKTAA a 3072x2465. Log 28: VPKTAA off (`vpktaa=0`); congelaciones `gpu.hold` hasta 1,35 s.
- Rosa a pantalla completa 1–2 s al abrir la inmersiva (todos los juegos): imágenes nuevas sin inicializar.
  `kl_vulkan.c` `klvk_image_alloc` añade TRANSFER_DST a las de color y `klvk_image_clear_black` las limpia a negro
  opaco (fence; si expira, desactiva; `KL_VK_CLEAR_NEW=0` lo apaga).
- App: VPKTAA activado por defecto (`vq.vpktaa3`); «Desactivar el MSAA del juego» independiente del VPKTAA
  (`vq.dropGameAA2`, def. sí; `force_single_sample = dropGameAA`).
- Audio: `kl_audio.c` latencia objetivo 80 → 40 ms (`KL_AUDIO_LATENCY_MS`).
- VPKTAA: guardas NaN/Inf por bits (`kbad`/`kbad1`) en resolve e historial y en RCAS.
- Capturas (`KL_CAPTURE`, ajuste «Capturas de diagnóstico»): frames 2/30/120/600/1200 + 45 frames tras el primer
  compuesto de cada capa overlay; añaden salidas VPKTAA (`capture-N-vpktaaE-0.png`, rgba16f → sRGB en CPU, NaN en
  magenta) y la imagen de cada capa (`capture-N-layerID-0.png`).
- Murciélago (capa 2, tira 2048x128 de 16 celdas): rect de textura correcto en log 28 (`src=[k/16 …]`), pero se ve
  cortado y sin animación. Log nuevo: `layer N image … written by the guest (vkCmdCopyImage|vkCmdBlitImage|render
  target) … source fmt F WxH` + captura de la capa para ver qué contiene.
### Comprobar en el log 29
- Sin rosa al abrir; con VPKTAA imagen correcta y `capture-*-vpktaa*`; `[taa] stats`; cadencia del compositor ~100 Hz.
- `gpu.pso.cache saved` sin fallos; menos/más cortos `gpu.hold`; segunda sesión casi sin ellos.

## Mejoras globales OpenXR/overlays (2026-10-12) → build siguiente
- OpenXR `XR_KHR_composition_layer_color_scale_bias` (v5, gate `KLXR_GATE_COLOR_BIAS`, `KL_XR_COLOR_BIAS=0` lo oculta e
  ignora): `kl_openxr.c` `klxr_layer_color_bias` (recorre `next`, máx. 16 saltos, tipo 1000034000). Capa de proyección
  → `kl_reproject_set_fade(…, all=0)` al final de `xrEndFrame` (solo si este camino lo puso: `xr_fade_owned`; no pisa
  el fundido de `ovrp_SetColorScaleAndOffset`). Quad/cilindro → `klxr_quad_record` rellena `color_on/color_scale/
  color_offset` (identidad = off). `klxr_quad_census` ignora los valores de color en la clave (sin spam en fundidos).
- `subImage.imageRect` del quad: ya cubierto (`viewport` en px → `uv_rect` en `kl_overlay_build`, misma convención de
  origen que la textura). Equirect OpenXR sigue sin componerse (nombrado una vez); cilindro → quad, hereda color.
- `kl_reproject.c/.h` `kl_overlay_color(ov, scale, offset)`: color propio, si no fundido global con `all`; usado por
  `kl_overlay_build` y por el equirect. `kl_msl_equirect` `KLEq` + `colorOn`(176) `colorScale[4]`(180)
  `colorOffset[4]`(196), stride 224 = `KLEqUniform` (Swift, tuplas de Float). `kl_eq_f`: rgb·scale+offset y
  ×saturate(scale.a+offset.a) (panorama opaco → funde a negro). `encodeEquirect` lo rellena; vídeo/skybox de carga en 0.
- Ruido `kl_ovrp.c`: `guest dlopen("libOVRPlugin.so")` solo #1–3 y potencias de 2 (`[open #N]`);
  `GetLayerTexturePtr` 16 primeras llamadas y luego solo si cambia el valor por (capa, stage, ojo), máx. 128 líneas.
### Comprobar en el próximo log
- Juegos OpenXR: `[xr] projection colour scale/bias: …` / `[xr] quad colour scale/bias: …` si el juego lo encadena.
- Batman: `[ovrp] guest dlopen … [open #N]` pocas veces; `GetLayerTexturePtr … (call N)` acotado.

## Batman log 26 (2026-10-12) → build siguiente
- Rendimiento resuelto: ~90 fps de juego, compositor 100 Hz, ningún frame >12 ms de GPU.
- Corrupción breve al inicio y en el visor de personajes: draws saltados por pipelines compilándose (caché de recetas
  vaciada por el cambio de traductor). VPEngine ahora retiene el frame (`gpu.hold`, ver su bitácora).
- Mapa cargando visible en vez de negro: el juego funde con `ovrp_SetColorScaleAndOffset` (9319 llamadas) y era no-op.
  `kl_ovrp.c` `klovrp_SetColorScaleAndOffset` (HFA s0–s3/s4–s7, bool w0) → `kl_reproject_set_fade`; `kl_reproject.c`
  `kl_reproject_uniforms.fade_on/fade_scale/fade_offset` (+ `KLReproj`, varyings planos) aplicado en `kl_reproject_f`
  (rgb·scale+offset). Overlays: con `all` y sin color propio. Log `[ovrp] colour fade …` (transiciones).
  `KL_OVRP_FADE=0` lo desactiva.
- Cuadrado rosa: la capa Quad 2048x128 es el icono de carga (tira de 16 fotogramas). `ovrp_EnqueueSubmitLayer2` lee
  ahora la cola de la firma (args 9–13 en pila: overrideTextureRectMatrix, &TextureRectMatrixf, overridePerLayerColor,
  &scale, &offset) → `kl_ovrp_overlay.rect_on/rect/scale_bias/color_on/color_scale/color_offset`; `kl_overlay_build`
  → `kl_overlay_uniforms` (+ `KLOv`); `kl_ov_f`: tex_uv = q·sb.xy + sb.zw, descarte fuera del rect, clamp medio texel,
  color por capa (`KL_OVERLAY_RECT=0` desactiva). Log `[ovrp] layer N submit tail: …`.
- Textura de la capa: `kl_vulkan.c` `klvk_layer_mark_written` (attachment de framebuffer, CmdCopyImage/BlitImage destino;
  flag en `g_layer_img[i].written`, se limpia al reclamar/expulsar, viaja con rekey) + `kl_vulkan_layer_written(key)`;
  `KleptonCompositor.swift` (quad y equirect) usa la imagen propia de la capa si el juego la escribió, si no texL.
### Comprobar en el log 27
- `gpu.hold` en el arranque/visor (sin `missing_pipelines` visibles); `[ovrp] colour fade` con scale 0 en las cargas;
  `layer N image … written by the guest`; `submit tail: rect=1 src=[k/16 …]`; icono animado sobre negro.

## Batman log 25 (2026-10-12) → build siguiente
- **Rendimiento**: meseta de 38–41 ms de GPU por frame de render (~22 fps de juego, ~95 en zonas ligeras) según hacia
  dónde se mira; pases idénticos en frames rápidos y lentos. Causa en VPEngine: todos los locales SPIR-V en un único
  arena `vp_private` → con un índice dinámico todo el shader va a la pila. Arreglo en VPEngine (slots por variable,
  ver su bitácora 2026-10-12 «log 25»). `app/Sources/VQLaunch.swift` `firstRunVersion = 2` (re-precompila con el
  MSL nuevo); VPEngine `kRecipeVersion = 2` (descarta recetas viejas: la 1.ª partida tras actualizar compila PSO).
- **Cuadrado rosa al empezar partida** (no era un vídeo roto): capa Quad 2048x128 de OVROverlay; `klovrp_GetLayerTexturePtr`
  daba el VkImage crudo y Unity (CreateExternalTexture en Vulkan espera `VkImage*`) lo desreferenciaba → handle
  salvaje 0xe → pintaba en el scratch. `klepton/runtime/xr/kl_ovrp.c`: con guest Vulkan devuelve la dirección de una
  celda estable por (capa, stage, ojo) (`klovrp_texcell`, 128 celdas, nunca se liberan); GLES sin cambios;
  `KL_OVRP_TEXPTR_RAW=1` vuelve al valor crudo. Log: `GetLayerTexturePtr(...) wrote <ptr> (VkImage … via cell)`;
  debe desaparecer `SUBSTITUTING scratch for wild handle`.
- **Vídeos**: en el log 25 el juego no tocó ningún .mp4 (ni stat, ni JNI de vídeo, ni AMedia*, ni capa Equirect).
  Sonda `kl_libc.c` `kl_media_probe_log` ampliada: `strcasestr` de .mp4/.m4v/.mov/.webm/.mkv en cualquier parte de la
  ruta, mutex, y llamada también desde `kl_fs_trace_open` (open/openat/__open_2) y `klb_fopen`.
- Ruido: `klovrp_Update3` dejaba `calls` de Update2 en 0 cada frame → 11k líneas `first-call ovrp_Update2`; ahora no
  baja de 1.
- Diagnóstico nuevo (VPEngine): `gpu.shader.mem … dyn_slots=` por shader y `gpu.time … enc_ms=` en frames lentos.
### Comprobado en el log 26 (rendimiento OK; resto en la entrada del log 26)
- `gpu.time` de la zona pesada (debería bajar de ~39 ms); si sigue alto: `enc_ms` (qué pase) y `gpu.shader.mem` con
  `dyn_slots>0`.
- Sin `wild handle 0xe`; el cuadrado del inicio muestra contenido (indicador de carga / texto).
- `[video] …` si el juego busca algún vídeo; sin `first-call ovrp_Update2` repetido; sin `archive_save` fallido.
- Pendiente si hace falta: perfilador automático de frames pesados (bisección por draws), tablas constantes en
  `constant`, mprobe de escena/objetos de vídeo (`kl_mprobe.c`).

## Batman log 24 (2026-10-12)
- Vídeo (descompilado con resolver IL2CPP propio): StereoVideoPlayer.Play @0x1da9d34 → path =
  Path.Combine(getObbDir, VideoFileName) → si !File.Exists vuelve false. PlayEnumerator espera Wwise
  AllSoundBanksLoaded → AndroidVideoPlayer.Play → OVROverlay Equirect (shape 5) isExternalSurface →
  GetLayerAndroidSurfaceObject → NativeVideoPlayer. Vídeos sueltos en OBB (DRM1..7, GothamToBlackgate): HEVC 8192x4096
  60 fps SBS equirect 180°; reproductor host `kl_hostvideo.c` + `kl_jni_hostvideo.c` + `HostVideoPlayer` (AVPlayer) en
  `KleptonCompositor.swift`. `OBBVideoWrapper` usa `/storage/emulated/0/Android/obb/<pkg>/X.mp4` → remap en
  `kl_libc.c` `kl_guest_path_inner`.
- VPKTAA (`KleptonTAA.swift`): rejilla sin jitter Blackman-Harris, Catmull-Rom, clip YCoCg, RCAS; jitter Halton en
  `klovrp_GetNodeFrustum2` si el resolve corre; por defecto OFF (`vq.vpktaa2`).
- Ajustes: aniso por nivel (`vq.anisoLevel`, def. 16), `VP_PASS_TIMING` («Diagnóstico de rendimiento»), resolución base
  original/compositor/nativa (`vq.resBase`), MSAA del juego a 1x con VPKTAA (`dropGameAA`).
- Shaders 1.ª vez: `app/Native/vq_harvest.c` (UnityFS + LZ4, ZIP, SPIR-V por magia) → `<cache>/spirv/*.spv` →
  VPEngine `vpvk_precompile_spirv` + `msl_index.bin`; stamp `firstrun.vN`.
- Passthrough automático: `kl_passthrough.c/.h`, ajuste Auto/VR/Mixto (`KL_PASSTHROUGH`).

## Resuelto en logs 8–22 (referencia breve; detalle en git)
- Vulkan en Batman: spoof Adreno 740 / driverVersion bit 31, `ovrp_GetDisplayAdapterId2` → NULL (log 7–8),
  `-force-vulkan` vía extra `unity` del Intent, AssetFile StatusByName como petición fallida.
- Sistema: StatFs, ELF TLS (TLSDESC), liblog completo, libdl namespaces, AAudio MMap, OpenSL PCM_EX float y
  GetConfiguration, AudioManager.getDevices/AudioDeviceInfo → audio OK desde log 17.
- Gráficos VPEngine: point_size, SubpassData como Texture y D2Array con multiview, VkComponentMapping → swizzle
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
- Gráficos: VPEngine VPVulkan.framework (`scripts/build-vpvulkan.sh`, cacheado por hash de `vpengine/gpu/**`) en lugar de
  MoltenVK (`VQSettings.useVPEngine`, `KL_MVK_DYLIB`). VisionQuestPro usa `gpu/metal2`.
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
