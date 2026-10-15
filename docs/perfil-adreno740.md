# Perfil Adreno 740 (Quest 3) vs VPKEngine / Klepton — 2026-10-15

## 0. Fuentes y fiabilidad (leer primero)
No se pudo descargar ningún vulkaninfo real de un Quest 3 desde este entorno:
- vulkan.gpuinfo.org: WebFetch bloqueado por robots.txt; curl → proxy 403; web.archive.org bloqueado.
  Reports Adreno 740 localizados por buscador (abrirlos desde el navegador / la app y exportar JSON):
  `displayreport.php?id=38321` (Adreno (TM) 740), `id=49267` (Galaxy Tab S9 SM-X710), `id=40499` (HONOR).
  Ninguno confirmado como Quest 3 (`eureka`). Buscar en `listdevices.php?platform=android` "Oculus Quest 3"/"eureka".
- `gh search code` → 403 (sesión limitada a repos propios).

Fiabilidad usada en la tabla:
- **[A]** verificado: perfil Khronos `VP_ANDROID_baseline_2022` (Vulkan-Profiles, mínimo que todo Android con Vulkan
  conforme cumple; el Quest 3 lo cumple) y Godot #101635 (Adreno 740 = Vulkan **1.3.128** en Tab S9, driver Qualcomm).
- **[B]** conocimiento previo del driver propietario Adreno 6xx/7xx (valores típicos de gpuinfo); probable, NO verificado
  para el firmware de Meta.
- **[C]** dudoso; confirmar con el vulkaninfo real antes de tocar.

**Acción imprescindible:** obtener el JSON de gpuinfo de un Quest 3 (o `vulkaninfo --json` vía adb de cualquier
usuario) y meterlo en `vpkengine/gpu/vulkan/docs/adreno740.json`; idealmente generar `physical_device.cpp` desde él.

## 1. Dónde se declara hoy
| Qué | Fichero:línea |
|---|---|
| Props/identidad/límites | `vpkengine/gpu/vulkan/src/physical_device.cpp:13-136` (`makeProps`) |
| Memoria | `physical_device.cpp:138-156` (`makeMem`) |
| Formatos | `physical_device.cpp:158-279` (`kFormats`, `formatFeatures`) |
| Extensiones instancia / dispositivo | `physical_device.cpp:284-317` (extras tras `extraExtensions()` = VPVK_EXTRA_EXTENSIONS, `ext.cpp:22`) |
| Properties2 pNext | `entry.cpp:225-296` |
| Features / Features2 | `entry.cpp:300-397` |
| Colas | `entry.cpp:~399-408` |
| Klepton: identidad (solo MoltenVK/batman) | `visionquestpro/klepton/runtime/gfx/kl_vulkan.c:6058-6120` (`klvk_spoof_gpu`, `klvk_spoof_props`, driverVersion 512.744.0) |
| Klepton: máscara de muestras | `kl_vulkan.c:6087-6101` (`KL_VK_MAX_SAMPLES`, AC Nexus=2) |
| Klepton: features | `kl_vulkan.c:6208-6243` (`klvk_patch_features`: vertexPipelineStoresAndAtomics=1, `KL_VK_IMG_NOFMT`) |
| Klepton: ASTC formatos (olar/wanderer) | `kl_vulkan.c:6269-6300` |
| Klepton: oculta FDM/shading rate | `kl_vulkan.c:6314-` |

Leyenda Estado: **=** igual · **MÁS** VPKEngine declara de más (riesgo: lo usa y falla) · **MENOS** declara de menos
(el juego descarta rutas) · **NOCUMPLE** declarado pero no implementado de verdad.

