// VisionQuestPro: graphics settings from the app (Settings sheet) to the runtime.
// Mirrors VPKEngine's gpu/vulkan/include/vpvk/config.h (same layout); forwarded to
// libVPVulkan with dlsym when that driver is loaded. Diagnostics: log "vq.fov".
#pragma once
#include <stdint.h>

typedef struct vq_gfx_config {
    int32_t foveation_mode;   // 0 off, 1 M2, 2 M5, 3 custom (vpvk_foveation_mode)
    float sharp_zone;         // custom: 0.05..1
    float periphery;          // custom: 0.1..1
    float height;             // custom: -0.5..0.5
    float render_scale;       // 0.5..1.5
    int32_t asw_disabled;
    int32_t quest3_profile;
    int32_t anisotropy;       // 0 = game's own; 16 = force 16x on mipmapped linear samplers
    int32_t msaa_force;       // 0 = the game's own; 2 / 4 = MSAA forced on the game's 1x scene passes (also env VPVK_MSAA_FORCE)
    float rcas;               // 0..1: «Nitidez (RCAS)» (0 = off). Appended field.
} vq_gfx_config;

// Stores the config and pushes it to VPVulkan if loaded (returns 1 if forwarded).
int vq_gfx_config_apply(const vq_gfx_config *c);
// Re-push the stored config (call after the driver is dlopen'ed).
int vq_gfx_config_forward(void);

// Shader precompilation (VPVulkan only; no-ops returning 0 on the MoltenVK path).
// set_enabled also turns the driver's own prewarm at vkCreateDevice on/off.
void vq_prewarm_set_enabled(int on);
// Stored pipeline recipes for the current VP_PIPELINE_CACHE_DIR (0: none / not VPVulkan).
uint32_t vq_prewarm_count(void);
// Compiles them all before the game starts (blocking; call off the main thread).
typedef void (*vq_prewarm_progress)(uint32_t done, uint32_t total, void *user);
uint32_t vq_prewarm_run(vq_prewarm_progress progress, void *user);
// Read from the progress callback of vq_prewarm_run: 0 = compiling recipes, 1 = saving the pipeline archive (done/total = entries archived).
uint32_t vq_prewarm_phase(void);
// First-run precompile: translates + compiles every <dir>/*.spv (vq_harvest_spirv output) with
// VPVulkan's vpvk_precompile_spirv. Returns the libraries compiled (0 without VPVulkan).
uint32_t vq_precompile_run(const char *dir, vq_prewarm_progress progress, void *user);
// AOT build (VPVulkan vpvk_aot_build, needs env VPVK_AOT=1): every <spirv_dir>/*.spv + every stored
// recipe compiled and serialized into the game's Metal binary archive. 0 without VPVulkan / gate off.
uint32_t vq_aot_run(const char *spirv_dir, vq_prewarm_progress progress, void *user);

// Shader-cache state of the live VPVulkan device (vpvk_cache_state). Mirrors VpvkCacheState in vpvk.h:
// 9 uint32 in this exact order (36 bytes); it MUST match the engine's struct.
// reason: 0 ok, 1 no pipelines, 2 recent pipelines, 3 no frames, 4 compiling, 5 unused modules.
struct vq_cache_state {
    uint32_t version, pipelines_unique, recipes, modules_created, modules_used, idle_ms, frames_idle, complete, reason;
};
// Fills *out and returns 1 when the symbol exists and a device is alive; else 0 (out zeroed). No side effects, never blocks.
int vq_cache_state_get(struct vq_cache_state *out);
