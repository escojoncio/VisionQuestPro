// kl_ovrp_input.c - Lote 2: ovrp entrada / seguimiento / manos / boundary.
//
// Registered with kl_ovrp.c through kl_ovrp_input_register (see kl_ovrp_priv.h).
//
// What lives here:
//   * the hand-tracking state (ovrp_GetHandState / 2 / 3 / GetHandStateAtTime), the
//     hand skeleton (ovrp_GetSkeleton / 2 / 3) and hand mesh (ovrp_GetMesh): the
//     26 ARKit joints KleptonControllers.swift publishes through
//     kl_ovrp_set_hand_joints, turned into an ovrpHandState;
//   * the legacy (ABI 1.x) node queries, as thin readers over the one pose
//     path kl_ovrp.c already has, so no version can contradict another;
//   * the controller / action / haptics shapes this host does not have;
//   * face, body and eye tracking: visionOS gives apps none of the three, so
//     each is refused by name (-1004) and nothing is written;
//   * the v1 boundary geometry, forwarded to the v2 answer.
//
// ABI rules followed throughout (read the long comments in kl_ovrp.c before
// changing one):
//   - A refusal never writes an out-struct. Several of these structs have no
//     layout we can check (ovrpFaceState, ovrpBodyState, ...), and zeroing a
//     guessed size is a stack smash; the managed wrapper tests the result.
//   - A function whose shape (Bool vs Result, by value vs out pointer) is a
//     reconstruction is either answered with a value that reads the same under
//     both shapes (0 = false = success with a zeroed out) or refused.
//   - Structs returned through x8 (the pose / velocity / acceleration queries,
//     28 bytes ovrpPosef-shaped in every OVRPlugin build read) go through
//     kl_ovrp_sret2.S, which moves x8 into the first free argument register
//     before anything can clobber it, then a plain C *_impl. HFA returns (the v0
//     frustums, s0..s3) are plain C returns.
#include "kl_ovrp_priv.h"
#include "kl_ovrp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Non-static in kl_ovrp.c, not in any header.
uint64_t klovrp_GetNodePoseState_impl(int step, int node, void *out);
uint64_t klovrp_GetControllerState2_impl(int mask, void *out);
// x8-capturing entry points (kl_ovrp_sret2.S) and the C bodies they branch to.
void klovrp_GetNodePose_entry(void);
void klovrp_GetNodeVelocity_entry(void);
void klovrp_GetNodeAcceleration_entry(void);
void klovrp_GetNodePose2_entry(void);
void klovrp_GetNodeVelocity2_entry(void);
void klovrp_GetNodeAcceleration2_entry(void);
void klovrp_GetTrackerPose_entry(void);

// ===========================================================================
// Hand joints: the seam from KleptonControllers.swift
// ===========================================================================
#define HJ_N 26

typedef struct {
    float    pose[HJ_N][7];    // px py pz qx qy qz qw, ARKit origin space
    uint32_t tracked;          // bit i = joint i tracked (bit 0 = wrist)
    float    pinch[5];         // thumb, index, middle, ring, little
    uint32_t pinch_bits;       // 1 << finger
    uint32_t valid;            // this hand is tracked right now
} hj_data;

// Seqlock: one writer (the frame loop), any number of guest readers.
static struct { uint32_t seq; hj_data d; } g_hj[2];
static int g_hj_supported;     // the frontend has a hand-tracking provider

// Joint order is KleptonControllers.handJointOrder: 0 wrist, 1 forearm,
// 2..5 thumb, 6..10 index, 11..15 middle, 16..20 ring, 21..25 little, each
// finger = metacarpal, knuckle, intermediate base, intermediate tip, tip
// (the thumb has no metacarpal).
void kl_ovrp_set_hand_joints(int hand, const float *pose7x26, uint32_t tracked_mask,
                             const float *pinch5, uint32_t pinch_bits) {
    if (hand < 0 || hand > 1) return;
    __atomic_store_n(&g_hj_supported, 1, __ATOMIC_RELAXED);
    hj_data n;
    memset(&n, 0, sizeof n);
    if (pose7x26) {
        memcpy(n.pose, pose7x26, sizeof n.pose);
        n.tracked = tracked_mask & ((1u << HJ_N) - 1u);
        n.valid = (n.tracked & 1u) ? 1u : 0u;
        for (int i = 0; i < HJ_N && n.valid; i++)
            for (int k = 0; k < 7; k++)
                if (!isfinite(n.pose[i][k])) { n.valid = 0; break; }
        if (pinch5) memcpy(n.pinch, pinch5, sizeof n.pinch);
        n.pinch_bits = pinch_bits & 0x1Fu;
    }
    uint32_t s = g_hj[hand].seq;
    __atomic_store_n(&g_hj[hand].seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(&g_hj[hand].d, &n, sizeof n);
    __atomic_store_n(&g_hj[hand].seq, s + 2, __ATOMIC_RELEASE);
}

static int hj_read(int hand, hj_data *out) {
    for (int i = 0; i < 16; i++) {
        uint32_t a = __atomic_load_n(&g_hj[hand].seq, __ATOMIC_ACQUIRE);
        if (a & 1u) continue;
        memcpy(out, &g_hj[hand].d, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_hj[hand].seq, __ATOMIC_RELAXED) == a) return 1;
    }
    memset(out, 0, sizeof *out);
    return 0;
}

