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

// Quest profile of one game: 1 = present a Quest 3, 0 = a Quest 2. choice is the game's own setting: 0 automatic, 2 Quest 2,
// 3 Quest 3. Automatic: Quest 3 only if the global setting is on and, when the manifest declares com.oculus.supportedDevices
// (declared), it lists quest3 (manifest_quest3); a manifest that declares nothing follows the global setting alone.
int vq_quest3_resolve(int32_t choice, int32_t global, int32_t declared, int32_t manifest_quest3);

// Play area of one game: mode 0 free (roomscale rectangle w x d, each side clamped to 2..6 m, default 3 for NaN / <= 0),
// 1 seated (circle; w = d = its diameter, 2 m). Unknown modes are free. Returns the resolved mode. Pure.
int vq_area_resolve(int32_t mode, float w, float d, float *out_w, float *out_d);

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
// reason: 0 ok, 1 no pipelines, 2 recent pipelines, 3 no frames, 4 compiling, 5 unused modules (the app ignores
// this one: see vq_cache_complete).
struct vq_cache_state {
    uint32_t version, pipelines_unique, recipes, modules_created, modules_used, idle_ms, frames_idle, complete, reason;
};
// Fills *out and returns 1 when the symbol exists and a device is alive; else 0 (out zeroed). No side effects, never blocks.
int vq_cache_state_get(struct vq_cache_state *out);
// Complete = no pipelines compiling and no new keys this session. The engine's unused-module criterion
// (reason 5) is NOT used: it never holds when a game has many unused recipes. Pure; 1 complete, 0 not.
int vq_cache_complete(const struct vq_cache_state *s);
// The `complete` to store on disk: a complete=1 saved by an earlier session is kept while this session added no new
// recipes (a short session that only serves keys from the cache); with new recipes it follows `complete`. Pure.
int vq_cache_persisted_complete(int saved_complete, int complete, uint32_t new_recipes);
