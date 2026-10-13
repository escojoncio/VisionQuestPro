# Bitácora — VisionQuestPro

Historial completo: `git log -p bitacora.md` (compactada el 2026-10-13 tras el log 54; todo lo anterior al 2026-10-13
tarde está resumido abajo y detallado en git).

## EMPEZAR AQUÍ — 2026-10-13 noche (log 55 + capturas; plan de conformidad en marcha)

### Log 55 (AC Nexus, con capturas): hallazgos firmes
- **Rosa = capas overlay, no el ojo.** Capturas: `eye0/1` NEGRO en todos los frames; `output` (compositor) rosa;
  26/33 capas OVROverlay (`capture-*-layerN-0.png`) son 255,0,255 sólido. Patrón: capas escritas por el juego con
  `vkCmdCopyImage` desde textura (fuente fmt 43/37) = rosa; capas cuyo RT se dibuja en el frame (`pass-600-*`,
  capas 27/28/29/32/5/16/26) = negro. Magenta exacto ⇒ shader de error de Unity en el origen o fallo de copia/subida
  en VPKEngine. Siguiente paso: rastrear qué imagen es el src de esos `vkCmdCopyImage` (log del src en
  `klvk_CmdCopyImage` de capas) y cómo se rellenó (CopyBufferToImage vs pase). El CTS (api.copy_and_blit,
  texture) debe cubrirlo.
- Congelado idéntico: serial 624 en cuanto empieza `ShaderWarmupManager.WarmUpShaders` (1700 pipelines en ~2,6 s,
  luego `PopulatePSOCache`); el usuario salió 1 s después. `gpu.shader.lib hit=0 miss=3488` aunque
  `gpu.pso.recipe loaded pipelines=3169` → la caché MSL/biblioteca NO acierta (arreglo de la build anterior no funciona).
- Audio: Wwise busca `main.207706.com.Ubisoft.ACNexusVR.obb` (no existe) y luego `AAssetManager_open` con ruta
  absoluta → MISS (arreglo pendiente descrito abajo). «Package path couldn't be set» ocurre ANTES de cualquier E/S.
- Glitch del visor: sin crash ni gpu.err; últimos 2 s: footprint 3,5→4,0 GB, cadencia del compositor 48 ms media /
  210 ms máx., mientras se guardaban ~36 PNG (capturas frame 1200) con el juego congelado; ojo 3840×2485 (escala 1,3
  pedida por el juego). Regla nueva: tests de conformidad SOLO en ventana (sin inmersivo).

### Conformidad: implementado (commits b517056, e87d43b; build de app pendiente)
- CI `.github/workflows/conformance.yml` (Linux, manual o `[conformance]`): VK-GL-CTS `vulkan-cts-1.4.6.2`,
  `DEQP_ANDROID_EXE=ON`, NDK del runner, API 29, `-Wl,--export-dynamic-symbol=main`, parche
  `conformance/vulkan/patch_deqp.py` (describePlatform sin activity), listas `conformance/vulkan/caselists.py`
  (perfil `rapida` ~36k casos de grupos relevantes para Quest, muestreo CRC32 estable, tandas de 400;
  `completa` solo con `VQ_CTS_FULL=1`). Empaqueta `VulkanCTS.apk` (aapt2: manifest `conformance/vulkan/
  AndroidManifest.xml`, meta-data `com.visionquestpro.guest=exe`, lib `libdeqp-vk.so` = el ejecutable, assets
  `vulkan/` + `caselists/`). Release `conformance`.
- Klepton: guest kind `KL_GUEST_EXE` (`runtime/guest/kl_exe.c`): carga como NativeActivity (`kl_native_load`), luego
  `main()` en hilo guest de 64 MB (`kl_guest_thread_create` en kl_pthread.c) una vez por línea de
  `KL_EXE_ARGS_FILE` (args separados por TAB); para entre líneas por `KL_EXE_BUDGET` s, `KL_EXE_MEM_MB`,
  `KL_EXE_STOP_FILE`; progreso en `KL_EXE_PROGRESS` ("i running" / "i done rc" / "N finished").