## 2. Identidad / API — Prioridad P0
| Campo | Adreno 740 (Quest 3) | VPKEngine | Estado | Cambio |
|---|---|---|---|---|
| apiVersion | 1.3.x [A: 1.3.128 Tab S9; Quest 3 podría ser 1.1/1.3, C] | 1.1 (`physical_device.cpp:15`) | MENOS | Unity 2022+/UE5 activan rutas 1.2/1.3 (dynamic rendering, sync2). Subir solo cuando Features 1.2/1.3 estén cubiertos. |
| driverVersion | 512.7xx con bit31 [B] (Klepton usa 512.744.0) | 0.1.0 (`:16`) | MENOS (**crítico**) | Unity rechaza Qualcomm sin bit31 → "hardware requirements" (ya documentado en `kl_vulkan.c:6107-6112`). Copiar `VK_MAKE_VERSION(512,744,0)` a `physical_device.cpp:16`. |
| vendorID/deviceID/name | 0x5143 / 0x43050a01 [C] / "Adreno (TM) 740" | 0x5143 / 0x43051401 / igual | = (deviceID C) | Confirmar deviceID real. |
| VkPhysicalDeviceDriverProperties (driverID QUALCOMM_PROPRIETARY, "Qualcomm Technologies Inc. Adreno Vulkan Driver") | sí [A: VK_KHR_driver_properties obligatorio Android] | no responde (`entry.cpp` switch sin caso) | MENOS | Añadir caso en `entry.cpp:~290`. |
| Colas | 1 familia G/C/T, queueCount 3 [B], timestampValidBits 64 | 1 familia, 1 cola | MENOS (bajo) | — |

## 3. Features 1.0 (`entry.cpp:300-347`) — P0/P1
| Feature | Adreno 740 | VPKEngine | Estado | Nota |
|---|---|---|---|---|
| robustBufferAccess, fullDrawIndexUint32, imageCubeArray, independentBlend, sampleRateShading, depthBiasClamp, fragmentStoresAndAtomics, largePoints, ETC2, ASTC_LDR, occlusionQueryPrecise, *ArrayDynamicIndexing (sampled/uniform/storage buf) | 1 [A] | 1 | = | |
| **vertexPipelineStoresAndAtomics** | 1 [B] | 0 salvo CTS (`:325`) | MENOS (P0, rosa Unity) | Klepton lo fuerza a 1 (`kl_vulkan.c:6210`). Hacer 1 fijo en VPKEngine y validar con CTS `dEQP-VK.*vertex*store*`. |
| **shaderInt16** | 1 [A, baseline] | 0 (no puesto) | MENOS (P0) | Obligatorio Android baseline. Unity/UE con half/min16 lo consultan. MSL tiene `short`: implementable. |
| shaderStorageImageArrayDynamicIndexing | 1 [A] | 0 | MENOS (P1) | MSL lo soporta (array de texturas). |
| geometryShader / tessellationShader | 1 [B] | 0; 1 con VPVK_REPORT_GEOM | NOCUMPLE si =1 | Metal no tiene GS; teselación sí (post-tess vertex). Mantener el puente actual + `vk.err what=stage`. |
| multiViewport | 1 [B], maxViewports 16 | 0 / maxViewports 1 | MENOS (P2) | Metal soporta 16 viewports (`[[viewport_array_index]]`). |
| shaderStorageImageReadWithoutFormat / WriteWithoutFormat | Write=1, Read=0? [C] | 0 (Klepton `KL_VK_IMG_NOFMT`) | C | Confirmar. |
| textureCompressionBC | **0** [B: Adreno móvil no tiene BC] | 1 (`:318`) | **MÁS** (P1) | Un juego con rama BC (UE escoge formato por feature) cogería texturas no presentes en el paquete ASTC/ETC. Poner 0 para igualar (los APK de Quest solo traen ASTC/ETC). |
| dualSrcBlend | 1 [B] | 1 | = | |
| depthClamp | 1 [B] | 1 | = | |
| fillModeNonSolid | 1 [B] | 1 | = | |
| shaderClipDistance 1 / shaderCullDistance 1 [B] | | clip 1, cull 0 (maxCullDistances 0) | MENOS (P3) | |
| shaderFloat64 / shaderInt64 | 0 / 1? [C] | 0 / 0 | C | |
| shaderImageGatherExtended | 1 [B] | 1 | = | |
| shaderStorageImageExtendedFormats | 1 [B] | 1 | = | |
| shaderStorageImageMultisample | 0 [B] | 0 | = | |
| logicOp | 1 [B] | 0 | MENOS (P3) | |
| depthBounds | 0 [B] | 0 | = | |
| pipelineStatisticsQuery | 0 [B] | 0 | = | |
| inheritedQueries | 1 [B] | 0 | MENOS (P3) | |
| shaderResourceMinLod / ResourceResidency / sparse* | 0 [B] | 0 | = | |

