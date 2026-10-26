// kl_ovrp_render.c - Lote 1: ovrp render / display / rendimiento / sistema.
//
// Registered with kl_ovrp.c through kl_ovrp_render_register (see kl_ovrp_priv.h for the
// recipe). This file owns 141 OVRPlugin entry points that no other file answers:
// the legacy (v0.x/1.x) spellings of calls kl_ovrp.c already serves under a newer
// name, per-eye render state, the ASW switches, performance/battery/system
// queries, and the render-side refusals.
//
// RULES THIS BATCH FOLLOWS (project rules + what kl_ovrp.c already tells the guest)
//   * ASW / Application SpaceWarp and every foveation knob are OFF. Setters are
//     accepted and have no effect; getters report "off".
//   * Everything is offline. Nothing here talks to a service; no user data is read.
//   * Frequencies / eye size / frustum / IPD / battery come from the same
//     kl_ovrp_* seams kl_ovrp.c answers its numbered siblings from, so two
//     spellings of one question cannot disagree.
//
// ABI CONVENTION (read off kl_ovrp.c's own siblings, not inferred from the names)
//   * Un-suffixed v0.x/1.x getters return their VALUE (float in s0, Bool/int in w0,
//     small struct by value); the "...2" spelling is `ovrpResult f(..., T *out)`.
//     kl_ovrp.c has the pairs: GetUserIPD (float) / GetUserIPD2 (out),
//     GetSystemBatteryLevel (float) / ...2, GetSystemBatteryStatus (int) / ...2,
//     GetEyeTextureSize (Sizei packed in x0). So a legacy getter here never
//     dereferences x0: when its ABI is unknowable a stale register is not a
//     pointer, and a write through it would corrupt the guest's memory.
//   * Setters of the legacy Bool era answer 1; of the Result era 0 (a Result
//     0 read as a Bool is "false", a Bool 1 read as a Result is a positive
//     "success"; each family below picks the era its siblings are in).
//   * Out-pointers are written only when pok() says they can be a pointer.
//   * Struct returns. The real plugin (nine builds, tools/ovrp_abi.py) returns the
//     eye/tracking-origin pose queries as a 28-byte struct THROUGH x8 (also the
//     velocity and acceleration ones: identity orientation, the vector in
//     Position), so those go through kl_ovrp_sret2.S (x8 sampled before any call,
//     then a plain C *_impl); GetHmdToEyeOffset and GetEyeFrustum come back in
//     s0..s2 / s0..s3 (HFA). The ABI of every function below was checked against
//     those builds, not inferred from the names: the earlier "Bool era answers 1"
//     reading was wrong for Initialize3/4, GetLayerTexture, the Fov getters,
//     SetDepthCompositingInfo and others, which return ovrpResult.
#include "kl_ovrp_priv.h"
#include "kl_openxr.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
#define FN(name) kl_ovrp_hit("ovrp_" #name)

// Could this register be a user-space pointer? Rejects NULL, small integers (eye
// and stage indices, bools) and anything with the top 16 bits set. It is a
// guard against a register that is NOT an out-parameter, not a validity proof.
static inline int pok(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000u && (v >> 48) == 0;
}

static inline float f_get(uint32_t *a) {
    uint32_t b = __atomic_load_n(a, __ATOMIC_RELAXED); float f; memcpy(&f, &b, 4); return f;
}
static inline void f_set(uint32_t *a, float f) {
    uint32_t b; memcpy(&b, &f, 4); __atomic_store_n(a, b, __ATOMIC_RELAXED);
}

// Where a legacy sibling lives. Looked up by name once; kl_ovrp_sym counts the
// lookup as a resolution, which is harmless. NULL if kl_ovrp.c does not have it.
static void *sib(const char *name) { return kl_ovrp_sym(name); }

// ---------------------------------------------------------------------------
// State the setters record and the getters read back. Defaults are Quest's.
// ---------------------------------------------------------------------------
#define KLR_DEFAULT_EYE_DEPTH 0.0805f      // OVRProfile.eyeDepth / neckEyeDistance default (m)
#define KLR_KEYS 16
static struct {
    int      eye_shared;
    int      srgb;                  // 1 until the app says otherwise
    uint32_t eye_height;            // float bits; 0 = never set, report the measured one
    uint32_t eye_depth;             // float bits
    uint32_t neck_eye;              // float bits
    int      cpu_priority;
    uint32_t caps;
    int      mv_tiled_level;
    int      invisible_pose;
    int      frame_counter;         // legacy ovrp_Update has no frame index of its own
    void *log_cb;
    int      float_key[KLR_KEYS];   // key + 1, 0 = free slot
    uint32_t float_val[KLR_KEYS];
    int      vr_key[KLR_KEYS];
    int      vr_val[KLR_KEYS];
} g = {
    .srgb = 1, .caps = 0x3u,                // tracking + stereo
    .eye_depth = 0, .neck_eye = 0,
};
static float eye_depth_get(void) { float f = f_get(&g.eye_depth); return f > 0 ? f : KLR_DEFAULT_EYE_DEPTH; }
static float neck_eye_get(void)  { float f = f_get(&g.neck_eye);  return f > 0 ? f : KLR_DEFAULT_EYE_DEPTH; }

