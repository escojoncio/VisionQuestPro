// See kl_mediandk.h. The guest-facing half of the video path; kl_vtdec.c is the
// decoder.
//
// Three objects and the relationships between them:
//
//   AMediaCodec   owns a kl_vtdec, a pool of input buffers, and a set of output
//                 slots holding decoded frames the guest has been told about
//                 but has not yet released.
//   AImageReader  is the codec's output surface. Release-with-render moves a
//                 frame from an output slot into the reader's queue and wakes
//                 the reader's dispatch thread, which calls the guest's
//                 listener.
//   AImage        is one frame taken off that queue, and AHardwareBuffer is a
//                 refcounted handle on the same CVPixelBuffer that outlives it.
//
// The dispatch thread is not an implementation detail — see the note above
// reader_dispatch for why calling the listener inline would deadlock.
#include "kl_mediandk.h"

#include <fcntl.h>          // F_GETPATH, to name the fd a demux was asked for
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_ndk.h"
#include "kl_vtdec.h"
#include "kl_avdec.h"

void kl_unresolved_named(const char *name);     // kl_shim.c
static int64_t klm_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// The ABI's own numbers. Transcribed from <media/NdkMediaError.h>,
// <media/NdkMediaCodec.h> and <media/NdkImage.h> — the guest tests these
// exactly, so they are contract, not preference.
// The whole ladder, spelled out, because it is derived arithmetic in the header
// (`AMEDIA_ERROR_BASE - n`) and INVALID_PARAMETER was transcribed one step short
// — it was -10003, which is INVALID_OBJECT, a DIFFERENT named error in the same
// enum. Checked against the NDK's own <media/NdkMediaError.h>. No guest is known
// to distinguish the two (Steam Link's decoder path tests against AMEDIA_OK),
// but that is an absence of evidence rather than a proof, which is exactly why
// the comment above calls these contract rather than preference: a value that is
// wrong and never load-bearing is the kind that stays wrong until it is.
enum { AMEDIA_OK = 0,
       AMEDIA_ERROR_UNKNOWN           = -10000,
       AMEDIA_ERROR_MALFORMED         = -10001,
       AMEDIA_ERROR_UNSUPPORTED       = -10002,
       AMEDIA_ERROR_INVALID_OBJECT    = -10003,
       AMEDIA_ERROR_INVALID_PARAMETER = -10004,
       AMEDIA_ERROR_INVALID_OPERATION = -10005,
       AMEDIA_ERROR_END_OF_STREAM     = -10006,
       AMEDIA_ERROR_IO                = -10007,
       AMEDIA_ERROR_WOULD_BLOCK       = -10008,
       AMEDIA_DRM_NOT_PROVISIONED     = -20001,
       AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE   = -30001,
       AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED   = -30002,
       AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE     = -30003,
       AMEDIA_IMGREADER_CANNOT_UNLOCK_IMAGE   = -30004,
       AMEDIA_IMGREADER_IMAGE_NOT_LOCKED      = -30005 };
enum { AMEDIACODEC_INFO_TRY_AGAIN_LATER    = -1,
       AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
       AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3 };
enum { AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2,
       AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4 };

typedef struct {
    int32_t  offset;
    int32_t  size;
    int64_t  presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

// AImageReader_ImageListener, by layout: the guest builds one of these and
// hands us its address, and we read both fields out of it.
typedef void (*klm_image_cb)(void *context, void *reader);
typedef struct { void *context; klm_image_cb onImageAvailable; } klm_listener;

// ---------------------------------------------------------------------------
// AMediaFormat — a key/value bag, and nothing more.
//
// Values are held rather than interpreted. The only key we read is "mime", to
// pick a decoder; width/height come from the bitstream's own parameter sets,
// which is more authoritative than anything the guest can tell us, and the
// colour keys describe a conversion VideoToolbox is doing for us. Keeping the
// rest anyway means AMediaFormat_toString and the report can show what the
// guest asked for, which is how a wrong assumption gets noticed.
// Typed like AMessage is: a key set as an int64 is not an int32 and a getter of
// the other type answers "absent". The one deliberate leniency is the widening
// the original getters already did (an int32 reads back as int64/float), since
// Unity stores durationUs and reads it either way.
#define KLM_FMT_MAX 64
enum { KLM_K_INT32 = 0, KLM_K_STR, KLM_K_INT64, KLM_K_FLOAT, KLM_K_DOUBLE,
       KLM_K_SIZE, KLM_K_BUF, KLM_K_RECT };
typedef struct {
    struct {
        char *key; char *sval; int32_t ival; int is_str;
        int kind; int64_t i64; double dbl; uint8_t *buf; size_t blen; int32_t rect[4];
    } kv[KLM_FMT_MAX];
    int n;
    char *text;                 // AMediaFormat_toString's result, owned by the format
} klm_format;

static int klm_fmt_slot(klm_format *f, const char *key) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key)) return i;
    if (f->n == KLM_FMT_MAX) return -1;
    f->kv[f->n].key = strdup(key);
    return f->n++;
}

// A slot being overwritten with a different type must not keep the old one's
// heap data alive or its stale kind.
static void klm_fmt_reset(klm_format *f, int i, int kind) {
    free(f->kv[i].sval); f->kv[i].sval = NULL;
    free(f->kv[i].buf);  f->kv[i].buf = NULL;
    f->kv[i].blen = 0;
    f->kv[i].is_str = kind == KLM_K_STR;
    f->kv[i].kind = kind;
}

static void *klm_AMediaFormat_new(void) { return calloc(1, sizeof(klm_format)); }

static int klm_AMediaFormat_delete(void *fmt) {
    klm_format *f = fmt;
    if (!f) return AMEDIA_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < f->n; i++) { free(f->kv[i].key); free(f->kv[i].sval); free(f->kv[i].buf); }
    free(f->text);
    free(f);
    return AMEDIA_OK;
}

static void klm_AMediaFormat_setInt32(void *fmt, const char *key, int32_t v) {
    klm_format *f = fmt;
    if (!f || !key) return;
    int i = klm_fmt_slot(f, key);
    if (i < 0) return;
    klm_fmt_reset(f, i, KLM_K_INT32);
    f->kv[i].ival = v;
}

static void klm_AMediaFormat_setString(void *fmt, const char *key, const char *v) {
    klm_format *f = fmt;
    if (!f || !key) return;
    int i = klm_fmt_slot(f, key);
    if (i < 0) return;
    // Copied, not pointed at: the guest builds these strings on its own stack
    // and in its own std::string temporaries.
    klm_fmt_reset(f, i, KLM_K_STR);
    f->kv[i].sval = v ? strdup(v) : NULL;
}

static const char *klm_fmt_string(klm_format *f, const char *key) {
    if (!f) return NULL;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key) && f->kv[i].is_str) return f->kv[i].sval;
    return NULL;
}

// The getters. `false` means "this format does not carry that key", and the NDK
// contract is that the out parameter is then LEFT ALONE — the caller keeps its
// own default. Writing a zero on the false path hands back a value the caller
// never asked for, indistinguishable from a measurement.
//
// Nothing here ever carries a key today (see the extractor below: it publishes
// no tracks, so no track format is ever built), which makes every one of these
// a truthful "no" rather than a stub. They exist because a format whose owner
// cannot fill it must still be ASKABLE — a guest querying an empty format is
// doing the right thing and must get an answer, not an abort.
static int klm_fmt_find(klm_format *f, const char *key) {
    if (!f || !key) return -1;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key)) return i;
    return -1;
}
static bool klm_AMediaFormat_getInt32(void *fmt, const char *key, int32_t *out) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || !out || f->kv[i].kind != KLM_K_INT32) return false;
    *out = f->kv[i].ival;
    return true;
}
static bool klm_AMediaFormat_getInt64(void *fmt, const char *key, int64_t *out) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || !out) return false;
    if (f->kv[i].kind == KLM_K_INT64) { *out = f->kv[i].i64; return true; }
    if (f->kv[i].kind == KLM_K_INT32) { *out = f->kv[i].ival; return true; }
    return false;
}
static bool klm_AMediaFormat_getFloat(void *fmt, const char *key, float *out) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || !out) return false;
    if (f->kv[i].kind == KLM_K_FLOAT)  { *out = (float)f->kv[i].dbl; return true; }
    if (f->kv[i].kind == KLM_K_INT32)  { *out = (float)f->kv[i].ival; return true; }
    return false;
}
static bool klm_AMediaFormat_getDouble(void *fmt, const char *key, double *out) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || !out) return false;
    if (f->kv[i].kind == KLM_K_DOUBLE || f->kv[i].kind == KLM_K_FLOAT) { *out = f->kv[i].dbl; return true; }
    return false;
}
static bool klm_AMediaFormat_getSize(void *fmt, const char *key, size_t *out) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || !out || f->kv[i].kind != KLM_K_SIZE) return false;
    *out = (size_t)f->kv[i].i64;
    return true;
}
static bool klm_AMediaFormat_getBuffer(void *fmt, const char *key, void **data, size_t *size) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i < 0 || f->kv[i].kind != KLM_K_BUF) return false;
    if (data) *data = f->kv[i].buf;
    if (size) *size = f->kv[i].blen;
    return true;
}
static void klm_AMediaFormat_setInt64(void *fmt, const char *key, int64_t v) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_INT64); f->kv[i].i64 = v;
}
static void klm_AMediaFormat_setFloat(void *fmt, const char *key, float v) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_FLOAT); f->kv[i].dbl = v;
}
static void klm_AMediaFormat_setDouble(void *fmt, const char *key, double v) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_DOUBLE); f->kv[i].dbl = v;
}
static void klm_AMediaFormat_setSize(void *fmt, const char *key, size_t v) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_SIZE); f->kv[i].i64 = (int64_t)v;
}
static void klm_AMediaFormat_setBuffer(void *fmt, const char *key, const void *data, size_t size) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_BUF);
    f->kv[i].buf = size ? malloc(size) : NULL;      // copied: the guest's csd lives on its stack
    if (f->kv[i].buf && data) { memcpy(f->kv[i].buf, data, size); f->kv[i].blen = size; }
}
static void klm_AMediaFormat_setRect(void *fmt, const char *key, int32_t l, int32_t t,
                                     int32_t r, int32_t b) {
    klm_format *f = fmt; int i;
    if (!f || !key || (i = klm_fmt_slot(f, key)) < 0) return;
    klm_fmt_reset(f, i, KLM_K_RECT);
    f->kv[i].rect[0] = l; f->kv[i].rect[1] = t; f->kv[i].rect[2] = r; f->kv[i].rect[3] = b;
}
// "crop" (API 28) is a rect. A decoder reports it as the four crop-* ints
// (MediaCodec's own output format on older releases); a rect the guest stored
// itself is read back as stored.
static bool klm_AMediaFormat_getRect(void *fmt, const char *key, int32_t *l, int32_t *t,
                                     int32_t *r, int32_t *b) {
    klm_format *f = fmt;
    int i = klm_fmt_find(f, key);
    if (i >= 0 && f->kv[i].kind == KLM_K_RECT) {
        if (l) *l = f->kv[i].rect[0];
        if (t) *t = f->kv[i].rect[1];
        if (r) *r = f->kv[i].rect[2];
        if (b) *b = f->kv[i].rect[3];
        return true;
    }
    if (!key || strcmp(key, "crop")) return false;
    int32_t v[4];
    if (!klm_AMediaFormat_getInt32(fmt, "crop-left", &v[0]) ||
        !klm_AMediaFormat_getInt32(fmt, "crop-top", &v[1]) ||
        !klm_AMediaFormat_getInt32(fmt, "crop-right", &v[2]) ||
        !klm_AMediaFormat_getInt32(fmt, "crop-bottom", &v[3])) return false;
    if (l) *l = v[0];
    if (t) *t = v[1];
    if (r) *r = v[2];
    if (b) *b = v[3];
    return true;
}
// Deep copy; the destination keeps nothing of what it held (AMessage::extend
// would merge, but the NDK documents a copy).
static int klm_AMediaFormat_copy(void *to, const void *from) {
    klm_format *d = to; const klm_format *s = from;
    if (!d || !s) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (d == s) return AMEDIA_OK;
    for (int i = 0; i < d->n; i++) { free(d->kv[i].key); free(d->kv[i].sval); free(d->kv[i].buf); }
    memset(d->kv, 0, sizeof d->kv);
    d->n = s->n;
    for (int i = 0; i < s->n; i++) {
        d->kv[i] = s->kv[i];
        d->kv[i].key = strdup(s->kv[i].key);
        d->kv[i].sval = s->kv[i].sval ? strdup(s->kv[i].sval) : NULL;
        d->kv[i].buf = NULL;
        if (s->kv[i].buf && s->kv[i].blen) {
            d->kv[i].buf = malloc(s->kv[i].blen);
            if (d->kv[i].buf) memcpy(d->kv[i].buf, s->kv[i].buf, s->kv[i].blen);
            else d->kv[i].blen = 0;
        }
    }
    return AMEDIA_OK;
}
// "{key: type(value), ...}", owned by the format and replaced on the next call —
// AMediaFormat_toString's own lifetime rule.
static const char *klm_AMediaFormat_toString(void *fmt) {
    klm_format *f = fmt;
    if (!f) return NULL;
    size_t cap = 64;
    for (int i = 0; i < f->n; i++)
        cap += strlen(f->kv[i].key) + (f->kv[i].sval ? strlen(f->kv[i].sval) : 0) + 64;
    char *t = malloc(cap), *w = t;
    if (!t) return NULL;
    w += snprintf(w, cap, "{");
    for (int i = 0; i < f->n; i++) {
        size_t left = cap - (size_t)(w - t);
        const char *sep = i ? ", " : "";
        switch (f->kv[i].kind) {
        case KLM_K_STR:    w += snprintf(w, left, "%s%s: string(%s)", sep, f->kv[i].key, f->kv[i].sval ? f->kv[i].sval : ""); break;
        case KLM_K_INT64:  w += snprintf(w, left, "%s%s: int64(%lld)", sep, f->kv[i].key, (long long)f->kv[i].i64); break;
        case KLM_K_FLOAT:  w += snprintf(w, left, "%s%s: float(%g)", sep, f->kv[i].key, f->kv[i].dbl); break;
        case KLM_K_DOUBLE: w += snprintf(w, left, "%s%s: double(%g)", sep, f->kv[i].key, f->kv[i].dbl); break;
        case KLM_K_SIZE:   w += snprintf(w, left, "%s%s: size(%lld)", sep, f->kv[i].key, (long long)f->kv[i].i64); break;
        case KLM_K_BUF:    w += snprintf(w, left, "%s%s: Buffer(%zu bytes)", sep, f->kv[i].key, f->kv[i].blen); break;
        case KLM_K_RECT:   w += snprintf(w, left, "%s%s: rect(%d, %d, %d, %d)", sep, f->kv[i].key, f->kv[i].rect[0], f->kv[i].rect[1], f->kv[i].rect[2], f->kv[i].rect[3]); break;
        default:           w += snprintf(w, left, "%s%s: int32(%d)", sep, f->kv[i].key, f->kv[i].ival); break;
        }
    }
    snprintf(w, cap - (size_t)(w - t), "}");
    free(f->text);
    f->text = t;
    return t;
}
static bool klm_AMediaFormat_getString(void *fmt, const char *key, const char **out) {
    if (!out) return false;
    const char *s = klm_fmt_string(fmt, key);
    if (!s) return false;
    *out = s;
    return true;
}

// The format-key constants. These are DATA symbols in the NDK
// (`extern const char *AMEDIAFORMAT_KEY_MIME;`), so the shim exports the
// ADDRESS of each pointer and the guest loads through it — get this wrong and
// the guest passes a string that is really a pointer-to-pointer, which lands as
// a key nobody matches rather than as a crash.
static const char *g_key_mime            = "mime";
static const char *g_key_width           = "width";
static const char *g_key_height          = "height";
static const char *g_key_max_width       = "max-width";
static const char *g_key_max_height      = "max-height";
static const char *g_key_color_transfer  = "color-transfer";
static const char *g_key_color_standard  = "color-standard";
static const char *g_key_color_range     = "color-range";
static const char *g_key_color_format    = "color-format";
static const char *g_key_priority        = "priority";
static const char *g_key_operating_rate  = "operating-rate";
// ...and the ones only a DEMUXING guest names. Unity's VideoPlayer READS a
// track's format rather than describing one, so these arrive through the
// getters below rather than the setters above.
// Only the ones libunity imports STRONGLY. It imports rotation-degrees,
// slice-height and encoder-delay weakly, i.e. it is written to run on an
// Android version that does not define them and checks for NULL first — so
// defining them here would not be completeness, it would be turning on a code
// path the guest asked to be able to skip.
static const char *g_key_duration        = "durationUs";
static const char *g_key_frame_rate      = "frame-rate";
static const char *g_key_language        = "language";
static const char *g_key_channel_count   = "channel-count";
static const char *g_key_sample_rate     = "sample-rate";
static const char *g_key_stride          = "stride";
static const char *g_key_rotation        = "rotation-degrees";
static const char *g_key_encoder_delay   = "encoder-delay";
static const char *g_key_slice_height    = "slice-height";
// The rest of <media/NdkMediaFormat.h> up to API 32 (Android 12L, the Quest 3's).
// A guest that imports one of these weakly checks for NULL first, so defining
// them only turns on code Android itself runs.
static const char *g_key_aac_drc_attenuation_factor = "aac-drc-cut-level";
static const char *g_key_aac_drc_boost_factor = "aac-drc-boost-level";
static const char *g_key_aac_drc_heavy_compression = "aac-drc-heavy-compression";
static const char *g_key_aac_drc_target_reference_level = "aac-target-ref-level";
static const char *g_key_aac_encoded_target_level = "aac-encoded-target-level";
static const char *g_key_aac_max_output_channel_count = "aac-max-output-channel_count";
static const char *g_key_aac_profile = "aac-profile";
static const char *g_key_audio_session_id = "audio-session-id";
static const char *g_key_bitrate_mode = "bitrate-mode";
static const char *g_key_bit_rate = "bitrate";
static const char *g_key_capture_rate = "capture-rate";
static const char *g_key_channel_mask = "channel-mask";
static const char *g_key_complexity = "complexity";
static const char *g_key_csd = "csd";
static const char *g_key_csd_0 = "csd-0";
static const char *g_key_csd_1 = "csd-1";
static const char *g_key_csd_2 = "csd-2";
static const char *g_key_display_crop = "crop";
static const char *g_key_display_height = "display-height";
static const char *g_key_display_width = "display-width";
static const char *g_key_frame_count = "frame-count";
static const char *g_key_grid_columns = "grid-cols";
static const char *g_key_grid_rows = "grid-rows";
static const char *g_key_hdr10_plus_info = "hdr10-plus-info";
static const char *g_key_hdr_static_info = "hdr-static-info";
static const char *g_key_is_adts = "is-adts";
static const char *g_key_is_autoselect = "is-autoselect";
static const char *g_key_is_default = "is-default";
static const char *g_key_is_forced_subtitle = "is-forced-subtitle";
static const char *g_key_i_frame_interval = "i-frame-interval";
static const char *g_key_latency = "latency";
static const char *g_key_level = "level";
static const char *g_key_location = "location";
static const char *g_key_loop = "loop";
static const char *g_key_low_latency = "low-latency";
static const char *g_key_max_bit_rate = "max-bitrate";
static const char *g_key_max_fps_to_encoder = "max-fps-to-encoder";
static const char *g_key_max_input_size = "max-input-size";
static const char *g_key_max_pts_gap_to_encoder = "max-pts-gap-to-encoder";
static const char *g_key_mpeg2_stream_header = "mpeg2-stream-header";
static const char *g_key_mpegh_compatible_sets = "mpegh-compatible-sets";
static const char *g_key_mpegh_profile_level_indication = "mpegh-profile-level-indication";
static const char *g_key_mpegh_reference_channel_layout = "mpegh-reference-channel-layout";
static const char *g_key_pcm_encoding = "pcm-encoding";
static const char *g_key_profile = "profile";
static const char *g_key_pssh = "pssh";
static const char *g_key_push_blank_buffers_on_stop = "push-blank-buffers-on-shutdown";
static const char *g_key_quality = "quality";
static const char *g_key_repeat_previous_frame_after = "repeat-previous-frame-after";
static const char *g_key_sar_height = "sar-height";
static const char *g_key_sar_width = "sar-width";
static const char *g_key_sei = "sei";
static const char *g_key_temporal_layer_id = "temporal-layer-id";
static const char *g_key_temporal_layering = "ts-schema";
static const char *g_key_text_format_data = "text-format-data";
static const char *g_key_thumbnail_csd_hevc = "thumbnail-csd-hevc";
static const char *g_key_thumbnail_height = "thumbnail-height";
static const char *g_key_thumbnail_time = "thumbnail-time";
static const char *g_key_thumbnail_width = "thumbnail-width";
static const char *g_key_time_us = "timeUs";
static const char *g_key_title = "title";
static const char *g_key_track_id = "track-id";
static const char *g_key_track_index = "track-index";
static const char *g_key_valid_samples = "valid-samples";
static const char *g_key_year = "year";
static const char *g_key_exif_offset = "exif-offset";
static const char *g_key_exif_size = "exif-size";
static const char *g_key_icc_profile = "icc-profile";
static const char *g_key_sample_file_offset = "sample-file-offset";
static const char *g_key_last_sample_index_in_chunk = "last-sample-index-in-chunk";
static const char *g_key_create_input_surface_suspended = "create-input-buffers-suspended";

