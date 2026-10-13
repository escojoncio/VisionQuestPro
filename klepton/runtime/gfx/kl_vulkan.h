// libvulkan.so — the synthetic Vulkan loader, over MoltenVK.
//
// BONELAB boots completely and cannot render because its graphics API is Vulkan
//. Unity `dlopen`s `libvulkan.so` as the first thing it
// tries, and it is the only API this build probes: when the open fails, Unity
// warns that the device does not meet the application's hardware requirements
// and falls back to a GLES 2 context whose shaders do not exist in the blob.
// So this file is what turns the first domino.
//
// It is served, not loaded, for exactly the reason kl_ovrp and kl_openxr are:
// the real `libvulkan.so` is Android's loader and would go looking for an ICD
// through Android's driver plumbing. MoltenVK IS the ICD here, so this stands
// in for the loader and nothing else.
//
// **There is no ABI translation anywhere in this file, and that is a measured
// property rather than an assumption.** Vulkan's structs are plain C with
// explicit types and the guest is aarch64 LP64 little-endian, exactly like the
// host — so a `VkImageCreateInfo` the guest fills in is byte-identical to one
// this process would build. That is what makes a 182-entry-point API tractable
// as a forwarding table, and it is emphatically NOT true of the JNI, libc or
// OVRPlugin seams next door. `make vkabi` is the gate that keeps it true.
//
// Three things are genuinely ours rather than forwarded, and they are all the
// same thing wearing different hats — Android's window system does not exist:
//
//   1. `VK_KHR_android_surface` / `vkCreateAndroidSurfaceKHR`. MoltenVK has no
//      such extension and never will. The surface is synthesized.
//   2. the `VK_KHR_swapchain` family. Ours end to end, backed by ordinary
//      VkImages we allocate — no CAMetalLayer, no presentation engine, nothing
//      that has to be on a screen. This is what lets a frame be produced with
//      the compositor wiring still an open question.
//   3. `vkQueuePresentKHR`, which is where the frame is read back and dumped.
//
// Everything else is MoltenVK's, reached through its own `vkGetInstanceProcAddr`.
#ifndef KL_VULKAN_H
#define KL_VULKAN_H

// The synthetic-library quartet, same shape as kl_egl / kl_ovrp / kl_mediandk.
// See kl_dl.c's klb_dlopen and kl_can_dlopen for why `claims` exists separately
// from `dlopen`: an existence check the guest makes on its own behalf has to be
// answerable without loading anything.
void *kl_vulkan_dlopen(const char *soname);   // NULL if not one of ours
int   kl_vulkan_claims(const char *soname);   // the same test, without opening
int   kl_vulkan_is_handle(const void *h);
void *kl_vulkan_sym(const char *name);        // dlsym semantics: NULL when absent

// Tier lookup for kl_shim_lookup, for a guest that binds vk* at load time
// through a DT_NEEDED rather than through dlopen/dlsym. libunity takes the
// dlopen road; libSLZQuestNative names six vk* symbols directly.
void *kl_vulkan_lookup(const char *name);

// Was MoltenVK actually reachable? Separated from `claims` so the failure can be
// reported once, by name, at the point the guest asks — rather than as a NULL
// symbol somewhere inside Unity's device selection.
int   kl_vulkan_available(void);

// ---------------------------------------------------------------------------
// The eye-texture seam, for kl_ovrp.c
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain: Unity's Oculus XR path takes its eye
// textures from `ovrp_GetLayerTexture2` and submits them to the Oculus
// compositor. kl_ovrp.c has always answered that with a GL texture name, and on
// a Vulkan guest the name goes straight into `vkCreateImageView` as a VkImage
// handle — a segfault inside MoltenVK with a small integer for an address.
//
// So on the Vulkan path the eye textures have to be real VkImages, and these
// are how kl_ovrp.c asks for them without knowing anything about Vulkan.

// Did the guest actually bring up a Vulkan device through us? This is the test
// kl_ovrp.c branches on, and it is a measurement of what the guest DID rather
// than a guess from a renderer enum: a guest that resolved vk* symbols and then
// chose GLES would answer false here, correctly.
int kl_vulkan_guest_active(void);

