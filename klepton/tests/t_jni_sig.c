// JNI signature decoders (kl_jni.c: klj_decode_args / klj_decode_args_a), runnable on Linux:
//   make jnisig   (or: see Makefile; the two functions are extracted from kl_jni.c at build time)
// Array parameters ("[F", "[[I", "[Ljava/lang/Object;") must always be one reference: a GP slot in the
// varargs form, jvalue.l in the A form, never a float/32-bit scalar.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KLJ_MAX_ARGS 16
typedef int32_t kl_jint;
typedef union { uint64_t j; double d; void *l; } klj_val;
typedef union {
    uint8_t z; int8_t b; uint16_t c; int16_t s;
    kl_jint i; int64_t j; float f; double d; void *l;
} klj_jvalue;
typedef struct { int unused; } kl_va;

// Scripted AAPCS64 register banks: kl_va_gp / kl_va_fp pop the next value of their own bank.
static uint64_t gp_q[32]; static double fp_q[32]; static int gp_n, fp_n;
uint64_t kl_va_gp(kl_va *v) { (void)v; return gp_q[gp_n++]; }
double   kl_va_fp(kl_va *v) { (void)v; return fp_q[fp_n++]; }

#include "jni_decoders.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static void reset(void) { gp_n = fp_n = 0; }

int main(void) {
    klj_val av[KLJ_MAX_ARGS];
    kl_va va = {0};
    int a, b, c;

    // ([F)V : one reference in GP; the FP bank must stay untouched.
    reset(); gp_q[0] = 0x7000ULL; fp_q[0] = 1.5;
    a = klj_decode_args("([F)V", &va, av);
    CHECK(a == 1 && av[0].l == (void *)0x7000 && gp_n == 1 && fp_n == 0, "varargs ([F)V");
    klj_jvalue jv[4]; memset(jv, 0, sizeof jv);
    jv[0].l = (void *)0x7000;
    a = klj_decode_args_a("([F)V", jv, av);
    CHECK(a == 1 && av[0].l == (void *)0x7000, "A ([F)V");

    // ([[I)V : 64-bit pointer survives (a [I read as jint would truncate it).
    reset(); gp_q[0] = 0x123456789000ULL;
    a = klj_decode_args("([[I)V", &va, av);
    CHECK(a == 1 && av[0].l == (void *)0x123456789000ULL && fp_n == 0, "varargs ([[I)V");
    jv[0].l = (void *)0x123456789000ULL;
    a = klj_decode_args_a("([[I)V", jv, av);
    CHECK(a == 1 && av[0].l == (void *)0x123456789000ULL, "A ([[I)V full 64 bits");
    const char *prim = "ZBCSIJFD";
    for (const char *p = prim; *p; p++) {
        char sig[16]; snprintf(sig, sizeof sig, "([%c)V", *p);
        jv[0].l = (void *)0x123456789000ULL;
        a = klj_decode_args_a(sig, jv, av);
        CHECK(a == 1 && av[0].l == (void *)0x123456789000ULL, sig);
    }

    // (I[DLjava/lang/String;F)V : I,[D,String in GP (3), F in FP (1).
    reset(); gp_q[0] = 7; gp_q[1] = 0x7100; gp_q[2] = 0x7200; fp_q[0] = 2.5;
    a = klj_decode_args("(I[DLjava/lang/String;F)V", &va, av);
    CHECK(a == 4 && av[0].j == 7 && av[1].l == (void *)0x7100 && av[2].l == (void *)0x7200
          && av[3].d == 2.5 && gp_n == 3 && fp_n == 1, "varargs (I[DLjava/lang/String;F)V");
    memset(jv, 0, sizeof jv);
    jv[0].i = -3; jv[1].l = (void *)0x7100; jv[2].l = (void *)0x7200; jv[3].f = 2.5f;
    a = klj_decode_args_a("(I[DLjava/lang/String;F)V", jv, av);
    CHECK(a == 4 && (int64_t)av[0].j == -3 && av[1].l == (void *)0x7100 && av[2].l == (void *)0x7200
          && av[3].d == 2.5, "A (I[DLjava/lang/String;F)V");

    // ([Ljava/lang/Object;J)V : the Object array does not swallow the J that follows.
    reset(); gp_q[0] = 0x7300; gp_q[1] = 0x1122334455667788ULL;
    a = klj_decode_args("([Ljava/lang/Object;J)V", &va, av);
    CHECK(a == 2 && av[0].l == (void *)0x7300 && av[1].j == 0x1122334455667788ULL && gp_n == 2 && fp_n == 0,
          "varargs ([Ljava/lang/Object;J)V");
    jv[0].l = (void *)0x7300; jv[1].j = 0x1122334455667788LL;
    a = klj_decode_args_a("([Ljava/lang/Object;J)V", jv, av);
    CHECK(a == 2 && av[0].l == (void *)0x7300 && av[1].j == 0x1122334455667788ULL, "A ([Ljava/lang/Object;J)V");

    // Malformed: dangling '[' and unterminated array of nothing.
    reset(); b = klj_decode_args("([)V", &va, av); c = klj_decode_args_a("([", jv, av);
    CHECK(b == -1 && c == -1, "malformed array rejected");

    printf(fails ? "t_jni_sig: %d FAILED\n" : "t_jni_sig: ok\n", fails);
    return fails != 0;
}
