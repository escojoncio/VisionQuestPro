# Bitácora — VisionQuestPro

## Last done
- 2026-11-10 (3) ITR2, niebla ausente (dato del usuario): UE móvil 5.5 la dibuja en el subpase 1 del pase principal leyendo la depth 4x como input attachment; pin VPKEngine `2efe696` añade `vk.sub_dump`/`vk.sub_draw` (draws de ese subpase con su estado) para saber si el draw llega. Candidatos ya dentro de la build: `VK_EXT_shader_viewport_index_layer` (UE 5.5 la conoce y el Quest la tiene; antes no la anunciábamos). Extensiones que UE conoce, el driver de Quest lista y no anunciamos: ver `notas/juegos.md`.
- 2026-11-10 (2) Tirones de Gundam: no son shaders (ninguna compilación en la sesión); el motor liberaba y volvía a reservar render targets de 46-100 MiB cada ~120 frames (45-58 ms cada vez) ⇒ pin VPKEngine `765fd08` (histéresis), build 4521d4b (CI OK, IPA publicada). ITR2: revisados binario y cache de PSO de UE, ver `notas/juegos.md`. SIN probar en visor.
- 2026-11-10 Lote tras logs 27 (build 0b7b5be probada: Gundam sigue cerrándose, ITR2 igual, HLVR Quest 3 carga la sala pero con depth el modelo desaparece y nada colisiona). Lote anterior = build 4f381bd (CI OK), pin VPKEngine `c6a665b`, SIN probar en visor; el Gate de macOS pasa t_gswap (con la zona malloc), t_evgate, t_envdepth_z y ctest 356/356.
- HLVR depth, dos causas DEMOSTRADAS: (1) convención: Meta usa forward-Z (0 cerca, 1 lejos); servíamos reversed-Z ⇒ todo decodificado a ~0,1 m. `kl_envdepth.c` (`kled_depth_fwd`, `kled_proj_ab`), `KleptonEnvDepth.swift` (compare less, clear 1), test `t_envdepth_z`. (2) el shader de preprocesado de profundidad de Meta fallaba `SetPass` porque el motor no anunciaba `VK_EXT_shader_viewport_index_layer` (Unity lo exige por nombre; el driver real de Quest 3 lo lista) ⇒ añadida en el motor. La profundidad ya salía de la malla de escena del visor (`KLSP_MESH`). Diagnóstico nuevo: `[envdepth] provider started`, `picture N: M of K scene meshes (T triangles)…`, `acquire #N`.
- Gundam cierre (logs 27, t=77 s, foot 8132M): lo grande del guest es malloc, no mmap ⇒ `kl_gswap.h` sirve también bloques de la familia malloc >= 4 MiB (`kl_gswap_big`; tabla `klgs_blks`; zona malloc registrada `klgs_zone` v9 para que free/realloc/malloc_size los encuentren desde cualquier llamante); wrappers `klb_malloc/calloc/realloc/posix_memalign/aligned_alloc/valloc` en `kl_libc.c` (tabla del shim; antes KL_FWD); `[mem] … blocks= blocks_total=`. Unverified en visor (la sección Apple del test corre en el Gate de macOS).
- ITR2 rendimiento (motor): clear completo en adjuntos TRANSIENT con área parcial y stores del cliente en el subpass que lee su depth (ver bitácora del motor). Síntoma visual de ITR2 sin causa: ver `notas/juegos.md`.
- Revisión de shaders/RAM pedida por el usuario: en los tres logs `vk.err`=0, `pso failed=0`, `draws_skipped=0`; el trim del motor no suelta contenido ni PSO; único hueco de soporte de shaders hallado = la extensión de arriba. Shader de error de Unity: 1 pipeline en HLVR y 6 en Gundam (warmup) sin causa identificada.
- 2026-11-10 (build 0b7b5be, probada): `kl_evgate.h` (eventos nunca en el drenaje que los pidió) ⇒ HLVR Quest 3 carga la sala (`kids=14/14`); plugin nulo `MetaXRAudioUnity`; guest swap de mmap; espera del ancla de calibración 8 s.

## Current state
- Funciona en visor: Gundam AR, Maestro (render y MSAA x4 forzado), HLVR perfil Quest 2, HLVR Quest 3 carga de sala (MRUK), calibración global (cuando el ancla se rastrea).
- Sin probar (este lote): HLVR Quest 3 con depth (oclusión y raycast), cierre por memoria de Gundam, ITR2 (fps).
- Leer en la próxima prueba: HLVR sin `DrawProceduralNow requires a successful material.SetPass`, `vk.device_req … VK_EXT_shader_viewport_index_layer`, `[envdepth] picture`/`acquire`, `dispatches=` y errores de readback; Gundam `[mem] … gswap live= blocks=` y foot máximo; ITR2 `vk.sub_draw` (¿hay un draw de 3-4 vértices con blend en sub=1?), `gpu.enc` (i4 `c0:CS,d:CS`, i5 `c0:LR,d:LR`), ausencia de `msaa_stale`, `gpu_ms`, `vk.rp_dump` del nivel.
- Pendiente: ITR2 síntoma visual (binario revisado sin causa, ver `notas/juegos.md`; falta captura VPCAP del nivel para `replay.yml`); ITR2 fusionar i4+i5 (depth de input en tile); Gundam GPU (RT/MSAA materializadas al 130 %) y `comp`≈1 GB; shader de error de Unity en HLVR/Gundam; `E/Unity: Incompatible SDK version… MetaXRAudioFMOD` (el plugin nulo devuelve versión 0); calibración por habitación (ofrecida, sin respuesta); menores del guardián; AoT x4; tarjeta del launcher de ITR2 dice GLES siendo Vulkan.
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
- Memoria del guest: mmap anónimos y bloques malloc >= 4 MiB respaldados por fichero cuando el footprint pasa del 45 % del límite (`kl_gswap.h`; ajuste «Memoria extendida», `KL_GSWAP=0` lo apaga).
- Profundidad de entorno: forward-Z de Meta (0 cerca, 1 lejos), desde la malla de escena del visor.
- Sin agentes cuando el usuario lo pide expresamente: el Director investiga y escribe.

## Files in play
- `klepton/runtime/xr/{kl_envdepth.c,kl_envdepth.h,kl_evgate.h}`, `klepton/visionos/Sources/KleptonEnvDepth.swift`, `klepton/runtime/libc/{kl_gswap.h,kl_libc.c,kl_bionic_extra2.c,kl_shim.c,kl_libc_table.h}`, `klepton/tests/{t_gswap,t_envdepth_z}.c`, `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/guardian.md`: reglas del guardian Quest 3, API kl_guardian, flujo Swift, claves vq.*.
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).