// ---------------------------------------------------------------------------
// AMediaExtractor — the DEMUXER, and the one part of this file that refuses.
//
// Unity's VideoPlayer on Android is extractor + codec: AMediaExtractor pulls
// compressed samples out of a container and hands them to AMediaCodec, which
// this file already serves over VideoToolbox. The codec half exists because
// Steam Link needed it and it arrives an elementary stream, already
// demuxed by the guest's own network protocol. Nothing here has ever opened a
// CONTAINER, and that is the whole gap: an .mp4 is a box structure that must be
// parsed to find where each sample begins.
//
// Open Brush is the first guest to reach it. It ships `animated-logo.mp4` and
// SEEDS it into its Media Library on first run, so its VideoCatalog scan
// prepares a VideoPlayer during startup whether or not the user ever opens the
// reference panel. Before this, that call was `AMediaExtractor_new` as an
// unresolved import, i.e. an abort — which is correct for something nobody has
// asked for yet, and wrong for something on the startup path of a working
// guest.
//
// So this is a REFUSAL, not a stub, and the difference is that a refusal is a
// legal answer the guest is already written to handle. A file it cannot demux
// is an ordinary condition for a media player — a corrupt download, a codec the
// device lacks — so VideoPlayer has an error path, reports through
// `errorReceived`, and Open Brush's ReferenceVideo simply leaves
// m_VideoInitialized false. The refusal is stated twice, at both points a
// caller might check: setDataSource returns UNSUPPORTED, and if the caller
// carries on regardless, the extractor then honestly has no tracks. Those two
// have to agree — an extractor that failed to open and then claims a track is a
// worse answer than either.
//
// What it would take to make real, recorded so the next person does not have to
// derive it: AVAssetReader over an AVURLAsset, with nil output settings so the
// samples come out still compressed, feeding the AMediaCodec path that is
// already here. That is the same split kl_vtdec/kl_mediandk already uses. It is
// a session of its own and it would land Unity's VideoPlayer for EVERY target
// (Beat Saber imports the same 41 symbols), which is why it is written down
// rather than half-started.
#define KLM_EXTRACTOR_MAGIC 0x4b4c4d58u   /* 'KLMX' */
typedef struct {
    uint32_t magic;
    char    *source;        // for the report: what it was asked to open
    kl_avdemux *dm;         // the demuxer, when a container was opened (else NULL)
    int      spooled;       // `source` is a temp file of ours (setDataSourceCustom): unlink on delete
    int      unselected;    // the guest unselected the (only) track: no more samples until reselected
} klm_extractor;

static klm_extractor *klm_ex(void *ex) {
    klm_extractor *x = ex;
    return (x && x->magic == KLM_EXTRACTOR_MAGIC) ? x : NULL;
}

// Counted so the end-of-run report can say a video was wanted and refused,
// rather than leaving "no picture" to be discovered by looking at the screen.
// The name outlives the extractor, which the guest deletes as soon as it gives
// up — so it is copied here rather than read back off an object at report time.
static unsigned g_ex_refused;
static char    *g_ex_last;

static void *klm_AMediaExtractor_new(void) {
    klm_extractor *x = calloc(1, sizeof *x);
    if (x) x->magic = KLM_EXTRACTOR_MAGIC;
    return x;
}

static int klm_AMediaExtractor_delete(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (x->dm) { kl_avdemux_close(x->dm); x->dm = NULL; }
    if (x->spooled && x->source) unlink(x->source);
    free(x->source);
    x->magic = 0;
    free(x);
    return AMEDIA_OK;
}

static int klm_extractor_refuse(klm_extractor *x, const char *what) {
    free(x->source);
    x->source = what ? strdup(what) : NULL;
    free(g_ex_last);
    g_ex_last = what ? strdup(what) : NULL;
    if (!g_ex_refused++)
        fprintf(stderr, "  [media] AMediaExtractor: no container demuxer here — "
                        "refusing \"%s\". The guest's own no-video path runs from "
                        "here (see the note in kl_mediandk.c).\n",
                what ? what : "(fd)");
    return AMEDIA_ERROR_UNSUPPORTED;
}

// KL_MEDIA_EXTRACTOR=0: refuse every extractor source politely, so a guest whose
// video pipeline wedges a load (zix: a Unity VideoPlayer pump thread spinning in
// AMediaExtractor_getSampleTrackIndex/advance while the hub scene never
// activates) errors its VideoPlayer and proceeds without the video. Diagnostic
// A/B first, per-target default only if it proves out — default ON (1) serves
// video exactly as before for everyone.
static int klm_extractor_enabled(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_MEDIA_EXTRACTOR", 1);
    return on;
}

static int klm_AMediaExtractor_setDataSource(void *ex, const char *location) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, location);
    if (location && (!strncmp(location, "http://", 7) || !strncmp(location, "https://", 8) ||
                     !strncmp(location, "rtsp://", 7) || !strncmp(location, "rtp://", 6) ||
                     !strncmp(location, "udp://", 6) || !strncmp(location, "ftp://", 6))) {
        klm_extractor_refuse(x, location);
        return AMEDIA_ERROR_IO;                 // offline: a stream that cannot be reached
    }
    if (location) {
        if (!strncmp(location, "file://", 7)) location += 7;
        x->dm = kl_avdemux_open(location, 0, 0);
        if (x->dm) {
            free(x->source); x->source = strdup(location);
            fprintf(stderr, "  [media] AMediaExtractor: demuxing \"%s\"\n", location);
            return AMEDIA_OK;
        }
    }
    return klm_extractor_refuse(x, location);
}

static int klm_AMediaExtractor_setDataSourceFd(void *ex, int fd, int64_t offset, int64_t length) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, NULL);
    // The fd's own path — the video lives at [offset, offset+length) inside it
    // (a subfile of the OBB, usually). kl_avdemux slices that range out and
    // demuxes it; the codec half (AMediaCodec -> kl_vtdec) decodes the Annex-B.
    char path[1024];
    int named = fd >= 0 && fcntl(fd, F_GETPATH, path) == 0;
    if (named) {
        x->dm = kl_avdemux_open(path, (long long)offset, (long long)length);
        if (x->dm) {
            free(x->source); x->source = strdup(path);
            fprintf(stderr, "  [media] AMediaExtractor: demuxing \"%s\" [%lld+%lld]\n",
                    path, (long long)offset, (long long)length);
            return AMEDIA_OK;
        }
    }
    return klm_extractor_refuse(x, named ? path : NULL);
}

// ---------------------------------------------------------------------------
// AMediaDataSource — a container the GUEST reads for us (API 28).
//
// Unity's VideoPlayer takes this route instead of setDataSourceFd whenever the
// movie is not a plain span of a file it can hand over as (fd, offset, length):
// it builds a data source, installs readAt/getSize/close callbacks over its own
// file abstraction, and gives it to the extractor. Arkham Shadow's first
// in-game tutorial clip (the glide prompt) is the first title here to do it,
// and all seven symbols were WEAK imports left NULL — so the call to
// AMediaDataSource_new was a branch to address 0, reported by visionOS as
// "CODESIGNING, Invalid Page" with nothing in our own log.
//
// kl_avdemux reads files, so the source is drained into one: TMPDIR, the same
// cache slice_out() uses and for the same reasons, keyed on the size the guest
// reports. The callbacks are guest code, called here on the guest's own thread
// inside setDataSourceCustom — which is where Android's MediaExtractor does its
// first reads too, so nothing about threading is new to the guest.
#define KLM_DATASOURCE_MAGIC 0x4b4c4d44u   /* 'KLMD' */
typedef ssize_t (*klm_ds_read_at)(void *userdata, off_t offset, void *buffer, size_t size);
typedef ssize_t (*klm_ds_get_size)(void *userdata);
typedef void    (*klm_ds_close)(void *userdata);
typedef ssize_t (*klm_ds_get_avail)(void *userdata, off_t offset);
typedef struct {
    uint32_t        magic;
    void           *userdata;
    klm_ds_read_at  read_at;
    klm_ds_get_size get_size;
    klm_ds_close    close;
    klm_ds_get_avail get_avail;  // API 34: how much past `offset` is readable now
    char           *uri;         // AMediaDataSource_newUri: no guest callbacks, the URI is the source
    int             closed;      // the close callback runs once, from close() or delete()
} klm_datasource;

static klm_datasource *klm_ds(void *p) {
    klm_datasource *d = p;
    return (d && d->magic == KLM_DATASOURCE_MAGIC) ? d : NULL;
}
static void *klm_AMediaDataSource_new(void) {
    klm_datasource *d = calloc(1, sizeof *d);
    if (d) d->magic = KLM_DATASOURCE_MAGIC;
    return d;
}
static void klm_AMediaDataSource_delete(void *p) {
    klm_datasource *d = klm_ds(p);
    if (!d) return;
    if (d->close && !d->closed) d->close(d->userdata);   // the guest's own cleanup of its stream
    d->magic = 0;
    free(d->uri);
    free(d);
}
static void klm_AMediaDataSource_setUserdata(void *p, void *userdata) {
    klm_datasource *d = klm_ds(p);
    if (d) d->userdata = userdata;
}
static void klm_AMediaDataSource_setReadAt(void *p, klm_ds_read_at fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->read_at = fn;
}
static void klm_AMediaDataSource_setGetSize(void *p, klm_ds_get_size fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->get_size = fn;
}
static void klm_AMediaDataSource_setClose(void *p, klm_ds_close fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->close = fn;
}

// A location the device can only reach over the network. This runtime is
// offline by design, and Android's own answer to a stream it cannot open is an
// I/O error, which is what a guest's player already handles.
static int klm_is_remote_uri(const char *u) {
    return u && (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8) ||
                 !strncmp(u, "rtsp://", 7) || !strncmp(u, "rtp://", 6) ||
                 !strncmp(u, "udp://", 6) || !strncmp(u, "ftp://", 6));
}

static int klm_AMediaExtractor_setDataSourceCustom(void *ex, void *src) {
    klm_extractor *x = klm_ex(ex);
    klm_datasource *d = klm_ds(src);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!d || (!d->uri && (!d->read_at || !d->get_size))) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (x->dm) { kl_avdemux_close(x->dm); x->dm = NULL; }
    if (x->spooled && x->source) { unlink(x->source); x->spooled = 0; }
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, "(custom data source)");
    if (d->uri) {
        // newUri: a source the framework opens itself. A file:// URI or a bare
        // path is a local file; anything networked is refused as offline.
        if (klm_is_remote_uri(d->uri)) {
            klm_extractor_refuse(x, d->uri);
            return AMEDIA_ERROR_IO;
        }
        const char *path = !strncmp(d->uri, "file://", 7) ? d->uri + 7 : d->uri;
        if (strstr(path, "://")) return klm_extractor_refuse(x, d->uri);   // content:// etc.
        x->dm = kl_avdemux_open(path, 0, 0);
        if (!x->dm) return klm_extractor_refuse(x, d->uri);
        free(x->source); x->source = strdup(path);
        fprintf(stderr, "  [media] AMediaExtractor: demuxing \"%s\" (data source URI)\n", path);
        return AMEDIA_OK;
    }

    long long size = (long long)d->get_size(d->userdata);
    // 2 GiB is far past any clip a title streams this way, and a negative or
    // absurd size is the guest's "unknown length" — a stream, which a file
    // demuxer cannot serve.
    if (size <= 0 || size > (2LL << 30)) {
        fprintf(stderr, "  [media] AMediaExtractor_setDataSourceCustom: the source "
                        "reports %lld bytes — refused\n", size);
        return klm_extractor_refuse(x, "(custom data source)");
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    static unsigned seq;
    char path[1024];
    snprintf(path, sizeof path, "%s/klepton-media-ds-%d-%u-%lld.mp4", tmp,
             (int)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), size);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return klm_extractor_refuse(x, "(custom data source)");
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    long long done = 0;
    int ok = buf != NULL;
    while (ok && done < size) {
        size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
        ssize_t got = d->read_at(d->userdata, (off_t)done, buf, want);
        // 0 or negative before the stated size is a truncated container, and
        // has to fail here: AVFoundation would refuse it later for reasons that
        // name nothing.
        if (got <= 0 || (size_t)got > want) { ok = 0; break; }
        if (write(fd, buf, (size_t)got) != got) { ok = 0; break; }
        done += got;
    }
    free(buf);
    close(fd);
    if (ok) x->dm = kl_avdemux_open(path, 0, 0);
    // The demuxer holds the asset by URL and reads it lazily, so the file stays
    // until the process's TMPDIR is reclaimed; a failed spool is removed now.
    if (!ok || !x->dm) {
        unlink(path);
        fprintf(stderr, "  [media] AMediaExtractor_setDataSourceCustom: %s after "
                        "%lld of %lld bytes\n",
                ok ? "the demuxer refused the container" : "the source stopped",
                done, size);
        return klm_extractor_refuse(x, "(custom data source)");
    }
    free(x->source); x->source = strdup(path); x->spooled = 1;
    fprintf(stderr, "  [media] AMediaExtractor: demuxing a custom data source, "
                    "%lld bytes spooled to \"%s\"\n", size, path);
    return AMEDIA_OK;
}

