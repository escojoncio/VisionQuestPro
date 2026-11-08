// Host test (Linux or macOS) of the guest low-memory rule and of the [mem] telemetry throttle (runtime/kl_memrule.h).
//   make lowmem        (or tools/ci/host-tests.sh)
#include <stdint.h>
#include <stdio.h>
#include "kl_memrule.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
#define MIB(n) ((uint64_t)(n) << 20)

// Evaluates with the shipping defaults for a process whose real limit is 8190 MiB and which uses `used_mib`.
static kl_lowmem_eval at(unsigned used_mib, unsigned avail_mib) {
    kl_lowmem_eval e;
    uint64_t used = MIB(used_mib), avail = MIB(avail_mib);
    kl_lowmem_eval_levels(used, avail, used + avail, KL_LOWMEM_AT_DEFAULT, KL_LOWMEM_CLEAR_DEFAULT, KL_LOWMEM_CRIT_DEFAULT,
                          KL_LOWMEM_CRIT_CLEAR_DEFAULT, MIB(384), &e);
    return e;
}

static void test_defaults(void) {
    CHECK(KL_LOWMEM_RATE_S_DEFAULT == 0, "the slope trigger is off for the guest by default");
    CHECK(KL_LOWMEM_AT_DEFAULT == 80, "the guest is first told at 80 % of the real limit");
    CHECK(KL_LOWMEM_CRIT_DEFAULT > KL_LOWMEM_AT_DEFAULT, "the second tier is above the first");
    CHECK(KL_LOWMEM_CLEAR_DEFAULT < KL_LOWMEM_AT_DEFAULT && KL_LOWMEM_CRIT_CLEAR_DEFAULT < KL_LOWMEM_CRIT_DEFAULT,
          "each tier re-arms below itself");
}

static void test_levels(void) {
    kl_lowmem_eval e;
    // The 65 % crossing that used to fire (5330 of 8190 MiB) is silence now: the OS has room and says nothing.
    e = at(5330, 2860);
    CHECK(!e.warn_hot && !e.crit_hot, "65 % of the real limit does not warn the guest");
    e = at(6500, 1690);
    CHECK(!e.warn_hot && !e.crit_hot, "79 % does not warn");
    e = at(6560, 1630);
    CHECK(e.warn_hot && !e.crit_hot, "80 % warns");
    e = at(7400, 790);
    CHECK(e.warn_hot && e.crit_hot, "90 % is critical (and implies the warning)");
    // Headroom floor: the OS is about to end the process whatever the percentage says.
    e = at(7700, 380);
    CHECK(e.crit_hot && e.why && !strcmp(e.why, "headroom"), "OS headroom at the floor is critical, reason headroom");
    // Above the headroom floor only the percentage counts. Limit 20000 MiB, floor 384: 16 000 used (80 %) with 4000 free.
    {
        kl_lowmem_eval g;
        kl_lowmem_eval_levels(MIB(15900), MIB(4100), MIB(20000), 80, 70, 90, 80, MIB(384), &g);
        CHECK(!g.warn_hot && !g.crit_hot, "big headroom, 79 % < AT: no warning");
        kl_lowmem_eval_levels(MIB(16000), MIB(4000), MIB(20000), 80, 70, 90, 80, MIB(384), &g);
        CHECK(g.warn_hot && !g.crit_hot, "big headroom, 80 % >= AT: warning, not critical");
        kl_lowmem_eval_levels(MIB(18000), MIB(2000), MIB(20000), 80, 70, 90, 80, MIB(384), &g);
        CHECK(g.crit_hot && g.why && !strcmp(g.why, "footprint"), "big headroom, 90 % >= CRIT: critical by footprint");
    }
    e = at(7700, 390);
    CHECK(e.crit_hot && e.why && !strcmp(e.why, "footprint"), "390 MiB free is above the floor: critical only by the 94 %");
    // Hysteresis: cold only when clearly below.
    e = at(5000, 3190);
    CHECK(e.warn_cold && e.crit_cold, "61 % is cold");
    e = at(6000, 2190);
    CHECK(!e.warn_cold && e.crit_cold, "73 % is not cold for the warning, cold for the critical tier");
    // No OS headroom figure (avail 0): only the percentage of the limit the caller passes (the guest total).
    kl_lowmem_eval f;
    kl_lowmem_eval_levels(MIB(3000), 0, MIB(8192), KL_LOWMEM_AT_DEFAULT, KL_LOWMEM_CLEAR_DEFAULT, KL_LOWMEM_CRIT_DEFAULT,
                          KL_LOWMEM_CRIT_CLEAR_DEFAULT, MIB(384), &f);
    CHECK(!f.warn_hot && !f.crit_hot && f.warn_cold, "no OS figure and 37 % of the budget: nothing");
    kl_lowmem_eval_levels(MIB(7000), 0, MIB(8192), KL_LOWMEM_AT_DEFAULT, KL_LOWMEM_CLEAR_DEFAULT, KL_LOWMEM_CRIT_DEFAULT,
                          KL_LOWMEM_CRIT_CLEAR_DEFAULT, MIB(384), &f);
    CHECK(f.warn_hot, "no OS figure and 85 % of the budget: warning");
    // No footprint at all: unknown, never hot.
    kl_lowmem_eval_levels(0, MIB(100), 0, KL_LOWMEM_AT_DEFAULT, KL_LOWMEM_CLEAR_DEFAULT, KL_LOWMEM_CRIT_DEFAULT,
                          KL_LOWMEM_CRIT_CLEAR_DEFAULT, MIB(384), &f);
    CHECK(f.crit_hot, "headroom below the floor stays critical even without a footprint");
    kl_lowmem_eval_levels(0, 0, 0, KL_LOWMEM_AT_DEFAULT, KL_LOWMEM_CLEAR_DEFAULT, KL_LOWMEM_CRIT_DEFAULT,
                          KL_LOWMEM_CRIT_CLEAR_DEFAULT, MIB(384), &f);
    CHECK(!f.warn_hot && !f.crit_hot, "nothing known, nothing hot");
}

