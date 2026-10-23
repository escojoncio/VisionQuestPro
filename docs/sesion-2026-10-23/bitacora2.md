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