static int slot_find(int *keys, int key, int create) {
    for (int i = 0; i < KLR_KEYS; i++)
        if (__atomic_load_n(&keys[i], __ATOMIC_RELAXED) == key + 1) return i;
    if (!create) return -1;
    for (int i = 0; i < KLR_KEYS; i++) {
        int expect = 0;
        if (__atomic_compare_exchange_n(&keys[i], &expect, key + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return i;
    }
    return -1;                              // table full: the set is dropped, the get stays default
}

// Eye geometry exactly as GetNodeFrustum2 / GetNodePoseState3 hand it out.
static void eye_tangents(int eye, float t[4]) {
    float px, py, pz, qx, qy, qz, qw;
    kl_ovrp_eye_view(eye == 1 ? 1 : 0, &px, &py, &pz, &qx, &qy, &qz, &qw, t);
    for (int i = 0; i < 4; i++) if (!isfinite(t[i]) || t[i] <= 0.01f) t[i] = 1.0f;
}
static void qrot(const float q[4], const float v[3], float o[3]) {   // q = x,y,z,w
    float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
    o[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    o[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    o[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
static void qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static ovrpPosef eye_pose(int eye) {
    kl_ovrp_pose7 h; kl_ovrp_guest_head(0, &h);
    float off[3] = {0, 0, 0}, rot[4] = {0, 0, 0, 1};
    kl_ovrp_eye_local(eye == 1 ? 1 : 0, off, rot);
    float hq[4] = { h.qx, h.qy, h.qz, h.qw }, p[3], q[4];
    qrot(hq, off, p);
    qmul(hq, rot, q);
    ovrpPosef r;
    r.Orientation = (ovrpQuatf){ q[0], q[1], q[2], q[3] };
    r.Position = (ovrpVector3f){ h.px + p[0], h.py + p[1], h.pz + p[2] };
    return r;
}

// ===========================================================================
// 1. LEGACY spellings -> the numbered sibling kl_ovrp.c already serves
//    (never reached once ovrp_GetVersion >= 1.20; answered so a stale wrapper
//    cannot abort the run)
// ===========================================================================
static void init_via_7(void) {
    uint64_t (*f)(void) = sib("ovrp_Initialize7");
    if (f) (void)f();
}
// Initialize / Initialize2 are ovrpBool in 1.59 and unclassifiable above it: 1
// reads as "ok" both as a Bool and as a non-negative Result. Initialize3/4 are
// ovrpResult in every build that has them (RESULT{-1003,0}): success is 0.
static int32_t r_Initialize(void)  { FN(Initialize);  init_via_7(); return 1; }
static int32_t r_Initialize2(void) { FN(Initialize2); init_via_7(); return 1; }
static ovrpResult r_Initialize3(void) { FN(Initialize3); init_via_7(); return KLOVRP_SUCCESS; }
static ovrpResult r_Initialize4(void) { FN(Initialize4); init_via_7(); return KLOVRP_SUCCESS; }
// Same success no-op every PreInitialize arity gives (kl_ovrp.c's ok list).
static int32_t r_PreInitialize2(void) { FN(PreInitialize2); return 0; }

// Frame lifecycle: the guest ignores these returns; pass the sibling's through.
static int32_t legacy_begin(int frame) {
    uint64_t (*f)(int, uint64_t) = sib("ovrp_BeginFrame4");
    return f ? (int32_t)f(frame, 0) : 0;
}
static int32_t legacy_end(int frame) {
    uint64_t (*f)(int) = sib("ovrp_EndFrame");
    return f ? (int32_t)f(frame) : 0;
}
static int32_t r_BeginFrame2(int frame) { FN(BeginFrame2); return legacy_begin(frame); }
static int32_t r_BeginFrame3(int frame) { FN(BeginFrame3); return legacy_begin(frame); }
static int32_t r_EndFrame2(int frame)   { FN(EndFrame2);   return legacy_end(frame); }
static int32_t r_EndFrame3(int frame)   { FN(EndFrame3);   return legacy_end(frame); }
static int32_t r_EndEye(void)           { FN(EndEye);      return 0; }   // = ovrp_EndEye2 (ok list)

static int32_t r_EnqueueSetupLayer(const void *desc, int *layer_id) {
    FN(EnqueueSetupLayer);
    uint64_t (*f)(const void *, int, int *) = sib("ovrp_EnqueueSetupLayer2");
    if (!f || !desc || !pok(layer_id)) return KLOVRP_INVALID_PARAM;
    return (int32_t)f(desc, 0, layer_id);
}
static int32_t r_EnqueueSubmitLayer(uint32_t flags, void *texL, void *texR, int layer_id,
                                    int frame_index, const float *pose, const float *scale,
                                    int layer_index) {
    FN(EnqueueSubmitLayer);
    uint64_t (*f)(uint32_t, void *, void *, int, int, const float *, const float *, int,
                  int, const float *, int, const float *, const float *) =
        sib("ovrp_EnqueueSubmitLayer2");
    if (!f) return 0;
    return (int32_t)f(flags, texL, texR, layer_id, frame_index, pose, scale, layer_index,
                      0, NULL, 0, NULL, NULL);
}
// ovrpResult ovrp_CalculateEyeLayerDesc(layout, mipLevels, sampleCount, format,
// layerFlags, ovrpLayerDescEye *out, float textureScale): the real build reads
// five ints, the out pointer in x5 and the scale in s0 (tools/ovrp_abi.py, all
// nine versions; in 1.59/1.81 the scale is a double - it only feeds the desc's
// TextureSize, and a stray s0 is clamped by the builder). It is
// CalculateEyeLayerDesc2 with the depth format left at 10 (D24_S8), exactly what
// kl_ovrp.c records of the real un-suffixed body.
static ovrpResult r_CalculateEyeLayerDesc(int layout, int mip_levels, int sample_count, int format,
                                          int layer_flags, void *out, float texture_scale) {
    FN(CalculateEyeLayerDesc);
    uint64_t (*f)(int, int, int, int, int, int, void *, float) = sib("ovrp_CalculateEyeLayerDesc2");
    if (!out) return KLOVRP_INVALID_PARAM;
    if (!f) return KLOVRP_UNSUPPORTED;
    return (ovrpResult)f(layout, mip_levels, sample_count, format, 10, layer_flags, out, texture_scale);
}

static int32_t r_Update(void) {
    FN(Update);
    uint64_t (*f)(int, int, double) = sib("ovrp_Update2");
    if (f) (void)f(0, __atomic_add_fetch(&g.frame_counter, 1, __ATOMIC_RELAXED), 0.0);
    return 1;
}
static int32_t r_SetupDistortionWindow2(void) { FN(SetupDistortionWindow2); return 1; }   // = ...Window3

static int32_t r_SetupEyeTexture(int eye, int stage, uintptr_t handle) {
    FN(SetupEyeTexture);
    uint64_t (*f)(int, int, uintptr_t, int, int, int, int, void *) = sib("ovrp_SetupEyeTexture2");
    if (!f) return 1;
    int w = 0, h = 0; kl_ovrp_eye_texture_size(&w, &h);
    return (int32_t)f(eye, stage, handle, w, h, 0, 2, NULL);   // fmt 2 = the eye format SetupEyeTexture2 maps
}
// SetOverlayQuad3 acknowledges and ignores its arguments; so do these.
static int32_t r_SetOverlayQuad(void)  { FN(SetOverlayQuad);  return KLOVRP_TRUE; }
static int32_t r_SetOverlayQuad2(void) { FN(SetOverlayQuad2); return KLOVRP_TRUE; }

// ovrpResult ovrp_GetLayerTexture(layerId, stage, eye, void **out) in every
// build read (RESULT{-1002,-1001}): the texture goes through x3, 0 is success.
static ovrpResult r_GetLayerTexture(int layer_id, int stage, int eye, void **out) {
    FN(GetLayerTexture);
    uint64_t (*f)(int, int, int, uint64_t *, uint64_t *) = sib("ovrp_GetLayerTexture2");
    uint64_t color = 0;
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    if (!f) return KLOVRP_UNSUPPORTED;
    ovrpResult r = (ovrpResult)f(layer_id, stage, eye, &color, NULL);
    if (r < 0) return r;
    *out = (void *)(uintptr_t)color;
    return KLOVRP_SUCCESS;
}

// Performance: the un-suffixed forms return a float (s0). Values agree with the
// "...2" refusals in kl_ovrp.c: no compositor timing is measured, scale neutral.
static float r_GetAdaptiveGpuPerformanceScale(void) { FN(GetAdaptiveGpuPerformanceScale); return 1.0f; }
static float r_GetAppCpuStartToGpuEndTime(void)     { FN(GetAppCpuStartToGpuEndTime);     return 0.0f; }
static int32_t r_GetAppLatencyTimings(void)         { FN(GetAppLatencyTimings);           return 0; }

static int32_t r_SetAppCPUPriority(int prio) {
    FN(SetAppCPUPriority);
    __atomic_store_n(&g.cpu_priority, prio, __ATOMIC_RELAXED);
    return 1;
}
static int32_t r_GetAppCPUPriority(void) { FN(GetAppCPUPriority); return __atomic_load_n(&g.cpu_priority, __ATOMIC_RELAXED); }
static int32_t r_GetAppCPUPriority2(int32_t *out) {
    FN(GetAppCPUPriority2);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = __atomic_load_n(&g.cpu_priority, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
// A pointer return: NULL is "no native SDK here" (libunity pre-zeroes the local and
// never dereferences it, see ovrp_GetNativeSDKPointer2). Never -1004.
static void *r_GetNativeSDKPointer(void) { FN(GetNativeSDKPointer); return NULL; }

static int32_t r_SetUserNeckEyeDistance(float v) {
    FN(SetUserNeckEyeDistance);
    if (isfinite(v) && v > 0) f_set(&g.neck_eye, v);
    return 1;
}
static float r_GetUserNeckEyeDistance(void) { FN(GetUserNeckEyeDistance); return neck_eye_get(); }
// ovrpResult ovrp_GetUserNeckEyeDistance2(ovrpVector2f *out): the real build writes
// 8 bytes (RESULT{-1002,-1001,0}); x is the neck->eye depth the setter recorded,
// y (height) is not modelled and stays 0.
static ovrpResult r_GetUserNeckEyeDistance2(ovrpVector2f *out) {
    FN(GetUserNeckEyeDistance2);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    out->x = neck_eye_get();
    out->y = 0.0f;
    return KLOVRP_SUCCESS;
}

// ===========================================================================
// 2. Per-eye render state
// ===========================================================================
// Scales: the eye size kl_ovrp reports IS the recommended size, so 1.0 everywhere.
static float r_GetEyeTextureScale(void)                { FN(GetEyeTextureScale);                return 1.0f; }
static float r_GetEyeViewportScale(void)               { FN(GetEyeViewportScale);               return 1.0f; }
static float r_GetEyeRecommendedResolutionScale(void)  { FN(GetEyeRecommendedResolutionScale);  return 1.0f; }
static int32_t r_GetEyeTextureFlippedY(void)           { FN(GetEyeTextureFlippedY);             return 0; }
// Same answer ovrp_GetEyeTextureArraySupported gives: array eyes iff multiview.
static int32_t r_GetEyeTextureArrayEnabled(void)       { FN(GetEyeTextureArrayEnabled);         return kl_ovrp_multiview() ? 1 : 0; }
static int32_t r_GetEyeTextureShared(void)             { FN(GetEyeTextureShared);               return __atomic_load_n(&g.eye_shared, __ATOMIC_RELAXED); }
static int32_t r_SetEyeTextureShared(int v)            { FN(SetEyeTextureShared); __atomic_store_n(&g.eye_shared, !!v, __ATOMIC_RELAXED); return 1; }
static int32_t r_GetEyeOcclusionMeshEnabled(void)      { FN(GetEyeOcclusionMeshEnabled);        return 0; }   // no mesh (GetEyeOcclusionMesh is "no")
// Both are ovrpResult f(int *out) in the 1.117 build (RESULT{-1002,-1001[,0]}).
static ovrpResult r_GetEyeFovPremultipliedAlphaMode(int32_t *out) {
    FN(GetEyeFovPremultipliedAlphaMode);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 1;
    return KLOVRP_SUCCESS;
}
// The eye layer's id is created by EnqueueSetupLayer*/SetupLayer, not here: this reports the
// live eye layer's id, or 0 = no id while the guest has not set one up.
static ovrpResult r_GetEyeFovLayerId(int32_t *out) {
    FN(GetEyeFovLayerId);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = kl_ovrp_active_eye_layer_id();
    return KLOVRP_SUCCESS;
}

static ovrpVector2f px_per_tan(int eye) {
    float t[4]; eye_tangents(eye, t);
    int w = 0, h = 0; kl_ovrp_eye_texture_size(&w, &h);
    ovrpVector2f v = { (float)w / (t[0] + t[1]), (float)h / (t[2] + t[3]) };
    if (!isfinite(v.x) || v.x <= 0) v.x = 1.0f;
    if (!isfinite(v.y) || v.y <= 0) v.y = 1.0f;
    return v;
}
static ovrpVector2f r_GetEyePixelsPerTanAngleAtCenter(int eye) {
    FN(GetEyePixelsPerTanAngleAtCenter);
    return px_per_tan(eye);
}
static int32_t r_GetEyePixelsPerTanAngleAtCenter2(int eye, ovrpVector2f *out) {
    FN(GetEyePixelsPerTanAngleAtCenter2);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = px_per_tan(eye);
    return KLOVRP_SUCCESS;
}

// Pre-1.20 eye queries, by value as OVRP_0_1_0 declared them (GetEyeTextureSize is
// the same family and is a by-value Sizei in kl_ovrp.c).
// ovrpFrustumf (v0) is {zNear, zFar, FovX, FovY}: four floats, an HFA, so s0..s3
// (the disassembly of GetEyeFrustum/GetNodeFrustum in all nine builds zeroes
// d0..d3 when uninitialised). The fields are degrees: tan->angle of both sides.
typedef struct {
    float zNear;
    float zFar;
    float fovX;
    float fovY;
} klr_frustum_v0;
static klr_frustum_v0 r_GetEyeFrustum(int eye) {
    FN(GetEyeFrustum);
    float t[4]; eye_tangents(eye, t);                       // left, right, up, down
    klr_frustum_v0 f = { kl_ovrp_frustum_near(), 1000.0f,
                         (atanf(t[0]) + atanf(t[1])) * (180.0f / 3.14159265f),
                         (atanf(t[2]) + atanf(t[3])) * (180.0f / 3.14159265f) };
    return f;
}
static ovrpVector3f eye_off(int eye) {
    float off[3] = {0, 0, 0}, rot[4] = {0, 0, 0, 1};
    kl_ovrp_eye_local(eye == 1 ? 1 : 0, off, rot);
    return (ovrpVector3f){ off[0], off[1], off[2] };
}
static ovrpVector3f r_GetHmdToEyeOffset(int eye) { FN(GetHmdToEyeOffset); return eye_off(eye); }
// ovrpResult ovrp_GetHmdToEyeOffset2(eye, ovrpVector3f *out): the real build writes
// 12 bytes (an offset, no rotation) in every version.
static ovrpResult r_GetHmdToEyeOffset2(int eye, ovrpVector3f *out) {
    FN(GetHmdToEyeOffset2);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = eye_off(eye);
    return KLOVRP_SUCCESS;
}
// Pose / velocity / acceleration of an eye: a 28-byte ovrpPosef-shaped struct
// through x8 (kl_ovrp_sret2.S moves x8 into `out`). Velocity and acceleration
// ride in .Position with an identity .Orientation, as in the real build.
static void eye_ret28(void *out, const float q[4], const float v[3]) {
    float f[7] = { q[0], q[1], q[2], q[3], v[0], v[1], v[2] };
    memcpy(out, f, sizeof f);
}
void klovrp_GetEyePose_impl(int eye, void *out) {
    FN(GetEyePose);
    ovrpPosef p = eye_pose(eye);
    memcpy(out, &p, 28);
}
void klovrp_GetEyeVelocity_impl(int eye, void *out) {
    FN(GetEyeVelocity); (void)eye;
    static const float qid[4] = { 0, 0, 0, 1 };
    float vel[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
    if (!kl_ovrp_head_motion(vel, ang)) vel[0] = vel[1] = vel[2] = 0;   // unknown is not "moving"
    eye_ret28(out, qid, vel);
}
void klovrp_GetEyeAcceleration_impl(int eye, void *out) {
    FN(GetEyeAcceleration); (void)eye;
    static const float qid[4] = { 0, 0, 0, 1 }, zero[3] = { 0, 0, 0 };
    eye_ret28(out, qid, zero);
}

// User body numbers. The display's real IPD stays what GetUserIPD/2 report:
// visionOS owns it, so a SetUserIPD from the guest is accepted and ignored.
static float r_GetUserEyeHeight(void) {
    FN(GetUserEyeHeight);
    float h = f_get(&g.eye_height);
    return h > 0 ? h : klovrp_measured_eye_height();
}
static int32_t r_SetUserEyeHeight(float v) {
    FN(SetUserEyeHeight);
    if (isfinite(v) && v > 0) f_set(&g.eye_height, v);
    return 1;
}
static int32_t r_SetUserEyeHeight2(float v) {
    FN(SetUserEyeHeight2);
    if (isfinite(v) && v > 0) f_set(&g.eye_height, v);
    return KLOVRP_SUCCESS;
}
static float r_GetUserEyeDepth(void) { FN(GetUserEyeDepth); return eye_depth_get(); }
static int32_t r_SetUserEyeDepth(float v) {
    FN(SetUserEyeDepth);
    if (isfinite(v) && v > 0) f_set(&g.eye_depth, v);
    return 1;
}
static int32_t r_SetUserIPD(void)  { FN(SetUserIPD);  return 1; }
static int32_t r_SetUserIPD2(void) { FN(SetUserIPD2); return KLOVRP_SUCCESS; }

static int32_t r_GetAppSRGB(void)        { FN(GetAppSRGB); return __atomic_load_n(&g.srgb, __ATOMIC_RELAXED); }
static int32_t r_SetAppSRGB(int v)       { FN(SetAppSRGB); __atomic_store_n(&g.srgb, !!v, __ATOMIC_RELAXED); return 1; }
static int32_t r_SetAppMonoscopic(void)  { FN(SetAppMonoscopic); return 1; }   // recorded; the render stays stereo

static int32_t r_SetEyeTexture(void)      { FN(SetEyeTexture);      return 1; }
static int32_t r_ReleaseEyeTexture(void)  { FN(ReleaseEyeTexture);  return 1; }
static int32_t r_RecreateEyeTexture(void) { FN(RecreateEyeTexture); return 1; }

// ovrpResult ovrp_GetEyeLayerRecommendedResolution(ovrpSizei *out): the real build
// (1.88 .. 1.117) has the out pointer as its ONLY argument (RESULT, x0 ptr 8 B).
static ovrpResult r_GetEyeLayerRecommendedResolution(ovrpSizei *out) {
    FN(GetEyeLayerRecommendedResolution);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    // The fixed recommended eye size, never the size of the layer the guest built (that fed the
    // guest's own choice back into what it was told). No live eye layer yet: -1004 without
    // writing (the good build's behaviour).
    int w = 0, h = 0;
    if (!kl_ovrp_active_eye_layer_size(&w, &h)) return (ovrpResult)-1004;
    kl_ovrp_eye_texture_size(&w, &h);
    static int last_w, last_h;
    if (w != last_w || h != last_h) {
        last_w = w; last_h = h;
        fprintf(stderr, "  [ovrp] GetEyeLayerRecommendedResolution -> %dx%d (recommended eye size)\n", w, h);
    }
    out->w = w; out->h = h;
    return KLOVRP_SUCCESS;
}
static int32_t r_SetEyeBufferSecure(void) { FN(SetEyeBufferSecure); return KLOVRP_SUCCESS; }
static int32_t r_GetBufferCount(void) { FN(GetBufferCount); return kl_ovrp_stage_count(); }

// Layer extras. The layer family is Result-typed (kl_ovrp.c: every member ends in
// `csel w0,w0,wzr,lt`), so these answer 0.
// ovrpResult ovrp_SetDepthCompositingInfo(int, float, float) (the two floats are
// doubles in 1.59/1.81; ignored either way: the compositor ignores depth info).
static ovrpResult r_SetDepthCompositingInfo(int mode, float near_z, float far_z) {
    FN(SetDepthCompositingInfo);
    (void)mode; (void)near_z; (void)far_z;
    return KLOVRP_SUCCESS;
}
static int32_t r_SetupLayerDepth(void)               { FN(SetupLayerDepth);               return KLOVRP_SUCCESS; }
static int32_t r_SetOctilinearInfo(void)             { FN(SetOctilinearInfo);             return KLOVRP_SUCCESS; }   // no octilinear foveation
static int32_t r_SetExternalLayerDynresEnabled(void) { FN(SetExternalLayerDynresEnabled); return KLOVRP_SUCCESS; }
static int32_t r_UpdateFoveation(void)               { FN(UpdateFoveation);               return KLOVRP_SUCCESS; }   // foveation off by rule

// Eye-tracked foveation centre: screen centre, no eye tracking. The (eye, out) or
// (out) shape is resolved like GetEyeLayerRecommendedResolution.
static int32_t r_GetFoveationEyeTrackedCenter(uintptr_t a, uintptr_t b) {
    FN(GetFoveationEyeTrackedCenter);
    ovrpVector2f *out = pok((void *)a) ? (ovrpVector2f *)a : (pok((void *)b) ? (ovrpVector2f *)b : NULL);
    if (!out) return KLOVRP_INVALID_PARAM;
    out->x = 0.5f; out->y = 0.5f;
    return KLOVRP_SUCCESS;
}

// No mirror window on visionOS. The Bool-era setup says "false"; the "2" form says
// OperationFailed. Teardown of something that never existed succeeds.
static int32_t r_SetupMirrorTexture(void)    { FN(SetupMirrorTexture);    return 0; }
static int32_t r_SetupMirrorTexture2(void)   { FN(SetupMirrorTexture2);   return KLOVRP_OPERATION_FAILED; }
static int32_t r_DestroyMirrorTexture(void)  { FN(DestroyMirrorTexture);  return 1; }
static int32_t r_DestroyMirrorTexture2(void) { FN(DestroyMirrorTexture2); return KLOVRP_SUCCESS; }

static int32_t r_GetMotionVectorTiledMultiResLevel(int32_t *out) {
    FN(GetMotionVectorTiledMultiResLevel);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0;                               // foveation off by rule, whatever the guest set
    return KLOVRP_SUCCESS;
}
static int32_t r_SetMotionVectorTiledMultiResLevel(int lvl) {
    FN(SetMotionVectorTiledMultiResLevel);
    __atomic_store_n(&g.mv_tiled_level, lvl, __ATOMIC_RELAXED);   // recorded, no effect
    return KLOVRP_SUCCESS;
}

// ===========================================================================
// 3. ASW / Application SpaceWarp: always off
// ===========================================================================
static int32_t asw_flag(const char *name, int32_t *out) {
    kl_ovrp_hit(name);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetASWEnable(int32_t *o)         { return asw_flag("ovrp_GetASWEnable", o); }
static int32_t r_GetASWAdaptiveMode(int32_t *o)   { return asw_flag("ovrp_GetASWAdaptiveMode", o); }
static int32_t r_IsRequestingASWData(int32_t *o)  { return asw_flag("ovrp_IsRequestingASWData", o); }
static int32_t r_SetASWEnable(void)               { FN(SetASWEnable);       return KLOVRP_SUCCESS; }
static int32_t r_SetASWAdaptiveMode(void)         { FN(SetASWAdaptiveMode); return KLOVRP_SUCCESS; }
static int32_t asw_scale(const char *name, float *out) {
    kl_ovrp_hit(name);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 1.0f;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetASWVelocityScale(float *o) { return asw_scale("ovrp_GetASWVelocityScale", o); }
static int32_t r_GetASWDepthScale(float *o)    { return asw_scale("ovrp_GetASWDepthScale", o); }

// ===========================================================================
// 4. Performance, battery, system
// ===========================================================================
// Same refusal as ovrp_GetGPUUtilLevel2 / ovrp_GetGPUUtilSupported (no GPU
// utilisation is measured); the out is zeroed first.
static int32_t r_GetGPUUtilLevel(float *out) {
    FN(GetGPUUtilLevel);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0.0f;
    return KLOVRP_UNSUPPORTED;
}
static int32_t r_ResetAppPerfStats2(void) { FN(ResetAppPerfStats2); return KLOVRP_SUCCESS; }   // no counters of ours to reset

// ovrpBatteryStatus: Charging 0, Discharging 1, Full 2, NotCharging 3 - the same
// mapping as ovrp_GetSystemBatteryStatus2 (kl_ovrp.c). Only the first 4 bytes of
// the guest's struct are written, which fits any layout.
static int32_t r_GetBatteryStatus(int32_t *out) {
    FN(GetBatteryStatus);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = !kl_ovrp_battery_charging() ? 1 : (kl_ovrp_battery_level() >= 100 ? 2 : 0);
    return KLOVRP_SUCCESS;
}
// ovrp_GetCaps() is `ovrpBool f(void)` that returns constant 0 in every build (a
// deprecated stub); ovrp_GetCaps2(mask) takes the mask BY VALUE and answers which
// of those bits are set (the real build tests each requested bit and ORs it in).
static ovrpBool r_GetCaps(void) { FN(GetCaps); return 0; }
static int32_t r_GetCaps2(uint32_t mask) {
    FN(GetCaps2);
    return (int32_t)(mask & __atomic_load_n(&g.caps, __ATOMIC_RELAXED));
}
// ovrpBool ovrp_SetCaps(caps): constant 1 in the real build.
static ovrpBool r_SetCaps(uint32_t caps) {
    FN(SetCaps);
    __atomic_store_n(&g.caps, caps, __ATOMIC_RELAXED);
    return KLOVRP_TRUE;
}
static int32_t r_SetSystemVSyncCount(void)  { FN(SetSystemVSyncCount);  return KLOVRP_SUCCESS; }   // visionOS fixes vsync
static int32_t r_SetSystemVSyncCount2(void) { FN(SetSystemVSyncCount2); return KLOVRP_SUCCESS; }
static int32_t r_BeginProfilingRegion(void) { FN(BeginProfilingRegion); return KLOVRP_SUCCESS; }
static int32_t r_EndProfilingRegion(void)   { FN(EndProfilingRegion);   return KLOVRP_SUCCESS; }

// ovrp_GetStatus() is `ovrpBool f(void)`, constant 0 in the real build (a
// deprecated stub). ovrp_GetStatus2(mask) takes a bit mask BY VALUE and answers
// which of the requested status bits hold; bit 0 = running, which is always true
// while the app renders (it is always the foreground app).
static ovrpBool r_GetStatus(void) { FN(GetStatus); return 0; }
static int32_t r_GetStatus2(uint32_t mask) {
    FN(GetStatus2);
    return (int32_t)(mask & 1u);
}
// The key enum is not known here, so no key has a string; the out is an empty
// string (never NULL) so a caller that ignores the result reads "".
static int32_t r_GetString(int key, const char **out) {
    FN(GetString);
    (void)key;
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = "";
    return KLOVRP_INVALID_PARAM;
}
// float ovrp_GetFloat(int key): the VALUE comes back in s0 (the real build zeroes
// d0 up front and returns `ldr s0, [..]` / `scvtf s0, w0` / a tail call, there is
// no out pointer). An unknown key is 0.0. ovrpBool ovrp_SetFloat(int key, float).
static float r_GetFloat(int key) {
    FN(GetFloat);
    int s = slot_find(g.float_key, key, 0);
    return s < 0 ? 0.0f : f_get(&g.float_val[s]);
}
static ovrpBool r_SetFloat(int key, float v) {
    FN(SetFloat);
    int s = slot_find(g.float_key, key, 1);
    if (s >= 0) f_set(&g.float_val[s], v);
    return KLOVRP_TRUE;
}
static int32_t r_GetVrApiPropertyInt(int prop, int32_t *out) {
    FN(GetVrApiPropertyInt);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    int s = slot_find(g.vr_key, prop, 0);
    *out = s >= 0 ? __atomic_load_n(&g.vr_val[s], __ATOMIC_RELAXED) : 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_SetVrApiPropertyInt(int prop, int v) {
    FN(SetVrApiPropertyInt);
    int s = slot_find(g.vr_key, prop, 1);
    if (s >= 0) __atomic_store_n(&g.vr_val[s], v, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
static int32_t r_SetVrApiPropertyFloat(void) { FN(SetVrApiPropertyFloat); return KLOVRP_SUCCESS; }

static int32_t out_zero_i32(const char *name, int32_t *out) {
    kl_ovrp_hit(name);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetSystemRegion(int32_t *o)  { return out_zero_i32("ovrp_GetSystemRegion", o); }     // Unspecified
static int32_t r_GetSystemRegion2(int32_t *o) {
    FN(GetSystemRegion2);
    if (!pok(o)) return KLOVRP_INVALID_PARAM;
    *o = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetSystemHmd3DofModeEnabled(int32_t *o) {
    FN(GetSystemHmd3DofModeEnabled);
    if (!pok(o)) return KLOVRP_INVALID_PARAM;
    *o = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetLocalDimmingSupported(int32_t *o) {
    FN(GetLocalDimmingSupported);
    if (!pok(o)) return KLOVRP_INVALID_PARAM;
    *o = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_IsConsentSettingsChangeEnabled(int32_t *o) { return out_zero_i32("ovrp_IsConsentSettingsChangeEnabled", o); }
static int32_t r_GetTrackingPoseEnabledForInvisibleSession(int32_t *out) {
    FN(GetTrackingPoseEnabledForInvisibleSession);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = __atomic_load_n(&g.invisible_pose, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
static int32_t r_SetTrackingPoseEnabledForInvisibleSession(int v) {
    FN(SetTrackingPoseEnabledForInvisibleSession);
    __atomic_store_n(&g.invisible_pose, !!v, __ATOMIC_RELAXED);
    return KLOVRP_SUCCESS;
}
// Pure notifications / UI: there is no system UI to show or inhibit, and nothing
// else in this runtime reads these back.
static int32_t r_SetDeveloperModeStrict(void) { FN(SetDeveloperModeStrict); return KLOVRP_SUCCESS; }
static int32_t r_SetInhibitSystemUX(void)     { FN(SetInhibitSystemUX);     return KLOVRP_SUCCESS; }
static int32_t r_SetInhibitSystemUX2(void)    { FN(SetInhibitSystemUX2);    return KLOVRP_SUCCESS; }
static int32_t r_ShowSystemUI(void)           { FN(ShowSystemUI);           return KLOVRP_SUCCESS; }
static int32_t r_ShowSystemUI2(void)          { FN(ShowSystemUI2);          return KLOVRP_SUCCESS; }
static ovrpBool r_ShowUI(void)                { FN(ShowUI);                 return 0; }   // ovrpBool: no UI shown
static int32_t r_DismissHSW(void)             { FN(DismissHSW);             return KLOVRP_SUCCESS; }
// ovrpBool, and the real build returns "is the plugin initialised" (cset on
// ovrp_GetInitialized) whatever the argument; focus is always the app's.
static ovrpBool r_SetAppIgnoreVrFocus(void)   { FN(SetAppIgnoreVrFocus);    return kl_ovrp_is_initialized() ? KLOVRP_TRUE : 0; }
static int32_t r_OnEditorShutdown(void)       { FN(OnEditorShutdown);       return KLOVRP_SUCCESS; }
static int32_t r_SetLogCallback2(void *cb) {
    FN(SetLogCallback2);
    __atomic_store_n(&g.log_cb, cb, __ATOMIC_RELAXED);   // kept, never called: our log goes to stderr
    return KLOVRP_SUCCESS;
}

// Native handles. Only the OpenXR proc-addr getter has something real to give
// (kl_openxr's own xrGetInstanceProcAddr); the rest is "no native object here".
static int32_t r_GetNativeOpenXRHandles(void *inst, void *sess, void *sys) {
    FN(GetNativeOpenXRHandles);
    if (pok(inst)) *(uint64_t *)inst = 0;
    if (pok(sess)) *(uint64_t *)sess = 0;
    if (pok(sys))  *(uint64_t *)sys = 0;
    return KLOVRP_UNSUPPORTED;              // handles exist only inside an OpenXR guest's own session
}
// Pointer outputs: NULL and 0, never written - the guest pre-zeroes the slot (see
// ovrp_GetNativeSDKPointer2) and a stale x0 must not be stored through.
static int32_t r_GetNativePointer(void)    { FN(GetNativePointer);    return KLOVRP_SUCCESS; }
// ovrpResult ovrp_GetUnityInterfaces(void **out): 8 bytes through x0, -1001 on NULL.
// No IUnityInterfaces registry is kept here: NULL is the honest value.
static ovrpResult r_GetUnityInterfaces(void **out) {
    FN(GetUnityInterfaces);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = NULL;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetOpenXRInstanceProcAddrFunc(void **out) {
    FN(GetOpenXRInstanceProcAddrFunc);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = kl_openxr_sym("xrGetInstanceProcAddr");
    return KLOVRP_SUCCESS;
}
// ovrpResult ovrp_GetRuntimeName(const char **out): the real build writes ONE
// pointer (8 bytes) through x0, not characters into a caller buffer.
static ovrpResult r_GetRuntimeName(const char **out) {
    FN(GetRuntimeName);
    static const char name[] = "Klepton OpenXR runtime";
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = name;
    return KLOVRP_SUCCESS;
}
// The buffer size is not an argument, so no string can be written safely. Refused
// (-1004): the machine id is optional telemetry for the guest.
// (see k_refused)

static int32_t r_GetAdapterId(int32_t *out) {
    FN(GetAdapterId);
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0;                               // no adapter id on a Quest either (see GetDisplayAdapterId2)
    return KLOVRP_SUCCESS;
}
// No Android audio device: audio is CoreAudio. Zeroed 32 bytes read as an empty
// wide string / null GUID (the shape of ovrp_GetAudioOutId2).
static int32_t device_id(const char *name, const void **out) {
    kl_ovrp_hit(name);
    static const uint8_t empty_id[32];
    if (pok(out)) *out = empty_id;
    return KLOVRP_SUCCESS;
}
static int32_t r_GetAudioInDeviceId(const void **o)   { return device_id("ovrp_GetAudioInDeviceId", o); }
static int32_t r_GetAudioInDeviceId2(const void **o)  { return device_id("ovrp_GetAudioInDeviceId2", o); }
static int32_t r_GetAudioOutDeviceId(const void **o)  { return device_id("ovrp_GetAudioOutDeviceId", o); }
static int32_t r_GetAudioOutDeviceId2(const void **o) { return device_id("ovrp_GetAudioOutDeviceId2", o); }
// v1 Bool form of ovrp_GetSystemHeadphonesPresent (kl_ovrp.c: "no"): no headphone route is tracked.
static int32_t r_GetHeadphonesPresent(void) { FN(GetHeadphonesPresent); return 0; }
static int32_t r_GetSystemHeadphonesPresent2(int32_t *o) { return out_zero_i32("ovrp_GetSystemHeadphonesPresent2", o); }

// Tracking origin. The floor/eye origin is the display's and never moves; the
// calibrated origin is the identity, exactly what ovrp_GetTrackingCalibratedOrigin2
// answers (kl_ovrp.c). A set is accepted and dropped so the two can never disagree.
// The real ovrp_GetTrackingCalibratedOrigin() returns a 28-byte ovrpPosef through
// x8 (kl_ovrp_sret2.S moves it into `out`).
void klovrp_GetTrackingCalibratedOrigin_impl(void *out) {
    FN(GetTrackingCalibratedOrigin);
    ovrpPosef p = { {0, 0, 0, 1}, {0, 0, 0} };
    memcpy(out, &p, 28);
}
static int32_t r_SetTrackingCalibratedOrigin2(void) { FN(SetTrackingCalibratedOrigin2); return KLOVRP_SUCCESS; }
static int32_t r_RecenterPose(void) {
    FN(RecenterPose);
    uint64_t (*f)(int) = sib("ovrp_RecenterTrackingOrigin2");
    if (f) (void)f(0);
    return 1;
}
static int32_t r_SetNodePositionTracked(void)  { FN(SetNodePositionTracked);  return KLOVRP_SUCCESS; }
static int32_t r_SetNodePositionTracked2(void) { FN(SetNodePositionTracked2); return KLOVRP_SUCCESS; }

// Feature fidelity: every feature reports off.
static int32_t r_FeatureFidelityGetFeatureState(int feature, int32_t *out) {
    FN(FeatureFidelityGetFeatureState);
    (void)feature;
    if (!pok(out)) return KLOVRP_INVALID_PARAM;
    *out = 0;
    return KLOVRP_SUCCESS;
}
static int32_t r_FeatureFidelitySetFeatureEnable(void)   { FN(FeatureFidelitySetFeatureEnable);   return KLOVRP_SUCCESS; }
static int32_t r_FeatureFidelitySetFeatureFidelity(void) { FN(FeatureFidelitySetFeatureFidelity); return KLOVRP_SUCCESS; }

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------
#define E(n) { "ovrp_" #n, (void *)r_##n }
// sret entry points (listed by hand below): kl_ovrp_sret2.S samples x8 and
// tail-calls klovrp_<n>_impl.
void klovrp_GetEyePose_entry(void);
void klovrp_GetEyeVelocity_entry(void);
void klovrp_GetEyeAcceleration_entry(void);
void klovrp_GetTrackingCalibratedOrigin_entry(void);
static const kl_ovrp_entry k_tab[] = {
    // 1. legacy spellings
    E(Initialize), E(Initialize2), E(Initialize3), E(Initialize4), E(PreInitialize2),
    E(BeginFrame2), E(BeginFrame3), E(EndFrame2), E(EndFrame3), E(EndEye),
    E(EnqueueSetupLayer), E(EnqueueSubmitLayer), E(CalculateEyeLayerDesc), E(Update),
    E(SetupDistortionWindow2), E(SetupEyeTexture), E(SetOverlayQuad), E(SetOverlayQuad2),
    E(GetLayerTexture), E(GetAdaptiveGpuPerformanceScale), E(GetAppCpuStartToGpuEndTime),
    E(GetAppLatencyTimings), E(SetAppCPUPriority), E(GetAppCPUPriority), E(GetAppCPUPriority2),
    E(GetNativeSDKPointer), E(SetUserNeckEyeDistance), E(GetUserNeckEyeDistance),
    // 2. per-eye render state
    E(GetEyeTextureScale), E(GetEyeViewportScale), E(GetEyeRecommendedResolutionScale),
    E(GetEyeTextureFlippedY), E(GetEyeTextureArrayEnabled), E(GetEyeTextureShared),
    E(SetEyeTextureShared), E(GetEyeOcclusionMeshEnabled), E(GetEyeFovPremultipliedAlphaMode),
    E(GetEyeFovLayerId), E(GetEyePixelsPerTanAngleAtCenter), E(GetEyePixelsPerTanAngleAtCenter2),
    E(GetEyeFrustum), E(GetHmdToEyeOffset), E(GetHmdToEyeOffset2),
    {"ovrp_GetEyePose", (void *)klovrp_GetEyePose_entry},
    {"ovrp_GetEyeVelocity", (void *)klovrp_GetEyeVelocity_entry},
    {"ovrp_GetEyeAcceleration", (void *)klovrp_GetEyeAcceleration_entry},
    E(GetUserNeckEyeDistance2), E(GetUserEyeHeight), E(SetUserEyeHeight),
    E(SetUserEyeHeight2), E(GetUserEyeDepth), E(SetUserEyeDepth), E(SetUserIPD), E(SetUserIPD2),
    E(GetAppSRGB), E(SetAppSRGB), E(SetAppMonoscopic), E(SetEyeTexture), E(ReleaseEyeTexture),
    E(RecreateEyeTexture), E(GetEyeLayerRecommendedResolution), E(SetEyeBufferSecure),
    E(GetBufferCount), E(SetDepthCompositingInfo), E(SetupLayerDepth), E(SetOctilinearInfo),
    E(SetExternalLayerDynresEnabled), E(UpdateFoveation), E(GetFoveationEyeTrackedCenter),
    E(SetupMirrorTexture), E(SetupMirrorTexture2), E(DestroyMirrorTexture), E(DestroyMirrorTexture2),
    E(GetMotionVectorTiledMultiResLevel), E(SetMotionVectorTiledMultiResLevel),
    // 3. ASW
    E(GetASWEnable), E(SetASWEnable), E(GetASWAdaptiveMode), E(SetASWAdaptiveMode),
    E(GetASWVelocityScale), E(GetASWDepthScale), E(IsRequestingASWData),
    // 4. performance / system
    E(GetGPUUtilLevel), E(ResetAppPerfStats2), E(GetBatteryStatus), E(GetCaps), E(GetCaps2),
    E(SetCaps), E(SetSystemVSyncCount), E(SetSystemVSyncCount2), E(BeginProfilingRegion),
    E(EndProfilingRegion), E(GetStatus), E(GetStatus2), E(GetString), E(GetFloat), E(SetFloat),
    E(GetVrApiPropertyInt), E(SetVrApiPropertyInt), E(SetVrApiPropertyFloat),
    E(GetSystemRegion), E(GetSystemRegion2), E(GetSystemHmd3DofModeEnabled),
    E(SetDeveloperModeStrict), E(SetInhibitSystemUX), E(SetInhibitSystemUX2), E(ShowSystemUI),
    E(ShowSystemUI2), E(ShowUI), E(DismissHSW), E(SetAppIgnoreVrFocus), E(OnEditorShutdown),
    E(SetLogCallback2), E(GetNativeOpenXRHandles), E(GetNativePointer),
    E(GetOpenXRInstanceProcAddrFunc), E(GetUnityInterfaces), E(GetRuntimeName), E(GetAdapterId),
    E(GetHeadphonesPresent), E(GetSystemHeadphonesPresent2), E(GetAudioInDeviceId),
    E(GetAudioInDeviceId2), E(GetAudioOutDeviceId), E(GetAudioOutDeviceId2),
    E(GetLocalDimmingSupported), E(SetTrackingCalibratedOrigin2),
    {"ovrp_GetTrackingCalibratedOrigin", (void *)klovrp_GetTrackingCalibratedOrigin_entry},
    E(RecenterPose), E(FeatureFidelityGetFeatureState), E(FeatureFidelitySetFeatureEnable),
    E(FeatureFidelitySetFeatureFidelity), E(IsConsentSettingsChangeEnabled),
    E(GetTrackingPoseEnabledForInvisibleSession), E(SetTrackingPoseEnabledForInvisibleSession),
    E(SetNodePositionTracked), E(SetNodePositionTracked2),
};

// Capabilities this host does not have, answered -1004 by name with nothing
// written (an out-parameter's position is not known for any of them):
//   - Application SpaceWarp textures: ASW is off by rule, and a refused
//     GetLayerTextureSpaceWarp makes the provider turn SpaceWarp off and go on.
//   - external-swapchain acquire and the occlusion mesh: no such object.
//   - MachineID: the guest's buffer size is not an argument.
static const char *const k_refused[] = {
    "ovrp_GetLayerMotionVectorTextures", "ovrp_GetLayerTextureSpaceWarp2",
    "ovrp_AcquireLayerSwapchain", "ovrp_GetLayerOcclusionMesh", "ovrp_GetMachineID",
    NULL,
};

void kl_ovrp_render_register(void) {
    kl_ovrp_register(k_tab, sizeof k_tab / sizeof k_tab[0], k_refused);
}
