# Bitácora — VisionQuestPro

## Estado (2026-10-12)
- Log 16:27 (Batman: Arkham Shadow): abort por `glGetShaderPrecisionFormat` sin implementar (ANGLE no da entrada
  para él). Arreglo: `klepton/runtime/gfx/kl_egl.c` `klgl_GetShaderPrecisionFormat` en `g_gl_impl` (float 127/127/23,
  int 31/30/0, como un GPU de 32 bits en todos los niveles). Llegó hasta Unity creando contexto GLES 2 y consultando
  límites; siguiente fallo pendiente de log.
- Paso 1 (app genérica Klepton + lanzador + conversión en el visor, Vulkan con MoltenVK, sin ANGLE): **build verde**
  (run 38219167270, commit «kl_app.c: sin objetivo por defecto compilado»). IPA 6,7 MB en release `visionos`
  (`VisionQuestPro.ipa`, con `Frameworks/MoltenVK.framework` y `VPEngineCertificates/`). Nada probado aún en el visor.
- Logs de CI: rama `ci-logs`, fichero `visionos.txt` (`scripts/ci-publish-log.sh`); los logs de Actions no se pueden
  descargar desde la sesión.
- Fallo resuelto en la 2.ª build: `KL_TARGET_DEFAULT_NAME` no existe sin el proyecto de Klepton → `kl_app.c` define
  `KL_TARGET_DEFAULT "(ninguno)"`.

## VPEngine como driver Vulkan + firma dentro de la app (2026-10-12)
- `scripts/build-vpvulkan.sh` (CI, paso cacheado por hash de `vpengine/gpu/**`): CMake visionOS de VPEngine `gpu/`,
  objetivo `VPVulkan` → `build/VPVulkan.framework` (`install_name @rpath/VPVulkan.framework/VPVulkan`, Info.plist propio);
  `project.yml` lo embebe (codeSign). Checkout de VPEngine con `runtime` + `gpu`.
- `VQSettings.useVPEngine` (UserDefaults `vq.vpengine`, por defecto sí; interruptor en «Gráficos»). `selectDriver()`
  (desde `applyToEnvironment`): `dlopen(Frameworks/VPVulkan.framework/VPVulkan, RTLD_GLOBAL)` antes del juego y
  `KL_MVK_DYLIB` = esa ruta (Klepton `kl_vulkan.c` `mvk_open` la usa); si falla o se apaga → MoltenVK. Log `Gráficos:`.
- Firma (lección de VPEngine: solo se aceptaron copias firmadas dentro del contenedor de la app, con id y requisitos de
  la app): `VQPrepare` separa conversión y firma. Conversión sin firmar en `QUEST/Cache/<j>/dylibs` (stamp `kld-2`,
  sin certificado); copias firmadas en `Library/Application Support/VisionQuestPro/Signed/<j>/` con `sign.txt`
  (stamp conversión|huella certificado + variante). `KL_DYLIB_DIR` apunta a las firmadas. Variante elegida con dlopen
  de la 1.ª librería, como antes.

