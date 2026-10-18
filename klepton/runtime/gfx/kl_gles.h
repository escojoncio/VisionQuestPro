// VPKEngine's own GLES 3.2 + EGL (gpu/gles in VPKEngine, exported by VPVulkan as vpgles_*) as the
// guest's GL, instead of the null driver / ANGLE. Gated: only with KL_GLES_VPK=1, which the app sets
// from its "OpenGL ES con VPKEngine (experimental)" setting (off by default). With the gate off
// nothing here runs and kl_egl.c / kl_glfb.c behave exactly as before.
#ifndef KL_GLES_H
#define KL_GLES_H
#include <stdint.h>

// 1 when the gate is on and vpgles came up on VPVulkan (lazy, first call does the work).
int kl_gles_enabled(void);

// The guest's GL/EGL entry point from vpgles, or NULL (the caller keeps its own answer).
void *kl_gles_sym(const char *name);

// OVRPlugin eye/layer storage on the vpgles path: a VkImage of vpgles' device, `layers` array
// slices (2 = both eyes, multiview), wrapped as a GL texture of the current context and published
// to the compositor (kl_glfb_note_eye_mtl_texture) for eyes. Returns the GL name, 0 on failure.
uint32_t kl_gles_layer_texture(int is_eye, int eye, int stage, int w, int h, uint32_t gl_internal_fmt,
                               int layers);

// End of a guest frame (ovrp EndFrame): the GL work of the frame is submitted and complete before
// the compositor samples the eye textures.
void kl_gles_frame_end(void);

#endif
