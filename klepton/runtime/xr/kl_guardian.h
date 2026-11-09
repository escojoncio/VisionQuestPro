// kl_guardian.h - the Guardian core: floor height, play space (roomscale polygon / stationary circle) and the room validity
// rules, from plain numbers. Thread-safe (own mutex); no allocation. Swift calls the functions marked [Swift]
// through @_cdecl-friendly C types only (int / float / const float *). Rules and figures: informe_fw_guardian.md.
//
// Spaces: floor heights are in the visionOS world (the frame of head_display_y); the boundary polygon is x,z pairs in the
// GUEST tracking space (what ovrp_GetBoundaryGeometry hands the game; its floor is the guest floor).
#ifndef KL_GUARDIAN_H
#define KL_GUARDIAN_H
#include <stdint.h>

enum { KLG_FLOOR_NONE = 0, KLG_FLOOR_AUTO = 1, KLG_FLOOR_MANUAL = 2 };            // floorSource
enum { KLG_MODE_ROOMSCALE = 0, KLG_MODE_STATIONARY = 1 };                         // boundary mode
enum { KLG_BSRC_NONE = 0, KLG_BSRC_ROOM = 1, KLG_BSRC_USER = 2 };                 // who set the boundary
#define KLG_MAX_POINTS 4096          // boundary points kept (kMaxGuardianPoints)
#define KLG_MIN_SIDE   1.15f         // m: both sides of the inscribed rectangle for a roomscale play space
#define KLG_DEFAULT_RADIUS 1.0f      // m: stationary radius (the 2 m diameter kl_ovrp.c has always used for its circle)

// ---- floor ----
// [Swift] floor from ARKit (source 1) or the user (2); source 0 clears it. |y| > 10 m or non-finite is ignored; AUTO never replaces a MANUAL floor and moves < 1 cm are ignored. Returns 1 if changed.
int   kl_guardian_set_floor(float y, int source);
// [Swift] current floor (visionOS world, m); returns floorSource (0 = none, then *y is 0).
int   kl_guardian_get_floor(float *y);
// [Swift] the confirmed floor: remembered apart from the live one (cancel leaves it alone).
void  kl_guardian_floor_confirm(void);
float kl_guardian_get_user_preferred_floor(void);   // NaN until confirmed
// 1 when floorY - headY lies in [-2.5, -0.7] (from_slam != 0) or [-2.5, 0] (anchor / plane).
int   kl_guardian_floor_plausible(float floorY, float headY, int from_slam);
// [Swift] manual adjust, once per frame: stickY up/down moves 6 mm (|stick| < 0.1 ignored), the floor stays within 1 m of
// controllerY and never above headY - 0.1524; a controller/hand under the floor pulls it down. With no floor yet it starts at
// headY - 1.15. NaN controllerY = no hand data. Returns the new floor, marks it manual.
float kl_guardian_floor_nudge(float stickY, float headY, float controllerY);
// [Swift] a tracked controller/hand at y under the floor lowers the floor to y. Returns the floor.
float kl_guardian_floor_lower_to(float y);

// [Swift] Quest's floor setup: the player grabs the floor grid (grip of a controller, pinch of a hand) and slides it to the real
// floor, up or down; letting go keeps it. Call once per frame with `pressed` = the grab button state: while it is non-zero the floor
// is handY, marked manual and clamped to [headY - 2.5, headY - 0.1524] (a non-finite handY/headY leaves it where it is); when it
// drops to 0 the floor stays. Returns 1 while grabbing. The confirmed floor (floor_confirm) is a separate step.
int   kl_guardian_floor_grab(int pressed, float handY, float headY);
int   kl_guardian_floor_grabbing(void);      // [Swift] last `pressed` given to floor_grab

// ---- boundary ----
// [Swift] points = x,z pairs (n >= 3 for roomscale; above 4096 an evenly spaced 4096 are kept), mode 0/1, radius for stationary (<= 0 = default).
// Validates (finite, within 1e4 m, non-zero area), copies, normalises to counter-clockwise, computes the inscribed rectangle.
// Returns -1 invalid (state unchanged), 0 stored but roomscale too small (callers use the stationary circle), 1 stored and ok.
int   kl_guardian_set_boundary(const float *points, int n, int mode, float radius);
void  kl_guardian_set_stationary_radius(float radius);     // [Swift]
void  kl_guardian_clear(void);                              // [Swift] back to "not configured"
// What a game can see of the boundary, as one number (FNV-1a over mode, stationary radius and the polygon, all in mm); 0 when not
// configured. Equal while the answers of ovrp_GetBoundaryGeometry* / Dimensions* and xrGetReferenceSpaceBoundsRect do not change, so
// kl_openxr.c compares it to tell the game (XrEventDataReferenceSpaceChangePending) that the bounds moved.
uint64_t kl_guardian_boundary_signature(void);

