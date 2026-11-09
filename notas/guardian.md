# Guardian (kl_guardian + Swift)
Reglas copiadas como COMPORTAMIENTO del Quest 3 (firmware solo como referencia offline, sin código de Meta).
- Sala válida V1: ≥3 paredes exteriores, ≥1 suelo, ≥1 techo sobre el suelo; anillo cerrado tolerancia 0,05 m. Avisos de tamaño 8 m, máx. 15 m.
- Zona de juego: rectángulo orientado inscrito ≥1,15×1,15 m (límite inclusivo, tolerancia de una celda), ≤4096 puntos (submuestreo uniforme), coords |x|≤1e4.
- Suelo: plausibilidad cabeza−2,5…−0,7 (SLAM); stick 6 mm/frame ±1 m del mando; nunca <0,1524 m bajo la cabeza; |y|≤10; AUTO (1) con histéresis 1 cm y no pisa MANUAL (2).
- C: `klepton/runtime/xr/kl_guardian.[ch]` (+kl_ovrp.c boundary/seat, kl_spatial.c `kl_spatial_update_guardian_default`, kl_openxr.c origen STAGE/LOCAL_FLOOR + `REFERENCE_SPACE_CHANGE_PENDING` si el suelo cambia >1 cm). Tests: t_guardian, t_room_build, t_seat_offset.
- Swift: `KleptonGuardian.swift` (KLGuardian, KLFloorEstimator, KLGuardianLive), `app/Sources/VQGuardianSetup.swift` (puerta VQGuardianGate antes de `kl_app_guest_start`, timeout 10 s), `KleptonSpatial.swift` (providers).
- Claves: `vq.envCapture`, `vq.guardianAsk`, `vq.guardian.<aaKey>.{floor,floorSource,kind,mode,w,d,radius,enabled}`, `vq.d24emul.<aaKey>`.
- Sin verificar: cableado kl_ovrp.c/kl_openxr.c y todo el Swift (solo CI); espacios guest vs mundo tras recenter; sentido de giro de las esquinas PlayArea.