## Foveado Quest apagado + ajustes gráficos en la app (2026-10-12)
- Foveado del juego **siempre apagado** (log `vq.fov`):
  - `klepton/runtime/xr/kl_ovrp.c` `ovrp_hit`: 1.ª llamada a cualquier `*Foveation*`/`*TiledMultiRes*` → línea
    `vq.fov ovrp <nombre> -> off`. Comportamiento previo de Klepton conservado: `GetTiledMultiResSupported` = no,
    `GetTiledMultiResLevel` = 0, `GetFoveationEyeTrackedSupported` = 0, `GetLayerTextureFoveation` = UNSUPPORTED,
    setters (`SetTiledMultiResLevel/Dynamic`, `SetFoveationEyeTracked`) = éxito sin efecto.
  - `kl_openxr.c`: `klxr_is_foveation_ext` (XR_FB_foveation, _configuration, _vulkan, XR_META_foveation_eye_tracked,
    XR_META_vulkan_swapchain_create_info, XR_FB_swapchain_update_state(_vulkan)): no se anuncian; si el juego las
    habilita igualmente, `xrCreateInstance` las acepta e ignora. Nuevos `xrCreateFoveationProfileFB` (perfil ficticio),
    `xrDestroyFoveationProfileFB`, `xrUpdateSwapchainFB` = éxito sin efecto (filas añadidas a `KL_XR_ENTRY_POINTS`).
    `xrCreateSwapchain` registra e ignora `XrSwapchainCreateInfoFoveationFB` (tipo 1000114000).
  - `gfx/kl_vulkan.c`: `vkEnumerateDeviceExtensionProperties` oculta `VK_EXT_fragment_density_map*`,
    `VK_KHR_fragment_shading_rate`, `VK_QCOM_fragment_density_map_offset`; `klvk_CreateDevice` los descarta si se piden.
- Ajustes en la app (`VQSettings` en `app/Sources/VQLaunch.swift`, UserDefaults `vq.fov*`, `vq.vpktaa*`,
  `vq.renderScale`, `vq.aswOff`, `vq.quest3`; sección «Gráficos» en `VQLauncher.swift`): foveado apagado/M2/M5/
  personalizado (zona nítida, periferia, altura; M5 solo si `VQSettings.isM5`, misma regla que PTVisionPro), VPKTAA
  (activado, %, nitidez), resolución de render (→ `KL_OVRP_EYE_SCALE`), ASW desactivado (Klepton ya nunca da texturas
  SpaceWarp), perfil Quest 3 (→ `KL_OVRP_HEADSET=11`; apagado por defecto: JNI/Build.MODEL de Klepton dicen Quest 2).
- `app/Native/vq_config.c/.h` (+ bridging header): `vq_gfx_config_apply` guarda, registra y reenvía por `dlsym` a
  `vpvk_config_set_*` de VPVulkan (VPEngine `gpu/vulkan/include/vpvk/config.h`); con MoltenVK solo registra.
- Build verde: run 38222233549 (commit 8c33463). Verificado antes: sintaxis de los .c con clang en Linux (sin errores en las zonas tocadas), `vq_config.c` con gcc -Wall.
  Swift sin compilar en local (CI).

### Pendiente (foveado/VPKTAA)
- Cargar libVPVulkan en lugar de MoltenVK (`KL_MVK_DYLIB`) y llamar `vq_gfx_config_forward()` tras el dlopen.
- VPEngine: rate map en pases multiview de ojo y pase VPKTAA final (ver VPEngine `docs/VULKAN.md` «Foveado y VPKTAA»).
- Comprobar en el visor que `KL_OVRP_HEADSET=11` no rompe el mapeo de mandos (si falla, dejar perfil Quest 3 apagado).

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

## 2026-10-12 — Batman caía a GLES
- Log: VPVulkan carga, Unity crea instancia y enumera GPU pero nunca llama vkCreateDevice; crea contexto GLES2 y aborta (sin ANGLE).
- Causa: comprobación de hardware del juego. El nombre "Adreno (TM) 740 (VPVulkan)" y deviceID 0x43050a01 no coinciden; el spoof de Klepton solo se activaba con target "batman".
- Arreglo: VPVulkan informa "Adreno (TM) 740" / 0x43051401 y un LUID válido; la app fija KL_VK_SPOOF_ADRENO=1 siempre.
- Identidad Quest 3 genérica: `KL_QUEST3=1` hace que Android Build (Quest 3/eureka), cabeza OVRPlugin (11), nombre OpenXR ("Meta Quest 3") y GPU (Adreno 740) se presenten como Quest 3 en todos los juegos, no por target. Ajuste "Perfil Quest 3" activado por defecto (clave nueva vq.quest3v2).

