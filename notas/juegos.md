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
- Pide origen suelo y luego ojos (`ovrp_SetTrackingOriginType`=0); el plano FLOOR llega a y=-eye. Profundidad de entorno 320x320 D16 reversed-Z (convención unverified).
- `libMetaXRAudioUnity.so` rechazada por AMFI (global) → plugin nulo. `libmrutilitykitshared.so` carga traducida.
- Crash antiguo: firma de `ovrp_DiscoverSpaces` (corregido). Etiquetas: count sin NUL (`Unknown classification`).

## Gundam Silver Phantom (añadido 2026-11-10)
- AR funciona con sala de zona dibujada (`[room] source=guardian`, 17 miembros) + etiquetas sin NUL; el juego ya no destruye los espacios (antes CheckRules fallaba).
- Nivel tras el AR: 14x 8192² ASTC6x6 + 8x 8192² ASTC4x4 + 2x 8192² RGBA8 (358 y 268 MiB) pedidos por el juego; ojo al 130 % = 825 MiB entre RT y MSAA materializada.

## Into the Radius 2 (UE 5.5, Vulkan ES3.1 móvil; añadido 2026-11-10)
- Un submit por frame con 5 pases (ver bitácora del motor). MSAA 4x propio, adjuntos TRANSIENT con LOAD/STORE.
- Texturas: ~2950 ASTC de 64x64 (7 mips) y ~190 mayores; el streamer deja de pedir mips con memoria libre (causa sin demostrar). Sesiones probadas de ~50 s (10 s de nivel).
- Descartado: shaders/pipelines (failed=0), Nanite/Lumen, memoria, formatos.