// ---- drawn zone: Quest's guardian setup, the player draws the play area on the floor with the controller ----
// A draft of vertices (x,z pairs in the same GUEST space as the boundary), kept apart from the boundary in use until it is closed.
// Closing a valid draft sets the boundary: roomscale, source KLG_BSRC_USER, the polygon counter-clockwise. The draft is NOT stored
// across sessions (the visionOS world origin changes; the Swift side never saves the polygon).
enum { KLG_ZONE_OK = 0, KLG_ZONE_TOO_FEW = 1, KLG_ZONE_CROSSING = 2, KLG_ZONE_TOO_SMALL = 3, KLG_ZONE_INVALID = 4 };
#define KLG_ZONE_CLOSE_DIST 0.3f     // m: a vertex this near the first one (with >= 3 placed) closes the zone
// Forgets the draft; when its closed polygon is the boundary in use, that boundary is cleared too (back to not configured).
void  kl_guardian_zone_clear(void);
// Adds a vertex. Returns the vertex count after it (> 0); 0 when it was within KLG_ZONE_CLOSE_DIST of the first vertex and the zone
// closed (the boundary is set); -1 rejected (not finite / beyond 1e4 m, the draft is closed, or KLG_MAX_POINTS reached); -2 near the
// first vertex but the polygon is not valid (kl_guardian_zone_validate says why; nothing was added). A point within 2 cm of the
// last vertex adds nothing and returns the current count.
int   kl_guardian_zone_add(float x, float z);
// Removes the last vertex; on a closed draft it reopens it instead (and clears the boundary it had set). Returns the count left.
int   kl_guardian_zone_undo(void);
// KLG_ZONE_*: 0 when the draft is a valid zone: >= 3 vertices, finite with a real area, no two non-adjacent edges crossing, and an
// inscribed rectangle with both sides >= 1.15 m (the V1 minimum). Does not change anything.
int   kl_guardian_zone_validate(void);
// Validates and, when OK, sets the boundary from the draft (closed). Returns the KLG_ZONE_* code; the draft stays as it was on error.
int   kl_guardian_zone_close(void);
// Copies the draft (as drawn) into xz (cap points); returns the count; *closed = 1 when it is closed.
int   kl_guardian_zone_get(float *xz, int cap, int *closed);
// Same as set_boundary(roomscale) for the floor outline of an ARKit room, but only when no user boundary exists and the
// outline fits a 1.15 m rectangle (else nothing changes). Returns 1 when stored.
int   kl_guardian_default_from_room(const float *xz, int n);
// Global calibration (anchored to an ARKit WorldAnchor): the zone's vertices stored in the anchor's frame (x,z pairs in metres,
// y = floor) to the GUEST space of this session, for an anchor at (ax, az) turned by yaw (about +Y, the anchor's own rotation:
// guest = a + (cos*lx + sin*lz, -sin*lx + cos*lz), i.e. yaw = atan2(col2.x, col2.z) of its transform). Rigid: winding (CCW stays CCW) and
// area are kept. Returns n and fills out_xz (2n floats); -1 (out_xz untouched) when a pointer is NULL, n < 3 or > KLG_MAX_POINTS, or an
// input or result is not finite / beyond 1e4 m. out_xz must not overlap local_xz.
int   kl_guardian_zone_from_anchor(float ax, float az, float yaw, const float *local_xz, int n, float *out_xz);
int   kl_guardian_configured(void);
int   kl_guardian_boundary_source(void);
int   kl_guardian_get_mode(void);
float kl_guardian_get_radius(void);
uint32_t kl_guardian_generation(void);                       // [Swift] rises with every change of floor or boundary
// Polygon copy (x,z pairs, counter-clockwise) into xz (cap points); returns the point count (0 = none).
int   kl_guardian_get_boundary(float *xz, int cap);
// Largest oriented rectangle inside the polygon: sides w, d, centre cx, cz, turn yaw. 1 when both sides >= 1.15 m (one grid cell of slack, so an exact 1.15 m room passes).
int   kl_guardian_playspace_ok(void);
int   kl_guardian_playspace(float *w, float *d, float *cx, float *cz, float *yaw);
// What the game sees: 0 = not configured (keep the old behaviour); else 1 and *circle = 1 for a stationary circle (also
// roomscale too small), *w, *d = diameter or the inscribed rectangle's sides.
int   kl_guardian_effective(int *circle, float *w, float *d);
// Signed distance of (x,z) to the roomscale polygon (positive inside) and the closest edge point / inward normal;
// 0 when there is no polygon in use.
int   kl_guardian_polygon_test(float x, float z, float *dist, float *px, float *pz, float *nx, float *nz);

// The play area (the inscribed rectangle, ovrpBoundary_PlayArea) apart from the polygon (OuterBoundary). All answer 0 unless a
// roomscale polygon with a valid rectangle is in use (stationary circle / too small / not configured).
// Four corners x,z (guest space, counter-clockwise) of the rectangle.
int   kl_guardian_playarea_corners(float xz[8]);
// Same as polygon_test, against the rectangle (positive inside, nearest side; negative outside).
int   kl_guardian_playarea_test(float x, float z, float *dist, float *px, float *pz, float *nx, float *nz);
// Axis-aligned extent (x, z) of the polygon.
int   kl_guardian_outer_size(float *w, float *d);

// ---- room rules ----
// A floor outline (x,z pairs) fit to become walls of height H: n >= 3, coordinates finite and within 1e4 m, area >= 0.01 m2,
// 1.5 m <= H <= 6 m.
int   kl_guardian_hull_ok(const float *xz, int n, float H);
// V1: >= 3 outer walls, >= 1 floor, >= 1 ceiling, ceilingY > floorY.
int   kl_guardian_room_v1_valid(int outer_walls, int floors, int ceilings, float floorY, float ceilingY);
// Wall quads, 4 vertices x,y,z (12 floats) per wall: vertices 3 and 2 of wall i must meet vertices 0 and 1 of wall i+1
// (cyclic) within tol (0.05) on every axis. Needs nwalls >= 3.
int   kl_guardian_ring_closed(const float *quads, int nwalls, float tol);
// Roombox: wall vertex count a multiple of 4 and >= 12.
int   kl_guardian_roombox_vertices_ok(int nverts);
// All of it for a roombox: vertices, ring closed (0.05), ceiling above floor.
int   kl_guardian_roombox_valid(const float *quads, int nwalls, float floorY, float ceilingY);

#endif
