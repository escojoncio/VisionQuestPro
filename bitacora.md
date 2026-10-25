# Bitácora — VisionQuestPro

## Last done
- 2026-10-25: sub-agentes `reader`/`coder`/`Explore` en `.claude/agents/`; bitácora compactada a `notas/`.
- 2026-10-25: log RE4 `047bd4c` revisado: CODESIGNING 0x2 a ~3 s tras "does not support Vulkan" (ya arreglado después); sin datos de MSAA.
- 2026-10-23: parche VPKEngine MSAA/depth memoryless + ledger (`docs/sesion-2026-10-23/vpkengine-memoryless.patch`, base `2ac7923`), aplicado en local sobre main `9c6f69c`, sin push.
- 2026-10-23: `kl_ue4.c` `libEOSSDK.so` en `UE4_CHAIN` (UBOAT); `kl_ovrp.c` unión de frustums con ojos colapsados (Thrill 2); `kl_app.c` buffers `[hb] gpu` 2048.
- 2026-10-21: RE4 `onLowMemory` (`kl_na_low_memory`), symlink `.f` (stack smashing), Deadpool idioma/audio.

## Current state
- Última build en verde: run CI 39419833988 (commit 4b29253, motor ed942c7). Todo lo posterior sin build ni visor.
- Ninguna build desde 9bbdbde. Todo lo de 2026-10-21/23 unverified en visor.
- Pendiente (orden):
  1. Revisar adversarialmente el parche memoryless, pushearlo a VPKEngine main, luego commit `[build]`.
  2. `kl_vulkan.c` 5619/6468/6545/6715: depth del ojo forzada SAMPLED/no-TRANSIENT y STORE aunque VPKTAA esté apagado → se materializa cada frame. Condicionar a `kl_vulkan_aa_gate()`.
  3. RE4 jetsam (9bbdbde, inventario tras cambio de zona): memoria GPU no contabilizada 0,2→3,7 GB; +94 allocs memtype0 vivas; +1399 imágenes sin destruir. Quién las retiene: unverified.
  4. Gundam mandos: `GetControllerState6(mask=0x10/0x60)` → Connected=0; probar `KL_PROBE_INPUT=1`.
  5. Maestro: CODESIGNING 0x2, PC=0 en libUE4. Sin cerrar.
  6. Deadpool: jetsam +225 s comp=1914M; crash al Salir en `KleptonCompositor.eyeSource`.
  7. Próximo log: `vk.msaa_tile memoryless`, `msaa_materialized why=`, `msaa_released`, `ledger:`, `eye layer widened`, `libEOSSDK JNI_OnLoad`.
- Riesgos abiertos: lo keyed por `kl_driver_target_name()` no se activa (target = carpeta); `klj_arr(x)->data` sin proteger en `jni/kl_jni_{android,bridge,services,ue4,window}.c`; pausa con la Corona = `_exit(0)`; foveado propio sin probar; GLES sin transform feedback/geometry/tess.

## Decisions
- Todo global, sin condiciones por juego (salvo ajustes del usuario: AA, resolución, anisótropo, RCAS).
- Solo offline: `KL_NET_OFFLINE`=1, `KL_PLAT_TOKEN_ERROR`=1; IAP = fallo OFFLINE honesto; resto de DRM aborta.
- MSAA del juego mantenido por defecto; Automático: VPKTAA solo si no hay MSAA.
- `nativeLowMemory` solo en el hilo del juego; no desde `kl_jni_drain_ui_tasks`.
- `ovrp_GetEyeLayerRecommendedResolution` → -1004 sin capa de ojo viva.
- Slider de resolución = tamaño final del ojo; render_scale = 1.
- Fin de frame segmentado apagado (`KL_VK_FRAME_ASYNC=1` lo activa): judder sin fps.
- Calidad del compositor 1.0: no tocar. Sin API para elegir 90/96/100 Hz; ritmo encajado al panel.
- GLES ES 3.1. Identidad Quest 3 (`KL_QUEST3`); mandos Touch de Quest 2; eye tracking Unsupported.
- Foveado de Quest apagado; propio opt-in, M5 solo en M5, exige VPKTAA nativo.
- ASW activo por defecto (`KL_CP_ASW`/`KL_CP_PTW`). Frame congelado: 3,0 s.
- Valores: burst audio 1024 con Wwise; `kMaxArchiveFiles` 512; `VPVK_MAX_DRAWS_PER_CB` 256; anillo TAA 4; máscara UI R8 slot 7; `KL_OVERLAY_SUBMIT_ORDER`=1; profundidad `.store` en el composite.
- Datos de juegos nunca en el repo.
- Build solo con `[build]`, completa y revisada; pushear VPKEngine antes.

## Files in play
- `vpkengine`: `gpu/vulkan/src/{entry.cpp,pipelines.cpp,platform_metal.mm,vpvk.h}`, `gpu/vulkan/include/vpvk/config.h`, `gpu/metal2/src/metal_backend.mm`, `gpu/metal2/include/vpgpu/metal2/metal_backend.h`, `gpu/vulkan/tests/vk_tests.cpp`.
- `klepton/runtime/gfx/kl_vulkan.c` (depth del ojo), `klepton/runtime/guest/kl_ue4.c`, `klepton/runtime/xr/kl_ovrp.c`.
- `docs/sesion-2026-10-23/` (parche, extractos de logs).

## Module index
- `notas/arquitectura.md`: runtime, app, visor, datos de plataforma, ABI OVRPlugin, resolución.
- `notas/diagnostico.md`: etiquetas del log y dónde está el log de CI.
- `notas/juegos.md`: historial por juego, causas demostradas e hipótesis refutadas.
- `notas/build.md`: CI, Gate, tests, compilación local.