- App: `VQConformance.swift` (planifica tandas desde `assets/caselists/index.txt`, resultados en
  `QUEST/Partidas/<test>/cts/<tanda>.<parte>.qpa`; caso abierto sin `#endTestCaseResult` = Crash y se reanuda desde
  el siguiente; resumen en log + `Registros/conformidad-vulkan.txt`), `VQConformanceView.swift` (ventana con
  progreso, Parar, aviso de cuelgue a 5 min; Ajustes > Conformidad: descarga, minutos, memoria, borrar resultados).
  `KL_IMMERSIVE=0` para el guest exe. URL de descarga desde `VQBuildInfo.swift` (CI escribe `$GITHUB_REPOSITORY`).
- Escáner de APIs (`VQApiScan.swift` + `app/Native/vq_scan.c`; Klepton `kl_shim_known`, `kl_jni_knows_class`):
  cada lanzamiento, log «Escáner de APIs: …» con importaciones nativas sin implementar y clases Java del sistema
  sin binding.
- Pendiente: verificar que la CI del CTS compila/empaqueta; build de app `[build]`; primera tanda en el visor;
  OpenXR CTS (fase B: gradle en `src/conformance`, NDK 23.2, lib `conformance_test`, necesita sesión inmersiva).

## Estado al cierre del 2026-10-13 tarde (logs 53–54, AC Nexus)
Última build: `de2bf58` (VisionQuestPro) + `166f0b1` (VPKEngine). Usuario: **AC Nexus y AoT siguen igual** (rosa
constante, también sobre los vídeos; el juego no avanza de la carga). En el log 54 el usuario NO activó «Diagnóstico
de ficheros» (solo 24 líneas `[fs]`). Reporta además que el visor se quedó «glitcheado» y tuvo que reiniciarlo
(sin evidencia en el log; probable relación con los 4 GB+ de imágenes/MSAA 4x — sin confirmar).

### Corrección importante de un diagnóstico anterior
- En el log 53 atribuí el congelado a `gpu.err … Too much geometry to support memoryless render pass attachments`
  (f=808, 16,5 s). **Era falso**: ese error ocurre a las 21:59:02, justo con `[app] scene phase -> background`
  (el usuario salió de la app); el congelado empezó a las 21:58:44. En el log 54, con el arreglo de memoryless
  aplicado, **no hay ningún gpu.err** y el congelado es idéntico. El arreglo de memoryless (VPKEngine `HeavyGeometry`
  + `memorylessFailed`) es correcto como defensa pero NO es la causa.

### Congelado de AC (y probablemente AoT): datos firmes
- Logs 53 y 54: el serial del compositor se para SIEMPRE en **632** tras `ShaderWarmupManager.WarmUpShaders` y
  ~1700 pipelines (`[vk] pipeline warmup: 1700 graphics`), ~5 s de compilación. Después no hay más
  vkCreateGraphicsPipelines ni submits; heartbeat sigue (`phase=frame pump`).
- `[snap]` (log 54 l.10955; log 53 l.9657): los 24 hilos guest (incluido "Klepton Guest" = hilo principal de Unity)
  están en `kl_futex_impl` ← `libunity.so+0xb8fe50`. Desensamblado (libunity de AC): esa función es
  `syscall(98 /*futex*/, addr, 128 /*FUTEX_WAIT|PRIVATE*/, val, &timespec{ms/1000, (ms%1000)*1e6})` = espera de
  semáforo con timeout relativo. Ningún hilo está dentro de VPVulkan/Klepton vk → nadie está bloqueado en GPU.
  Hipótesis a verificar (en orden): (1) despertar perdido en `kl_futex_impl` (FUTEX_WAKE vs WAIT con timeout,
  `val` comparado de forma no atómica); (2) Unity espera un evento que debe señalar un hilo que murió/salió
  (buscar hilos que terminan cerca de 632: `pthread_exit`, excepciones JNI); (3) espera a una respuesta de
  Platform/AssetFile (`[plat]`) que no llega. Siguiente paso: leer `kl_futex_impl` (klepton/runtime/libc/kl_pthread.c
  o kl_libc.c) y añadir contador por dirección de futex en el `[snap]` (addr, val actual, waiters, último WAKE).
- APK de AC extraído para estudio (no se sube nada): libunity/libAkSoundEngine/global-metadata en el scratchpad de
  la sesión; el usuario dio acceso a `A:\QuestSideload\Juegos\Assassins Creed Nexus VR` y `…\Attack on Titan VR-
  Unbreakable` (solo lectura, vía puente del PC).