static void test_threshold(void) {
    // ActivityManager.MemoryInfo.threshold: the free bytes below which lowMemory is true = (100 - AT) % of the total.
    CHECK(kl_lowmem_threshold(MIB(8192), KL_LOWMEM_AT_DEFAULT) == MIB(8192) / 100 * 20, "threshold follows the first tier");
    CHECK(kl_lowmem_threshold(MIB(8192), 0) == 0, "AT 0 (off) has no threshold");
}

static void test_sanitize(void) {
    kl_lowmem_eval e;
    // at > 100 used to wrap (100 - at) in the threshold.
    CHECK(kl_lowmem_threshold(MIB(8192), 150) == 0, "AT 150 clamps to 100: threshold 0, no wrap");
    CHECK(kl_lowmem_threshold(MIB(8192), -5) == 0, "negative AT: no threshold");
    CHECK(kl_lowmem_pct(-1) == 0 && kl_lowmem_pct(0) == 0 && kl_lowmem_pct(100) == 100 && kl_lowmem_pct(101) == 100,
          "pct clamp 0..100");
    // Levels with absurd values behave as the clamped ones: 100 % never reached before the limit itself.
    kl_lowmem_eval_levels(MIB(8000), MIB(190), MIB(8190), 500, 400, 900, 800, MIB(384), &e);
    CHECK(e.crit_hot && !strcmp(e.why, "headroom"), "clamped tiers: only the headroom floor fires at 97 %");
    kl_lowmem_eval_levels(MIB(5000), MIB(3190), MIB(8190), 500, 400, 900, 800, MIB(384), &e);
    CHECK(!e.warn_hot && !e.crit_hot && e.warn_cold && e.crit_cold, "clamped tiers: 61 % is silent and cold");
    CHECK(kl_lowmem_floor_bytes(-1) == MIB(384), "negative FREE_MB falls back to the default");
    CHECK(kl_lowmem_floor_bytes(0) == 0 && kl_lowmem_floor_bytes(512) == MIB(512), "FREE_MB 0 and 512 are honoured");
    // The negative floor really is ignored end to end: 300 MiB free is critical with the 384 default.
    kl_lowmem_eval_levels(MIB(7890), MIB(300), MIB(8190), 80, 70, 90, 80, kl_lowmem_floor_bytes(-7), &e);
    CHECK(e.crit_hot, "negative FREE_MB -> default floor 384: 300 MiB free is critical");
}

