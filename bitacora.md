# Bitácora — VisionQuestPro

## Last done
- 2026-11-08 Lote «game-changer», pin VPKEngine `cccdcb3`, sin probar en visor; Swift, .mm, kl_libc.c, kl_ovrp.c y kl_vulkan.c no compilados fuera de CI (unverified). Tests Linux: motor ctest 336/336; t_lowmem, t_cache_complete, t_vq_area, t_closed_room, t_kwcopy, t_quest_profile, spatial_test OK (gcc+ASan/UBSan). Cada tanda pasó revisión adversarial. Primer arranque tras actualizar: kLowerVersion 13/kEmitVersion 12 invalidan archive/AOT; `purgeOnTranslatorChange` conserva `recipes.bin` si hay sello `vq.translator` (el motor retraduce en `refresh()`), sin sello la borra.
  - Motor: `gl_Position` invariante por defecto (`VPVK_INVARIANT_POS=0` lo apaga; `#pragma STDGL invariant(all)`), reintento de `newLibraryWithSource` sin `, invariant` (contadores `libInvariant`/`libInvRetries` en `gpu.shader.lib`); `mix`=fma(y,a,fma(x,-a,x)), NMin/NMax/NClamp `precise::`; `vk.msaa main` solo en cambios + latido 60 s; flush del archive también con reason 5; `vpvk_trim_memory` no bloqueante + `trimSerialized` (MSAA/args encadenados) + `vpvk_mem_brief`; GPU anunciada Adreno 650/740 por perfil (`VPVK_QUEST_GPU_NAME`).
  - Memoria (klepton): avisos al guest solo con presión real del SO o ≥80 % del límite real (CLEAR 70, CRIT 90, `kl_memrule.h`); `KL_LOWMEM_RATE_S`=0 (aviso por tasa apagado), `KL_TRIM_RATE_S`=6 (solo trim de host); `lowMemory` de getMemoryInfo con la misma regla; telemetría `[mem]` cada 5 s (tope 120 + cambios >10 %). Pendiente: comprobar con `[mem]` y el informe del trim de dónde crece la RAM (guest/IOAccelerator); duda `kl_libc.c ~1584` «jetsam at 6.5 GB» vs límite 8190 (leer `[mem] threshold`).
  - App/XR: perfil Quest 2/3 por juego; caché «estable» (`vq_cache_complete`: complete o reason 5; no degrada un estable guardado sin recetas nuevas); HUD/tarjeta «recetas nuevas»; autorización ARKit [.handTracking,.worldSensing] con log y `[hands] 5 s`; proveedores filtrados por permiso; captura de log del guest (400 líneas, Scene/Room 40 c/u); `VPVK_FRAME_OPT=0` retirado; log `[msaa] game= global= perGame= effective= source=` (no hallado bug de override para AoT; leer `vk.msaa main`).
  - Sala/zona: sala SIEMPRE cerrada (suelo, techo, 4 paredes; sintética o real completa y convexa, paredes reales giradas hacia dentro al purgar; planos sintéticos fuera de la profundidad de entorno); ajuste por juego «Área de juego» Libre (2–6 m, defecto 3×3) / Sentado (círculo 1,0 m, radio unverified) → `KL_OVRP_BOUNDARY_MODE/_W/_D`, log `[area]`. Pendiente: B) dibujo manual de la zona dentro del espacio inmersivo (requiere poses de mando/mano; no hecho); una L real nunca es «completa» (se queda la sintética ajustada).
- 2026-11-08 Profundidad del entorno (klepton, SIN build, sin probar en visor; Swift/Darwin no compilados fuera de CI, unverified). Estado verificado por lectura: `XR_META_environment_depth` y `ovrp_*EnvironmentDepth*` implementados (kl_envdepth.c/.h, kl_ovrp_spatial.c:1297-1403, kl_openxr_spatial.c:1044-1156), alimentados por la malla de ARKit (`SceneReconstructionProvider`) rasterizada a D16 320x320 por ojo (KleptonEnvDepth.swift, ≤30 Hz); NO hay mapa de profundidad LiDAR por píxel (Apple no lo documenta en visionOS; cámara principal solo con licencia enterprise); hand removal solo guarda el flag; passthrough sin oclusión por profundidad; planos de respaldo = rectángulos. Cambios: `refreshInterval=0.1`/`fullCopyInterval=2.0` (antes 0.5 s/~5 s; vigilar CPU/MTLBuffer); timeout 2.0 s del trabajo en vuelo (estado `S_LOST`, `seq`, `kled_job_expired`/`kled_job_is_current`/`kled_done_action`, log `lost_note` limitado a 1/5 s); un slot `S_LOST` no se reutiliza (con ≥2 perdidos el anillo de 4 se agota y la profundidad se congela, solo queda el log); test `klepton/tests/t_envdepth_job.c` en host-tests.sh (pasa sin sanitizers; ASan/UBSan solo en CI). Pendiente: `supported()` (kl_envdepth.c ~135) sigue anunciando soporte por `wants_scene` aunque el permiso se deniegue; arreglo honesto = estado pendiente/permitido/denegado en kl_spatial puesto desde `monitor()`; preexistente: `free` espera hasta 0.5 s a un job colgado (kl_envdepth.c ~296-305); sin confirmar si el juego recibe la textura en la práctica (sin sincronización Metal→VkImage explícita, unverified).

