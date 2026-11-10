# Bitácora — VisionQuestPro

## Last done
- 2026-11-10 (5) Tras logs 29 (build 981bf25): Gundam RAM OK (pico 5208/8192 MiB, sin cierre); HLVR colisión OK (GLOBAL_MESH, 38214 vértices). HLVR profundidad: imagen volteada en Y (`Renderer.projection` de `KleptonEnvDepth.swift` niega la fila Y; fila 0 = abajo, arriba = última fila; `gl_upload` de `kl_envdepth.c` ya no voltea). Causa SIN DEMOSTRAR (no se pudo leer DepthPreprocessing.shader); si empeora: revertir ambos. Swift sin compilar en Linux. Log nuevo: `eye0 forward.y`, `Y flipped`.
- ITR2 logs 29 (sin cambio de código): GPU-bound en sync (frame 29,7 ms; fin de frame espera 22 ms); p5/p6 (~43 ms por timestamp) no escalan con los píxeles (-27 % píxeles, +3-6 % tiempo), por eso bajar la resolución no sube fps. "Emular profundidad 24 bits" llega al motor (`VPVK_D24_EMUL`) pero provocó 6929 fallos de receta de caché (11 min de compilación, una sola vez). Existen quads de pantalla completa con mezcla en subpase 1 (contradice nota anterior; que sean niebla: SIN DEMOSTRAR). Por probar: `KL_VK_FRAME_ASYNC=1`, ITR2 al 50 % sin d24, volcar load/store y hashes de p5/p6.
- 2026-11-10 (4) Build 981bf25 (CI OK, IPA publicada). Lote tras logs 28 (build ac281e9 probada: Gundam sigue cerrándose al cargar el nivel, ITR2 igual, HLVR con oclusión parcial y sin colisión). Pin VPKEngine `98a21f1`. SIN probar en visor.
- Gundam / memoria (global): el cierre es jetsam por ~3 GB de texturas que pide el juego en la carga (detalle en `notas/juegos.md`). (1) Presupuesto de texturas: `kl_texbudget_cap`/`kl_texbudget_next` (`runtime/kl_memrule.h`, test en `tests/t_lowmem.c`) en `kl_mem_rate_poll` (`kl_libc.c`) → `kl_vulkan_texture_budget` (`kl_vulkan.c`, dlsym `vpvk_texture_budget`): >= 50 % del límite tope 4096 px, >= 70 % 2048, >= 85 % 1024; solo aprieta; log `[mem] texture budget:` y `vk.tex_budget`. (2) Swap del guest desde el 20 % del límite (`KL_GSWAP_AT`, antes 45). (3) Motor: memoria host de Vulkan >= 1 MiB en mapeos propios (VM tag 241). (4) `kl_mem_vm_print`: 150 ms de presupuesto y 10 s tras un corte (antes 50 ms / 30 s: el cierre quedó sin mapa de regiones).
- HLVR colisión (DEMOSTRADO con libil2cpp + log): el juego usa `MRUKAnchor.GlobalMesh`; la fachada ovrp no daba ancla `GLOBAL_MESH` y `ovrp_GetSpaceTriangleMesh` tenía la ABI equivocada. `kl_spatial.c`: `k_synth_gmesh`, `kl_spatial_global_mesh`, `gmesh_info_locked` (trozos unidos, vértices en el marco del origen, solo trozos enteros); `kl_ovrp_spatial.c`: `comps_of` (+`C_LABELS`), etiqueta `GLOBAL_MESH`, miembro final de `GetSpaceContainer`, `pose_of`, `run_query` (una sola malla), `sp_GetSpaceTriangleMesh(space, trimesh*)`. Test `runtime/xr/tests/spatial_test.c`.
- ITR2: sin causa demostrada (ver `notas/juegos.md`: la niebla de pantalla completa no llega al motor; hipótesis del pool de streaming agotado por render targets). Solo diagnóstico: motor `vk.draw_census`, `vk.comp_census`, `vk.memreq`, `vk.mem_seen`, `vk.tex_hist top{}`, `vk.pso.census drawn=`; Klepton `[ovrp] ovrp_GetSystemHeadsetType2 -> N`.
- 2026-11-10 (3) Build ac281e9: `vk.sub_dump`/`vk.sub_draw`. (2) build 4521d4b: histéresis de respaldos de render target (tirones de Gundam). Lote tras logs 27 (build 4f381bd): profundidad de entorno en forward-Z de Meta + `VK_EXT_shader_viewport_index_layer` (HLVR: confirmado en logs 28, sin error `SetPass`), bloques malloc >= 4 MiB al swap (logs 28: no movió nada, `blocks=1`), clears de adjuntos TRANSIENT y stores del subpase (ITR2).
- 2026-11-10 (build 0b7b5be, probada): `kl_evgate.h` ⇒ HLVR Quest 3 carga la sala; plugin nulo `MetaXRAudioUnity`; guest swap de mmap; espera del ancla de calibración 8 s.

