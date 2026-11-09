// The whole C surface the Swift app sees.
//
// Swift owns the platform layer, C owns the guest, and the
// seam between them is a plain C header — no Objective-C anywhere. That is not
// an aesthetic choice, it is what keeps the boundary narrow: everything below
// is four functions, and the guest-facing complexity (JNIEnv, jobjects, ELF
// images) never crosses into Swift at all.
#import "kl_app.h"
// The compositor widens the seam by three headers, and only for the compositor: kl_glfb
// owns the eye-texture provider (the MTLTexture the guest renders into),
// kl_ovrp owns the pose-in seam ARKit answers and the stage-keyed record of
// what each frame was rendered with, and kl_reproject owns the composite pass
// that consumes that record — shader and matrices both, shared with the macOS
// viewer so the picture is only ever debugged once. Everything guest-facing
// still stays on the C side.
#import "kl_glfb.h"
#import "kl_ovrp.h"
#import "kl_openxr.h"
#import "kl_passthrough.h"
#import "kl_reproject.h"
#import "kl_reproject_ptw.h"
// ...and by one more for audio: kl_audio owns the CoreAudio output unit, but
// the AVAudioSession it needs on this platform can only be configured from
// Swift (KleptonAudio.swift). Three calls cross: the session's measured rate
// going in, and the two ways the OS takes the stream away coming back.
#import "kl_audio.h"
// ...and one for host video (NativeVideoPlayer cutscenes): kl_hostvideo.
#import "kl_hostvideo.h"
// ...and by two for the flat guest's window. kl_present says WHAT SHAPE
// of picture the guest is producing — one flat image wants a WindowGroup, an
// eye pair wants the ImmersiveSpace — and kl_mono is what a window then talks
// to: the newest presented frame out, the pointer and the keyboard in. Both are
// shared with kl_view.c, so the host viewer and this app cannot disagree about
// what a click is.
#import "kl_present.h"
#import "kl_mono.h"
// ...and by one for the VULKAN guest's frame seam. The compositor needs one
// thing from it and nothing else: WHEN a Vulkan guest's eye picture is
// complete. On the GL path that is an MTLSharedEvent kl_glfb signals from
// inside ANGLE's command stream; there is no such event on this path, so
// kl_vulkan publishes a serial instead and the compositor reads it exactly
// where it reads the fence value. The eye TEXTURE still arrives through
// kl_glfb's own table, so nothing else about this seam widens.
#import "kl_vulkan.h"
// ...and the background gate: the scene phase goes in (KleptonApp.swift), the guest's submit paths block on it.
#import "kl_bg.h"
// VisionQuestPro: the launcher's C side (APK reader, in-process klepton-ld) and
// VPKEngine's signature (runtime/vp_codesign.h, vp_pkcs12.h, from VPKEngine at build time).
#import "vq_apk.h"
#import "vq_config.h"
#import "vq_harvest.h"
#import "vq_scan.h"
#import "vp_codesign.h"
#import "vp_pkcs12.h"
#include <dlfcn.h>
// ...and the spatial store (planes, meshes, rooms, anchors) ARKit feeds: kl_spatial.
#import "kl_spatial.h"
// ...and the Guardian core (floor, play space): KleptonGuardian.swift.
#import "kl_guardian.h"
// ...and the environment depth ring KleptonEnvDepth.swift renders into: kl_envdepth.
#import "kl_envdepth.h"
// ...and the live-Metal-object registry the heartbeat's gpu.attr line reads: kl_mtl_attr.
#import "kl_mtl_attr.h"
// ...the 26 hand joints ARKit's HandTrackingProvider feeds the ovrp hand state (kl_ovrp_input.c).
void kl_ovrp_set_hand_joints(int hand, const float *pose7x26, uint32_t tracked_mask,
                             const float *pinch5, uint32_t pinch_bits);
// ...and which hands have a Sense controller attached (kl_ovrp_input.c): ovrp_GetHandTrackingEnabled.
void kl_ovrp_set_controller_attached(int hand, int attached);
