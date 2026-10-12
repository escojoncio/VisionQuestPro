# Plan técnico — VisionQuestPro

## Base: Klepton (shinyquagsire23/Klepton, MIT)
Klepton ya resuelve la parte de CPU/sistema: `klepton-ld` (`tools/klepton_ld.c`) convierte cada `.so` en dylib firmable
con el código casi intacto, parchea x18 (slots TLS por librería), y aporta bionic, NDK, JNI sintético y `ovrp_*`
reimplementado; probado con Beat Saber, BONELAB, RE4 VR, Red Matter 2… Lo que aporta VisionQuestPro encima:
- Vulkan → **VPEngine** en lugar de MoltenVK (lector SPIR-V → VPIR + capa de API Vulkan).
- VPKTAA, foveado propio, resolución, ASW desactivable, perfil Quest 3.
- Launcher con `QUEST/Juegos` y conversión sin Mac (klepton-ld en el visor o en CI; firma propia de VPEngine).
Las secciones de abajo describen el mecanismo; donde Klepton ya lo implementa, se usa el de Klepton.

## Idea central
El código nativo de un juego de Quest ya es **ARM64**, la misma ISA del Vision Pro. No hay que traducir la CPU
(nada de FEX ni AOT x86): basta con **cargar** las `.so` Android y darles el entorno que esperan.

## Cómo se carga el código sin JIT
- Conversión (una vez, en el visor): cada `.so` ELF se envuelve en un Mach-O dylib **sin tocar sus bytes de código**:
  mismos segmentos con los mismos desplazamientos relativos (`__TEXT` = PT_LOAD ejecutable, `__DATA` = PT_LOAD RW).
  Se firma con la firma propia de VPEngine (`runtime/vp_codesign.c`) y se guarda en `QUEST/Cache/<juego>/`.
- Carga: `dlopen` del dylib firmado (camino ya aceptado por visionOS en VPEngine, variante `req4k`). Un cargador propio
  aplica las relocaciones ELF (`R_AARCH64_RELATIVE/GLOB_DAT/JUMP_SLOT/ABS64`, RELR) sobre las páginas de datos y
  resuelve los imports contra nuestras capas. El código nunca se escribe → no hace falta JIT.
- Relocaciones de texto: no existen en ARM64 PIC (se rechaza el juego con aviso si aparecen).

## Capas que hay que dar (superficie medida, no supuesta)
| Capa | Qué es | Notas |
|---|---|---|
| bionic | libc/libm/libdl/liblog de Android sobre Darwin | ABI distinta (`FILE`, `pthread_*` de tamaño distinto, errno, `__system_property_get`, TLS por TPIDR_EL0) |
| JNI | `JavaVM`/`JNIEnv` falsos | Unity y Unreal llaman a la actividad Java; se responden solo los métodos que use cada motor |
| OpenXR | runtime propio sobre CompositorServices + ARKit | cabeza, manos; mandos Quest → mandos PS VR2 Sense (`SenseTracking.swift`) |
| Vulkan | `libvulkan` propio → VPEngine (VPIR, emisor MSL, backend Metal) | SPIR-V → VPIR nuevo; sin MoltenVK |
| GLES | fuera de la primera versión | se avisa como no compatible |
| Audio | AAudio/OpenSL ES → AVAudioEngine espacial | |
| Ficheros | APK (zip) + OBB montados de solo lectura; guardados en `QUEST/` | |

## Riesgos (comprobar primero, en el visor)
1. `x18`: Darwin lo pone a cero en cambios de contexto. El NDK lo reserva en aarch64-linux-android → debería estar libre;
   el analizador lo verifica escaneando el código.
2. TLS de bionic en `TPIDR_EL0`: hay que confirmar que Darwin lo conserva por hilo para procesos de usuario.
3. Tamaño de página: Android 4 KB, Darwin 16 KB → segmentos no alineados a 16 KB obligan a reubicar el envoltorio
   (el código sigue intacto si es PIC; se valida en el analizador).

## Fases
1. **Analizador** (`tools/vqanalyze`, Linux + visor): APK/OBB, motor, API gráfica, variante Quest 3, alineación de
   segmentos, uso de x18, lista exacta de imports por librería → define la superficie de bionic/JNI/OpenXR/Vulkan.
2. Envoltorio ELF→Mach-O + cargador + bionic mínimo: el `JNI_OnLoad` del juego se ejecuta en el visor (prueba de vida).
3. JNI + OpenXR + bucle de frames con Vulkan nulo (sin dibujar): el juego llega a su bucle principal.
4. Vulkan → VPEngine: primer frame visible.
5. Ajustes (resolución, VPKTAA, foveado, ASW off, perfil Quest 3), audio, mandos.
