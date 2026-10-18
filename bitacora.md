# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-15; lo cerrado queda en una línea con referencia).

## 2026-10-18 — VPKTAA nativo: máscara de UI/2D (VPKEngine paso-a12; sin cambios de código en esta app)
- La máscara vive entera en VPKEngine (ver su bitácora): los draws de UI/2D del pase de ojo (sin escritura de
  profundidad, tras lo opaco, con mezcla o sin test) no llevan jitter y marcan un R8 que el pase nativo usa para
  no mezclar historia ni afilar ahí (menús y textos nítidos). Va bajo el ajuste «VPKTAA nativo» y la variable
  `VPVK_TAA_UI_MASK` (def. 1 con el nativo; `=0` para comparar). `vpvk_taa_native_frame` no cambia: Klepton/
  `KleptonTAA.swift` no necesitan tocarse.
- Cómo comprobarlo en el visor: log `vk.taa uimask … draws_3d= draws_ui= masked_passes=` y `vk.taa native gpu_ms …
  uimask_eyes=2`; con `VPVK_TAA_UI_MASK=0` el texto debe volver a emborronarse.
- Pendiente: probar en visor (ver «Sin validar» de la bitácora de VPKEngine); si algún juego pinta la UI antes
  de lo opaco, ajustar la heurística o añadir un ajuste en la app.

## 2026-10-18 — VPKTAA nativo (VPKEngine paso-a8) + profundidad de ojo para PTW (sin probar en el visor)
- Ajuste «VPKTAA nativo» (`VQLaunch.swift` `vpktaaNative`, clave `vq.vpktaaNative`, def. off; toggle en
  `VQLauncher.swift` `VQVPKTAATuning`) → `vq_gfx_config.taa_native` (`vq_config.{h,c}`, dlsym
  `vpvk_config_set_taa_native`) + env `VPVK_TAA_NATIVE`. Conformidad: `VPVK_TAA_NATIVE=0` (`VQConformance.neutralGraphics`).
- `kl_vulkan.{h,c}`: `kl_vulkan_taa_native_on/encode` (dlsym débil `vpvk_taa_native_enabled/encode`), espejo
  `kl_vulkan_taa_native_frame`. `KleptonTAA.swift` `inlineResolve`: con nativo encendido el callback de
  `kl_vulkan_native_submit` llama `kl_vulkan_taa_native_encode` (`nativeFrame`: pose, tangentes sin jitter,
  crop, profundidad viva incl. MSAA, MV AppSW) en vez de `VPKTAACore.encode`; mismo anillo/publicación
  (`InlineOut.both` = vista unorm de las 2 capas). El jitter lo elige VPKEngine (Halton propio); el de
  `klvk_taa_next_jitter` solo da enabled/tamaño.
- Profundidad de ojo desacoplada: `klvk_taa_depth_on()` = (`KL_VPKTAA`=1 o `KL_CP_PTW`=1) y `KL_VPKTAA_DEPTH`≠0;
  API `kl_vulkan_eye_depth_get` sin cambios. `klvk_taa_wants_ms_depth` también con PTW o nativo.
  Profundidad MSAA: `klvk_ed_ms_resolve` → `vpvk_depth_resolve_encode` (muestra más cercana, depth32Float del
  motor, clave etapa*2+ojo) en cola de VPKEngine; se archiva como copia (samples 1, D32_SFLOAT). Solo sin inline
  o con PTW, y nunca con nativo (lee las muestras). `KL_DEPTH_MS_RESOLVE=0` lo apaga.
- Pendiente: probar en visor (log `vk.taa native gpu_ms`, objetivo < 1 ms/ojo), PTW + VPKTAA nativo a la vez con
  MSAA (PTW recibe entonces la profundidad MS sin resolver), compilar Swift (sin Mac).

## SIGUIENTE PASO OBLIGATORIO — emulación perfecta de Quest 3 (antes de cualquier otra tarea)
Objetivo: que ningún juego note diferencia con un Quest 3. Hacerlo en este orden:
1. **GPU idéntica a la Adreno 740 del Quest 3.** VPKEngine debe declarar exactamente lo que declara la Adreno
   (features, límites, extensiones, formatos) y cumplirlo. Herramienta: Vulkan CTS dentro de la app hasta 0 fallos en
   lo que la Adreno soporta (base: Pass 8 567 / Fail 898 / NotSupported 26 622, perfil rápida). Incluye MSAA
   transitorio `memoryless` en VPKEngine (como la memoria en chip del Adreno: sin RAM) y, hecho eso, volver a 4x en AC.
2. **Runtime del Quest completo (OVRPlugin/OpenXR).** OpenXR CTS pasando en el visor; tiempos (predicted display time),
   poses, capas y SpaceWarp como el sistema real.
3. **Android completo.** Todas las APIs de sistema/NDK que piden los juegos: usar el «Escáner de APIs» por juego
   (`VQApiScan.swift`) y cerrar lo que marque SIN IMPLEMENTAR (p. ej. AMediaDataSource_setGetAvailableSize/newUri).
4. **Compositor.** Ritmo de panel fijo (100 Hz) y reproyección con profundidad por píxel (no solo giro).
5. **Rendimiento.** Shaders precompilados sin tirones y paso a la traducción anticipada (AOT) de VPKEngine.
6. **OpenGL ES** (tras el paso 1, con el CTS de Vulkan a 0 fallos): capa GLES/EGL propia encima del Vulkan de VPKEngine
   (`libGLESv2/v3` + `libEGL` del guest → llamadas Vulkan; GLSL ES → SPIR-V → traductor existente), código 100 %
   propio, sin ANGLE. Así los juegos GLES heredan perfil Adreno, MSAA, VPKTAA, foveado y caché de shaders. Validar con
   el CTS de GLES (dEQP-GLES2/3/31, ya incluido en VK-GL-CTS). Quitar el bloqueo «solo Vulkan» del launcher al terminar.
7. **Limpieza de muletas de AC** (al cerrar pasos 2 y 4): ver entrada (20).
Todos estos pasos son OBLIGATORIOS y van en este orden.
REGLA del paso 1: toda capacidad nueva del perfil Adreno entra primero tras la puerta del CTS (`VPVK_EXTRA_EXTENSIONS`)
y solo se expone a los juegos tras una tanda del Vulkan CTS limpia en sus grupos (ej. vertex stores: dEQP-VK.robustness.*,
dEQP-VK.binding_model.*, dEQP-VK.memory_model.*). Motivo: vertex stores expuesto sin CTS → cuelgue de GPU en el warmup de AC (2026-10-16).
Plan acordado: implementar TODOS los pasos 1-7 tras la puerta del CTS (los juegos no lo ven; el CTS sí), y al final
pasadas completas del Vulkan/OpenXR/GLES CTS en el visor → arreglar fallos → abrir la puerta por grupos a los juegos.

