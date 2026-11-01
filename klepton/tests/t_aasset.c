// Host test (Linux or macOS) of AAsset_read / AAsset_getBuffer error semantics in kl_ndk.c, cut out by KL-TEST markers.
//   make aasset
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct { FILE *f; int64_t len; void *buf; char path[1200]; } kl_asset;
#include "aasset.inc"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static kl_asset *open_asset(const char *path, int64_t len) {
    kl_asset *a = calloc(1, sizeof *a);
    a->f = fopen(path, "rb"); a->len = len;
    return a;
}
int main(void) {
    char path[] = "/tmp/t_aassetXXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, "0123456789", 10) == 10);

    // EOF is 0, not an error.
    kl_asset *a = open_asset(path, 10);
    char b[16];
    CHECK(kl_AAsset_read(a, b, 10) == 10);
    CHECK(kl_AAsset_read(a, b, 10) == 0);
    CHECK(kl_AAsset_read(a, b, 10) == 0);
    CHECK(kl_AAsset_read(NULL, b, 1) == -1);
    fclose(a->f); a->f = NULL;

    // A read error is -1 (a write-only stream fails every fread), and clears the error flag.
    int wfd = open(path, O_WRONLY);
    a->f = fdopen(wfd, "w");
    CHECK(kl_AAsset_read(a, b, 4) == -1);
    CHECK(!ferror(a->f));
    fclose(a->f); free(a);

    // Complete read: buffer is cached, NUL terminated, same pointer twice.
    a = open_asset(path, 10);
    const char *p = kl_AAsset_getBuffer(a);
    CHECK(p && memcmp(p, "0123456789", 10) == 0 && p[10] == 0);
    CHECK(kl_AAsset_getBuffer(a) == p);
    fclose(a->f); free(a->buf); free(a);

    // Truncated after open: short read -> NULL, nothing cached, file position kept.
    a = open_asset(path, 10);
    CHECK(fread(b, 1, 3, a->f) == 3);
    CHECK(ftruncate(fd, 4) == 0);
    CHECK(kl_AAsset_getBuffer(a) == NULL);
    CHECK(a->buf == NULL);
    CHECK(ftello(a->f) == 3);
    fclose(a->f); free(a);

    close(fd); unlink(path);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("t_aasset: ok");
    return 0;
}
