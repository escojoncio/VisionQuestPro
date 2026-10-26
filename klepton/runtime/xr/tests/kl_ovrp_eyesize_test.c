// Tamaño de ojo del slider de resolución (kl_ovrp_eyesize.h). Desde runtime/xr:
// gcc -Wall -I. tests/kl_ovrp_eyesize_test.c -lm && ./a.out
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "kl_ovrp_eyesize.h"

static int f;
#define CK(c) do{ if(!(c)){printf("FAIL %s (line %d)\n",#c,__LINE__);f=1;} }while(0)
#define NEAR(a,b,t) (fabs((double)(a)-(double)(b))<=(t))

int main(void) {
    const float asp = 6261.0f / 5023.0f;   // the Vision Pro request in the AC log
    int w, h;

    // factor: 100 % = Quest 3, 200 % = Vision Pro density, linear per axis between them
    CK(kles_pct_factor(100) == 1.0f);
    CK(NEAR(kles_pct_factor(50), 0.5, 1e-6));
    CK(NEAR(kles_pct_factor(75), 0.75, 1e-6));
    float fa = sqrtf((3660.0f * 3200.0f) / (2064.0f * 2208.0f));
    CK(NEAR(kles_pct_factor(200), fa, 1e-5));
    CK(NEAR(kles_pct_factor(150), (1.0 + fa) / 2.0, 1e-5));
    CK(NEAR(kles_pct_factor(10), 0.5, 1e-6));          // below the range
    CK(NEAR(kles_pct_factor(999), fa, 1e-5));          // above the range
    CK(NEAR(kles_pct_factor(NAN), 1.0, 1e-6));
    CK(kles_pct_factor(101) > kles_pct_factor(100) && kles_pct_factor(100) > kles_pct_factor(99));

    // target: 100 % at the display's aspect is what "original" gave in the log (2383x1912)
    CK(kles_target_size(asp, 100, 0, &w, &h) && w == 2383 && h == 1912);
    CK(NEAR((double)w * h, 2064.0 * 2208.0, 2064.0 * 2208.0 * 0.001));   // same pixel budget as Quest 3
    CK(kles_target_size(asp, 200, 0, &w, &h) && NEAR((double)w * h, 3660.0 * 3200.0, 3660.0 * 3200.0 * 0.001));
    CK(NEAR((float)w / h, asp, 0.002));
    CK(kles_target_size(asp, 150, 0, &w, &h) && NEAR(w, 2383 * kles_pct_factor(150), 1) && NEAR(h, 1912 * kles_pct_factor(150), 1));
    CK(kles_target_size(asp, 50, 0, &w, &h) && NEAR(w, 1192, 1) && NEAR(h, 956, 1));
    CK(kles_target_size(asp, 200, 3072, &w, &h) && w == 3072 && NEAR(h, 3072 / asp, 1));   // cap keeps the aspect
    CK(kles_target_size(asp, 200, KLES_CAP_DFLT, &w, &h) && w < KLES_CAP_DFLT);              // 200 % fits the default cap
    w = h = 7;
    CK(!kles_target_size(0.0f, 100, 0, &w, &h) && w == 7 && h == 7);   // bad aspect: untouched
    CK(!kles_target_size(NAN, 100, 0, &w, &h));
    CK(!kles_target_size(asp, 100, 0, NULL, &h));

    // desc: the guest's scale above 1 is cancelled, one below 1 is kept; the frozen widening is divided out
    int tw = 2383, th = 1912;
    const int C = KLES_CAP_DFLT;
    kles_desc_size(tw, th, 1.0f, 1.0f, 1.0f, C, &w, &h);  CK(w == tw && h == th);
    kles_desc_size(tw, th, 1.3f, 1.0f, 1.0f, C, &w, &h);  CK(w == tw && h == th);        // AC: no 1.3x on top
    kles_desc_size(tw, th, 0.8f, 1.0f, 1.0f, C, &w, &h);  CK(w == 1906 && h == 1530);    // the game asks for fewer pixels
    kles_desc_size(tw, th, 0.0f, 1.0f, 1.0f, C, &w, &h);  CK(w == tw && h == th);        // garbage scale
    kles_desc_size(tw, th, NAN, 1.0f, 1.0f, C, &w, &h);   CK(w == tw && h == th);
    kles_desc_size(tw, th, 1.0f, 1.25f, 1.0f, C, &w, &h); CK(w == 1906 && h == th);
    kles_desc_size(tw, th, 1.0f, 0.5f, NAN, C, &w, &h);   CK(w == tw && h == th);        // widening < 1 ignored
    kles_desc_size(1, 1, 1.0f, 1.6f, 1.6f, C, &w, &h);    CK(w == 1 && h == 1);          // never 0
    kles_desc_size(3660, 3200, 1.0f, 1.0f, 1.0f, 3072, &w, &h);                          // capped, aspect kept
    CK(w == 3072 && NEAR(h, 3200.0 * 3072 / 3660.0, 1));
    kles_desc_size(5000, 4000, 1.0f, 1.0f, 1.0f, 0, &w, &h); CK(w == 5000 && h == 4000);   // cap <= 0: none

    // freeze the widening: the AC sequence from the log (desc 2383x1912 -> layer 2976x1912), once
    {
        float ww = 1.0f, wh = 1.0f; int fz = 0;
        CK(kles_freeze_widen(2383, 1912, 2976, 1912, &fz, &ww, &wh) == 1 && fz == 1);
        CK(NEAR(ww, 2976.0 / 2383.0, 1e-4) && wh == 1.0f);
        float w1 = ww;
        CK(kles_freeze_widen(2000, 1000, 2400, 1000, &fz, &ww, &wh) == 0 && ww == w1);   // frozen: never re-learned
        CK(kles_freeze_widen(2000, 1000, 2000, 1000, &fz, &ww, &wh) == 0 && ww == w1);
        float a1 = 1.0f, b1 = 1.0f; int f1 = 0;
        CK(!kles_freeze_widen(0, 0, 2976, 1912, &f1, &a1, &b1) && !f1);                   // nothing to compare
        CK(!kles_freeze_widen(1000, 1000, 5000, 1000, &f1, &a1, &b1) && !f1);             // guest's own size: not frozen
        CK(!kles_freeze_widen(1000, 1000, 900, 1000, &f1, &a1, &b1) && !f1);
        // ~1.0 stays provisional (Batman / the guest's first layers): widen = 1 is used and a later widened layer freezes
        CK(!kles_freeze_widen(2000, 1000, 2000, 1000, &f1, &a1, &b1) && !f1 && a1 == 1.0f);
        CK(!kles_freeze_widen(2000, 1000, 2030, 1000, &f1, &a1, &b1) && !f1);             // < 2 %: rounding
        CK(kles_freeze_widen(2000, 1000, 2500, 1000, &f1, &a1, &b1) && f1 && NEAR(a1, 1.25, 1e-5) && b1 == 1.0f);
        // the plugin widens the height only: that axis freezes, the other stays 1
        float a2 = 1.0f, b2 = 1.0f; int f2 = 0;
        CK(kles_freeze_widen(2000, 1000, 2000, 1250, &f2, &a2, &b2) && a2 == 1.0f && NEAR(b2, 1.25, 1e-5));
    }

    // OpenXR: the guest's scale over the recommendation is frozen once; then recommended = slider / scale
    {
        int rw, rh, fz = 0;
        float s = 1.0f;
        kles_xr_recommended(2383, 1912, s, &rw, &rh);            CK(rw == 2383 && rh == 1912);
        CK(!kles_freeze_xr_scale(rw, rh, rw, rh, &fz, &s) && !fz && s == 1.0f);          // the game kept our size: provisional
        CK(!kles_freeze_xr_scale(rw, rh, 2400, 1912, &fz, &s) && !fz);                    // not eye-shaped (side by side...)
        CK(!kles_freeze_xr_scale(rw, rh, 512, 512, &fz, &s) && !fz);                      // a UI quad
        CK(!kles_freeze_xr_scale(rw, rh, rw * 3, rh * 3, &fz, &s) && !fz);                // > 2x: not a scale
        CK(kles_freeze_xr_scale(rw, rh, 3098, 2486, &fz, &s) && fz && NEAR(s, 1.3, 0.001));   // created at 1.3x
        kles_xr_recommended(2383, 1912, s, &rw, &rh);
        CK(NEAR(rw * s, 2383, 2) && NEAR(rh * s, 1912, 2));
        float frozen = s;
        CK(!kles_freeze_xr_scale(rw, rh, rw * 2, rh * 2, &fz, &s) && s == frozen);        // never re-learned
        kles_xr_recommended(2383, 1912, NAN, &rw, &rh);          CK(rw == 2383 && rh == 1912);
        kles_xr_recommended(1, 1, 2.0f, &rw, &rh);               CK(rw == 1 && rh == 1);
    }

    // 100 % is the Quest 3 pixel budget at ANY display aspect; monotonic; aspect kept; never 0
    {
        const float asps[] = {0.5f, 0.8f, 1.0f, 1.2465f, 2.0f, 4.0f};
        for (unsigned i = 0; i < sizeof asps / sizeof asps[0]; i++) {
            int pw = 0, ph = 0, pw0 = 0;
            CK(kles_target_size(asps[i], 100, 0, &w, &h));
            CK(NEAR((double)w * h, 2064.0 * 2208.0, 2064.0 * 2208.0 * 0.002));
            CK(NEAR((double)w / h, asps[i], asps[i] * 0.002));
            for (int pct = 50; pct <= 200; pct++) {
                CK(kles_target_size(asps[i], (float)pct, 0, &pw, &ph));
                CK(pw >= 1 && ph >= 1 && pw >= pw0);       // monotonic in width
                CK(NEAR((double)pw / ph, asps[i], asps[i] * 0.01 + 2.0 / ph));
                pw0 = pw;
            }
            // the default cap keeps the aspect and never exceeds 4096
            CK(kles_target_size(asps[i], 200, KLES_CAP_DFLT, &pw, &ph) && pw <= KLES_CAP_DFLT && ph <= KLES_CAP_DFLT);
            CK(NEAR((double)pw / ph, asps[i], asps[i] * 0.01 + 2.0 / ph));
        }
        CK(!kles_target_size(0.05f, 100, 0, &w, &h) && !kles_target_size(20.0f, 100, 0, &w, &h));   // absurd aspects refused
    }

    // A guest whose plugin quantises the layer width (multiples of 96 over ~1.23x the desc: the
    // log's 2231 -> 2784, 2256 -> 2784, 2281 -> 2880) rebuilt its eye layer on every desc it was
    // handed (29 rebuilds in 7 s). The widening is measured once and frozen: at most 2 descs.
    {
        #define QUANT(dw) (((int)ceilf((float)(dw) * 1.23f / 96.0f)) * 96)
        CK(QUANT(2231) == 2784 && QUANT(2256) == 2784 && QUANT(2281) == 2880);   // the model fits the log

        // The old feedback (re-learned every layer): the model must reproduce the loop (>= 3 descs).
        int legacy[64], nl = 0;
        float aw = 1.0f;
        for (int i = 0; i < 30; i++) {
            int dw = (int)((float)tw / aw + 0.5f), seen = 0;
            for (int k = 0; k < nl; k++) seen |= legacy[k] == dw;
            if (!seen) legacy[nl++] = dw;
            float r = (float)QUANT(dw) / (float)dw;
            if (r >= 0.98f && r <= 1.6f) aw = r < 1.02f ? 1.0f : r;
        }
        CK(nl >= 3);

        // First layers at ~1.0 (a plugin that has not widened yet), then widened ones: the widening
        // freezes at the first widened layer, the descs before it are 1.0-based, at most 2 distinct.
        {
            int descs[64], nd = 0, lw = 0;
            float ww = 1.0f, wh = 1.0f; int fz = 0;
            for (int i = 0; i < 30; i++) {
                int dw, dh, seen = 0;
                kles_desc_size(tw, th, 1.0f, ww, wh, KLES_CAP_DFLT, &dw, &dh);
                for (int k = 0; k < nd; k++) seen |= descs[k] == dw;
                if (!seen) descs[nd++] = dw;
                lw = i < 3 ? dw : QUANT(dw);          // layers 0..2 built exactly at the desc, the rest widened
                (void)kles_freeze_widen(dw, dh, lw, dh, &fz, &ww, &wh);
            }
            CK(nd <= 2 && fz == 1);
            CK(abs(lw - tw) <= 96);
        }

        const float scales[] = { 1.0f, 1.3f };
        for (unsigned si = 0; si < 2; si++) {
            int descs[64], nd = 0, lw = 0;
            float ww = 1.0f, wh = 1.0f; int fz = 0;
            for (int i = 0; i < 30; i++) {
                int dw, dh, seen = 0;
                kles_desc_size(tw, th, scales[si], ww, wh, KLES_CAP_DFLT, &dw, &dh);
                for (int k = 0; k < nd; k++) seen |= descs[k] == dw;
                if (!seen) descs[nd++] = dw;
                lw = QUANT(dw);
                (void)kles_freeze_widen(dw, dh, lw, dh, &fz, &ww, &wh);
            }
            CK(nd <= 2);
            CK(fz == 1);
            CK(abs(lw - tw) <= 96);                  // final layer ~ the slider, within one quantisation step
        }
    }

    puts(f ? "FALLA" : "OK");
    return f;
}
