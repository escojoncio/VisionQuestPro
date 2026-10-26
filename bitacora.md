# Bitácora — VisionQuestPro

## Last done
- 2026-10-26 VPKEngine `5b8432f`: PSO `Priority::Urgent` (boost real) + creaciones del juego a `Visible` (causa de tirones: backlog del precache UE ~7,4k PSO en Blocking, boost al final de la cola); `skip_by{}` en `vk.submit` (ITR2: ~236/270 draws descartados por ok=false silencioso, motivo por medir); vpgles program binaries (causa Maestro: `PROGRAM_BINARY_LENGTH`=0 → UE fatal "Binary program returned 0 bytes!" en FOpenGLProgramBinaryCache → ForceQuit ×90 → SIGSEGV SceneColor nulo en RenderForward); `vpgpu::BackgroundGate`.
- 2026-10-26 VQP: `klepton/runtime/gfx/kl_bg.{c,h}`: guest bloqueado en segundo plano (QueueSubmit/Present, SwapBuffers, ovrp Update2/EndFrame4/end_frame_impl, xr WaitFrame/EndFrame; foco VrFocus2/InputFocus=0), Swift `scenePhaseChanged`→`kl_bg_set`, reenvío por dlsym a `vpgpu_set_background`; watchdog silenciado en bg. `kl_ue4.c`: UE4CommandLine.txt en `<files>/<native>/<Proj>/` (antes ruta que UE no leía → sin log UE); extras por target solo con `KL_UE4_TARGET_EXTRAS=1`. Tests `t_bg`, `t_ue4_cmdline`.
- 2026-10-26 VPKEngine `710574a`: `ScopedPool` (objc_autoreleasePoolPush/Pop, core/include/vpgpu/core/scoped_pool.h) en métodos públicos de MetalBackend, MetalPlatform taa/depthResolve y entradas vk*/vpvk_* de entry.cpp (causa inferida de RAM: MTLTexture autoreleased en hilos guest sin pool = inmortales; attr ≫ ledger en Gundam/Deadpool/ITR2). Texturas `pinned` con retención 1 s + 4 frames (`pinnedHold`/`promotePinned`). `Attachment::keepSamples` (=true en todo StoreAndResolve del traductor; solo DONT_CARE+resolve es memoryless), `msaaColorResolveOnTile` por defecto (`VP_MSAA_TILE`). gles: subidas DEPTH_COMPONENT→D16/D24/D32F. Contadores img_rel_*/tex_created/destroyed/g_tex_live en [vkagg].
- 2026-10-26 VQP: tamaño de ojo sin bucle: primer push = constante de la partida (`kl_ovrp_set_eye_texture_size` devuelve aceptado; Swift guarda solo si aceptado; espera 500 ms si el guest pregunta antes); `kles_desc_size` cancela escala guest >1 (modo pct), cap `KL_OVRP_EYE_MAX`; `kles_freeze_widen` (ensanche del plugin congelado una vez, >1.02) y `kles_freeze_xr_scale` (OpenXR) en xr/kl_ovrp_eyesize.h. Retire: `g_eye_published`, imagen nunca publicada → quick sin hold; ASW MV/depth quick=2 (0,5 s+3 frames o 2 s); `kl_vulkan_retire_poll` desde `kl_mem_rate_poll` (no_publish). `klvk_forget_cached_mtl` también en `klvk_DestroyImage`. Swift `pruneArrayViews`. `ue4_stdout_default`=1 para todo UE.
- `07bd8fa` (VPKEngine): vpgles enlaza varyings por location (confirmado en visor: Maestro sin "fragment input not written").
- `b27c9c7`: DT_NEEDED del entry UE (confirmado en visor: ITR2 0 imports sin resolver).
- `3280533`: auditoría estática de enlace+JNI de 6 APK: `ovrMessageType_ToString`, `AMediaMuxer_*` fallo limpio, trap Vulkan -7/0 por nombre, nativos UE con firma máxima (`nativeSetWindowInfo` ZII, `nativeSetAndroidVersionInformation` 7 args), Electra/EOS ITR2, `getAppPackageName`, `System.loadLibrary`, EOS cache dirs, `ReflectionHelper.newProxyInstance`, `kl_jni_onload_claim` (JNI_OnLoad único por imagen).
- `68e5ecb`+`d7f81fc`: `[hb] gpu.attr` (objetos Metal vivos por etiqueta, total/tracked/untracked) y `gpu.bigalloc`/`[mtl] bigalloc` ≥32 MiB.
- `c61f2cf`: `[hb] vm` (mapa VM por tag, tag 240 = mmap anónimo del guest) y `[fatal] last guest errors` (anillo async-safe).
- `c6f8362`: carátula tienda: timeouts 30/120 s, backoff net 2/10/30/60 min, respaldo nombre exacto+organización.

## Current state
- Última build en verde: run CI 40308522356 (commit 14f415d, VPKEngine 5b8432f): cambios 2026-10-26 unverified en visor.
- Pendiente (orden):
  1. Logs build af035c0: untracked Metal plano (~200-256 MiB) → autoreleasepool confirmado; picos: Deadpool 6,3 GB (antes 8,2), Gundam 6,7 GB (antes jetsam), ITR2 4,9 GB; ITR2 `g_tex_live` 56→4071 por vigilar. Gundam: parpadeo sin causa en log (0 skips, 6 rebuilds de capa).
  2. Build nueva: ITR2 leer `skip_by{}`/`vk.skip` → arreglar el motivo; medir tirones (`vk.pipeline_skip`, `hard_waited_ms`); Maestro sin ForceQuit; `[bg] gpu paused/resumed` sin errores BackgroundExecutionNotPermitted.
  3. Pipelines depth-only no se archivan (metal_backend.mm:5915) y archive sin flush periódico; GLES sin precompilación (pipeline por estado en primer draw).
  3. Auditoría restante (no aplicada): ovrp que devuelven -1004 donde se espera handle (`ovrp_GetNativeSDKPointer2`, `GetEyeFovLayerId`, `SetupMirrorTexture2`); OpenXR FB/META sin fila (ITR2); 75 thunks `AndroidThunkJava_*` NO-OP; 184 pares Unity sin enlace; `createTelemetryHandler` NULL (Gundam). Detalle: auditoría en sesión (no en repo).
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
- Slider de resolución = tamaño final del ojo; render_scale = 1. Tamaño recomendado fijo por partida; nunca reaprender por capa (bucle ITR2).
- Presupuesto anunciado al guest = límite real (6144 ya probado: guests se ahogan). No reintroducir.
- MSAA: STORE del cliente siempre conserva samples (keepSamples).
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