## Current state
- Build del lote 2026-11-08 sin probar en visor. IPA en release `visionos`.
- Pendiente (orden): (1) probar en visor: Gundam AR (¿se coloca algo con la sala cerrada?), Maestro/ITR2 geometría (invarianza), RAM (`[mem]`), AoT x4 (`[msaa]`, `vk.msaa main`); (2) dibujo manual de zona; (3) `supported()` honesto de profundidad (kl_envdepth.c ~135) y manos (0 frames en ARKit: ver `[cp] ARKit authorization`, `[hands]`); (4) clics muertos: si persisten, medir tiempos de `play()`/`url`; (5) `choose()` espera `persistLock`; certificado UI race; `cachedFingerprint` sin sincronizar; `VQConformanceView.swift:68`.
- Hipótesis vivas sin demostrar: geometría perdida en Maestro/ITR2 por z-fighting/invarianza, por avisos de memoria falsos (corregido) o identidad Quest 3 en juegos Quest 2 (perfil por juego). Gundam FMOD crash libfmod+0x7d3f8 sin causa (notas/juegos.md).
- Homebrew: APK única, detección exige `.apk` (VQGames.swift ~109); sin APK falta tarjeta «incompatible» (no hecho).

## Decisions
- Sin VPKTAA ni foveado propio. AA = MSAA del juego o forzado x2/x4, siempre memoryless.
- Klepton envía en su cola 0 fija; las copias de sombra van en la cola del guest.
- Solo offline: `KL_NET_OFFLINE`=1, `KL_PLAT_TOKEN_ERROR`=1; solo `Entitle*` DRM aborta.
- Mandos: Touch 0x3 siempre conectado + bits de mano 0x60.
- Render pass: el guest recibe sus store ops; twin con STORE forzado solo sobre imagen de ojo o framebuffer desconocido.
- `vkGetInstanceProcAddr` desconocido devuelve stub no-NULL (Wrath2); `vkQueueSubmit2*` NULL si no hay entrada real. `VPVK_REPORT_GEOM=1`.
- `VPVK_MSAA_DEAD_STORE` apagado.
- `nativeLowMemory` solo en el hilo del juego.
- Slider de resolución = tamaño final del ojo; render_scale = 1; tamaño fijo por partida.
- Presupuesto anunciado al guest = límite real.
- Fin de frame segmentado apagado (`KL_VK_FRAME_ASYNC=1` lo activa).
- Calidad del compositor 1.0; ritmo encajado al panel.
- ASW/PTW apagados por defecto; SpaceWarp servido siempre (AoT falla sin él). Frame congelado: 3,0 s.
- Valores: burst audio 1024 con Wwise; `kMaxArchiveFiles` 512; `VPVK_MAX_DRAWS_PER_CB` 256; máscara UI R8 slot 7; `KL_OVERLAY_SUBMIT_ORDER`=1.
- Datos de juegos nunca en el repo. Build solo con `[build]`, completa y revisada.
- Passthrough ovrp: por forma de capa (7/8/10/11) o `EnqueueSubmitLayer2`, con InsightPassthrough inicializado.
- `VPVK_ROBUST` aprendido por juego (0 o app); purga de caché al cambiar `vpvk_translator_tag()`.
- VPKEngine `main` siempre con todos los arreglos del motor; VQP fija `ref:` a SHA de main.
- Todo global salvo ajustes del usuario por juego: resolución, MSAA, anisótropo, RCAS, ASW, captura, perfil del visor (Quest 2/3), área de juego.
- Perfil Vulkan/GLES = Quest 3 real del motor; la identidad (GPU, headset 9/11) es por juego (auto = Quest 3 si global y el manifiesto lo declara o no declara dispositivos). GLES ES 3.1.
- Avisos de memoria al guest solo con presión real; el trim de host por tasa es independiente (`KL_TRIM_RATE_S`).
- Sala entregada a MRUK siempre cerrada y de un único origen.

## Files in play
- `klepton/runtime/{kl_memrule.h,libc/kl_libc.c,libc/kl_va_handlers.c,xr/kl_spatial.c,xr/kl_ovrp.c,xr/kl_ovrp_spatial.c,gfx/kl_vulkan.c}`, `klepton/visionos/Sources/{KleptonCompositor,KleptonSpatial,KleptonEnvDepth}.swift`, `app/Sources/{VQLaunch,VQLauncher,VQCacheState}.swift`, `app/Native/vq_config.c`, `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).

