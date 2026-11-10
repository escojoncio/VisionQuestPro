# Historial por juego (causas demostradas e hipótesis refutadas)

## AC Nexus (Unity/Vulkan, Wwise)
- Jetsam ~8191 MiB por ~39.000 blit encoders (~39k regiones IOAccelerator) en ~4 s → presupuesto de encoders con contrapresión (VPKEngine). Sin confirmar.
- Resolución 2976→3840: el plugin ensancha el desc y AC pide textureScale 1,3 → `klovrp_fill_eye_desc` no multiplica por el escalado del juego.
- «Se ve la habitación» en carga: causa sin confirmar. Refutado: alfa 0 del backfill.
- Warmup 22 s → 4,5 s: sello de caché por `vpvk_translator_tag` + pipelines async.
- Congelación del guest: >3,0 s sin serial y `staleInARow>=8` → backfill.
- Cuadrados rosas: sin identificar (candidato: capa Quad/head 512x512 clear-only).
- `OVRInput connected=0x0` toda la sesión, pero `GetControllerState4` da `Connected=0x3`; nadie llama a `ovrp_GetConnectedControllers*`. No bloquea.
- Usa `ovrp_EnqueueSubmitLayer2` (sin factores de blend; el plugin rellena SrcAlpha/OneMinusSrcAlpha).
- Refutado: H0 de memoria (VkDeviceMemory host-visible noCopy) no es la causa del salto.

## Demeo (Unity OpenXR + vpgles)
- SIGABRT por `GetPrimitiveArrayCritical` → JNI completo. `([F)V` como escalar → arreglado. IAP → fallo OFFLINE.
- Cuadro gris `Texture must have width greater than 0` → INFO_OUTPUT_FORMAT_CHANGED en modo superficie. Sin confirmar.
- Vídeo de logos NO era EXTERNAL_OES. Riesgo: hilo bloqueado esperando el primer frame.
- Passthrough negro: alfa moría en vpgles `formatByVk` (fila RGB para imágenes envueltas). Arreglado en el motor. Sin probar.
- Congelado ~55 s por guardado de MTLBinaryArchive en el worker → guardado en segundo plano (VPKEngine).

## Batman (Vulkan)
- Congelaciones de 40 s → pipelines async también con perfil completo. Pase vacío MSAA fusionado.
- Fin de frame segmentado no dio fps y metió judder → apagado. Memoria: 847 MB `tex_msaa` por profundidad MSAA muestreada (corte Feedback).

## AoT (Unity/Vulkan, IL2CPP)
- GPU al límite con MSAA x2 → 50 Hz (`HALF RATE`); `KL_CP_HALFRATE_RECOVER`. `nativeLowMemory` solo en el hilo del juego.

## SuperHOT (Unity, vpgles)
- Negro (`SetupEyeTexture2` solo ANGLE) → `kl_gles_legacy_eye_texture` + `vpgles_wrap_image_as`. Sin reprobar.

## Hellsplit (UE4)
- Ojos separados en cargas → drop de EndFrame4 sin capa de ojo (`ever_eye_layer`); envolvente háptica. Sin reprobar.

## TRIANGLE STRATEGY (UE4)
- Deadlock en rwlock → rwlocks propios; `[snap] game thread silent`. Sin reprobar.

## Resident Evil 4 (UE4 4.25, Vulkan, `com.Armature.VR4`)
- "does not support Vulkan": el target es "Resident Evil 4" (carpeta) → firma 4.26 de `nativeSetAndroidVersionInformation`. Fix: `is_425` por paquete. Además parche `re4-force-vulkan-available` (`IsVulkanAvailable` → `mov w0,#1; ret`).
- Stack smashing en `cCard::saveFileCheck` (ruta de guardado larga) → symlink corto `<Documents>/.f`. Riesgo: `realpath()` del guest.
- Jetsam (log b27f425, ~157 s, al abrir el inventario tras cambiar de zona): memoria GPU no contabilizada 0,2→3,7 GB; `[vkagg]` memtype0 +94 allocs vivas / +2,3 GB; img_create 2422 vs destroy 1023. `tex_msaa` 275–551 MiB con `msaa_freed_total=0`. `onLowMemory` entregado (x7) sin liberar.

## Deadpool (UE4, libUE4 270 MB)
- Veneers: 4138 sitios x18 en 120,4–122,3 MB → pool bajo alcanza.
- Audio: resampleo en `kl_opensl.c`. Idioma: getter de metadata de packs devolvía "" → `kl_ovrplat.c` pide la versión al juego. Sin verificar.
- Sombras depth-only con `draws=0`: causa NO demostrada (ver bitácora de VPKEngine).
- Crash al Salir en `KleptonCompositor.eyeSource` (teardown). Jetsam +225 s comp=1914M sin atribuir.

## Gundam (Unity/Vulkan, MSAA 4x)
- Jetsam +48 s (foot 8038M, `tex_msaa` 1478 MB). Mandos: botones llegan, `connected=0x3` desde f1440, EventSystem `selected=<none>`; `GetControllerState6(mask=0x10/0x60)` → Connected=0.

## Thrill of the Fight 2 (Unity/Vulkan)
- Ojos mal: ensanche 2815→3552 y ambos ojos colapsados → unión de frustums. `tex_msaa` 1027 MB.

## UBOAT (UE4/GLES)
- SIGABRT `fatal: called unresolved import 'EOS_Initialize'` → `libEOSSDK.so` en `UE4_CHAIN` (`KL_REFUSE_EOS=1` lo omite).

## Maestro (UE4/GLES)
- CODESIGNING 0x2, PC=0 en hilo de libUE4 tras `ovrp_GetSystemHeadsetType2`. Candidatos: `ovrMessageType_ToString` sin resolver, `eglGetProcAddress` no resuelto. Sin cerrar.