// The 8-byte LUID of the GPU the guest's Vulkan instance sees, for ovrp_GetDisplay
// AdapterId2 to hand back so Unity's OculusXR device match picks the right (only)
// VkPhysicalDevice instead of a NULL one. Fills out[8] and returns 1 when a Vulkan
// instance exists and reports a valid LUID; returns 0 (out untouched) otherwise —
// a GLES guest, where the adapter id must stay empty.
int kl_vulkan_display_luid(uint8_t out[8]);

// Create (or return) the eye image for a swapchain stage and eye. The returned
// value is the VkImage handle widened to 64 bits, which is exactly what
// ovrp_GetLayerTexture2's out-parameter carries.
unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb);

// ...and the same thing for the layout where the two eyes are ARRAY LAYERS of
// one image rather than two images — ovrpLayout_Array, which is what Unity
// calls Single Pass Instanced / Multiview and what a Quest title normally ships
// with. `layers` is 2 there and 1 for Stereo. Under Array both eyes get the
// SAME handle back and the eye is the layer index, so kl_ovrp.c must not treat
// two identical handles as a mistake.
unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers);

// ...and the general form, which is what `ovrp_GetLayerTexture2` actually asks:
// storage for ONE LAYER's stage/eye.
//
// The two above are this with `layer_key = KLVK_EYE_LAYER`, and until 2026-08-15
// they were the only form — so every layer the guest set up was handed the eye
// layer's images. A Unity guest never noticed (its one other layer is a 1x1
// dummy nothing renders into); an Unreal guest draws its splash and its stereo
// UI quads into real ones, and they landed in the corner of the eye texture.
//
// Only the eye layer is published into kl_glfb's eye table, because that table
// is what every compositor samples. A size that differs from the cached one
// re-allocates rather than answering with storage of the wrong shape.
#define KLVK_EYE_LAYER (-1)
unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int srgb, int layers);

// ...and the MTLTexture behind one of those, for a compositor that has to draw
// the layer. The eye layer is deliberately NOT reachable here: its textures go
// through kl_glfb's eye table, where every consumer already looks, and a second
// door onto the same storage is how two answers start to differ.
//
// A layer allocated for eye 0 alone — the common case, one texture with a
// per-eye ViewportRect selecting the part — answers eye 1 from it rather than
// with nothing. NULL when the layer has no storage for that stage.
void *kl_vulkan_mtl_for_handle(unsigned long long h);
// 1 once the guest has rendered/copied into layer `layer_key`'s own image.
int kl_vulkan_layer_written(int layer_key);
void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h);

// Rename a layer's slot in the image table. kl_ovrp calls this when a guest
// destroys a layer and sets up an identical one (wrath2's "compiling shaders"
// toast does it on every text update): the new id inherits the old id's
// images, still holding the previous picture, instead of allocating a fresh
// empty one that blinks the layer off until the guest has drawn into it. This
// is the same reuse the slot's GL names have always had. A no-op if the old
// key is not in the table.
void kl_vulkan_layer_rekey(int old_key, int new_key);

// The MTLTexture behind the wild-handle scratch — the image AC Nexus actually
// composites its frame into. NULL until the scratch exists. w/h report 4096.
void *kl_vulkan_wild_scratch_mtl(int *w, int *h);

// Application SpaceWarp storage (VPKTAA phase 1): the motion-vector and depth
// images ovrp_GetLayerTextureSpaceWarp hands the guest for the EYE layer, one
// pair per swapchain stage. Same shape as the eye images: under the Array
// (multiview) layout one image per stage with two array layers, both eyes get
// the same handle and the eye is the slice; under Stereo one image per eye.
// `vk_format` is a VkFormat (int so this header stays Vulkan-free). The
// motion-vector image is a colour target the guest renders into; the depth
// image a depth/stencil target. Both are SAMPLED so a compositor can read them.
// Returns the VkImage as a 64-bit handle, 0 on failure. Only kl_ovrp calls it,
// and only with KL_VPKTAA=1.
#define KLVK_ASW_MOTION 0
#define KLVK_ASW_DEPTH  1
unsigned long long kl_vulkan_spacewarp_image(int stage, int eye, int kind,
                                             unsigned w, unsigned h,
                                             int vk_format, int layers);
// The MTLTexture behind one of those, for the compositor / VPKTAA. `slice` is
// the array layer holding `eye` (0 under Stereo). NULL when nothing was
// allocated for that (stage, eye, kind). For a depth image the texture is the
// depth/stencil texture MoltenVK made (D24S8 is served as depth32Float_stencil8
// on Apple GPUs) — a sampler needs a depth-plane view of it.
void *kl_vulkan_spacewarp_mtl(int eye, int stage, int kind,
                              int *w, int *h, int *slice);
// Monotonic count of guest commands RECORDED that write the (stage, kind)
// image: a render pass / dynamic rendering that has it as an attachment, or a
// copy/blit into it. Recorded, not executed — a per-frame "the guest wrote its
// motion vectors" signal is "this count moved since the previous frame on this
// stage", which is what kl_ovrp files into the frame record.
unsigned kl_vulkan_spacewarp_writes(int stage, int kind);

// ---------------------------------------------------------------------------
// VPKTAA eye depth (universal, every Vulkan guest; only with KL_VPKTAA=1 and
// KL_VPKTAA_DEPTH != 0).
//
// Per guest frame kl_vulkan watches the render passes / dynamic rendering the
// guest RECORDS and picks the depth attachment that belongs to the eye picture:
//   1 "eye pass"   the depth attachment of a pass that also has the eye image
//                  as an attachment (colour or resolve target) — preferred:
//                  full resolution, same layout as the colour;
//   2 "spacewarp"  the AppSW depth image (kl_vulkan_spacewarp_image) when the
//                  guest wrote it this frame;
//   3 "size match" the depth attachment of the LAST pass whose depth image has
//                  the eye image's size (URP final-blit pattern: the scene pass
//                  has no eye attachment).
// To make that depth readable at all, eye-sized depth images are created with
// SAMPLED usage and without TRANSIENT (VPKEngine would keep them on tile), and
// every depth attachment's storeOp is forced to STORE (DONT_CARE discards it
// on Metal). At kl_vulkan_frame_done(stage) the pick is filed for the stage;
// a depth image the guest shares between stages is first COPIED (on the
// guest's queue, before the frame-end idle wait) into a per-stage snapshot,
// so the asynchronous VPKTAA never reads a depth the next frame is drawing.
// kl_ovrp tags the record with the frame-record serial (kl_vulkan_eye_depth_tag)
// so a consumer can tell "this depth belongs to frame N" from a stale one.
// INLINE VPKTAA (kl_vulkan_taa_inline, below) reads the depth inside
// kl_vulkan_frame_done, before the next frame can draw into it: no snapshot is
// ever taken, `mtl` is always the guest's live image, and a multisampled depth
// is filed too (samples > 1, `mtl` = the MS texture; eye-sized MS depth images
// are then also created SAMPLED and not TRANSIENT).
//
// Depth convention (Vulkan clip z in [0,1]): reversed_z = 1 when the eye pass
// cleared depth to 0.0 (Unity/Unreal on Vulkan), else from the census of
// pipeline depthCompareOps (GREATER* = reversed).
typedef struct {
    void *mtl;              // MTLTexture (borrowed): the depth (or depth/stencil) texture
    int   w, h, layers;     // of that texture
    int   slice;            // the array layer holding the requested eye
    int   vk_format;        // VkFormat of the guest's image
    int   samples;          // 1 unless the guest kept MSAA (async path: not usable;
                            // inline: `mtl` is then the multisample texture, live)
    int   reversed_z;       // 1 reversed (far = 0), 0 standard, -1 unknown
    float clear_depth;      // depth clear value seen on the eye pass, -1 if none
    int   source;           // 1 eye pass, 2 spacewarp depth, 3 size match
    int   snapshot;         // 1 = our per-stage copy, 0 = the guest's live image
    unsigned long long tag; // kl_ovrp frame-record serial (0 = not tagged yet)
    unsigned long long vk_serial; // kl_vulkan_frame_serial() after that frame
} kl_vulkan_eye_depth;
// The record filed for `stage`, for `eye`. When `want_tag` is non-zero and the
// stage's record carries another tag, every stage is searched for want_tag.
// Returns 1 when *out is filled with a usable record.
int  kl_vulkan_eye_depth_get(int stage, int eye, unsigned long long want_tag,
                             kl_vulkan_eye_depth *out);
// The short form: the MTLTexture of `stage`'s eye depth (NULL = none filed).
void *kl_vulkan_eye_depth_mtl(int stage, int *w, int *h, int *layers, int *fmt);
// Called by kl_ovrp right after kl_vulkan_frame_done(stage) with the serial of
// the frame record it filed for that frame.
void kl_vulkan_eye_depth_tag(int stage, unsigned long long serial);

// Write both eyes of a stage out as PNGs. Called at frame submission, which on
// this path is ovrp_EndFrame4 — the guest's own assertion that it has finished
// drawing them.
void kl_vulkan_capture_eyes(unsigned frame, int stage);
void kl_vulkan_capture_layers(void);

// "The guest has finished drawing this stage's eye textures." Called from the
// same place the capture is (ovrp_EndFrame4), and it is what a COMPOSITOR waits
// on: it makes the guest's queue idle and then advances a serial, so past that
// serial the picture in the eye MTLTexture is complete rather than merely
// submitted. `kl_vulkan_frame_serial` is the value — monotonic, 0 before the
// first frame, and exactly the role kl_glfb_gpu_fence_value plays on the GL
// path (where the wait is on the GPU instead). See the implementation for why
// this one is a CPU stall and what would remove it.
void kl_vulkan_frame_done(int stage);
unsigned long long kl_vulkan_frame_serial(void);

// ---------------------------------------------------------------------------
// VPKTAA INLINE — the resolve runs at the guest's frame end, on VPKEngine's own
// Metal queue, instead of asynchronously in the compositor.
//
// VPKEngine (VPVulkan) jitters the eye geometry's VIEWPORT when it translates a
// submit and records what it applied (vpvk_taa_*); Klepton calls a hook (set
// by the Swift side) inside kl_vulkan_frame_done, after the eye depth is filed
// and BEFORE the frame-end vkQueueWaitIdle — so the resolve, encoded into a
// command buffer on the same queue, is ordered after the frame and covered by
// the same wait. Its output is published as the stage's eye picture.
//
// Every function here is safe without VPVulkan (MoltenVK, or no Vulkan at all):
// the vpvk_* exports are resolved lazily from the loaded driver and every
// answer is then 0 / NULL. The async path is untouched when inline is off.

// 1 when inline mode is active: KL_VPKTAA=1, KL_VPKTAA_INLINE != 0 (default
// 1), a Vulkan guest, and VPVulkan's vpvk_native_submit resolves. Decided
// (and logged) once, the first time it is asked with a Vulkan guest up.
int  kl_vulkan_taa_inline(void);
// The resolve hook. Called on the guest's frame thread from
// kl_vulkan_frame_done(stage) — only in inline mode, only when kl_ovrp filed
// THIS frame's record under `stage` (kl_ovrp_stage_render_pose(stage) then
// returns it, complete, with its serial) and with this frame's eye depth
// filed (kl_vulkan_eye_depth_get(stage, eye, 0, ...)). fn = NULL unregisters.
typedef void (*kl_vulkan_taa_hook_fn)(int stage, void *user);
void kl_vulkan_set_taa_hook(kl_vulkan_taa_hook_fn fn, void *user);
// Forwards to vpvk_native_submit: a command buffer on the queue VPKEngine sends
// all guest work to, after everything sent so far; fn encodes into it
// (id<MTLCommandBuffer>, unretained), then it is committed. Synchronous.
// 1 = committed, 0 = no VPVulkan / no queue.
int  kl_vulkan_native_submit(void (*fn)(void *mtl_cmdbuf, void *user), void *user);
// vpvk_native_device: id<MTLDevice>, unretained. NULL without VPVulkan.
void *kl_vulkan_native_device(void);
// The guest's LIVE eye colour for (stage, eye) — the image it rendered, never
// the VPKTAA output published over it. id<MTLTexture> (borrowed); *slice is
// the array layer holding `eye` (Array/multiview layout), 0 under Stereo.
void *kl_vulkan_eye_color_mtl(int stage, int eye, int *slice, int *w, int *h);
// Publish `tex` (2DArray, 2 layers: slice 0 = left, 1 = right, the eye's size
// and format) as `stage`'s eye picture for every compositor (kl_glfb's eye
// table, as an override over the live entry). tex = NULL restores the live
// image. The override is dropped by itself when the stage's live eye image
// changes (reallocation, size), and refused when w x h is not the eye's size.
void kl_vulkan_taa_publish(int stage, void *tex, int w, int h);
// The jitter VPKEngine actually applied to the frame that is ending
// (vpvk_taa_get_applied, which resets its counters — call once per frame).
// Returns 1 when ≥1 draw was jittered, else 0; the out-parameters (any may be
// NULL) are filled either way (zeros without VPVulkan). jx/jy in pixels of
// the jittered target (+x right, +y down), target_w/h its size.
int  kl_vulkan_taa_applied(float *jx, float *jy, int *target_w, int *target_h,
                           unsigned *jittered, unsigned *skipped, unsigned *mixed);