## Current state
- Funciona en visor: Gundam AR, Maestro (render y MSAA x4 forzado), HLVR perfil Quest 2, HLVR Quest 3 carga de sala (MRUK) y oclusión por profundidad (parcial: huecos de la malla de ARKit, sin manos ni objetos móviles), calibración global (cuando el ancla se rastrea).
- Sin probar (este lote): cierre por memoria de Gundam (presupuesto de texturas), colisión de HLVR (malla global), diagnósticos de ITR2.
- Leer en la próxima prueba: Gundam `[mem] texture budget:`, `vk.tex_budget … MiB so far`, `[mem] t=` (foot máximo, `gswap live=`), `[hb] vm … tag241/…` y `MALLOC_LARGE` en el pico, que el nivel arranque; calidad visible de las texturas recortadas. HLVR `[sp-label] … label=GLOBAL_MESH`, `[sp-call] fn=ovrp_GetSpaceTriangleMesh … vcount= icount=` > 0, `GetSpaceContainer count=14`. ITR2 `vk.memreq`, `vk.tex_hist top{}`, `vk.draw_census` (¿algún draw con `in_att` a pantalla completa?), `vk.comp_census`, `ovrp_GetSystemHeadsetType2 -> 11`; y el resultado de bajar su resolución al mínimo.
- Pendiente: ITR2 síntoma visual (hipótesis en `notas/juegos.md`; falta captura VPCAP del nivel: gatillo armado, nunca pulsado); ITR2 rendimiento (gpu 54-57 ms a 3552x2258x2 4x; p5 y p6 ~20 ms cada uno); presupuesto de texturas en imágenes de un solo nivel comprimidas (no se recortan); manos en la profundidad de entorno (no hay); zero-init de las imágenes de profundidad en su primera lectura (1 frame); shader de error de Unity en HLVR/Gundam; `E/Unity: Incompatible SDK version… MetaXRAudioFMOD`; fachada OpenXR sigue dando la malla por trozos; calibración por habitación (ofrecida, sin respuesta); menores del guardián; AoT x4; tarjeta del launcher de ITR2 dice GLES siendo Vulkan.
- Hipótesis vivas sin demostrar: en `notas/juegos.md`.

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
- Eventos asíncronos (ovrp_PollEvent*/xrPollEvent): nunca en el drenaje del frame que los pidió ni antes de 50 ms (`kl_evgate.h`).
- Memoria del guest: mmap anónimos y bloques malloc >= 4 MiB respaldados por fichero desde que el footprint pasa del 20 % del límite (`kl_gswap.h`; ajuste «Memoria extendida», `KL_GSWAP=0` lo apaga).
- Presupuesto de texturas (global, solo aprieta en la partida): 50 % ⇒ 4096, 70 % ⇒ 2048, 85 % ⇒ 1024; nunca adjuntos ni imágenes de almacenamiento.
- Malla de escena hacia ovrp = una sola ancla `GLOBAL_MESH` por sala (trozos de ARKit unidos), solo con identidad Quest 3 (`KL_QUEST3`; `KL_OVRP_GLOBAL_MESH` lo fuerza): un Quest 2 no tiene malla. Profundidad y raycast siguen usando los trozos.
- Profundidad de entorno: forward-Z de Meta (0 cerca, 1 lejos), desde la malla de escena del visor.
- Agentes: los que el usuario autorice en la sesión (2026-11-10: agentes Fable para análisis y diagnóstico); si pide que no, el Director investiga y escribe.

## Files in play
- `klepton/runtime/kl_memrule.h`, `klepton/runtime/libc/{kl_libc.c,kl_gswap.h}`, `klepton/runtime/gfx/kl_vulkan.{c,h}`, `klepton/runtime/xr/{kl_spatial.c,kl_spatial.h,kl_ovrp_spatial.c,kl_ovrp.c,kl_envdepth.c}`, `klepton/runtime/xr/tests/spatial_test.c`, `klepton/tests/t_lowmem.c`, `klepton/visionos/Sources/KleptonEnvDepth.swift`, `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/guardian.md`: reglas del guardian Quest 3, API kl_guardian, flujo Swift, claves vq.*.
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).
