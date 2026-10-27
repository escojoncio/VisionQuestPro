# Bitácora — VisionQuestPro

## Last done
- 2026-10-27 VQP `VQRobust.purgeOnTranslatorChange` (VQLaunch.swift ~896-934): purga de la caché del juego (pipelines*.metalar/.keys/.count, recipes.bin, firstrun.*, aot.*; no spirv/ ni vq.robust) cuando `vpvk_translator_tag()` difiere de `<cache>/vq.translator`; sella solo si todos los borrados salen bien. Sustituye al sello único `vq.robustMigrated.v1` (queda huérfano). `ref:` → VPKEngine `6181b91`.
- 2026-10-26 VPKEngine `d0ff957` + VQP `VQRobust` (VQLaunch.swift): `VPVK_ROBUST` = `0` si el juego aprendió que no pide robustBufferAccess (`<VP_PIPELINE_CACHE_DIR>/vq.robust`, escrito por `klvk_robust_store` en kl_vulkan.c, OR pegajoso por proceso), si no `app`; aplicado antes del prewarm; sellos `firstrun.*`/`aot.*` llevan `.rb0/.rbapp`; purga única por caché (ahora por versión de traductor, ver arriba). Test `t_vk_robust`. Conformance sin VPVK_ROBUST.
- 2026-10-26 VPKEngine `51b78be` (main; `ref:` subido): `VPVK_ASYNC_PIPELINES=0` = vkCreate*Pipelines síncrono (semántica Quest; timeout 60 s, backoff 500 ms/5 s, FAIL_ON_COMPILE_REQUIRED); vpgles async salvo `VPVK_GLES_SYNC_PIPELINES=1`; archive depth-only aparte; RAM: TileLocal entre encoders limpios, `transientViolated`, depth de ojo TAA materializado. Logs nuevos: `vpk.version`, `vk.pso.census`, `vk.pso.fail`, `vk.hitch`, `gles.uniform_err`, `gles.tex`, `gles.pso.recipe`, `gpu.missing`, `gpu.mem.rt`. Captura VPCAP + `vpreplay`/`vpimgdiff` + CI lavapipe vs Metal (detalle: `notas/captura.md`).
- 2026-10-26 VQP: `VQLaunch.swift` pone `VPVK_ASYNC_PIPELINES=0` para juegos (barra de precache del juego real; antes async: ITR2 >8000 PSO «hechos» sin compilar). `kl_frame_boundary` (kl_bg.c) llama `vpvk_frame_boundary` 1/frame desde EndFrame4/EndFrame/xrEndFrame (dueño de ruta, traspaso 500 ms).
- 2026-10-26 VQP passthrough ovrp: `klovrp_shape_is_passthrough` = shapes 7/8/10/11 (enum OVRPlugin real; antes 6/7 → Gundam shape 8 SurfaceProjected nunca activaba); `klovrp_shape_name` alineado; aviso si capa passthrough sin InitializeInsightPassthrough. Test `t_pt_scan`.
- 2026-10-26 VQP libc/JNI: `kl_bs_resolve` (ruta con '\' literal primero, si no '\'→'/'; `[fs] backslash_fallback`); `klj_arr_data` en 13 sitios JNI; cuarentena pthread libera solo si destroy==0; ForceQuit vuelca `[forcequit]` a fd 2 sin crash.log; receptores UE `com/epicgames/unreal/*` (antes solo ue4/* → ITR2 sin batería/volumen); tabla `g_ue4_thunks` (36 filas S/H/O); AudioFocus GRANTED; isFinishing/getDisplay; `[jni] noop_hit`; `GetEyeFovLayerId` real; `createTelemetryHandler` objeto sintético. Tests `t_bslash`, `t_forcequit`, `t_ue4_thunk`, `t_ue4_receivers`, `t_noop_hit`, `t_jni_synth`, `t_ovrp_render`, `t_libc_sem`.
- 2026-10-26 VQP captura: ajuste por juego «Captura de frames (diagnóstico)» (VPVK_CAPTURE=1, `QUEST/capturas/<juego>/`), gesto pulsación larga (`kl_diagcap.c`, combo/segundos configurables) → `vpvk_capture_request`; «Incluir capturas» en registros. Test `t_diagcap`, `t_frame_boundary`.
- Build `d67eb28`/VPK `5cf5c80` (medido): ITR2 fb ASW 1116x1129 y 0 `empty_scissor` (arreglo ASW confirmado); ITR2 `missing_resources` ~511/submit; Maestro GLES con INVALID_OPERATION en setUniform; Gundam SIGSEGV en libfmod tras opens de bundles con '\'; pso_wait in-game 0 (archive 6048 PSO).

## Current state
- Última build verde: run 40420398660 (VQP `bb687de`, VPK `74b2d52`), sin probar en visor. Pendiente de build: `ref:` → VPK `6181b91` (kEmitVersion 3, gpu.tex, split views opt-in, tileMaxVertices en caliente) + purga por versión de traductor.
- Pendiente (orden):
  1. Log nuevo por juego: ITR2 `gpu.missing` (qué recurso falta) y `vk.pso.census` (created vs >8000 del juego); Maestro `gles.uniform_err` (uniform concreto del morado) y `gles.tex`; Gundam `[pt] passthrough ON`, `[fs] backslash_fallback`, si sigue el crash en libfmod; todos `vk.hitch`, `gpu.mem.rt mat_why=`, `gles.pso.recipe`, `[jni] noop_hit`; tiempo de arranque con PSO síncrono.
  2. Capturar frame roto (Maestro morado, ITR2) y correr `replay.yml` → primer draw divergente.
  3. RAM: MSAA `HeavyGeometry` sigue materializando (Gundam ~650 MiB); EXT_multisampled_render_to_texture de GLES crea 1x (decisión pendiente: MSAA real transient o nada).
  4. `VPVK_SPLIT_VIEWS=1` (pase por ojo, VPKEngine `493d7da`) candidato para MSAA HeavyGeometry multiview; apagado. Si se activa con foveado propio, el mapa VRR de 2 capas lo construye el motor (commands.cpp ~697 `mapFor`, platform_metal.mm ~235): el cambio sería en el motor (ref. AVP `kl_gpu_time.m`). unverified.
  5. -1004 restantes en ovrp son deliberados (lista en kl_ovrp*.c); Unity Choreographer/Swappy sin verificar (falta log Gundam).
- Riesgos abiertos: twin render pass depende de marcar imágenes de ojo; draws con `imageBuffer` sin enlazar se saltan; pausa con la Corona = `_exit(0)` (justificado en KleptonApp.swift:100-112: ARKit/compositor/FMOD/Unity no reanudables); PSO síncrono puede alargar arranque en juegos de un solo hilo.

## Decisions
- Todo global, sin condiciones por juego (salvo ajustes del usuario: AA, resolución, anisótropo, RCAS, captura).
- Solo offline: `KL_NET_OFFLINE`=1, `KL_PLAT_TOKEN_ERROR`=1; IAP/AssetFile = fallo OFFLINE tipado; solo `Entitle*` DRM aborta.
- Mandos: Touch 0x3 siempre conectado (Sense real o emulación por pellizco) + bits de mano 0x60; `HandTrackingEnabled` = proveedor de manos y ningún Sense adjunto.
- Render pass: el guest recibe sus store ops; twin con STORE forzado solo si toca imagen de ojo o framebuffer desconocido (`KL_VK_RESOLVE_STORE=2` = todo forzado).
- `vkGetInstanceProcAddr` desconocido devuelve stub no-NULL (Wrath2). `VPVK_REPORT_GEOM=1` se mantiene (128 interpoladores Unity).
- `VPVK_MSAA_DEAD_STORE` apagado en juegos y conformidad. MSAA: STORE del cliente siempre conserva samples (keepSamples).
- VPKEngine `main` siempre actualizado con cualquier arreglo del motor de cualquier proyecto; VQP fija `ref:` a SHA de main.
- Pipelines: juegos con `VPVK_ASYNC_PIPELINES=0` (síncrono, como el driver de Quest); el motor es async por defecto (AVP).
- MSAA del juego mantenido por defecto; Automático: VPKTAA solo si no hay MSAA.
- `nativeLowMemory` solo en el hilo del juego; no desde `kl_jni_drain_ui_tasks`.
- `ovrp_GetEyeLayerRecommendedResolution` → -1004 sin capa de ojo viva.
- Slider de resolución = tamaño final del ojo; render_scale = 1. Tamaño recomendado fijo por partida; nunca reaprender por capa.
- Presupuesto anunciado al guest = límite real (6144 ya probado: guests se ahogan). No reintroducir.
- Fin de frame segmentado apagado (`KL_VK_FRAME_ASYNC=1` lo activa).
- Calidad del compositor 1.0: no tocar. Ritmo encajado al panel.
- GLES ES 3.1. Identidad Quest 3 (`KL_QUEST3`, headset 11); mandos Touch de Quest 2; eye tracking Unsupported.
- Foveado de Quest apagado; propio opt-in, M5 solo en M5, exige VPKTAA nativo.
- ASW/PTW apagados por defecto (`KL_CP_ASW`=0, `KL_CP_PTW`=0). Frame congelado: 3,0 s.
- Valores: burst audio 1024 con Wwise; `kMaxArchiveFiles` 512; `VPVK_MAX_DRAWS_PER_CB` 256; anillo TAA 4; máscara UI R8 slot 7; `KL_OVERLAY_SUBMIT_ORDER`=1.
- Datos de juegos nunca en el repo.
- Build solo con `[build]`, completa y revisada.
- Modo sentado solo eleva origen floor/stage (EyeLevel y XR LOCAL intactos); se cachea al lanzar.
- Identidad Quest/Adreno: VQP la activa siempre (`vq_gfx_config_forward` → `vpvk_config_set_quest_identity(1)` + `VPVK_QUEST_IDENTITY=1`); verificar en log.
- Passthrough ovrp: solo por forma de capa (7/8/10/11) con InsightPassthrough inicializado; el estilo no activa.
- Auriculares UE: `stateChanged(0)` (sin jack, altavoz del visor).
- `VPVK_ROBUST`: valor aprendido por juego (0 o app) para que prewarm y juego coincidan; GLES siempre app (= robust on, sin cambio).
- Knockout de shaders de VPKEngine (AVP) no se usa en VQP (no se arma).

## Files in play
- `app/Sources/{VQLaunch,VQLauncher,VQLog,VQFolder}.swift`.
- `klepton/runtime/xr/{kl_ovrp.c,kl_ovrp_render.c,kl_openxr.c}`, `klepton/runtime/gfx/kl_bg.{c,h}`, `klepton/runtime/diag/kl_diagcap.{c,h}`.
- `klepton/runtime/jni/{kl_jni_ue4.c,kl_jni_android.c,kl_jni_window.c,kl_jni_display.c,kl_jni_io.c,kl_jni_bridge.c,kl_jni_services.c}`, `klepton/runtime/kl_jni.c`.
- `klepton/runtime/libc/{kl_libc.c,kl_pthread.c}`, `klepton/runtime/kl_fault.c`.
- `tools/ci/{host-tests.sh,jni-audit.py}`, `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).
