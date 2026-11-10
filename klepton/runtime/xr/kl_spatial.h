// kl_spatial.h - the single spatial store shared by the ovrp facade (Lote 3,
// kl_ovrp_spatial.c) and the OpenXR facade (Lote 4, kl_openxr_spatial.c).
//
// CONTRACT FROZEN by the coordinator (Paso 0). Lote 3 implements it in
// kl_spatial.c; Lote 4 only consumes it. Changing a signature needs the
// coordinator's permission.
#ifndef KL_SPATIAL_H
#define KL_SPATIAL_H
#include <stdint.h>

typedef struct { uint8_t b[16]; } kl_uuid;
enum { KLSP_PLANE = 1, KLSP_MESH = 2, KLSP_ANCHOR = 3, KLSP_ROOM = 4 };
enum { KLSP_LBL_OTHER, KLSP_LBL_FLOOR, KLSP_LBL_CEILING, KLSP_LBL_WALL, KLSP_LBL_TABLE, KLSP_LBL_COUCH, KLSP_LBL_DOOR, KLSP_LBL_WINDOW };

// Swift -> C (ARKit threads; copy the data)
void kl_spatial_set_policy(int wants_scene, int wants_anchors);
void kl_spatial_set_available(int planes_ok, int mesh_ok, int room_ok, int anchors_ok);
void kl_spatial_plane(kl_uuid id, int removed, const float m4[16] /*originFromAnchor*/, float w, float h, int label, const float *poly2d, int npoly);
void kl_spatial_mesh(kl_uuid id, int removed, const float m4[16], const float *verts3, uint32_t nv, const uint32_t *tri3, uint32_t nt);
void kl_spatial_room(kl_uuid id, int removed, const kl_uuid *plane_ids, uint32_t n);
void kl_spatial_anchor(kl_uuid id, int removed, const float m4[16], int tracked);
void kl_spatial_request_done(uint64_t req, int ok, kl_uuid id);

// C -> Swift (the host registers these functions)
typedef struct { void (*create_anchor)(uint64_t req, const float m4[16]); void (*remove_anchor)(uint64_t req, kl_uuid id); } kl_spatial_host;
void kl_spatial_set_host(const kl_spatial_host *h);

// Readers (L3 and L4), all in the guest's tracking space
uint32_t kl_spatial_count(int kind);
int  kl_spatial_get(int kind, uint32_t i, kl_uuid *id, float m4[16]);
int  kl_spatial_plane_info(kl_uuid id, float *w, float *h, int *label, uint32_t cap, float *poly2d, uint32_t *npoly);
int  kl_spatial_mesh_info(kl_uuid id, uint32_t vcap, float *v, uint32_t *nv, uint32_t tcap, uint32_t *t, uint32_t *nt);
// The room's global mesh as one entity (all the mesh chunks joined; kl_spatial_mesh_info takes its id): 1 when there is one,
// with its id and its pose in guest space (the tracking origin: its vertices are already in that frame).
int  kl_spatial_global_mesh(kl_uuid *id, float m4[16]);
int  kl_spatial_raycast(const float o[3], const float d[3], float maxd, float *hit_pos, float *hit_nrm, kl_uuid *hit_id);
uint64_t kl_spatial_handle_for(kl_uuid id);   // ovrpSpace == internal stable XrSpace id

