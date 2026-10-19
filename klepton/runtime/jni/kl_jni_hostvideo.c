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
#include <stdlib.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_jni_int.h"
#include "kl_hostvideo.h"
#include "kl_mediandk.h"
#include "kl_gles.h"
#include "kl_ndk.h"

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


// ---------------------------------------------------------------------------
// android.graphics.SurfaceTexture / android.view.Surface(SurfaceTexture) — the
// surface-mode path of Unity's AndroidVideoMedia:
//
//   st = new SurfaceTexture(texId); st.setOnFrameAvailableListener(l[, handler]);
//   s  = new Surface(st); ... ANativeWindow_fromSurface(s) -> AMediaCodec_configure
//
// The codec decodes into the AImageReader behind kl_jni_surface_window() (a
// host-side reader, kl_mediandk.c); each published frame fires onFrameAvailable
// on the reader's dispatch thread (Android uses the handler's thread; Unity's
// listener only flags/notifies natively, so the thread does not matter), and
// updateTexImage converts the newest frame to RGBA8 and uploads it to the
// guest's GL texture through vpgles (kl_gles_sym), as GL_TEXTURE_EXTERNAL_OES.
#define STEX_CLASS "android/graphics/SurfaceTexture"
#define STEX_LISTENER "android/graphics/SurfaceTexture$OnFrameAvailableListener"
#define GL_TEX_EXTERNAL_OES   0x8D65
#define GL_TEX_BIND_EXTERNAL  0x8D67
#define GL_TEX_2D             0x0DE1
#define GL_TEX_BIND_2D        0x8069

typedef struct {
    int      tex, w, h;          // GL name, default buffer size
    int      tex_w, tex_h;       // storage currently allocated on the GL side
    int      target;            // GL target the frames are uploaded to
    int      params_set, released;
    int64_t  ts_ns;
    void    *reader, *self, *listener;
} klhv_stex;

typedef struct { klhv_stex *t; } klhv_ssurf;
static void klhv_surface_destroy(void *p) { free(p); }

static klhv_stex *klhv_stex_of(void *o) {
    klj_object *ko = klj_as_object(o);
    return (ko && ko->cls && strcmp(ko->cls, STEX_CLASS) == 0) ? ko->data : NULL;
}

static void klhv_on_frame(void *ctx) {
    klhv_stex *t = ctx;
    if (!t || t->released) return;
    void *l = t->listener;
    KLJ_LOG_ONCE("SurfaceTexture: first decoded frame published (onFrameAvailable %s)", l ? "delivered" : "has no listener");
    if (!l) return;
    klj_PushLocalFrame(NULL, 0);
    void *args = klj_new_array('L', "java/lang/Object", 1);
    ((void **)klj_arr(args)->data)[0] = t->self;
    klj_proxy_invoke(l, STEX_LISTENER, "onFrameAvailable", "(Landroid/graphics/SurfaceTexture;)V", args);
    klj_PopLocalFrame(NULL, NULL);
}

