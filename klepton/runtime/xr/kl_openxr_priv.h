// kl_openxr_priv.h - what the OpenXR "batch" files need from kl_openxr.c.
//
// kl_openxr.c defines its own OpenXR types (it includes no Khronos header), so
// this header is self-contained: batches declare the structs they need locally
// (transcribed from the spec) and use only the primitives below.
//
// ---------------------------------------------------------------------------
// HOW A BATCH REGISTERS ITS FUNCTIONS AND EXTENSIONS
// ---------------------------------------------------------------------------
// 1. Write the functions with their real OpenXR signatures; they return
//    XrResult (int32, 0 = XR_SUCCESS, negatives = errors). A function is called
//    straight by the guest through the pointer xrGetInstanceProcAddr hands out
//    (no name in x0).
//
//        static XrResult klxr_GetFooFB(void *session, int32_t *out) { ... }
//
// 2. Register from ONE function named kl_openxr_<batch>_register (already
//    declared below, with an empty body in each batch file):
//
//        static const klxr_row_def k_rows[] = {
//            {"xrGetFooFB", (void *)klxr_GetFooFB},
//        };
//        static const char *const k_foo_fns[] = {"xrGetFooFB", NULL};
//        static int foo_gate(void) { return kl_spatial_planes_ok(); }  // optional
//
//        void kl_openxr_spatial_register(void) {
//            klxr_register_rows(k_rows, sizeof k_rows / sizeof k_rows[0]);
//            // advertise ONLY if the whole cycle is served (the startup
//            // selfcheck prints SELFCHECK for any listed fn that is not):
//            klxr_register_extension("XR_FB_foo", 1, foo_gate, k_foo_fns);
//            // extensions this host will never serve: every entry point answers
//            // XR_ERROR_FUNCTION_UNSUPPORTED by name (logged once, counted):
//            klxr_register_refused("XR_ML_bar", k_bar_fns);   // NULL-terminated
//        }
//
//    A name that already has a row in kl_openxr.c's KL_XR_ENTRY_POINTS table
//    (still a refusal stub, or implemented) is OVERRIDDEN by klxr_register_rows;
//    any other name gets a new dynamic row (table capacity 1024 rows in all).
//    Tables/strings must have static storage duration.
//
//    The register functions run once, inside klxr_install (so also before the
//    first xrEnumerateInstanceExtensionProperties), after the core table is
//    filled. They are called explicitly (not __attribute__((constructor)))
//    because libklepton.a is an archive: an unreferenced member is not linked.
//
// 3. kl_openxr_status: 2 = implemented (core or registered), 3 = registered
//    refusal, 0 = still the generic refusal stub, -1 = no such name.
//
// 4. Events: klxr_push_event(type, payload, len) queues an event for xrPollEvent
//    (thread-safe). `payload` is the event struct AFTER its 16-byte base header
//    (type, next); the runtime writes type and next = NULL. Delivered after the
//    session's own state events, and only from the poll AFTER the one in
//    progress at push time, so an async request's future/id reaches the guest
//    before its completion event.
#ifndef KL_OPENXR_PRIV_H
#define KL_OPENXR_PRIV_H
#include <stddef.h>
#include <stdint.h>
#include "kl_openxr.h"

typedef int32_t XrResult;
enum { KLXR_R_SUCCESS = 0, KLXR_R_FUNCTION_UNSUPPORTED = -7 };   // spec values

typedef struct { const char *name; void *fn; } klxr_row_def;

// Install (or override) implementations. n rows; name/fn must be non-NULL.
void klxr_register_rows(const klxr_row_def *defs, size_t n);
// Every name answers XR_ERROR_FUNCTION_UNSUPPORTED, counted and logged once.
// `ext` is for the log only. NULL-terminated names.
void klxr_register_refused(const char *ext, const char *const *names);
// Announce an extension in xrEnumerateInstanceExtensionProperties. gate: NULL =
// always, else evaluated once when the list is resolved (non-zero = advertise).
// fns: NULL-terminated entry points the selfcheck must find served (may be NULL).
void klxr_register_extension(const char *name, uint32_t version, int (*gate)(void),
                             const char *const *fns);
int  klxr_push_event(uint32_t type, const void *payload, size_t len);   // len <= 480; 0 / -1

void *klxr_current_session(void);   // the XrSession handle, NULL until xrCreateSession
int64_t klxr_time_now(void);        // XrTime as xrWaitFrame's clock
void *klxr_current_instance(void);  // the XrInstance handle, NULL until xrCreateInstance
int   klxr_spatial_entity_enabled(void);   // 1 when the live instance enabled XR_FB_spatial_entity

// External spaces (kl_openxr.c): a pooled XrSpace whose pose is owned by a batch.
// xrLocateSpace calls kl_openxr_spatial_pose(user, q, p) (1 tracked / 0 lost);
// xrDestroySpace calls kl_openxr_spatial_space_destroyed(user). NULL = pool full.
void *klxr_space_create_external(void *user);
int   kl_openxr_spatial_pose(void *user, float q[4], float p[3]);
void  kl_openxr_spatial_space_destroyed(void *user);
int   kl_openxr_spatial_system_props(void *chain);   // fills spatial caps in an xrGetSystemProperties chain

// One per batch, defined (empty) in each batch file.
void kl_openxr_spatial_register(void);   // kl_openxr_spatial.c (Lote 4)
void kl_openxr_ext_register(void);       // kl_openxr_ext.c     (Lote 5)
void kl_openxr_vendor_register(void);    // kl_openxr_vendor.c  (Lote 6)

#endif