### Estado del paso 1 (2026-10-16) — lo YA tocado
- VPKEngine en main: `ced6226` memoria LAZILY_ALLOCATED sin RAM; `ad18998` atómicos R32; `5099bb3` driverVersion
  512.744.0, sin BC, vertexPipelineStoresAndAtomics=1 FIJO; `1495086` sin formatos BC, ASTC no cuadrados, R4G4B4A4,
  sin EXT_metal_objects; `1e6d988` shaderInt16, shaderDrawParameters (buffer 29 `VpDrawParams`), maintenance2/3,
  storage_buffer_storage_class, driver_properties Qualcomm, structs Vulkan11/12/13, extensiones 1.2/1.3 visibles;
  `f836aeb` host_query_reset; `1e483b2` baseVertex no indexado, spec int16, ext16; `ce43429` README crédito.
- VisionQuestPro: `288e73c` AC a MSAA 4x (`KL_VK_MAX_SAMPLES` def. 0); Klepton `KL_VK_VTX_SSBO`=1 (build 18).
- Build 288e73c probada (log 02:37 AC): cuelgue de GPU (`device_lost`, ningún command buffer en 20 s) en
  `ShaderWarmupManager.WarmUpShaders` tras 1700 pipelines. Sospecha: SSBO en vértice sin robustez real (OOB) ahora que
  Unity incluye esas variantes; 2ª: VpDrawParams buffer 29. Agente trabajando: robustBufferAccess real en todas las
  etapas + diagnóstico de cuelgue (pipelines del command buffer colgado). Progreso: `/home/claude/progreso/agente-cuelgue-gpu.md`
  (local de la sesión) y bitácora de VPKEngine.

- Arreglo del cuelgue en VPKEngine main: `d0df859` robustBufferAccess real (EmitOptions::robustBuffers, `bs<slot>`
  BufferSize, lecturas OOB=0, escrituras/atómicos OOB descartados, traductor v9 → caché retraducida), `5934065`
  informe `gpu.err what=hang_report` + líneas `gpu.hang` (pipelines por command buffer en vuelo), `e5d9dfd` arreglos de
  revisión. Build lanzada sobre ese VPKEngine; resultado pendiente.

### 2026-10-18 — parches de AC retirados, carátulas — build lanzada
- `kl_is_ac_nexus()` (kl_jni_services.c) devuelve 0 salvo `KL_AC_NEXUS_PATCHES=1`: todos los parches solo-AC apagados
  (2 etapas, token error, SpaceWarp spin, pose fresca, overlay texl/orden, burst AC, MSAA/ojo). Riesgo: con 3 etapas AC
  llegó a OOM (8,1 GB) en su día; vigilar memoria. JNI LoadingScreen de nexusvr se queda (emulación de clase).
- `KL_VK_VTX_SSBO` def. 0 y VPKEngine `vertexPipelineStoresAndAtomics` tras `VPVK_EXTRA_EXTENSIONS` otra vez (cuelgue del
  warmup). El rosa de AC NUNCA se fue: siguió con vertexStores=1
  en todas las builds (16-17/10), así que NO es la causa del rosa (ni apagarlo lo empeora). Causa del rosa sin
  identificar: ver punto «Siguiente para el rosa» del TODO.
