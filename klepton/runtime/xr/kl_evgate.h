// When a queued runtime event (ovrp_PollEvent*, xrPollEvent) may be handed to the guest. Pure; tests/t_evgate.c.
//
// On a Quest an asynchronous request (space discovery/query, component status, save/erase...) never completes inside
// the frame that issued it: the completion event arrives in a later frame's drain. Guests rely on that. Measured on
// MRUK v71 (Harem Life VR): MRUK.Awake starts the scene load and MRUK.Start assigns _cameraRig; with the events
// deliverable from the very next poll the whole load (rooms, children, LocateSpace) ran inside the drain of the frame
// that ran Awake, before Start, and CreateSceneDataFromDevice threw NullReferenceException on _cameraRig.trackingSpace
// (libil2cpp +0x4244e5c), so no room and no floor collider.
//
// Rule: a guest drains its queue once per frame with `while (Poll(...))`, so every poll that answers "nothing" closes
// one drain. An event is deliverable only (a) in a drain that started after the one in progress (or next) at push time
// was closed, and (b) once KL_EVGATE_LATENCY_MS have passed, which covers a guest that drains more than once a frame.
#ifndef KL_EVGATE_H
#define KL_EVGATE_H

#include <stdint.h>

#define KL_EVGATE_LATENCY_MS 50

typedef struct { uint64_t drain; uint64_t due_ms; } kl_evgate;

// At push: `drains` = empty polls answered so far.
static inline kl_evgate kl_evgate_stamp(uint64_t drains, uint64_t now_ms) {
    kl_evgate g = { drains + 1, now_ms + KL_EVGATE_LATENCY_MS };
    return g;
}

// At poll: 1 when the head event may be delivered.
static inline int kl_evgate_due(kl_evgate g, uint64_t drains, uint64_t now_ms) {
    return drains >= g.drain && now_ms >= g.due_ms;
}

#endif