static void test_hysteresis(void) {
    _Atomic int armed = 1, crit_armed = 1;
    const char *why;
    kl_lowmem_eval e;
    #define STEP(u, a) (e = at((u), (a)), why = e.why, kl_lowmem_step(&armed, &crit_armed, &e, &why))
    CHECK(STEP(5000, 3190) == 0, "hyst: 61 % silent");
    CHECK(STEP(6560, 1630) == 1 && !strcmp(why, "footprint warning"), "hyst: 80 % warns once");
    CHECK(STEP(6600, 1590) == 0, "hyst: still hot, no repeat");
    CHECK(STEP(6000, 2190) == 0 && !armed, "hyst: 73 % is not cold for the warning: stays disarmed");
    CHECK(STEP(6560, 1630) == 0, "hyst: back to 80 % without crossing 70 %: no second warning");
    CHECK(STEP(5000, 3190) == 0 && armed, "hyst: 61 % re-arms the warning");
    CHECK(STEP(6560, 1630) == 1, "hyst: warns again after re-arming");
    CHECK(STEP(7400, 790) == 2 && !strcmp(why, "footprint critical"), "hyst: 90 % is critical once");
    CHECK(STEP(7450, 740) == 0, "hyst: critical does not repeat");
    CHECK(STEP(6700, 1490) == 0 && !crit_armed, "hyst: 82 % keeps the critical tier disarmed");
    CHECK(STEP(6000, 2190) == 0 && crit_armed && !armed, "hyst: 73 % re-arms critical only (below 80 %)");
    CHECK(STEP(7400, 790) == 2, "hyst: critical again after its re-arm");
    CHECK(STEP(5000, 3190) == 0 && armed && crit_armed, "hyst: cold re-arms both");
    CHECK(STEP(7700, 380) == 2 && !strcmp(why, "headroom"), "hyst: headroom floor is critical, reason kept");
    #undef STEP
}

static void test_is_low(void) {
    const int a = KL_LOWMEM_AT_DEFAULT, c = KL_LOWMEM_CLEAR_DEFAULT, k = KL_LOWMEM_CRIT_DEFAULT,
              kc = KL_LOWMEM_CRIT_CLEAR_DEFAULT;
    CHECK(!kl_lowmem_is_low(MIB(5330), MIB(2860), MIB(8190), a, c, k, kc, MIB(384)), "is_low: 65 % is not low");
    CHECK(kl_lowmem_is_low(MIB(6560), MIB(1630), MIB(8190), a, c, k, kc, MIB(384)), "is_low: 80 % is low");
    CHECK(kl_lowmem_is_low(MIB(100), MIB(300), MIB(400), a, c, k, kc, MIB(384)), "is_low: headroom floor is low at 25 %");
    CHECK(!kl_lowmem_is_low(MIB(7000), MIB(1190), MIB(8190), 0, c, k, kc, MIB(384)), "is_low: AT 0 = notifications off");
    CHECK(!kl_lowmem_is_low(0, 0, 0, a, c, k, kc, MIB(384)), "is_low: nothing known is not low");
}