## 4. Features2 / 1.1 / 1.2 / 1.3 (`entry.cpp:349-397`) — P0
VPKEngine **no responde** a `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1/1_2/1_3_FEATURES` (quedan en 0). Con apiVersion 1.1
es legal, pero al subir a 1.3 hay que rellenarlos.
| Feature | Adreno 740 | VPKEngine | Estado |
|---|---|---|---|
| multiview | 1 [A] | 1 | = |
| **shaderDrawParameters** | 1 [A, baseline] | 0 (`:387`, no lowering) | MENOS + incumple baseline Android (P0). Unity multiview usa `gl_BaseInstance`/DrawID en algunas rutas; implementar en `spirv lower.cpp` (Metal tiene `[[base_instance]]`, `[[base_vertex]]`). |
| **samplerYcbcrConversion** | 1 [A, baseline] | 0 (`:389`) | MENOS (P1: vídeo en texturas Android/AHardwareBuffer) |
| **variablePointers / variablePointersStorageBuffer** | 1 [A, baseline] | 0 (`:383`) | MENOS (P2) |
| storageBuffer16BitAccess / uniformAndStorageBuffer16BitAccess / storagePushConstant16 / storageInputOutput16 | 1/1/0/1? [B/C] | 0 (`:379`) | MENOS (P1, UE5 mobile) |
| protectedMemory | 0 [B] | 0 | = |
| timelineSemaphore, synchronization2, dynamicRendering, separateDepthStencilLayouts, scalarBlockLayout, uniformBufferStandardLayout, multiDraw | 1 [B] (driver 1.3) | solo con VPVK_EXTRA_EXTENSIONS | MENOS hasta validar CTS |
| descriptorIndexing (+ UpdateAfterBind, partiallyBound, runtimeDescriptorArray) | 1 [B] | 0 | MENOS (P2, UE5 bindless opcional) |
| bufferDeviceAddress | 1 [B] | 0 | MENOS (P2) |
| hostQueryReset, imagelessFramebuffer, shaderSubgroupExtendedTypes, vulkanMemoryModel | 1 [B] | 0 | MENOS (P3) |
| shaderFloat16 / shaderInt8 / storageBuffer8BitAccess | 1/1/1 [B] | 0 | MENOS (P1: Unity half en shaders mobile lo consulta) |
| fragmentDensityMap / fragment_shading_rate / QCOM_fdm_offset | 1 [B] | ocultos a propósito (foveado propio) | MENOS intencional — mantener |

