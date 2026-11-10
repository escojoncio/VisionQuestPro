// kl_ovrp_priv.h - what the ovrp_* "batch" files need from kl_ovrp.c.
//
// kl_ovrp.c keeps its 9,900 lines of state private; a batch (kl_ovrp_render.c,
// kl_ovrp_input.c, kl_ovrp_spatial.c, kl_ovrp_misc.c) sees only what is declared
// here. Nothing in this header is for the compositor or for Swift: it is NOT in
// the bridging header.
//
// ---------------------------------------------------------------------------
// HOW A BATCH REGISTERS ITS FUNCTIONS
// ---------------------------------------------------------------------------
// 1. Write the functions. The guest calls them straight from libunity /
//    libOculusXRPlugin with the real OVRPlugin argument list (AAPCS64), so a
//    function takes its true parameters, NOT a name in x0. Call kl_ovrp_hit()
//    first (call accounting and the "first-call <- image+off" log line).
//
//        static ovrpResult klovrp_GetFoo(float *out) {
//            kl_ovrp_hit("ovrp_GetFoo");
//            if (!out) return KLOVRP_INVALID_PARAM;
//            *out = 1.0f;
//            return KLOVRP_SUCCESS;
//        }
//
//    ovrpBool getters (ABI v1.x) return KLOVRP_TRUE / 0 instead; an ovrpResult
//    0 read as a Bool is "false" (see the long comment above klovrp_called in
//    kl_ovrp.c). Do not guess the shape from the name: tools/ovrp_abi.py
//    disassembles real libOVRPlugin.so builds and reports Bool-vs-Result,
//    float-vs-int, sret (x8) and out-pointer extents against these files.
//    Functions that return a struct through x8 (ovrpPosef-shaped, 28 bytes;
//    ovrpPoseStatef, 88) need an sret thunk: kl_ovrp_sret.S and kl_ovrp_sret2.S
//    are taken - put yours in your own .S and add it to RUNTIME_SHIP; the C side
//    is `void klovrp_Foo_impl(args..., void *out)` plus an extern
//    `klovrp_Foo_entry` in the table (see kl_ovrp_sret2.S).
//
// 2. Build the table and the refusal list, and register them from ONE function
//    named exactly kl_ovrp_<batch>_register (the four are already declared
//    below and called from kl_ovrp.c; the empty bodies already exist):
//
//        static const kl_ovrp_entry k_tab[] = {
//            {"ovrp_GetFoo", (void *)klovrp_GetFoo},
//        };
//        // Capabilities this host does NOT have: answered ovrpFailure_Unsupported
//        // (-1004) by name, logged once. NULL-terminated. Use for whole
//        // subsystems; write your own function when out-params must be zeroed.
//        static const char *const k_refused[] = { "ovrp_StartFaceTracking", NULL };
//
//        void kl_ovrp_render_register(void) {
//            kl_ovrp_register(k_tab, sizeof k_tab / sizeof k_tab[0], k_refused);
//        }
//
//    Tables must have static storage duration (the pointers are kept). Either
//    argument may be empty/NULL.
//
//    WHY NOT __attribute__((constructor)): libklepton.a is a static archive; a
//    member nothing references is never linked, so its constructor never runs.
//    The explicit kl_ovrp_<batch>_register call from kl_ovrp_register_all()
//    is the reference that keeps the member in.
//
// 3. Lookup order (kl_ovrp_sym / kl_ovrp_status):
//        g_ovrp_impl (kl_ovrp.c's own)  ->  registered tables, in registration
//        order (first one wins)  ->  the shared ok/unsupported/yes/no lists
//        ->  the named "unimplemented" stub.
//    So a registered entry OVERRIDES the shared lists (no need to delete a name
//    from g_ovrp_result_unsupported), but cannot override a name kl_ovrp.c
//    already implements itself. kl_ovrp_status reports 2 for a registered
//    function and 3 for a registered refusal.
//
// 4. Events: kl_ovrp_push_event(type, payload, len) queues one for
//    ovrp_PollEvent2 / ovrp_PollEvent. It is thread-safe and may be called from
//    any thread (a Swift callback, an ARKit task). An event pushed becomes
//    visible only from the NEXT poll on, so the requestId a Create/Query/Save
//    call returns always reaches the guest before the event that names it, even
//    if another guest thread is polling concurrently.
//
// 5. Add a test under klepton/runtime/xr/tests/ and update bitacora.md.
#ifndef KL_OVRP_PRIV_H
#define KL_OVRP_PRIV_H
#include <stddef.h>
#include <stdint.h>
#include "kl_ovrp.h"

// ---- OVRPlugin ABI scalar types (ABI 1.x; ovrpBool is a 4-byte int) -------
typedef int32_t  ovrpResult;
typedef int32_t  ovrpBool;
typedef struct { float x, y; }       ovrpVector2f;
typedef struct { float x, y, z; }    ovrpVector3f;
typedef struct { float x, y, z, w; } ovrpQuatf;
typedef struct { ovrpQuatf Orientation; ovrpVector3f Position; } ovrpPosef;   // 28 bytes
typedef struct { int32_t w, h; }     ovrpSizei;

