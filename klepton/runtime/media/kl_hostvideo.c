// Host video state machine. See kl_hostvideo.h.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "kl_hostvideo.h"
#include "klepton.h"

static pthread_mutex_t g_hv_lock = PTHREAD_MUTEX_INITIALIZER;
static kl_hostvideo_request g_req = { .layer_id = -1, .speed = 1.0f };
static struct {
    int state; long long dur, pos; int w, h, stereo, playing;
} g_st = { KL_HV_IDLE, 0, 0, 0, 0, 0, 0 };
static int g_host_present;

static const char *hv_state_name(int s) {
    switch (s) {
    case KL_HV_IDLE: return "IDLE"; case KL_HV_BUFFERING: return "BUFFERING";
    case KL_HV_READY: return "READY"; case KL_HV_ENDED: return "ENDED";
    }
    return "?";
}

// Caller holds the lock.
static void hv_set_state_locked(int s) {
    if (g_st.state == s) return;
    fprintf(stderr, "  [video] state %s -> %s\n", hv_state_name(g_st.state), hv_state_name(s));
    g_st.state = s;
}

void kl_hostvideo_set_host_present(int present) {
    pthread_mutex_lock(&g_hv_lock);
    g_host_present = present;
    pthread_mutex_unlock(&g_hv_lock);
}

unsigned kl_hostvideo_get_request(kl_hostvideo_request *out) {
    pthread_mutex_lock(&g_hv_lock);
    if (out) *out = g_req;
    unsigned g = g_req.gen;
    pthread_mutex_unlock(&g_hv_lock);
    return g;
}

void kl_hostvideo_publish(unsigned gen, int state, long long duration_ms,
                          long long position_ms, int width, int height,
                          int stereo_mode, int is_playing) {
    pthread_mutex_lock(&g_hv_lock);
    if (gen == g_req.gen && g_req.active) {
        if (state == KL_HV_ENDED && g_st.state != KL_HV_ENDED)
            fprintf(stderr, "  [video] ended at %lld/%lld ms\n", position_ms, duration_ms);
        if ((width != g_st.w || height != g_st.h || stereo_mode != g_st.stereo) && width > 0)
            fprintf(stderr, "  [video] host item %dx%d stereo=%d duration=%lld ms\n",
                    width, height, stereo_mode, duration_ms);
        hv_set_state_locked(state);
        g_st.dur = duration_ms; g_st.pos = position_ms;
        g_st.w = width; g_st.h = height; g_st.stereo = stereo_mode;
        g_st.playing = is_playing;
    }
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_play(const char *guest_path, int layer_id) {
    char buf[1024];
    const char *host = guest_path ? kl_guest_path(guest_path, buf, sizeof buf) : NULL;
    if (!host) host = guest_path ? guest_path : "";
    struct stat st;
    int exists = host[0] && stat(host, &st) == 0;
    pthread_mutex_lock(&g_hv_lock);
    fprintf(stderr, "  [video] playVideo \"%s\" -> host \"%s\" layer=%d%s\n",
            guest_path ? guest_path : "(null)", host, layer_id,
            exists ? "" : " (FILE MISSING)");
    g_req.gen++;
    snprintf(g_req.path, sizeof g_req.path, "%s", host);
    g_req.layer_id = layer_id;
    g_req.active = 1;
    g_req.paused = 0;
    g_st.dur = g_st.pos = 0; g_st.w = g_st.h = 0; g_st.stereo = 0; g_st.playing = 0;
    // No file, or no compositor to play it: report ENDED so the guest moves on
    // (its pre-port behaviour) rather than waiting on a video that never comes.
    if (!exists || !g_host_present) {
        if (!g_host_present)
            fprintf(stderr, "  [video] error: no host video player in this build - skipping\n");
        g_st.state = KL_HV_IDLE;
        hv_set_state_locked(KL_HV_ENDED);
    } else {
        g_st.state = KL_HV_IDLE;
        hv_set_state_locked(KL_HV_BUFFERING);
        g_st.playing = 1;   // ExoPlayer playWhenReady
    }
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_stop(void) {
    pthread_mutex_lock(&g_hv_lock);
    if (g_req.active) fprintf(stderr, "  [video] stop\n");
    g_req.active = 0;
    g_req.path[0] = 0;
    g_st.playing = 0;
    hv_set_state_locked(KL_HV_IDLE);
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_set_paused(int paused) {
    pthread_mutex_lock(&g_hv_lock);
    if (g_req.paused != paused) fprintf(stderr, "  [video] %s\n", paused ? "pause" : "resume");
    g_req.paused = paused;
    if (g_req.active && g_st.state != KL_HV_ENDED) g_st.playing = !paused;
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_set_looping(int looping) {
    pthread_mutex_lock(&g_hv_lock);
    if (g_req.looping != looping) fprintf(stderr, "  [video] setLooping(%d)\n", looping);
    g_req.looping = looping;
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_seek(long long ms) {
    pthread_mutex_lock(&g_hv_lock);
    fprintf(stderr, "  [video] setPlaybackPosition(%lld ms)\n", ms);
    g_req.seek_ms = ms < 0 ? 0 : ms;
    g_req.seek_gen++;
    g_st.pos = g_req.seek_ms;
    pthread_mutex_unlock(&g_hv_lock);
}

void kl_hostvideo_set_speed(float speed) {
    pthread_mutex_lock(&g_hv_lock);
    if (g_req.speed != speed) fprintf(stderr, "  [video] setPlaybackSpeed(%g)\n", (double)speed);
    g_req.speed = speed > 0 ? speed : 1.0f;
    pthread_mutex_unlock(&g_hv_lock);
}

#define HV_GET(type, name, expr) \
    type name(void) { pthread_mutex_lock(&g_hv_lock); type v = (expr); \
                      pthread_mutex_unlock(&g_hv_lock); return v; }
HV_GET(int,       kl_hostvideo_state,      g_st.state)
HV_GET(int,       kl_hostvideo_is_playing, g_st.playing && g_st.state != KL_HV_ENDED)
HV_GET(long long, kl_hostvideo_duration,   g_st.dur)
HV_GET(long long, kl_hostvideo_position,   g_st.pos)
HV_GET(int,       kl_hostvideo_width,      g_st.w)
HV_GET(int,       kl_hostvideo_height,     g_st.h)
HV_GET(int,       kl_hostvideo_stereo,     g_st.stereo)