### Rosa (AC, "todo el rato, incluso en los vídeos")
- No hay textura placeholder rosa en VPKEngine. El vídeo de carga lo dibuja nuestro compositor (AVPlayer, skybox
  Equirect capa 4) → si se ve rosa encima, es la **capa de ojo del juego** (opaca, `.full`) pintada en rosa.
  Rosa/magenta uniforme en Unity = **shader de error de Unity** (variante no soportada / pipeline fallido) o memoria
  sin inicializar. Log 54 no tiene `vk.err`, ni `Shader Unsupported`.
- Siguiente paso: activar `diagCaptures` (KL_CAPTURE, ya existe en Ajustes) una vez y pedir la captura del ojo para
  ver si es rosa sólido (shader de error) o basura; comparar con `VPVK_FORCE_1X=1` (modo AA VPKTAA en Ajustes del
  juego) para descartar el nuevo camino MSAA 4x (antes de hoy se forzaba 1x y el rosa ya existía según el usuario).

### Audio de AC (Wwise 2019.2.13): datos firmes
- La copia del usuario NO tiene `main.*.obb` (release VRP v207706; no es necesario): los bancos están en
  `Master.pck` (1,3 GB) en la carpeta obb, más `en.lang` (paquete de idioma).
- `CSharp_SetBasePath` (0x5dd60) → SetBasePath del low-level IO (0x5c3a8): solo copia la ruta (falla solo si >259).
  «Package path couldn't be set, check paths» es del C# de Ubisoft (global-metadata), no de Wwise.
- Log 54: `[asset] AAssetManager_open("/sdcard/Android/obb/com.Ubisoft.ACNexusVR/English(US)/Init.bnk") MISS` →
  Wwise de Android abre por **AAssetManager_open con RUTA ABSOLUTA**. Nuestro `kl_AAssetManager_open`
  (klepton/runtime/guest/kl_ndk.c:381) antepone siempre la raíz de assets → falla. Arreglo genérico pendiente: si
  `fname` empieza por '/', mapear con `kl_guest_path` y abrir ese fichero (igual en `AAssetManager_openDir`).
  Probable causa también del «Package path couldn't be set» (comprobación vía openDir/open de Master.pck).

### Cobertura libc pendiente (genérica)
Sin shim (si un juego las importa → «called unresolved import»): fstatat(64), faccessat, open64, openat64,
__openat_2, freopen, readlink(at), mkdirat, rmdir, unlinkat, renameat, statfs64, statvfs64, truncate, chmod,
utime/utimes/utimensat, nftw/ftw, glob, creat, remove, pathconf, __realpath_chk, __getcwd_chk, fchdir. Implementar
con `kl_guest_path` + conversión de struct (stat_to_bionic, bionic_statfs) y de constantes *at (AT_FDCWD Linux −100 /
Darwin −2; AT_SYMLINK_NOFOLLOW 0x100/0x20; AT_REMOVEDIR 0x200/0x80; AT_EACCESS 0x200/0x10).

### Plan acordado con el usuario (no hacer parches por juego)
1. **Conformidad** (en curso, nada compilado aún): tests oficiales Khronos ejecutados como guest dentro de la app.
   - Hecho: lanzador por motor (`VQGames.guestKind/entryLib/nativeFuncName`, env `KL_GUEST_KIND/ENTRY/FUNC`,
     `kl_app.c` y `kl_native.c`), extras de Intent genéricos `KL_INTENT_EXTRAS` ("k=v" por línea) con
     `getStringExtra`/`getStringArrayExtra` (TAB) en `kl_jni_android.c`. Quitado el bloqueo «solo Unity».
   - Falta: workflow `conformance.yml` (Linux + NDK del runner; manual): VK-GL-CTS `vulkan-cts-1.4.6.2` con
     `scripts/android/build_apk.py --sdk $ANDROID_HOME --ndk $ANDROID_NDK_LATEST_HOME --abis arm64-v8a
     --native-build-type Release` (NativeActivity lib `deqp`, `android.app.func_name=createTestActivity`, extra
     `cmdLine`; manifest sin feature Vulkan → excepción por paquete `com.drawelements.deqp` en VQGames);
     OpenXR-CTS `openxr-cts-1.1.58.0` (gradle en `src/conformance`, NDK 23.2.8568313, lib `conformance_test`,
     extras `args` (array), `graphicsPlugin`, `xmlFilename`; excluir tests interactivos). Release `conformance`.
     Después: botón «Conformidad» en la app (descarga, lanza, resume .qpa/.xml en el log; /sdcard = carpeta de
     partidas del juego). Regla: agentes ≤1 h, reporte cada 20 min.