// ---- results (same values as OVRP_* in kl_ovrp.c) --------------------------
#define KLOVRP_SUCCESS            0
#define KLOVRP_TRUE               1
#define KLOVRP_INVALID_PARAM      (-1001)
#define KLOVRP_NOT_INITIALIZED    (-1002)
#define KLOVRP_UNSUPPORTED        (-1004)
#define KLOVRP_OPERATION_FAILED   (-1006)
#define KLOVRP_INSUFFICIENT_SIZE  (-1007)

// ---- registration ----------------------------------------------------------
typedef struct { const char *name; void *fn; } kl_ovrp_entry;
// tab: functions served (may be NULL if n == 0). refused: NULL-terminated names
// answered -1004 by the shared handler (may be NULL). Safe to call from the
// batch's kl_ovrp_<batch>_register; not for use after the guest has started.
void kl_ovrp_register(const kl_ovrp_entry *tab, size_t n, const char *const *refused);

// One per batch; defined (empty) in each batch file, called once by kl_ovrp.c
// before the first lookup.
void kl_ovrp_render_register(void);    // kl_ovrp_render.c
void kl_ovrp_input_register(void);     // kl_ovrp_input.c
void kl_ovrp_spatial_register(void);   // kl_ovrp_spatial.c
void kl_ovrp_spatial_global_mesh(int on);   // kl_ovrp_spatial.c: whether rooms carry a GLOBAL_MESH anchor (Quest 3 identity)
void kl_ovrp_misc_register(void);      // kl_ovrp_misc.c

// ---- helpers over kl_ovrp.c internals ---------------------------------------
// Call accounting + first-call log; call first in every served function.
void kl_ovrp_hit(const char *name);
// Budget of permanent diagnostic lines (spatial call/event traces, passthrough expiry detail): 1 while the session
// total is under KL_OVRP_DIAG_LINES, else 0 (and one "[diag] budget exhausted" line when it runs out). Call once per line.
int  kl_ovrp_diag_ok(void);
// "[sp-sum]" periodic summary (kl_ovrp_spatial.c): one PollEvent call, with the type of the event it delivered (-1 = none).
void kl_ovrp_sp_note_poll(int32_t type);
// The shared handlers' bodies, for a function that wants the same answer
// without a table entry of its own: returns 0 / -1004 after kl_ovrp_hit(name).
ovrpResult kl_ovrp_ok(const char *name);
ovrpResult kl_ovrp_unsupported(const char *name);
// ovrp_Initialize* has run (and ovrp_Shutdown has not).
int  kl_ovrp_is_initialized(void);

// Poses exactly as the guest is told them: the pinned per-frame head/hand, with
// klovrp_guest_origin_y() already subtracted (and the Touch grip offset for
// hands). step = 0 render, 1 physics (as ovrp_GetNodePoseState3's step).
// hand: 0 left, 1 right. All spatial output (anchors, planes, rays) must go
// through the same transform.
typedef struct { float px, py, pz, qx, qy, qz, qw; } kl_ovrp_pose7;
void  kl_ovrp_guest_head(int step, kl_ovrp_pose7 *out);
void  kl_ovrp_guest_hand(int step, int hand, kl_ovrp_pose7 *out);
// Y of the guest's tracking origin relative to the display's (0 under
// floor/stage tracking, the measured eye height under EyeLevel). The same value
// kl_ovrp.c subtracts from every pose it reports.
float klovrp_guest_origin_y(void);
// Standing eye height used for the default head (m).
float klovrp_measured_eye_height(void);
// kl_ovrp_tracking_origin() (kl_ovrp.h): 0 eye, 1 floor, 2 stage.

// ---- controller / hand coherence (defined in kl_ovrp_input.c) --------------
// The ONE answer to "what is the app driven by", shared by every query that
// can disagree about it. ovrpController bits: LTouch 1, RTouch 2, LHand 0x20,
// RHand 0x40. The Touch pair is always connected (pinch emulation drives a hand
// without a Sense controller), the Hand bits follow the hand-tracking provider.
uint32_t klovrp_ctrl_connected(void);   // ovrp_GetConnectedControllers
uint32_t klovrp_ctrl_active(void);      // ovrp_GetActiveController
int      klovrp_ctrl_in_hand(int node); // ovrp_GetControllerIsInHand (nodes 3/4/12/13)
int      klovrp_node_live(int node);    // ovrp node present / tracked: head always

// ---- events (ovrp_PollEvent2 / ovrp_PollEvent) ------------------------------
// type: ovrpEventType value. payload/len: the bytes of the event-specific struct
// (what ovrp_PollEvent2 returns through *eventData; for ovrp_PollEvent they are
// copied after the 4-byte type inside ovrpEventDataBuffer). len <= 256.
// ovrp_PollEvent never writes more than the guest's OWN OVRPlugin version writes
// (kl_ovrp.c klovrp_pollevent_cap: 4 .. 3996 bytes depending on the SDK, type
// included), so on an old SDK a payload longer than its buffer is delivered
// truncated by that call; ovrp_PollEvent2 always hands out the whole payload.
// Returns 0, or -1 if the queue (64) is full or len is too large.
int kl_ovrp_push_event(int32_t type, const void *payload, size_t len);

#endif
