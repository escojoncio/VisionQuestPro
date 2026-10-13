# VisionQuestPro

App para Apple Vision Pro que ejecuta juegos de Quest a partir de sus ficheros descomprimidos
(APK + carpeta de datos), sin necesitar un Quest. Gráficos con VPKEngine (escojoncio/VPKEngine) y
antialiasing VPKTAA (escojoncio/PTVisionPro).

## Flujo
1. El usuario copia la carpeta de un juego en `QUEST/Juegos` (carpeta elegida con el selector del
   sistema, fuera de la app: no se pierde al reinstalar; mismo concepto que VPS4 en AstroVisionPro).
2. La app detecta los juegos de `QUEST/Juegos`.
3. La primera vez prepara el juego en el propio visor y guarda el resultado en `QUEST/` (caché
   reutilizable, no se repite).
4. El usuario elige juego y juega.

## Launcher
- Tarjeta por juego: nombre, versión, motor (Unity/Unreal/…), API gráfica (Vulkan/GLES), si trae
  variante Quest 3.
- Aviso claro si un juego no es compatible y por qué.
- Registros compartibles desde la app; todo el diagnóstico va al log (no hay Mac para depurar).
- Base visual y estructura: launcher de AstroVisionPro.

## Ajustes (todos dentro de la app, nunca en ficheros de texto)
- Resolución de render.
- VPKTAA en lugar del AA del juego (porcentaje, nitidez).
- Perfil Quest 3 cuando el juego lo tenga.
- ASW desactivable: todos los frames reales, sin extrapolar.
- Foveado: zona nítida, periferia, presets M2 / M5 / personalizado (como PTVisionPro; M5 solo
  seleccionable en un Vision Pro M5).

## Gráficos
- Traductor propio Vulkan/SPIR-V → Metal dentro de VPKEngine, reutilizando su IR (VPIR), emisor MSL,
  backend Metal y grafo de pases. Código 100 % propio (sin MoltenVK, SPIRV-Cross ni Mesa).
- Se quita el MSAA del juego y se aplica VPKTAA.
- Si el juego genera vectores de movimiento (juegos con SpaceWarp), VPKTAA los usa para no dejar
  estelas en objetos en movimiento.
- El foveado del juego se conecta con el foveado propio.

## Alcance
- Solo juego sin conexión: sin cuentas ni servicios en línea de ningún tipo.
- Partidas guardadas en `QUEST/`.
- Pruebas solo con juegos propios del usuario.
