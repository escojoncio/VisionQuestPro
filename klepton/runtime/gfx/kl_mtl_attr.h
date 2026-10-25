// Live Metal objects made by klepton and the Swift app, by creation site: the part of
// MTLDevice.currentAllocatedSize the heartbeat's "[hb] gpu" counters (VPKEngine's own) do not cover.
// Each site passes its tag ("cp.eye", "env.depth"); the object goes into a weak table, so nothing hooks
// its release: kl_mtl_attr_print walks the tables and adds what is still alive. Any thread; creation is O(1).
//   kl_mtl_track        MTLBuffer / MTLTexture: summed by allocatedSize; >= 32 MiB logs "[mtl] bigalloc" at once.
//   kl_mtl_track_count  views (share their base's memory), functions: counted, never summed.
//   kl_mtl_track_obj    libraries / pipelines (no allocatedSize): counted, plus the growth of
//                       currentAllocatedSize since `before` (= kl_mtl_attr_probe(device) taken before the creation;
//                       cumulative per tag, approximate).
// Compiled with or without ARC (the xros archive is built without).
#ifndef KL_MTL_ATTR_H
#define KL_MTL_ATTR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __OBJC__
#define KL_ATTR_OBJ id
#else
#define KL_ATTR_OBJ void *
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint64_t kl_mtl_attr_probe(KL_ATTR_OBJ device);
void kl_mtl_track(const char *tag, KL_ATTR_OBJ obj);
void kl_mtl_track_count(const char *tag, KL_ATTR_OBJ obj);
void kl_mtl_track_obj(const char *tag, KL_ATTR_OBJ obj, uint64_t before);

// One line "<prefix> gpu.attr total=..M tracked=..M (vpk=..M kl=..M) untracked=..M devs=N kl_top=tag:MiB/live,..
// kl_obj=tag:~MiB/live kl_cnt=tag:live vpk_top=.. vpk_obj=.. vpk_cnt=.." on stderr. `vpk` = the text of
// kl_vulkan_mem_stats (NULL: asked here); total = currentAllocatedSize, untracked = total - both registries.
void kl_mtl_attr_print(const char *prefix, const char *vpk);

#ifdef __cplusplus
}
#endif
#endif