static klj_val klhv_stex_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    int tex = 0;
    if (n > 0) tex = (int)(int32_t)a[0].j;
    KLJ_LOG_ONCE("new SurfaceTexture(%d) — real SurfaceTexture over a host AImageReader", tex);
    klhv_stex *t = calloc(1, sizeof *t);
    if (!t) return (klj_val){0};
    t->tex = tex; t->w = t->h = 1; t->target = GL_TEX_EXTERNAL_OES;
    t->reader = kl_media_surface_new(NULL, NULL);
    void *o = klj_new_object_data(STEX_CLASS, t);
    t->self = o;
    pthread_mutex_lock(&g_lock);
    klj_object *ko = klj_as_object(o);
    if (ko) ko->pinned++;     // the guest and the reader thread hold it for life, until release()
    pthread_mutex_unlock(&g_lock);
    if (t->reader) kl_media_surface_set_cb(t->reader, klhv_on_frame, t);
    fprintf(stderr, "  [video] SurfaceTexture %p tex=%d reader=%p\n", o, tex, t->reader);
    return (klj_val){.l = o};
}
// SurfaceTexture(boolean singleBufferMode): detached, texture comes with attachToGLContext.
static klj_val klhv_stex_init_z(void *env, void *clazz, const klj_val *a, int n) {
    (void)a; (void)n;
    klj_val z[1] = {{0}};
    return klhv_stex_init(env, clazz, z, 1);
}
static klj_val klhv_stex_setBuf(void *e, void *self, const klj_val *a, int n) {
    (void)e;
    KLJ_LOG_ONCE("SurfaceTexture.setDefaultBufferSize");
    klhv_stex *t = klhv_stex_of(self);
    if (t && n >= 2) {
        t->w = (int)(int32_t)a[0].j; t->h = (int)(int32_t)a[1].j;
        kl_media_surface_set_size(t->reader, t->w, t->h);
    }
    return (klj_val){0};
}
static klj_val klhv_stex_setListener(void *e, void *self, const klj_val *a, int n) {
    (void)e;
    KLJ_LOG_ONCE("SurfaceTexture.setOnFrameAvailableListener");
    klhv_stex *t = klhv_stex_of(self);
    if (t) {
        // The listener arrives as a local ref of the guest's call frame: pin it (global-ref count) so
        // PopLocalFrame does not recycle it while the reader thread still calls through it.
        void *nl = n > 0 ? a[0].l : NULL;
        pthread_mutex_lock(&g_lock);
        klj_object *nk = klj_as_object(nl), *ok = klj_as_object(t->listener);
        if (nk) nk->pinned++;
        t->listener = nl;
        if (ok && ok->pinned) ok->pinned--;
        pthread_mutex_unlock(&g_lock);
    }
    return (klj_val){0};
}

// GL entry points come from vpgles (the guest's GL on this path); resolved once.
typedef void (*gl_bind_t)(uint32_t, uint32_t);
typedef void (*gl_teximg_t)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
typedef void (*gl_texsub_t)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
typedef void (*gl_geti_t)(uint32_t, int32_t *);
typedef uint32_t (*gl_gete_t)(void);
typedef void (*gl_texpi_t)(uint32_t, uint32_t, int32_t);
typedef void (*gl_pixst_t)(uint32_t, int32_t);

static void klhv_gl_upload(klhv_stex *t, const uint8_t *px, int w, int h) {
    static gl_bind_t bind; static gl_teximg_t teximg; static gl_texsub_t texsub; static gl_geti_t geti;
    static gl_gete_t gete; static gl_texpi_t texpi; static gl_pixst_t pixst; static int resolved;
    if (!resolved) {
        bind   = kl_gles_sym("glBindTexture");   teximg = kl_gles_sym("glTexImage2D");
        texsub = kl_gles_sym("glTexSubImage2D"); geti   = kl_gles_sym("glGetIntegerv");
        gete   = kl_gles_sym("glGetError");      texpi  = kl_gles_sym("glTexParameteri");
        pixst  = kl_gles_sym("glPixelStorei");
        if (bind && teximg && texsub) resolved = 1;
        else {
            KLJ_LOG_ONCE("SurfaceTexture.updateTexImage: vpgles GL entry points unavailable (KL_GLES_VPK off?) — frames not uploaded");
            return;
        }
    }
    if (!t->tex) return;
    int32_t prev = 0, rowlen = 0, align = 4;
    uint32_t bind_q = t->target == GL_TEX_EXTERNAL_OES ? GL_TEX_BIND_EXTERNAL : GL_TEX_BIND_2D;
    if (geti) { geti(bind_q, &prev); geti(0x0CF2 /* UNPACK_ROW_LENGTH */, &rowlen); geti(0x0CF5 /* UNPACK_ALIGNMENT */, &align); }
    if (pixst) { pixst(0x0CF2, 0); pixst(0x0CF5, 4); }
    for (int i = 0; gete && i < 8 && gete(); i++) {}
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t tg = (uint32_t)t->target;
        bind(tg, (uint32_t)t->tex);
        if (t->tex_w != w || t->tex_h != h) teximg(tg, 0, 0x1908 /* RGBA */, w, h, 0, 0x1908, 0x1401, px);
        else texsub(tg, 0, 0, 0, w, h, 0x1908, 0x1401, px);
        uint32_t err = gete ? gete() : 0;
        if (err && tg == GL_TEX_EXTERNAL_OES) {
            fprintf(stderr, "  [video] SurfaceTexture upload to GL_TEXTURE_EXTERNAL_OES tex %d -> GL error 0x%x; "
                            "falling back to GL_TEXTURE_2D\n", t->tex, err);
            bind(tg, (uint32_t)prev);
            t->target = GL_TEX_2D; t->tex_w = t->tex_h = 0; t->params_set = 0;
            continue;
        }
        if (err) { fprintf(stderr, "  [video] SurfaceTexture upload tex %d -> GL error 0x%x\n", t->tex, err); }
        else {
            t->tex_w = w; t->tex_h = h;
            if (!t->params_set && texpi) {
                texpi(tg, 0x2801, 0x2601); texpi(tg, 0x2800, 0x2601);        // LINEAR/LINEAR
                texpi(tg, 0x2802, 0x812F); texpi(tg, 0x2803, 0x812F);        // CLAMP_TO_EDGE
                t->params_set = 1;
            }
        }
        break;
    }
    bind((uint32_t)t->target, (uint32_t)prev);
    if (pixst) { pixst(0x0CF2, rowlen); pixst(0x0CF5, align); }
}

