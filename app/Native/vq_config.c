#include "vq_config.h"

#include <dlfcn.h>
#include <stdio.h>

// Same layout as vpvk/config.h (VPKEngine); kept local so the app builds without the header.
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
    void (*set_aniso)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_anisotropy");
    void (*set_1x)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_force_single_sample");
    // Newer VPKEngine only (also read from VPVK_MSAA by the driver): weak, may be absent.
    void (*set_msaa)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_msaa_override");
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
    if (set_aniso) set_aniso(g_cfg.anisotropy);
    if (set_1x) set_1x(g_cfg.force_single_sample);
    if (set_msaa) set_msaa(g_cfg.msaa_override);
    else if (g_cfg.msaa_override)
        fprintf(stderr, "vq.aa msaa override %d: vpvk_config_set_msaa_override not in this VPVulkan (env VPVK_MSAA only)\n",
                g_cfg.msaa_override);
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
            "vpktaa=%d %.0f%% sharp=%.2f scale=%.2f asw_off=%d quest3=%d aniso=%d force1x=%d msaa=%d\n",
            names[m], c->sharp_zone, c->periphery, c->height, c->vpktaa_enabled, c->vpktaa_percent,
            c->vpktaa_sharpness, c->render_scale, c->asw_disabled, c->quest3_profile, c->anisotropy,
            c->force_single_sample, c->msaa_override);
    return vq_gfx_config_forward();
}

void vq_prewarm_set_enabled(int on) {
    void (*f)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_prewarm");
    if (f) f(on ? 1 : 0);
}

uint32_t vq_prewarm_count(void) {
    uint32_t (*f)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "vpvk_prewarm_count");
    return f ? f() : 0;
}

uint32_t vq_prewarm_run(vq_prewarm_progress progress, void *user) {
    typedef uint32_t (*run_fn)(vq_prewarm_progress, void *);
    run_fn f = (run_fn)dlsym(RTLD_DEFAULT, "vpvk_prewarm");
    if (!f) {
        fprintf(stderr, "vq.prewarm skipped: VPVulkan not loaded\n");
        return 0;
    }
    return f(progress, user);
}

uint32_t vq_precompile_run(const char *dir, vq_prewarm_progress progress, void *user) {
    typedef uint32_t (*run_fn)(const char *, vq_prewarm_progress, void *);
    run_fn f = (run_fn)dlsym(RTLD_DEFAULT, "vpvk_precompile_spirv");
    if (!f) {
        fprintf(stderr, "vq.precompile skipped: VPVulkan not loaded\n");
        return 0;
    }
    return f(dir, progress, user);
}
