# Guardian (kl_guardian + Swift)
Reglas copiadas como COMPORTAMIENTO del Quest 3 (firmware solo como referencia offline, sin código de Meta).
- Sala válida V1: ≥3 paredes exteriores, ≥1 suelo, ≥1 techo sobre el suelo; anillo cerrado tolerancia 0,05 m. Avisos de tamaño 8 m, máx. 15 m.
- Zona de juego: rectángulo orientado inscrito ≥1,15×1,15 m (límite inclusivo, tolerancia de una celda), ≤4096 puntos (submuestreo uniforme), coords |x|≤1e4.
- Suelo: plausibilidad cabeza−2,5…−0,7 (SLAM); stick 6 mm/frame ±1 m del mando; nunca <0,1524 m bajo la cabeza; |y|≤10; AUTO (1) con histéresis 1 cm y no pisa MANUAL (2).
- C: `klepton/runtime/xr/kl_guardian.[ch]` (+kl_ovrp.c boundary/seat, kl_spatial.c `kl_spatial_update_guardian_default`, kl_openxr.c origen STAGE/LOCAL_FLOOR + `REFERENCE_SPACE_CHANGE_PENDING` si el suelo cambia >1 cm). Tests: t_guardian, t_room_build, t_seat_offset.
- Swift: `KleptonGuardian.swift` (KLGuardian, KLFloorEstimator, KLGuardianLive), `app/Sources/VQGuardianSetup.swift` (puerta VQGuardianGate antes de `kl_app_guest_start`, timeout 10 s), `KleptonSpatial.swift` (providers).
- Claves: `vq.envCapture`, `vq.guardianAsk`, `vq.guardian.<aaKey>.{floor,floorSource,kind,mode,w,d,radius,enabled}`, `vq.d24emul.<aaKey>`.
- v2 (2026-11-09): passthrough forzado (`kl_passthrough_force`), malla+rejilla (`KleptonGuardianDraw.swift`, solo con `KLGuardianScene.active`), suelo = agarrar rejilla con grip/pellizco (`kl_guardian_floor_grab`), log `[floor]`; sin slider/5 s/stick. Malla viva: `kl_spatial_live_update` (opt-in `KL_SPATIAL_LIVE_NOTIFY=1` para eventos 300/301). Orden de planos FLOOR,CEILING,WALL,resto,OTHER.
- Sin verificar: cableado kl_ovrp.c/kl_openxr.c y todo el Swift (solo CI); espacios guest vs mundo tras recenter; sentido de giro de las esquinas PlayArea.

- v3 (2026-11-09): zona DIBUJADA (kl_guardian_zone_*), Estacionario por defecto si no se dibuja; prioridad de sala real > dibujada > outline > fallback (kl_spatial.c); getters ovrp vivos; sin persistir polígono entre sesiones.
