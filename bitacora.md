# Bitácora — VisionQuestPro

## Last done
- 2026-10-26: rama `wip-crashes` → `main` con `[build]`. VPKEngine `main` `07bd8fa`.
- `07bd8fa` (VPKEngine): vpgles enlaza varyings por location; input FS no escrito = 0 (Private+OpConstantNull); reserva de locations explícitas. Causa Maestro (20 programas "fragment input not written" → UE4 Fatal → BindPendingShaderState NULL+0x588).
- `b27c9c7`: cadena UE carga los DT_NEEDED del entry (`kl_load_needed`, kl_dl.c; kl_ue4.c). Causa ITR2: libUnreal NEEDED libOmniConnectShared (libc++ estática) no cargada → 120 imports sin resolver.
- `3280533`: auditoría estática de enlace+JNI de 6 APK: `ovrMessageType_ToString`, `AMediaMuxer_*` fallo limpio, trap Vulkan -7/0 por nombre, nativos UE con firma máxima (`nativeSetWindowInfo` ZII, `nativeSetAndroidVersionInformation` 7 args), Electra/EOS ITR2, `getAppPackageName`, `System.loadLibrary`, EOS cache dirs, `ReflectionHelper.newProxyInstance`, `kl_jni_onload_claim` (JNI_OnLoad único por imagen).
- `68e5ecb`+`d7f81fc`: `[hb] gpu.attr` (objetos Metal vivos por etiqueta, total/tracked/untracked) y `gpu.bigalloc`/`[mtl] bigalloc` ≥32 MiB.
- `c61f2cf`: `[hb] vm` (mapa VM por tag, tag 240 = mmap anónimo del guest) y `[fatal] last guest errors` (anillo async-safe).
- `c6f8362`: carátula tienda: timeouts 30/120 s, backoff net 2/10/30/60 min, respaldo nombre exacto+organización.

## Current state
- Build en curso con todo lo anterior: unverified en visor.
- Logs a5f6bc0 (causas medidas): Maestro = link GLSL (arreglado); ITR2 = DT_NEEDED (arreglado); Gundam y Deadpool = jetsam (foot 8170/8192): RAM CPU ~3,2 GB estable, MTLDevice.currentAllocatedSize 4,2/6,2 GB con solo ~1,8 GB en gauges, crece a saltos (no atribuido); UBOAT funciona ~68 fps pero faltan OBB de parche (dato del usuario) y error de submit GPU en background al salir; carátula Deadpool = timeout búsqueda OculusDB.
- Pendiente (orden):
  1. Leer en los logs nuevos: `[hb] gpu.attr` (vpk_top/kl_top/untracked) y `gpu.bigalloc` en Gundam/Deadpool → arreglar el origen de la memoria Metal. Si crece `untracked` → no son objetos nuestros.
  2. `[hb] vm` (reparto CPU), `[fatal] last guest errors`, `gles.program varying_unwritten`, `[dl] … DT_NEEDED`, carátula ("ficha elegida por …").
  3. Auditoría restante (no aplicada): ovrp que devuelven -1004 donde se espera handle (`ovrp_GetNativeSDKPointer2`, `GetEyeFovLayerId`, `SetupMirrorTexture2`); OpenXR FB/META sin fila (ITR2); 75 thunks `AndroidThunkJava_*` NO-OP; 184 pares Unity sin enlace; `createTelemetryHandler` NULL (Gundam). Detalle: auditoría en sesión (no en repo).
  4. Submit GPU en background (UBOAT al salir): parar trabajo GPU al pasar a background.
  5. VPKEngine: fusión con rama AVP pendiente; `msaaResolveOnTile` necesita `Attachment::keepSamples`.
- Riesgos abiertos: twin render pass depende de marcar imágenes de ojo; draws con `imageBuffer` sin enlazar se saltan; cuarentena cond/mutex por tiempo; `klj_arr(x)->data` sin proteger en `jni/kl_jni_{android,bridge,services,ue4,window}.c`; pausa con la Corona = `_exit(0)`; ForceQuit crea `klepton-crash.log` sin crash (volcado `[fatal]`).

## Decisions
- Todo global, sin condiciones por juego (salvo ajustes del usuario: AA, resolución, anisótropo, RCAS).
- Solo offline: `KL_NET_OFFLINE`=1, `KL_PLAT_TOKEN_ERROR`=1; IAP/AssetFile = fallo OFFLINE tipado; solo `Entitle*` DRM aborta.
- Mandos: Touch 0x3 siempre conectado (Sense real o emulación por pellizco) + bits de mano 0x60; `HandTrackingEnabled` = proveedor de manos y ningún Sense adjunto.
- Render pass: el guest recibe sus store ops; twin con STORE forzado solo si toca imagen de ojo o framebuffer desconocido (`KL_VK_RESOLVE_STORE=2` = todo forzado).
- `vkGetInstanceProcAddr` desconocido devuelve stub no-NULL (Wrath2). `VPVK_REPORT_GEOM=1` se mantiene (128 interpoladores Unity).
- `VPVK_MSAA_DEAD_STORE` (VPKEngine 454f7cc, elisión heurística de stores MSAA) apagado en juegos y conformidad hasta que exista `Attachment::keepSamples`.
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
- `klepton/runtime/gfx/{kl_vulkan.c,kl_mtl_attr.m}`, `klepton/runtime/guest/kl_ue4.c`, `klepton/runtime/kl_fault.c`, `klepton/runtime/libc/kl_va_handlers.c`, `app/Sources/VQCover.swift`.

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
