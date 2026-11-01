#include "vq_config.h"

#include <dlfcn.h>
#include <stdio.h>

// Same layout as vpvk/config.h (VPKEngine); kept local so the app builds without the header.
typedef struct { int32_t mode; float sharp_zone, periphery, height; } vpvk_foveation;

static vq_gfx_config g_cfg;
static int g_have;

int vq_gfx_config_forward(void) {
    // Quest/Adreno 740 identity: always on, independent of the settings and of g_have. Absent in older VPKEngine: no-op.
    void (*set_identity)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_quest_identity");
    if (set_identity) set_identity(1);
    else fprintf(stderr, "[vqcfg] vpvk_config_set_quest_identity missing (older VPKEngine)\n");
    if (!g_have) return 0;
    void (*set_fov)(const vpvk_foveation *) =
        (void (*)(const vpvk_foveation *))dlsym(RTLD_DEFAULT, "vpvk_config_set_foveation");
    void (*set_scale)(float) = (void (*)(float))dlsym(RTLD_DEFAULT, "vpvk_config_set_render_scale");
    void (*set_asw)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_asw_disabled");
    void (*set_q3)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_quest3_profile");
    void (*set_aniso)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_anisotropy");
    // MSAA forced on the game's 1x scene passes (also read from VPVK_MSAA_FORCE by the driver): weak, may be absent.
    void (*set_msaa_force)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_msaa_force");
    // RCAS (sharpening pass): newer VPKEngine only, weak (also read from VPVK_RCAS by the driver).
    void (*set_rcas)(float) = (void (*)(float))dlsym(RTLD_DEFAULT, "vpvk_config_set_rcas");
    // Own foveation (rate map per eye): the app has none now, the mode is always Off (VQLaunch also sets VPVK_FOVEATION=0).
    void (*set_fov_apply)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "vpvk_config_set_foveation_apply");
    if (!set_fov && !set_scale && !set_msaa_force && !set_rcas) {
        fprintf(stderr, "vq.fov config stored; VPVulkan not loaded (MoltenVK path): VPKEngine settings not applied\n");
        return 0;
    }
    vpvk_foveation f = { g_cfg.foveation_mode, g_cfg.sharp_zone, g_cfg.periphery, g_cfg.height };
    if (set_fov) set_fov(&f);
    if (set_fov_apply) set_fov_apply(g_cfg.foveation_mode != 0);
    if (set_scale) set_scale(g_cfg.render_scale);
    if (set_asw) set_asw(g_cfg.asw_disabled);
    if (set_q3) set_q3(g_cfg.quest3_profile);
    if (set_aniso) set_aniso(g_cfg.anisotropy);
    if (set_rcas) set_rcas(g_cfg.rcas);
    else if (g_cfg.rcas > 0)
        fprintf(stderr, "vq.rcas intensity %.2f: vpvk_config_set_rcas not in this VPVulkan (env VPVK_RCAS only)\n", g_cfg.rcas);
    if (set_msaa_force) set_msaa_force(g_cfg.msaa_force);
    else if (g_cfg.msaa_force)
        fprintf(stderr, "vq.aa msaa force %d: vpvk_config_set_msaa_force not in this VPVulkan (env VPVK_MSAA_FORCE only)\n",
                g_cfg.msaa_force);
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
            "rcas=%.2f scale=%.2f asw_off=%d quest3=%d aniso=%d msaa_force=%d\n",
            names[m], c->sharp_zone, c->periphery, c->height,
            c->rcas, c->render_scale, c->asw_disabled, c->quest3_profile, c->anisotropy,
            c->msaa_force);
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

uint32_t vq_prewarm_phase(void) {
    uint32_t (*f)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "vpvk_prewarm_phase");
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

uint32_t vq_aot_run(const char *spirv_dir, vq_prewarm_progress progress, void *user) {
    typedef uint32_t (*run_fn)(const char *, vq_prewarm_progress, void *);
    run_fn f = (run_fn)dlsym(RTLD_DEFAULT, "vpvk_aot_build");
    if (!f) {
        fprintf(stderr, "vq.aot skipped: VPVulkan without vpvk_aot_build\n");
        return 0;
    }
    return f(spirv_dir, progress, user);
}
