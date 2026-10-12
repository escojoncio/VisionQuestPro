// VisionQuestPro: harvesting a game's Vulkan SPIR-V modules at preparation time.
//
// Walks the game folder (APK/OBB zips, Unity bundles, loose files), unpacks UnityFS
// archives (LZ4/LZ4HC blocks; LZMA blocks are skipped and logged), finds every SPIR-V
// module (magic + header + validated instruction walk) and writes each unique one to
// `out_dir/<hash>.spv`. VPVulkan's vpvk_precompile_spirv() then compiles them before
// the game ever runs.
#ifndef VQ_HARVEST_H
#define VQ_HARVEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VqHarvestStats {
  uint32_t files;      // files / archive entries scanned
  uint32_t bundles;    // UnityFS archives unpacked
  uint32_t lzma_skip;  // LZMA blocks skipped
  uint32_t spirv;      // modules found (with duplicates)
  uint32_t unique;     // modules written
  double ms;
} VqHarvestStats;

typedef void (*vq_harvest_progress)(uint32_t done, uint32_t total, void *user);

// Scans `game_dir` (recursively) and writes unique modules to `out_dir` (created).
// Returns 0 on success, -1 on error (reason in `err`). Logs
// "vq.harvest files=N spirv=N unique=N ms=..." through `log` when given.
int vq_harvest_spirv(const char *game_dir, const char *out_dir, VqHarvestStats *stats,
                     vq_harvest_progress progress, void *user,
                     void (*log)(const char *line, void *user), char *err, size_t err_len);

// Building blocks (exposed for tests).
// LZ4 block decode; returns bytes written (must equal dst_len) or -1.
long vq_lz4_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);
// Length in bytes of the SPIR-V module at `p` (0 if not a plausible module).
size_t vq_spirv_length(const uint8_t *p, size_t avail);
// Decompressed payload of a UnityFS archive (malloc'd, caller frees) or NULL.
uint8_t *vq_unityfs_unpack(const uint8_t *b, size_t n, size_t *out_len, uint32_t *lzma_skipped);

#ifdef __cplusplus
}
#endif
#endif