// Everything past the refusal describes an extractor with nothing in it. These
// are the values the NDK defines for "no more samples" / "no such track", not
// invented ones, so a guest that ignored the setDataSource status still walks a
// consistent empty stream rather than reading uninitialised memory.
static size_t klm_AMediaExtractor_getTrackCount(void *ex) {
    klm_extractor *x = klm_ex(ex);
    return (x && x->dm) ? 1 : 0;
}
static void *klm_AMediaExtractor_getTrackFormat(void *ex, size_t i) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || i != 0) return NULL;
    int w = 0, h = 0; const char *mime = NULL; long long dur = 0; float fps = 0;
    kl_avdemux_info(x->dm, &w, &h, &mime, &dur, &fps);
    klm_format *f = klm_AMediaFormat_new();
    if (!f) return NULL;
    klm_AMediaFormat_setString(f, g_key_mime, mime ? mime : "video/avc");
    klm_AMediaFormat_setInt32(f, g_key_width, w);
    klm_AMediaFormat_setInt32(f, g_key_height, h);
    klm_AMediaFormat_setInt32(f, g_key_duration, (int32_t)dur);
    // Frame rate: without it Unity's AndroidVideoMedia::GetFormatInfo cannot
    // read a rate from the format, falls back to timing the first two samples,
    // gets a zero delta on our probe path, logs "Unable to detect video frame
    // rate", and tears the VideoPlayer down before real playback ever starts —
    // which is why the world was black. nominalFrameRate gives it directly.
    if (fps > 0)
        klm_AMediaFormat_setInt32(f, g_key_frame_rate, (int32_t)(fps + 0.5f));
    // Unity sizes its codec input buffers from max-input-size; without it the
    // allocation is 0/garbage and the guest null-derefs in its allocator right
    // after opening the video. A keyframe (with the parameter sets we inject) is
    // well under this bound.
    if (w > 0 && h > 0)
        klm_AMediaFormat_setInt32(f, "max-input-size", w * h);
    return f;
}
static int klm_AMediaExtractor_selectTrack(void *ex, size_t i) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || i != 0) return AMEDIA_ERROR_INVALID_PARAMETER;
    x->unselected = 0;
    return AMEDIA_OK;               // the single video track is already reading
}
static ssize_t klm_AMediaExtractor_readSampleData(void *ex, uint8_t *buf, size_t cap) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected) return -1;
    return (ssize_t)kl_avdemux_read(x->dm, buf, (unsigned long)cap);
}
static int klm_AMediaExtractor_getSampleTrackIndex(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected) return -1;
    return kl_avdemux_sample_time_us(x->dm) < 0 ? -1 : 0;
}
static int64_t klm_AMediaExtractor_getSampleTime(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected) return -1;
    return (int64_t)kl_avdemux_sample_time_us(x->dm);
}
static bool klm_AMediaExtractor_advance(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return false;
    return kl_avdemux_advance(x->dm) != 0;
}
static int klm_AMediaExtractor_seekTo(void *ex, int64_t us, int mode) {
    (void)mode;
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return AMEDIA_ERROR_UNSUPPORTED;
    return kl_avdemux_seek_us(x->dm, (long long)us) ? AMEDIA_OK : AMEDIA_ERROR_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// AHardwareBuffer — a refcounted handle on one CVPixelBuffer.

// Two kinds share the handle. A DECODER's buffer wraps the CVPixelBuffer it was
// decoded into (pixels != NULL) and is what EGL imports. One the guest allocated
// itself with AHardwareBuffer_allocate is plain CPU memory (mem != NULL): the
// CPU-side contract (describe/lock/unlock) is complete, but no GPU API can see
// it, since the graphics side only maps decoder frames.
typedef struct {
    uint32_t magic;
    int      refs;
    void    *pixels;            // CVPixelBufferRef, retained (decoder frames)
    void    *mem;               // guest-allocated storage (AHardwareBuffer_allocate)
    uint32_t width, height, layers, format;
    uint64_t usage, id;
    uint32_t stride_px;         // allocated buffers: row length in pixels
    int      locked;
} klm_hwbuf;

#define KLM_HWBUF_MAGIC 0x4b4c4842u    /* 'KLHB' */

// CoreVideo through function pointers would be silly here; kl_vtdec.h already
// pulls CoreVideo in, and this file is Darwin-only for the same reason it is.
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>   // VTPixelTransferSession (CPU output path)

static klm_hwbuf *klm_hwbuf_new(CVPixelBufferRef pb) {
    klm_hwbuf *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->magic = KLM_HWBUF_MAGIC;
    h->refs = 1;
    h->pixels = (void *)CVPixelBufferRetain(pb);
    static uint64_t next_id;
    h->id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    return h;
}

static void klm_hwbuf_release(klm_hwbuf *h) {
    if (!h || h->magic != KLM_HWBUF_MAGIC) return;
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (h->pixels) CVPixelBufferRelease((CVPixelBufferRef)h->pixels);
    free(h->mem);
    h->magic = 0;
    free(h);
}

void *kl_mediandk_buffer_pixels(const void *buf) {
    const klm_hwbuf *h = buf;
    if (!h || h->magic != KLM_HWBUF_MAGIC) return NULL;
    return h->pixels;
}

static void klm_AHardwareBuffer_acquire(void *buf) {
    klm_hwbuf *h = buf;
    if (h && h->magic == KLM_HWBUF_MAGIC)
        __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
}
static void klm_AHardwareBuffer_release(void *buf) { klm_hwbuf_release(buf); }

// ---------------------------------------------------------------------------
// AImage — one frame, taken off the reader's queue.

typedef struct {
    uint32_t magic;
    void    *pixels;            // CVPixelBufferRef, retained
    int64_t  pts_us;
    klm_hwbuf *hw;              // made on demand, owned by the image
    int32_t  format;            // the reader's, for AImage_getFormat
    int      locked;            // pixels are locked for plane access
} klm_image;

#define KLM_IMAGE_MAGIC 0x4b4c494du    /* 'KLIM' */

static klm_image *klm_image_new(CVPixelBufferRef pb, int64_t pts_us) {
    klm_image *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->magic = KLM_IMAGE_MAGIC;
    im->pixels = (void *)CVPixelBufferRetain(pb);
    im->pts_us = pts_us;
    return im;
}

static void klm_AImage_delete(void *image) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC) return;
    // The hardware buffer deliberately outlives the image: the guest's own
    // sequence is acquire the image, take its buffer, AHardwareBuffer_acquire,
    // delete the image, and use the buffer for several frames afterwards
    // (QSVLFrameServer keeps a deque of them). Releasing our reference here and
    // letting the guest's keep it alive is exactly Android's contract.
    klm_hwbuf_release(im->hw);
    if (im->locked) CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)im->pixels, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease((CVPixelBufferRef)im->pixels);
    im->magic = 0;
    free(im);
}

static int klm_AImage_getWidth(void *image, int32_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = (int32_t)CVPixelBufferGetWidth((CVPixelBufferRef)im->pixels);
    return AMEDIA_OK;
}
static int klm_AImage_getHeight(void *image, int32_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = (int32_t)CVPixelBufferGetHeight((CVPixelBufferRef)im->pixels);
    return AMEDIA_OK;
}
// NANOseconds here, microseconds everywhere else in this file. AImage's
// timestamp is the one place the NDK changes unit, and the guest uses it to
// pace presentation — reporting microseconds would make every frame look 1000x
// too early and is the kind of thing that reads as a network problem.
static int klm_AImage_getTimestamp(void *image, int64_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = im->pts_us * 1000;
    return AMEDIA_OK;
}
// The frame path is six handoffs long — submit, dequeue, release(render),
// publish, acquire, hardware buffer — and any one of them not happening looks
// exactly like any other from outside: no picture. So each one says so the first
// time it happens, and the run's log then names the LAST rung reached rather
// than leaving the whole ladder to be inferred from an end-of-run count.
#define KLM_FIRST(flagvar, ...) do { \
        static int flagvar; \
        if (!flagvar++) { fprintf(stderr, "  [media] " __VA_ARGS__); } \
    } while (0)

static int klm_AImage_getHardwareBuffer(void *image, void **out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (!im->hw) im->hw = klm_hwbuf_new((CVPixelBufferRef)im->pixels);
    if (!im->hw) return AMEDIA_ERROR_UNKNOWN;
    *out = im->hw;
    KLM_FIRST(said_hw, "first AHardwareBuffer handed out (%p) — the guest can now "
                       "ask EGL for an image of it\n", (void *)im->hw);
    return AMEDIA_OK;
}

// ---------------------------------------------------------------------------
// AImageReader — the codec's output surface, and the queue behind it.

// The queue behind the reader. This guest asks for 20 images and its
// QSVLFrameServer really does hold a deque of them, so a pool sized to a
// plausible-looking 8 would silently drop frames at exactly the moment the
// stream is healthiest. Sized above the request, and the request is reported.
#define KLM_READER_MAX 32

typedef struct {
    uint32_t magic;
    int32_t  width, height, format;
    uint64_t usage;
    int32_t  max_images;

    void    *window;            // what getWindow hands out; a kl_ndk window

    klm_listener listener;      // copied out of the guest's struct
    int      have_listener;

    pthread_mutex_t lock;
    pthread_cond_t  cv;
    struct { void *pb; int64_t pts_us; } q[KLM_READER_MAX];
    int      head, count;
    unsigned n_queued, n_acquired, n_dropped;

    pthread_t dispatch;
    int       running;
    int       pending;          // listener calls owed to the guest

    // Host-side consumer (a java SurfaceTexture, kl_jni_hostvideo.c): called from
    // the dispatch thread, like the guest's listener, when a frame is published.
    void    (*host_cb)(void *ctx);
    void     *host_ctx;
    int       abandoned;        // SurfaceTexture.release(): publish drops frames
    uint8_t  *rgba;             // last take_rgba conversion (owned)
    size_t    rgba_cap;
    int       converting;       // take_rgba is using rgba outside the lock
    // take_rgba's hardware path for any frame that is not already BGRA: one
    // VTPixelTransferSession and one reused 32BGRA target (a pool of one: the
    // transfer is synchronous, so the target is free again on return). Touched
    // only by take_rgba and, under !converting, by abandon/delete.
    void     *xfer;             // VTPixelTransferSessionRef
    void     *xdst;             // CVPixelBufferRef, 32BGRA
    size_t    xw, xh;
} klm_reader;

#define KLM_READER_MAGIC 0x4b4c5244u   /* 'KLRD' */

static klm_reader *g_reader;        // for the report; this guest makes one
// Lifetime totals. The per-object counters die with the object, and the run
// where "did anything decode?" matters most is the CLEAN one — where the app
// shuts down and deletes both the codec and the reader before anything gets
// to report. These outlive that: an empty media report otherwise reads as "the
// decoder was never fed" when it only means "teardown got there first".
static unsigned g_life_in, g_life_out, g_life_rendered, g_life_published, g_life_acquired;
// ...and the one bit that says the report is worth printing at all: a codec or a
// reader existed at some point. Counters can legitimately all be zero, and that
// is the single most interesting thing a report can say.
static int g_ever_created;

// Android delivers onImageAvailable on a thread that is NOT the one that
// released the buffer, and that is load-bearing rather than incidental. This
// guest's HandleOnImageAvailable takes the same codec mutex its render loop
// holds across AMediaCodec_releaseOutputBuffer, so calling the listener inline
// from release would re-enter that mutex and wedge — the "a HandlerThread needs
// a real thread" rule, in a new subsystem. One thread per reader,
// woken by the queue.
static void *reader_dispatch(void *arg) {
    klm_reader *r = arg;
    // Guest code runs on this thread, so the per-thread TLS and veneer setup
    // has to happen before the first call into it.
    kl_thread_init();
    pthread_mutex_lock(&r->lock);
    while (r->running) {
        while (r->running && !r->pending)
            pthread_cond_wait(&r->cv, &r->lock);
        if (!r->running) break;
        r->pending--;
        klm_listener l = r->listener;
        int have = r->have_listener;
        void (*hcb)(void *) = r->host_cb;
        void *hctx = r->host_ctx;
        pthread_mutex_unlock(&r->lock);
        if (hcb) hcb(hctx);
        if (have && l.onImageAvailable) {
            KLM_FIRST(said_cb, "first onImageAvailable delivered to the guest\n");
            l.onImageAvailable(l.context, r);
        }
        pthread_mutex_lock(&r->lock);
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

static int klm_AImageReader_newWithUsage(int32_t w, int32_t h, int32_t format,
                                         uint64_t usage, int32_t max_images,
                                         void **out) {
    if (!out || max_images < 1) return AMEDIA_ERROR_INVALID_PARAMETER;
    klm_reader *r = calloc(1, sizeof *r);
    if (!r) return AMEDIA_ERROR_UNKNOWN;
    r->magic = KLM_READER_MAGIC;
    r->width = w; r->height = h; r->format = format;
    r->usage = usage;
    r->max_images = max_images < KLM_READER_MAX ? max_images : KLM_READER_MAX;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cv, NULL);

    // The window the codec will render into. Its size is the reader's for now
    // and is corrected the moment the decoder reports the real one — the guest
    // asks for 1x1 here deliberately (see the header), so believing it would
    // publish a 1x1 surface.
    r->window = kl_ndk_window_new(w, h, format, r);

    r->running = 1;
    if (pthread_create(&r->dispatch, NULL, reader_dispatch, r) != 0) {
        r->running = 0;
        fprintf(stderr, "  [media] AImageReader: could not start the dispatch "
                        "thread; onImageAvailable will never fire\n");
    }
    *out = r;
    g_reader = r;
    fprintf(stderr, "  [media] AImageReader %dx%d format 0x%x usage 0x%llx, "
                    "%d images\n", w, h, format, (unsigned long long)usage,
            max_images);
    return AMEDIA_OK;
}

static int klm_AImageReader_setImageListener(void *reader, void *listener) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&r->lock);
    if (listener) {
        // Copied out by value. The guest's own struct is a member of an object
        // it may move or destroy, and Android copies it too.
        r->listener = *(const klm_listener *)listener;
        r->have_listener = 1;
    } else {
        r->have_listener = 0;
    }
    pthread_mutex_unlock(&r->lock);
    return AMEDIA_OK;
}

static int klm_AImageReader_getWindow(void *reader, void **out) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = r->window;
    return AMEDIA_OK;
}

// "Latest" is the whole contract: hand back the newest frame and drop
// everything older, because for live video an older frame has no value and
// holding it only delays the next one. Dropping silently would read as a
// decoder that is behind, so the count is reported.
static int klm_AImageReader_acquireLatestImage(void *reader, void **out) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&r->lock);
    if (!r->count) { pthread_mutex_unlock(&r->lock); return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE; }
    while (r->count > 1) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
        r->n_dropped++;
    }
    void *pb = r->q[r->head].pb;
    int64_t pts = r->q[r->head].pts_us;
    r->head = (r->head + 1) % KLM_READER_MAX;
    r->count--;
    r->n_acquired++;
    pthread_mutex_unlock(&r->lock);
    KLM_FIRST(said_acq, "first acquireLatestImage (pts %lld us)\n", (long long)pts);

    klm_image *im = klm_image_new((CVPixelBufferRef)pb, pts);
    CVPixelBufferRelease((CVPixelBufferRef)pb);   // the image took its own
    if (!im) return AMEDIA_ERROR_UNKNOWN;
    im->format = r->format;
    *out = im;
    return AMEDIA_OK;
}

static void klm_reader_free_xfer(klm_reader *r) {
    if (r->xfer) {
        VTPixelTransferSessionInvalidate((VTPixelTransferSessionRef)r->xfer);
        CFRelease(r->xfer);
        r->xfer = NULL;
    }
    if (r->xdst) { CVPixelBufferRelease((CVPixelBufferRef)r->xdst); r->xdst = NULL; }
    r->xw = r->xh = 0;
}

static void klm_AImageReader_delete(void *reader) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->dispatch, NULL);
    while (r->count) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
    }
    kl_ndk_window_free(r->window);
    klm_reader_free_xfer(r);
    free(r->rgba);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cv);
    g_life_published += r->n_queued; g_life_acquired += r->n_acquired;
    if (g_reader == r) g_reader = NULL;
    r->magic = 0;
    free(r);
}

// Called by the codec when a frame is released with render=true.
static void klm_reader_publish(klm_reader *r, CVPixelBufferRef pb, int64_t pts_us) {
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    if (r->abandoned) { pthread_mutex_unlock(&r->lock); return; }
    if (r->count == KLM_READER_MAX) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
        r->n_dropped++;
    }
    int slot = (r->head + r->count) % KLM_READER_MAX;
    r->q[slot].pb = (void *)CVPixelBufferRetain(pb);
    r->q[slot].pts_us = pts_us;
    r->count++;
    r->n_queued++;
    r->pending++;
    // The window's size is the producer's, not the 1x1 the guest asked for.
    // Publishing it here rather than at configure time is what makes it the
    // decoder's answer.
    kl_ndk_window_set_size(r->window, (int32_t)CVPixelBufferGetWidth(pb),
                           (int32_t)CVPixelBufferGetHeight(pb));
    KLM_FIRST(said_pub, "first frame published to the reader: %zux%zu, pts %lld us "
                        "— onImageAvailable fires next\n",
              CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
              (long long)pts_us);
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->lock);
}


// ---------------------------------------------------------------------------
// Host-side surface: the consumer end of an AImageReader that is a java
// SurfaceTexture rather than guest native code. The codec renders into
// kl_media_surface_window() exactly as for a guest AImageReader; the host gets a
// callback per published frame and pulls the latest one as RGBA8 (the pixels
// SurfaceTexture.updateTexImage uploads to the guest's GL texture).
void *kl_media_surface_new(void (*on_frame)(void *), void *ctx) {
    klm_reader *keep = g_reader;
    void *out = NULL;
    if (klm_AImageReader_newWithUsage(1, 1, 0x22 /* PRIVATE */, 0, 8, &out) != AMEDIA_OK)
        return NULL;
    g_reader = keep;                      // the report's reader stays the guest's
    klm_reader *r = out;
    pthread_mutex_lock(&r->lock);
    r->host_cb = on_frame; r->host_ctx = ctx;
    pthread_mutex_unlock(&r->lock);
    return r;
}
void kl_media_surface_set_cb(void *reader, void (*on_frame)(void *), void *ctx) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    r->host_cb = on_frame; r->host_ctx = ctx;
    pthread_mutex_unlock(&r->lock);
}
void *kl_media_surface_window(void *reader) {
    klm_reader *r = reader;
    return (r && r->magic == KLM_READER_MAGIC) ? r->window : NULL;
}
void kl_media_surface_set_size(void *reader, int w, int h) {
    klm_reader *r = reader;
    if (r && r->magic == KLM_READER_MAGIC && w > 0 && h > 0) kl_ndk_window_set_size(r->window, w, h);
}
// SurfaceTexture.release(): no more callbacks or frames. The reader itself is
// NOT freed — a codec configured onto its window may still hold the pointer
// (AMediaCodec_configure keeps c->surface), and freeing it under the codec is a
// use-after-free; the struct (a few hundred bytes) and its parked state leak.
void kl_media_surface_abandon(void *reader) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    r->abandoned = 1; r->host_cb = NULL; r->have_listener = 0;
    r->running = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->dispatch, NULL);
    pthread_mutex_lock(&r->lock);
    while (r->count) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
    }
    if (!r->converting) {   // else take_rgba frees it
        free(r->rgba); r->rgba = NULL; r->rgba_cap = 0;
        klm_reader_free_xfer(r);
    }
    pthread_mutex_unlock(&r->lock);
}

static int klm_pb_is_nv12(CVPixelBufferRef pb);
typedef struct { int yoff, yk, rv, gu, gv, bu; } klm_yuv_k;
// Fixed point /256. Video range scales Y by 255/219; full range does not.
static const klm_yuv_k klm_k601v = {16, 298, 409, -100, -208, 516}, klm_k709v = {16, 298, 459, -55, -136, 541},
                       klm_k601f = {0, 256, 359, -88, -183, 454},   klm_k709f = {0, 256, 403, -48, -120, 475};
static inline uint8_t klm_clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

// Hardware YUV (or any other native layout) -> 32BGRA into the reader's reused
// target, so a 4K frame never goes through the CPU YUV->RGB loop. Returns the
// reader-owned target (not retained) or NULL; *st gets the OSStatus (-1 when
// something before the transfer failed).
static CVPixelBufferRef klm_reader_to_bgra(klm_reader *r, CVPixelBufferRef pb, OSStatus *st) {
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    *st = -1;
    if (r->xdst && (r->xw != w || r->xh != h)) {
        CVPixelBufferRelease((CVPixelBufferRef)r->xdst);
        r->xdst = NULL;
    }
    if (!r->xdst) {
        CFDictionaryRef iosurf = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        const void *k[1] = { kCVPixelBufferIOSurfacePropertiesKey };
        const void *v[1] = { iosurf };
        CFDictionaryRef attrs = iosurf ? CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks) : NULL;
        CVPixelBufferRef nb = NULL;
        if (CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
                                attrs, &nb) == kCVReturnSuccess) {
            r->xdst = nb; r->xw = w; r->xh = h;
        }
        if (attrs) CFRelease(attrs);
        if (iosurf) CFRelease(iosurf);
    }
    if (!r->xfer) {
        VTPixelTransferSessionRef sess = NULL;
        if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &sess) == noErr) r->xfer = (void *)sess;
    }
    if (!r->xdst || !r->xfer) return NULL;
    *st = VTPixelTransferSessionTransferImage((VTPixelTransferSessionRef)r->xfer, pb,
                                              (CVPixelBufferRef)r->xdst);
    return *st == noErr ? (CVPixelBufferRef)r->xdst : NULL;
}

static char klm_fcc_char(uint32_t v) { return v >= 32 && v < 127 ? (char)v : '.'; }

