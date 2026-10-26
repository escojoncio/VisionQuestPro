# Diagnóstico: etiquetas del log

Logs en `QUEST/Registros/` (compartibles desde la app); línea 1 = hash de build. Log de CI: rama `ci-logs`, fichero `visionos.txt` (`git fetch origin ci-logs` o API `repos/<owner>/<repo>/contents/visionos.txt?ref=ci-logs`; primero errores, luego últimas 150 líneas). `gh run view --log` da 403. `compositor-api.txt` va embebido en `visionos.txt`.

| Etiqueta | Significado |
|---|---|
| `[hb]`, `[hb] jump +N MiB`, `[hb] blocked`, `[hb] gpu … ledger:` | Heartbeat con memoria (ledger gráfico/media/net/purg, fp_peak, lim_rem); salto >200 MiB/tick; hilo bloqueado; ledger de VPKEngine |
| `[hbm]`, `[vkagg]`, `[mtl]` | Cadencia rápida tras salto: regiones, agregado `vkCreateImage`, estado Metal |
| `[hb\|mem] gpu.attr total= tracked= (vpk= kl=) untracked=`, `gpu.bigalloc`, `[mtl] bigalloc` | Con cada `[hb] gpu` y en los avisos `[mem]`: `currentAllocatedSize` frente a los objetos Metal vivos por sitio de creación (`kl_top`/`vpk_top` = tag:MiB/vivos, `*_obj` = PSO/librerías con crecimiento medido al crearlas `~`, acumulado y aproximado, `*_cnt` = vistas/funciones, no suman); `untracked` = lo que ningún sitio etiquetado explica; `devs>1` = más de un MTLDevice. Toda asignación individual ≥32 MiB (máx. 200) se avisa al crearse con tag, tamaño, dims, formato, storage y usage |
| `[mtl] cut_enc` / `chunk_wait` | Corte de encoders por presupuesto / contrapresión |
| `[mem] threshold`, `[mem] OS pressure`, `[mem] trim (…)`, `[mem] low memory (<why>): told the guest` | Aviso 65/80 %, presión del SO, recorte, entrega al guest (Unity `nativeLowMemory` o NativeActivity `onLowMemory`) |
| `[hb\|mem] vm regions=N groups=G total res= dirty= swap=; MiB res/dirty/swap: TAG/kind=r/d/s … rest(K)=r/d/s` | Cuenta cerrada de la memoria de CPU: mapa de regiones (`vm_region_recurse_64`) agrupado por `user_tag` y tipo (`file` = mapeo a fichero; `priv`/`cow`/`shared`/`tshared`/`palias`/`salias`/`lpage`/`empty` = share mode anónimo), en MiB: residente / sucio (`pages_dirtied`) / comprimido (`pages_swapped_out`); top 12 por residente+comprimido y `rest` con el resto. Con cada `[hb] gpu.attr` (16 s), en `[hb] jump`/`blocked` y en los avisos `[mem]`; máx. 1 cada 2 s; `KL_MEM_REGIONS_MAX` (60000) corta el recorrido (`PARTIAL`, 30 s sin repetir). `dirty` es por objeto VM y puede contar dos veces lo compartido: explica `phys_footprint`, no lo suma al byte. Tags: `guest_mmap` = mmap anónimo del guest (`klb_mmap`, tag 240 del rango de aplicación); el malloc del guest es el del host (`MALLOC_*`, `KL_FWD(malloc)`), no se distingue del nuestro; `IOAccelerator`/`IOSurface`/`IOKit`/`CG*` = gráficos; `dyld`/`shared_pmap`/`mapped_file` = imágenes y ficheros; `untagged`, `tagN` = sin nombre |
| `vk.msaa_tile memoryless image=… usage=…`, `msaa_materialized why=`, `msaa_released`, `tile_rejected>0` | MSAA/depth en tile; materializado (y por qué); liberado tras 120 frames; MSAA 4x no cabe |
| `gpu.bigframe`, `gpu.enc ls=`, `gpu.stall_warn`, `gpu.cbtime`, `gpu.sec`, `gpu.slowcb`, `gpu.hang`, `gpu lost` | VPKEngine: frame anómalo, encoders por frame, GPU sin progreso, tiempos; GPU lost → ojo negro y cierre |
| `vk.boot_summary`, `vk.pipeline_skip`, `vk.pipeline_wait what=…`, `vk.eye_image`, `skipped_by_dest{…}` | Arranque VPVulkan; pipelines async omitidos/esperados; marca de ojo |
| `vk.zeroinit`, `gpu.shader.msl cached=`, `gpu.prewarm`, `vk.taa native gpu_ms`, `vk.caps`, `vk.err` | Otros de VPKEngine |
| `[frame-end] mode=…`, `[frame-end] fence wait …`, `[ovrp] frame end waited` | Fin de frame |
| `[cp] gpu p50/p95/max … | pass frag/vert …`, `[cp] slack`, `[cp] slow frame` | Coste y plazo del compositor |
| `[cp] stale -> backfill`, `[cp] fresh again` | Frame congelado >3 s / recuperación |
| `[pt] immersion style requested`, `[pt] mode auto`, `[pt] passthrough ON`, `[pt] xrEndFrame: environment blend mode …` | Inmersión y passthrough |
| `[cp] HALF RATE`, `minimumFrameRepeatCount` | Medio ritmo impuesto |
| `[cp] layer paused/invalidated`, `[cp] backfill`, `eye stage N …`, `[zero] …`, `[prov] layer N … no write seen`, `[prov] overlay layer N: last 600 …` | Ciclo de capas y procedencia |
| `[cp] RCAS fused: UI mask …`, `[cp] composite pipeline failed` | Máscara UI / fallo MSL |
| `[taa-inline] … output ring rebuilt`, `no free output` | Anillo TAA (si `no free output`, subir a 5) |
| `[ovrp] eye desc … for a final`, `plugin widening`, `[ovrp] eye layer widened`, `[xr] swapchain … x the recommended` | Resolución y ensanche |
| `[media] take_rgba #N`, `[media] surface codec: INFO_OUTPUT_FORMAT_CHANGED`, `[media] vstat` | Vídeo |
| `[vtrace tid=…]`, `NO-OP game Java void`, `[jni] klj_as_object` | JNI |
| `== klepton-crash: build <commit>, pid, session start ==` | Cabecera del crash |
| `[fatal] last guest errors:` + hasta 32 líneas | Por qué murió el guest: anillo de las últimas líneas ERROR/FATAL de logcat (cualquier tag) y de las de tag `UE4`/`UE`/`Unity` con `Fatal`, `Assertion failed` o `Error:`, más `__android_log_assert` y `android_set_abort_message`. Se vuelca en el log y en `klepton-crash.log`: en cada señal fatal (abort/assert del guest acaban en SIGABRT) y, una vez por sesión, en `AndroidThunkJava_ForceQuit` (que se ignora; abre `klepton-crash.log` aunque no haya crash) |
| `[snap] game thread silent` | Hilo del juego sin actividad a 10/30 s |
| `[cp] haptics: …`, `FIRST REAL PULSE` | Háptica (`KL_HAPTICS_LOCALITY`, `KL_HAPTICS_FLOOR`, `KL_HAPTICS_MIN`) |
| `gles.diag`/`gles.warn`, `[gles] ruta:`, `[egl] absent GL function called` | GLES |
| `[magenta]` | Shader de error de Unity (siempre calienta uno) |
| `[ctrl] vel hand`, `[ctrl] vel swing`, `[ctrl] menu transition`, `[mprobe fN]`, `[xr-ctrl]` | Mandos |
| `[unity-kw]` | Logcat de estado de partida |
| `[ovrp] layer N blend:` | Blend por capa |
| `libEOSSDK JNI_OnLoad -> …` | EOS de UE4 cargado |
| `[plat] game version string for language-pack metadata` | Packs de idioma (Deadpool) |
| `[path] … -> obb dir` | Redirección de `.lang` al obb |
| `guest props/limits`, `guest dlsym('vk…') -> NULL` | Trazas Vulkan del guest |