2. **Escáner de APIs Android por juego** (al preparar): importaciones de cada .so (libc/NDK) + cadenas JNI de
   .so/classes.dex vs lo implementado → log de faltantes. Inventario base: `docs/android-ndk-inventario.md`.
3. Tirones de shaders: fase posterior (precompilar todos los pipelines antes de jugar, nunca compilar en un frame).
- Juegos solo GLES siguen bloqueados («solo Vulkan»): pendiente.

## Hecho el 2026-10-13 (builds 57ba002 → de2bf58; detalle en git)
- AA por juego: `KL_AA_MODE` auto|vpktaa|msaa|off (def. auto), `VPVK_MSAA` 0/2/4/8 (chip del usuario: máx. 4x),
  detección `vq.aaDetected.<key>` (funciona: AC → `vk.msaa detected eye=4x`, VPKTAA off). Hoja «Ajustes del juego».
- VPKTAA inline sin `QueueWaitIdle` (serial publicado al completar en GPU), core «pt-port v1» (algoritmo de PT,
  historial rgba16Float, sharp 1.0, disocc 0.05). Sin probar aún en un juego sin MSAA.
- Vídeo sin surface (buffers CPU NV12), overlays nunca escritos → limpieza única transparente, imágenes viejas
  liberadas a los 5 s, near = frustum.
- SDK Quest: Platform con errores «offline» tipados (cola 256), 485 `ovrp_*` explícitos (boundary virtual 3×3 m),
  OpenXR 105 funciones de extensión + 1.1. Ajuste «Diagnóstico de ficheros» (`vq.fsDiag` → `KL_TRACE_FS=1`) y log
  `[asset] … MISS`.

## Resumen logs 44–52 (detalle en git)
- VPKTAA: ruta async sustituida por INLINE (jitter aplicado por VPKEngine al traducir, `vpvk_native_submit`); en
  logs 50–52 inline funcionaba pero serializaba (Batman ~90→37 fps) → corregido hoy; ver arriba.
- AoT 44/47: cuelgue en teardown de vídeo (`Message.getTarget`/`Handler.getLooper`) y congelado por QueueSubmit sin
  sincronización externa (`g_qsub_mu`/`klvk_qsubmit`); vigilante `[snap]` (`KL_STALL_SNAP`).
- AC 45/48: usuario offline Ubisoft (`klplat_user_default` con paquete `com.ubisoft.`), idiomas `*.lang`,
  `klplat_called` sin abort (respuesta neutra), frontera del set de overlays = `ovrp_EndFrame4`.
- Pose EyeLevel (origin 0) restando la altura medida (`kl_ovrp_guest_origin_y`), Unity/UE4 en inmersión `.full`.

## Resumen logs 24–43 (detalle en git)
- VPKTAA (ruta async, ya sustituida por INLINE): core reutilizable `VPKTAACore.swift` (API en su cabecera: color
  2DArray, tangentes sin jitter, jitter, crop/cropOff, pose del ojo, depth `VPKTAADepthInput`, MV `VPKTAAMotionInput`);
  reproyección por profundidad, MV AppSW, desoclusión, clip YCoCg, RCAS fusionado, AA espacial; profundidad elegida
  en `kl_vulkan.c` (`klvk_ed_*`, prioridad pase-ojo > SpaceWarp > tamaño), reversed-Z por clear/censo; env
  `KL_VPKTAA_NEAR/FAR/REVZ/MV/MV_SCALE/MV_SIGN/MV_MAP/DEPTH_MAP/DISOCC`.
