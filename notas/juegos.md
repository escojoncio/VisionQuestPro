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
- Oclusión: shader de preprocesado de Meta (vertex `SV_RenderTargetArrayIndex`, `DrawProceduralNow(...,3,2)` sobre RGBA16F 320x320x2); exige `VK_EXT_shader_viewport_index_layer` (anunciada desde c6a665b; logs 28: sin error `SetPass`, pase presente cada frame). Parcial por diseño: con malla no se dibujan planos (hueco = lejos), la malla de ARKit es estática (sin manos ni objetos movidos) y la imagen es 320 px.
- Colisiones (logs 28, DEMOSTRADO con libil2cpp): NO van por raycast de profundidad (`EnvironmentRaycastManager` sin llamantes del juego). `SceneGlobalMeshManager` pone `MeshCollider.sharedMesh = MRUKAnchor.GlobalMesh`, que exige un hijo de la sala con etiqueta `GLOBAL_MESH` y llama a `ovrp_GetSpaceTriangleMesh(ref space, ref TriangleMeshInternal)`. La fachada ovrp solo daba planos (13) y tenía la firma de 7 argumentos ⇒ corregido: malla global única (`kl_spatial_global_mesh`, trozos de ARKit unidos en el marco del origen), miembro final del contenedor, etiqueta, componente y ABI de 2 punteros. Sin probar en visor (pose/orientación de la malla sin ejercitar).
- `libMetaXRAudioUnity.so` rechazada por AMFI (global) → plugin nulo. `libmrutilitykitshared.so` carga traducida.
- Crash antiguo: firma de `ovrp_DiscoverSpaces` (corregido). Etiquetas: count sin NUL (`Unknown classification`).

## Gundam Silver Phantom (añadido 2026-11-10)
- AR funciona con sala de zona dibujada (`[room] source=guardian`, 17 miembros) + etiquetas sin NUL; el juego ya no destruye los espacios (antes CheckRules fallaba).
- Nivel tras el AR: 14x 8192² ASTC6x6 + 8x 8192² ASTC4x4 + 2x 8192² RGBA8 (358 y 268 MiB) pedidos por el juego; ojo al 130 % = 825 MiB entre RT y MSAA materializada.
- Cierre (logs 27 y 28, jetsam al cargar el nivel, t≈72 s): dos cargas idénticas (la 1.ª destruye 126 imágenes al terminar; pico 8057M, sobrevivió por 134M) y la 2.ª muere con foot 7886M = gfx 4502M (astc 1831, color 1873, rt/msaa mat. ~700) + int 2207M + comp 973M. Los bloques malloc >= 4 MiB al swap no movieron nada (`blocks=1`): los datos del guest van por mmap (ya en swap desde el 45 %) y el resto es Metal. Mapa de regiones a 6,9 GB (parcial): MALLOC_LARGE 67 res + 713 comprimidos, guest_mmap 339+327 (mapeos anteriores al 45 %), MALLOC_SMALL 148+313.
- Lo que pide el juego en esa carga (formatos Vulkan del propio juego, no convertimos nada): 14x 8192² ASTC6x6 (39,8 MiB) + 8x 8192² ASTC4x4 (85,3 MiB) + RGBA8 sin comprimir 8192² con mips (344 MiB), 8192² sin mips (258 MiB), 2x 4096² (86 MiB), ~13x 2048² ⇒ ~3 GB de texturas. Un Quest 3 lo aguanta con 4 GiB de zram (`ro.vendor.swap.zram_size` en su firmware) y sin límite por proceso.
- Arreglo (build de logs 28+1, sin probar): presupuesto de texturas (motor `vpvk_texture_budget`; Klepton `kl_texbudget_cap`: >= 50 % del límite ⇒ tope 4096, >= 70 % ⇒ 2048, >= 85 % ⇒ 1024; solo aprieta; `KL_TEX_BUDGET=0` lo apaga), swap del guest desde el 20 % (antes 45 %), memoria host de Vulkan en mapeos propios (tag 241).
- Tirones (logs 27, medidos con `vk.hitch`; en la sesión `compiled_in_session=0`, `pso_create_ms`≈0: ningún shader/pipeline se compiló, todo salió de las recetas): (a) `backend_ms` 45-58 ms cada vez que un render target de 46-100 MiB vuelve tras `msaa_released` (288 materializaciones y 168 liberaciones en 77 s) ⇒ histéresis en el motor (765fd08); (b) `guest_gap` 38-50 ms por cada textura 8192² ASTC con 14 mips (85 MiB: buffer de staging nuevo + copia) que el juego carga al aparecer objetos, 150-270 ms con las RGBA8 de 268/358 MiB; (c) escena `LVL_Warmup`: 4 frames de 55-82 ms con 612 copias a texturas 128x1 RGBA16F (1794 encoders). (b) y (c) sin tocar.
- Unity 2021.3.15f1. 6 pipelines con el shader de error de Unity en el pase de ojo durante `LVL_Warmup` (frame ~1092), sin aviso `Unsupported` de Unity: causa sin identificar (¿variantes de la colección de warmup?).