static void test_rate(void) {
    const uint64_t min = MIB(128);
    kl_rate_dec d;
    // Defaults: the host trim follows the slope, the guest is never told.
    d = kl_rate_decide(3.0, MIB(500), min, KL_TRIM_RATE_S_DEFAULT, KL_LOWMEM_RATE_S_DEFAULT);
    CHECK(d.trim == 1 && d.warn == 0, "rate: defaults trim the host without telling the guest");
    d = kl_rate_decide(7.0, MIB(500), min, KL_TRIM_RATE_S_DEFAULT, KL_LOWMEM_RATE_S_DEFAULT);
    CHECK(!d.trim && !d.warn, "rate: 7 s left is outside the 6 s window");
    d = kl_rate_decide(3.0, MIB(100), min, KL_TRIM_RATE_S_DEFAULT, KL_LOWMEM_RATE_S_DEFAULT);
    CHECK(!d.trim && !d.warn, "rate: slope under the minimum");
    d = kl_rate_decide(-1.0, 0, min, KL_TRIM_RATE_S_DEFAULT, KL_LOWMEM_RATE_S_DEFAULT);
    CHECK(!d.trim && !d.warn, "rate: footprint not rising");
    // KL_LOWMEM_RATE_S=0 must not switch the trim off (it governs only the guest notice); KL_TRIM_RATE_S=0 does.
    d = kl_rate_decide(3.0, MIB(500), min, 6, 0);
    CHECK(d.trim && !d.warn, "rate: LOWMEM_RATE_S 0 keeps the host trim");
    d = kl_rate_decide(3.0, MIB(500), min, 0, 0);
    CHECK(!d.trim && !d.warn, "rate: TRIM_RATE_S 0 and LOWMEM_RATE_S 0 is fully off");
    d = kl_rate_decide(3.0, MIB(500), min, 6, 6);
    CHECK(d.trim && d.warn, "rate: LOWMEM_RATE_S 6 also tells the guest");
    d = kl_rate_decide(8.0, MIB(500), min, 6, 10);
    CHECK(!d.trim && d.warn, "rate: a wider guest window can fire without the trim window");
}

static void test_tele(void) {
    kl_tele_state s = {0};
    // First call prints; inside the period nothing; 5 s later prints again.
    CHECK(kl_tele_due(&s, 1000, MIB(2000), 3, 5000) == 1, "tele: first call prints");
    CHECK(kl_tele_due(&s, 3000, MIB(2000), 3, 5000) == 0, "tele: inside the period");
    CHECK(kl_tele_due(&s, 6000, MIB(2000), 3, 5000) == 1, "tele: second line");
    CHECK(kl_tele_due(&s, 11000, MIB(2000), 3, 5000) == 1, "tele: third line (cap reached)");
    // Past the cap only a change of more than 10 % of the last printed footprint gets through.
    CHECK(kl_tele_due(&s, 16000, MIB(2100), 3, 5000) == 0, "tele: +5 % after the cap is silent");
    CHECK(kl_tele_due(&s, 21000, MIB(2199), 3, 5000) == 0, "tele: +9.95 % after the cap is silent");
    CHECK(kl_tele_due(&s, 26000, MIB(2300), 3, 5000) == 1, "tele: +15 % after the cap prints");
    CHECK(kl_tele_due(&s, 31000, MIB(2300), 3, 5000) == 0, "tele: the reference moved to the last printed value");
    CHECK(kl_tele_due(&s, 36000, MIB(2000), 3, 5000) == 1, "tele: a fall of more than 10 % prints too");
    // 120 lines in the first 600 s, then silence for a flat footprint.
    kl_tele_state t = {0};
    unsigned n = 0;
    for (uint64_t ms = 5000; ms <= 5000ull * 400; ms += 5000) n += (unsigned)kl_tele_due(&t, ms, MIB(3000), 120, 5000);
    CHECK(n == 120, "tele: at most 120 lines for a flat footprint");
}

static void test_kv(void) {
    const char *l = "alloc=4096 tex_astc=10 pso=7 fn=3 lib=3 lib_compiled=99 buf_own=5 args_b=12";
    CHECK(kl_kv_u64(l, "alloc") == 4096, "kv: first key");
    CHECK(kl_kv_u64(l, "lib") == 3, "kv: exact key, not the lib_compiled prefix");
    CHECK(kl_kv_u64(l, "lib_compiled") == 99, "kv: longer key");
    CHECK(kl_kv_u64(l, "args_b") == 12, "kv: last key");
    CHECK(kl_kv_u64(l, "b") == 0 && kl_kv_u64(l, "missing") == 0, "kv: absent key");
    CHECK(kl_kv_u64("", "alloc") == 0 && kl_kv_u64("alloc=x", "alloc") == 0, "kv: empty / not a number");
    CHECK(kl_kv_u64("xalloc=5 alloc=6", "alloc") == 6, "kv: a key must start a word");
}

int main(void) {
    test_defaults();
    test_levels();
    test_threshold();
    test_sanitize();
    test_hysteresis();
    test_is_low();
    test_rate();
    test_tele();
    test_kv();
    if (fails) { printf("t_lowmem: %d FAILED\n", fails); return 1; }
    printf("t_lowmem: ok\n");
    return 0;
}
