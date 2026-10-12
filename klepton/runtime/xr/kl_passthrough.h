// kl_passthrough — shared "is the guest showing passthrough this frame" state.
//
// Both XR shims (OVRPlugin, OpenXR) report what the guest submitted each frame;
// the visionOS host polls kl_passthrough_active() to pick .mixed vs .full
// immersion and to composite the eye texture with its alpha instead of opaque.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Mode from KL_PASSTHROUGH: 0 = auto (default), 1 = off (always VR), 2 = on.
int  kl_passthrough_mode(void);

// Called once per submitted frame by a shim. `present` = a live passthrough
// layer is in this frame's layer list (or the frame's blend mode is
// alpha/additive); `layer` = its id/index for the log; `underlay` = it sits
// beneath the eye layer; `unpremultiplied` = the eye layer's alpha is straight
// (XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT), so the host premultiplies.
void kl_passthrough_note_frame(int present, int layer, int underlay, int unpremultiplied);

// Host side. 1 when the guest asked for passthrough on its last frame (or the
// mode forces it), 0 otherwise; mode "off" always answers 0.
int  kl_passthrough_active(void);
// 1 when the eye layer's alpha is straight (unpremultiplied).
int  kl_passthrough_unpremultiplied(void);

#ifdef __cplusplus
}
#endif
