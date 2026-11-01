# Bitácora — VisionQuestPro

## Last done
- 2026-11-01 Build `[build]` con VQP + pin VPKEngine `1b50d48` (GLES uniform lenient `4b44477`, harvest con presión de memoria `122fdb3`). Sin probar en visor.
- 2026-11-01 `fa624ba`: `ovrp_RequestSceneCapture` sin planos ARKit tras la espera crea habitación sintética 4x4x2,5 m centrada en la cabeza (`kl_spatial_synth_room`, planos uuid "KLSYNTH"); se purga con room real o suelo+3 paredes reales; `SetSpaceComponentStatus` guarda enable (anillo 256, `cs_*`, limpiado en Destroy/Erase). `KL_SPATIAL_CAPTURE_TIMEOUT_MS` (defecto 8000). spatial_test OK (ASan/UBSan).
- 2026-11-01 `10c795a`: `[vk] [magenta-bind]` (8 primeros binds de pipelines con error shader de Unity; antes solo se veía la creación, que Unity hace siempre); palabras clave `subshaders`/`fallback shader`/`this GPU` en copia de log del guest (kl_va_handlers.c); `AAsset_read` -1 con ferror, `getBuffer` NULL con lectura corta y creación bajo mutex (t_aasset); `vq.psoInQuest` por defecto true (caché de pipelines en `QUEST/Cache/<juego>/Pipelines`, no se pierde al reinstalar).
- 2026-10-28 Logging permanente (VPK `03aec8c`): `[sp-call]` (args+ret de ovrp espaciales, 20/fn), `[sp-ev] push|polled`, `[pt] expire detail`, `[diag]` presupuesto 2000 líneas, `[fatal] last log lines` (8 últimas líneas en klepton-crash.log). Ficheros: klepton/runtime/xr/kl_ovrp_spatial.c, kl_ovrp.c, kl_ovrp_priv.h, klepton/runtime/kl_fault.c y .h, klepton/visionos/Sources/kl_app.c. kl_ovrp.c y kl_fault.c no compilados en Linux (unverified hasta CI). Sin cambio de comportamiento. Etiquetas en notas/diagnostico.md.
- 2026-10-28 `184e4dd` (revisión adversarial): fin de frame espera solo las colas del guest usadas en el frame (hasta 8, `QueueWaitIdle` bajo `g_qsub_mu`; sin `gq_multi` pegajoso ni `DeviceWaitIdle`); `klvk_shadow_pump(q)` en la cola del guest; suelo de retiro para imágenes normales `(age>=after && issued>=frame+8 && gpu_serial>=frame+1) || age>=4*after`; `vkQueueSubmit2/KHR` → NULL si el driver no lo tiene; `klvk_wq` fija la cola 0 de Klepton (acquire, clears y marcador). Tests `t_frame_fence`, `t_retire_due`, `t_vk_trap` (kl_vulkan.c entero sin compilar fuera de visionOS).
- 2026-10-28 `77d58dc`: hooks `klvk_QueueSubmit2` (`vkQueueSubmit2` y `vkQueueSubmit2KHR` en `g_ours[]`, misma lógica que `klvk_QueueSubmit` sin trace/probe); `docs/perfil-adreno740.md` al día.
- 2026-10-27 `57b09ba`: Klepton alineado con el perfil Quest 3 único del motor: multi-cola (`klvk_wq`, `klvk_note_guest_queue`), `EXT_metal_objects` interno vía gipa, lista de extensiones 128, fuera `KL_VK_VTX_SSBO`/`KL_VK_IMG_NOFMT`/parche ASTC, fuera ajuste «Perfil Vulkan completo» y `VPVK_EXTRA_EXTENSIONS`; VQConformance con `VPVK_FORCE_ANISO=0`, `VPVK_QUEST_IDENTITY=1`, esperas 120000.
- 2026-10-27 `872ffa3`: fuera VPKTAA y foveado propio; ajuste MSAA Juego/x2/x4 (`VPVK_MSAA_FORCE`, global y por juego, siempre memoryless); captura instantánea (`vq.cap.hold.*`, defecto 0).
- 2026-10-27 Build `7f66ae7` (run 40562139335): causas medidas de ITR2/Gundam/captura; detalle en `notas/juegos.md`.