// ---- small quaternion helpers (x y z w) ----
static void q_mul(const float a[4], const float b[4], float r[4]) {
    r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    r[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    r[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void q_norm(float q[4]) {
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (!(n > 1e-8f)) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    float inv = 1.0f / sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] *= inv;
}
static void q_rot(const float q[4], const float v[3], float o[3]) {
    float tx = 2.0f * (q[1]*v[2] - q[2]*v[1]);
    float ty = 2.0f * (q[2]*v[0] - q[0]*v[2]);
    float tz = 2.0f * (q[0]*v[1] - q[1]*v[0]);
    o[0] = v[0] + q[3]*tx + (q[1]*tz - q[2]*ty);
    o[1] = v[1] + q[3]*ty + (q[2]*tx - q[0]*tz);
    o[2] = v[2] + q[3]*tz + (q[0]*ty - q[1]*tx);
}
// Rotation with columns (x, y, z) -> quaternion.
static void q_from_axes(const float x[3], const float y[3], const float z[3], float q[4]) {
    float m00 = x[0], m10 = x[1], m20 = x[2];
    float m01 = y[0], m11 = y[1], m21 = y[2];
    float m02 = z[0], m12 = z[1], m22 = z[2];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
    q_norm(q);
}

// ---- ovrpHandState (512 bytes) ----------------------------------------------
//   +0   i32  Status               HandTracked 1, InputStateValid 2
//   +4   Posef RootPose            quat xyzw, then position   (28)
//   +32  Quatf BoneRotations[24]   local to the parent bone   (384)
//   +416 i32  Pinches              1 << finger (thumb 0 ... little 4)
//   +420 f32  PinchStrength[5]
//   +440 Posef PointerPose
//   +468 f32  HandScale
//   +472 i32  HandConfidence       Low 0 / High 0x40000000
//   +476 i32  FingerConfidences[5]
//   +496 f64  RequestedTimeStamp
//   +504 f64  SampleTimeStamp
#define HS_SIZE        512
#define HS_STATUS_TRACKED   0x1
#define HS_STATUS_INPUT     0x2
#define HS_CONF_HIGH   0x40000000

// OVR bone -> published joint, and parent bone (-1 for the root). Bone ids:
// 0 WristRoot 1 ForearmStub 2-5 Thumb0..3 6-8 Index1..3 9-11 Middle1..3
// 12-14 Ring1..3 15-18 Pinky0..3 19-23 Thumb/Index/Middle/Ring/Pinky tip.
static const struct { int8_t joint, parent; } k_bone[24] = {
    { 0, -1}, { 1, 0},
    { 2, 0}, { 3, 2}, { 4, 3}, { 5, 4},          // thumb: knuckle, int base, int tip, tip
    { 7, 0}, { 8, 6}, { 9, 7},                   // index: knuckle, int base, int tip
    {12, 0}, {13, 9}, {14, 10},                  // middle
    {17, 0}, {18, 12}, {19, 13},                 // ring
    {21, 0}, {22, 15}, {23, 16}, {24, 17},       // little: metacarpal first
    { 5, 5}, {10, 8}, {15, 11}, {20, 14}, {25, 18},   // tips
};
// Joints whose tracking decides each finger's confidence.
static const int8_t k_finger_joints[5][4] = {
    { 2,  3,  4,  5}, { 7,  8,  9, 10}, {12, 13, 14, 15}, {17, 18, 19, 20}, {22, 23, 24, 25},
};

static double hand_now(void) {
    double t = kl_ovrp_predicted_display_time();
    if (!isfinite(t) || t <= 0.0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    }
    return t;
}

static void put_f(uint8_t *out, size_t off, const float *v, size_t n) { memcpy(out + off, v, n * sizeof(float)); }
static void put_i(uint8_t *out, size_t off, int32_t v) { memcpy(out + off, &v, 4); }
static void put_d(uint8_t *out, size_t off, double v) { memcpy(out + off, &v, 8); }

// The pointer ray: origin at the thumb-index pinch point, aimed from an
// estimated shoulder (the head, 17 cm to the side, 17 cm down, turned with the
// head's yaw only) through it. That is what a Quest does and what keeps the ray
// steady while the wrist rolls.
static void hand_pointer(int hand, const float pinch_pt[3], float pose[7]) {
    kl_ovrp_pose7 h;
    kl_ovrp_guest_head(0, &h);
    float hq[4] = { h.qx, h.qy, h.qz, h.qw };
    float fwd[3], f0[3] = { 0.0f, 0.0f, -1.0f };
    q_rot(hq, f0, fwd);
    float yaw = atan2f(-fwd[0], -fwd[2]);
    float c = cosf(yaw), s = sinf(yaw);
    float sx = hand ? 0.17f : -0.17f, sy = -0.17f;
    float sh[3] = { h.px + c * sx, h.py + sy, h.pz - s * sx };
    float d[3] = { pinch_pt[0] - sh[0], pinch_pt[1] - sh[1], pinch_pt[2] - sh[2] };
    float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    float q[4] = { 0, 0, 0, 1 };
    if (len > 1e-4f) {
        float z[3] = { -d[0] / len, -d[1] / len, -d[2] / len };          // -Z = ray direction
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float x[3] = { up[1]*z[2] - up[2]*z[1], up[2]*z[0] - up[0]*z[2], up[0]*z[1] - up[1]*z[0] };
        float xl = sqrtf(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
        if (xl > 1e-4f) {
            x[0] /= xl; x[1] /= xl; x[2] /= xl;
            float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
            q_from_axes(x, y, z, q);
        }
    }
    pose[0] = q[0]; pose[1] = q[1]; pose[2] = q[2]; pose[3] = q[3];
    pose[4] = pinch_pt[0]; pose[5] = pinch_pt[1]; pose[6] = pinch_pt[2];
}

// Fills the 512 bytes. Returns 1 when the hand is tracked.
static int hand_fill(int hand, double requested, uint8_t *out) {
    memset(out, 0, HS_SIZE);
    static const float ident[7] = { 0, 0, 0, 1, 0, 0, 0 };
    put_f(out, 4, ident, 7);
    for (int i = 0; i < 24; i++) put_f(out, 32 + (size_t)i * 16, ident, 4);
    put_f(out, 440, ident, 7);
    float one = 1.0f;
    put_f(out, 468, &one, 1);
    double now = hand_now();
    put_d(out, 496, requested > 0.0 ? requested : now);
    put_d(out, 504, now);

    hj_data d;
    if (!hj_read(hand, &d) || !d.valid) return 0;

    // Everything in the guest's tracking space: ARKit origin space with the
    // origin's height removed, exactly as kl_ovrp.c does for every pose.
    float oy = klovrp_guest_origin_y();
    float wq[HJ_N][4], wp[HJ_N][3];
    for (int i = 0; i < HJ_N; i++) {
        wp[i][0] = d.pose[i][0]; wp[i][1] = d.pose[i][1] - oy; wp[i][2] = d.pose[i][2];
        wq[i][0] = d.pose[i][3]; wq[i][1] = d.pose[i][4]; wq[i][2] = d.pose[i][5]; wq[i][3] = d.pose[i][6];
        q_norm(wq[i]);
    }
    // Root: the wrist, in the frame ARKit gives it. The bones below are local
    // to their parents IN ARKit's joint frames: a skeleton served alongside
    // (ovrp_GetSkeleton2) has to express its bind pose in the same frames.
    float root[7] = { wq[0][0], wq[0][1], wq[0][2], wq[0][3], wp[0][0], wp[0][1], wp[0][2] };
    put_f(out, 4, root, 7);
    for (int b = 1; b < 24; b++) {
        int j = k_bone[b].joint, pj = k_bone[k_bone[b].parent].joint;
        float inv[4] = { -wq[pj][0], -wq[pj][1], -wq[pj][2], wq[pj][3] };   // parent^-1
        float r[4];
        q_mul(inv, wq[j], r);
        q_norm(r);
        put_f(out, 32 + (size_t)b * 16, r, 4);
    }

    put_i(out, 416, (int32_t)(d.pinch_bits & 0x1Eu));
    float pinch[5] = { 0, d.pinch[1], d.pinch[2], d.pinch[3], d.pinch[4] };
    for (int i = 1; i < 5; i++) pinch[i] = pinch[i] < 0 ? 0 : (pinch[i] > 1 ? 1 : pinch[i]);
    put_f(out, 420, pinch, 5);

    float pp[3] = { (wp[5][0] + wp[10][0]) * 0.5f, (wp[5][1] + wp[10][1]) * 0.5f,
                    (wp[5][2] + wp[10][2]) * 0.5f };          // thumb tip / index tip
    float ptr[7];
    hand_pointer(hand, pp, ptr);
    put_f(out, 440, ptr, 7);

    unsigned n_tracked = (unsigned)__builtin_popcount(d.tracked);
    put_i(out, 472, (n_tracked >= HJ_N / 2) ? HS_CONF_HIGH : 0);
    for (int f = 0; f < 5; f++) {
        int ok = 1;
        for (int k = 0; k < 4; k++) if (!(d.tracked & (1u << k_finger_joints[f][k]))) ok = 0;
        put_i(out, 476 + (size_t)f * 4, ok ? HS_CONF_HIGH : 0);
    }
    put_i(out, 0, HS_STATUS_TRACKED | HS_STATUS_INPUT);
    return 1;
}

static int hand_note_first(int hand, int tracked) {
    static int said[2][2];
    if (said[hand][tracked]) return 0;
    said[hand][tracked] = 1;
    fprintf(stderr, "  [ovrp] hand state: %s hand %s (ARKit HandTrackingProvider)\n",
            hand ? "right" : "left", tracked ? "tracked" : "not tracked");
    return 1;
}

static ovrpResult hand_state(const char *name, int hand, double requested, void *out) {
    kl_ovrp_hit(name);
    if (!out) return KLOVRP_INVALID_PARAM;
    if (hand < 0 || hand > 1) return KLOVRP_INVALID_PARAM;
    if (!__atomic_load_n(&g_hj_supported, __ATOMIC_RELAXED)) return KLOVRP_UNSUPPORTED;
    int tracked = hand_fill(hand, requested, (uint8_t *)out);
    hand_note_first(hand, tracked);
    return KLOVRP_SUCCESS;
}

// ovrpResult ovrp_GetHandState3(ovrpStep, int frameIndex, ovrpHand, ovrpHandState *)
// (and GetHandState2, the same four arguments). The wrapper in every
// libOVRPlugin forwards them as (this, step, frameIndex, hand, out) to the one
// implementation, which checks `hand <= 1` and memcpy()s 512 bytes into x4.
// The legacy ovrp_GetHandState(step, hand, out) is that same call with
// frameIndex = -1 ("latest"). frameIndex only selects a sample; the frontend
// already publishes one prediction per frame, so it is ignored.
static ovrpResult klovrp_GetHandState3(int step, int frame_index, int hand, void *out) {
    (void)step; (void)frame_index;
    return hand_state("ovrp_GetHandState3", hand, 0.0, out);
}
static ovrpResult klovrp_GetHandState2(int step, int frame_index, int hand, void *out) {
    (void)step; (void)frame_index;
    return hand_state("ovrp_GetHandState2", hand, 0.0, out);
}
static ovrpResult klovrp_GetHandState(int step, int hand, void *out) {
    (void)step;
    return hand_state("ovrp_GetHandState", hand, 0.0, out);
}
// ovrpResult ovrp_GetHandStateAtTime(double time, ovrpHand, ovrpHandState *)
// (a double and an int travel in different register files, so the order the
// two are written in cannot matter). The frontend already predicts the joints
// to the frame's presentation time, so the time only stamps the answer.
static ovrpResult klovrp_GetHandStateAtTime(double time, int hand, void *out) {
    return hand_state("ovrp_GetHandStateAtTime", hand, time, out);
}
// ovrpResult ovrp_GetHandTrackingEnabled(ovrpBool *out). The gate OVRHand
// reads before it ever asks for a state; true where the device has a hand
// provider (a hand out of view is then just "not tracked", as on a Quest).
// Not in the Lote 2 list, but nothing above is reachable without it.
static ovrpResult klovrp_GetHandTrackingEnabled(int32_t *out) {
    kl_ovrp_hit("ovrp_GetHandTrackingEnabled");
    if ((uintptr_t)out < 0x10000u) return KLOVRP_INVALID_PARAM;
    *out = __atomic_load_n(&g_hj_supported, __ATOMIC_RELAXED) ? 1 : 0;
    return KLOVRP_SUCCESS;
}

// ===========================================================================
// Legacy node queries (ABI 1.x): all read ovrp_GetNodePoseState's body
// ===========================================================================
// All three un-numbered queries, and the "2" spellings that take (step, node),
// return a 28-byte ovrpPosef-shaped struct THROUGH x8 in every build read
// (tools/ovrp_abi.py over nine OVRPlugin versions): the pose as is, and for
// velocity / acceleration an identity .Orientation with the vector in
// .Position. The ...PoseState2 form is ovrpResult f(step, node, ovrpPoseStatef *).
// The un-numbered ones read step 0 (render) as before; the "2" forms take the
// guest's step.
static void node_ret28(const char *name, int step, int node, int which, void *out) {
    kl_ovrp_hit(name);
    float f[0x58 / 4];
    klovrp_GetNodePoseState_impl(step, node, f);
    if (which == 0) {
        memcpy(out, f, 28);                 // quat xyzw at +0, position at +0x10
    } else {
        const float *v = f + (which == 1 ? 7 : 10);   // Velocity @28, Acceleration @40
        float r[7] = { 0, 0, 0, 1, v[0], v[1], v[2] };
        memcpy(out, r, sizeof r);
    }
}
void klovrp_GetNodePose_impl(int node, void *out)          { node_ret28("ovrp_GetNodePose", 0, node, 0, out); }
void klovrp_GetNodeVelocity_impl(int node, void *out)      { node_ret28("ovrp_GetNodeVelocity", 0, node, 1, out); }
void klovrp_GetNodeAcceleration_impl(int node, void *out)  { node_ret28("ovrp_GetNodeAcceleration", 0, node, 2, out); }
void klovrp_GetNodePose2_impl(int step, int node, void *out)         { node_ret28("ovrp_GetNodePose2", step, node, 0, out); }
void klovrp_GetNodeVelocity2_impl(int step, int node, void *out)     { node_ret28("ovrp_GetNodeVelocity2", step, node, 1, out); }
void klovrp_GetNodeAcceleration2_impl(int step, int node, void *out) { node_ret28("ovrp_GetNodeAcceleration2", step, node, 2, out); }

static ovrpResult klovrp_GetNodePoseState2(int step, int node, void *out) {
    kl_ovrp_hit("ovrp_GetNodePoseState2");
    if (!out) return KLOVRP_INVALID_PARAM;
    memset(out, 0, 0x58);                   // 88 bytes, the whole ovrpPoseStatef
    klovrp_GetNodePoseState_impl(step, node, out);
    return KLOVRP_SUCCESS;
}

// ovrpFrustumf (v0) = {zNear, zFar, FovX, FovY}, four floats: an HFA returned in
// s0..s3 (the real GetNodeFrustum / GetTrackerFrustum zero d0..d3 when
// uninitialised and otherwise hand back the callee's four floats). Not a Bool and
// not the 24-byte Frustum2f of the "2" spelling. FOV is in degrees: both tangent
// sides of GetNodeFrustum2's answer.
typedef struct {
    float zNear;
    float zFar;
    float fovX;
    float fovY;
} klin_frustum_v0;
typedef uint64_t (*node_frustum_fn)(int node, void *out);
static klin_frustum_v0 klovrp_GetNodeFrustum(int node) {
    kl_ovrp_hit("ovrp_GetNodeFrustum");
    static node_frustum_fn fn;
    klin_frustum_v0 r = { 0, 0, 0, 0 };
    if (!fn) fn = (node_frustum_fn)kl_ovrp_sym("ovrp_GetNodeFrustum2");
    float f[6];                              // near, far, up, down, left, right
    if (!fn || (int32_t)fn(node, f) < 0) return r;
    r.zNear = f[0];
    r.zFar = f[1];
    r.fovX = (atanf(f[4]) + atanf(f[5])) * (180.0f / 3.14159265f);
    r.fovY = (atanf(f[2]) + atanf(f[3])) * (180.0f / 3.14159265f);
    return r;
}

// No external tracking cameras: a zero frustum (the uninitialised answer of the
// real build) and an identity pose, written nowhere else.
static klin_frustum_v0 klovrp_GetTrackerFrustum(int tracker) {
    (void)tracker;
    kl_ovrp_hit("ovrp_GetTrackerFrustum");
    klin_frustum_v0 r = { 0, 0, 0, 0 };
    return r;
}
void klovrp_GetTrackerPose_impl(int tracker, void *out) {
    (void)tracker;
    kl_ovrp_hit("ovrp_GetTrackerPose");
    static const float ident[7] = { 0, 0, 0, 1, 0, 0, 0 };
    memcpy(out, ident, sizeof ident);
}

// ===========================================================================
// Controllers
// ===========================================================================
// ovrpResult ovrp_GetControllerState3(mask, ovrpControllerState3 *out): 64 bytes
// through x1 (RESULT{-1001,0}, 1.117), NOT a by-value return like State/State2.
// The v2 body writes exactly those 0x40 bytes.
static ovrpResult klovrp_GetControllerState3(unsigned mask, void *out) {
    kl_ovrp_hit("ovrp_GetControllerState3");
    if (!out) return KLOVRP_INVALID_PARAM;
    memset(out, 0, 0x40);
    klovrp_GetControllerState2_impl((int)mask, out);
    return KLOVRP_SUCCESS;
}

// The haptics shapes below have no layout to read. The buffered and PCM paths
// (SetControllerHaptics, SetControllerHapticsPcm) are the ones served.
static ovrpResult klovrp_SetControllerHaptics2(int controller, const void *state) {
    (void)controller; (void)state;
    return kl_ovrp_ok("ovrp_SetControllerHaptics2");
}
static ovrpResult klovrp_SetRemoteHandedness(int controller, int handedness) {
    (void)controller; (void)handedness;
    return kl_ovrp_ok("ovrp_SetRemoteHandedness");
}
// These all answer "false, nothing written": right as an ovrpBool and as an
// ovrpResult(ovrpBool *out) onto a zeroed out.
#define KLIN_FALSE(fn, nm)                                                    \
    static ovrpResult fn(void) { return kl_ovrp_ok(nm); }
KLIN_FALSE(klovrp_AreHandPosesGeneratedByControllerData, "ovrp_AreHandPosesGeneratedByControllerData")
KLIN_FALSE(klovrp_AreControllerDrivenHandPosesNatural, "ovrp_AreControllerDrivenHandPosesNatural")
KLIN_FALSE(klovrp_IsControllerDrivenHandPosesEnabled, "ovrp_IsControllerDrivenHandPosesEnabled")
KLIN_FALSE(klovrp_IsMultimodalHandsControllersSupported, "ovrp_IsMultimodalHandsControllersSupported")
KLIN_FALSE(klovrp_IsWideMotionModeHandPosesEnabled, "ovrp_IsWideMotionModeHandPosesEnabled")
KLIN_FALSE(klovrp_GetInputState, "ovrp_GetInputState")
KLIN_FALSE(klovrp_SetWideMotionModeHandPoses, "ovrp_SetWideMotionModeHandPoses")
KLIN_FALSE(klovrp_SendMicrogestureHint, "ovrp_SendMicrogestureHint")
#undef KLIN_FALSE


// ===========================================================================
// Hand skeleton (ovrp_GetSkeleton / 2 / 3) and hand mesh (ovrp_GetMesh)
// ===========================================================================
// Frame choice. GetHandState3 hands out BoneRotations that are local to the
// parent bone IN ARKit's joint frames (hand_fill). The bind pose is therefore
// expressed in those same frames, with every bind rotation = identity: a flat,
// open hand has (near) identity parent-to-child rotations in ARKit's skeleton,
// so a rest hand reads as the bind pose and a bone's runtime rotation is its
// delta from rest. The alternative (OVR's own canonical bone axes) would need
// a per-joint frame correction C_j in both the state and the bind pose, and
// ARKit's rest frames are not documented well enough to derive C_j without
// guessing; a wrong C_j is worse than none. Bone positions are the geometry
// of a generic adult hand along +X (fingers), +Y (palm normal), thumb on -Z for
// the right hand; the left hand mirrors Z. Mirrored identity rotations stay
// identity, so the left skeleton is exactly as coherent as the right.
//
// Layouts (verified against libOVRPlugin disassembly, all versions alike):
//   Bone         36 B: i32 Id, i16 ParentBoneIndex (-1 root), 2 pad, ovrpPosef Pose
//                (quat xyzw then position, LOCAL to the parent bone)
//   BoneCapsule  32 B: i16 BoneIndex, 2 pad, vec3 Start, vec3 End (bone-local), f32 Radius
//   ovrpSkeleton   1484 B: i32 Type, u32 NumBones, u32 NumBoneCapsules,
//                  Bones[24] @12, Capsules[19] @876          (hand only)
//   ovrpSkeleton2  3140 B: same header, Bones[70] @12, Capsules[19] @2532
//   ovrpSkeleton3  3644 B: same header, Bones[84] @12, Capsules[19] @3036
//   ovrpMesh     204012 B: i32 Type, u32 NumVertices, u32 NumIndices, then
//                  Positions vec3[3000] @12, Indices u16[18000] @36012,
//                  Normals vec3[3000] @72012, UV0 vec2[3000] @108012,
//                  BlendIndices i16[4][3000] @132012, BlendWeights f32[4][3000] @156012
// Skeleton types: 0 HandLeft, 1 HandRight, 2 Body, 3 FullBody, 4/5 XRHandLeft/Right.
// Body / FullBody are refused; so are the XR hands: their 26-bone rig has no
// counterpart in the 24-bone ovrpHandState this file serves.
#define SK_BONES      24
#define SK_CAPS_MAX   19
#define SK_BONE_SZ    36
#define SK_CAP_SZ     32

static const int8_t k_child[SK_BONES] = {            // the next bone along the chain, -1 none
    -1, -1, 3, 4, 5, 19, 7, 8, 20, 10, 11, 21, 13, 14, 22, 16, 17, 18, 23, -1, -1, -1, -1, -1,
};
// Rest offset of each bone from its parent, in the parent's frame (right hand, metres).
static const float k_rest[SK_BONES][3] = {
    {0, 0, 0},                   {-0.040f, 0, 0},
    {0.016f, -0.004f, -0.022f},  {0.030f, -0.002f, -0.014f}, {0.030f, 0, -0.008f}, {0.024f, 0, -0.004f},
    {0.092f, 0, -0.030f},        {0.040f, 0, 0},             {0.024f, 0, 0},
    {0.094f, 0, -0.010f},        {0.044f, 0, 0},             {0.027f, 0, 0},
    {0.090f, 0, 0.010f},         {0.041f, 0, 0},             {0.025f, 0, 0},
    {0.040f, 0, 0.028f},         {0.048f, 0, 0.006f},        {0.028f, 0, 0},   {0.018f, 0, 0},
    {0.022f, 0, -0.002f},        {0.019f, 0, 0},             {0.021f, 0, 0},   {0.020f, 0, 0}, {0.017f, 0, 0},
};
static const float k_radius[SK_BONES] = {            // tube radius of the segment bone -> child
    0, 0, 0.010f, 0.009f, 0.0085f, 0.0075f, 0.0085f, 0.0075f, 0.007f,
    0.0088f, 0.0078f, 0.0072f, 0.0082f, 0.0072f, 0.0068f, 0.0090f, 0.0075f, 0.0068f, 0.0062f,
    0, 0, 0, 0, 0,
};

static void put_i16(uint8_t *p, size_t off, int16_t v) { memcpy(p + off, &v, 2); }
static void put_u32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }

// Rest offset of bone b from its parent for `hand` (0 left mirrors Z).
static void rest_local(int hand, int b, float o[3]) {
    o[0] = k_rest[b][0]; o[1] = k_rest[b][1]; o[2] = hand ? k_rest[b][2] : -k_rest[b][2];
}
static void rest_world(int hand, float w[SK_BONES][3]) {
    for (int b = 0; b < SK_BONES; b++) {
        float o[3];
        rest_local(hand, b, o);
        int p = k_bone[b].parent;
        for (int k = 0; k < 3; k++) w[b][k] = o[k] + (p >= 0 ? w[p][k] : 0.0f);
    }
}

// Writes header, 24 bones and the capsules into a zeroed buffer whose capsule
// array starts at caps_off. Returns the number of capsules.
static unsigned skel_build(int type, uint8_t *buf, size_t caps_off) {
    int hand = type;                                  // 0 left, 1 right
    unsigned ncaps = 0;
    for (int b = 0; b < SK_BONES; b++) {
        uint8_t *p = buf + 12 + (size_t)b * SK_BONE_SZ;
        float o[3], pose[7] = { 0, 0, 0, 1, 0, 0, 0 };
        rest_local(hand, b, o);
        pose[4] = o[0]; pose[5] = o[1]; pose[6] = o[2];
        put_u32(p, 0, (uint32_t)b);
        put_i16(p, 4, (int16_t)k_bone[b].parent);
        put_f(p, 8, pose, 7);
        if (k_child[b] >= 0 && b >= 2 && ncaps < SK_CAPS_MAX) {
            float e[3];
            rest_local(hand, k_child[b], e);
            uint8_t *c = buf + caps_off + (size_t)ncaps * SK_CAP_SZ;
            float cap[7] = { 0, 0, 0, e[0], e[1], e[2], k_radius[b] };
            put_i16(c, 0, (int16_t)b);
            put_f(c, 4, cap, 7);
            ncaps++;
        }
    }
    put_u32(buf, 0, (uint32_t)type);
    put_u32(buf, 4, SK_BONES);
    put_u32(buf, 8, ncaps);
    return ncaps;
}

// size: bytes of the guest struct (all known); caps_off: where its capsule array starts.
// legacy: the real call rejects every type above HandRight with INVALID_PARAM.
static ovrpResult skel_get(const char *name, int type, void *out, size_t size, size_t caps_off, int legacy) {
    kl_ovrp_hit(name);
    if (type < 0 || type > 5 || !out || (legacy && type > 1)) return KLOVRP_INVALID_PARAM;
    if (type > 1) return KLOVRP_UNSUPPORTED;          // body, full body, XR hands
    memset(out, 0, size);
    skel_build(type, (uint8_t *)out, caps_off);
    return KLOVRP_SUCCESS;
}
// ovrpResult ovrp_GetSkeleton(ovrpSkeletonType, ovrpSkeleton *)   (hand types only)
static ovrpResult klovrp_GetSkeleton(int type, void *out) {
    return skel_get("ovrp_GetSkeleton", type, out, 12 + SK_BONES * SK_BONE_SZ + SK_CAPS_MAX * SK_CAP_SZ,
                    12 + SK_BONES * SK_BONE_SZ, 1);
}
static ovrpResult klovrp_GetSkeleton2(int type, void *out) {
    return skel_get("ovrp_GetSkeleton2", type, out, 3140, 12 + 70 * SK_BONE_SZ, 0);
}
static ovrpResult klovrp_GetSkeleton3(int type, void *out) {
    return skel_get("ovrp_GetSkeleton3", type, out, 3644, 12 + 84 * SK_BONE_SZ, 0);
}

// ---- hand mesh: a low-poly skinned hand built from the same rest geometry ----
// 17 six-sided tubes (one per bone segment) and a palm box. Weights only
// reference skinnable bones (0..18); a tube's far ring is shared 50/50 with
// the next bone for a softer knuckle, rigid on the fingertip.
#define MESH_MAX_V 3000
#define MESH_MAX_I 18000
#define MESH_SIDES 6
#define MESH_SZ    204012u
#define MESH_OFF_IDX 36012u
#define MESH_OFF_NRM 72012u
#define MESH_OFF_UV  108012u
#define MESH_OFF_BI  132012u
#define MESH_OFF_BW  156012u

typedef struct {
    float (*pos)[3], (*nrm)[3], (*uv)[2], (*bw)[4];
    int16_t (*bi)[4];
    uint16_t *idx;
    unsigned nv, ni;
    int flip;                                       // mirrored: reverse the winding
} mesh_w;

static unsigned mesh_vert(mesh_w *m, const float p[3], const float n[3], float u, float v,
                          int b0, float w0, int b1, float w1) {
    unsigned i = m->nv++;
    memcpy(m->pos[i], p, 12); memcpy(m->nrm[i], n, 12);
    m->uv[i][0] = u; m->uv[i][1] = v;
    m->bi[i][0] = (int16_t)b0; m->bi[i][1] = (int16_t)b1; m->bi[i][2] = 0; m->bi[i][3] = 0;
    m->bw[i][0] = w0; m->bw[i][1] = w1; m->bw[i][2] = 0; m->bw[i][3] = 0;
    return i;
}
static void mesh_tri(mesh_w *m, unsigned a, unsigned b, unsigned c) {
    if (m->flip) { unsigned t = b; b = c; c = t; }
    m->idx[m->ni++] = (uint16_t)a; m->idx[m->ni++] = (uint16_t)b; m->idx[m->ni++] = (uint16_t)c;
}
static void v3_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static void v3_unit(float v[3]) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Built in the right-handed rest frame; the caller mirrors Z for the left hand.
static void mesh_tube(mesh_w *m, const float p0[3], const float p1[3], float r,
                      int b0, int b1, float end_share) {
    float d[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    v3_unit(d);
    float ref[3] = { 0, 1, 0 };
    if (fabsf(d[1]) > 0.9f) { ref[0] = 0; ref[1] = 0; ref[2] = 1; }
    float u[3], v[3];
    v3_cross(ref, d, u); v3_unit(u);                  // u x v = d
    v3_cross(d, u, v);
    unsigned base = m->nv;
    for (int ring = 0; ring < 2; ring++)
        for (int k = 0; k < MESH_SIDES; k++) {
            float th = 6.2831853f * (float)k / MESH_SIDES, c = cosf(th), s = sinf(th);
            float n[3] = { c*u[0] + s*v[0], c*u[1] + s*v[1], c*u[2] + s*v[2] };
            const float *p = ring ? p1 : p0;
            float q[3] = { p[0] + r*n[0], p[1] + r*n[1], p[2] + r*n[2] };
            float sh = ring ? end_share : 0.0f;
            mesh_vert(m, q, n, (float)k / MESH_SIDES, (float)ring, b0, 1.0f - sh, b1, sh);
        }
    for (int k = 0; k < MESH_SIDES; k++) {
        int k1 = (k + 1) % MESH_SIDES;
        unsigned a0 = base + k, a1 = base + k1, c0 = base + MESH_SIDES + k, c1 = base + MESH_SIDES + k1;
        mesh_tri(m, a0, a1, c1);
        mesh_tri(m, a0, c1, c0);
    }
}
// Axis-aligned box lo..hi, each vertex fully on bone `b`.
static void mesh_box(mesh_w *m, const float lo[3], const float hi[3], int b) {
    static const float N[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (int f = 0; f < 6; f++) {
        int ax = f / 2;
        int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;     // a1 x a2 = ax (cyclic)
        float sgn = N[f][ax];
        float fixed = sgn > 0 ? hi[ax] : lo[ax];
        float c[4][3];
        static const int sa[4] = { 0, 1, 1, 0 }, sb[4] = { 0, 0, 1, 1 };
        for (int i = 0; i < 4; i++) {
            c[i][ax] = fixed;
            c[i][a1] = sa[i] ? hi[a1] : lo[a1];
            c[i][a2] = sb[i] ? hi[a2] : lo[a2];
        }
        unsigned q[4];
        for (int i = 0; i < 4; i++) q[i] = mesh_vert(m, c[i], N[f], (float)sa[i], (float)sb[i], b, 1.0f, 0, 0.0f);
        if (sgn > 0) { mesh_tri(m, q[0], q[1], q[2]); mesh_tri(m, q[0], q[2], q[3]); }
        else         { mesh_tri(m, q[0], q[2], q[1]); mesh_tri(m, q[0], q[3], q[2]); }
    }
}

// Fills the 204012-byte ovrpMesh for hand 0/1 (the size the real call memsets).
static void mesh_build(int hand, uint8_t *out) {
    memset(out, 0, MESH_SZ);
    mesh_w m = {
        .pos = (float (*)[3])(out + 12), .idx = (uint16_t *)(out + MESH_OFF_IDX),
        .nrm = (float (*)[3])(out + MESH_OFF_NRM), .uv = (float (*)[2])(out + MESH_OFF_UV),
        .bi = (int16_t (*)[4])(out + MESH_OFF_BI), .bw = (float (*)[4])(out + MESH_OFF_BW),
        .nv = 0, .ni = 0, .flip = hand ? 0 : 1,
    };
    float w[SK_BONES][3];
    rest_world(1, w);                                 // right-handed frame; mirrored below
    for (int b = 2; b < SK_BONES; b++) {
        int c = k_child[b];
        if (c < 0) continue;
        mesh_tube(&m, w[b], w[c], k_radius[b], b, c < 19 ? c : b, c < 19 ? 0.5f : 0.0f);
    }
    float lo[3] = { 0.005f, -0.012f, w[6][2] - 0.006f }, hi[3] = { w[9][0], 0.012f, w[15][2] + 0.006f };
    mesh_box(&m, lo, hi, 0);
    if (!hand)                                        // mirror Z (winding already reversed)
        for (unsigned i = 0; i < m.nv; i++) { m.pos[i][2] = -m.pos[i][2]; m.nrm[i][2] = -m.nrm[i][2]; }
    put_u32(out, 4, m.nv);
    put_u32(out, 8, m.ni);
}

// ovrpResult ovrp_GetMesh(ovrpMeshType, ovrpMesh *): 0 HandLeft, 1 HandRight
// (4/5 are the XR hands: refused with the skeletons). The managed side only
// checks the result; on a refusal OVRMesh stays uninitialised and OVRHand /
// OVRSkeleton keep working without a rendered mesh, so a mesh is a bonus.
static ovrpResult klovrp_GetMesh(int type, void *out) {
    kl_ovrp_hit("ovrp_GetMesh");
    if (type < 0 || type > 5 || !out) return KLOVRP_INVALID_PARAM;
    if (type > 1) return KLOVRP_UNSUPPORTED;
    mesh_build(type, (uint8_t *)out);
    return KLOVRP_SUCCESS;
}

// ===========================================================================
// Boundary
// ===========================================================================
typedef uint64_t (*boundary_fn)(int type, float *pts, int *count);
// ovrpBool ovrp_GetBoundaryGeometry(type, points, int *count): the same
// rectangle GetBoundaryGeometry2/3 and GetBoundaryDimensions answer.
static ovrpBool klovrp_GetBoundaryGeometry(int type, float *pts, int *count) {
    kl_ovrp_hit("ovrp_GetBoundaryGeometry");
    static boundary_fn fn;
    if (!fn) fn = (boundary_fn)kl_ovrp_sym("ovrp_GetBoundaryGeometry2");
    if (!fn) return 0;
    return ((int32_t)fn(type, pts, count) != 0) ? 1 : 0;
}

// ===========================================================================
// Registration
// ===========================================================================
static const kl_ovrp_entry k_tab[] = {
    {"ovrp_GetHandState",        (void *)klovrp_GetHandState},
    {"ovrp_GetHandState2",       (void *)klovrp_GetHandState2},
    {"ovrp_GetHandState3",       (void *)klovrp_GetHandState3},
    {"ovrp_GetSkeleton",         (void *)klovrp_GetSkeleton},
    {"ovrp_GetSkeleton2",        (void *)klovrp_GetSkeleton2},
    {"ovrp_GetSkeleton3",        (void *)klovrp_GetSkeleton3},
    {"ovrp_GetMesh",             (void *)klovrp_GetMesh},
    {"ovrp_GetHandStateAtTime",  (void *)klovrp_GetHandStateAtTime},
    {"ovrp_GetHandTrackingEnabled", (void *)klovrp_GetHandTrackingEnabled},
    {"ovrp_GetNodePose",         (void *)klovrp_GetNodePose_entry},
    {"ovrp_GetNodeVelocity",     (void *)klovrp_GetNodeVelocity_entry},
    {"ovrp_GetNodeAcceleration", (void *)klovrp_GetNodeAcceleration_entry},
    {"ovrp_GetNodePose2",        (void *)klovrp_GetNodePose2_entry},
    {"ovrp_GetNodeVelocity2",    (void *)klovrp_GetNodeVelocity2_entry},
    {"ovrp_GetNodeAcceleration2", (void *)klovrp_GetNodeAcceleration2_entry},
    {"ovrp_GetNodePoseState2",   (void *)klovrp_GetNodePoseState2},
    {"ovrp_GetNodeFrustum",      (void *)klovrp_GetNodeFrustum},
    {"ovrp_GetTrackerFrustum",   (void *)klovrp_GetTrackerFrustum},
    {"ovrp_GetTrackerPose",      (void *)klovrp_GetTrackerPose_entry},
    {"ovrp_GetControllerState3", (void *)klovrp_GetControllerState3},
    {"ovrp_GetInputState",       (void *)klovrp_GetInputState},
    {"ovrp_SetControllerHaptics2", (void *)klovrp_SetControllerHaptics2},
    {"ovrp_SetRemoteHandedness", (void *)klovrp_SetRemoteHandedness},
    {"ovrp_AreHandPosesGeneratedByControllerData", (void *)klovrp_AreHandPosesGeneratedByControllerData},
    {"ovrp_AreControllerDrivenHandPosesNatural",   (void *)klovrp_AreControllerDrivenHandPosesNatural},
    {"ovrp_IsControllerDrivenHandPosesEnabled",    (void *)klovrp_IsControllerDrivenHandPosesEnabled},
    {"ovrp_IsMultimodalHandsControllersSupported", (void *)klovrp_IsMultimodalHandsControllersSupported},
    {"ovrp_IsWideMotionModeHandPosesEnabled",      (void *)klovrp_IsWideMotionModeHandPosesEnabled},
    {"ovrp_SetWideMotionModeHandPoses",            (void *)klovrp_SetWideMotionModeHandPoses},
    {"ovrp_SendMicrogestureHint",                  (void *)klovrp_SendMicrogestureHint},
    {"ovrp_GetBoundaryGeometry", (void *)klovrp_GetBoundaryGeometry},
};

// Answered -1004 by name by the shared handler, nothing written. Either the
// host lacks the capability (face, body, eye, parametric haptics, OpenXR
// actions, multimodal hands) or the shape is not verifiable (the haptics
// envelope, GetHandTrackingState).
static const char *const k_refused[] = {
    // haptics
    "ovrp_SetControllerHapticsAmplitudeEnvelope", "ovrp_SetControllerHapticsParametric",
    "ovrp_GetControllerParametricProperties",
    // OpenXR action layer
    "ovrp_GetActionStateBoolean", "ovrp_GetActionStateFloat", "ovrp_GetActionStatePose",
    "ovrp_GetActionStatePose2", "ovrp_GetActionStateVector2f", "ovrp_TriggerVibrationAction",
    "ovrp_GetCurrentInteractionProfileName", "ovrp_GetCurrentDetachedInteractionProfile",
    // hands
    "ovrp_GetHandTrackingState",
    "ovrp_SetMultimodalHandsControllersSupported", "ovrp_SetSimultaneousHandsAndControllersEnabled",
    // face
    "ovrp_StartFaceTracking", "ovrp_StartFaceTracking2", "ovrp_StopFaceTracking",
    "ovrp_StopFaceTracking2", "ovrp_GetFaceState", "ovrp_GetFaceState2",
    "ovrp_GetFaceVisemesState", "ovrp_SetFaceTrackingVisemesEnabled",
    // body
    "ovrp_StartBodyTracking", "ovrp_StartBodyTracking2", "ovrp_StopBodyTracking",
    "ovrp_GetBodyState", "ovrp_GetBodyState4", "ovrp_RequestBodyTrackingFidelity",
    "ovrp_SuggestBodyTrackingCalibrationOverride", "ovrp_ResetBodyTrackingCalibration",
    // eyes
    "ovrp_StartEyeTracking", "ovrp_StopEyeTracking",
    NULL
};

void kl_ovrp_input_register(void) {
    kl_ovrp_register(k_tab, sizeof k_tab / sizeof k_tab[0], k_refused);
}
