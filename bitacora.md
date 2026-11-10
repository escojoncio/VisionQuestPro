# Bitácora — VisionQuestPro

## Last done
- 2026-11-10 Lote tras logs 26 (build de 40aba2f probada en visor; este lote = build 0b7b5be, CI run 42729728397 OK e IPA publicada, SIN probar en visor; pin VPKEngine `a4eccac`; todo compila en CI y t_gswap/t_evgate pasan en macOS). Resultado del visor: AR de Gundam funciona (sala de zona dibujada + etiquetas sin NUL), MSAA forzado de Maestro funciona (`passes=5527`, sin skips, 94-100 fps), HLVR en Quest 2 bien.
- HLVR en Quest 3 (DEMOSTRADO con libil2cpp: `MRUK.<CreateSceneDataFromDevice>d__68.MoveNext` +0x4244e5c lee `this._cameraRig` (0x90) y lanza NRE; `_cameraRig` se asigna en `MRUK.Start`, la carga empieza en `Awake`): nuestros eventos 300/301 se entregaban en el siguiente poll, así que toda la carga corría en el drenaje del frame de `Awake`, antes de `Start` ⇒ sin sala ni collider de suelo. Arreglo: `klepton/runtime/xr/kl_evgate.h` (un evento solo es entregable en un drenaje posterior y tras 50 ms), usado en `kl_ovrp.c` (`g_ev_drains`) y `kl_openxr.c` (`g_xr_drains`); test `t_evgate`. Además `MetaXRAudioUnity` servido como plugin nulo (`kl_dl.c`: `ovrAudio_*/MetaXRAudio_*` devuelven 0; antes DllNotFoundException por frame, 5747).
- Gundam cierre a +566 s = jetsam por límite del proceso (foot 8051M de 8192M; sin señal): al salir del AR carga 22 texturas 8192² (ASTC 1830M + RGBA8 600M) y el heap del guest sube ~1 GB (16 mmap = 2,3 GB). Arreglo: guest swap `klepton/runtime/libc/kl_gswap.h` (mmap anónimos privados no ejecutables >= 4 MiB del guest sobre ficheros temporales sin nombre: no cuentan en phys_footprint y el kernel puede escribirlos a disco); política `kl_gswap_on` en `kl_libc.c` (auto: se engancha al 45 % del límite; `KL_GSWAP`, `KL_GSWAP_AT`); `klb_munmap` nuevo (tabla del shim; antes KL_FWD), `kl_guest_anon_map` para `klb_mremap`, MADV_DONTNEED y MAP_FIXED sobre lo nuestro = páginas nuevas del mismo fichero (`kl_gswap_fresh`); `[mem] … gswap live= files= total= fresh= refused=`; ajuste «Memoria extendida (intercambio a disco)» (`vq.guestSwap`, on). Test `t_gswap` (Linux). Unverified en visor: coste de fallos de página de fichero, que F_PUNCHHOLE suelte páginas sucias, espacio en disco.
- ITR2 (ver bitácora del motor): UE pide LOAD+STORE de MSAA 4x transient cada frame; al dejar de ser tile-local mueve ~2 GiB/frame por memoria (20-25 fps). Motor: la máscara UI ya no corta la continuación MSAA y `vk.rp_dump` muestra el render pass de Vulkan. La build 40aba2f NO llevaba `vk.device_req`/`vk.tex_hist`/`[ue-log]` (esta sí).
- Calibración: el ancla falló «not tracked in 3 s» en 2 de 4 arranques ⇒ `VQCalibration.applyAtLaunch` espera hasta 8 s y además lee `tracker.allAnchors` cada 200 ms (sale a los 3 s si el ancla no está en la lista: «not in this space»).
- 2026-11-09 (build 40aba2f, probada): etiquetas ovrp sin NUL, rejilla del guardián a la cabeza, calibración global por WorldAnchor (`vq.calib.v1`), MSAA forzado con stencil Load, sala real + dibujo de zona. Detalle en `notas/guardian.md` y git.

## Current state
- Funciona en visor: Gundam AR, Maestro (render y MSAA x4 forzado), HLVR perfil Quest 2, calibración global (cuando el ancla se rastrea).
- Sin probar (este lote): HLVR Quest 3 (suelo/colisiones), cierre por memoria de Gundam, ITR2, espera del ancla.
- Leer en la próxima prueba: `[sp-ev] polled` un frame después del push; HLVR sin NullReferenceException y `[sp-sum] kids=14/14` con LocateSpace2 > 1; `[gswap] on:` y `[mem] … gswap live=` (Gundam: foot máximo, `refused`); `vk.rp_dump` y `vk.taa uimask skipped_pass why=msaa_continuation` (ITR2: ¿i4+i5 en un encoder? `gpu.enc`, `gpu.sec p95`); `vk.device_req … unsupported=`, `vk.tex_hist`, `[ue-log]`; `[calib] applied`.
- Pendiente: ITR2 rendimiento de fondo (LOAD/STORE entre frames prescindible, depth de input en tile, resolve de depth en tile: decidir con `vk.rp_dump`); ITR2 texturas en stubs 64x64 (¿presupuesto del streamer?, sin causa); Gundam `comp`≈1 GB y malloc grande del guest fuera del swap (MALLOC_LARGE ~650 MiB sin atribuir guest/host); rematerialización de MSAA tras trim con poco margen; menores del guardián (Confirmar sin mando/manos ni suelo detectado; rejilla en `savedSection`); AoT x4; tarjeta del launcher de ITR2 dice GLES siendo Vulkan.
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
- Memoria del guest: mmap anónimos >= 4 MiB respaldados por fichero cuando el footprint pasa del 45 % del límite (`kl_gswap.h`; ajuste «Memoria extendida», `KL_GSWAP=0` lo apaga).
- Sin agentes cuando el usuario lo pide expresamente: el Director investiga y escribe.

## Files in play
- `klepton/runtime/xr/{kl_evgate.h,kl_ovrp.c,kl_openxr.c,kl_ovrp_spatial.c,kl_spatial.c}`, `klepton/runtime/libc/{kl_gswap.h,kl_libc.c,kl_bionic_extra2.c,kl_shim.c,kl_libc_table.h}`, `klepton/runtime/kl_dl.c`, `klepton/tests/{t_evgate,t_gswap}.c`, `app/Sources/{VQCalibration,VQLaunch,VQLauncher}.swift`, `.github/workflows/visionos.yml` (`ref:`).

## Module index
- `notas/guardian.md`: reglas del guardian Quest 3, API kl_guardian, flujo Swift, claves vq.*.
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
- `notas/captura.md`: captura VPCAP en visor, reproducción y comparación (vpreplay, vpimgdiff, replay.yml).
