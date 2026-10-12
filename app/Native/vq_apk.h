// VisionQuestPro: reading a game's APK on the headset.
//
// A ZIP reader (central directory, stored and deflated entries, ZIP64 sizes)
// and a decoder of Android's binary XML (AndroidManifest.xml inside an APK) to
// plain text XML — the form Klepton's runtime reads beside assets/.
#ifndef VQ_APK_H
#define VQ_APK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VqZip VqZip;

// Opens the ZIP at `path`. NULL on error (reason in `err`).
VqZip *vq_zip_open(const char *path, char *err, size_t err_len);
void vq_zip_close(VqZip *z);
// Number of entries, and entry `i`'s name (NUL-terminated, owned by `z`) and
// uncompressed size.
int vq_zip_count(const VqZip *z);
const char *vq_zip_name(const VqZip *z, int i);
uint64_t vq_zip_size(const VqZip *z, int i);
// Index of the entry called `name`, or -1.
int vq_zip_find(const VqZip *z, const char *name);
// Extracts entry `i` to the file `out` (created or truncated). 0 on success,
// -1 on error (reason in `err`). The CRC-32 is checked.
int vq_zip_extract(VqZip *z, int i, const char *out, char *err, size_t err_len);
// Reads entry `i` into a malloc'd buffer (caller frees). Refuses entries larger
// than `max`. NULL on error.
uint8_t *vq_zip_read(VqZip *z, int i, size_t max, size_t *len, char *err, size_t err_len);

// Android binary XML (AXML) to text XML, malloc'd and NUL-terminated (caller
// frees). Attribute values that are resource references come out as "@0x7f..."
// and typed values in decimal / true / false. NULL if `data` is not AXML.
char *vq_axml_to_text(const uint8_t *data, size_t len);

// The in-process klepton-ld (klepton/tools/klepton_ld.c built with
// -DKLEPTON_LD_INPROC): ELF .so -> Mach-O dylib for visionOS with an empty
// LC_CODE_SIGNATURE. 0 on success, -1 and the reason in `err`.
int klepton_ld_run(const char *in, const char *out, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif
