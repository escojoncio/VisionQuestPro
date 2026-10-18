// libandroid beyond the original kl_ndk.c surface: the Quest 3's AConfiguration,
// AChoreographer, AThermal, APerformanceHint, AStorageManager/AObb, ASharedMemory
// and the AInput*/ANativeActivity accessors. kl_ndk.c delegates to this table for
// any name it does not own, so a lookup still answers NULL for what is not served
// (the unresolved-import report keeps working).
#ifndef KL_ANDROID_H
#define KL_ANDROID_H

void *kl_android_lookup(const char *name);

// AChoreographer callbacks are delivered on the looper thread that asked for the
// instance, by the looper itself, exactly like an fd callback. kl_ndk.c's poll
// calls this before and after waiting; it runs whatever frame callbacks the
// calling thread has due and returns how many ran (0 when none).
int kl_android_choreo_dispatch(void);
#endif
