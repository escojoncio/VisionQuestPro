# Bitácora — VisionQuestPro

## Last done
- 2026-10-25: rama `wip-auditoria` (sin build, sin pasar a `main`): bloques A–C + conformidad (`52279ac`, `339722e`, `cc7ca88`, `3aa1393`). Detalle abajo.
- 2026-10-25: VPKEngine `main` `9f5867b` (Load de memoria indefinida → DontCare, pool memoryless con desalojo), `4b20da0` (parches AVP 0025/0033/0034 apagado/0035/0044/0030 core/0050), `cab00a7` (vpgles texture_buffer, draw_buffers_indexed, features no estrictas, gauges de memoria Metal).
- 2026-10-25: logs de 698fa20 analizados (Maestro, UBOAT, Gundam, Deadpool, RE4) + auditoría de API con los APK de los 5 juegos (libc, GL/EGL/Vulkan, OVRPlugin/Platform, JNI).
- 2026-10-25: VPKEngine `662a70b`: todo attachment memoryless diferido; build 698fa20.

## Current state
- Última build: 698fa20 (VPKEngine 662a70b). Todo lo de `wip-auditoria` y VPKEngine posterior: unverified en visor; `.mm`/Swift solo compilan en CI.
- Causas demostradas (APK + logs) y arreglo en `wip-auditoria`/VPKEngine:
  - Maestro: `GLFuncPointers::glTexBufferEXT`==NULL (vpgles no anunciaba `GL_EXT_texture_buffer`) → VPKEngine `cab00a7`.
  - UBOAT: EOS `Application.getClassLoader`/`ClassLoader.loadClass` simulados → jmethodID 0 → abort → JNI implementado (bloque A).
  - Gundam: `HandsQualityFilter` oculta los brazos si connected no tiene 0x20/0x40 → connected 0x63, active 0x3, in-hand, `HandTrackingEnabled` estable (bloque A; Swift publica Sense adjunto).
- Pendiente (orden):
  1. Pasar `wip-auditoria` a `main` y build `[build]` SOLO con visto bueno del usuario (esperar a cambios externos que el usuario anunció).
  2. Deadpool jetsam: `MTLDevice.currentAllocatedSize` crece ~10 MB/s con texturas/buffers planos (no atribuido). Leer en el próximo log `[hb] gpu … held_b views args_b buf_int_b taa_b fov_b` para atribuirlo.
  3. Verificar en log: `[dlsym] … -> NULL` (traza siempre activa), `[mprobe f1800/f3600]` detalle de escena (Gundam), `loads_dropped=`, `ml_pool=`, `memoryless_attrib`, `image_buffer_unbound`, `device_feature_not_present`.
  4. Resto auditoría no aplicado: `AMediaMuxer_*` (Gundam, solo grabar); `/proc/stat`, `self/exe`, topology; `ovrp_GetNodePresent`/`PositionTracked` sin sufijo siempre true; 2 tipos de mensaje IAP sin `ovrMessageType` (id 0).
  5. VPKEngine: fusión completa con la rama AVP (`19a1f44` + 50 parches, ~330 conflictos) pendiente; `msaaResolveOnTile` necesita `Attachment::keepSamples` antes de activarse.
- Riesgos abiertos: twin render pass depende de marcar imágenes de ojo (OVRP `g_eye`, OpenXR `g_xeye`); draws con `imageBuffer` sin enlazar se saltan; cuarentena de cond/mutex por tiempo (fuga si >40k destroy/s); `klj_arr(x)->data` sin proteger en `jni/kl_jni_{android,bridge,services,ue4,window}.c`; pausa con la Corona = `_exit(0)`.

## Decisions
- Todo global, sin condiciones por juego (salvo ajustes del usuario: AA, resolución, anisótropo, RCAS).
- Solo offline: `KL_NET_OFFLINE`=1, `KL_PLAT_TOKEN_ERROR`=1; IAP/AssetFile = fallo OFFLINE tipado; solo `Entitle*` DRM aborta.
- Mandos: Touch 0x3 siempre conectado (Sense real o emulación por pellizco) + bits de mano 0x60; `HandTrackingEnabled` = proveedor de manos y ningún Sense adjunto.
- Render pass: el guest recibe sus store ops; twin con STORE forzado solo si toca imagen de ojo o framebuffer desconocido (`KL_VK_RESOLVE_STORE=2` = todo forzado).
- `vkGetInstanceProcAddr` desconocido devuelve stub no-NULL (Wrath2). `VPVK_REPORT_GEOM=1` se mantiene (128 interpoladores Unity).
- VPKEngine `main` siempre actualizado con cualquier arreglo del motor de cualquier proyecto.
- MSAA del juego mantenido por defecto; Automático: VPKTAA solo si no hay MSAA.
- `nativeLowMemory` solo en el hilo del juego; no desde `kl_jni_drain_ui_tasks`.
- `ovrp_GetEyeLayerRecommendedResolution` → -1004 sin capa de ojo viva.
- Slider de resolución = tamaño final del ojo; render_scale = 1.
- Fin de frame segmentado apagado (`KL_VK_FRAME_ASYNC=1` lo activa).
- Calidad del compositor 1.0: no tocar. Ritmo encajado al panel.
- GLES ES 3.1. Identidad Quest 3 (`KL_QUEST3`, headset 11); mandos Touch de Quest 2; eye tracking Unsupported.
- Foveado de Quest apagado; propio opt-in, M5 solo en M5, exige VPKTAA nativo.
- ASW/PTW apagados por defecto (`KL_CP_ASW`=0, `KL_CP_PTW`=0). Frame congelado: 3,0 s.
- Valores: burst audio 1024 con Wwise; `kMaxArchiveFiles` 512; `VPVK_MAX_DRAWS_PER_CB` 256; anillo TAA 4; máscara UI R8 slot 7; `KL_OVERLAY_SUBMIT_ORDER`=1.
- Datos de juegos nunca en el repo.
- Build solo con `[build]`, completa y revisada; pushear VPKEngine antes.

## Files in play
- `klepton/runtime/xr/{kl_ovrp.c,kl_ovrp_input.c,kl_ovrplat.c}`, `klepton/visionos/Sources/KleptonControllers.swift` (+ bridging header).
- `klepton/runtime/jni/{kl_jni_lang.c,kl_jni_android.c,kl_jni_ue4.c,kl_jni_window.c,kl_jni_prefs.c}`, `klepton/runtime/kl_jni.c`, `klepton/runtime/kl_dl.c`, `klepton/runtime/diag/kl_mprobe.c`.
- `klepton/runtime/libc/{kl_libc.c,kl_pthread.c,kl_shim.c}`, `klepton/tests/t_libc_sem.c` (Gate: `tools/ci/host-tests.sh`).
- `klepton/runtime/gfx/kl_vulkan.c` (twin render pass), `app/Sources/{VQConformance.swift,VQLaunch.swift}`.

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
