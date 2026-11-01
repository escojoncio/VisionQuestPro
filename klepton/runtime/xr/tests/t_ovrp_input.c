// t_ovrp_input.c - host test of kl_ovrp_input.c's hand state (stubs for kl_ovrp.c).
// clang -I.. -I../.. -o /tmp/t_ovrp_input t_ovrp_input.c -lm && /tmp/t_ovrp_input
#include "../kl_ovrp_input.c"
#include <assert.h>
void kl_ovrp_hit(const char *n) { (void)n; }
ovrpResult kl_ovrp_ok(const char *n) { (void)n; return 0; }
double kl_ovrp_predicted_display_time(void) { return 5.0; }
void kl_ovrp_guest_head(int s, kl_ovrp_pose7 *o) { (void)s; memset(o, 0, sizeof *o); o->py = 1.6f; o->qw = 1; }
float klovrp_guest_origin_y(void) { return 0.5f; }
void *kl_ovrp_sym(const char *n) { (void)n; return 0; }
static const kl_ovrp_entry *g_tab; static size_t g_ntab;
void kl_ovrp_register(const kl_ovrp_entry *t, size_t n, const char *const *r) { (void)r; g_tab = t; g_ntab = n; }
static void *find(const char *name) {         // what kl_ovrp_sym hands the guest for a registered name
    for (size_t i = 0; i < g_ntab; i++) if (!strcmp(g_tab[i].name, name)) return g_tab[i].fn;
    return NULL;
}
static int refused(const char *name) {
    for (const char *const *r = k_refused; *r; r++) if (!strcmp(*r, name)) return 1;
    return 0;
}
uint64_t klovrp_GetNodePoseState_impl(int a, int b, void *c) { (void)a; (void)b; memset(c, 0, 0x58); ((float *)c)[3] = 1; ((float *)c)[4] = 1; return 0; }
// x8 entry points live in kl_ovrp_sret2.S (arm64 only); the host test calls the C bodies.
void klovrp_GetNodePose_entry(void) {} void klovrp_GetNodeVelocity_entry(void) {}
void klovrp_GetNodeAcceleration_entry(void) {} void klovrp_GetNodePose2_entry(void) {}
void klovrp_GetNodeVelocity2_entry(void) {} void klovrp_GetNodeAcceleration2_entry(void) {}
void klovrp_GetTrackerPose_entry(void) {}
uint64_t klovrp_GetControllerState2_impl(int a, void *c) { (void)a; memset(c, 0, 0x40); return 0; }
int main(void) {
    float pose[26 * 7], pin[5] = { 0, 0.8f, 0.2f, 0, 0 };
    for (int i = 0; i < 26; i++) { float *p = pose + i * 7; memset(p, 0, 28); p[0] = 0.01f * i; p[1] = 1.0f; p[2] = -0.3f; p[6] = 1; }
    uint8_t out[512];
    assert(klovrp_GetHandState3(0, -1, 1, out) == KLOVRP_UNSUPPORTED);       // no provider yet
    kl_ovrp_set_hand_joints(1, pose, 0x3FFFFFF, pin, 2);
    assert(klovrp_GetHandState3(0, -1, 1, out) == 0);
    float f[128]; memcpy(f, out, 512);
    assert(*(int *)out == 3 && fabsf(f[6] - 0.5f) < 1e-5f);             // tracked, y minus origin
    assert(*(int *)(out + 416) == 2 && fabsf(f[106] - 0.8f) < 1e-5f);   // index pinch
    kl_ovrp_set_hand_joints(1, NULL, 0, NULL, 0);
    klovrp_GetHandState3(0, -1, 1, out);
    assert(*(int *)out == 0 && ((float *)out)[4] == 1.0f);               // untracked: identity
    assert(klovrp_GetHandState3(0, -1, 3, out) == KLOVRP_INVALID_PARAM);
    assert(klovrp_GetHandState3(0, -1, 0, 0) == KLOVRP_INVALID_PARAM);
    // legacy hand state: same 512 bytes through GetHandState2 (4 args) and GetHandState (3)
    kl_ovrp_set_hand_joints(1, pose, 0x3FFFFFF, pin, 2);
    uint8_t o2[512], o1[512];
    assert(klovrp_GetHandState2(0, -1, 1, o2) == 0 && *(int *)o2 == 3);
    assert(klovrp_GetHandState(0, 1, o1) == 0 && *(int *)o1 == 3);
    assert(klovrp_GetHandState(0, 2, o1) == KLOVRP_INVALID_PARAM);
    // skeleton: 24 bones, parent before child, identity bind rotations, tips, capsules
    static uint8_t sk[3644 + 64];
    for (int ver = 0; ver < 3; ver++)
        for (int t = 0; t < 2; t++) {
            size_t sz = ver == 0 ? 1484 : ver == 1 ? 3140 : 3644, co = ver == 0 ? 876 : ver == 1 ? 2532 : 3036;
            memset(sk, 0xAB, sizeof sk);
            ovrpResult r = ver == 0 ? klovrp_GetSkeleton(t, sk) : ver == 1 ? klovrp_GetSkeleton2(t, sk) : klovrp_GetSkeleton3(t, sk);
            assert(r == 0);
            for (size_t i = sz; i < sz + 64 && i < sizeof sk; i++) assert(sk[i] == 0xAB);   // nothing past the struct
            uint32_t h[3]; memcpy(h, sk, 12);
            assert((int)h[0] == t && h[1] == 24 && h[2] > 0 && h[2] <= 19);
            for (int b = 0; b < 24; b++) {
                uint8_t *p = sk + 12 + b * 36; int32_t id; int16_t par; float pose7[7];
                memcpy(&id, p, 4); memcpy(&par, p + 4, 2); memcpy(pose7, p + 8, 28);
                assert(id == b && par == k_bone[b].parent && par < b);
                assert(pose7[0] == 0 && pose7[1] == 0 && pose7[2] == 0 && pose7[3] == 1);
                if (b > 1 && b < 19) assert((t ? pose7[6] : -pose7[6]) == k_rest[b][2]);   // left mirrors Z
            }
            for (uint32_t c = 0; c < h[2]; c++) {
                uint8_t *p = sk + co + c * 32; int16_t bi; float f[7];
                memcpy(&bi, p, 2); memcpy(f, p + 4, 28);
                assert(bi >= 2 && bi < 19 && f[6] > 0.005f && f[3] != 0.0f);
            }
        }
    assert(klovrp_GetSkeleton(2, sk) == KLOVRP_INVALID_PARAM && klovrp_GetSkeleton2(2, sk) == KLOVRP_UNSUPPORTED);
    assert(klovrp_GetSkeleton3(4, sk) == KLOVRP_UNSUPPORTED && klovrp_GetSkeleton3(9, sk) == KLOVRP_INVALID_PARAM);
    assert(klovrp_GetSkeleton2(0, 0) == KLOVRP_INVALID_PARAM);
    // mesh: valid counts, in-range indices/bones, unit weights, outward-wound triangles
    static uint8_t mesh[MESH_SZ + 64];
    for (int t = 0; t < 2; t++) {
        memset(mesh, 0xAB, sizeof mesh);
        assert(klovrp_GetMesh(t, mesh) == 0);
        for (size_t i = MESH_SZ; i < MESH_SZ + 64; i++) assert(mesh[i] == 0xAB);
        uint32_t nv, ni; memcpy(&nv, mesh + 4, 4); memcpy(&ni, mesh + 8, 4);
        assert(nv > 100 && nv <= 3000 && ni > 0 && ni <= 18000 && ni % 3 == 0);
        float (*P)[3] = (float (*)[3])(mesh + 12), (*N)[3] = (float (*)[3])(mesh + MESH_OFF_NRM);
        float (*W)[4] = (float (*)[4])(mesh + MESH_OFF_BW); int16_t (*BI)[4] = (int16_t (*)[4])(mesh + MESH_OFF_BI);
        uint16_t *I = (uint16_t *)(mesh + MESH_OFF_IDX);
        for (uint32_t v = 0; v < nv; v++) {
            assert(fabsf(W[v][0] + W[v][1] + W[v][2] + W[v][3] - 1.0f) < 1e-5f);
            assert(BI[v][0] >= 0 && BI[v][0] < 19 && BI[v][1] >= 0 && BI[v][1] < 19);
            float l = sqrtf(N[v][0]*N[v][0] + N[v][1]*N[v][1] + N[v][2]*N[v][2]);
            assert(fabsf(l - 1.0f) < 1e-4f);
        }
        int bad = 0;
        for (uint32_t k = 0; k < ni; k += 3) {
            assert(I[k] < nv && I[k+1] < nv && I[k+2] < nv);
            float a[3], b[3], c[3];
            for (int i = 0; i < 3; i++) { a[i] = P[I[k+1]][i] - P[I[k]][i]; b[i] = P[I[k+2]][i] - P[I[k]][i]; }
            v3_cross(a, b, c);
            float d = c[0]*N[I[k]][0] + c[1]*N[I[k]][1] + c[2]*N[I[k]][2];
            if (d <= 0.0f) bad++;
        }
        assert(bad == 0);                                // every face winds with its normal, both hands
    }
    assert(klovrp_GetMesh(4, mesh) == KLOVRP_UNSUPPORTED && klovrp_GetMesh(7, mesh) == KLOVRP_INVALID_PARAM);
    // legacy node queries through the sret bodies: 28 bytes, velocity rides in Position
    float r[8] = {0};
    klovrp_GetNodePose_impl(0, r);
    assert(r[3] == 1.0f && r[4] == 1.0f);                                  // stub pose: identity-ish, x = 1
    klovrp_GetNodeVelocity_impl(0, r);
    assert(r[0] == 0 && r[3] == 1.0f && r[4] == 0 && r[7] == 0);           // identity orientation, 28 bytes only
    klovrp_GetNodeAcceleration2_impl(0, 0, r);
    assert(r[3] == 1.0f);
    ovrpResult ps = klovrp_GetNodePoseState2(0, 0, out);
    assert(ps == 0 && klovrp_GetNodePoseState2(0, 0, 0) == KLOVRP_INVALID_PARAM);
    klovrp_GetTrackerPose_impl(0, r);
    assert(r[3] == 1.0f && klovrp_GetTrackerFrustum(0).zNear == 0.0f);
    assert(klovrp_GetControllerState3(3, out) == 0 && klovrp_GetControllerState3(3, 0) == KLOVRP_INVALID_PARAM);
    // controller/hand coherence: one source for connected, active, in-hand, node presence and
    // GetHandTrackingEnabled. The Touch pair is always connected (pinch emulation drives a hand
    // with no Sense controller) next to the hand bits; attachment only turns hand tracking off.
    int32_t ht;
    kl_ovrp_set_hand_joints(0, NULL, 0, NULL, 0);
    kl_ovrp_set_hand_joints(1, NULL, 0, NULL, 0);
    assert(klovrp_ctrl_connected() == 0x63u && klovrp_ctrl_active() == 0x3u);
    assert(klovrp_ctrl_in_hand(3) && klovrp_ctrl_in_hand(4) && klovrp_ctrl_in_hand(12) && klovrp_ctrl_in_hand(13));
    assert(!klovrp_ctrl_in_hand(0) && !klovrp_ctrl_in_hand(9));
    assert(klovrp_node_live(0) && klovrp_node_live(3) && klovrp_node_live(4) && klovrp_node_live(12) && klovrp_node_live(13));
    // Stable system-setting semantics: enabled (provider present, nothing attached) whether or
    // not a hand is visible right now.
    assert(klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 1);
    kl_ovrp_set_hand_joints(1, pose, 0x3FFFFFF, pin, 2);
    assert(klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 1);
    assert(klovrp_ctrl_connected() == 0x63u && klovrp_ctrl_active() == 0x3u);
    assert(klovrp_GetHandTrackingEnabled(0) == KLOVRP_INVALID_PARAM);
    // One Sense controller attached, other hand free: both Touch bits stay (the free hand keeps
    // its pinch emulation), hand bits stay, hand tracking is off.
    kl_ovrp_set_controller_attached(0, 1);
    assert(klovrp_ctrl_connected() == 0x63u && klovrp_ctrl_active() == 0x3u);
    assert(klovrp_ctrl_in_hand(3) && klovrp_ctrl_in_hand(12) && klovrp_ctrl_in_hand(4) && klovrp_ctrl_in_hand(13));
    assert(klovrp_node_live(3) && klovrp_node_live(12) && klovrp_node_live(4) && klovrp_node_live(13));
    assert(klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 0);
    kl_ovrp_set_controller_attached(0, 0);
    kl_ovrp_set_controller_attached(1, 1);
    assert(klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 0);
    kl_ovrp_set_controller_attached(0, 1);
    assert(klovrp_ctrl_connected() == 0x63u && klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 0);
    kl_ovrp_set_controller_attached(2, 1);                          // out-of-range hand: ignored
    kl_ovrp_set_controller_attached(0, 0);
    kl_ovrp_set_controller_attached(1, 0);
    assert(klovrp_GetHandTrackingEnabled(&ht) == 0 && ht == 1);
    kl_ovrp_set_hand_joints(1, NULL, 0, NULL, 0);
    kl_ovrp_input_register();
    // ovrp_GetEyeGazesState: success with nothing written read as a valid (garbage) gaze. Eye tracking
    // is refused by name (like Start/StopEyeTracking), so the caller never reads the struct.
    assert(refused("ovrp_GetEyeGazesState") && !find("ovrp_GetEyeGazesState"));
    puts("t_ovrp_input ok");
    return 0;
}
