# Bitácora — VisionQuestPro

## Estado (2026-10-12)
- Paso 1 (app genérica Klepton + lanzador + conversión en el visor, Vulkan con MoltenVK, sin ANGLE): **build verde**
  (run 38219167270, commit «kl_app.c: sin objetivo por defecto compilado»). IPA 6,7 MB en release `visionos`
  (`VisionQuestPro.ipa`, con `Frameworks/MoltenVK.framework` y `VPEngineCertificates/`). Nada probado aún en el visor.
- Logs de CI: rama `ci-logs`, fichero `visionos.txt` (`scripts/ci-publish-log.sh`); los logs de Actions no se pueden
  descargar desde la sesión.
- Fallo resuelto en la 2.ª build: `KL_TARGET_DEFAULT_NAME` no existe sin el proyecto de Klepton → `kl_app.c` define
  `KL_TARGET_DEFAULT "(ninguno)"`.

## Paso 1 — app VisionQuestPro (2026-10-12)
- `klepton/`: copia de Klepton `ce74582` (MIT, `klepton/LICENSE`; detalle en `THIRD-PARTY-NOTICES.md`).
  - `tools/klepton_ld.c`: `-DKLEPTON_LD_INPROC` → `klepton_ld_run(in,out,err,len)` (die → longjmp, libera mmap/img/pool/out,
    borra `out`); emite `LC_CODE_SIGNATURE` vacío (dataoff = fin de strtab alineado a 16, datasize 0) como último LC.
  - `visionos/Sources/kl_app.c/.h`: `kl_app_configure_game(name,tree,apk,dylibs,files,obb_rel,container)` (objetivo Unity
    sintetizado, entry libmain); `configure_rest()` = cola común con `kl_app_configure`; `have_translations` acepta `<lib>.dylib`.
  - `KleptonApp.swift`: ventana raíz `VQRootView`; `boot()` llama a `VQLaunch.configure()`.
  - Bridging header: + `vq_apk.h`, `vp_codesign.h`, `vp_pkcs12.h`, `dlfcn.h`. Info.plist: sin icono, + `LSSupportsOpeningDocumentsInPlace`.
- `app/Native/vq_apk.c/.h`: lector ZIP (central dir, stored/deflate, ZIP64, CRC) + AXML→XML texto. **Verificado en Linux**
  (gcc + ASan/UBSan) con APK sintético: lista entradas, extrae stored, decodifica manifest (package, versionName,
  uses-feature vulkan, meta-data supportedDevices).
- `app/Sources/`: `VQLog` (Documents/Registros/app.txt, compartible), `VQFolder` (QUEST: bookmark ajustes+llavero;
  Juegos/Cache/Partidas), `VQGames` (detección y tarjeta), `VQSigner` (p12 en llavero, variantes req4k/req16k/plain4k/plain16k,
  vp_codesign), `VQPrepare` (Cache/<juego>/{tree,dylibs,stamp.txt}; extrae assets/ y manifest texto; .so → klepton_ld_run →
  firma; la 1.ª librería elige variante con dlopen), `VQLaunch` (+ `VQSettings`: sin red, log a fichero, vigilante),
  `VQLauncher` (UI).
- Rutas en el arranque: tree=`QUEST/Cache/<j>/tree`, KL_DYLIB_DIR=`QUEST/Cache/<j>/dylibs`, files=`QUEST/Partidas/<j>`,
  OBB = carpeta del .obb (o la del juego) relativa a files; KL_MVK_DIR = Frameworks de la app.
- CI `.github/workflows/visionos.yml` + `project.yml` (xcodegen): VPEngine sparse (runtime/), MoltenVK v1.4.2 cacheado,
  `libklepton.a` cacheado por hash de `klepton/runtime/**`, IPA sin firmar `com.kdt.livecontainer`, release `visionos`.

### Riesgos abiertos (paso 1)
- dlopen de dylibs firmados que viven en QUEST (carpeta externa, File Provider) puede ser rechazado; alternativa:
  copiar `dylibs/` a Library/Application Support de la app antes de arrancar (como VPGamePack).
- assets/ extraído al tree (duplica espacio); Unity lee el APK montado vía `kl_guest_path_map`.
- No probado: `kl_app_configure_game` con un juego real; OBB relativo con `..`.
- `libklepton.a` se enlaza con `-lklepton` + `-dead_strip`: si falta algún símbolo que el invitado pide por nombre
  (tabla libc/JNI resuelta en runtime), pasar a `-Wl,-force_load`.
- El juego sigue en `KL_IMMERSIVE=1`/`KL_AUTOBOOT=1` forzados por `VQSettings.applyToEnvironment()`; BootView muestra
  aún los paneles de Klepton (Controller alignment, Matting).
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
0. Resultado de la primera build del paso 1; luego prueba en el visor con un juego Unity+Vulkan.
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