- Carátulas: `app/Sources/VQCover.swift` (imagen de la carpeta: nombre cover/banner/poster o la mayor; si no, arte del
  APK res/*: banner > ic_launcher/app_icon > round/foreground, mayor tamaño = mayor densidad → `Cache/<j>/cover-apk.<ext>`;
  si no, degradado con nombre). `VQCoverView` 16:9 arriba de `VQGameCard` (VQLauncher.swift). Sin probar en el visor.

### TODO domingo (en orden)
1. Cerrar el cuelgue de AC. Build 709566a (robustez real) NO lo arregla (log 03:08): `gpu.hang cb=vpgpu.f662
   age_ms=20010 draws=4630 dispatches=0 pipelines=1576`, un solo encoder, todos los VS `ssbo_r robust` (pipelines de la
   caché de recetas, etiqueta `vk.prewarm` de recipes.cpp:313) = los draws de `ShaderVariantCollection.WarmUp` de Unity.
   No es el reloj (clock_getres solo afecta a UbiServices; el bloqueo es la GPU). Siguiente: (a) loguear por draw del CB
   colgado vertexCount/instanceCount/indexCount/indirect, tamaño del render target y muestras; (b) partir el CB en
   trozos (p. ej. 256 draws) con fence propio para que el cuelgue señale el pipeline exacto y descartar timeout de
   Metal por CB enorme frente a cuelgue real; (c) volcar el MSL del VS culpable a QUEST/Registros; (d) A/B: ajuste en la
   app «Escritura en buffers desde vértice» (off = como build 17, que sí pasaba el warmup).
2. Poner tras la puerta del CTS lo expuesto sin validar: vertex stores (VPKEngine entry.cpp y Klepton `KL_VK_VTX_SSBO`),
   shaderInt16, shaderDrawParameters, extensiones 1.2/1.3 recién visibles. El rosa de AC NO venía de vertex stores: con
   vertexStores=1 (log 02:37) siguen `[magenta] fragment module … (1,0,1,1)` y el rosa visible. Hipótesis descartada.
   Siguiente para el rosa: comparar TODO lo que Unity consulta (features, límites, formatos con sus flags, extensiones)
   contra el perfil Adreno real y loguear qué subshader/variante cae al error shader (Unity no lo dice en Vulkan:
   identificar el material por el hash del pipeline y el render pass); revisar también `[prov] layer 3 … no write seen`.
3. Paso 1 restante: samplerYcbcrConversion, variablePointers, 16/8-bit storage, apiVersion 1.3 (maintenance4, BDA,
   memory model, imageless framebuffer, extended_dynamic_state, private data, inline uniform blocks, subgroup size
   control), B4G4R4A4 (ABGR4 + swizzle), fallos conocidos del CTS (texel buffers sint/snorm/16-bit, fragment output
   int/snorm, blits 1D). Valores [B]/[C] de `docs/perfil-adreno740.md` pendientes de un vulkaninfo real de Quest 3.
4. Pasos 2-7 en orden, todo tras la puerta.
5. Pasadas CTS completas (Vulkan, OpenXR, GLES) en el visor → arreglar → abrir puerta por grupos.
6. Launcher: carátula de cada juego en el selector, como la biblioteca de Quest. Sin servidores de Meta, por orden:
   (a) imagen que el usuario ponga en la carpeta del juego (`cover.jpg/png`, también `*.jpg/png` suelto de releases
   VRP); (b) arte del APK: `application android:icon/banner` del AndroidManifest (AXML, `vq_apk.c`) resuelto por
   `resources.arsc` a la mayor densidad (mipmap-xxxhdpi, adaptive icon foreground+background, WebP/PNG); (c) fondo
   generado con nombre. Caché en `QUEST/Cache/<j>/cover.png`; formato horizontal tipo tienda (recorte/relleno con
   desenfoque si es icono cuadrado).

## DESPUÉS de los pasos obligatorios — SSAO propio en VPKEngine
- Pase de oclusión ambiental en pantalla propio, a partir del buffer de profundidad del juego (el mismo que captura
  VPKTAA), aplicado a la imagen antes de enviarla al compositor. Genérico para todos los juegos.
- **Toggle en Ajustes** (por juego, desactivado por defecto): un juego que ya trae su propio SSAO/AO no debe recibirlo
  dos veces. Opcional: detectar AO propio del juego (p. ej. renderer feature SSAO de URP / pases AO conocidos de Unreal)
  y avisar en la tarjeta del juego.
- Objetivo: el menor coste en ms posible (VR estéreo, 2 ojos a alta resolución). Pautas: media (o cuarto de)
  resolución, profundidad reconstruida a lineal una sola vez y compartida con VPKTAA, pocas muestras + ruido azul
  y acumulación temporal reutilizando la historia/vectores de VPKTAA, desenfoque bilateral separable por profundidad,
  upsample guiado por profundidad, ambos ojos en un único dispatch (array de capas), solo dentro de la zona nítida del
  foveado (periferia barata o sin AO). Medir ms GPU por pase en el log.
- Origen: Attack on Titan VR: Unbreakable (URP 2021.3) no trae el shader SSAO de URP en la build, así que no se puede
  activar editando sus assets.
Lo que hace falta del usuario: logs de juegos variados (cada uno destapa huecos distintos); tandas periódicas del
Vulkan CTS desde la app; el informe de capacidades (vulkaninfo) de un Quest 3 real para comparar con datos exactos
(vulkan.gpuinfo.org bloquea la descarga automática).

## EMPEZAR AQUÍ — 2026-10-16 (20): perfil Adreno bloque 2 + AC a 4x — SIN BUILD (pendiente visto bueno)
- VPKEngine (push): `1495086` sin BC, ASTC no cuadrados (`FormatInfo::blockHeight`), R4G4B4A4=ABGR4, sin
  EXT_metal_objects visible; `1e6d988` shaderInt16 (16 bits en registros de 32), shaderDrawParameters (buffer 29
  `VpDrawParams`), maintenance2/3, storage_buffer_storage_class, driver_properties QUALCOMM, Vulkan11/12/13 structs,
  extensiones 1.2/1.3 fuera de VPVK_EXTRA_EXTENSIONS (menos EXT_multi_draw); `kTranslatorVersion`=8 (retraduce
  recetas); `f836aeb` host_query_reset; `1e483b2` arreglos de revisión (baseVertex no indexado, spec int16, ext16).
- NO declarado (el traductor no lo cumple): samplerYcbcrConversion, variablePointers, 16/8-bit storage, apiVersion 1.3
  (faltan maintenance4, BDA, memory model…), B4G4R4A4. MSL nuevo sin compilar en Apple: vigilar errores de pipeline.
- VisionQuestPro: AC vuelve a MSAA 4x (`KL_VK_MAX_SAMPLES` def. 0 para todos); ojo 3840 de AC solo si se pide 2x
  explícito (kl_ovrp.c). Vigilar memoria en AC.
- Parches solo-AC (`kl_is_ac_nexus()`) a revisar al cerrar pasos 2 y 4: probar AC con cada uno apagado y quitar los
  que ya no hagan falta. Muletas de tiempos/compositor: `KL_NEXUS_FRAME_BUDGET`, `KL_GUEST_FRESH_POSE`, 2 etapas
  (`kl_ovrp_stage_count`), `KL_OVERLAY_TEXL`=0, `KL_OVERLAY_SUBMIT_ORDER`. Se quedan (no son muletas): token offline
  `KL_PLAT_TOKEN_ERROR`, burst Wwise. Arreglos globales (clock_getres, publicar tras GPU…) son emulación correcta.

## 2026-10-15 (19): paso 1 (GPU = Adreno 740) iniciado
- VPKEngine, 4 commits locales (pendientes de push): `ced6226` memoria LAZILY_ALLOCATED sin RAM (tipo 3 sin host,
  `DeviceMemory::lazy`, commitment 0; la ruta MSAA memoryless/MultisampleResolve ya existía en metal2); `ad18998`
  STORAGE_IMAGE_ATOMIC/STORAGE_TEXEL_BUFFER_ATOMIC en R32_UINT/SINT (`MTLTextureUsageShaderAtomic`, sin probar en
  visionOS); `e626ce4` bitácora VPKEngine; `5099bb3` driverVersion 512.744.0 (bit 31), `textureCompressionBC=FALSE`,
  `vertexPipelineStoresAndAtomics=TRUE` fijo (entry.cpp:324, ya sin la puerta del CTS).
- Revisión adversarial: sin bloqueantes. Pendiente: test de store/atomic en VS; BC1/3/7 siguen anunciados en
  formatFeatures (physical_device.cpp:208-241) → quitar SAMPLED_IMAGE para forzar ruta ETC2/ASTC como el Adreno
  (rompe test vk_tests.cpp:405, ajustarlo).
- Fallos CTS de la lista: la mayoría ya resueltos en (8)/(12) según arneses en Linux; confirmar con CTS nuevo en el
  visor. Queda B4G4R4A4_UNORM_PACK16 (obligatorio; Metal solo ABGR4 → swizzle).
- Perfil Adreno comparado: `docs/perfil-adreno740.md` (copiado de progreso). Siguientes P0: añadir lo que obliga
  VP_ANDROID_baseline_2022 (shaderInt16, shaderDrawParameters, KHR_maintenance2/3, storage_buffer_storage_class,
  samplerYcbcrConversion, variablePointers), quitar `EXT_metal_objects` del listado al guest, apiVersion 1.3 con
  structs VULKAN_1_1/1_2/1_3_FEATURES, ASTC no cuadrado, centralizar en VPKEngine y dejar `klvk_patch_features` no-op.
  Falta vulkaninfo real de un Quest 3 para cerrar valores [B]/[C].
- Volver a MSAA 4x en AC (`KL_VK_MAX_SAMPLES`) ahora que la MSAA transitoria no gasta RAM.

## 2026-10-18: paso 4b (agente 9) — ASW 2.0: extrapolación de objetos con MV de SpaceWarp sobre PTW (sin build)
- `klepton/runtime/gfx/kl_reproject_ptw.{c,h}`: `kl_ptw_uniforms` (sigue 192 B) estrena `asw[4]` (escala MV x/y con signo,
  fracción de frame, 1 = MV ligados; offset 112) y `mv_size[4]` (w, h, tope por eje, slice; offset 128). Funciones C
  `kl_asw_fraction` ((edad−lead)/intervalo, 0..max), `kl_asw_displace` (mv*escala*fracción, y invertida si origen arriba-izq,
  tope; NaN/inf/|mv|>4 → 0) y `kl_ptw_fill_asw`. MSL `kl_ptw_v`: texturas de vértice 2/3 = MV por ojo
  (`texture2d_array`), MV leído en el texel de menor profundidad de la vecindad 3x3 (el que da el z del vértice) y el vértice
  se desplaza en espacio de rejilla (`cm = c + dc`), mismo z y mismo uv de color; estiramiento de malla = relleno de desoclusión.
- `KleptonCompositor.swift`: puerta `KL_CP_ASW=1` (def. 0, exige `KL_CP_PTW=1`); `KL_CP_ASW_OFF=1` (lo pone
  `app/Sources/VQLaunch.swift` desde «ASW desactivado», def. true) la anula. Fracción: `aswFraction` = (edad de `submit_ns` −
  lead)/intervalo; lead = EMA de la edad de un frame fresco al primer uso (acotada a 1 intervalo), intervalo = EMA del hueco
  de `submit_ns` por serial → frame a tiempo = 0, retenido ≈ 1, tope `KL_CP_ASW_MAX` (1.5). MV: `aswMotion` (mismo filtro que
  `KleptonTAA.motionInput`: flags STORAGE/MV, `kl_vulkan_spacewarp_mtl`, vista 2D-array). Textura 1x1 ficticia en slots 2/3
  cuando una vista no tiene MV. Otras: `KL_CP_ASW_CAP` (0.08 del cuadro/eje), `KL_CP_ASW_SCALE` (0.5), `KL_CP_ASW_SIGN`,
  `KL_CP_ASW_YFLIP`. Log `[cp] ASW …` cada 5 s y motivo de inactividad al cambiar.
- Validado en Linux: `make ptw` (t_ptw.c amplía: MV conocido → desplazamiento en uv y en metros, flip Y, tope, rechazo
  NaN/|mv|>4, signo, fracción, offsets de uniformes). MSL y Swift revisados a mano, SIN compilar.
- Capas de proyección (Steam Link, `encodeProjLayers`): NO reciben PTW: son texturas GL de `kl_glfb`, sin profundidad ni MV
  (la profundidad de ojo es solo de kl_vulkan); comparten pipeline de rotación pura y siguen igual. Ruta GL: sin cambios
  (GLES nuevo en otro agente); ningún título GLES usa AppSW.
- Sin validar: signo/escala reales de los MV de AC/BONELAB (VPKTAA los vota en `KleptonTAA`, aquí es fijo por env: si el
  objeto va al revés, `KL_CP_ASW_SIGN=-1`); la fracción asume que `submit_ns` marca el inicio del frame del juego;
  el desplazamiento del texel de dilatación se aplica en uv de profundidad (≈ uv de recorte); sin medir coste GPU.
- Pendiente: compartir el signo votado de VPKTAA con ASW; probar en visor `KL_CP_PTW=1 KL_VPKTAA=1 KL_CP_ASW=1`.

## 2026-10-18: paso 4 (compositor) — PTW con profundidad por píxel + ritmo fijo de panel (sin build)
- `runtime/gfx/kl_reproject_ptw.{c,h}` (nuevo, en Makefile RUNTIME_SHIP): reproyección posicional tipo
  Oculus PTW/ASW 2.0. Malla densa (KL_CP_PTW_GRID, def. 96x96/ojo) sobre la imagen; cada vértice se lleva
  a la distancia que da la profundidad del juego (`kl_vulkan_eye_depth_get` con el serial del registro de
  frame, monomuestra) y se mira desde el ojo ACTUAL con el delta rígido completo (rotación + traslación,
  offset de ojo incluido). Desoclusión: malla continua (estira el fondo) + dilatación al más cercano 3x3
  (`KL_CP_PTW_DILATE`, def. 1) para que el estirado caiga en el fondo. Lejos/infinito → `KL_CP_PTW_FAR`
  (def. 200 m). El pase escribe la profundidad real al depth del drawable → CompositorServices recibe
  geometría por píxel (análogo XR_KHR_composition_layer_depth). Fragment = `kl_reproject_f` (mismo sRGB,
  fade, chroma); vértice `kl_ptw_v` (MSL concatenado a `kl_reproject_msl()`), buffer(2) `kl_ptw_uniforms`
  (192 B), texturas de vértice 0/1 = profundidad por ojo.
- Puerta `KL_CP_PTW=1` (def. 0). Necesita `KL_VPKTAA=1` (la profundidad del ojo solo se rastrea con él) y
  guest Vulkan. Sin profundidad válida cae por frame al quad de solo giro (log `[cp] PTW off this frame: …`).
  Tras el dibujo PTW, overlays sin estado propio usan compare-always (la profundidad ya no es 500 m).
- Medición: `[cp] PTW gpu: composite X ms mean with PTW (n), Y ms rotation-only (m); P passes / F fallbacks`
  cada 5 s (gpuEnd−gpuStart del command buffer del compositor).
- Ritmo: tras el latch de `primeDisplay`, la frecuencia se encaja SIEMPRE en una frecuencia real de panel
  (`panelHz` 120/100/96/90, ±6 %, por defecto; `KL_CP_PANEL_SNAP=0` lo quita) o se fuerza con
  `KL_CP_PANEL_HZ=100`. Justificación: el panel solo funciona a esas frecuencias; antes una medida 1–6 %
  fuera (frames retenidos/ruido) quedaba como frecuencia permanente del juego (deriva). 60 Hz no se toca.
- Validado en Linux: `make ptw` (tests/t_ptw.c: linealización rev/std finita/infinita, model-view
  identidad, paralaje −dx/z, tangentes, layout 192 B). MSL y Swift revisados a mano, SIN compilar (no hay Mac).
- Pendiente: probar en visor con KL_CP_PTW=1 KL_VPKTAA=1 (AC/BONELAB); profundidad MSAA no soportada
  (cae a giro); PTW no aplica a capas de proyección por capa (Steam Link) ni ruta GL; vectores de
  movimiento SpaceWarp (ASW extrapolación de objetos) no usados aún; interfaz con agente 1: nada nuevo de
  kl_ovrp (usa depth_near/far, stage, serial existentes).

## 2026-10-15 (18): crash AMediaDataSource, panel 50→100 Hz, vertex SSBO (rosa), token offline — build 18
- Resultado de (17) (build 505b8dc, AC 21:24–21:32, primera vez que el juego arranca): 0 `Scheduler::dispatch` (reloj
  OK); `[nexus] SpaceWarp spin 18.89 ms -> 0`; memoria 3,9 GB; rosa sigue (cover quad + menús); fin por
  `[klepton] fatal: called unresolved import 'AMediaDataSource_new'` (VideoPlayer con fuente propia).
- `kl_mediandk.c`: AMediaDataSource_* + `AMediaExtractor_setDataSourceCustom` (vuelca la fuente a
  TMPDIR/klepton-media-ds-*.mp4 y abre con kl_avdemux; `spooled` → unlink al borrar; close() del guest al borrar).
- Display: `display rate SETTLED at 50.028 Hz` en panel de 100 Hz (frames de arranque mostrados 2 veces) → al juego
  se le dijo 50 Hz. `KleptonCompositor.swift`: `unheldPanelPeriod` en el latch de arranque (<55 Hz y x2/x3 de
  90/96/100/120 → canónico; `KL_CP_UNHOLD_RATE`), y re-latch hacia arriba en `cadenceSummary` solo desde <85 Hz,
  ≥25 % de huecos cortos (`cadenceShort`) y 6 ventanas (~3 s; la ventana es de 0,5 s).
- Rosa: VPKEngine reporta `vertexPipelineStoresAndAtomics=0` (atado a VPVK_EXTRA_EXTENSIONS, entry.cpp:325); Adreno y
  MoltenVK reportan 1 → Unity descarta subshaders con StructuredBuffer en vértice → shader de error.
  `kl_vulkan.c` `klvk_patch_features` (Features y Features2): 1 para todos (`KL_VK_VTX_SSBO`, def. 1);
  `KL_VK_IMG_NOFMT=1` (def. 0) reporta también storage image sin formato. Log: `features to the guest: …`.
  Escrituras de vértice en el traductor sin validar (CTS) — vigilar `vk.err`.
- UbiServices offline ~50 s (reintentos DNS de login con el token Oculus): `kl_ovrplat.c` `ovr_User_GetAccessToken`
  → completion de ERROR para AC (`KL_PLAT_TOKEN_ERROR`, latch solo con paquete conocido).
- Log `[cp] thermal state: nominal|fair|SERIOUS|CRITICAL` (al cambiar y cada 30 s), KleptonCompositor.swift.
- Pendiente: MSAA memoryless en VPKEngine (MSAA transitorio sin RAM, como en Adreno) y volver a 4x en AC; paneles AC
  girados 180° (`KL_OVERLAY_FLIP_X/Y`); idioma voces AC (menú del juego).

## 2026-10-15 (16)-(17): etapa publicada tras la GPU (temblor al girar), clock_getres Linux (AC) — build 17
- Resultado de (15) (Registros 11/12, build 8f080bf, AoT 20:21 a 100 Hz): censo `answered head vs pinned pose worst
  0,06–0,07°` → las poses YA son una por frame; el temblor seguía → la causa NO eran las poses. Compositor sano
  (cadencia 10,00 ms). Registros `fin-<juego>` funcionan.
- CAUSA del temblor (también visto en Arkham Shadow, «flicker when I
  look around»): `klovrp_end_frame_impl` publicaba `last_complete` ANTES de que `kl_vulkan_frame_done` esperara a
  la GPU (~12 ms en AoT = un periodo entero) → el compositor reproyectaba con la pose del frame N una textura que
  aún tenía (parcial o totalmente) el frame N-3. Arreglo `kl_ovrp.c`: `t_defer_complete` (EndFrame4, guest Vulkan,
  `KL_OVRP_PUBLISH_AFTER_GPU`, def. 1) → `deferred_complete` + `r[stage].complete=0`; `klovrp_publish_deferred()` tras
  `kl_vulkan_frame_done` (y al final de EndFrame4). Escaneos de VPKTAA en EndFrame4 aceptan la etapa diferida. Log
  cada 600: `[ovrp] frame end waited for the GPU X ms mean … stage published AFTER the wait`.
- AC ralentizaciones: 4464× `Scheduler::dispatch (1255 ms)` en 30 s. Causa: UbiServices usa
  1e9/clock_getres(MONOTONIC); Darwin da 1000 ns → reloj 1000x rápido. `klb_clock_getres` (kl_libc.c) responde 1 ns
  para ids 0/1/4/7 a TODOS los juegos (`KL_CLOCK_RES_LINUX`, def. 1).
- AC magenta: `[prov] layer 3 source …: no write seen` (el quad de portada se copia de una textura que nada escribió).
  Probado y DESCARTADO limpiar a cero (la tabla de procedencia es un anillo con pérdidas y no cubre resolve/compute/
  dynamic rendering → borraría contenido legítimo). Queda solo el diagnóstico `[prov]`.
- (17) Arreglos de AC — gate AC = `kl_is_ac_nexus()` (kl_jni_services.c, por
  paquete com.Ubisoft.ACNexusVR; `klj_guest_package` ahora con mutex y sin fijar el fallback antes de tener manifest):
  - AC: `KL_NEXUS_FRAME_BUDGET` (kl_mprobe.c, desde `kl_driver_frame`: pone a 0 `_nexusFrameTicksSpaceWarp` de
    NexusUtils.PhaseSyncHack vía il2cpp; valores 150000–260000), `KL_GUEST_FRESH_POSE` (kl_app.c), `KL_VK_MAX_SAMPLES`=2
    (límites + sampleCounts por formato), 2 etapas de ojo (`kl_ovrp_stage_count`), tope de ojo 3840 si MSAA≤2 y 2 etapas,
    `KL_OVERLAY_TEXL`=0, `KL_OVERLAY_SUBMIT_ORDER` (Swift; escribe profundidad solo si writeDepth).
  - Todos: `KL_OVRP_ENQ_OWNS_OVERLAYS` (EndFrame4 vacío no borra overlays encolados; contador solo con capas no-ojo),
    AAudio burst 256 (Wwise 1024: AC o libAkSoundEngine.so; `KL_AAUDIO_WWISE_BURST`), `KL_FS_REL_SDCARD` (rutas
    relativas "sdcard/…" como absolutas mientras el guest no haga chdir).
  - Ya equivalentes: idiomas (.lang listados; la voz en español se elige en el menú del juego), SpaceWarp, texptr,
    soft null-class, injectActivityJava, red offline, vídeo de carga.
  - Fork, no portado: `klovrp_enq_age` (ruta EndFrame legada), usuarios offline por app id de otras tiendas,
    `KL_OVERLAY_FLIP_X/Y` (los paneles de AC podrían salir girados 180°; comprobar).

## 2026-10-18 (agente 14): Android completo B (libandroid, libmediandk, AAudio/OpenSL, JNI de clases Android) — SIN PROBAR EN DISPOSITIVO
- Solo comprobado con `clang -fsyntax-only` y arneses Linux con stubs de CoreVideo/VideoToolbox/CommonCrypto (no hay build Darwin).
- `media/kl_mediandk.c`: AMediaFormat tipado (64 claves; Int64/Float/Double/Size/Buffer/Rect, copy, toString, +71 AMEDIAFORMAT_KEY_*),
  errores AMEDIA_*, DataSource por URI (esquemas remotos -> ERROR_IO), Extractor completo, Codec (async, nombre, formatos,
  parámetros, salida a surface; codificación y DRM no soportados a propósito), AImageReader/AImage (planos solo YUV_420_888 sobre NV12),
  AHardwareBuffer (allocate/lock/lockPlanes/describe/unlock/getId), ATrace.
- `media/kl_aaudio.c`: requestPause/Flush, release, getters de usage/contentType/capturePolicy/privacy/spatialized/hardware*,
  convertStreamStateToText, getTimestamp validado.
- `media/kl_opensl.c`: todos los SL_IID_*, MuteSolo, EffectSend, OutputMix, EngineCapabilities, Query* del motor;
  Create* rechaza con FEATURE_UNSUPPORTED una interfaz requerida no servida. Efectos/3D/MIDI no soportados.
- `guest/kl_android.c` (+`.h`, nuevo; `kl_ndk_lookup` cae a `kl_android_lookup`): AConfiguration de Quest 3, AChoreographer sobre el looper
  (hilo de pulso a kl_ovrp_display_frequency), AThermal real (`media/kl_thermal.m`: NSProcessInfo.thermalState), APerformanceHint,
  AStorageManager/AObb (montaje -> ERROR_COULD_NOT_MOUNT), ASharedMemory, AInput*/AKeyEvent/AMotionEvent, ANativeActivity.
  `kl_ndk.c`: ANativeWindow (transform, dataspace, frame rate), ASensor accessors. Makefile: kl_android.c, kl_thermal.m, kl_jni_sysinfo.c.
- JNI: nuevo `jni/kl_jni_sysinfo.c` (tabla `klj_bind_sysinfo`): SystemClock, Process (myPid/myUid/is64Bit...), PowerManager
  (isInteractive, isPowerSaveMode, getCurrentThermalStatus real vía `kl_android_thermal_status`, newWakeLock/WakeLock con contador),
  BatteryManager.getLongProperty, Build.getSerial, Debug, Locale (display names, ISO3, toString). `kl_jni.c`: campos Build
  (BOARD, HOST, SERIAL, CPU_ABI, SOC_*), Build.VERSION (SDK, SECURITY_PATCH, PREVIEW_SDK_INT), VERSION_CODES, constantes de PowerManager/
  BatteryManager/Process. Con `KL_QUEST3=1` (o target batman): SDK_INT=32 (`klj_field_ival`, también `kl_jni_build_int`), RELEASE=12,
  SDK="32", fingerprint ":12/". Por defecto sigue siendo Quest 2 / SDK 29 (decisión previa; ver comentarios de Build en `kl_jni.c`).
  ANDROID_ID ya era fijo sintético (`kl_jni_net.c`).
- Pendiente: sensores del Quest (lista ASensor vacía), AObb montaje real, AImage otros formatos, `ovrp` aún dice Quest 2 si no hay KL_QUEST3,
  sysinfo sin probar con un juego (Unity/Unreal); comprobar que gen_xcodeproj/project.yml de visionos lista los ficheros nuevos.

## 2026-10-15 (15): una sola pose por frame para TODAS las lecturas; registros al acabar el juego — SIN PROBAR
- Resultado de (14) (Registros 9/10, builds e56af9c/c29ae8e): PEOR al girar («los ojos ni se sincronizan»). El log lo
  mide: `frame record -> last Update(Render)` worst 2–7°, mean 0,7–1,2° girando, 22° en tirones = lo que el registro se
  separó de la pose dibujada → Unity dibuja con la PRIMERA lectura del frame (el display provider lee los ojos justo tras
  el primer Update), NO con la segunda. `KL_OVRP_POSE_LAST` def. 0 (variante «última» solo para A/B por env); ajuste
  «Pose del frame» eliminado de la app. HUD AoT: salió `overlays enqueued on thread X, EndFrame4 on Y` (arreglo activo;
  sin veredicto del usuario). AC: `[magenta] fragment module … returns the constant (1,0,1,1)` ×2 y 4 pipelines
  (renderPass 0x13d764e40, 2 samples) → Unity SÍ sustituye un shader por el de error; falta saber cuál/por qué.
- Cambio (15) `kl_ovrp.c`: el segundo Update(Render,F) ya no refresca `g_step_head` con una lectura nueva: reusa la
  pose fijada de F. Las lecturas Render sin frame fijado/vivo (frameIndex 0 del C#, otros hilos) responden la del
  frame más nuevo fijado (`g_fring_newest`) en vez de `g_step_head` fresco. BeginFrame4(F) sin Update previo fija F.
  Censo cada 300 frames: `[ovrp] Render reads over 300 frames: … answered head vs pinned pose worst X deg`
  (DEBE ser 0,00; si no, bug del latch). Si el temblor sigue con 0,00, las poses NO son la causa: mirar compositor
  (kl_reproject / anchor / cadencia) — la matemática de kl_reproject_build se revisó en papel y es correcta.
- Registros: `VQLog.saveSessionEnd` al cerrar el espacio inmersivo (`Lifecycle.immersiveEnded`): carpeta
  `QUEST/Registros/<fecha>-fin-<juego>`, en segundo plano (espera ≤20 s solo antes de `_exit`). La copia al reabrir
  (`savePreviousSessionToQuest`) queda para crashes: 12 reintentos ×5 s al arrancar, 1 intento al lanzar, se detiene
  si ya se lanzó un juego; motivo en app.txt si falla.
- AC magenta, dato nuevo (log 12:42): la capa 3 (quad de portada, head-locked 1024²) ya era 100 % magenta a las
  12:42:06 y el PRIMER módulo de error de Unity se creó a las 12:42:19 → ese magenta NO viene del shader de error.
  Añadido `[prov]` (kl_vulkan.c, `klvk_prov_*`): procedencia de la imagen fuente de cada capa (último escritor:
  render pass + pipelines + fs MAGENTA/no, upload, clear [MAGENTA si (1,0,1)], blit, copia). Se imprime en el
  `written by the guest` de cada capa. Siguiente: leer `[prov] layer N source …` en el log de AC.
- Pendiente: shader magenta de AC (qué shader: registrar nombre/keywords del material? Unity no lo dice en Vulkan; opción:
  volcar las features que Unity consulta — `vkGetPhysicalDeviceFeatures/Properties` — y comparar con Adreno 740);
  saturación al iniciar AC; voces «Voice Starvation»; idioma AC (`Language set: English(US)` con es_ES).

## 2026-10-15 (14) — cerrado en (15)
- Pose «última» (revertida), overlays por hilo (`g_enq_tid`, `g_enq_idle_endframes`), detector magenta
  (`klvk_spirv_const_magenta_fs`), `KL_VK_FRAME_ASYNC` def. 0 (AC: `native_stuck` + aborto en VPKEngine).

## 2026-10-15 (13) — cerrado en (14)
- Fin de frame ASÍNCRONO (`KL_VK_FRAME_ASYNC`, def. 1; `frame end: ASYNC` en el log, funcionando); lectura del hilo de
  render (`KL_OVRP_RENDER_READ`, nunca dispara en AoT); VPKEngine `628c3b4` `VPVK_REPORT_GEOM=1` (no arregla AC).

## 2026-10-15 (9)–(12) — resumen (detalle en git)
- (12) anillo de poses por frameIndex `g_fring` (`KL_OVRP_FRAME_RING`); capturas de capas + fuentes (`vk_layerNN*.png`).
- (11) SpaceWarp siempre servido (`KL_VPKTAA_ASW`); QoS RT hilos guest; audio adaptativo; ajuste «Idioma del juego»
  (`vq.gameLang`); sonda botón PS (visionOS no expone el PS); shims libc varios.
- (10) x18: veneers `adr/ldr literal/add-sub sp` con x18, pools GCC como datos (`klx_lit_map`, converter kld-3);
  `ovrp_GetLayerTextureSpaceWarp` 7 args (depth en x5); VPKTAA off desde el arranque en juegos MSAA.
- (9) VPKEngine 935aa1b (CTS); mutex con tumbas; capa de conformidad OpenXR; AAsset con rutas absolutas (Wwise);
  shims libc *at; futex reescrito con `kl_futex_dump` en `[snap]`.
- 2026-10-14 (6): `.restorationBehavior(.disabled)` en la ventana principal (no arrancaba al restaurar escena).
  NO tocar el toolbar del launcher (ShareLink como en a1df14a).
- 2026-10-14 (4)/(7)/(8) y «Robustez del CTS» (device lost, vigilantes por caso, fences precisas, cola de retiro,
  `VQCompilerProbe`, registros a QUEST/Registros): ver `git log -p bitacora.md`.

## Conformidad (Vulkan CTS / OpenXR CTS como guest dentro de la app)
- CI `conformance.yml` (Linux, manual o `[conformance]`): VK-GL-CTS `vulkan-cts-1.4.6.2`, `DEQP_ANDROID_EXE=ON`, parche
  `conformance/vulkan/patch_deqp.py` (exporta `vq_exe_main`), listas `conformance/vulkan/caselists.py` (perfil
  `rapida` ~36k casos, CRC32 estable, tandas de 400; `completa` con `VQ_CTS_FULL=1`) → release `conformance`
  (`VulkanCTS.apk`, meta-data `com.visionquestpro.guest=exe`). OpenXR CTS 1.1.58 → release `conformance-openxr`.
- Klepton `KL_GUEST_EXE` (`runtime/guest/kl_exe.c`): main() en hilo guest de 64 MB por línea de `KL_EXE_ARGS_FILE`
  (TAB); `KL_EXE_BUDGET`/`KL_EXE_MEM_MB`/`KL_EXE_STOP_FILE`/`KL_EXE_PROGRESS`; recuperación por caso con sigsetjmp
  (`kl_fault_set_recovery`), reanuda en `<log>.r<k>.txt/.qpa`, máx. `KL_EXE_MAX_RECOVERIES`=50.
- App: `VQConformance.swift` (tandas desde `assets/caselists/index.txt`, resultados en `QUEST/Partidas/<test>/cts/`,
  caso abierto = Crash y se salta), `VQConformanceView.swift` (progreso, Parar; Ajustes > Conformidad). Args deqp:
  `--quiet` (no `--deqp-quiet`), `--deqp-log-flush=enable`. OpenXR: `KL_INTENT_EXTRAS` (graphicsPlugin=Vulkan2…).
- Escáner de APIs (`VQApiScan.swift`, `app/Native/vq_scan.c`): log «Escáner de APIs» por lanzamiento, caché
  `Cache/<j>/apiscan-raw.txt`.
- Sesiones: log 56 fesetround & co. (resuelto); log 57 aborto 1D comprimida (resuelto en VPKEngine) y `[imgfmt]`
  limitado; log 58: 6551 casos, 96,9 % de los soportados; log 59: bloqueo en draw (resuelto arriba, sin probar).
- Pendiente (por orden):
  1-2. HECHO: caso de draw identificado (OpArrayLength) y OpArrayLength implementado (VPKEngine `a0b39c4`).
  3. Fails conocidos (104): texel buffers sint/snorm/16-bit, fragment_shader_output int/snorm, blits 1D (1D como 2D),
     device_init (multiview/shader_draw_parameters → FEATURE_NOT_PRESENT si no se anuncian), format_properties
     (A1R5G5B5, A8B8G8R8_SNORM, R32G32B32 vertex), image_format_properties sampleCounts>1 sin color attachment,
     protected memory props, submit_wait_many_semaphores.
  4. NotSupported: implementar según «No soportados por motivo», con objetivo el perfil Adreno 740 (no todo).
  5. OpenXR CTS en el visor.
  5a. (paso 2, rama paso-a1) `kl_openxr.c` xrEndFrame: validación por capa según spec 1.1 tras `KL_XR_STRICT_LAYERS`
     (def. 0): space inválido/swapchain inválido → HANDLE_INVALID; projection viewCount≠2 o view.type malo o
     imageArrayIndex≥arraySize → VALIDATION_FAILURE; swapchain sin imagen liberada → LAYER_INVALID; imageRect fuera
     → SWAPCHAIN_RECT_INVALID (-25); cuaternión no unitario (±0,01) → POSE_INVALID (-39). Solo compilado en Linux; falta pasar el CTS.
     5b. `kl_ovrp.c`: `KL_OVRP_QUEST3_ID` (def. 0): con `KL_QUEST3` activo, ovrpSystemHeadset=11 (Meta_Quest_3) y
     producto «Meta Quest 3» en GetSystemProductName/2, para que OVRPlugin no contradiga a Build.MODEL.
     `VQConformance.prepareOpenXR` exporta `KL_XR_STRICT_LAYERS=1`; `VQLaunch` lo quita para juegos.
     5c. `KL_OVRP_PRED_FRAMEIDX` (def. 0): GetPredictedDisplayTime(frameIndex) y Time (+0x50) de GetNodePoseState3
     (Render) suman k/hz por frame futuro respecto a `g_fring_begin` (k≤3). Arnés `klepton/runtime/xr/tests/
     pred_frame_offset_test.c` OK. `KL_OVRP_LATENCY` (def. 0): GetAppLatencyTimings2 implementado (Render = EMA
     BeginFrame→GPU done + 1 periodo; Timewarp = 1 periodo; resto 0); apagado sigue Unsupported.
     5d. GetNodePoseState3(Render) con `KL_OVRP_PRED_FRAMEIDX`: `klovrp_extrapolate_pose` (p+=v·dt, q=exp(ω·dt/2)·q,
     dt≤50 ms) al mismo Time; caso en el arnés OK. `ovrp_Start/StopEyeTracking` → Unsupported (sin puerta: Quest 3
     no tiene eye tracking).
     Sin cambiar (no verificable sin OVR_Plugin_Types.h: raw.githubusercontent y repos de Oculus-VR no accesibles): flags SpaceWarp (1<<8|1<<12, 1<<9|1<<13),
     aceleraciones de pose a 0 (velocidades sí), listas bool_yes/result_ok (no mienten sobre passthrough/anclas).

## Juegos: problemas abiertos (resto)
- Congelado tras `ShaderWarmupManager.WarmUpShaders` (AC, logs 53–55): `[futex]` en el `[snap]` para diagnosticar.
- Caché de shaders: `gpu.shader.lib hit` bajo frente a `gpu.pso.recipe loaded` → la caché MSL no acierta del todo.
- Juegos solo GLES bloqueados («solo Vulkan»); Boundary «no configurado»; VPKTAA inline sin probar en juego sin MSAA;
  tirones: precompilar todo antes de jugar.

## Resuelto (referencia; detalle en git)
- 2026-10-13: AA por juego (`KL_AA_MODE`, `VPVK_MSAA`, detección `vq.aaDetected.<key>`), VPKTAA inline sin
  `QueueWaitIdle` (core «pt-port v1»), vídeo sin surface (NV12 CPU), SDK Quest offline (Platform tipado, 485 `ovrp_*`,
  boundary virtual 3×3 m, OpenXR 105 funciones + 1.1), «Diagnóstico de ficheros» (`KL_TRACE_FS`).
- Logs 44–52: VPKTAA inline, QueueSubmit serializado (`g_qsub_mu`), vigilante `[snap]`, usuario offline Ubisoft,
  idiomas `*.lang`, frontera de overlays `ovrp_EndFrame4`, pose EyeLevel con altura medida.
- Logs 24–43: VPKTAA core (`VPKTAACore.swift`: profundidad, MV de SpaceWarp, desoclusión, YCoCg, RCAS), SpaceWarp
  0x300, compositor de overlays (rects, color scale, Equirect 180° SBS, HUD FPS, capturas), JNI/OpenSL/AAsset varios;
  rendimiento ~90 fps (log 26).
- Logs 8–22: Vulkan en Unity (spoof Adreno 740), TLS ELF, liblog, libdl, AAudio/OpenSL (audio OK log 17), SubpassData,
  swizzle, MTLBinaryArchive, poses/postura/recentrado, mandos Sense (PS = botones Meta), manos ocultas, idioma del
  sistema, foveado Quest siempre apagado.

## Arquitectura (referencia)
- Base Klepton (MIT, copia `ce74582` en `klepton/`, ver `THIRD-PARTY-NOTICES.md`): cada `.so` ARM64 se envuelve en un
  dylib firmado (`tools/klepton_ld.c`, `klepton_ld_run`) y se carga con `dlopen`; bionic/JNI/OVRPlugin/OpenXR propios.
  Sin traducción de CPU.
- Gráficos: VPKEngine `VPVulkan.framework` (`scripts/build-vpvulkan.sh`, caché por hash de `vpkengine/gpu/**`; la CI
  clona VPKEngine `main`) en lugar de MoltenVK (`VQSettings.useVPKEngine`, `KL_MVK_DYLIB`). Usa `gpu/metal2`.
- Firma: conversión sin firmar en `QUEST/Cache/<j>/dylibs`; firmadas en
  `Library/Application Support/VisionQuestPro/Signed/<j>/` (`VQPrepare`, `VQSigner`).
- App (`app/Sources/`): `VQLog`, `VQFolder`, `VQGames`, `VQPrepare`, `VQLaunch` (+ `VQSettings`), `VQLauncher`,
  `VQConformance*`; `app/Native/vq_apk.c` (ZIP + AXML), `vq_config.c` (reenvío `vpvk_config_set_*`).
- CI `visionos.yml`: solo `[build]` o manual; release `visionos` (`VisionQuestPro.ipa`, bundle `com.kdt.livecontainer`);
  log de CI en rama `ci-logs` (`visionos.txt`; `gh run view --log-failed` da 403 desde aquí).

## Paso 3A (agente 13): libc/libm/pthread/bionic + escáner
- Escáner: `kl_shim_known` (klepton/runtime/libc/kl_shim.c) ya no marca por prefijo AMedia*/AImage*/AHardwareBuffer*/AAudio*; consulta `kl_aaudio_lookup` / `kl_mediandk_lookup` y la tabla nueva `kl_bionic_extra_lookup`.
- Nuevos: `libc/kl_bionic_extra.c` (tabla g_extra, ~504 símbolos: libm/long double binario128 vía `kl_ld128.h`, clasificación FP, strings/ctype/locale, multibyte UTF-8, nl_langinfo, android_get_device_api_level=32, fdsan, liblog, _chk, pwd/grp AID, spawn attrs...), `libc/kl_bionic_extra2.c` (solo Darwin: xattr, rlimit, memoria, splice/copy_file_range, ids/sched, ptrace, printf ancho, err.h, clock_nanosleep, temporizadores POSIX con hilo, stdio extra, mallinfo/mallopt).
- Modificados: kl_pthread.c (timed locks, rwlock attr, spin, barrier, sem_clockwait, gettid/getname), kl_libc.c (propiedades Quest 3 eureka/kalama API 32 y Quest 2; `__system_property_*` extra; set/add/update denegados), kl_va_thunks.S + kl_va_handlers.c (scanf/wprintf/err...), Makefile (RUNTIME_SHIP).
- Test host: `libc/test/t_bionic_extra.c` (`clang -DKL_EXTRA_TEST -I.. ... -lm`) -> "all ok".
- NO verificado en Darwin (no se compiló nada): kl_bionic_extra2.c, adiciones de kl_pthread.c, ensamblado de los thunks, posibles choques de símbolos klb_timer_* con los static de kl_shim.c.
- Pendiente: ~300 nombres aún sin servir (ver lista de escáner: regex/regcomp, hsearch/tsearch, res_*, iconv, getopt_long, mkstemps/mkostemp, timerfd, signalfd, tcgetattr extra, utmp, getmntent, pselect/ppoll64, preadv/pwritev, pthread_cancel...), inotify ENOSYS, posix_spawn ENOSYS, getcpuclockid solo propio hilo.

## Paso 3 (agente 15): Android completo

- libc/kl_bionic_extra3.c (portable, test host en libc/test/t_bionic_extra3.c, contrastado con glibc): regex, tsearch/hsearch/lsearch, rand48 exacto, getopt_long, iconv (UTF-8/16/32, Latin-1), C11 threads, complejos, utmp/mntent, red offline (gethostby*_r, protocolos, res_*, ns_*), sig2str, termios cf*.
- libc/kl_bionic_extra4.c (solo Darwin): timerfd, signalfd, inotify sobre kqueue, termios bionic, stdio_ext, statx, renameat2, mkstemps, preadv2, pselect, epoll_pwait, sendmmsg/recvmmsg, pidfd (solo pid propio), sigqueue/sigtimedwait, negativas honestas (exec*, SysV IPC, openpty).
- Orden de resolucion: kl_shim_lookup, kl_bionic_extra_lookup, extra3, extra4. Las entradas inotify de extra4 ganan a los ENOSYS de kl_shim.c.
- Riesgos del agente 13: timers renombrados a kl_xt_timer_*, rlimit NOFILE sin bajar el hard, mbsinit con estado.
- Inotify: IN_MODIFY/ATTRIB/DELETE_SELF/MOVE_SELF por EVFILT_VNODE; en directorios CREATE/DELETE/MOVED por diff (max 128 hijos). IN_ACCESS/OPEN/CLOSE_* no existen en kqueue.
- Sin servir a proposito (bionic no los exporta): pthread_cancel, pthread_setcancelstate, wordexp, strfmon.
- Sensores: solo giroscopio (kl_ovrp_head_motion rotado al marco de la cabeza, ticker por cola). Acelerometro, magnetometro y proximidad sin fuente real: no se listan.
- Sin compilar ni probar en Darwin: extra2, extra4, thunks, internals de FILE, kqueue.