- SpaceWarp: flags 0x300 (AC/AoT) con `GetLayerTextureSpaceWarp`; MV size en el desc del ojo; Batman (0x1000) no la usa.
- Overlays/compositor: rect de textura y color por capa, `SetColorScaleAndOffset` (fundidos), origen abajo (bit 3),
  imagen propia de capa si el juego la escribe, OVROverlay con `VkImage*` en celda estable, vídeos 180° SBS por capa
  Equirect, skybox de carga solo con vídeo vivo, HUD FPS, capturas de diagnóstico, OpenXR color scale/bias.
- Sistema/JNI: Object.toString/hashCode/equals, NO-OP de métodos void del juego, excepciones JNI por hilo, pthread
  attr/sched, OpenSL volume/feeder, AAsset 64 bits, índice genérico de la carpeta del juego (obb), imágenes nuevas a
  negro, audio 40 ms, MSAA del juego configurable (`vq.gameMSAA`).
- Rendimiento Batman resuelto en log 26 (~90 fps; VPKEngine locales en slots por variable, `gpu.hold`).

## Resuelto en logs 8–22 (referencia breve; detalle en git)
- Vulkan en Batman: spoof Adreno 740 / driverVersion bit 31, `ovrp_GetDisplayAdapterId2` → NULL (log 7–8),
  `-force-vulkan` vía extra `unity` del Intent, AssetFile StatusByName como petición fallida.
- Sistema: StatFs, ELF TLS (TLSDESC), liblog completo, libdl namespaces, AAudio MMap, OpenSL PCM_EX float y
  GetConfiguration, AudioManager.getDevices/AudioDeviceInfo → audio OK desde log 17.
- Gráficos VPKEngine: point_size, SubpassData como Texture y D2Array con multiview, VkComponentMapping → swizzle
  (fuentes OK log 22), pipelines fuera del mutex + MTLBinaryArchive persistente.
- XR: zNear/zFar, Time de poses, RelativePose EyeLevel con altura medida por el Vision Pro, LateLatching off,
  postura auto/sentado/de pie, recentrado con PS derecho largo, PS izq = Start, toques capacitivos, manos ocultas,
  idioma del sistema (`Locale.preferredLanguages`), versión OVRPlugin leída del APK.
- Foveado Quest siempre apagado (ovrp, OpenXR FB/META, extensiones Vulkan FDM/VRS ocultas).
- Pendiente antiguo aún abierto: aborto final `mutex lock failed: Invalid argument` (log 22); Boundary «no configurado».

## Arquitectura (referencia)
- Base Klepton (MIT, copia `ce74582` en `klepton/`, ver `THIRD-PARTY-NOTICES.md`): cada `.so` ARM64 se envuelve en un
  dylib firmado (`tools/klepton_ld.c` en proceso, `klepton_ld_run`) y se carga con `dlopen`; relocs ELF propias;
  bionic/JNI/OVRPlugin/OpenXR propios. Sin traducción de CPU.
- Gráficos: VPKEngine VPVulkan.framework (`scripts/build-vpvulkan.sh`, cacheado por hash de `vpkengine/gpu/**`) en lugar de
  MoltenVK (`VQSettings.useVPKEngine`, `KL_MVK_DYLIB`). VisionQuestPro usa `gpu/metal2`.
- Firma: conversión sin firmar en `QUEST/Cache/<j>/dylibs`; copias firmadas en
  `Library/Application Support/VisionQuestPro/Signed/<j>/` (`VQPrepare`, `VQSigner`, variantes req4k/req16k/plain).
- App (`app/Sources/`): `VQLog`, `VQFolder` (QUEST: Juegos/Cache/Partidas), `VQGames`, `VQPrepare`, `VQLaunch`
  (+ `VQSettings`, todos los ajustes en la app), `VQLauncher`; `app/Native/vq_apk.c` (ZIP + AXML), `vq_config.c`
  (reenvío `vpvk_config_set_*`).
- CI `.github/workflows/visionos.yml`: solo `[build]` o manual; release `visionos` (`VisionQuestPro.ipa`, bundle
  `com.kdt.livecontainer`); logs de CI en rama `ci-logs` (`visionos.txt`).

## Batman: Arkham Shadow (com.camouflaj.manta, solo estudio)
- Unity 2022.3.45 IL2CPP, Vulkan multiview, SRP propio «Manta» (derivado de HDRP), OculusXRPlugin → libOVRPlugin
  1.98; Wwise (OpenSL); `supportedDevices=quest3+`. x18 usado como registro normal (Klepton lo parchea).