static klj_val klhv_stex_update(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    KLJ_LOG_ONCE("SurfaceTexture.updateTexImage — newest decoded frame -> RGBA -> GL texture");
    klhv_stex *t = klhv_stex_of(self);
    if (!t || t->released || !t->reader) return (klj_val){0};
    uint8_t *px = NULL; int w = 0, h = 0; int64_t pts = 0;
    int rc = kl_media_surface_take_rgba(t->reader, &px, &w, &h, &pts);
    if (rc == 1) {
        t->ts_ns = pts * 1000;
        klhv_gl_upload(t, px, w, h);
        static unsigned said;
        if (said++ < 3) fprintf(stderr, "  [video] SurfaceTexture tex %d <- %dx%d frame (pts %lld us)\n", t->tex, w, h, (long long)pts);
    }
    return (klj_val){0};
}
static klj_val klhv_stex_getMatrix(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)self;
    KLJ_LOG_ONCE("SurfaceTexture.getTransformMatrix (identity + standard Y flip)");
    klj_array *arr = n > 0 ? klj_arr(a[0].l) : NULL;
    if (arr && arr->kind == 'F' && arr->len >= 16) {
        float *m = arr->data;
        memset(m, 0, 16 * sizeof *m);
        m[0] = 1.f; m[5] = -1.f; m[10] = 1.f; m[13] = 1.f; m[15] = 1.f;   // column-major
    }
    return (klj_val){0};
}
static klj_val klhv_stex_getTs(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    klhv_stex *t = klhv_stex_of(self);
    return (klj_val){.j = (uint64_t)(t ? t->ts_ns : 0)};
}
static klj_val klhv_stex_release(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    KLJ_LOG_ONCE("SurfaceTexture.release");
    klhv_stex *t = klhv_stex_of(self);
    if (t && !t->released) {
        t->released = 1;
        kl_media_surface_abandon(t->reader);   // joins the callback thread: no more calls through the listener
        pthread_mutex_lock(&g_lock);
        klj_object *ko = klj_as_object(self), *lk = klj_as_object(t->listener);
        if (ko && ko->pinned) ko->pinned--;
        if (lk && lk->pinned) lk->pinned--;
        t->listener = NULL;
        pthread_mutex_unlock(&g_lock);
    }
    return (klj_val){0};
}
static klj_val klhv_stex_isReleased(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    klhv_stex *t = klhv_stex_of(self);
    return (klj_val){.j = t ? (uint64_t)t->released : 0};
}
static klj_val klhv_stex_attach(void *e, void *self, const klj_val *a, int n) {
    (void)e;
    KLJ_LOG_ONCE("SurfaceTexture.attachToGLContext");
    klhv_stex *t = klhv_stex_of(self);
    if (t && n > 0) { t->tex = (int)(int32_t)a[0].j; t->tex_w = t->tex_h = 0; t->params_set = 0; }
    return (klj_val){0};
}
static klj_val klhv_stex_noop(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    KLJ_LOG_ONCE("SurfaceTexture.detachFromGLContext/releaseTexImage (no-op)");
    return (klj_val){0};
}

