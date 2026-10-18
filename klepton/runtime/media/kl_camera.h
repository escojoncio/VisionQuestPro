// libcamera2ndk.so — served as a device with no cameras.
//
// visionOS gives the guest no camera, and Demeo's libUnityARFoundationMeta.so
// imports the whole camera2 NDK surface without needing it. Every entry point
// answers the way an Android device with zero cameras would: the manager
// exists, the id list is empty, anything that wants a device or a session gets
// a state error. Nothing here touches hardware.
#ifndef KL_CAMERA_H
#define KL_CAMERA_H

// ELF-import door: NULL for names this file does not serve, so the unresolved
// report keeps working. Same discipline as kl_aaudio_lookup.
void *kl_camera_lookup(const char *name);

#endif
