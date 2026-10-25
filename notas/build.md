# Compilar, probar y publicar

- **CI**: `.github/workflows/visionos.yml` (solo con `[build]` en el commit o manual): VPVulkan (caché por hash de `vpkengine/gpu/**`) → MoltenVK 1.4.2 → `tools/ci/syntax-xros.sh` → `tools/ci/host-tests.sh` → `libklepton.a` → app (`xcodebuild`) → `compositor-api.txt` → **Gate** → `scripts/ci-publish-log.sh` → release `visionos` (asset `VisionQuestPro.ipa`).
- **VPKEngine** se clona de `main` en CI (sparse `runtime` + `gpu`): pushear VPKEngine ANTES de lanzar la build.
- **Gate**: falla si `syntax-xros.sh` o `host-tests.sh` fallan (ASan/UBSan; `gpu_host` con `-k`; cada test con `alarm 240`). Con Gate rojo no hay IPA. macOS: `lgamma_r`/`lgammaf_r` con prototipos explícitos.
- **Tests de host**: `t_frame_fence`, `t_jni_sig`, `t_memdiag`, `t_media_announce`, `t_media_backpressure`, `t_ptw`, `kl_ovrp_eyesize_test`, `pred_frame_offset_test`, `t_ovrp_input`, `spatial_test`, y VPKEngine `gpu/` (cmake+ctest, `gpu/metal2` para xros).
- **Conformidad**: `conformance.yml` (Vulkan CTS 1.4.6.2 `rapida`, OpenXR CTS 1.1.58). Referencia 96,9 % de 6551 casos.
- **Local (Linux, sin Mac)**: `clang -fsyntax-only` con `XROS_CFLAGS` solo sirve para ficheros sin cabeceras Darwin; usar shim de Darwin y distinguir errores previos. Swift/.mm solo compilan en CI. `host-tests.sh` no corre completo aquí; tests sueltos con `make jnisig|memdiag|mediaannounce|eyesize|mediabp` y `CC=gcc`.
- **VPKEngine local**: `cmake -S gpu -B /tmp/vpbuild -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`; ctest 232/232.
- **Tras un fallo**: leer `visionos.txt` en `ci-logs`.
- **Identidad de commit**: `Claude <noreply@anthropic.com>`.