static klj_val klhv_surf_init_stex(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    klhv_stex *t = n > 0 ? klhv_stex_of(a[0].l) : NULL;
    KLJ_LOG_ONCE("new Surface(SurfaceTexture %p)%s", n > 0 ? a[0].l : NULL, t ? "" : " — not one of ours");
    klhv_ssurf *ss = calloc(1, sizeof *ss);
    if (ss) ss->t = t;
    void *o = klj_new_object_data(KLHV_SURF_CLASS, ss);
    if (ss) klj_own(o, klhv_surface_destroy);
    return (klj_val){.l = o};
}
static klj_val klhv_surf_release(void *e, void *s, const klj_val *a, int n) {
    (void)e; (void)s; (void)a; (void)n;
    KLJ_LOG_ONCE("Surface.release (no-op)");
    return (klj_val){0};
}
static klj_val klhv_surf_isValid(void *e, void *self, const klj_val *a, int n) {
    (void)e; (void)a; (void)n;
    KLJ_LOG_ONCE("Surface.isValid");
    klj_object *ko = klj_as_object(self);
    if (ko && ko->destroy == klhv_surface_destroy && ko->data) {
        klhv_ssurf *ss = ko->data;
        return (klj_val){.j = ss->t && !ss->t->released};
    }
    return (klj_val){.j = 1};
}

// kl_ndk.c: ANativeWindow_fromSurface for a Surface(SurfaceTexture). NULL if it
// is any other Surface. The window is the host reader's, so AMediaCodec_configure
// finds the reader through kl_ndk_window_owner and renders real frames into it.
void *kl_jni_surface_window(void *surface) {
    klj_object *ko = klj_as_object(surface);
    if (!ko || ko->destroy != klhv_surface_destroy || !ko->data) return NULL;
    klhv_stex *t = ((klhv_ssurf *)ko->data)->t;
    if (!t || t->released) return NULL;
    void *w = kl_media_surface_window(t->reader);
    fprintf(stderr, "  [video] ANativeWindow_fromSurface(%p) -> SurfaceTexture %p reader window %p\n", surface, t->self, w);
    return w;
}

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
    {"android/graphics/SurfaceTexture", "<init>", "(I)V", klhv_stex_init},
    {"android/graphics/SurfaceTexture", "<init>", "(IZ)V", klhv_stex_init},
    {"android/graphics/SurfaceTexture", "<init>", "(Z)V", klhv_stex_init_z},
    {"android/graphics/SurfaceTexture", "setDefaultBufferSize", "(II)V", klhv_stex_setBuf},
    {"android/graphics/SurfaceTexture", "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V", klhv_stex_setListener},
    {"android/graphics/SurfaceTexture", "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;Landroid/os/Handler;)V", klhv_stex_setListener},
    {"android/graphics/SurfaceTexture", "updateTexImage", "()V", klhv_stex_update},
    {"android/graphics/SurfaceTexture", "getTransformMatrix", "([F)V", klhv_stex_getMatrix},
    {"android/graphics/SurfaceTexture", "getTimestamp", "()J", klhv_stex_getTs},
    {"android/graphics/SurfaceTexture", "release", "()V", klhv_stex_release},
    {"android/graphics/SurfaceTexture", "isReleased", "()Z", klhv_stex_isReleased},
    {"android/graphics/SurfaceTexture", "attachToGLContext", "(I)V", klhv_stex_attach},
    {"android/graphics/SurfaceTexture", "detachFromGLContext", "()V", klhv_stex_noop},
    {"android/graphics/SurfaceTexture", "releaseTexImage", "()V", klhv_stex_noop},
    {"android/view/Surface", "<init>", "(Landroid/graphics/SurfaceTexture;)V", klhv_surf_init_stex},
    {"android/view/Surface", "release", "()V", klhv_surf_release},
    {"android/view/Surface", "isValid", "()Z", klhv_surf_isValid},
    {NULL, NULL, NULL, NULL},
};
