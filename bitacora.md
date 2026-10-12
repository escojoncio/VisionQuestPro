# Bitácora — VisionQuestPro

Historial completo de sesiones anteriores: `git log -p bitacora.md` (compactada el 2026-10-12 tras el log 25).

## Batman log 25 (2026-10-12) → build siguiente
- **Rendimiento**: meseta de 38–41 ms de GPU por frame de render (~22 fps de juego, ~95 en zonas ligeras) según hacia
  dónde se mira; pases idénticos en frames rápidos y lentos. Causa en VPEngine: todos los locales SPIR-V en un único
  arena `vp_private` → con un índice dinámico todo el shader va a la pila. Arreglo en VPEngine (slots por variable,
  ver su bitácora 2026-10-12 «log 25»). `app/Sources/VQLaunch.swift` `firstRunVersion = 2` (re-precompila con el
  MSL nuevo); VPEngine `kRecipeVersion = 2` (descarta recetas viejas: la 1.ª partida tras actualizar compila PSO).
- **Cuadrado rosa al empezar partida** (no era un vídeo roto): capa Quad 2048x128 de OVROverlay; `klovrp_GetLayerTexturePtr`
  daba el VkImage crudo y Unity (CreateExternalTexture en Vulkan espera `VkImage*`) lo desreferenciaba → handle
  salvaje 0xe → pintaba en el scratch. `klepton/runtime/xr/kl_ovrp.c`: con guest Vulkan devuelve la dirección de una
  celda estable por (capa, stage, ojo) (`klovrp_texcell`, 128 celdas, nunca se liberan); GLES sin cambios;
  `KL_OVRP_TEXPTR_RAW=1` vuelve al valor crudo. Log: `GetLayerTexturePtr(...) wrote <ptr> (VkImage … via cell)`;
  debe desaparecer `SUBSTITUTING scratch for wild handle`.
- **Vídeos**: en el log 25 el juego no tocó ningún .mp4 (ni stat, ni JNI de vídeo, ni AMedia*, ni capa Equirect).
  Sonda `kl_libc.c` `kl_media_probe_log` ampliada: `strcasestr` de .mp4/.m4v/.mov/.webm/.mkv en cualquier parte de la
  ruta, mutex, y llamada también desde `kl_fs_trace_open` (open/openat/__open_2) y `klb_fopen`.
- Ruido: `klovrp_Update3` dejaba `calls` de Update2 en 0 cada frame → 11k líneas `first-call ovrp_Update2`; ahora no
  baja de 1.
- Diagnóstico nuevo (VPEngine): `gpu.shader.mem … dyn_slots=` por shader y `gpu.time … enc_ms=` en frames lentos.
### Comprobar en el log 26
- `gpu.time` de la zona pesada (debería bajar de ~39 ms); si sigue alto: `enc_ms` (qué pase) y `gpu.shader.mem` con
  `dyn_slots>0`.
- Sin `wild handle 0xe`; el cuadrado del inicio muestra contenido (indicador de carga / texto).
- `[video] …` si el juego busca algún vídeo; sin `first-call ovrp_Update2` repetido; sin `archive_save` fallido.
- Pendiente si hace falta: perfilador automático de frames pesados (bisección por draws), tablas constantes en
  `constant`, mprobe de escena/objetos de vídeo (`kl_mprobe.c`).

## Batman log 24 (2026-10-12)
- Vídeo (descompilado con resolver IL2CPP propio): StereoVideoPlayer.Play @0x1da9d34 → path =
  Path.Combine(getObbDir, VideoFileName) → si !File.Exists vuelve false. PlayEnumerator espera Wwise
  AllSoundBanksLoaded → AndroidVideoPlayer.Play → OVROverlay Equirect (shape 5) isExternalSurface →
  GetLayerAndroidSurfaceObject → NativeVideoPlayer. Vídeos sueltos en OBB (DRM1..7, GothamToBlackgate): HEVC 8192x4096
  60 fps SBS equirect 180°; reproductor host `kl_hostvideo.c` + `kl_jni_hostvideo.c` + `HostVideoPlayer` (AVPlayer) en
  `KleptonCompositor.swift`. `OBBVideoWrapper` usa `/storage/emulated/0/Android/obb/<pkg>/X.mp4` → remap en
  `kl_libc.c` `kl_guest_path_inner`.
- VPKTAA (`KleptonTAA.swift`): rejilla sin jitter Blackman-Harris, Catmull-Rom, clip YCoCg, RCAS; jitter Halton en
  `klovrp_GetNodeFrustum2` si el resolve corre; por defecto OFF (`vq.vpktaa2`).
- Ajustes: aniso por nivel (`vq.anisoLevel`, def. 16), `VP_PASS_TIMING` («Diagnóstico de rendimiento»), resolución base
  original/compositor/nativa (`vq.resBase`), MSAA del juego a 1x con VPKTAA (`dropGameAA`).
- Shaders 1.ª vez: `app/Native/vq_harvest.c` (UnityFS + LZ4, ZIP, SPIR-V por magia) → `<cache>/spirv/*.spv` →
  VPEngine `vpvk_precompile_spirv` + `msl_index.bin`; stamp `firstrun.vN`.
- Passthrough automático: `kl_passthrough.c/.h`, ajuste Auto/VR/Mixto (`KL_PASSTHROUGH`).

## Resuelto en logs 8–22 (referencia breve; detalle en git)
- Vulkan en Batman: spoof Adreno 740 / driverVersion bit 31, `ovrp_GetDisplayAdapterId2` → NULL (log 7–8),
  `-force-vulkan` vía extra `unity` del Intent, AssetFile StatusByName como petición fallida.
- Sistema: StatFs, ELF TLS (TLSDESC), liblog completo, libdl namespaces, AAudio MMap, OpenSL PCM_EX float y
  GetConfiguration, AudioManager.getDevices/AudioDeviceInfo → audio OK desde log 17.
- Gráficos VPEngine: point_size, SubpassData como Texture y D2Array con multiview, VkComponentMapping → swizzle
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
- Gráficos: VPEngine VPVulkan.framework (`scripts/build-vpvulkan.sh`, cacheado por hash de `vpengine/gpu/**`) en lugar de
  MoltenVK (`VQSettings.useVPEngine`, `KL_MVK_DYLIB`). VisionQuestPro usa `gpu/metal2`.
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
