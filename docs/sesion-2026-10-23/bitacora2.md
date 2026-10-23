# Bitácora 2 — sesión 2026-10-23 (logs 9bbdbde: Gundam, Thrill 2, UBOAT, Maestro, Deadpool)

Estado: A MEDIAS. Nada probado en visor. Ninguna build lanzada.
- `vpkengine-memoryless.patch`: cambios de VPKEngine (base `2ac7923`). NO pusheados (push a VPKEngine = 403).
  Aplicar: `cd vpkengine && git apply ../docs/sesion-2026-10-23/vpkengine-memoryless.patch`, pushear a main ANTES de `[build]`.
- `extractos-logs.txt`: extractos usados (zips en `Visionquestprologs/`, sin versionar).

## 1. Hecho en este repo (main)
- `kl_ue4.c` (UBOAT): `libEOSSDK.so` en `UE4_CHAIN` + su `JNI_OnLoad`. Causa: 319 `EOS_*` sin resolver ->
  `fatal: called unresolved import 'EOS_Initialize'`. `KL_REFUSE_EOS=1` lo omite. Log: `libEOSSDK JNI_OnLoad -> ...`.
- `kl_ovrp.c` (Thrill, ojos): el plugin ensancha la capa 2815->3552 (frustum unión) y envía los dos ojos a 0,0 3552x2258
  sin sub-rect; el compositor reproyectaba cada ojo con su cono canteado. `klovrp_note_collapsed_rects()` (EndFrame4)
  activa `g_unify_detected` y `klovrp_eye_tan()` (modo auto) da la unión a todos los lectores. Log: `[ovrp] eye layer
  widened ...`. `KL_OVRP_UNIFY_FRUSTUM=0/1` fuerza. Riesgo: si el juego lee la frustum una sola vez antes de la detección,
  añadir su target a `klovrp_unify_default()`. 0 errores en líneas nuevas (clang + shim Darwin).
- `kl_app.c`: buffers `[hb] gpu` 1024 -> 2048 (ledger).


## 2. Hecho en VPKEngine (parche) — MSAA/depth memoryless + ledger
Causa del MSAA en RAM (Gundam 1478 MB, Thrill 1027 MB, `msaa_freed_total=0`): `entry.cpp vkCreateImage` solo dejaba
memoryless el MSAA de color con usage EXACTO `COLOR_ATTACHMENT[|TRANSIENT]`. Unity URP añade SAMPLED/INPUT/TRANSFER, así
que quedaba persistente. La profundidad MSAA era siempre persistente, y Klepton (`kl_vulkan.c:5619`) la fuerza
SAMPLED/no-TRANSIENT para VPKTAA aunque en auto VPKTAA esté apagado.
- `entry.cpp`: todo attachment MSAA (color o depth, cualquier usage) y todo depth/stencil 2D -> `persistent=false`. El backend
  lo crea diferido: memoryless en tile; memoria solo cuando un frame la necesita (`msaa_materialized why=`), devuelta tras
  120 frames en tile (`msaa_released`). Log `vk.msaa_tile memoryless image=... usage=...`. `VPVK_MSAA_TILE=0` = todo residente.
- `pipelines.cpp classifyPass`: MSAA no-transient cuenta como destino persistente (puerta de pipelines igual que antes).
- `metal_backend.mm`: `msaa_memoryless` incluye depth; ledger `memLedger()` (resident/msaa_mat/rt_mat/memoryless MiB/n +
  top 8 texturas `index:WxHxL/sN/fmt/tipo/MiB`) cada 120 frames y tras materialize/release; sale en `[hb] gpu ... ledger:`.
- `vk_tests.cpp` actualizado. Linux: build + ctest 232/232 OK. `.mm` solo compila en CI Mac: vigilar ese paso.

## 3. Pendiente (orden)
1. Pushear el parche a VPKEngine main; después commit con `[build]`.
2. `kl_vulkan.c` 6468/6545/6715: STORE forzado de la depth del ojo (DONT_CARE->STORE) hace que se materialice cada frame.
   Si el log muestra la depth del ojo en `msaa_materialized`, condicionar esos STORE (y 5619) a `kl_vulkan_aa_gate()`.
3. Gundam mandos: los botones llegan (A/B/gatillo/grip/sticks en `[input]`), OVRInput connected=0x3 active=0x3 desde f1440;
   EventSystem `selected=<none>` siempre. Probar `KL_PROBE_INPUT=1` + `KL_PROBE_TYPES` con clases del juego; revisar
   `GetControllerState6(mask=0x10/0x60)` -> Connected=0.
4. Gundam crash: jetsam +48 s (foot 8038M, tex_msaa 1478M). Debería bajar con el parche; confirmar con el ledger.
5. Maestro (UE4/GLES): CODESIGNING 0x2, PC=0 en hilo de libUE4 tras `ovrp_GetSystemHeadsetType2`. Candidatos:
   `ovrMessageType_ToString` sin resolver, puntero NULL de OVRPlugin. Sin cerrar.
6. Deadpool: jetsam +225 s con comp=1914M (compositor). Sin atribuir.
7. Próximo log: `vk.msaa_tile memoryless`, `msaa_materialized why=`, `msaa_released`, `ledger:`, `eye layer widened`,
   `libEOSSDK JNI_OnLoad`.

## 4. Notas
- La terminal no devuelve salida: redirigir a fichero y leerlo.
- Build local VPKEngine: `cmake -S vpkengine/gpu -B /tmp/vpbuild -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`.
- Un MSAA que el juego lee o guarda entre frames no puede ser memoryless en Metal; esos frames lo materializan y el ledger lo dice.
