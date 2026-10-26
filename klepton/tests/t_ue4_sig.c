// UE native signatures of guest/kl_ue4.c (KL-TEST ue4sig block, cut out at build time).
// ITR2 (UE5) takes nativeSetWindowInfo(ZII) and nativeSetAndroidVersionInformation(String,int,String x5):
// the host must call with the longest form so no declared parameter is read as garbage. Fake natives
// check every argument arrives in its position, the last String included (on the stack on AArch64).
#include <stdio.h>
#include <string.h>

typedef unsigned char ue4_jboolean;
typedef void (*ue4_fn_v)(void *, void *);
#include "ue4sig.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
static int got_win, got_ver;
static ue4_jboolean w_p; static int w_d, w_a;
static void fake_window(void *e, void *t, ue4_jboolean p, int d, int a) { (void)e; (void)t; w_p = p; w_d = d; w_a = a; got_win = 1; }
static void *v_s[6]; static int v_i;
static void fake_ver(void *e, void *t, void *s1, int i, void *s2, void *s3, void *s4, void *s5, void *s6) {
    (void)e; (void)t; v_s[0] = s1; v_i = i; v_s[1] = s2; v_s[2] = s3; v_s[3] = s4; v_s[4] = s5; v_s[5] = s6; got_ver = 1;
}

int main(void) {
    ue4_fn_ZII w = (ue4_fn_ZII)fake_window;
    w(0, 0, 1, 16, 2);
    CHECK(got_win && w_p == 1 && w_d == 16 && w_a == 2, "nativeSetWindowInfo (ZII) gets all three");
    ue4_fn_sIsssss v = (ue4_fn_sIsssss)fake_ver;
    char c[6];
    v(0, 0, &c[0], 32, &c[1], &c[2], &c[3], &c[4], &c[5]);
    CHECK(got_ver && v_i == 32, "nativeSetAndroidVersionInformation int arg");
    for (int i = 0; i < 6; i++) CHECK(v_s[i] == &c[i], "string argument in position");
    if (fails) return 1;
    printf("t_ue4_sig ok\n");
    return 0;
}
