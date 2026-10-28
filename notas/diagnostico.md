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
| `vk.msaa force=N passes= skip{…} twins= game_auto= skip_unreadable= rej{n,eye,cd,smp,res,desc}` | MSAA forzado (1/s, acumulado). `rej` = pasadas que `forceMsaa` deja a 1x sin `skip*`: n = forzado apagado (<2, cuenta todas), eye = no es pasada de escena del ojo (`taaEyePass`), cd = sin color 0 o sin depth, smp = MSAA propio del juego, res = resolve propio, desc = `textureDesc` falla |
| `vk.msaa rej <motivo> WxHxL color=<fmt>/<s>x depth=<fmt>/<s>x` | Pasada dejada a 1x (cualquier motivo, `skip*` y `rej*`; motivos: eye, cd, desc, smp, res, shape, input, fmt, fmt_size, load, area, scan_input, dsb, eds, scan_shape, samples). Solo (tamaño, capas, motivo) distintos: 12 de <1 Mpx y 12 de >=1 Mpx, para que las pasadas pequeñas no tapen la del ojo |
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
| `gles.uloc prog= name= ret=`, `gles.uniform_err … prog=<id>` | Uniformes GLES: localización de los empaquetados (1 vez por (prog,nombre), tope 1024) y glUniform* rechazado (1 vez por (prog,nombre,tipo llamado), tope 512; `prog=` al final de la línea) |
| `[sp-call] fn=ovrp_X n=k <args> ret=<código>` | Primeras 20 llamadas de cada ovrp_* espacial (anclas, consultas, componentes, malla/room/boundary, RequestSceneCapture, Discover) con argumentos clave y código devuelto; `req=` = requestId devuelto |
| `[sp-ev] push type= req= result= queued=` / `[sp-ev] polled via=PollEvent2\|PollEvent type= len= req=` | Cada evento encolado (50 por tipo; `queued=0` = cola llena) y cuándo el juego lo recoge en PollEvent; casar por `req=` |
| `[sp-call] fn=ovrp_QuerySpaces\|QuerySpaces2 q#k filterType= comps\|ids=N types=… excludeFilterType= exclude_n=` + `… q#k info64=<hex>` | Filtro realmente pedido (QuerySpaces: exclude=-1, no está en nuestro layout; QuerySpaces2: tipo/cuenta de Filter y Exclude, -1 = ausente) y volcado hex de los primeros 64 bytes del struct (QuerySpaces2: solo sizeof). Primeras 6 llamadas de cada función. `loc=1` = Local (ovrpSpaceStorageLocation: 0 Invalid, 1 Local, 2 Cloud) |
| `[pt] expire detail #N: layer= idle= ef4= last_enq_ef4= ago= last_enq_flags= calls_last3=` | Con `[pt] enqueued passthrough layer N expired` (máx. 8): frames idle, EndFrame4 del último re-encolado y llamadas ovrp_* del juego en los últimos 3 EndFrame4 (`nombre xN`) |
| `[diag] budget exhausted lines=2000` | Tope de líneas de `[sp-call]`/`[sp-ev]`/`[pt] expire detail` por sesión (compartido) |
| `[fatal] last log lines:` + 8 líneas | En `klepton-crash.log` y fd 2 tras `last guest errors`: las 8 últimas líneas de `klepton-boot.log` tal como están en disco (lo que siga en búfer stdio no aparece) |
| `libEOSSDK JNI_OnLoad -> …` | EOS de UE4 cargado |
| `[plat] game version string for language-pack metadata` | Packs de idioma (Deadpool) |
| `[path] … -> obb dir` | Redirección de `.lang` al obb |
| `guest props/limits`, `guest dlsym('vk…') -> NULL` | Trazas Vulkan del guest |
