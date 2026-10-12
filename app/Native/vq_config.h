// VisionQuestPro: graphics settings from the app (Settings sheet) to the runtime.
// Mirrors VPEngine's gpu/vulkan/include/vpvk/config.h (same layout); forwarded to
// libVPVulkan with dlsym when that driver is loaded. Diagnostics: log "vq.fov".
#pragma once
#include <stdint.h>

typedef struct vq_gfx_config {
    int32_t foveation_mode;   // 0 off, 1 M2, 2 M5, 3 custom (vpvk_foveation_mode)
    float sharp_zone;         // custom: 0.05..1
    float periphery;          // custom: 0.1..1
    float height;             // custom: -0.5..0.5
    int32_t vpktaa_enabled;
    float vpktaa_percent;     // 0..100
    float vpktaa_sharpness;   // 0..1
    float render_scale;       // 0.5..1.5
    int32_t asw_disabled;
    int32_t quest3_profile;
} vq_gfx_config;

// Stores the config and pushes it to VPVulkan if loaded (returns 1 if forwarded).
int vq_gfx_config_apply(const vq_gfx_config *c);
// Re-push the stored config (call after the driver is dlopen'ed).
int vq_gfx_config_forward(void);
