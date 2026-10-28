# Bitácora — VisionQuestPro

## Last done
- 2026-10-28 VQP pin VPKEngine `c6c0087` (Load de TRANSIENT con memoria indefinida ya no materializa MSAA; ver bitácora del motor). Build lanzada con `[build]`; sin probar en visor. Sin cambios de Klepton/app.
- 2026-10-28 Build verde run 40694850720 con VQP `e0bb8a6`+pin y VPKEngine `8c21013` (incluye orden de operandos OpSMod determinista gcc/clang y test de occlusion query con espera) (motor validado por CTS: Vulkan 26 Fail / GLES 64 Fail, 0 crash; ver `notas/cts.md` del motor). Sin probar en visor.
- 2026-10-28 `184e4dd` (revisión adversarial): fin de frame espera solo las colas del guest usadas en el frame (hasta 8, `QueueWaitIdle` bajo `g_qsub_mu`; sin `gq_multi` pegajoso ni `DeviceWaitIdle`); `klvk_shadow_pump(q)` en la cola del guest; suelo de retiro para imágenes normales `(age>=after && issued>=frame+8 && gpu_serial>=frame+1) || age>=4*after`; `vkQueueSubmit2/KHR` → NULL si el driver no lo tiene; `klvk_wq` fija la cola 0 de Klepton (acquire, clears y marcador). Tests `t_frame_fence`, `t_retire_due`, `t_vk_trap` (kl_vulkan.c entero sin compilar fuera de visionOS).
- 2026-10-28 `77d58dc`: hooks `klvk_QueueSubmit2` (`vkQueueSubmit2` y `vkQueueSubmit2KHR` en `g_ours[]`, misma lógica que `klvk_QueueSubmit` sin trace/probe); `docs/perfil-adreno740.md` al día.
- 2026-10-27 `57b09ba`: Klepton alineado con el perfil Quest 3 único del motor: multi-cola (`klvk_wq`, `klvk_note_guest_queue`), `EXT_metal_objects` interno vía gipa, lista de extensiones 128, fuera `KL_VK_VTX_SSBO`/`KL_VK_IMG_NOFMT`/parche ASTC, fuera ajuste «Perfil Vulkan completo» y `VPVK_EXTRA_EXTENSIONS`; VQConformance con `VPVK_FORCE_ANISO=0`, `VPVK_QUEST_IDENTITY=1`, esperas 120000.
- 2026-10-27 `872ffa3`: fuera VPKTAA y foveado propio; ajuste MSAA Juego/x2/x4 (`VPVK_MSAA_FORCE`, global y por juego, siempre memoryless); captura instantánea (`vq.cap.hold.*`, defecto 0).
- 2026-10-27 Build `7f66ae7` (run 40562139335): causas medidas de ITR2/Gundam/captura; detalle en `notas/juegos.md`.

## Current state
- Última build verde: run 40694850720 (VQP `81136fc`, VPK `8c21013`); sin probar en visor.
- Pendiente (orden):
  1. Logs 2026-10-28 (VQP `81136fc`) analizados; sin causa demostrada aún:
     - Gundam crash: SIGSEGV libfmod.so+0x7d3f8, hilo guest, dirección no mapeada; RAM descartada (foot 3771M, lim_rem 4420M); sin símbolos de libfmod.
     - Gundam RA: `RequestSceneCapture` devuelve id síncrono y el evento 100 sale tras ≤8 s+1,5 s (`kl_ovrp_spatial.c:701-735`); room posiblemente sintética (`kl_spatial.c:67,560`); `SetSpaceComponentStatus` ignora enable (`:465-482`); capa passthrough 4 expira a 3 EndFrame4 sin re-encolar (`kl_ovrp.c:6998-7056`). Causa de que el botón de Space Setup no cree la habitación: unverified; falta el retorno numérico de las llamadas ovrp_* spatial en el log.
     - Gundam gráfico: 6 fragment modules magenta (Unity error shader) `[vk] [magenta]`, `[imgfmt] fmt 157/151/127 UNSUPPORTED` (3), PSO failed=0.
     - ITR2 (sesión 18:05Z, `81136fc`): SIGKILL del sistema a los 13 s durante compilación de shaders (foot 3028M, lim_rem 5163M, compresor saturado); `msaa_materialized=49` (1249M) con `transient_violation` en tex 77/80: `Load` sobre TRANSIENT en su primer attach materializa la imagen para siempre (`vpkengine gpu/core/src/frame_compiler.cpp:1056-1059`). Propuesta pendiente: tratar ese Load como contenido indefinido y mantener memoryless. Causa del kill: unverified.
     - Maestro morado: `gles.uniform_err base_type_mismatch` vec4 declarado vs ivec4/uvec4 (vc0_h[0], vc1_h[0]); unverified que sea la causa.
  2. Probar MSAA forzado x2/x4 y RCAS; captura instantánea (`.vpcap` en `QUEST/capturas/`).
  3. Si un frame sale roto: captura + `replay.yml` del motor.
  4. Crash libfmod+0x7d3f8 (Gundam, hilo audio) sin causa.
- Riesgos: `g_qsub_mu` retenido durante `QueueWaitIdle` bloquea submits de otros hilos (tope 2 s del motor si un lote timeline espera a otro hilo); Submit2-only no sale en trazas `[TRACE]`/probe; claves de ajustes huérfanas sin efecto (`vq.aaMode.*`, `vq.aaMSAA.*`, `vq.vkFullProfile`, `vq.cap.sec.*`); pausa con la Corona = `_exit(0)`.

## Decisions
- Todo global, sin condiciones por juego (salvo ajustes del usuario: resolución, MSAA, anisótropo, RCAS, ASW, captura).
- Sin VPKTAA ni foveado propio. AA = MSAA del juego o forzado x2/x4, siempre memoryless.
- Perfil Vulkan/GLES único = Quest 3 real del motor; Klepton no fuerza features ni extensiones.
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
- GLES ES 3.1. Identidad Quest 3 (`KL_QUEST3`, headset 11, `VPVK_QUEST_IDENTITY=1`).
- ASW/PTW apagados por defecto; SpaceWarp servido siempre (AoT falla sin él). Frame congelado: 3,0 s.
- Valores: burst audio 1024 con Wwise; `kMaxArchiveFiles` 512; `VPVK_MAX_DRAWS_PER_CB` 256; máscara UI R8 slot 7; `KL_OVERLAY_SUBMIT_ORDER`=1.
- Datos de juegos nunca en el repo. Build solo con `[build]`, completa y revisada.
- Passthrough ovrp: por forma de capa (7/8/10/11) o `EnqueueSubmitLayer2`, con InsightPassthrough inicializado.
- `VPVK_ROBUST` aprendido por juego (0 o app); purga de caché al cambiar `vpvk_translator_tag()`.
- VPKEngine `main` siempre con todos los arreglos del motor; VQP fija `ref:` a SHA de main.

## Files in play
- `klepton/runtime/gfx/kl_vulkan.c`, `klepton/runtime/xr/{kl_ovrp.c,kl_ovrp.h}`, `klepton/tests/{t_frame_fence,t_retire_due}.c`.
- `app/Sources/{VQLaunch,VQLauncher,VQConformance}.swift`, `docs/perfil-adreno740.md`.
- `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).
