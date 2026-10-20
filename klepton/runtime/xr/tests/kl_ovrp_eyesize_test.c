// Tamaño de ojo del slider de resolución (kl_ovrp_eyesize.h). Desde runtime/xr:
// gcc -Wall -I. tests/kl_ovrp_eyesize_test.c -lm && ./a.out
#include <stdio.h>
#include <math.h>
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

    // desc: a game scale above 1 is cancelled, one below 1 is kept
    int tw = 2383, th = 1912;
    kles_desc_size(tw, th, 1.0f, 1.0f, 1.0f, &w, &h);  CK(w == tw && h == th);
    kles_desc_size(tw, th, 1.3f, 1.0f, 1.0f, &w, &h);  CK(w == tw && h == th);            // AC: no 1.3x on top
    kles_desc_size(tw, th, 0.8f, 1.0f, 1.0f, &w, &h);  CK(w == 1906 && h == 1530);       // the game asks for fewer pixels
    kles_desc_size(tw, th, 0.0f, 1.0f, 1.0f, &w, &h);  CK(w == tw && h == th);            // garbage scale
    kles_desc_size(tw, th, NAN, 1.0f, 1.0f, &w, &h);   CK(w == tw && h == th);
    kles_desc_size(tw, th, 1.0f, 1.25f, 1.0f, &w, &h); CK(w == 1906 && h == th);
    kles_desc_size(tw, th, 1.0f, 0.5f, NAN, &w, &h);   CK(w == tw && h == th);            // widening < 1 ignored
    kles_desc_size(1, 1, 1.0f, 1.6f, 1.6f, &w, &h);    CK(w == 1 && h == 1);              // never 0

    // learn widening: the AC sequence from the log (desc 2383x1912 -> layer 2976x1912)
    float ww = 1.0f, wh = 1.0f;
    kles_learn_widen(2383, 1912, 2976, 1912, &ww, &wh);
    CK(NEAR(ww, 2976.0 / 2383.0, 1e-4) && wh == 1.0f);
    float w1 = ww;
    kles_learn_widen(0, 0, 2976, 1912, &ww, &wh);      CK(ww == w1);                      // nothing to compare
    kles_learn_widen(1000, 1000, 5000, 1000, &ww, &wh); CK(ww == w1);                     // guest's own size: kept
    kles_learn_widen(2000, 1000, 2000, 1000, &ww, &wh); CK(ww == 1.0f && wh == 1.0f);     // no widening (Batman)
    kles_learn_widen(2000, 1000, 2030, 1000, &ww, &wh); CK(ww == 1.0f);                   // < 2 %: rounding

    // end to end, AC: final layer never above the chosen size once the widening is known
    {
        const float plugin = 2976.0f / 2383.0f;        // what the plugin does to the width
        int dw, dh, fw, fh;
        ww = wh = 1.0f;
        kles_desc_size(tw, th, 1.0f, ww, wh, &dw, &dh);                       // first layer (scale 1.0)
        fw = (int)(dw * plugin + 0.5f); fh = dh;
        kles_learn_widen(dw, dh, fw, fh, &ww, &wh);
        CK(fw == 2976 && fh == 1912);                  // first layer: widening not known yet (documented)
        kles_desc_size(tw, th, 1.3f, ww, wh, &dw, &dh);                       // rebuilt with the game's 1.3
        fw = (int)(dw * plugin + 0.5f); fh = dh;
        CK(NEAR(fw, tw, 2) && fh == th);               // 2383x1912, not 3840x2485
        CK((double)fw * fh <= (double)tw * th * 1.002);
    }

    // OpenXR: the guest scale over the recommendation is learned and divided out
    {
        int rw, rh;
        kles_xr_recommended(tw, th, 1.0f, &rw, &rh);   CK(rw == tw && rh == th);
        float s = kles_learn_xr_scale(rw, rh, 3097, 2485, 1.0f);              // created at 1.3x
        CK(NEAR(s, 1.3, 0.001));
        kles_xr_recommended(tw, th, s, &rw, &rh);
        CK(NEAR(rw * s, tw, 2) && NEAR(rh * s, th, 2));
        CK(kles_learn_xr_scale(rw, rh, rw * 2, rh, s) == s);                  // side-by-side: ignored
        CK(kles_learn_xr_scale(rw, rh, 512, 512, s) == s);                    // a UI quad: ignored
        CK(kles_learn_xr_scale(rw, rh, rw * 10, rh * 10, s) == s);            // absurd: ignored
        CK(kles_learn_xr_scale(rw, rh, rw, rh, s) == 1.0f);                   // the game kept our size
        CK(kles_learn_xr_scale(0, 0, 100, 100, 1.2f) == 1.2f);
        kles_xr_recommended(tw, th, 0.3f, &rw, &rh);   CK(rw == tw && rh == th);   // scale < 1 ignored
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

    // widening learning cannot run away: a guest that re-creates the layer many times with the
    // same plugin widening converges at once; a mismeasured ratio is bounded to 1..1.6 and is
    // re-measured (not accumulated) at the next layer, so the desc never shrinks past 1/1.6.
    {
        float aw = 1.0f, ah = 1.0f;
        const float plugin = 1.249f;
        int dw, dh, fw = 0, fh = 0, last = 0;
        for (int i = 0; i < 50; i++) {
            kles_desc_size(2383, 1912, 1.3f, aw, ah, &dw, &dh);
            fw = (int)(dw * plugin + 0.5f); fh = dh;
            kles_learn_widen(dw, dh, fw, fh, &aw, &ah);
            if (i >= 2) CK(fw == last);                  // fixed point after the first two cycles
            last = fw;
            CK(aw >= 1.0f && aw <= 1.6f && ah == 1.0f && dw >= 1 && dh >= 1);
        }
        CK(NEAR(fw, 2383, 3));
        // worst mismeasure (ratio 1.6) then a clean layer: back to no widening, no residue
        aw = ah = 1.0f;
        kles_learn_widen(1000, 1000, 1600, 1000, &aw, &ah);  CK(aw == 1.6f);
        kles_desc_size(2383, 1912, 1.0f, aw, ah, &dw, &dh);  CK(dw >= 2383 / 1.6 - 1);
        kles_learn_widen(dw, dh, dw, dh, &aw, &ah);          CK(aw == 1.0f);
        // NaN / inf learned inputs never poison the state
        aw = 1.3f; kles_learn_widen(2000, 1000, 0, 0, &aw, &ah); CK(aw == 1.3f);
        kles_desc_size(2383, 1912, 1.0f, INFINITY, 0.0f, &dw, &dh); CK(dw >= 1 && dh == 1912);
    }

    // OpenXR: repeated swapchain cycles converge; the recommendation never reaches 0
    {
        int rw, rh, cw, ch;
        float s = 1.0f;
        for (int i = 0; i < 20; i++) {
            kles_xr_recommended(tw, th, s, &rw, &rh);
            cw = (int)(rw * 1.3f + 0.5f); ch = (int)(rh * 1.3f + 0.5f);
            s = kles_learn_xr_scale(rw, rh, cw, ch, s);
            CK(s >= 1.0f && s <= 4.0f && rw >= 1 && rh >= 1);
        }
        CK(NEAR(s, 1.3, 0.01));
        kles_xr_recommended(tw, th, s, &rw, &rh);
        CK(NEAR(rw * 1.3, tw, 3) && NEAR(rh * 1.3, th, 3));   // game's 1.3x lands on the target
        kles_xr_recommended(1, 1, 4.0f, &rw, &rh);  CK(rw == 1 && rh == 1);
        kles_xr_recommended(tw, th, NAN, &rw, &rh); CK(rw == tw && rh == th);
        CK(kles_learn_xr_scale(rw, rh, 1, 1, 1.5f) == 1.5f);   // 1x1 swapchain: ignored
        CK(kles_learn_xr_scale(rw, rh, (int)(rw * 4.0f), (int)(rh * 4.0f), 1.0f) >= 3.99f);   // upper edge accepted
    }

    puts(f ? "FALLA" : "OK");
    return f;
}
