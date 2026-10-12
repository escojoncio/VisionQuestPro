// Host video: a guest-driven video the HOST decodes and composites.
//
// Meta's com.oculus.videoplayer.NativeVideoPlayer (ExoPlayer) renders a movie
// into the Android Surface of an OVROverlay layer. There is no Surface here, so
// the JNI side (kl_jni_hostvideo.c) only records what the guest asked for, and
// the visionOS compositor plays the same file with AVPlayer and draws it in
// place of that layer's texture. Generic: any Quest title using the plugin.
//
// One video at a time (the Java class is all-static). Request goes C -> Swift,
// status goes Swift -> C; both under one mutex, copied out whole.
#ifndef KL_HOSTVIDEO_H
#define KL_HOSTVIDEO_H
#ifdef __cplusplus
extern "C" {
#endif

// ExoPlayer Player.STATE_*
enum { KL_HV_IDLE = 1, KL_HV_BUFFERING = 2, KL_HV_READY = 3, KL_HV_ENDED = 4 };
// Stereo layout of the video frame: 0 mono, 1 top/bottom, 2 left/right.
enum { KL_HV_MONO = 0, KL_HV_TOP_BOTTOM = 1, KL_HV_LEFT_RIGHT = 2 };

typedef struct {
    char      path[1024];  // HOST path, empty when nothing is requested
    int       layer_id;    // overlay layer whose Surface was passed, -1 unknown
    int       active;      // 1 between playVideo and stop
    int       paused;
    int       looping;
    float     speed;
    long long seek_ms;     // valid when seek_gen changed
    unsigned  seek_gen;
    unsigned  gen;         // bumps on every playVideo (new item)
} kl_hostvideo_request;

// Swift: the compositor exists and will consume requests. Without it playVideo
// reports ENDED at once so the guest skips the video instead of waiting.
void kl_hostvideo_set_host_present(int present);
// Swift, per frame: copy the request out. Returns its gen.
unsigned kl_hostvideo_get_request(kl_hostvideo_request *out);
// Swift: publish playback status for request `gen` (ignored if stale).
void kl_hostvideo_publish(unsigned gen, int state, long long duration_ms,
                          long long position_ms, int width, int height,
                          int stereo_mode, int is_playing);

// JNI side (kl_jni_hostvideo.c)
void kl_hostvideo_play(const char *guest_path, int layer_id);
void kl_hostvideo_stop(void);
void kl_hostvideo_set_paused(int paused);
void kl_hostvideo_set_looping(int looping);
void kl_hostvideo_seek(long long ms);
void kl_hostvideo_set_speed(float speed);
int       kl_hostvideo_state(void);
int       kl_hostvideo_is_playing(void);
long long kl_hostvideo_duration(void);
long long kl_hostvideo_position(void);
int       kl_hostvideo_width(void);
int       kl_hostvideo_height(void);
int       kl_hostvideo_stereo(void);

// A pinned jobject of class android/view/Surface that names an overlay layer,
// and the reverse. Implemented in kl_jni_hostvideo.c (needs JNI internals);
// kl_ovrp.c hands the first out from ovrp_GetLayerAndroidSurfaceObject.
void *kl_hostvideo_surface_object(int layer_id);
int   kl_hostvideo_surface_layer(void *surface);   // -1 when not ours

#ifdef __cplusplus
}
#endif
#endif