## HLVR = Harem Life VR (Unity 2022.3, ovrp 1.103, MRUK v71; carpeta `homebrew`)
- Quest 3: el juego activa MR (MRUK C# `CreateSceneDataFromDevice`: rooms → container → hijos → labels/BB2D/Boundary2D/LocateSpace2 por hijo). `_cameraRig` se asigna en `MRUK.Start`; ver `kl_evgate.h`. Quest 2: sin MR, suelo propio.
- Pide origen suelo y luego ojos (`ovrp_SetTrackingOriginType`=0); el plano FLOOR llega a y=-eye.
- Profundidad de entorno (DEMOSTRADO con libil2cpp, `EnvironmentDepthUtils`): forward-Z, d = f(z-n)/((f-n)z), 0 cerca, 1 lejos/sin geometría; el shader decodifica x/((2d-1)+y) con x=-2fn/(f-n), y=-(f+n)/(f-n); los fov del frame son tangentes. El reversed-Z de la build 0b7b5be decodificaba todo a ~0,1 m (modelo oculto).
- Colisiones/colocación en Quest 3 = `EnvironmentRaycastManager` → `EnvironmentDepthRaycaster` (compute que copia la depth a un ComputeBuffer + `AsyncGPUReadback`); la oclusión usa el shader de preprocesado de Meta (vertex `SV_RenderTargetArrayIndex`, `DrawProceduralNow(...,3,2)` sobre RGBA16F 320x320x2), que fallaba `SetPass` sin `VK_EXT_shader_viewport_index_layer` (logs 27, frame 408). Un pipeline con el shader de error de Unity en un pase 1x desde ese frame (causa sin identificar).
- `libMetaXRAudioUnity.so` rechazada por AMFI (global) → plugin nulo. `libmrutilitykitshared.so` carga traducida.
- Crash antiguo: firma de `ovrp_DiscoverSpaces` (corregido). Etiquetas: count sin NUL (`Unknown classification`).

## Gundam Silver Phantom (añadido 2026-11-10)
- AR funciona con sala de zona dibujada (`[room] source=guardian`, 17 miembros) + etiquetas sin NUL; el juego ya no destruye los espacios (antes CheckRules fallaba).
- Nivel tras el AR: 14x 8192² ASTC6x6 + 8x 8192² ASTC4x4 + 2x 8192² RGBA8 (358 y 268 MiB) pedidos por el juego; ojo al 130 % = 825 MiB entre RT y MSAA materializada.
- Cierre de logs 27 (t=77 s, jetsam): foot 8132M = gfx 4683M (astc 1827, color 1995, rt.mat 563M/28, msaa.mat 478M/6) + int 2176M + comp 1056M; el swap de mmap solo movía 512M porque lo grande del guest es malloc (MALLOC_LARGE 1357M res + 417 comp) → bloques malloc >= 4 MiB al swap (`kl_gswap_big`, zona malloc).
- Unity 2021.3.15f1. 6 pipelines con el shader de error de Unity en el pase de ojo durante `LVL_Warmup` (frame ~1092), sin aviso `Unsupported` de Unity: causa sin identificar (¿variantes de la colección de warmup?).

## Into the Radius 2 (UE 5.5, Vulkan ES3.1 móvil; añadido 2026-11-10)
- Un submit por frame con 5 pases (ver bitácora del motor). MSAA 4x propio, adjuntos TRANSIENT con LOAD/STORE.
- `vk.tex_hist` en nivel (logs 27): 3165 texturas con >= 6 mips = 197 MiB (media 62 KiB, ~256 px), 298 con 1-2 mips = 108 MiB, RT 41 = 489 MiB; constante durante la sesión. Sin log de UE (shipping). Síntoma visual (texturas de poco detalle, trozos de mapa que faltan) sin causa demostrada.
- Draws indirectos con argumentos escritos por CPU (`vk.indirect_stats cpu_readable=todos`, hasta 5415 instancias); `draws_skipped=0`, `vk.err`=0, `pso failed=0`, sin shader de error.
- Descartado: shaders/pipelines (failed=0), Nanite/Lumen, memoria (/proc/meminfo 8 GiB, heap Vulkan 6 GiB), formatos, tamaño reportado de imágenes ASTC (`imageBytes` por bloques), trim del motor (no suelta contenido ni PSO).
- UE resuelve (sin que conste que use) funciones de ray tracing, descriptor buffer, mesh shader, device fault, calibrated timestamps: reciben stub.
- Binario revisado (2026-11-10, carpeta del PC ya conectada: `arm64-v8a/libUnreal.so` 152 MB, APK 74 MB, OBB main 4,1 GB + patch 2,2 GB): UE shipping sin cadenas de `UE_LOG` (no habrá log propio); la config (DeviceProfiles, TextureLODGroups, pool de streaming) va en los OBB, que superan el límite de traslado (400 MB/fichero) ⇒ perfil de dispositivo elegido por UE desconocido. `UECommandLine.txt` del APK solo trae `-project`.
- Descartado además: consultas de oclusión (ninguna lectura, no hay `vk.query_stats`), compute del juego para culling (los 72 dispatches/frame son `vk.indirect_patch` del motor), fetch de atributos por instancia con stride 0 (PrimitiveId stream de GPUScene: `dividedInstance`/stride en lower.cpp correcto), clear por quads en área parcial. Carga estable en nivel (memoria y texturas planas) ⇒ el síntoma es estado estacionario, no streaming en curso.
- Siguiente paso objetivo: captura VPCAP de un frame del nivel (ajuste «Captura de frames») y `replay.yml` (lavapipe vs Metal) para obtener `FIRST DIVERGENCE`; en la rama `captures` del motor solo hay `maestro.vpcap`.
