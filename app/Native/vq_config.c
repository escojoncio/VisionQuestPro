#include "vq_config.h"

#include <dlfcn.h>
#include <stdio.h>

// Same layout as vpvk/config.h (VPEngine); kept local so the app builds without the header.
typedef struct { int32_t mode; float sharp_zone, periphery, height; } vpvk_foveation;
typedef struct { int enabled; float percent, sharpness; } vpvk_vpktaa;

static vq_gfx_config g_cfg;
static int g_have;

int vq_gfx_config_forward(void) {
    if (!g_have) return 0;
    void (*set_fov)(const vpvk_foveation *) =
        (void (*)(const vpvk_foveation *))dlsym(RTLD_DEFAULT, "vpvk_config_set_foveation");
    void (*set_taa)(const vpvk_vpktaa *) =
        (void (*)(const vpvk_vpktaa *))dlsym(RTLD_DEFAULT, "vpvk_config_set_vpktaa");
    void (*set_scale)(float) = (void (*)(float))dlsym(RTLD_DEFAULT, "vpvk_config_set_render_scale");
    void (*set_asw)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_asw_disabled");
    void (*set_q3)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_quest3_profile");
    if (!set_fov || !set_taa) {
        fprintf(stderr, "vq.fov config stored; VPVulkan not loaded (MoltenVK path): own foveation/VPKTAA not applied\n");
        return 0;
    }
    vpvk_foveation f = { g_cfg.foveation_mode, g_cfg.sharp_zone, g_cfg.periphery, g_cfg.height };
    vpvk_vpktaa t = { g_cfg.vpktaa_enabled, g_cfg.vpktaa_percent, g_cfg.vpktaa_sharpness };
    set_fov(&f);
    set_taa(&t);
    if (set_scale) set_scale(g_cfg.render_scale);
    if (set_asw) set_asw(g_cfg.asw_disabled);
    if (set_q3) set_q3(g_cfg.quest3_profile);
    fprintf(stderr, "vq.fov config forwarded to VPVulkan\n");
    return 1;
}

int vq_gfx_config_apply(const vq_gfx_config *c) {
    if (!c) return 0;
    g_cfg = *c;
    g_have = 1;
    static const char *const names[] = { "off", "m2", "m5", "custom" };
    int m = (c->foveation_mode >= 0 && c->foveation_mode <= 3) ? c->foveation_mode : 0;
    fprintf(stderr, "vq.fov settings quest_foveation=off(always) own=%s sharp=%.2f periphery=%.2f height=%.2f "
            "vpktaa=%d %.0f%% sharp=%.2f scale=%.2f asw_off=%d quest3=%d\n",
            names[m], c->sharp_zone, c->periphery, c->height, c->vpktaa_enabled, c->vpktaa_percent,
            c->vpktaa_sharpness, c->render_scale, c->asw_disabled, c->quest3_profile);
    return vq_gfx_config_forward();
}