// Newest published frame as tightly packed RGBA8 (top row first). Returns 1 with
// *rgba valid until the next call (reader-owned), 0 if no new frame since the
// last call, -1 if the frame's pixel format is not one we convert.
int kl_media_surface_take_rgba(void *reader, uint8_t **rgba, int *w, int *h, int64_t *pts_us) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || r->abandoned) return 0;
    pthread_mutex_lock(&r->lock);
    if (!r->count) { pthread_mutex_unlock(&r->lock); return 0; }
    while (r->count > 1) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
        r->n_dropped++;
    }
    CVPixelBufferRef pb = (CVPixelBufferRef)r->q[r->head].pb;
    int64_t pts = r->q[r->head].pts_us;
    r->head = (r->head + 1) % KLM_READER_MAX;
    r->count--;
    r->n_acquired++;
    r->converting = 1;
    pthread_mutex_unlock(&r->lock);

    int fw = (int)CVPixelBufferGetWidth(pb), fh = (int)CVPixelBufferGetHeight(pb);
    size_t need = (size_t)fw * fh * 4;
    int rc = -1;
    int64_t t0_us = klm_now_us(), xfer_us = 0;
    OSType t_in = CVPixelBufferGetPixelFormatType(pb);
    OSStatus xst = noErr;
    const char *via = "direct";
    // Anything that is not BGRA is converted in hardware first (the decoder's
    // native visionOS format '&8v0' is neither NV12 nor BGRA); the BGRA copy
    // below then finishes the job. If the transfer fails an NV12 frame still has
    // the CPU YUV path.
    CVPixelBufferRef use = pb;
    if (t_in != kCVPixelFormatType_32BGRA) {
        CVPixelBufferRef cv = klm_reader_to_bgra(r, pb, &xst);
        xfer_us = klm_now_us() - t0_us;
        if (cv) { use = cv; via = "VTPixelTransfer->BGRA"; }
        else via = klm_pb_is_nv12(pb) ? "CPU NV12 (transfer failed)" : "none (transfer failed)";
    }
    if (r->rgba_cap < need) {
        uint8_t *n = realloc(r->rgba, need);
        if (n) { r->rgba = n; r->rgba_cap = need; }
    }
    if (r->rgba && r->rgba_cap >= need &&
        CVPixelBufferLockBaseAddress(use, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
        OSType t = CVPixelBufferGetPixelFormatType(use);
        uint8_t *dst = r->rgba;
        if (klm_pb_is_nv12(use) && CVPixelBufferGetPlaneCount(use) >= 2) {
            const klm_yuv_k *k = t == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                     ? (fh >= 720 ? &klm_k709f : &klm_k601f)
                                     : (fh >= 720 ? &klm_k709v : &klm_k601v);
            const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(use, 0);
            const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(use, 1);
            size_t ys = CVPixelBufferGetBytesPerRowOfPlane(use, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(use, 1);
            if (y && uv) {
                for (int j = 0; j < fh; j++) {
                    const uint8_t *yr = y + (size_t)j * ys, *ur = uv + (size_t)(j >> 1) * uvs;
                    uint8_t *o = dst + (size_t)j * fw * 4;
                    for (int i = 0; i < fw; i++, o += 4) {
                        int u = ur[(i & ~1)] - 128, v = ur[(i & ~1) + 1] - 128;
                        int yy = (yr[i] - k->yoff) * k->yk + 128;
                        o[0] = klm_clamp8((yy + k->rv * v) >> 8);
                        o[1] = klm_clamp8((yy + k->gu * u + k->gv * v) >> 8);
                        o[2] = klm_clamp8((yy + k->bu * u) >> 8);
                        o[3] = 255;
                    }
                }
                rc = 1;
            }
        } else if (t == kCVPixelFormatType_32BGRA) {
            const uint8_t *src = CVPixelBufferGetBaseAddress(use);
            size_t ss = CVPixelBufferGetBytesPerRow(use);
            if (src) {
                for (int j = 0; j < fh; j++) {
                    const uint8_t *s = src + (size_t)j * ss;
                    uint8_t *o = dst + (size_t)j * fw * 4;
                    for (int i = 0; i < fw; i++, s += 4, o += 4) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; }
                }
                rc = 1;
            }
        }
        CVPixelBufferUnlockBaseAddress(use, kCVPixelBufferLock_ReadOnly);
    }
    CVPixelBufferRelease(pb);
    pthread_mutex_lock(&r->lock);
    r->converting = 0;
    if (r->abandoned) {   // release() ran meanwhile: the buffers are ours to free, the frame is not wanted
        free(r->rgba); r->rgba = NULL; r->rgba_cap = 0;
        klm_reader_free_xfer(r);
        pthread_mutex_unlock(&r->lock);
        return 0;
    }
    pthread_mutex_unlock(&r->lock);
    {   // first 3 calls and every failure (capped), never a line per frame
        static unsigned ncall, nfail;
        unsigned nth = ++ncall;
        int bad = rc != 1;
        if (nth <= 3 || (bad && nfail++ < 16))
            fprintf(stderr, "  [media] take_rgba #%u: '%c%c%c%c' %dx%d via %s (OSStatus %d), "
                            "xfer %.2f ms, total %.2f ms -> %s\n", nth,
                    klm_fcc_char(t_in >> 24), klm_fcc_char((t_in >> 16) & 255),
                    klm_fcc_char((t_in >> 8) & 255), klm_fcc_char(t_in & 255), fw, fh, via,
                    (int)xst, xfer_us / 1000.0, (klm_now_us() - t0_us) / 1000.0,
                    bad ? "FAILED (texture not updated)" : "ok");
    }
    if (rc != 1) return -1;
    *rgba = r->rgba; *w = fw; *h = fh;
    if (pts_us) *pts_us = pts;
    return 1;
}

// ---------------------------------------------------------------------------
// AMediaCodec

#define KLM_IN_BUFFERS  8
#define KLM_IN_CAP      (2u << 20)      // per input buffer; a 4K keyframe fits
#define KLM_OUT_SLOTS   16

typedef struct {
    uint32_t magic;
    char     mime[32];
    kl_vtdec *dec;
    klm_reader *surface;
    int      started;

    pthread_mutex_t lock;
    struct { uint8_t *data; int in_use; } in[KLM_IN_BUFFERS];
    struct { void *pb; int64_t pts_us; int in_use; } out[KLM_OUT_SLOTS];

    // The decoded size, learned from the first frame rather than from
    // configure(): the bitstream's own parameter sets are more authoritative
    // than anything the guest can say, and until a frame exists there is
    // nothing to report. AMediaCodec_getOutputFormat reads these.
    int      out_w, out_h;

    unsigned n_in, n_out, n_rendered;

    // ByteBuffer mode: configured WITHOUT a surface (Unity's VideoPlayer for its
    // cutscenes: AoT). Android then hands the decoded frame out as a linear
    // YUV 4:2:0 buffer the guest reads (getOutputBuffer + the stride/slice-height/
    // color-format of getOutputFormat). See klm_cpu_* below.
    int      bytebuf;
    int      cpu_fmt;           // 21 = YUV420SemiPlanar (NV12), 19 = YUV420Planar (I420)
    int      cpu_full_range;    // the decoded frame's range (color-range key)
    int      fmt_w, fmt_h;      // size the last INFO_OUTPUT_FORMAT_CHANGED announced
    int      cpu_stride, cpu_slice;
    size_t   cpu_bytes;         // bytes of one frame at that layout
    void    *held_pb;           // frame dequeued behind an OUTPUT_FORMAT_CHANGED
    int64_t  held_pts;
    struct { uint8_t *data; size_t cap; size_t size; } cpu[KLM_OUT_SLOTS];
    void    *xfer;              // VTPixelTransferSessionRef (non-NV12 native frames)
    void    *xfer_dst;          // CVPixelBufferRef NV12 target of the transfer
    pthread_mutex_t conv_lock;  // xfer/xfer_dst
    unsigned n_cpu, n_cpu_fail, n_fmt_changed;

    // END_OF_STREAM queued by the guest: once the decoder has nothing left the
    // next dequeue returns an empty buffer flagged EOS (Unity's VideoPlayer ends
    // a clip, or loops it, only on that flag).
    int      eos_in;
    int64_t  eos_quiet_us;      // monotonic time of EOS / of the last frame after it
    int      eos_sent;
    int64_t  last_pts;

    // AMediaCodec_setAsyncNotifyCallback: a thread that does the dequeueing and
    // calls the guest's four callbacks, as the framework's looper does.
    pthread_t athread;
    int      arunning;
    void    *aud;
    void   (*a_in)(void *codec, void *ud, int32_t idx);
    void   (*a_out)(void *codec, void *ud, int32_t idx, AMediaCodecBufferInfo *info);
    void   (*a_fmt)(void *codec, void *ud, void *format);
    void   (*a_err)(void *codec, void *ud, int err, int32_t action, const char *detail);
} klm_codec;

#define KLM_CODEC_MAGIC 0x4b4c4344u    /* 'KLCD' */

static klm_codec *g_codec;

// Video-creation trace window (kl_media_video_trace).
#define KLM_VTRACE_MAX 400
static volatile int g_vtrace_open;
static int          g_vtrace_lines;
int kl_media_video_trace(void) {
    if (!g_vtrace_open) return 0;
    return __atomic_add_fetch(&g_vtrace_lines, 1, __ATOMIC_RELAXED) <= KLM_VTRACE_MAX;
}
static uint64_t klm_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }


static void *klm_AMediaCodec_createDecoderByType(const char *mime) {
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_createDecoderByType(\"%s\") enter\n",
            (unsigned long long)klm_tid(), mime ? mime : "(null)");
    if (mime && strncmp(mime, "video/", 6) == 0) {
        __atomic_store_n(&g_vtrace_lines, 0, __ATOMIC_RELAXED);
        g_vtrace_open = 1;
    }
    klm_codec *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->magic = KLM_CODEC_MAGIC;
    snprintf(c->mime, sizeof c->mime, "%s", mime ? mime : "");
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->conv_lock, NULL);
    c->dec = kl_vtdec_create(mime);
    if (!c->dec) {
        // Not an abort. "No decoder for this type" is a state MediaCodec has,
        // the guest checks for it, and its own message is better than ours.
        fprintf(stderr, "  [media] AMediaCodec_createDecoderByType(\"%s\"): "
                        "no host decoder for that type\n", mime ? mime : "(null)");
        pthread_mutex_destroy(&c->lock);
        pthread_mutex_destroy(&c->conv_lock);
        free(c);
        return NULL;
    }
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_createDecoderByType(\"%s\") exit -> %p\n",
            (unsigned long long)klm_tid(), mime, (void *)c);
    g_codec = c;
    g_ever_created = 1;
    return c;
}

static int klm_AMediaCodec_configure(void *codec, const void *format,
                                     void *window, void *crypto, uint32_t flags) {
    klm_codec *c = codec;
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_configure enter: codec=%p window=%p flags=0x%x\n",
            (unsigned long long)klm_tid(), codec, window, flags);
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (crypto) {
        // A decrypting configure is DRM, and this project does not do that
        //. Steam Link never passes one; refusing
        // rather than ignoring keeps it that way.
        fprintf(stderr, "  [media] AMediaCodec_configure with an AMediaCrypto — "
                        "refused\n");
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    const char *mime = klm_fmt_string((klm_format *)format, "mime");
    // The surface is the AImageReader's window; that is the only kind this
    // guest passes, and the owner pointer the window carries is what connects
    // the two objects back up.
    klm_reader *r = kl_ndk_window_owner(window);
    c->surface = r;
    // No surface at all: ByteBuffer output, as on Android. KL_MEDIA_CPU_OUT=0
    // restores the old behaviour (NULL output buffers, frames dropped).
    c->bytebuf = !window && kl_env_on("KL_MEDIA_CPU_OUT", 1);
    // 21 (NV12) by default: what Qualcomm decoders hand out and what every
    // YUV reader handles; KL_MEDIA_CPU_FMT=19 gives planar I420 instead.
    c->cpu_fmt = kl_env_int("KL_MEDIA_CPU_FMT", 21) == 19 ? 19 : 21;
    fprintf(stderr, "  [media] AMediaCodec_configure: mime=%s surface=%p flags=0x%x%s\n",
            mime ? mime : "(unset)", window, flags,
            c->bytebuf ? (c->cpu_fmt == 19 ? " -> no surface: CPU output buffers, YUV420Planar (19)"
                                           : " -> no surface: CPU output buffers, YUV420SemiPlanar/NV12 (21)")
                       : "");
    if (window && !r)
        fprintf(stderr, "  [media]   ...that window is not an AImageReader's; "
                        "rendered frames will have nowhere to go\n");
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_configure exit: OK\n", (unsigned long long)klm_tid());
    g_vtrace_open = 0;
    return AMEDIA_OK;
}

static int klm_AMediaCodec_start(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    c->started = 1;
    return AMEDIA_OK;
}

static int klm_AMediaCodec_stop(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    c->started = 0;
    return AMEDIA_OK;
}

static void klm_codec_drop_outputs(klm_codec *c) {
    for (int i = 0; i < KLM_OUT_SLOTS; i++)
        if (c->out[i].in_use) {
            if (c->out[i].pb) CVPixelBufferRelease((CVPixelBufferRef)c->out[i].pb);
            c->out[i].pb = NULL;
            c->out[i].in_use = 0;
        }
    if (c->held_pb) { CVPixelBufferRelease((CVPixelBufferRef)c->held_pb); c->held_pb = NULL; }
}

static int klm_AMediaCodec_flush(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) c->in[i].in_use = 0;
    c->eos_in = 0; c->eos_sent = 0; c->eos_quiet_us = 0;
    pthread_mutex_unlock(&c->lock);
    kl_vtdec_flush(c->dec);
    return AMEDIA_OK;
}

static void klm_codec_async_stop(klm_codec *c);

static int klm_AMediaCodec_delete(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    g_vtrace_open = 0;
    klm_codec_async_stop(c);
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) free(c->in[i].data);
    for (int i = 0; i < KLM_OUT_SLOTS; i++) free(c->cpu[i].data);
    pthread_mutex_unlock(&c->lock);
    kl_vtdec_destroy(c->dec);
    pthread_mutex_lock(&c->conv_lock);
    if (c->xfer) {
        VTPixelTransferSessionInvalidate((VTPixelTransferSessionRef)c->xfer);
        CFRelease(c->xfer);
        c->xfer = NULL;
    }
    if (c->xfer_dst) { CVPixelBufferRelease((CVPixelBufferRef)c->xfer_dst); c->xfer_dst = NULL; }
    pthread_mutex_unlock(&c->conv_lock);
    if (c->bytebuf)
        fprintf(stderr, "  [media] codec deleted: %u CPU frames copied out, %u failed, %u format change(s)\n",
                c->n_cpu, c->n_cpu_fail, c->n_fmt_changed);
    pthread_mutex_destroy(&c->conv_lock);
    pthread_mutex_destroy(&c->lock);
    g_life_in += c->n_in; g_life_out += c->n_out; g_life_rendered += c->n_rendered;
    if (g_codec == c) g_codec = NULL;
    c->magic = 0;
    free(c);
    return AMEDIA_OK;
}

// timeoutUs: negative means block indefinitely, 0 means poll. We always have a
// buffer unless the guest has dequeued all eight without queueing any, so the
// wait is a formality — but honouring it costs nothing and a guest that passes
// -1 expecting to block must not get an instant refusal it reads as an error.
static ssize_t klm_AMediaCodec_dequeueInputBuffer(void *codec, int64_t timeoutUs) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    int64_t waited = 0;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < KLM_IN_BUFFERS; i++)
            if (!c->in[i].in_use) {
                c->in[i].in_use = 1;
                pthread_mutex_unlock(&c->lock);
                return i;
            }
        pthread_mutex_unlock(&c->lock);
        if (timeoutUs == 0 || (timeoutUs > 0 && waited >= timeoutUs))
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        usleep(1000);
        waited += 1000;
    }
}

static uint8_t *klm_AMediaCodec_getInputBuffer(void *codec, size_t idx,
                                               size_t *out_size) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_IN_BUFFERS) return NULL;
    pthread_mutex_lock(&c->lock);
    if (!c->in[idx].data) c->in[idx].data = malloc(KLM_IN_CAP);
    uint8_t *p = c->in[idx].data;
    pthread_mutex_unlock(&c->lock);
    if (out_size) *out_size = p ? KLM_IN_CAP : 0;
    return p;
}

// The OUTPUT side's two, and both answers follow from this codec being
// surface-configured rather than from anything missing.
//
// getOutputBuffer is NULL because a codec configured with a surface has no
// client-visible output buffers at all — the frames go to the surface, which
// here is the AImageReader above. That is Android's own answer for the same
// configuration, not a gap: a guest that gets NULL here is being told to read
// the surface, which is exactly what Unity does.
//
// ...unless the codec was configured WITHOUT a surface (c->bytebuf): then the
// output buffer is real, a linear YUV 4:2:0 copy of the decoded frame made at
// dequeue time (klm_cpu_copy), laid out as getOutputFormat describes.
static uint8_t *klm_AMediaCodec_getOutputBuffer(void *codec, size_t idx, size_t *out_size) {
    klm_codec *c = codec;
    if (out_size) *out_size = 0;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    if (c->bytebuf) {
        if (idx >= KLM_OUT_SLOTS) return NULL;
        pthread_mutex_lock(&c->lock);
        // out_size is the buffer's CAPACITY, as on Android; the valid bytes
        // are the BufferInfo's offset/size from dequeueOutputBuffer.
        uint8_t *p = c->out[idx].in_use ? c->cpu[idx].data : NULL;
        size_t n = p ? c->cpu[idx].cap : 0, valid = p ? c->cpu[idx].size : 0;
        pthread_mutex_unlock(&c->lock);
        if (out_size) *out_size = n;
        KLM_FIRST(said_cpubuf, "first getOutputBuffer on a surfaceless codec: slot %zu, %zu bytes "
                               "valid of %zu (CPU copy)\n", idx, valid, n);
        return p;
    }
    KLM_FIRST(said_outbuf, "AMediaCodec_getOutputBuffer on a surface-configured "
                           "codec — NULL, as on Android; the frames are on the "
                           "reader\n");
    return NULL;
}