## Into the Radius 2 (UE 5.5, Vulkan ES3.1 móvil; añadido 2026-11-10)
- Un submit por frame con 5 pases (ver bitácora del motor). MSAA 4x propio, adjuntos TRANSIENT con LOAD/STORE.
- `vk.tex_hist` en nivel (logs 27): 3165 texturas con >= 6 mips = 197 MiB (media 62 KiB, ~256 px), 298 con 1-2 mips = 108 MiB, RT 41 = 489 MiB; constante durante la sesión. Sin log de UE (shipping). Síntoma visual (texturas de poco detalle, trozos de mapa que faltan) sin causa demostrada.
- Draws indirectos con argumentos escritos por CPU (`vk.indirect_stats cpu_readable=todos`, hasta 5415 instancias); `draws_skipped=0`, `vk.err`=0, `pso failed=0`, sin shader de error.
- Descartado: shaders/pipelines (failed=0), Nanite/Lumen, memoria (/proc/meminfo 8 GiB, heap Vulkan 6 GiB), formatos, tamaño reportado de imágenes ASTC (`imageBytes` por bloques), trim del motor (no suelta contenido ni PSO).
- UE resuelve (sin que conste que use) funciones de ray tracing, descriptor buffer, mesh shader, device fault, calibrated timestamps: reciben stub.
- Binario revisado (2026-11-10, carpeta del PC ya conectada: `arm64-v8a/libUnreal.so` 152 MB, APK 74 MB, OBB main 4,1 GB + patch 2,2 GB): UE shipping sin cadenas de `UE_LOG` (no habrá log propio); la config (DeviceProfiles, TextureLODGroups, pool de streaming) va en los OBB, que superan el límite de traslado (400 MB/fichero) ⇒ perfil de dispositivo elegido por UE desconocido. `UECommandLine.txt` del APK solo trae `-project`.
- Descartado además: consultas de oclusión (ninguna lectura, no hay `vk.query_stats`), compute del juego para culling (los 72 dispatches/frame son `vk.indirect_patch` del motor), fetch de atributos por instancia con stride 0 (PrimitiveId stream de GPUScene: `dividedInstance`/stride en lower.cpp correcto), clear por quads en área parcial. Carga estable en nivel (memoria y texturas planas) ⇒ el síntoma es estado estacionario, no streaming en curso.
- Cache de PSO (logs 27): UE abre su cache empaquetada (GUID A5773C0A…; `OnShaderPipelineCacheOpened` carga `VulkanProgramBinaryCache/VulkanPSO_<guid>.5143.43051401` = nuestro blob de 6998 claves) y la precompila en la pantalla de carga: `vk.pso.census created` 7 → 2661 → 4829 → 7347 llamadas a vkCreateGraphicsPipelines, 6990 pipelines Metal distintas, todas servidas desde recetas (`compiled_in_session=0`, `failed=0`, `draws_skipped=0`); después 20 s sin crear nada. El usuario ve en el juego un contador de más de 8300: las ~950 entradas que no llegan al driver se quedan dentro de UE (sin log de UE no se sabe si son duplicados por clave Vulkan, p. ej. variantes con mapa de densidad que ocultamos, o entradas descartadas). El manifest declara `VulkanProgramService`..7 (compilación remota de PSO) pero UE no los consulta en nuestras sesiones. Sin `vkGetPipelineCacheData` en la sesión (UE no llegó a guardar).
- Niebla ausente (usuario, comparando con vídeos): el binario trae `r.Mobile.PixelFogQuality`, `r.Mobile.PixelFogDepthTest`, `r.Mobile.DisableVertexFog`, `r.LocalFogVolume.*`, `r.SupportLocalFogVolumes`, `r.SkyAtmosphere.*` ⇒ niebla por píxel en el subpase 1 (25 draws en i5) leyendo la depth 4x D24S8 como input attachment (sin demostrar cuál de los sistemas usa el juego). Vistas de imagen de la sesión: ~3035 texturas ASTC con exactamente 7 mips (cola no streameada de 64x64) y ~300 con 8-12.
- Extensiones que el UE del juego conoce, el driver del Quest 3 lista y no anunciamos: `VK_EXT_texture_compression_astc_hdr`, `VK_QCOM_render_pass_shader_resolve`, `VK_QCOM_multiview_per_view_viewports/render_areas`, `VK_KHR_fragment_shading_rate`, `VK_EXT_fragment_density_map(2)` y `VK_QCOM_fragment_density_map_offset` (ocultas a propósito), `VK_EXT_descriptor_indexing`, `VK_KHR_shader_atomic_int64`, `VK_EXT_shader_image_atomic_int64`, `VK_EXT_calibrated_timestamps`, `VK_KHR_maintenance5`, `VK_EXT_device_fault`, `VK_EXT_descriptor_buffer`, ray tracing y mesh shader (que el driver tenga la cadena no prueba que el Quest 3 la exponga).
- Logs 28 (build ac281e9): igual. Subpase 1 muestreado (1 frame): 9 draws, todos con blend, ninguno a pantalla completa ⇒ la niebla de pantalla completa no llega al motor en ese frame; el estudio tiene `CM.r.Mobile.DisableFullScreenMobileFog` ("auto disabled if we use vertex fog") y `EQualityType::Quest3/Low/Medium/High/Custom`. UE activó `VK_EXT_shader_viewport_index_layer` sin pases nuevos. Pipelines 7362 llamadas / 6989 distintos, 0 fallos. Argumentos indirectos consumidos por GPU (`ind{gpu=3628 cpu=0}`) y un encoder de cómputo propio del juego por frame (`vk.comp1`, ~20 dispatches): corrige lo de «no hay compute del juego».
- Texturas (logs 28): 2963 ASTC de 64 px con 7 mips, solo ~218 por encima; foot 4,7 GB. Memoria Vulkan que UE cree tener: 805M device-local + 384M lazily (ojo 3552x2258x2 por ojo, 4x) ⇒ ~1 GB de render targets frente a ~195M de texturas en streaming. HIPÓTESIS sin demostrar (el pool está en los OBB): el pool de streaming de UE (<= 1000 MB en la escalabilidad por defecto) se agota con los render targets a nuestra resolución (2,7x los píxeles de un Quest 3) y el streamer deja todo en la cola mínima. Prueba sin build: bajar la resolución de ITR2 al mínimo y mirar las texturas. Diagnóstico nuevo: `vk.memreq`, `vk.mem_seen`, `vk.tex_hist top{}`, `vk.draw_census`, `vk.comp_census`, `[ovrp] ovrp_GetSystemHeadsetType2 -> N`.
- Klepton no llama a `nativeSetConfigRulesVariables`, `nativeSetAffinityInfo`, `nativeSetSurfaceViewInfo`, `nativeInitHMDs` (libUnreal los exporta); `ovrp_GetGPUFrameTime` e `ovrp_IsPerfMetricsSupported` responden no soportado y el juego los consulta cada frame. Sin relación causal demostrada.
