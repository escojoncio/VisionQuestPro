// INFO_OUTPUT_FORMAT_CHANGED sequencing of kl_mediandk.c (klm_fmt_announce, cut out at build time).
// Surface mode must announce the size BEFORE the first frame, once per size change; the frame is held
// and handed out by the next dequeue. The tiny model below mirrors klm_AMediaCodec_dequeueOutputBuffer's
// held-frame handshake around the real decision function.
#include <stdio.h>

#include "announce.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

enum { CHANGED = -2, FRAME = 1 };
typedef struct { int fw, fh, held, hw, hh; } M;
static int dequeue(M *m, const int (*q)[2], int *qi, int n) {
    int w, h;
    if (m->held) { w = m->hw; h = m->hh; m->held = 0; }
    else if (*qi < n) { w = q[*qi][0]; h = q[*qi][1]; (*qi)++; }
    else return 0;
    if (klm_fmt_announce(&m->fw, &m->fh, w, h)) { m->held = 1; m->hw = w; m->hh = h; return CHANGED; }
    return FRAME;
}

int main(void) {
    const int q[][2] = {{1920, 1080}, {1920, 1080}, {1280, 720}, {1280, 720}};
    M m = {0};
    int qi = 0;
    CHECK(dequeue(&m, q, &qi, 4) == CHANGED && m.fw == 1920 && m.fh == 1080, "first frame announces 1920x1080");
    CHECK(dequeue(&m, q, &qi, 4) == FRAME, "held frame is delivered next");
    CHECK(dequeue(&m, q, &qi, 4) == FRAME, "same size: no new announce");
    CHECK(dequeue(&m, q, &qi, 4) == CHANGED && m.fw == 1280, "size change announces again");
    CHECK(dequeue(&m, q, &qi, 4) == FRAME, "held frame after change");
    CHECK(dequeue(&m, q, &qi, 4) == FRAME, "second 1280x720 frame");
    CHECK(dequeue(&m, q, &qi, 4) == 0, "drained");
    int fw = 0, fh = 0;
    CHECK(!klm_fmt_announce(&fw, &fh, 0, 0), "0x0 never announces");
    CHECK(!klm_fmt_announce(&fw, &fh, -1, 5), "negative never announces");
    CHECK(fw == 0 && fh == 0, "state untouched by invalid sizes");
    if (fails) return 1;
    printf("t_media_announce ok\n");
    return 0;
}