// getOutputFormat hands back a NEW format the caller owns and deletes. What it
// can honestly carry is what the DECODER measured, not what the guest asked for
// — so the size is the first decoded frame's, and BEFORE that first frame the
// format carries no size at all. That is not a gap: MediaCodec's own output
// format is only meaningful once the codec has reported
// INFO_OUTPUT_FORMAT_CHANGED, and the getters answer "absent" by leaving the
// caller's own default in place rather than by writing a zero over it.
static void *klm_AMediaCodec_getOutputFormat(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    klm_format *f = klm_AMediaFormat_new();
    if (!f) return NULL;
    klm_AMediaFormat_setString(f, g_key_mime, c->mime);
    pthread_mutex_lock(&c->lock);
    int w = c->out_w, h = c->out_h;
    pthread_mutex_unlock(&c->lock);
    if (w && h) {
        klm_AMediaFormat_setInt32(f, g_key_width,  w);
        klm_AMediaFormat_setInt32(f, g_key_height, h);
    }
    // Surface mode: not the stride — the frames never pass through a linear
    // buffer (see getOutputBuffer), so there is no row pitch to report.
    // ByteBuffer mode: the layout of the CPU copy, as a Qualcomm decoder
    // reports it (color-format, stride, slice-height, crop, range).
    if (c->bytebuf) {
        pthread_mutex_lock(&c->lock);
        int fw = c->fmt_w, fh = c->fmt_h, st = c->cpu_stride, sl = c->cpu_slice;
        int full = c->cpu_full_range, cf = c->cpu_fmt;
        pthread_mutex_unlock(&c->lock);
        if (fw && fh) {
            klm_AMediaFormat_setInt32(f, g_key_width,  fw);
            klm_AMediaFormat_setInt32(f, g_key_height, fh);
            klm_AMediaFormat_setInt32(f, g_key_color_format, cf);
            klm_AMediaFormat_setInt32(f, g_key_stride, st);
            klm_AMediaFormat_setInt32(f, g_key_slice_height, sl);
            klm_AMediaFormat_setInt32(f, "crop-left", 0);
            klm_AMediaFormat_setInt32(f, "crop-top", 0);
            klm_AMediaFormat_setInt32(f, "crop-right", fw - 1);
            klm_AMediaFormat_setInt32(f, "crop-bottom", fh - 1);
            klm_AMediaFormat_setInt32(f, g_key_color_range, full ? 1 : 2);  // FULL=1, LIMITED=2
        }
        KLM_FIRST(said_cpufmt, "getOutputFormat (surfaceless): %dx%d color-format %d stride %d "
                               "slice-height %d range %s\n", fw, fh, cf, st, sl,
                  full ? "full" : "limited");
    }
    return f;
}

static int klm_AMediaCodec_queueInputBuffer(void *codec, size_t idx, off_t offset,
                                            size_t size, uint64_t time_us,
                                            uint32_t flags) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_IN_BUFFERS)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    uint8_t *p = c->in[idx].data;
    c->in[idx].in_use = 0;
    c->n_in++;
    pthread_mutex_unlock(&c->lock);
    if (!p) return AMEDIA_ERROR_UNKNOWN;
    KLM_FIRST(said_in, "first access unit queued: %zu bytes, pts %llu us, flags 0x%x\n",
              size, (unsigned long long)time_us, flags);
    if (offset < 0 || size > KLM_IN_CAP || (size_t)offset > KLM_IN_CAP - size)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        // The access unit riding with the flag (if any) is still decoded.
        if (size && kl_vtdec_submit(c->dec, p + offset, size, (int64_t)time_us) != 0)
            fprintf(stderr, "  [media] EOS access unit: submit failed\n");
        pthread_mutex_lock(&c->lock);
        c->eos_in = 1; c->eos_quiet_us = klm_now_us(); c->eos_sent = 0;
        pthread_mutex_unlock(&c->lock);
        KLM_FIRST(said_eos_in, "END_OF_STREAM queued by the guest\n");
        return AMEDIA_OK;
    }
    // A codec-config buffer is parameter sets and nothing else, which is
    // exactly what kl_vtdec absorbs without producing a frame. No special case
    // is needed here; the flag is checked only so an empty one is not an error.
    if (!size) return (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) ? AMEDIA_OK
                                                                    : AMEDIA_ERROR_INVALID_PARAMETER;
    return kl_vtdec_submit(c->dec, p + offset, size, (int64_t)time_us) == 0
             ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

// ---------------------------------------------------------------------------
// ByteBuffer output: the decoded CVPixelBuffer copied into the slot's linear
// buffer. The decoder's native frame is normally biplanar 8-bit 4:2:0 ('420v'/
// '420f'), copied plane by plane; anything else (10-bit, Apple's lossless
// compressed layouts) goes through a VTPixelTransferSession to '420v' first.

static int klm_even(int v) { return (v + 1) & ~1; }

static int klm_pb_is_nv12(CVPixelBufferRef pb) {
    OSType t = CVPixelBufferGetPixelFormatType(pb);
    return t == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           t == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

// Returns a retained NV12 frame with the same picture as pb (pb itself, or the
// transfer target, which is reused frame to frame), NULL on failure.
static CVPixelBufferRef klm_cpu_nv12(klm_codec *c, CVPixelBufferRef pb) {
    if (klm_pb_is_nv12(pb)) return CVPixelBufferRetain(pb);
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    pthread_mutex_lock(&c->conv_lock);
    CVPixelBufferRef dst = c->xfer_dst;
    if (dst && (CVPixelBufferGetWidth(dst) != w || CVPixelBufferGetHeight(dst) != h)) {
        CVPixelBufferRelease(dst);
        dst = c->xfer_dst = NULL;
    }
    if (!dst) {
        CVPixelBufferRef nb = NULL;
        if (CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                NULL, &nb) == kCVReturnSuccess)
            dst = c->xfer_dst = nb;
    }
    if (!c->xfer) {
        VTPixelTransferSessionRef s = NULL;
        if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &s) == noErr) c->xfer = (void *)s;
    }
    OSStatus st = -1;
    if (dst && c->xfer)
        st = VTPixelTransferSessionTransferImage((VTPixelTransferSessionRef)c->xfer, pb, dst);
    CVPixelBufferRef out = (st == noErr && dst) ? CVPixelBufferRetain(dst) : NULL;
    pthread_mutex_unlock(&c->conv_lock);
    OSType t = CVPixelBufferGetPixelFormatType(pb);
    KLM_FIRST(said_xfer, "CPU output: native frame format '%c%c%c%c' is not NV12 -> "
                         "VTPixelTransfer to 420v: %s (OSStatus %d)\n",
              (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t,
              out ? "ok" : "FAILED", (int)st);
    return out;
}

// Copies the frame into c->cpu[slot] with the layout announced in the format
// (stride/slice/color-format). The slot is the caller's (in_use, not yet
// returned to the guest). Returns the bytes written, 0 on failure.
static size_t klm_cpu_copy(klm_codec *c, CVPixelBufferRef src, int slot) {
    pthread_mutex_lock(&c->lock);
    int stride = c->cpu_stride, slice = c->cpu_slice, fw = c->fmt_w, fh = c->fmt_h;
    int planar = c->cpu_fmt == 19;
    size_t bytes = c->cpu_bytes;
    pthread_mutex_unlock(&c->lock);
    if (!bytes || stride <= 0 || slice <= 0) return 0;
    if (c->cpu[slot].cap < bytes) {
        uint8_t *n = realloc(c->cpu[slot].data, bytes);
        if (!n) return 0;
        c->cpu[slot].data = n;
        c->cpu[slot].cap = bytes;
    }
    uint8_t *dst = c->cpu[slot].data;
    CVPixelBufferRef pb = klm_cpu_nv12(c, src);
    if (!pb) return 0;
    size_t done = 0;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
        if (CVPixelBufferIsPlanar(pb) && CVPixelBufferGetPlaneCount(pb) >= 2) {
            const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
            const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
            size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
            size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
            int pw = (int)CVPixelBufferGetWidth(pb), ph = (int)CVPixelBufferGetHeight(pb);
            int cw = pw < fw ? pw : fw, ch = ph < fh ? ph : fh;   // copy what both have
            if (cw > stride) cw = stride;
            if (ch > slice) ch = slice;
            int cch = (ch + 1) / 2, ccw = (cw + 1) / 2;           // chroma rows / samples
            int pch = (int)CVPixelBufferGetHeightOfPlane(pb, 1);
            if (cch > pch) cch = pch;
            if (y && uv) {
                // Pad (frame smaller than announced) as black in limited range.
                if (cw < fw || ch < fh) {
                    memset(dst, 16, (size_t)stride * slice);
                    memset(dst + (size_t)stride * slice, 128, bytes - (size_t)stride * slice);
                }
                for (int r = 0; r < ch; r++)
                    memcpy(dst + (size_t)r * stride, y + (size_t)r * ys, (size_t)cw);
                uint8_t *c0 = dst + (size_t)stride * slice;
                if (!planar) {
                    for (int r = 0; r < cch; r++)
                        memcpy(c0 + (size_t)r * stride, uv + (size_t)r * uvs, (size_t)ccw * 2);
                } else {
                    size_t cst = (size_t)stride / 2;
                    uint8_t *u = c0, *v = c0 + cst * (size_t)(slice / 2);
                    for (int r = 0; r < cch; r++) {
                        const uint8_t *s = uv + (size_t)r * uvs;
                        uint8_t *ur = u + (size_t)r * cst, *vr = v + (size_t)r * cst;
                        for (int k = 0; k < ccw; k++) { ur[k] = s[2 * k]; vr[k] = s[2 * k + 1]; }
                    }
                }
                done = bytes;
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    }
    CVPixelBufferRelease(pb);
    c->cpu[slot].size = done;
    if (done) c->n_cpu++; else c->n_cpu_fail++;
    if (!done) KLM_FIRST(said_cpufail, "CPU output: could not read the decoded frame "
                                       "(not planar / lock failed) — the guest gets an empty buffer\n");
    return done;
}

// A frame whose size differs from the announced format: hold it and tell the
// guest INFO_OUTPUT_FORMAT_CHANGED first, as Android does before the first
// ByteBuffer frame. The next dequeue hands the held frame out.
static int klm_cpu_announce(klm_codec *c, CVPixelBufferRef pb, int64_t pts) {
    int w = klm_even((int)CVPixelBufferGetWidth(pb));
    int h = klm_even((int)CVPixelBufferGetHeight(pb));
    pthread_mutex_lock(&c->lock);
    if (w == c->fmt_w && h == c->fmt_h) { pthread_mutex_unlock(&c->lock); return 0; }
    if (c->held_pb) CVPixelBufferRelease((CVPixelBufferRef)c->held_pb);
    c->held_pb = (void *)pb;           // takes the caller's reference
    c->held_pts = pts;
    c->fmt_w = w; c->fmt_h = h;
    c->out_w = (int)CVPixelBufferGetWidth(pb);
    c->out_h = (int)CVPixelBufferGetHeight(pb);
    c->cpu_stride = w;
    c->cpu_slice = h;
    c->cpu_bytes = (size_t)w * (size_t)h * 3 / 2;
    c->cpu_full_range = CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    c->n_fmt_changed++;
    unsigned n = c->n_fmt_changed;
    pthread_mutex_unlock(&c->lock);
    if (n <= 4)
        fprintf(stderr, "  [media] surfaceless codec: INFO_OUTPUT_FORMAT_CHANGED -> %dx%d, "
                        "color-format %d, stride %d, slice-height %d, %zu bytes/frame\n",
                w, h, c->cpu_fmt, w, h, (size_t)w * (size_t)h * 3 / 2);
    return 1;
}

static ssize_t klm_AMediaCodec_dequeueOutputBuffer(void *codec,
                                                   AMediaCodecBufferInfo *info,
                                                   int64_t timeoutUs) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    int64_t waited = 0;
    for (;;) {
        int64_t pts = 0;
        CVPixelBufferRef pb = NULL;
        if (c->bytebuf) {
            pthread_mutex_lock(&c->lock);
            pb = (CVPixelBufferRef)c->held_pb;
            pts = c->held_pts;
            c->held_pb = NULL;
            pthread_mutex_unlock(&c->lock);
        }
        if (!pb) pb = kl_vtdec_pull(c->dec, &pts);
        if (pb && c->eos_in) { pthread_mutex_lock(&c->lock); c->eos_quiet_us = klm_now_us(); pthread_mutex_unlock(&c->lock); }
        if (pb && c->bytebuf && klm_cpu_announce(c, pb, pts))
            return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;   // pb is held, not lost
        if (pb) {
            pthread_mutex_lock(&c->lock);
            for (int i = 0; i < KLM_OUT_SLOTS; i++)
                if (!c->out[i].in_use) {
                    c->out[i].in_use = 1;
                    // ByteBuffer mode keeps no frame in the slot: it is copied
                    // out below and released, the buffer is the picture.
                    c->out[i].pb = c->bytebuf ? NULL : (void *)pb;
                    c->out[i].pts_us = pts;
                    c->last_pts = pts;
                    c->out_w = (int)CVPixelBufferGetWidth(pb);
                    c->out_h = (int)CVPixelBufferGetHeight(pb);
                    c->n_out++;
                    if (c->bytebuf) c->cpu[i].size = 0;
                    pthread_mutex_unlock(&c->lock);
                    KLM_FIRST(said_out, "first decoded frame dequeued (slot %d, "
                                        "pts %lld us)\n", i, (long long)pts);
                    size_t size = 0;
                    if (c->bytebuf) {
                        size = klm_cpu_copy(c, pb, i);
                        CVPixelBufferRelease(pb);
                    }
                    if (info) {
                        info->offset = 0;
                        // Surface mode: zero, and deliberately — the frame is
                        // on the surface, there is no CPU-visible output buffer
                        // and Android reports 0 too. ByteBuffer mode: the CPU
                        // copy's size.
                        info->size = (int32_t)size;
                        info->presentationTimeUs = pts;
                        info->flags = 0;
                    }
                    return i;
                }
            // Every slot outstanding: the guest is not releasing. Put it back
            // rather than dropping it, and tell the guest to come back.
            pthread_mutex_unlock(&c->lock);
            if (c->bytebuf) {
                pthread_mutex_lock(&c->lock);
                if (!c->held_pb) { c->held_pb = (void *)pb; c->held_pts = pts; pb = NULL; }
                pthread_mutex_unlock(&c->lock);
            }
            if (pb) CVPixelBufferRelease(pb);
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        }
        // EOS: after 150 ms (of dequeue polling) with no new frame, the decoder
        // is drained; hand out one empty buffer carrying the flag.
        pthread_mutex_lock(&c->lock);
        if (c->eos_in && !c->eos_sent && !c->held_pb) {
            if (klm_now_us() - c->eos_quiet_us >= 150000) {
                for (int i = 0; i < KLM_OUT_SLOTS; i++)
                    if (!c->out[i].in_use) {
                        c->out[i].in_use = 1;
                        c->out[i].pb = NULL;
                        c->out[i].pts_us = c->last_pts;
                        c->cpu[i].size = 0;
                        // A real (empty) buffer, as on Android: a guest may
                        // test the pointer before it looks at the flag.
                        if (c->bytebuf && !c->cpu[i].data) {
                            c->cpu[i].data = malloc(64);
                            c->cpu[i].cap = c->cpu[i].data ? 64 : 0;
                        }
                        c->eos_sent = 1;
                        pthread_mutex_unlock(&c->lock);
                        if (info) {
                            info->offset = 0;
                            info->size = 0;
                            info->presentationTimeUs = c->last_pts;
                            info->flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
                        }
                        fprintf(stderr, "  [media] END_OF_STREAM output buffer (slot %d) after %u frames\n",
                                i, c->n_out);
                        return i;
                    }
            }
        }
        pthread_mutex_unlock(&c->lock);
        if (timeoutUs == 0 || (timeoutUs > 0 && waited >= timeoutUs))
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        usleep(1000);
        waited += 1000;
    }
}

static int klm_AMediaCodec_releaseOutputBuffer(void *codec, size_t idx, bool render) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_OUT_SLOTS)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    if (!c->out[idx].in_use) { pthread_mutex_unlock(&c->lock); return AMEDIA_ERROR_INVALID_PARAMETER; }
    CVPixelBufferRef pb = (CVPixelBufferRef)c->out[idx].pb;
    int64_t pts = c->out[idx].pts_us;
    klm_reader *r = c->surface;
    c->out[idx].in_use = 0;
    c->out[idx].pb = NULL;
    if (render) c->n_rendered++;
    pthread_mutex_unlock(&c->lock);

    if (render) KLM_FIRST(said_rel, "first releaseOutputBuffer(render=true)%s\n",
                          r ? "" : c->bytebuf ? " (surfaceless: the guest read the CPU buffer)"
                              : " — but the codec has no output surface, "
                                "so the frame goes nowhere");
    if (render && r && pb) klm_reader_publish(r, pb, pts);
    if (pb) CVPixelBufferRelease(pb);
    return AMEDIA_OK;
}

// ---------------------------------------------------------------------------
// ATrace. Two calls, no state, and they are on the guest's per-frame path — the
// point of serving them is that they are free and their absence is not.
static void klm_ATrace_beginSection(const char *name) { (void)name; }
static void klm_ATrace_endSection(void) { }

// ---------------------------------------------------------------------------

void kl_mediandk_report(FILE *f) {
    unsigned in = g_life_in + (g_codec ? g_codec->n_in : 0);
    unsigned out = g_life_out + (g_codec ? g_codec->n_out : 0);
    unsigned rendered = g_life_rendered + (g_codec ? g_codec->n_rendered : 0);
    unsigned published = g_life_published + (g_reader ? g_reader->n_queued : 0);
    unsigned acquired = g_life_acquired + (g_reader ? g_reader->n_acquired : 0);
    // Never silent once anything media-shaped has happened. "Did a frame reach
    // the guest?" is the question this run exists to answer, and a report that
    // prints nothing is indistinguishable from a report that never ran — which
    // is how a silent reporting path reads.
    // ...and the demuxer's refusals count as "media-shaped", because a guest
    // that asked for a video and was refused is the case most likely to be
    // investigated as a rendering problem. `g_ever_created` is false on that
    // path — no codec is ever built — so it has to be tested separately or the
    // refusal is invisible in exactly the run that needs it.
    if (!g_ever_created && !g_ex_refused) return;
    fprintf(f, "\n=== media (AMediaCodec / AImageReader) ===\n");
    if (g_ex_refused)
        fprintf(f, "  extractor: %u source(s) REFUSED — there is no container "
                   "demuxer here%s%s. The guest's own no-video path ran.\n",
                g_ex_refused, g_ex_last ? ", last: " : "", g_ex_last ? g_ex_last : "");
    if (!g_ever_created) return;
    fprintf(f, "  codec \"%s\": %u buffers queued, %u frames dequeued, %u rendered%s\n",
            g_codec ? g_codec->mime : "video/hevc", in, out, rendered,
            g_codec ? "" : "  (codec deleted; totals are for the whole run)");
    if (g_codec && g_codec->bytebuf)
        fprintf(f, "  surfaceless (ByteBuffer) output: %u CPU frames, %u failed, %dx%d color-format %d\n",
                g_codec->n_cpu, g_codec->n_cpu_fail, g_codec->fmt_w, g_codec->fmt_h, g_codec->cpu_fmt);
    fprintf(f, "  reader: %u published, %u acquired, %u dropped as stale, %d held\n",
            published, acquired, g_reader ? g_reader->n_dropped : 0,
            g_reader ? g_reader->count : 0);
    kl_vtdec_report(f);
}

// ---------------------------------------------------------------------------
// The table, and the two doors into it.

// ---------------------------------------------------------------------------
// The rest of libmediandk as Android 12L/13/14 ships it (NDK r26), served for
// what the Quest 3 can honestly do from here:
//   - decode: yes (AMediaCodec over VideoToolbox, above).
//   - encode, DRM, secure buffers: no. A guest that asks gets the error Android
//     itself returns for "no such codec / not an encoder / no crypto", which is
//     the path every media player already has.
// Nothing below invents data: an extractor with one video track says so, a
// decoder-made AHardwareBuffer describes what the decoder produced.