// ===========================================================================
// Lote 3 additions (APPENDED; everything above is the frozen contract).
// ===========================================================================
// Coordinate conventions the readers above promise:
//  * m4 is column-major (simd order), rigid, in the GUEST tracking space
//    (recenter applied, klovrp guest origin Y subtracted - kl_spatial_to_guest).
//  * KLSP_PLANE pose is the ARKit PlaneAnchor frame, as the contract says
//    (normal = local +Y, rectangle in local X/Z: w along X, h along Z); the
//    Meta/OpenXR consumers rotate it with Rx(-90deg) (Meta x = X, y = -Z, z = Y,
//    as kl_openxr_spatial.c plane_to_meta). Swift normalises the frame BEFORE
//    sending so that, after that rotation, the Meta convention holds: local +Y
//    is up for floors/tables, down for ceilings, and for walls Meta +Y (= -Z) is
//    up. The synthesised rectangle polygon is symmetric, so it is axis-agnostic.
//  * KLSP_MESH vertices are local to the mesh pose; mesh_info with NULL arrays
//    returns only the counts. vcap is in vertices (3 floats), tcap in triangles
//    (3 uint32 each).
//  * KLSP_ROOM: a real RoomAnchor, or ONE synthetic room (stable uuid) while
//    planes exist and no RoomAnchor does. KLSP_ANCHOR: only the anchors this
//    game created/saved (anclas.json in the game's files dir).
//  * plane_info with poly==NULL returns only npoly; with no stored polygon it
//    synthesises the rectangle (4 points, CCW, in plane XY).
enum { KLSP_CAP_PLANES = 1, KLSP_CAP_MESH = 2, KLSP_CAP_ROOM = 4, KLSP_CAP_ANCHORS = 8 };
int      kl_spatial_available(int cap);                       // 1 when that provider runs and is authorised
int      kl_spatial_available_known(void);                  // 1 once ARKit's answer is in (kl_spatial_set_available called)
void     kl_spatial_policy(int *wants_scene, int *wants_anchors);   // for KleptonSpatial.providers()
// recenterXform of the compositor (guest = recenter * ARKit). Identity until set.
void     kl_spatial_set_recenter(const float m4[16]);
void     kl_spatial_to_guest(const float arkit_m4[16], float guest_m4[16]);
void     kl_spatial_from_guest(const float guest_m4[16], float arkit_m4[16]);
// Async requests, shared by both facades. Allocate the id FIRST, register it in
// the facade, then issue it; the result arrives on every listener (filter by id).
uint64_t kl_spatial_new_request(void);
int      kl_spatial_create_anchor(uint64_t req, const float guest_m4[16]);   // 0 issued, -1 no host / anchors unavailable
int      kl_spatial_remove_anchor(uint64_t req, kl_uuid id);                 // 0 issued, -1 no host
typedef void (*kl_spatial_listener)(uint64_t req, int ok, kl_uuid id);
void     kl_spatial_add_listener(kl_spatial_listener fn);       // up to 4; called on the thread that completed
// Handles <-> uuids. kind 0 = handle valid but the entity is gone.
int      kl_spatial_uuid_for_handle(uint64_t h, kl_uuid *id, int *kind);
int      kl_spatial_kind_of(kl_uuid id);                        // KLSP_* or 0
// Anchor storage (QUEST/Partidas/<game>/anclas.json). SaveSpace -> saved=1, EraseSpace -> 0.
void     kl_spatial_anchor_set_saved(kl_uuid id, int saved);
int      kl_spatial_anchor_saved(kl_uuid id);
int      kl_spatial_anchor_tracked(kl_uuid id);
// Room: floor/ceiling uuids (zero when unknown), walls ordered around the room
// (adjacent walls consecutive), members = every plane contained. Arrays may be
// NULL (counts only); returns 1 when the room exists.
int      kl_spatial_room_info(kl_uuid id, kl_uuid *floor, kl_uuid *ceiling,
                              uint32_t wcap, kl_uuid *walls, uint32_t *nwalls,
                              uint32_t mcap, kl_uuid *members, uint32_t *nmembers);
// Synthetic CLOSED room (floor, ceiling, one wall per edge, ends touching) for a scene capture without a complete real set. Source, in
// order, logged as "[room] source=": guardian (the zone the player DREW, kl_guardian user source / roomscale, >= 3 vertices; edges under
// 0.3 m fused; his head need not be inside; a stationary circle is never a room), outline (the footprint of the real WALL planes at their
// measured extent -- partitions, duplicates and other rooms left out -- a missing side closed by a straight wall; no hull, no margin; an
// L stays an L; the player must stand inside it when it is first chosen), fallback (nothing drawn, no usable wall: a 2.5 m box, zw x zd +
// 0.5 m per side, centred on guest (hx, hz), partial real walls turn/grow it). Height: real ceiling, else the scene mesh inside the
// footprint, else 2.4 m. Floor: the guardian's floor, else the real one.
// 1 = synthetic room present; 0 = a complete real set (floor, ceiling, closed walls) exists (untouched).
int      kl_spatial_synth_room(float hx, float hz, float zw, float zd);
// Live room. kl_spatial_synth_room records what the game was last handed; from then on the scene capture keeps running and
// kl_spatial_live_update re-checks it (no-op until a room was handed out, when nothing was written to the store since the last
// check, and at most once per 2 s). When the room changed significantly -- walls / floor / ceiling set, footprint by >= 10 cm, scene-mesh
// points in it by > 25 %, or a real complete set replaced the synthetic one -- it is rebuilt (the user's guardian floor is only read),
// kl_spatial_room_generation rises and the listener (kl_spatial_set_room_listener) is called, outside the store lock. Returns 1 when notified.
// kl_spatial_live_update_at takes the clock (host tests). kl_spatial_generation rises with every write to the store (UI caches).
int      kl_spatial_live_update(float hx, float hz, float zw, float zd);
int      kl_spatial_live_update_at(float hx, float hz, float zw, float zd, uint64_t now_ns);
void     kl_spatial_set_room_listener(void (*fn)(void));
uint32_t kl_spatial_room_generation(void);
uint32_t kl_spatial_generation(void);
// kl_ovrp_spatial.c: the periodic call (Swift, ~1 Hz) for a game that sees the scene: kl_spatial_live_update with the head and play area.
int      kl_ovrp_spatial_live_tick(void);
// The guardian's default play space from the room ARKit has now (kl_guardian_default_from_room), for a head at guest (hx, hz);
// at most once per 2 s. Returns the guardian's answer (1 = stored), 0 when rate-limited / no valid footprint / a user boundary exists.
int      kl_spatial_update_guardian_default(float hx, float hz);
// 1 when the plane is one of the synthetic room's (not measured geometry: kept out of the environment depth).
int      kl_spatial_is_synthetic(kl_uuid id);

#endif