// kl_ovrp, right BEFORE kl_vulkan_frame_done: the frame about to finish was
// filed as the record of `stage` with `serial` (stage -1 / serial 0 = not
// filed: dropped, or a path that files after the frame end). The hook only
// runs when this names the stage kl_vulkan_frame_done is called with.
void kl_vulkan_taa_frame_record(int stage, unsigned long long serial);

// How many frames have been presented, and how many were written out. The
// end-of-run report reads these; a run with a healthy frame loop and zero
// captures is a different bug from one that never presented.
void  kl_vulkan_stats(unsigned *presented, unsigned *captured);

// ---------------------------------------------------------------------------
// The OpenXR seam, for kl_openxr.c — XR_KHR_vulkan_enable
// ---------------------------------------------------------------------------
//
// Open Brush is the first guest at the junction of this project's two graphics
// halves: it speaks OPENXR (kl_openxr.c, which until now had only ever carried
// GLES guests) and its renderer is VULKAN (this file, which until now had only
// ever been reached through OVRPlugin). Neither half needed the other, so
// neither knew about it.
//
// The split below is the same one kl_openxr already keeps with kl_glfb: the XR
// file states WHAT it needs in OpenXR's vocabulary and this file answers it in
// Vulkan's, so kl_openxr.c includes no Vulkan header and stays linkable in a
// build with no MoltenVK vendored. Every Vulkan handle crossing this boundary is
// a `void *` because a dispatchable Vulkan handle IS a pointer on LP64 — the
// same measured property that makes the forwarding table upstairs possible.
//
// **Under XR_KHR_vulkan_enable the APP creates the VkInstance and VkDevice**,
// not the runtime (that inverts in `_enable2`), and it creates them through the
// synthetic loader in this file. So by the time the graphics binding arrives, a
// guest's device is already one klvk_CreateDevice interposed on — which is what
// makes the two extension lists below honestly empty.

// Is there a Vulkan device the OpenXR half could bind a session to? This gates
// whether XR_KHR_vulkan_enable is ADVERTISED at all: naming an extension we
// cannot back is the one thing the extension table upstairs refuses to do, and
// a checkout with no MoltenVK must fail the guest's xrCreateInstance rather
// than fail later, deeper, on a NULL entry point.
int kl_vulkan_xr_supported(void);

// The VkInstance / VkDevice extensions an app MUST enable for its Vulkan
// objects to be usable by this runtime, space-delimited as the spec wants them.
//
// Both are EMPTY, and that is a true answer rather than an unimplemented one:
// the only extension this runtime needs on the device is VK_EXT_metal_objects
// (how an eye VkImage's MTLTexture reaches the compositor), and klvk_CreateDevice
// already adds it to every device the guest creates. Requiring it of the app as
// well would be asking for something we have already taken.
const char *kl_vulkan_xr_instance_extensions(void);
const char *kl_vulkan_xr_device_extensions(void);

// The VkPhysicalDevice this runtime requires the app to render with, chosen
// from the app's OWN VkInstance (which is the handle the spec passes, and not
// necessarily one this file has seen). NULL when it cannot be answered.
void *kl_vulkan_xr_physical_device(void *vk_instance);

// The Vulkan API version range a session may be created against, as major/minor
// pairs. Answered from the physical device rather than asserted — MoltenVK's
// instance-level and device-level versions differ, and the one that governs an
// app's device is the device's.
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor);

// One OpenXR swapchain image, as a VkImage handle widened to 64 bits — which is
// exactly what XrSwapchainImageVulkanKHR carries. `vk_format` is a VkFormat, not
// a GL internal format: the two lists are different vocabularies for the same
// question and kl_openxr keeps one per graphics API. 0 on failure.
//
// Unlike kl_vulkan_eye_image this is not keyed on (stage, eye), because at
// xrCreateSwapchain time nothing knows which swapchain is an eye — see the
// implementation.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth);

// ...and the MTLTexture behind one, for the compositor seam. NULL when it cannot
// be exported.
void *kl_vulkan_xr_image_mtl(unsigned long long image);

#endif
