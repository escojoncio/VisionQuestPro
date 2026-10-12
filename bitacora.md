# Bitácora — VisionQuestPro

## Estado (2026-10-12)
- Solo diseño: `CLAUDE.md` (requisitos) y `docs/PLAN.md` (arquitectura y fases). Sin código ni builds.
- Decisión de arquitectura: el código de Quest es ARM64 → sin traducción de CPU. Cada `.so` se envuelve sin cambios
  en un dylib firmado (firma propia de VPEngine, `runtime/vp_codesign.c`) y se carga con `dlopen`; un cargador propio
  aplica relocaciones ELF y resuelve imports contra capas bionic/JNI/OpenXR/Vulkan propias.

## Pendiente (siguiente sesión)
1. Fase 1: `tools/vqanalyze` (APK/OBB, motor, Vulkan/GLES, variante Quest 3, alineación de segmentos, uso de x18,
   imports por librería). Necesita el juego de prueba elegido por el usuario.
2. Comprobar en el visor: TPIDR_EL0 por hilo y x18 (riesgos 1–2 de `docs/PLAN.md`).
