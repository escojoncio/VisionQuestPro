# Captura y reproducción de frames (VPCAP)

## En el visor
- Ajuste por juego «Captura de frames (diagnóstico)» → `VPVK_CAPTURE=1` (tracking desde vkCreateInstance; más RAM),
  `VPVK_CAPTURE_DIR=<QUEST>/capturas/<game.id>/`, `KL_DIAGCAP=1`, `KL_DIAGCAP_BTN`, `KL_DIAGCAP_HOLD_MS`.
- Disparo: pulsación larga (stick izq., stick der., ambos sticks o ambos gatillos/pellizcos; 1–10 s) → `kl_diagcap.c`
  → `vpvk_capture_request(1, NULL)` por dlsym; háptico corto/fuerte = en cola, largo/suave = rechazado. Log `[cap] requested rc=`.
- Unidad de frame: `vpvk_frame_boundary` (llamada por `kl_frame_boundary` en EndFrame4/EndFrame/xrEndFrame) ⇒ `frame_unit=host`.
- Fichero `.vpcap`: STATE (objetos vivos del device capturado) + FRAME (llamadas, `vpcapMemoryData` por páginas 4 KiB,
  `vpcapImageData` en primer uso leído de Metal, tope `VPVK_CAPTURE_IMG_BUDGET_MB`=512/submit). END: `invalid=`/`reason=`,
  `unhooked_used`, `frame_unit`. Logs `vk.capture start/images/done`.
- Compartir: «Incluir capturas (.vpcap)» en Ajustes > Registros (pueden pesar cientos de MB).

## Fuera del visor (VPKEngine)
- `vpcap_dump f.vpcap [-l]`: cabecera, META/END y totales por llamada.
- `vpreplay f.vpcap --icd vpvk|system [--dump DIR --dump-at pass|draw] [--strict]`: reproduce; PNG por pase/draw + `index.json`.
  Con `--icd vpvk` desactiva TAA/RCAS/foveado/aniso del driver.
- `vpimgdiff A B [--min-psnr 40]`: primer dump divergente (`FIRST DIVERGENCE`, llamada y pipeline).
- `tools/replay/compare.sh f.vpcap`: lavapipe vs vpvk + diff + `report.txt`.
- CI: `gpu.yml` (refcaps de test, lavapipe Linux vs Metal en simulador visionOS) y `replay.yml` (workflow_dispatch con
  `url` https o `path` en rama `captures`).

## Límites conocidos
- Llamadas sin hook usadas ⇒ captura `invalid`; vkCmdPushDescriptorSet no existe en el driver.
- D24S8 depth y MSAA no se leen (`Unsupported`); memoryless/transient no se capturan.
- Sin verificar en Mac ni en visor (2026-10-26).