## 5. Límites (`physical_device.cpp:24-135`) — P1
Valores Adreno [B] salvo indicación. Solo filas con diferencia o relevantes.
| Límite | Adreno 740 | VPKEngine | Estado |
|---|---|---|---|
| maxImageDimension2D/Cube | 16384 | 16384 | = |
| maxImageDimension3D | 2048 | 2048 | = |
| maxImageArrayLayers | 2048 | 2048 | = |
| maxTexelBufferElements | 134217728 [C] | 2^26 | MENOS (bajo) |
| maxUniformBufferRange | 65536 | 65536 | = |
| maxStorageBufferRange | 2^30 [C] | 2^30 | = |
| maxPushConstantsSize | 128 [A] | 128 | = |
| maxBoundDescriptorSets | 32 [C] (baseline ≥4) | 4 | MENOS posible (UE5 usa hasta 6+; Turnip tuvo bug por esto, mesa#5546) — confirmar |
| maxPerStageDescriptorSamplers / SampledImages | 16 / 128? [C] | 16 / 128 | C |
| maxPerStageDescriptorStorageBuffers | 24 [C] | 24 | = (C) |
| maxVertexInputAttributes / Bindings | 32 / 32 | 31 / 31 | MENOS (bajo; limitación Metal slot 30) |
| maxVertexOutputComponents / maxFragmentInputComponents | 128 / 128 | 124 / 124 | MENOS (bajo; Metal 124) |
| maxComputeSharedMemorySize | 32768 | 32768 | = |
| maxComputeWorkGroupInvocations / Size | 1024 / 1024,1024,64 | igual | = |
| subgroupSize | 64 (wave64; 128 también) [B] | 32 (`entry.cpp:288`) | **diferente** (P1) — shaders con `WaveGetLaneCount` asumido 64 fallan. No se puede hacer 64 real en Apple: dejar 32 (honesto). |
| subgroup supportedStages / ops | all stages; basic,vote,arith,ballot,shuffle,shuffleRel,clustered,quad [B] | frag+comp; basic,vote,ballot | MENOS (P2) — Metal soporta shuffle/arith/quad: añadir y validar CTS |
| maxDrawIndirectCount | 2^32-1 [B] | 65536 | MENOS (bajo) |
| maxViewports | 16 | 1 | MENOS (con multiViewport) |
| timestampPeriod | 52.083 (19.2 MHz) [B] | 1.0 | diferente; el juego multiplica, ambos correctos. Opcional igualar. |
| minUniform/Storage/TexelBufferOffsetAlignment | 64 / 64 / 64 [B] | 64 / 64 / 64 | = |
| nonCoherentAtomSize | 1 [C] | 64 | diferente, inocuo |
| bufferImageGranularity | 1 [B] | 1 | = |
| maxCullDistances / maxCombined | 8 / 8 | 0 / 8 | MENOS (P3) |
| maxFragmentCombinedOutputResources | 72? [C] | 72 | C |
| maxSamplerAnisotropy / LodBias | 16 / 15.99 | 16 / 15.99 | = |
| pointSizeRange | 1..1023? [C] (baseline ≥511) | 1..511 | C |
| lineWidthRange / wideLines | 1..1 / 0? [C] | 1..1 | = (C) |
| maxMultiviewViewCount / InstanceIndex | 6? / 2^27-1 [A baseline ≥6] | 8 / 2^27-1 | MÁS leve (inocuo) |
| maxPerSetDescriptors / maxMemoryAllocationSize | ? / 4 GB? [C] | 1024 / 4 GB | C |

## 6. Sample counts — P0 (ligado a MSAA memoryless)
| Campo | Adreno 740 | VPKEngine | Klepton | Estado |
|---|---|---|---|---|
| framebuffer*/sampledImage Color/Depth/Stencil | 1\|2\|4 (+8? C) [B; baseline ≥1\|4] | 1\|2\|4 | AC Nexus enmascarado a 1\|2 (`kl_vulkan.c:6087`) | = (Klepton reduce a propósito: quitar cuando MSAA memoryless funcione) |
| sampledImageIntegerSampleCounts | 1\|2\|4 [B] | 1\|2\|4 | | = |
| storageImageSampleCounts | 1 | 1 | | = |
| standardSampleLocations | 1 | 1 | | = |

## 7. Memoria (`physical_device.cpp:138-156`) — P1
| | Adreno 740 (Quest 3) [B] | VPKEngine | Estado |
|---|---|---|---|
| Heaps | 1 heap DEVICE_LOCAL ~ RAM total (~7-8 GB en 8 GB) | 1 heap 6 GB | ≈ (tamaño C) |
| Types | 0: DL · 1: DL\|HV\|HC · 2: DL\|HV\|HC\|HCached · 3: DL\|LAZILY (+ PROTECTED) | 0 DL · 1 DL\|HV\|HC · 2 DL\|HV\|HC\|HCached · 3 DL\|LAZILY | = salvo tipo PROTECTED (no hace falta) |
| LAZILY_ALLOCATED real | sí (GMEM) | declarado; **NOCUMPLE** si no se crea `MTLStorageModeMemoryless` | NOCUMPLE → tarea "MSAA transitorio memoryless" de la bitácora (paso 1) |
| VK_EXT_memory_budget | sí [B] | no | MENOS (P2; Unity/UE ajustan streaming) |

## 8. Formatos (`physical_device.cpp:158-279`) — P1
| Grupo | Adreno 740 | VPKEngine | Estado |
|---|---|---|---|
| ETC2/EAC, ASTC LDR 4x4..12x12 cuadrados | todos los bloques ASTC (incl. 5x4,5x5,6x5,8x5,8x6,10x5,10x6,10x8,12x10) [B] | solo cuadrados | **MENOS** (P1: texturas ASTC 5x5/8x6 de Unity → "formato no soportado" → fallback o gris). Metal soporta todos los bloques ASTC: añadir filas en `kFormats` + `vpgpu::Format`. |
| ASTC HDR (EXT_texture_compression_astc_hdr) | sí [C] | no | C |
| BC1-7 | **no** [B] | BC1/3/7 sí | **MÁS** (P1) — quitar (ver feature BC) |
| R4G4B4A4 / B4G4R4A4 / R5G5B5A1 / B5G6R5 | sí (sampled/attach/blend) [B] | solo A1R5G5B5 y R5G6B5 (ABGR4 existe en core pero sin fila) | MENOS (P3) |
| R8G8B8_* , R16G16B16_* | buffer (vertex) sí [B] | no | MENOS (P2: vértices) |
| R32G32B32_* | vertex buffer | vertex buffer | = |
| D24_UNORM_S8_UINT / X8_D24 | sí | sí (emulado D32S8) | = (NOCUMPLE en precisión exacta, inocuo) |
| D32_SFLOAT_S8_UINT | sí [B] | sí | = |
| A2R10G10B10 / B10G11R11 / E5B9G9R9 storage | storage no para packed [B] | idem | = |
| Storage image en RGBA8/R32 etc. | sí | sí | = |
| G8_B8R8_2PLANE_420 (YCbCr) | sí [A baseline] | no | MENOS (con samplerYcbcrConversion, P2) |

## 9. Extensiones
### Instancia (`physical_device.cpp:284-289`)
Adreno/Android loader [A]: get_physical_device_properties2, external_{memory,semaphore,fence}_capabilities, surface,
android_surface, get_surface_capabilities2, swapchain_colorspace, debug_report/debug_utils (loader).
VPKEngine: las 4 primeras. Superficie la provee Klepton/OpenXR → **=** en lo relevante. `VK_EXT_debug_utils` falta:
MENOS (P2, Unity lo intenta habilitar si existe; inocuo si no).

### Dispositivo (`physical_device.cpp:293-317`)
| Extensión | Adreno 740 | VPKEngine | Estado / prioridad |
|---|---|---|---|
| KHR_multiview, create_renderpass2, maintenance1, bind_memory2, get_memory_requirements2, dedicated_allocation, descriptor_update_template, sampler_mirror_clamp_to_edge | sí [A] | sí | = |
| KHR_swapchain | sí [A] | no (lo da Klepton) | = efectivo |
| **KHR_maintenance2 / maintenance3** | sí [A/B] | no | MENOS (P0: Unity las pide; maint3 ya responde en Properties2 sin anunciarla) |
| **KHR_shader_draw_parameters** | sí [A] | no | MENOS (P0, ver feature) |
| KHR_storage_buffer_storage_class | sí [A] | no | MENOS (P0, trivial: el traductor ya acepta StorageBuffer) |
| KHR_variable_pointers | sí [A] | no | MENOS (P2) |
| KHR_external_memory/_fd, external_semaphore/_fd, external_fence/_fd, ANDROID_external_memory_android_hardware_buffer, EXT_queue_family_foreign | sí [A] | no | MENOS (P2; vídeo/AHB, Klepton lo puentea) |
| KHR_driver_properties | sí [A] | no | MENOS (P1) |
| KHR_image_format_list, KHR_sampler_ycbcr_conversion, KHR_16bit_storage, KHR_8bit_storage, KHR_shader_float16_int8, KHR_depth_stencil_resolve, KHR_imageless_framebuffer, KHR_timeline_semaphore, KHR_synchronization2, KHR_dynamic_rendering, KHR_copy_commands2, KHR_draw_indirect_count, KHR_separate_depth_stencil_layouts, KHR_uniform_buffer_standard_layout, EXT_scalar_block_layout, KHR_buffer_device_address, EXT_descriptor_indexing, KHR_shader_subgroup_extended_types, KHR_spirv_1_4, KHR_shader_float_controls, KHR_vulkan_memory_model, KHR_maintenance4, KHR_format_feature_flags2, EXT_host_query_reset, EXT_memory_budget, EXT_subgroup_size_control, EXT_texture_compression_astc_hdr, EXT_sampler_filter_minmax, EXT_extended_dynamic_state(2), EXT_pipeline_creation_cache_control, EXT_robustness2 | sí [B] | parte detrás de VPVK_EXTRA_EXTENSIONS, resto no | MENOS (P1-P3, ir validando con CTS) |
| EXT_multi_draw | no [C] | con extras | posible MÁS |
| EXT_metal_objects | **no** | sí | **MÁS** (P2) — delata que no es Adreno; quitarla del listado al juego (mantener interna para Klepton) |
| EXT_fragment_density_map(2), QCOM_fragment_density_map_offset, KHR_fragment_shading_rate, QCOM_render_pass_transform/store_ops/rotated_copy, QCOM_tile_properties | sí [B] | no (intencional para FDM) | MENOS intencional |
| ANDROID_native_buffer / GOOGLE_display_timing | sí [A] | no | inocuo con OpenXR |

## 10. Orden de trabajo recomendado (Unity/Unreal en Quest primero)
1. **P0 identidad**: driverVersion 512.744.0 en VPKEngine (`physical_device.cpp:16`).
2. **P0 vertexPipelineStoresAndAtomics=1** fijo (`entry.cpp:325`) + CTS de stores en vértice.
3. **P0 shaderInt16, shaderDrawParameters + KHR_shader_draw_parameters, KHR_maintenance2/3, KHR_storage_buffer_storage_class**
   (todas del perfil Android baseline: un juego puede asumirlas sin comprobar).
4. **P1 quitar BC** (feature + filas BC en `kFormats`) y **EXT_metal_objects** de la lista expuesta.
5. **P1 ASTC no cuadrados** en `kFormats` + `vpgpu::Format`.
6. **P1 MSAA memoryless real** para LAZILY_ALLOCATED → luego retirar `KL_VK_MAX_SAMPLES` de AC.
7. **P1 16-bit storage / float16_int8, driver_properties, subgroup ops**.
8. **P1-P2 Vulkan 1.2/1.3**: mover extras a fijo según CTS, rellenar VULKAN_1_1/1_2/1_3_FEATURES/PROPERTIES y subir apiVersion.
9. Cuando llegue el vulkaninfo real: repasar todas las filas [B]/[C].

Nota Klepton: con VPKEngine como backend, `klvk_patch_features`/`klvk_spoof_props` deberían quedar como no-op; todo lo
anterior debe vivir en VPKEngine (fuente única) para que el CTS lo vea igual que el juego.
