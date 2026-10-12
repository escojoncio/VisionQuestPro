# VisionQuestPro

App para Apple Vision Pro que ejecuta juegos de Quest a partir de sus ficheros (APK + carpeta de datos), sin un Quest.

## Uso
1. Instala `VisionQuestPro.ipa` (release `visionos`) con tu herramienta de sideload.
2. En la app Archivos, crea la carpeta `QUEST` en «En mi Apple Vision Pro». Dentro, en `QUEST/Juegos/<juego>/`, pon el
   `.apk` del juego y su carpeta de datos (OBB).
3. Abre la app, elige la carpeta `QUEST` e importa en Ajustes el certificado `.p12` con el que se firmó la app.
4. Pulsa «Preparar y jugar»: la primera vez el juego se convierte y se firma en el visor y se guarda en
   `QUEST/Cache/<juego>/`; las siguientes arranca directamente.

Estado: v1 = juegos Unity con Vulkan (MoltenVK). OpenGL ES y Unreal aún no.

## Créditos
Escrito por Claude sobre el gran trabajo de [Klepton](https://github.com/shinyquagsire23/Klepton), VPEngine y MoltenVK
(ver `THIRD-PARTY-NOTICES.md`); el dueño del repositorio solo aportó ideas sobre cómo adaptar algunas cosas.
