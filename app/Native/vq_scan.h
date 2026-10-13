// VisionQuestPro: what a game asks of Android, read from its own files before it runs.
//
// The scanner behind «Escáner de APIs»: the dynamic imports and exports of each ARM64 library
// (ELF .dynsym), and the Java class names its code refers to (JNI FindClass strings in the
// libraries, AndroidJavaClass names in IL2CPP's global-metadata.dat). The launcher checks them
// against what Klepton implements (kl_shim_known, kl_jni_knows_class) and logs the gaps.
#ifndef VQ_SCAN_H
#define VQ_SCAN_H

#ifdef __cplusplus
extern "C" {
#endif

// Newline-separated dynamic symbol names of the ELF at `path`, malloc'd (caller frees).
// `defined` 0: undefined imports (with a leading "w " for weak ones); 1: defined exports.
// NULL if the file is not a 64-bit ELF.
char *vq_elf_symbols(const char *path, int defined);

// Newline-separated, de-duplicated Java class names found as strings in any file (ELF or
// metadata), in JNI form ("android/media/MediaCodec"; dotted names are converted, inner
// classes keep '$'). Only the platform families a game can only get from the OS: android/,
// java/, javax/, dalvik/, com/oculus/, com/meta/, com/android/, com/google/android/.
char *vq_java_classes(const char *path);

// Klepton's own answers (libklepton.a; kl_shim.c and kl_jni.c): 1 shim / 2 served family / 0 none,
// and whether any JNI binding or field exists for a class.
int kl_shim_known(const char *name);
int kl_jni_knows_class(const char *cls);

#ifdef __cplusplus
}
#endif
#endif