## 2026-10-12 — Unity seguía en GLES tras el spoof de GPU
- Log (klepton-boot 12:25): spoof Adreno activo, 2 instancias Vulkan, sin vkCreateDevice; justo antes de EGL, `Activity.getPreferences(0).getBoolean` sobre `com.unity3d.player.UnityPlayerActivity.xml` (1 entrada guardada). (Hipótesis descartada en el log siguiente: ver entrada «gles-api-check».) ANGLE no va en la IPA → null driver → aborto en `glGetShaderPrecisionFormat`.
- `klepton/runtime/jni/kl_jni_prefs.c`: `klj_unity_activity_bool` (en `KLJ_SP_GET`, solo kind 'Z' y fichero UnityPlayerActivity): registra cada lectura (`prefs[UnityPlayerActivity] getBoolean(...)`) y un `true` guardado se devuelve `false`; `KL_KEEP_UNITY_GFX_FALLBACK=1` lo desactiva.
- `klepton/runtime/gfx/kl_glfb.c`: fallo de carga de ANGLE se recuerda (`g_load_failed`), un solo aviso.
- `app/Sources/VQPrepare.swift` `writeVersionFile`: en cada arranque escribe `tree/apktool.yml` (versionCode/versionName del manifiesto) → nombre de OBB correcto (antes 545 vs `main.350961`).
- No añadido stub GL de `glGetShaderPrecisionFormat`: GLES no es la ruta objetivo.
### Comprobar en el siguiente log
- Línea `prefs[UnityPlayerActivity] getBoolean("<clave>")`: confirma la clave. Debe aparecer `vkCreateDevice` y no `[egl] context`.
- `guest version 350961 / ...` en vez de `no apktool.yml`.
- Si sigue en GLES: falta otra comprobación (driverVersion/driverID de Adreno, extensiones) → registrar `vkGetPhysicalDeviceFeatures2`/extensiones pedidas.

## 2026-10-12 — `gles-api-check` = casilla del aviso de hardware; -force-vulkan
- Log klepton-boot 3: OBB correcto (`guest version 350961`, main.350961 presente). La clave leída es `gles-api-check`: casilla «no volver a avisar» del diálogo de Unity «Your device does not match the hardware requirements»; Klepton pulsa Continue y Unity sigue por GLES. La detección Vulkan de Unity se repite en cada arranque; la casilla solo oculta el aviso (la anulación de `kl_jni_prefs.c` se mantiene, inocua).
- Datos del APK (`assets/bin/Data/data.unity3d` → globalgamemanagers): **sin lista de filtrado de dispositivos Vulkan**. libunity 2022.3.45 tiene el argumento `force-vulkan` y lee extras del Intent (getExtras/containsKey).
- `klepton/runtime/jni/kl_jni_android.c` `klj_Intent_getExtras`: kv[6]; nuevo extra `unity` = `KL_UNITY_ARGS`.
- `kl_jni_services.c` `klj_Bundle_containsKey`: registra clave y resultado.
- App: `VQSettings.forceVulkan` (UserDefaults `vq.forceVulkan`, por defecto sí; interruptor «Forzar Vulkan en juegos Unity» en Gráficos) → `KL_UNITY_ARGS=-force-vulkan`; log `Gráficos: forzar Vulkan en Unity`.
### Comprobar en el siguiente log
- `Bundle.containsKey("unity") -> true` y línea `CommandLine:` de Unity; luego `vkCreateDevice`.
- Si Unity no lee `unity`: la detección rechaza algo de la GPU. Siguiente paso: registrar todas las consultas vkGetPhysicalDevice* de la instancia de Unity (apiVersion 1.1 de MoltenVK vs 1.3 de Adreno; driverVersion con vendor Qualcomm).

