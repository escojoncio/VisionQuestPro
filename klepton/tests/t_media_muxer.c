// AMediaMuxer_* of kl_mediandk.c (KL-TEST muxer block + its table rows, cut out at build time).
// Gundam's libAVProMovieCaptureNative links all seven strongly; recording is unsupported, so they must exist and fail cleanly.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

enum { AMEDIA_OK = 0, AMEDIA_ERROR_UNSUPPORTED = -10002 };
typedef struct { int32_t offset, size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
#define KLM_FIRST(flagvar, ...) do { static int flagvar; if (!flagvar++) fprintf(stderr, "  [media] " __VA_ARGS__); } while (0)

#include "muxer.inc"
#define M(sym, fn) {sym, (void *)(fn)}
static const struct { const char *name; void *fn; } tab[] = {
#include "muxer_tab.inc"
};

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
static void *find(const char *n) {
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].name, n)) return tab[i].fn;
    return NULL;
}

int main(void) {
    static const char *const names[] = { "AMediaMuxer_new", "AMediaMuxer_delete", "AMediaMuxer_addTrack",
        "AMediaMuxer_start", "AMediaMuxer_stop", "AMediaMuxer_writeSampleData", "AMediaMuxer_setOrientationHint" };
    CHECK(sizeof tab / sizeof tab[0] == 7, "seven rows in the table");
    for (size_t i = 0; i < 7; i++) CHECK(find(names[i]), names[i]);

    void *(*nw)(int, int) = find("AMediaMuxer_new");
    int (*del)(void *) = find("AMediaMuxer_delete");
    ssize_t (*add)(void *, const void *) = find("AMediaMuxer_addTrack");
    int (*start)(void *) = find("AMediaMuxer_start"), (*stop)(void *) = find("AMediaMuxer_stop");
    int (*wr)(void *, size_t, const uint8_t *, const AMediaCodecBufferInfo *) = find("AMediaMuxer_writeSampleData");
    int (*hint)(void *, int) = find("AMediaMuxer_setOrientationHint");

    CHECK(nw(3, 0) == NULL, "new -> NULL");
    CHECK(del(NULL) == AMEDIA_OK, "delete(NULL) is a no-op");
    CHECK(add(NULL, NULL) == AMEDIA_ERROR_UNSUPPORTED, "addTrack unsupported");
    CHECK(start(NULL) == AMEDIA_ERROR_UNSUPPORTED, "start unsupported");
    CHECK(stop(NULL) == AMEDIA_ERROR_UNSUPPORTED, "stop unsupported");
    CHECK(wr(NULL, 0, NULL, NULL) == AMEDIA_ERROR_UNSUPPORTED, "writeSampleData unsupported");
    CHECK(hint(NULL, 90) == AMEDIA_ERROR_UNSUPPORTED, "setOrientationHint unsupported");
    if (fails) return 1;
    printf("t_media_muxer ok\n");
    return 0;
}