// ---- AMediaDataSource: URI sources, close, available size --------------------
static void *klm_AMediaDataSource_newUri(const char *uri, int numheaders,
                                         const char *const *key_values) {
    (void)numheaders; (void)key_values;     // headers only matter to a network fetch
    if (!uri) return NULL;
    klm_datasource *d = klm_AMediaDataSource_new();
    if (!d) return NULL;
    d->uri = strdup(uri);
    if (!d->uri) { free(d); return NULL; }
    return d;
}
static void klm_AMediaDataSource_setGetAvailableSize(void *p, klm_ds_get_avail fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->get_avail = fn;               // we spool the whole source, so it is never asked
}
// Closes the stream without deleting the source (reads then fail): the guest's
// close callback runs exactly once, from here or from delete.
static void klm_AMediaDataSource_close(void *p) {
    klm_datasource *d = klm_ds(p);
    if (!d || d->closed) return;
    d->closed = 1;
    if (d->close) d->close(d->userdata);
}

// ---- AMediaExtractor: the remaining getters ---------------------------------
static void *klm_AMediaExtractor_getFileFormat(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return NULL;
    int w = 0, h = 0; const char *mime = NULL; long long dur = 0; float fps = 0;
    kl_avdemux_info(x->dm, &w, &h, &mime, &dur, &fps);
    klm_format *f = klm_AMediaFormat_new();
    if (!f) return NULL;
    klm_AMediaFormat_setString(f, g_key_mime, "video/mp4");   // the container's, as MediaExtractor.getMetrics does
    klm_AMediaFormat_setInt64(f, g_key_duration, dur);
    return f;
}
static void *klm_AMediaExtractor_getSampleFormat(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected || kl_avdemux_sample_time_us(x->dm) < 0) return NULL;
    return klm_AMediaExtractor_getTrackFormat(ex, 0);
}
static ssize_t klm_AMediaExtractor_getSampleSize(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected) return -1;
    unsigned char probe[1];
    return (ssize_t)kl_avdemux_read(x->dm, probe, 0);      // full length even when cap is 0
}
static uint32_t klm_AMediaExtractor_getSampleFlags(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || x->unselected || kl_avdemux_sample_time_us(x->dm) < 0) return 0;
    return kl_avdemux_sample_keyframe(x->dm) ? 1u /* AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC */ : 0u;
}
static int64_t klm_AMediaExtractor_getCachedDuration(void *ex) {
    (void)ex;
    return -1;                   // a local file has no cache to measure; Android answers -1 too
}
static int klm_AMediaExtractor_unselectTrack(void *ex, size_t i) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || i != 0) return AMEDIA_ERROR_INVALID_PARAMETER;
    x->unselected = 1;
    return AMEDIA_OK;
}
// Both are NULL for a clear (non-DRM) stream, which is what every file here is.
static void *klm_AMediaExtractor_getPsshInfo(void *ex) { (void)ex; return NULL; }
static void *klm_AMediaExtractor_getSampleCryptoInfo(void *ex) { (void)ex; return NULL; }

// ---- AMediaCrypto / AMediaDrm: no DRM, by the project's standing rule -------
static bool klm_AMediaCrypto_isCryptoSchemeSupported(const uint8_t uuid[16]) { (void)uuid; return false; }
static bool klm_AMediaCrypto_requiresSecureDecoderComponent(const char *mime) { (void)mime; return false; }
static void *klm_AMediaCrypto_new(const uint8_t uuid[16], const void *init, size_t n) {
    (void)uuid; (void)init; (void)n;
    return NULL;                 // "scheme not supported" — the guest falls back to clear playback or errors
}
static void klm_AMediaCrypto_delete(void *c) { (void)c; }
static bool klm_AMediaDrm_isCryptoSchemeSupported(const uint8_t uuid[16], const char *mime) {
    (void)uuid; (void)mime;
    return false;
}
static void *klm_AMediaDrm_createByUUID(const uint8_t uuid[16]) { (void)uuid; return NULL; }
static int klm_AMediaDrm_release(void *drm) { (void)drm; return AMEDIA_OK; }

// ---- AMediaCodecCryptoInfo: a plain container --------------------------------
typedef struct {
    uint32_t magic;
    int      mode;
    size_t   n;
    uint8_t  key[16], iv[16];
    size_t  *clear, *enc;
    int32_t  pat_enc, pat_skip;
} klm_cryptoinfo;
#define KLM_CRYPTOINFO_MAGIC 0x4b4c4349u   /* 'KLCI' */
static klm_cryptoinfo *klm_ci(void *p) {
    klm_cryptoinfo *c = p;
    return (c && c->magic == KLM_CRYPTOINFO_MAGIC) ? c : NULL;
}
static void *klm_AMediaCodecCryptoInfo_new(int n, uint8_t key[16], uint8_t iv[16], int mode,
                                           size_t *clear, size_t *enc) {
    if (n < 0 || !key || !iv || (n && (!clear || !enc))) return NULL;
    klm_cryptoinfo *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->magic = KLM_CRYPTOINFO_MAGIC; c->mode = mode; c->n = (size_t)n;
    memcpy(c->key, key, 16); memcpy(c->iv, iv, 16);
    c->clear = calloc(n ? (size_t)n : 1, sizeof(size_t));
    c->enc   = calloc(n ? (size_t)n : 1, sizeof(size_t));
    if (!c->clear || !c->enc) { free(c->clear); free(c->enc); free(c); return NULL; }
    for (int i = 0; i < n; i++) { c->clear[i] = clear[i]; c->enc[i] = enc[i]; }
    return c;
}
static int klm_AMediaCodecCryptoInfo_delete(void *p) {
    klm_cryptoinfo *c = klm_ci(p);
    if (!c) return AMEDIA_ERROR_INVALID_OBJECT;
    c->magic = 0; free(c->clear); free(c->enc); free(c);
    return AMEDIA_OK;
}
static void klm_AMediaCodecCryptoInfo_setPattern(void *p, const int32_t *pat) {
    klm_cryptoinfo *c = klm_ci(p);
    if (c && pat) { c->pat_enc = pat[0]; c->pat_skip = pat[1]; }
}
static size_t klm_AMediaCodecCryptoInfo_getNumSubSamples(void *p) {
    klm_cryptoinfo *c = klm_ci(p);
    return c ? c->n : 0;         // Android returns 0 for an invalid info too
}
static int klm_AMediaCodecCryptoInfo_getKey(void *p, uint8_t *dst) {
    klm_cryptoinfo *c = klm_ci(p);
    if (!c || !dst) return AMEDIA_ERROR_INVALID_PARAMETER;
    memcpy(dst, c->key, 16);
    return AMEDIA_OK;
}
static int klm_AMediaCodecCryptoInfo_getIV(void *p, uint8_t *dst) {
    klm_cryptoinfo *c = klm_ci(p);
    if (!c || !dst) return AMEDIA_ERROR_INVALID_PARAMETER;
    memcpy(dst, c->iv, 16);
    return AMEDIA_OK;
}
static int klm_AMediaCodecCryptoInfo_getMode(void *p) {
    klm_cryptoinfo *c = klm_ci(p);
    return c ? c->mode : AMEDIA_ERROR_INVALID_OBJECT;
}
static int klm_AMediaCodecCryptoInfo_getClearBytes(void *p, size_t *dst) {
    klm_cryptoinfo *c = klm_ci(p);
    if (!c || !dst) return AMEDIA_ERROR_INVALID_PARAMETER;
    memcpy(dst, c->clear, c->n * sizeof(size_t));
    return AMEDIA_OK;
}
static int klm_AMediaCodecCryptoInfo_getEncryptedBytes(void *p, size_t *dst) {
    klm_cryptoinfo *c = klm_ci(p);
    if (!c || !dst) return AMEDIA_ERROR_INVALID_PARAMETER;
    memcpy(dst, c->enc, c->n * sizeof(size_t));
    return AMEDIA_OK;
}

// ---- AMediaCodec: the rest of the surface ------------------------------------
// Names a Qualcomm/AOSP decoder carries ("OMX.qcom.video.decoder.avc",
// "c2.qti.hevc.decoder", "c2.android.avc.decoder"); createCodecByName serves the
// decoders and says NULL ("no such codec") for everything else.
static const char *klm_mime_for_codec_name(const char *name) {
    if (!name || !strstr(name, "ecoder")) return NULL;     // decoder / Decoder
    if (strstr(name, "hevc") || strstr(name, "h265") || strstr(name, "HEVC")) return "video/hevc";
    if (strstr(name, "avc") || strstr(name, "h264") || strstr(name, "AVC")) return "video/avc";
    return NULL;
}
static void *klm_AMediaCodec_createCodecByName(const char *name) {
    const char *mime = klm_mime_for_codec_name(name);
    if (!mime) {
        fprintf(stderr, "  [media] AMediaCodec_createCodecByName(\"%s\"): not a decoder served here\n",
                name ? name : "(null)");
        return NULL;
    }
    return klm_AMediaCodec_createDecoderByType(mime);
}
static void *klm_AMediaCodec_createEncoderByType(const char *mime) {
    fprintf(stderr, "  [media] AMediaCodec_createEncoderByType(\"%s\"): there is no encoder here\n",
            mime ? mime : "(null)");
    return NULL;
}
static int klm_AMediaCodec_setOutputSurface(void *codec, void *window) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || !window) return AMEDIA_ERROR_INVALID_PARAMETER;
    klm_reader *r = kl_ndk_window_owner(window);
    pthread_mutex_lock(&c->lock);
    c->surface = r;                          // later frames go to the new reader
    pthread_mutex_unlock(&c->lock);
    if (!r) fprintf(stderr, "  [media] AMediaCodec_setOutputSurface: that window is not an AImageReader's\n");
    return AMEDIA_OK;
}
// Decoders have no input surface; MediaCodec answers INVALID_OPERATION.
static int klm_AMediaCodec_createInputSurface(void *codec, void **out) {
    (void)codec; if (out) *out = NULL;
    return AMEDIA_ERROR_INVALID_OPERATION;
}
static int klm_AMediaCodec_createPersistentInputSurface(void **out) {
    if (out) *out = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;         // needs an encoder
}
static int klm_AMediaCodec_setInputSurface(void *codec, void *window) {
    (void)codec; (void)window;
    return AMEDIA_ERROR_INVALID_OPERATION;
}
static int klm_AMediaCodec_signalEndOfInputStream(void *codec) {
    (void)codec;
    return AMEDIA_ERROR_INVALID_OPERATION;
}
static int klm_AMediaCodec_setParameters(void *codec, const void *params) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || !params) return AMEDIA_ERROR_INVALID_PARAMETER;
    return AMEDIA_OK;                        // nothing a hardware decoder changes mid-stream is exposed
}
static int klm_AMediaCodec_getName(void *codec, char **out) {
    klm_codec *c = codec;
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_getName enter: codec=%p\n", (unsigned long long)klm_tid(), codec);
    if (!c || c->magic != KLM_CODEC_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = strdup(strstr(c->mime, "hevc") ? "c2.qti.hevc.decoder" : "c2.qti.avc.decoder");
    fprintf(stderr, "  [media] tid=%llu AMediaCodec_getName exit: \"%s\"\n", (unsigned long long)klm_tid(), *out ? *out : "(null)");
    return *out ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}
static void klm_AMediaCodec_releaseName(void *codec, char *name) { (void)codec; free(name); }
static void *klm_AMediaCodec_getInputFormat(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    klm_format *f = klm_AMediaFormat_new();
    if (f) klm_AMediaFormat_setString(f, g_key_mime, c->mime);
    return f;
}
static void *klm_AMediaCodec_getBufferFormat(void *codec, size_t idx) {
    (void)idx;
    return klm_AMediaCodec_getOutputFormat(codec);
}
static int klm_AMediaCodec_releaseOutputBufferAtTime(void *codec, size_t idx, int64_t ts_ns) {
    (void)ts_ns;                             // the reader presents on arrival; there is no display timeline to hold it to
    return klm_AMediaCodec_releaseOutputBuffer(codec, idx, true);
}
// Secure input needs a crypto the codec was never configured with.
static int klm_AMediaCodec_queueSecureInputBuffer(void *codec, size_t idx, off_t off,
                                                  void *info, uint64_t time_us, uint32_t flags) {
    (void)codec; (void)idx; (void)off; (void)info; (void)time_us; (void)flags;
    return AMEDIA_ERROR_INVALID_OPERATION;
}

typedef struct {
    void (*onAsyncInputAvailable)(void *codec, void *ud, int32_t idx);
    void (*onAsyncOutputAvailable)(void *codec, void *ud, int32_t idx, AMediaCodecBufferInfo *info);
    void (*onAsyncFormatChanged)(void *codec, void *ud, void *format);
    void (*onAsyncError)(void *codec, void *ud, int err, int32_t action, const char *detail);
} klm_async_cb;

static void *klm_codec_async_loop(void *arg) {
    klm_codec *c = arg;
    kl_thread_init();                        // guest callbacks run on this thread
    while (__atomic_load_n(&c->arunning, __ATOMIC_ACQUIRE)) {
        int progress = 0;
        if (c->started) {
            ssize_t i = klm_AMediaCodec_dequeueInputBuffer(c, 0);
            if (i >= 0) {
                if (c->a_in) c->a_in(c, c->aud, (int32_t)i);
                progress = 1;
            }
            AMediaCodecBufferInfo info;
            memset(&info, 0, sizeof info);
            ssize_t o = klm_AMediaCodec_dequeueOutputBuffer(c, &info, 0);
            if (o >= 0) {
                if (c->a_out) c->a_out(c, c->aud, (int32_t)o, &info);
                progress = 1;
            } else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                void *f = klm_AMediaCodec_getOutputFormat(c);
                if (f && c->a_fmt) c->a_fmt(c, c->aud, f);
                if (f) klm_AMediaFormat_delete(f);      // owned by the codec for the callback's duration
                progress = 1;
            }
        }
        if (!progress) usleep(1000);
    }
    return NULL;
}
static void klm_codec_async_stop(klm_codec *c) {
    if (!c->arunning) return;
    __atomic_store_n(&c->arunning, 0, __ATOMIC_RELEASE);
    pthread_join(c->athread, NULL);
}
static int klm_AMediaCodec_setAsyncNotifyCallback(void *codec, klm_async_cb cb, void *ud) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    klm_codec_async_stop(c);
    c->a_in = cb.onAsyncInputAvailable; c->a_out = cb.onAsyncOutputAvailable;
    c->a_fmt = cb.onAsyncFormatChanged; c->a_err = cb.onAsyncError; c->aud = ud;
    if (!c->a_in && !c->a_out && !c->a_fmt) return AMEDIA_OK;   // callbacks cleared: back to synchronous mode
    c->arunning = 1;
    if (pthread_create(&c->athread, NULL, klm_codec_async_loop, c) != 0) {
        c->arunning = 0;
        return AMEDIA_ERROR_UNKNOWN;
    }
    return AMEDIA_OK;
}

// ---- AImageReader / AImage: the rest -----------------------------------------
#define KLM_AIMAGE_FORMAT_PRIVATE     0x22
#define KLM_AIMAGE_FORMAT_YUV_420_888 0x23
static int klm_AImageReader_new(int32_t w, int32_t h, int32_t format, int32_t max_images, void **out) {
    // No usage given: the default is CPU_READ_OFTEN, which is what makes the
    // planes readable.
    return klm_AImageReader_newWithUsage(w, h, format, 3 /* AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN */,
                                         max_images, out);
}
static int klm_reader_get(void *reader, int32_t *out, int which) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = which == 0 ? r->width : which == 1 ? r->height : which == 2 ? r->format : r->max_images;
    return AMEDIA_OK;
}
static int klm_AImageReader_getWidth(void *r, int32_t *o)     { return klm_reader_get(r, o, 0); }
static int klm_AImageReader_getHeight(void *r, int32_t *o)    { return klm_reader_get(r, o, 1); }
static int klm_AImageReader_getFormat(void *r, int32_t *o)    { return klm_reader_get(r, o, 2); }
static int klm_AImageReader_getMaxImages(void *r, int32_t *o) { return klm_reader_get(r, o, 3); }
// FIFO: the oldest queued frame, no dropping (acquireLatestImage is the one that drops).
static int klm_AImageReader_acquireNextImage(void *reader, void **out) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&r->lock);
    if (!r->count) { pthread_mutex_unlock(&r->lock); return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE; }
    void *pb = r->q[r->head].pb;
    int64_t pts = r->q[r->head].pts_us;
    r->head = (r->head + 1) % KLM_READER_MAX;
    r->count--;
    r->n_acquired++;
    pthread_mutex_unlock(&r->lock);
    klm_image *im = klm_image_new((CVPixelBufferRef)pb, pts);
    CVPixelBufferRelease((CVPixelBufferRef)pb);
    if (!im) return AMEDIA_ERROR_UNKNOWN;
    im->format = r->format;
    *out = im;
    return AMEDIA_OK;
}
// The "Async" forms exist for a fence the producer signals when the frame is
// really ready. Ours is ready when queued, so the fence is -1 (already signalled).
static int klm_AImageReader_acquireNextImageAsync(void *r, void **out, int *fence) {
    if (!fence) return AMEDIA_ERROR_INVALID_PARAMETER;
    *fence = -1;
    return klm_AImageReader_acquireNextImage(r, out);
}
static int klm_AImageReader_acquireLatestImageAsync(void *r, void **out, int *fence) {
    if (!fence) return AMEDIA_ERROR_INVALID_PARAMETER;
    *fence = -1;
    return klm_AImageReader_acquireLatestImage(r, out);
}
// Fires when the producer detaches a buffer from the queue; this reader never
// reallocates buffers behind the guest's back, so it is accepted and never called.
static int klm_AImageReader_setBufferRemovedListener(void *reader, const void *listener) {
    klm_reader *r = reader;
    (void)listener;
    if (!r || r->magic != KLM_READER_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    return AMEDIA_OK;
}
static klm_image *klm_img(void *p) {
    klm_image *im = p;
    return (im && im->magic == KLM_IMAGE_MAGIC) ? im : NULL;
}
static int klm_AImage_getFormat(void *image, int32_t *out) {
    klm_image *im = klm_img(image);
    if (!im || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = im->format ? im->format : KLM_AIMAGE_FORMAT_PRIVATE;
    return AMEDIA_OK;
}
typedef struct { int32_t left, top, right, bottom; } klm_rect;
static int klm_AImage_getCropRect(void *image, klm_rect *r) {
    klm_image *im = klm_img(image);
    if (!im || !r) return AMEDIA_ERROR_INVALID_PARAMETER;
    r->left = 0; r->top = 0;
    r->right  = (int32_t)CVPixelBufferGetWidth((CVPixelBufferRef)im->pixels);
    r->bottom = (int32_t)CVPixelBufferGetHeight((CVPixelBufferRef)im->pixels);
    return AMEDIA_OK;
}
static void klm_AImage_deleteAsync(void *image, int fence) {
    if (fence >= 0) close(fence);            // ownership passes to the callee
    klm_AImage_delete(image);
}
// Planes: only a YUV_420_888 reader over an NV12 frame has CPU-readable planes
// (Y, then the interleaved CbCr read as pixelStride-2 U and V). A PRIVATE
// reader's pixels belong to the GPU, and Android refuses plane access on those.
static int klm_image_lock_planes(klm_image *im) {
    if (im->format != KLM_AIMAGE_FORMAT_YUV_420_888 ||
        !klm_pb_is_nv12((CVPixelBufferRef)im->pixels))
        return AMEDIA_ERROR_UNSUPPORTED;
    if (!im->locked) {
        if (CVPixelBufferLockBaseAddress((CVPixelBufferRef)im->pixels, kCVPixelBufferLock_ReadOnly) != 0)
            return AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
        im->locked = 1;
    }
    return AMEDIA_OK;
}
static int klm_AImage_getNumberOfPlanes(void *image, int32_t *out) {
    klm_image *im = klm_img(image);
    if (!im || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (im->format != KLM_AIMAGE_FORMAT_YUV_420_888 ||
        !klm_pb_is_nv12((CVPixelBufferRef)im->pixels)) return AMEDIA_ERROR_UNSUPPORTED;
    *out = 3;
    return AMEDIA_OK;
}
static int klm_AImage_getPlaneRowStride(void *image, int plane, int32_t *out) {
    klm_image *im = klm_img(image);
    if (!im || !out || plane < 0 || plane > 2) return AMEDIA_ERROR_INVALID_PARAMETER;
    int st = klm_image_lock_planes(im);
    if (st != AMEDIA_OK) return st;
    *out = (int32_t)CVPixelBufferGetBytesPerRowOfPlane((CVPixelBufferRef)im->pixels, plane ? 1 : 0);
    return AMEDIA_OK;
}
static int klm_AImage_getPlanePixelStride(void *image, int plane, int32_t *out) {
    klm_image *im = klm_img(image);
    if (!im || !out || plane < 0 || plane > 2) return AMEDIA_ERROR_INVALID_PARAMETER;
    int st = klm_image_lock_planes(im);
    if (st != AMEDIA_OK) return st;
    *out = plane ? 2 : 1;
    return AMEDIA_OK;
}
static int klm_AImage_getPlaneData(void *image, int plane, uint8_t **data, int *len) {
    klm_image *im = klm_img(image);
    if (!im || !data || !len || plane < 0 || plane > 2) return AMEDIA_ERROR_INVALID_PARAMETER;
    int st = klm_image_lock_planes(im);
    if (st != AMEDIA_OK) return st;
    CVPixelBufferRef pb = (CVPixelBufferRef)im->pixels;
    size_t h = CVPixelBufferGetHeightOfPlane(pb, plane ? 1 : 0);
    size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane ? 1 : 0);
    uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pb, plane ? 1 : 0);
    if (!base) return AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
    *data = plane == 2 ? base + 1 : base;      // Cr interleaves right after Cb
    *len = (int)(stride * h) - (plane == 2 ? 1 : 0);
    return AMEDIA_OK;
}

// ---- AHardwareBuffer: allocate / describe / lock / unlock --------------------
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} klm_ahb_desc;
typedef struct { int32_t left, top, right, bottom; } klm_arect;
typedef struct { void *data; uint32_t pixelStride, rowStride; } klm_ahb_plane;
typedef struct { uint32_t planeCount; klm_ahb_plane planes[4]; } klm_ahb_planes;

enum { KLM_AHB_CPU_READ_MASK = 0xF, KLM_AHB_CPU_WRITE_MASK = 0xF0,
       KLM_AHB_USAGE_PROTECTED = 0x4000 };
enum { KLM_AHB_BLOB = 0x21, KLM_AHB_YUV420 = 0x23 };

// Bytes per pixel of a gralloc format, 0 for one we do not know (and for the
// planar/blob ones, which have their own size rules).
static int klm_ahb_bpp(uint32_t fmt) {
    switch (fmt) {
    case 1: case 2: case 0x2b: case 0x32: case 0x33: case 0x3a: return 4;
    case 3: case 0x31: return 3;
    case 4: case 0x30: case 0x39: return 2;
    case 0x16: case 0x34: return 8;
    case 0x35: case 0x38: return 1;
    default: return 0;
    }
}
static int klm_ahb_check(const klm_ahb_desc *d) {
    if (!d || !d->width || !d->height || !d->layers) return -22;           // -EINVAL
    if (d->usage & KLM_AHB_USAGE_PROTECTED) return -22;                    // no protected memory: DRM line
    if (d->format == KLM_AHB_BLOB) return (d->height == 1 && d->layers == 1) ? 0 : -22;
    if (d->format == KLM_AHB_YUV420) return 0;
    return klm_ahb_bpp(d->format) ? 0 : -22;
}
static int klm_AHardwareBuffer_isSupported(const klm_ahb_desc *d) { return klm_ahb_check(d) == 0; }
static klm_hwbuf *klm_hw(const void *p) {
    const klm_hwbuf *h = p;
    return (h && h->magic == KLM_HWBUF_MAGIC) ? (klm_hwbuf *)h : NULL;
}
static int klm_AHardwareBuffer_allocate(const klm_ahb_desc *d, void **out) {
    if (!out) return -22;
    *out = NULL;
    int bad = klm_ahb_check(d);
    if (bad) return bad;
    uint32_t stride = d->format == KLM_AHB_BLOB ? d->width : (d->width + 63u) & ~63u;   // gralloc pads rows
    size_t bytes;
    if (d->format == KLM_AHB_BLOB) bytes = d->width;
    else if (d->format == KLM_AHB_YUV420) bytes = (size_t)stride * d->height * 3 / 2;
    else bytes = (size_t)stride * d->height * (size_t)klm_ahb_bpp(d->format) * d->layers;
    if (bytes > (512u << 20)) return -12;                                   // -ENOMEM
    klm_hwbuf *h = calloc(1, sizeof *h);
    if (!h) return -12;
    h->mem = calloc(1, bytes ? bytes : 1);
    if (!h->mem) { free(h); return -12; }
    h->magic = KLM_HWBUF_MAGIC; h->refs = 1;
    h->width = d->width; h->height = d->height; h->layers = d->layers;
    h->format = d->format; h->usage = d->usage; h->stride_px = stride;
    static uint64_t next_id = 1ull << 32;      // apart from decoder frames' ids
    h->id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    *out = h;
    return 0;
}
static void klm_AHardwareBuffer_describe(const void *buf, klm_ahb_desc *out) {
    klm_hwbuf *h = klm_hw(buf);
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!h) return;
    if (h->mem) {
        out->width = h->width; out->height = h->height; out->layers = h->layers;
        out->format = h->format; out->usage = h->usage; out->stride = h->stride_px;
        return;
    }
    CVPixelBufferRef pb = (CVPixelBufferRef)h->pixels;
    out->width = (uint32_t)CVPixelBufferGetWidth(pb);
    out->height = (uint32_t)CVPixelBufferGetHeight(pb);
    out->layers = 1;
    out->format = klm_pb_is_nv12(pb) ? KLM_AHB_YUV420 : 1;
    out->usage = 0x100 /* GPU_SAMPLED_IMAGE */ | 3 /* CPU_READ_OFTEN */;
    size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    out->stride = (uint32_t)(klm_pb_is_nv12(pb) ? bpr : bpr / 4);
}
static int klm_ahb_rect_ok(const klm_hwbuf *h, const klm_arect *r) {
    if (!r) return 1;
    uint32_t w, hh;
    if (h->mem) { w = h->width; hh = h->height; }
    else { w = (uint32_t)CVPixelBufferGetWidth((CVPixelBufferRef)h->pixels);
           hh = (uint32_t)CVPixelBufferGetHeight((CVPixelBufferRef)h->pixels); }
    return r->left >= 0 && r->top >= 0 && r->right >= r->left && r->bottom >= r->top &&
           (uint32_t)r->right <= w && (uint32_t)r->bottom <= hh;
}
// Common front of lock/lockPlanes/lockAndGetInfo. Returns 0 or -errno as
// gralloc does: usage must name CPU access the buffer was allocated for, and the
// rect must lie inside it. The fence is ours to close.
static int klm_ahb_lock_common(klm_hwbuf *h, uint64_t usage, int fence, const klm_arect *rect) {
    if (fence >= 0) close(fence);            // producers here are synchronous: always signalled
    if (!h) return -22;
    uint64_t cpu = usage & (KLM_AHB_CPU_READ_MASK | KLM_AHB_CPU_WRITE_MASK);
    if (!cpu || (usage & ~(uint64_t)(KLM_AHB_CPU_READ_MASK | KLM_AHB_CPU_WRITE_MASK))) return -22;
    if (h->mem) {
        if ((cpu & KLM_AHB_CPU_READ_MASK) && !(h->usage & KLM_AHB_CPU_READ_MASK)) return -22;
        if ((cpu & KLM_AHB_CPU_WRITE_MASK) && !(h->usage & KLM_AHB_CPU_WRITE_MASK)) return -22;
    } else if (cpu & KLM_AHB_CPU_WRITE_MASK) {
        return -22;                          // a decoder's frame is read-only to the CPU
    }
    if (!klm_ahb_rect_ok(h, rect)) return -22;
    return 0;
}
static int klm_ahb_lock_pixels(klm_hwbuf *h) {
    if (h->mem) { h->locked++; return 0; }
    if (CVPixelBufferLockBaseAddress((CVPixelBufferRef)h->pixels, kCVPixelBufferLock_ReadOnly) != 0)
        return -5;                           // -EIO
    h->locked++;
    return 0;
}
static int klm_AHardwareBuffer_lock(void *buf, uint64_t usage, int32_t fence,
                                    const klm_arect *rect, void **out) {
    klm_hwbuf *h = klm_hw(buf);
    int rc = klm_ahb_lock_common(h, usage, fence, rect);
    if (rc) return rc;
    if (!out) return -22;
    // Planar YUV has no single address: Android refuses lock() on it, and the
    // guest must come through lockPlanes.
    if (h->mem ? h->format == KLM_AHB_YUV420 : klm_pb_is_nv12((CVPixelBufferRef)h->pixels)) return -22;
    if ((rc = klm_ahb_lock_pixels(h))) return rc;
    *out = h->mem ? h->mem : CVPixelBufferGetBaseAddress((CVPixelBufferRef)h->pixels);
    return *out ? 0 : -5;
}
static int klm_AHardwareBuffer_lockPlanes(void *buf, uint64_t usage, int32_t fence,
                                          const klm_arect *rect, klm_ahb_planes *out) {
    klm_hwbuf *h = klm_hw(buf);
    int rc = klm_ahb_lock_common(h, usage, fence, rect);
    if (rc) return rc;
    if (!out) return -22;
    if ((rc = klm_ahb_lock_pixels(h))) return rc;
    memset(out, 0, sizeof *out);
    if (h->mem) {
        uint8_t *m = h->mem;
        if (h->format == KLM_AHB_YUV420) {
            size_t ysz = (size_t)h->stride_px * h->height;
            out->planeCount = 3;
            out->planes[0] = (klm_ahb_plane){m, 1, h->stride_px};
            out->planes[1] = (klm_ahb_plane){m + ysz, 2, h->stride_px};
            out->planes[2] = (klm_ahb_plane){m + ysz + 1, 2, h->stride_px};
        } else {
            int bpp = klm_ahb_bpp(h->format);
            out->planeCount = 1;
            out->planes[0] = (klm_ahb_plane){m, (uint32_t)(bpp ? bpp : 1),
                                             h->format == KLM_AHB_BLOB ? h->width : h->stride_px * (uint32_t)bpp};
        }
        return 0;
    }
    CVPixelBufferRef pb = (CVPixelBufferRef)h->pixels;
    if (klm_pb_is_nv12(pb)) {
        out->planeCount = 3;
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        out->planes[0] = (klm_ahb_plane){y, 1, (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 0)};
        out->planes[1] = (klm_ahb_plane){c, 2, (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 1)};
        out->planes[2] = (klm_ahb_plane){c ? c + 1 : NULL, 2, (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 1)};
    } else {
        out->planeCount = 1;
        out->planes[0] = (klm_ahb_plane){CVPixelBufferGetBaseAddress(pb), 4,
                                         (uint32_t)CVPixelBufferGetBytesPerRow(pb)};
    }
    return 0;
}
static int klm_AHardwareBuffer_lockAndGetInfo(void *buf, uint64_t usage, int32_t fence,
                                              const klm_arect *rect, void **out,
                                              int32_t *bpp, int32_t *stride_bytes) {
    klm_hwbuf *h = klm_hw(buf);
    int rc = klm_AHardwareBuffer_lock(buf, usage, fence, rect, out);
    if (rc) return rc;
    int b = h->mem ? klm_ahb_bpp(h->format) : 4;
    if (bpp) *bpp = b ? b : -1;                         // gralloc says -1 where it has no single answer
    if (stride_bytes)
        *stride_bytes = h->mem ? (b ? (int32_t)(h->stride_px * (uint32_t)b) : -1)
                               : (int32_t)CVPixelBufferGetBytesPerRow((CVPixelBufferRef)h->pixels);
    return 0;
}
static int klm_AHardwareBuffer_unlock(void *buf, int32_t *fence) {
    klm_hwbuf *h = klm_hw(buf);
    if (!h) return -22;
    if (h->locked <= 0) return -22;                     // unlock without lock
    h->locked--;
    if (!h->mem) CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)h->pixels, kCVPixelBufferLock_ReadOnly);
    if (fence) *fence = -1;
    return 0;
}
static int klm_AHardwareBuffer_getId(const void *buf, uint64_t *id) {
    klm_hwbuf *h = klm_hw(buf);
    if (!h || !id) return -22;
    *id = h->id;
    return 0;
}

// ---- ATrace -------------------------------------------------------------------
// Tracing is never enabled for a shipped app, so isEnabled is false and the
// rest cost nothing.
static bool klm_ATrace_isEnabled(void) { return false; }
static void klm_ATrace_beginAsyncSection(const char *name, int32_t cookie) { (void)name; (void)cookie; }
static void klm_ATrace_endAsyncSection(const char *name, int32_t cookie) { (void)name; (void)cookie; }
static void klm_ATrace_setCounter(const char *name, int64_t value) { (void)name; (void)value; }

