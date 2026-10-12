# Avisos de terceros

## Klepton (MIT)
- Origen: https://github.com/shinyquagsire23/Klepton, commit `ce74582cb1e434db210014dbcd0ca0aad0e46ba2` (2026-09-03).
- Copia en `klepton/` con su licencia (`klepton/LICENSE`): `runtime/`, `Makefile`, `tools/klepton_ld.c`,
  `tools/mvk_fetch.sh`, `tools/mvk_retarget.sh`, `visionos/Sources`, `visionos/Info.plist`, `visionos/Klepton.entitlements`.
- Cambios propios (marcados con «VisionQuestPro» en el código):
  - `tools/klepton_ld.c`: modo en proceso (`-DKLEPTON_LD_INPROC`, `klepton_ld_run()`; los errores vuelven con
    `longjmp` en vez de `exit`) y un `LC_CODE_SIGNATURE` vacío al final de `__LINKEDIT`.
  - `visionos/Sources/kl_app.c/.h`: `kl_app_configure_game()` (juego elegido en tiempo de ejecución); traducciones
    `<lib>.dylib` además de `<lib>.framework`.
  - `visionos/Sources/KleptonApp.swift`: ventana raíz = lanzador de VisionQuestPro; configuración con las rutas del juego.
  - `visionos/Sources/Klepton-Bridging-Header.h`, `visionos/Info.plist`: cabeceras y claves del lanzador.

## VPEngine
- `runtime/vp_codesign.c`, `runtime/vp_pkcs12.c` (firma de librerías en el visor) se toman de
  el repositorio VPEngine (mismo propietario que este) al compilar (CI); no se copian a este repositorio. Licencia: la de VPEngine.
- `app/Sources/VQFolder.swift` y `app/Sources/VQSigner.swift` siguen el diseño de `VPS4Folder.swift` y `VPGamePack.swift`.

## MoltenVK (Apache-2.0)
- v1.4.2, descargado y verificado (sha256) en CI por `klepton/tools/mvk_fetch.sh`; se incluye en la app como
  `Frameworks/MoltenVK.framework`.