## Last build candidata (2026-10-28)
- Gundam AR: `STORAGE_LOCAL` era 0 (OVRPlugin: 0 Invalid, 1 Local, 2 Cloud; el juego envía loc=1 y `run_query` devolvía 0 siempre) → ahora 1; loc 0 se sigue sirviendo como Local, Cloud no (`IS_LOCAL_LOC`, kl_ovrp_spatial.c). Valor del enum de memoria, unverified en dispositivo. Log nuevo `[sp-call] ... q#k filterType= ...` + `info64=`. Test spatial_test.c OK.
- MSAA forzado: AoT solo twinnea 6 pasadas 1116x1129 s4; la del ojo 3552x2258 no; contadores `vk.msaa rej` en el motor para ver por qué. El usuario no nota diferencia con forzado en AoT.
- ITR2 sigue muriendo por SIGKILL del sistema tras >7 s sin frames del guest durante compilación de shaders (356 libs, 11,5 s); `.ips` 20:34:42 pendiente de analizar (el usuario lo adjuntó). Maestro (uniform_err prog=2873/4751) y FMOD sin arreglo. Ver ramas wip/2026-10-28 ya mergeadas.

## Current state
- Última build verde: run 40694850720 (VQP `81136fc`, VPK `8c21013`); build 2026-11-01 lanzada (ver Last done).
- Pendiente (orden):
  1. Logs 2026-10-28 (VQP `81136fc`) analizados; sin causa demostrada aún:
     - Gundam crash: SIGSEGV libfmod.so+0x7d3f8, hilo guest, dirección no mapeada; RAM descartada (foot 3771M, lim_rem 4420M); sin símbolos de libfmod.
     - Gundam RA: `RequestSceneCapture` devuelve id síncrono y el evento 100 sale tras ≤8 s+1,5 s (`kl_ovrp_spatial.c:701-735`); room posiblemente sintética (`kl_spatial.c:67,560`); `SetSpaceComponentStatus` ignora enable (`:465-482`); capa passthrough 4 expira a 3 EndFrame4 sin re-encolar (`kl_ovrp.c:6998-7056`). Causa de que el botón de Space Setup no cree la habitación: unverified; falta el retorno numérico de las llamadas ovrp_* spatial en el log.
     - Gundam gráfico: 6 fragment modules magenta (Unity error shader) `[vk] [magenta]`, `[imgfmt] fmt 157/151/127 UNSUPPORTED` (3), PSO failed=0.
     - ITR2 (sesión 18:05Z, `81136fc`): SIGKILL del sistema a los 13 s durante compilación de shaders (foot 3028M, lim_rem 5163M, compresor saturado); `msaa_materialized=49` (1249M) con `transient_violation` en tex 77/80: `Load` sobre TRANSIENT en su primer attach materializa la imagen para siempre (`vpkengine gpu/core/src/frame_compiler.cpp:1056-1059`). Propuesta pendiente: tratar ese Load como contenido indefinido y mantener memoryless. Causa del kill: unverified.
     - Gundam FMOD (desensamblado de libfmod): el fallo es el decodificador FADPCM (función en +0x7d370) leyendo `ldrsh [x8,#8]` con x8=0x715a823c1c devuelto por un método virtual (vtable slot 4) del objeto en `[x21+0x170]`; la página 0x715a823000 está sin mapear (1,4 MB tras el fin de una región Malloc Large). Descartado: mmap/munmap de ficheros (no hay en el log; audio leído con read), alineación, burst. Hipótesis sin probar: bloque de datos de audio liberado mientras se decodifica (carrera de unload) o EOF mal calculado. Sin símbolos de build-id.
     - Maestro morado: UE4 sube `glUniform4iv/4uiv` sobre `vec4 vc0_h[]` (Adreno copia bits); el motor lo descartaba. Arreglado en motor `4b44477`; unverified en visor (buscar `gles.uniform_lenient`).
     - Gundam FMOD: libfmod usa AAsset_* por dlsym (AAssetManager_fromJava) y fopen/read; sin AAssetFileDescriptor. En +0x7d370 el puntero sale de un método virtual (slot 4, vtable+0x20) del objeto en `[this+0x170]`, args (x1=&out, w2=0x8c), registros de 12 bytes. Quién rellena +0x170: candidatos 0xa7470/0xe4b94 (sin confirmar). Sin arreglo.
     - Magenta Gundam: los 6 módulos son el error shader que Unity 2021.3.15f1 crea siempre; no prueba que se dibuje. Ver `[magenta-bind]` y líneas "All subshaders removed" en el próximo log.
     - ITR2: PSO service de UE5 (libpsoservice, `PSOProgramService`) no existe; `bindService` devuelve false (kl_jni_window.c:477) y UE compila en proceso. Mitigación del kill: harvest cede (motor `122fdb3`) y caché en QUEST.
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
