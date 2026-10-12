// com.oculus.videoplayer.NativeVideoPlayer - Meta's all-static ExoPlayer
// wrapper (Oculus Integration's OVROverlay video sample, shipped by Unity
// titles e.g. for cutscenes). The guest renders nothing: it hands the player
// the Android Surface of an external-surface overlay layer, and the player
// draws into it. Here the call is recorded in kl_hostvideo and the visionOS
// compositor plays the file and draws it as that layer.
//
// android/view/Surface objects come from ovrp_GetLayerAndroidSurfaceObject;
// each carries (layer_id + 1) as its payload and is pinned (a global for life).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_jni_int.h"
#include "kl_hostvideo.h"

#define KLHV_SURF_CLASS "android/view/Surface"
#define KLHV_MAX_SURF 64
static void *g_surf[KLHV_MAX_SURF];
static int   g_surf_layer[KLHV_MAX_SURF];
static int   g_nsurf;

void *kl_hostvideo_surface_object(int layer_id) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsurf; i++)
        if (g_surf_layer[i] == layer_id) { void *o = g_surf[i]; pthread_mutex_unlock(&g_lock); return o; }
    pthread_mutex_unlock(&g_lock);
    void *o = klj_new_object_data(KLHV_SURF_CLASS, (void *)(intptr_t)(layer_id + 1));
    if (!o) return NULL;
    pthread_mutex_lock(&g_lock);
    klj_object *ko = klj_as_object(o);
    if (ko) ko->pinned = 1;   // DeleteLocalRef/PopLocalFrame must not recycle it
    if (g_nsurf < KLHV_MAX_SURF) { g_surf[g_nsurf] = o; g_surf_layer[g_nsurf] = layer_id; g_nsurf++; }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "  [video] Surface %p for overlay layer %d\n", o, layer_id);
    return o;
}

int kl_hostvideo_surface_layer(void *surface) {
    if (!surface) return -1;
    pthread_mutex_lock(&g_lock);
    int layer = -1;
    for (int i = 0; i < g_nsurf; i++) if (g_surf[i] == surface) { layer = g_surf_layer[i]; break; }
    pthread_mutex_unlock(&g_lock);
    return layer;
}

static klj_val klhv_playVideo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *path = n > 1 && a[1].l ? klj_str(a[1].l) : NULL;
    const char *drm  = n > 2 && a[2].l ? klj_str(a[2].l) : NULL;
    int layer = n > 3 ? kl_hostvideo_surface_layer(a[3].l) : -1;
    if (drm && drm[0])
        fprintf(stderr, "  [video] playVideo DRM license url \"%s\" ignored (clear content assumed)\n", drm);
    if (n > 3 && a[3].l && layer < 0)
        fprintf(stderr, "  [video] playVideo: surface %p is not an overlay layer Surface\n", a[3].l);
    kl_hostvideo_play(path, layer);
    return (klj_val){0};
}
static klj_val klhv_stop(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; kl_hostvideo_stop(); return (klj_val){0}; }
static klj_val klhv_pause(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; kl_hostvideo_set_paused(1); return (klj_val){0}; }
static klj_val klhv_resume(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; kl_hostvideo_set_paused(0); return (klj_val){0}; }
static klj_val klhv_setLooping(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; kl_hostvideo_set_looping(n > 0 && (a[0].j & 0xff) != 0); return (klj_val){0}; }
static klj_val klhv_setPos(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; kl_hostvideo_seek(n > 0 ? (long long)a[0].j : 0); return (klj_val){0}; }
static klj_val klhv_setSpeed(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; kl_hostvideo_set_speed(n > 0 ? (float)a[0].d : 1.0f); return (klj_val){0}; }
static klj_val klhv_setRot(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;   // spatial audio orientation: no audio track to rotate
    KLJ_LOG_ONCE("NativeVideoPlayer.setListenerRotationQuaternion recorded (no-op)");
    return (klj_val){0};
}
static klj_val klhv_isPlaying(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)kl_hostvideo_is_playing()}; }
static klj_val klhv_state(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)(int64_t)kl_hostvideo_state()}; }
static klj_val klhv_duration(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)kl_hostvideo_duration()}; }
static klj_val klhv_position(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)kl_hostvideo_position()}; }
static klj_val klhv_width(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)(int64_t)kl_hostvideo_width()}; }
static klj_val klhv_height(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)(int64_t)kl_hostvideo_height()}; }
static klj_val klhv_stereo(void *e, void *s, const klj_val *a, int n) { (void)e; (void)s; (void)a; (void)n; return (klj_val){.j = (uint64_t)(int64_t)kl_hostvideo_stereo()}; }

#define NVP "com/oculus/videoplayer/NativeVideoPlayer"
const klj_binding klj_bind_hostvideo[] = {
    {NVP, "playVideo", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Landroid/view/Surface;)V", klhv_playVideo},
    {NVP, "stop",   "()V", klhv_stop},
    {NVP, "pause",  "()V", klhv_pause},
    {NVP, "resume", "()V", klhv_resume},
    {NVP, "setLooping", "(Z)V", klhv_setLooping},
    {NVP, "setPlaybackPosition", "(J)V", klhv_setPos},
    {NVP, "setPlaybackSpeed", "(F)V", klhv_setSpeed},
    {NVP, "setListenerRotationQuaternion", "(FFFF)V", klhv_setRot},
    {NVP, "getIsPlaying", "()Z", klhv_isPlaying},
    {NVP, "getCurrentPlaybackState", "()I", klhv_state},
    {NVP, "getDuration", "()J", klhv_duration},
    {NVP, "getPlaybackPosition", "()J", klhv_position},
    {NVP, "getWidth",  "()I", klhv_width},
    {NVP, "getHeight", "()I", klhv_height},
    {NVP, "getStereoMode", "()I", klhv_stereo},
    {NULL, NULL, NULL, NULL},
};