## 2026-10-12 — Causa real del rechazo Vulkan: driverVersion (desensamblado de libunity)
- Log 4: `-force-vulkan` llega (`Bundle.containsKey("unity") -> true`, `getString`), Unity reintenta Vulkan pero vuelve a rechazar. `force-vulkan` no salta la detección.
- libunity 2022.3.45 (Batman), función de detección en 0xdaa154 (log «Vulkan detection: %d», resultado 0 sin Vulkan / 1 rechazado / 2 válido). Lee VkPhysicalDeviceProperties y conmuta por vendorID:
  - Qualcomm 0x5143: nombre «Adreno (TM) 4xx» → rechazado (0xdcc858). Si no, 0xdcc8e8 (Qualcomm y global <0) → exige `driverVersion >= 0x80000000`; si no, apiVersion >= 1.0.49.
  - ARM 0x13b5 (Mali-T…), NVIDIA, Intel, ImgTec: mínimos propios. Otro vendor (Apple): válido solo si API Android > 29.
- MoltenVK da un driverVersion pequeño → rechazo. Arreglo: `kl_vulkan.c` `klvk_spoof_props` pone `driverVersion = VK_MAKE_VERSION(512,744,0)` (= 0x802e8000, como un driver Adreno 512.x real); se registra en la línea del spoof.
- Para otros juegos Unity 2022 la misma regla aplica: con vendor Qualcomm hace falta bit 31 en driverVersion.

## 2026-10-12 — Log 5: driverVersion no bastó; registro de quién consulta la GPU
- driverVersion 0x802e8000 aplicado (línea del spoof), aviso de hardware sigue. La salida `printf_console` de Unity (p. ej. «Vulkan detection: %d», «[Vulkan init] …») no aparece en el log: build release, no se puede leer el resultado directamente.
- libunity: la selección de API (0x89eb.. → 0x89ece4 muestra el aviso `gles-api-check`) recorre la lista de APIs; 0x89e4b0 es la comprobación GLES/EGL. Selección de GPU Vulkan en 0xdb11cc: si el plugin XR da un `xrDevice` que no está en la lista enumerada → NULL → Vulkan descartado. Ninguna llamada a Properties2 con ID/LUID en el log.
- `kl_vulkan.c`: `klvk_who`/`KLVK_WHO` registra cada sitio de llamada distinto (`who: <vk fn> <- lib+off`, máx. 128) en EnumeratePhysicalDevices, Get*Properties(2), Features(2), QueueFamilyProperties, MemoryProperties, EnumerateDeviceExtensionProperties. Include `klepton.h` para `kl_addr_image`.
### Siguiente
- Con las líneas `who:` de libunity, situar cada llamada en el desensamblado (detección 0xdaa154, selección 0xdb11cc) y ver qué rama descarta Vulkan.

## 2026-10-12 — Log 6: causa real, el id de adaptador XR
- `who:` muestra: detección 0xdaa154 completa (Properties en 0xdaa2e0) y pasa con el spoof. El rechazo está en la creación del GfxDevice Vulkan (0xdaa54c): `dab8d4` guarda `XRManager->GetGraphicsAdapterId(Vulkan=21)` (OculusXRPlugin → `ovrp_GetDisplayAdapterId2`) y `0xdb11cc` lo compara **como handle VkPhysicalDevice** con los enumerados; un LUID ≠ 0 no coincide → NULL → `cf68b8` (0x889ddc) falla → GLES (0x89ea70, que solo mira GLES2/3 y muestra el aviso `gles-api-check` al no haber ninguno). Con 0 elige por deviceType (como en Quest, donde no hay LUID).
- `kl_vulkan.c` `cache_display_luid`: guarda LUID en cero por defecto (`KL_ADAPTER_LUID_REAL=1` lo desactiva); la lectura de deviceLUID en Properties2 se fuerza al mismo cero, así la ruta por LUID de Unity antigua (AC Nexus) sigue coincidiendo (sin verificar en log). Comentario de `kl_ovrp.c` `klovrp_GetDisplayAdapterId2` actualizado.
### Comprobar
- Línea `display adapter id reported as zero`; `who: vkGetPhysicalDeviceQueueFamilyProperties <- libunity.so+0xdaa774` y `vkCreateDevice`; sin aviso de hardware.
