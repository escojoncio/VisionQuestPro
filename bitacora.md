# Bitácora — VisionQuestPro

## Estado (2026-10-12)
- Solo diseño: `CLAUDE.md` (requisitos) y `docs/PLAN.md` (arquitectura y fases). Sin código ni builds.
- Decisión de arquitectura: el código de Quest es ARM64 → sin traducción de CPU. Cada `.so` se envuelve sin cambios
  en un dylib firmado (firma propia de VPEngine, `runtime/vp_codesign.c`) y se carga con `dlopen`; un cargador propio
  aplica relocaciones ELF y resuelve imports contra capas bionic/JNI/OpenXR/Vulkan propias.

- Base de CPU/sistema: Klepton (MIT). Ya parchea x18, carga sin JIT, bionic/NDK/JNI/ovrp. Gráficos: VPEngine sustituye
  a MoltenVK.

## Klepton: puntos de integración (leído en shinyquagsire23/Klepton, commit de 2026-09-03)
- Hoy: un juego por app, convertido en Mac (`visionos/mkguest.sh` → `build/klepton-ld`) y metido en `Frameworks/<target>/`.
- `tools/klepton_ld.c` (776 líneas, C puro, escribe Mach-O sin linker; parchea x18 y TLS; sección `__TEXT,__klelf`) →
  compilable dentro de la app para convertir en el visor.
- `runtime/kl_image.c` `kl_load_auto`: busca traducciones en `KL_DYLIB_DIR` (`<lib>.dylib` o `<lib>.framework/<lib>`).
- `runtime/kl_target.c` `synthesize()`: objetivo genérico Unity (entry `libmain`) para árboles fuera de la tabla.
- ANGLE se carga con `dlopen` (`gfx/kl_glfb.c`) → opcional; se omite (GLES fuera de la v1).
- `gfx/kl_vulkan.c` (6070 líneas): `libvulkan.so` del invitado → `dlopen` de MoltenVK + `VK_EXT_metal_objects` para dar el
  `MTLTexture` del ojo al compositor. **Punto de entrada de VPEngine**: un driver Vulkan propio (dylib con
  `vkGetInstanceProcAddr`) cargado en lugar de MoltenVK, que también expone `VK_EXT_metal_objects`.

## Pendiente (siguiente sesión)
1. Fase 1: `tools/vqanalyze` (APK/OBB, motor, Vulkan/GLES, variante Quest 3, alineación de segmentos, uso de x18,
   imports por librería). Necesita el juego de prueba elegido por el usuario.
2. Comprobar en el visor: TPIDR_EL0 por hilo y x18 (riesgos 1–2 de `docs/PLAN.md`).

## Análisis de Batman: Arkham Shadow (com.camouflaj.manta, solo para estudio)
- Unity IL2CPP, Vulkan, `com.oculus.supportedDevices=quest3+`, FDM (`xr-vulkan-extension-fragment-density-map-enabled=1`),
  OculusXRPlugin → libOVRPlugin → libopenxr_loader. Audio Wwise (OpenSL ES). Datos: bundles Addressables sueltos + OBB.
- 20 `.so` arm64. Relocaciones solo `RELATIVE` (538k), `ABS64`, `GLOB_DAT`, `JUMP_SLOT`. Sin TEXTREL, sin TLSDESC.
- Ninguna lee `tpidr_el0` → riesgo TLS descartado para este juego.
- Páginas: todo alineado a 4 KB; texto y datos comparten páginas de 16 KB → el envoltorio debe desplazar datos a frontera
  de 16 KB y parchear ADRP/ADR/LDR-literal que apunten a ellos (en conversión, firmado; sin JIT).
- **x18 usado como registro normal** (XNU lo pone a 0 en cambios de contexto): libunity 387 funciones, libOni 32,
  libc++_shared 8, libOculusXRPlugin 4, libil2cpp 1 (solo guardado/restaurado de contexto, inocuo). Solo 4 funciones
  tienen un registro x9–x17 libre en toda la función → hace falta renombrado por vivacidad (x18 nunca vive a través de
  llamadas en AAPCS64, así que basta un temporal libre en cada rango vivo de x18). Script: análisis por FDE de `.eh_frame`.