#define M(sym, fn) {sym, (void *)(fn)}
static const struct { const char *name; void *fn; } g_media[] = {
    M("AMediaFormat_new",       klm_AMediaFormat_new),
    M("AMediaFormat_delete",    klm_AMediaFormat_delete),
    M("AMediaFormat_setInt32",  klm_AMediaFormat_setInt32),
    M("AMediaFormat_setString", klm_AMediaFormat_setString),
    M("AMediaFormat_getInt32",  klm_AMediaFormat_getInt32),
    M("AMediaFormat_getInt64",  klm_AMediaFormat_getInt64),
    M("AMediaFormat_getFloat",  klm_AMediaFormat_getFloat),
    M("AMediaFormat_getString", klm_AMediaFormat_getString),
    M("AMediaFormat_getRect",   klm_AMediaFormat_getRect),
    M("AMediaFormat_setInt64",  klm_AMediaFormat_setInt64),
    M("AMediaFormat_setFloat",  klm_AMediaFormat_setFloat),
    M("AMediaFormat_setDouble", klm_AMediaFormat_setDouble),
    M("AMediaFormat_setSize",   klm_AMediaFormat_setSize),
    M("AMediaFormat_setBuffer", klm_AMediaFormat_setBuffer),
    M("AMediaFormat_setRect",   klm_AMediaFormat_setRect),
    M("AMediaFormat_getDouble", klm_AMediaFormat_getDouble),
    M("AMediaFormat_getSize",   klm_AMediaFormat_getSize),
    M("AMediaFormat_getBuffer", klm_AMediaFormat_getBuffer),
    M("AMediaFormat_copy",      klm_AMediaFormat_copy),
    M("AMediaFormat_toString",  klm_AMediaFormat_toString),

    // Data symbols, not functions — see the note by the definitions.
    M("AMEDIAFORMAT_KEY_MIME",           &g_key_mime),
    M("AMEDIAFORMAT_KEY_WIDTH",          &g_key_width),
    M("AMEDIAFORMAT_KEY_HEIGHT",         &g_key_height),
    M("AMEDIAFORMAT_KEY_MAX_WIDTH",      &g_key_max_width),
    M("AMEDIAFORMAT_KEY_MAX_HEIGHT",     &g_key_max_height),
    M("AMEDIAFORMAT_KEY_COLOR_TRANSFER", &g_key_color_transfer),
    M("AMEDIAFORMAT_KEY_COLOR_STANDARD", &g_key_color_standard),
    M("AMEDIAFORMAT_KEY_COLOR_RANGE",    &g_key_color_range),
    M("AMEDIAFORMAT_KEY_COLOR_FORMAT",   &g_key_color_format),
    M("AMEDIAFORMAT_KEY_PRIORITY",       &g_key_priority),
    M("AMEDIAFORMAT_KEY_OPERATING_RATE", &g_key_operating_rate),
    M("AMEDIAFORMAT_KEY_DURATION",       &g_key_duration),
    M("AMEDIAFORMAT_KEY_FRAME_RATE",     &g_key_frame_rate),
    M("AMEDIAFORMAT_KEY_LANGUAGE",       &g_key_language),
    M("AMEDIAFORMAT_KEY_CHANNEL_COUNT",  &g_key_channel_count),
    M("AMEDIAFORMAT_KEY_SAMPLE_RATE",    &g_key_sample_rate),
    M("AMEDIAFORMAT_KEY_STRIDE",         &g_key_stride),
    M("AMEDIAFORMAT_KEY_ROTATION",       &g_key_rotation),
    M("AMEDIAFORMAT_KEY_ENCODER_DELAY",  &g_key_encoder_delay),
    M("AMEDIAFORMAT_KEY_SLICE_HEIGHT",   &g_key_slice_height),
    M("AMEDIAFORMAT_KEY_AAC_DRC_ATTENUATION_FACTOR", &g_key_aac_drc_attenuation_factor),
    M("AMEDIAFORMAT_KEY_AAC_DRC_BOOST_FACTOR", &g_key_aac_drc_boost_factor),
    M("AMEDIAFORMAT_KEY_AAC_DRC_HEAVY_COMPRESSION", &g_key_aac_drc_heavy_compression),
    M("AMEDIAFORMAT_KEY_AAC_DRC_TARGET_REFERENCE_LEVEL", &g_key_aac_drc_target_reference_level),
    M("AMEDIAFORMAT_KEY_AAC_ENCODED_TARGET_LEVEL", &g_key_aac_encoded_target_level),
    M("AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT", &g_key_aac_max_output_channel_count),
    M("AMEDIAFORMAT_KEY_AAC_PROFILE", &g_key_aac_profile),
    M("AMEDIAFORMAT_KEY_AUDIO_SESSION_ID", &g_key_audio_session_id),
    M("AMEDIAFORMAT_KEY_BITRATE_MODE", &g_key_bitrate_mode),
    M("AMEDIAFORMAT_KEY_BIT_RATE", &g_key_bit_rate),
    M("AMEDIAFORMAT_KEY_CAPTURE_RATE", &g_key_capture_rate),
    M("AMEDIAFORMAT_KEY_CHANNEL_MASK", &g_key_channel_mask),
    M("AMEDIAFORMAT_KEY_COMPLEXITY", &g_key_complexity),
    M("AMEDIAFORMAT_KEY_CSD", &g_key_csd),
    M("AMEDIAFORMAT_KEY_CSD_0", &g_key_csd_0),
    M("AMEDIAFORMAT_KEY_CSD_1", &g_key_csd_1),
    M("AMEDIAFORMAT_KEY_CSD_2", &g_key_csd_2),
    M("AMEDIAFORMAT_KEY_DISPLAY_CROP", &g_key_display_crop),
    M("AMEDIAFORMAT_KEY_DISPLAY_HEIGHT", &g_key_display_height),
    M("AMEDIAFORMAT_KEY_DISPLAY_WIDTH", &g_key_display_width),
    M("AMEDIAFORMAT_KEY_FRAME_COUNT", &g_key_frame_count),
    M("AMEDIAFORMAT_KEY_GRID_COLUMNS", &g_key_grid_columns),
    M("AMEDIAFORMAT_KEY_GRID_ROWS", &g_key_grid_rows),
    M("AMEDIAFORMAT_KEY_HDR10_PLUS_INFO", &g_key_hdr10_plus_info),
    M("AMEDIAFORMAT_KEY_HDR_STATIC_INFO", &g_key_hdr_static_info),
    M("AMEDIAFORMAT_KEY_IS_ADTS", &g_key_is_adts),
    M("AMEDIAFORMAT_KEY_IS_AUTOSELECT", &g_key_is_autoselect),
    M("AMEDIAFORMAT_KEY_IS_DEFAULT", &g_key_is_default),
    M("AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE", &g_key_is_forced_subtitle),
    M("AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", &g_key_i_frame_interval),
    M("AMEDIAFORMAT_KEY_LATENCY", &g_key_latency),
    M("AMEDIAFORMAT_KEY_LEVEL", &g_key_level),
    M("AMEDIAFORMAT_KEY_LOCATION", &g_key_location),
    M("AMEDIAFORMAT_KEY_LOOP", &g_key_loop),
    M("AMEDIAFORMAT_KEY_LOW_LATENCY", &g_key_low_latency),
    M("AMEDIAFORMAT_KEY_MAX_BIT_RATE", &g_key_max_bit_rate),
    M("AMEDIAFORMAT_KEY_MAX_FPS_TO_ENCODER", &g_key_max_fps_to_encoder),
    M("AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", &g_key_max_input_size),
    M("AMEDIAFORMAT_KEY_MAX_PTS_GAP_TO_ENCODER", &g_key_max_pts_gap_to_encoder),
    M("AMEDIAFORMAT_KEY_MPEG2_STREAM_HEADER", &g_key_mpeg2_stream_header),
    M("AMEDIAFORMAT_KEY_MPEGH_COMPATIBLE_SETS", &g_key_mpegh_compatible_sets),
    M("AMEDIAFORMAT_KEY_MPEGH_PROFILE_LEVEL_INDICATION", &g_key_mpegh_profile_level_indication),
    M("AMEDIAFORMAT_KEY_MPEGH_REFERENCE_CHANNEL_LAYOUT", &g_key_mpegh_reference_channel_layout),
    M("AMEDIAFORMAT_KEY_PCM_ENCODING", &g_key_pcm_encoding),
    M("AMEDIAFORMAT_KEY_PROFILE", &g_key_profile),
    M("AMEDIAFORMAT_KEY_PSSH", &g_key_pssh),
    M("AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP", &g_key_push_blank_buffers_on_stop),
    M("AMEDIAFORMAT_KEY_QUALITY", &g_key_quality),
    M("AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER", &g_key_repeat_previous_frame_after),
    M("AMEDIAFORMAT_KEY_SAR_HEIGHT", &g_key_sar_height),
    M("AMEDIAFORMAT_KEY_SAR_WIDTH", &g_key_sar_width),
    M("AMEDIAFORMAT_KEY_SEI", &g_key_sei),
    M("AMEDIAFORMAT_KEY_TEMPORAL_LAYER_ID", &g_key_temporal_layer_id),
    M("AMEDIAFORMAT_KEY_TEMPORAL_LAYERING", &g_key_temporal_layering),
    M("AMEDIAFORMAT_KEY_TEXT_FORMAT_DATA", &g_key_text_format_data),
    M("AMEDIAFORMAT_KEY_THUMBNAIL_CSD_HEVC", &g_key_thumbnail_csd_hevc),
    M("AMEDIAFORMAT_KEY_THUMBNAIL_HEIGHT", &g_key_thumbnail_height),
    M("AMEDIAFORMAT_KEY_THUMBNAIL_TIME", &g_key_thumbnail_time),
    M("AMEDIAFORMAT_KEY_THUMBNAIL_WIDTH", &g_key_thumbnail_width),
    M("AMEDIAFORMAT_KEY_TIME_US", &g_key_time_us),
    M("AMEDIAFORMAT_KEY_TITLE", &g_key_title),
    M("AMEDIAFORMAT_KEY_TRACK_ID", &g_key_track_id),
    M("AMEDIAFORMAT_KEY_TRACK_INDEX", &g_key_track_index),
    M("AMEDIAFORMAT_KEY_VALID_SAMPLES", &g_key_valid_samples),
    M("AMEDIAFORMAT_KEY_YEAR", &g_key_year),
    M("AMEDIAFORMAT_KEY_EXIF_OFFSET", &g_key_exif_offset),
    M("AMEDIAFORMAT_KEY_EXIF_SIZE", &g_key_exif_size),
    M("AMEDIAFORMAT_KEY_ICC_PROFILE", &g_key_icc_profile),
    M("AMEDIAFORMAT_KEY_SAMPLE_FILE_OFFSET", &g_key_sample_file_offset),
    M("AMEDIAFORMAT_KEY_LAST_SAMPLE_INDEX_IN_CHUNK", &g_key_last_sample_index_in_chunk),
    M("AMEDIAFORMAT_KEY_CREATE_INPUT_SURFACE_SUSPENDED", &g_key_create_input_surface_suspended),

    M("AMediaCodec_createDecoderByType",  klm_AMediaCodec_createDecoderByType),
    M("AMediaCodec_configure",            klm_AMediaCodec_configure),
    M("AMediaCodec_start",                klm_AMediaCodec_start),
    M("AMediaCodec_stop",                 klm_AMediaCodec_stop),
    M("AMediaCodec_flush",                klm_AMediaCodec_flush),
    M("AMediaCodec_delete",               klm_AMediaCodec_delete),
    M("AMediaCodec_dequeueInputBuffer",   klm_AMediaCodec_dequeueInputBuffer),
    M("AMediaCodec_getInputBuffer",       klm_AMediaCodec_getInputBuffer),
    M("AMediaCodec_queueInputBuffer",     klm_AMediaCodec_queueInputBuffer),
    M("AMediaCodec_dequeueOutputBuffer",  klm_AMediaCodec_dequeueOutputBuffer),
    M("AMediaCodec_releaseOutputBuffer",  klm_AMediaCodec_releaseOutputBuffer),
    M("AMediaCodec_getOutputBuffer",      klm_AMediaCodec_getOutputBuffer),
    M("AMediaCodec_getOutputFormat",      klm_AMediaCodec_getOutputFormat),

    // The demuxer, which REFUSES — see the long note by the implementation.
    M("AMediaExtractor_new",                 klm_AMediaExtractor_new),
    M("AMediaExtractor_delete",              klm_AMediaExtractor_delete),
    M("AMediaExtractor_setDataSource",       klm_AMediaExtractor_setDataSource),
    M("AMediaExtractor_setDataSourceFd",     klm_AMediaExtractor_setDataSourceFd),
    M("AMediaExtractor_setDataSourceCustom", klm_AMediaExtractor_setDataSourceCustom),
    M("AMediaDataSource_new",                klm_AMediaDataSource_new),
    M("AMediaDataSource_delete",             klm_AMediaDataSource_delete),
    M("AMediaDataSource_setUserdata",        klm_AMediaDataSource_setUserdata),
    M("AMediaDataSource_setReadAt",          klm_AMediaDataSource_setReadAt),
    M("AMediaDataSource_setGetSize",         klm_AMediaDataSource_setGetSize),
    M("AMediaDataSource_setClose",           klm_AMediaDataSource_setClose),
    M("AMediaExtractor_getTrackCount",       klm_AMediaExtractor_getTrackCount),
    M("AMediaExtractor_getTrackFormat",      klm_AMediaExtractor_getTrackFormat),
    M("AMediaExtractor_selectTrack",         klm_AMediaExtractor_selectTrack),
    M("AMediaExtractor_readSampleData",      klm_AMediaExtractor_readSampleData),
    M("AMediaExtractor_getSampleTrackIndex", klm_AMediaExtractor_getSampleTrackIndex),
    M("AMediaExtractor_getSampleTime",       klm_AMediaExtractor_getSampleTime),
    M("AMediaExtractor_advance",             klm_AMediaExtractor_advance),
    M("AMediaExtractor_seekTo",              klm_AMediaExtractor_seekTo),

    M("AImageReader_newWithUsage",       klm_AImageReader_newWithUsage),
    M("AImageReader_setImageListener",   klm_AImageReader_setImageListener),
    M("AImageReader_getWindow",          klm_AImageReader_getWindow),
    M("AImageReader_acquireLatestImage", klm_AImageReader_acquireLatestImage),
    M("AImageReader_delete",             klm_AImageReader_delete),

    M("AImage_getWidth",          klm_AImage_getWidth),
    M("AImage_getHeight",         klm_AImage_getHeight),
    M("AImage_getTimestamp",      klm_AImage_getTimestamp),
    M("AImage_getHardwareBuffer", klm_AImage_getHardwareBuffer),
    M("AImage_delete",            klm_AImage_delete),

    M("AHardwareBuffer_acquire", klm_AHardwareBuffer_acquire),
    M("AHardwareBuffer_release", klm_AHardwareBuffer_release),

    M("ATrace_beginSection", klm_ATrace_beginSection),
    M("ATrace_endSection",   klm_ATrace_endSection),
    M("AMediaCodec_createCodecByName",       klm_AMediaCodec_createCodecByName),
    M("AMediaCodec_createEncoderByType",     klm_AMediaCodec_createEncoderByType),
    M("AMediaCodec_setOutputSurface",        klm_AMediaCodec_setOutputSurface),
    M("AMediaCodec_createInputSurface",      klm_AMediaCodec_createInputSurface),
    M("AMediaCodec_createPersistentInputSurface", klm_AMediaCodec_createPersistentInputSurface),
    M("AMediaCodec_setInputSurface",         klm_AMediaCodec_setInputSurface),
    M("AMediaCodec_signalEndOfInputStream",  klm_AMediaCodec_signalEndOfInputStream),
    M("AMediaCodec_setParameters",           klm_AMediaCodec_setParameters),
    M("AMediaCodec_getName",                 klm_AMediaCodec_getName),
    M("AMediaCodec_releaseName",             klm_AMediaCodec_releaseName),
    M("AMediaCodec_getInputFormat",          klm_AMediaCodec_getInputFormat),
    M("AMediaCodec_getBufferFormat",         klm_AMediaCodec_getBufferFormat),
    M("AMediaCodec_releaseOutputBufferAtTime", klm_AMediaCodec_releaseOutputBufferAtTime),
    M("AMediaCodec_queueSecureInputBuffer",  klm_AMediaCodec_queueSecureInputBuffer),
    M("AMediaCodec_setAsyncNotifyCallback",  klm_AMediaCodec_setAsyncNotifyCallback),

    M("AMediaCodecCryptoInfo_new",             klm_AMediaCodecCryptoInfo_new),
    M("AMediaCodecCryptoInfo_delete",          klm_AMediaCodecCryptoInfo_delete),
    M("AMediaCodecCryptoInfo_setPattern",      klm_AMediaCodecCryptoInfo_setPattern),
    M("AMediaCodecCryptoInfo_getNumSubSamples", klm_AMediaCodecCryptoInfo_getNumSubSamples),
    M("AMediaCodecCryptoInfo_getKey",          klm_AMediaCodecCryptoInfo_getKey),
    M("AMediaCodecCryptoInfo_getIV",           klm_AMediaCodecCryptoInfo_getIV),
    M("AMediaCodecCryptoInfo_getMode",         klm_AMediaCodecCryptoInfo_getMode),
    M("AMediaCodecCryptoInfo_getClearBytes",   klm_AMediaCodecCryptoInfo_getClearBytes),
    M("AMediaCodecCryptoInfo_getEncryptedBytes", klm_AMediaCodecCryptoInfo_getEncryptedBytes),

    M("AMediaCrypto_isCryptoSchemeSupported",        klm_AMediaCrypto_isCryptoSchemeSupported),
    M("AMediaCrypto_requiresSecureDecoderComponent", klm_AMediaCrypto_requiresSecureDecoderComponent),
    M("AMediaCrypto_new",                            klm_AMediaCrypto_new),
    M("AMediaCrypto_delete",                         klm_AMediaCrypto_delete),
    M("AMediaDrm_isCryptoSchemeSupported",           klm_AMediaDrm_isCryptoSchemeSupported),
    M("AMediaDrm_createByUUID",                      klm_AMediaDrm_createByUUID),
    M("AMediaDrm_release",                           klm_AMediaDrm_release),

    M("AMediaDataSource_newUri",             klm_AMediaDataSource_newUri),
    M("AMediaDataSource_setGetAvailableSize", klm_AMediaDataSource_setGetAvailableSize),
    M("AMediaDataSource_close",              klm_AMediaDataSource_close),
    M("AMediaExtractor_getFileFormat",       klm_AMediaExtractor_getFileFormat),
    M("AMediaExtractor_getSampleFormat",     klm_AMediaExtractor_getSampleFormat),
    M("AMediaExtractor_getSampleSize",       klm_AMediaExtractor_getSampleSize),
    M("AMediaExtractor_getSampleFlags",      klm_AMediaExtractor_getSampleFlags),
    M("AMediaExtractor_getCachedDuration",   klm_AMediaExtractor_getCachedDuration),
    M("AMediaExtractor_unselectTrack",       klm_AMediaExtractor_unselectTrack),
    M("AMediaExtractor_getPsshInfo",         klm_AMediaExtractor_getPsshInfo),
    M("AMediaExtractor_getSampleCryptoInfo", klm_AMediaExtractor_getSampleCryptoInfo),

    M("AImageReader_new",                     klm_AImageReader_new),
    M("AImageReader_getWidth",                klm_AImageReader_getWidth),
    M("AImageReader_getHeight",               klm_AImageReader_getHeight),
    M("AImageReader_getFormat",               klm_AImageReader_getFormat),
    M("AImageReader_getMaxImages",            klm_AImageReader_getMaxImages),
    M("AImageReader_acquireNextImage",        klm_AImageReader_acquireNextImage),
    M("AImageReader_acquireNextImageAsync",   klm_AImageReader_acquireNextImageAsync),
    M("AImageReader_acquireLatestImageAsync", klm_AImageReader_acquireLatestImageAsync),
    M("AImageReader_setBufferRemovedListener", klm_AImageReader_setBufferRemovedListener),
    M("AImage_getFormat",         klm_AImage_getFormat),
    M("AImage_getCropRect",       klm_AImage_getCropRect),
    M("AImage_deleteAsync",       klm_AImage_deleteAsync),
    M("AImage_getNumberOfPlanes", klm_AImage_getNumberOfPlanes),
    M("AImage_getPlaneRowStride", klm_AImage_getPlaneRowStride),
    M("AImage_getPlanePixelStride", klm_AImage_getPlanePixelStride),
    M("AImage_getPlaneData",      klm_AImage_getPlaneData),

    M("AHardwareBuffer_allocate",     klm_AHardwareBuffer_allocate),
    M("AHardwareBuffer_describe",     klm_AHardwareBuffer_describe),
    M("AHardwareBuffer_lock",         klm_AHardwareBuffer_lock),
    M("AHardwareBuffer_lockPlanes",   klm_AHardwareBuffer_lockPlanes),
    M("AHardwareBuffer_lockAndGetInfo", klm_AHardwareBuffer_lockAndGetInfo),
    M("AHardwareBuffer_unlock",       klm_AHardwareBuffer_unlock),
    M("AHardwareBuffer_isSupported",  klm_AHardwareBuffer_isSupported),
    M("AHardwareBuffer_getId",        klm_AHardwareBuffer_getId),

    M("ATrace_isEnabled",         klm_ATrace_isEnabled),
    M("ATrace_beginAsyncSection", klm_ATrace_beginAsyncSection),
    M("ATrace_endAsyncSection",   klm_ATrace_endAsyncSection),
    M("ATrace_setCounter",        klm_ATrace_setCounter),
};

void *kl_mediandk_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_media / sizeof g_media[0]; i++)
        if (!strcmp(g_media[i].name, name)) return g_media[i].fn;
    return NULL;                // must be able to say no — see the header
}

// ---------------------------------------------------------------------------
// The dlopen door. Beat Saber's FMOD never asks; this exists because a guest
// that dlopens libmediandk.so must get a handle it can dlsym through, and
// because libOpenMAXAL.so has no implementation at all and should say so by
// name rather than by null pointer.

static const char g_md_handle[]  = "klepton-mediandk";
static const char g_oma_handle[] = "klepton-openmaxal";

int kl_mediandk_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libmediandk.so") == 0 || strcmp(b, "libOpenMAXAL.so") == 0;
}

void *kl_mediandk_dlopen(const char *soname) {
    if (!kl_mediandk_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    if (strcmp(b, "libmediandk.so") == 0) return (void *)g_md_handle;
    fprintf(stderr, "  [media] guest dlopen(\"%s\") -> stub handle "
                    "(OpenMAX AL is not implemented)\n", b);
    return (void *)g_oma_handle;
}

int kl_mediandk_is_handle(const void *h) {
    return h == (const void *)g_md_handle || h == (const void *)g_oma_handle;
}

void *kl_mediandk_sym(const char *name) {
    void *fn = kl_mediandk_lookup(name);
    // dlsym semantics differ from the ELF-import door's: a miss here becomes a
    // named stub, because the guest is asking at runtime and deserves to fail
    // where it CALLS rather than where it looked up.
    return fn ? fn : kl_named_stub(name, (void *)kl_unresolved_named);
}
