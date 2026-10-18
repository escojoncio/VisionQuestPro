// The rest of bionic's libc / libm surface that no guest had asked for yet.
//
// kl_shim.c's table is what the guests Klepton has loaded so far imported, so it
// is a record of the past; this file is the other half, written from bionic's own
// export list (libc.map.txt, API 32-34 — the Quest's range) rather than from any
// one title. The rule is the project's: a name is served only with the behaviour
// bionic has, and a name bionic does NOT export (glibc-only, removed, or hidden)
// is left out so a guest that imports it fails by name, exactly as on Android.
//
//   * forward  — the Darwin call has the same signature and semantics. A table
//                entry pointing straight at it, or a wrapper that says why not.
//   * translate — the call exists on both and differs: long double is binary128
//                on bionic and binary64 here; FP_* numbering; langinfo item
//                numbers; ilogb's special values. The wrapper is the difference.
//   * absent   — the facility does not exist here. Fails the way Android fails
//                when the caller lacks the privilege, not by pretending.
//
// kl_bionic_extra_lookup is consulted by kl_shim_lookup/kl_shim_known BEFORE the
// generated table: the three long double names the generator forwarded blindly
// (frexpl, ldexpl, truncl) are overridden here, and a regenerated table cannot
// undo that.
#define _GNU_SOURCE 1
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include "kl_ld128.h"
void *kl_shim_lookup(const char *name);        // kl_shim.c

#ifndef __APPLE__
// Linux build (harness for the translation logic): the Darwin-only pieces are
// compiled out, everything else is plain C.
#endif

typedef struct { const char *name; void *fn; } kl_xentry;
#define X(n) { #n, (void *)(n) }
#define XW(n, f) { n, (void *)(f) }

// ======================================================================
// math: long double (binary128) <-> double
// ======================================================================
#if defined(__aarch64__) && !defined(KL_EXTRA_TEST)
#include <arm_neon.h>
// A 128-bit vector travels in the same Q register as a binary128 long double.
typedef uint64x2_t kld;
static inline kl_q ld_in(kld v) { kl_q q = { vgetq_lane_u64(v, 0), vgetq_lane_u64(v, 1) }; return q; }
static inline kld ld_out(kl_q q) { uint64x2_t v = { q.lo, q.hi }; return v; }
#else
typedef kl_q kld;
static inline kl_q ld_in(kld v) { return v; }
static inline kld ld_out(kl_q q) { return q; }
#endif
static inline double D(kld v) { return kl_q_to_double(ld_in(v)); }
static inline kld Q(double d) { return ld_out(kl_q_from_double(d)); }

#define LD1(n, f)  static kld klx_##n(kld x) { return Q(f(D(x))); }
#define LD2(n, f)  static kld klx_##n(kld x, kld y) { return Q(f(D(x), D(y))); }
LD1(acosl, acos) LD1(acoshl, acosh) LD1(asinl, asin) LD1(asinhl, asinh)
LD1(atanl, atan) LD1(atanhl, atanh) LD1(cbrtl, cbrt) LD1(ceill, ceil)
LD1(cosl, cos) LD1(coshl, cosh) LD1(erfl, erf) LD1(erfcl, erfc)
LD1(exp2l, exp2) LD1(expl, exp) LD1(expm1l, expm1) LD1(floorl, floor)
LD1(log10l, log10) LD1(log1pl, log1p) LD1(log2l, log2) LD1(logbl, logb)
LD1(logl, log) LD1(nearbyintl, nearbyint) LD1(rintl, rint) LD1(roundl, round)
LD1(sinl, sin) LD1(sinhl, sinh) LD1(sqrtl, sqrt) LD1(tanl, tan)
LD1(tanhl, tanh) LD1(tgammal, tgamma) LD1(lgammal, lgamma)
LD2(atan2l, atan2) LD2(copysignl, copysign) LD2(fmaxl, fmax) LD2(fminl, fmin)
LD2(fmodl, fmod) LD2(hypotl, hypot) LD2(nextafterl, nextafter)
LD2(powl, pow) LD2(remainderl, remainder)
// nexttowardl(long double, long double): both operands are binary128 here too.
LD2(nexttowardl, nextafter)
// truncl/frexpl/ldexpl were forwarded to Darwin by the generated table, which reads
// the wrong half of the Q register; they live here now.
LD1(truncl, trunc)
static kld klx_exp10l(kld x) { return Q(pow(10.0, D(x))); }
static kld klx_fmal(kld x, kld y, kld z) { return Q(fma(D(x), D(y), D(z))); }
static kld klx_ldexpl(kld x, int e) { return Q(ldexp(D(x), e)); }
static kld klx_scalbnl(kld x, int e) { return Q(scalbn(D(x), e)); }
static kld klx_scalblnl(kld x, long e) { return Q(scalbln(D(x), e)); }
static kld klx_frexpl(kld x, int *e) { return Q(frexp(D(x), e)); }
static kld klx_nanl(const char *tag) { return Q(nan(tag)); }
static kld klx_remquol(kld x, kld y, int *quo) { return Q(remquo(D(x), D(y), quo)); }
static kld klx_lgammal_r(kld x, int *sign) { double r = lgamma_r(D(x), sign); return Q(r); }
static void klx_sincosl(kld x, kld *s, kld *c) { double v = D(x); *s = Q(sin(v)); *c = Q(cos(v)); }
// bionic's lrintl & friends return long / long long; the double value is rounded
// by the Darwin routine, so a quad beyond double's exact range is rounded twice
// (only differs for |x| > 2^53, where long already saturates or is exact).
static long klx_lrintl(kld x) { return lrint(D(x)); }
static long long klx_llrintl(kld x) { return llrint(D(x)); }
static long klx_lroundl(kld x) { return lround(D(x)); }
static long long klx_llroundl(kld x) { return llround(D(x)); }

// ======================================================================
// math: classification — bionic numbers FP_* like FreeBSD, Darwin does not.
//   bionic: FP_INFINITE 1, FP_NAN 2, FP_NORMAL 4, FP_SUBNORMAL 8, FP_ZERO 16
//   Darwin: FP_NAN 1, FP_INFINITE 2, FP_ZERO 3, FP_NORMAL 4, FP_SUBNORMAL 5
// The *l forms look at the binary128 bits directly: converting to double first
// would call a finite 1e5000 infinite.
// ======================================================================
#define B_FP_INFINITE 0x01
#define B_FP_NAN      0x02
#define B_FP_NORMAL   0x04
#define B_FP_SUBNORMAL 0x08
#define B_FP_ZERO     0x10
static int fpclass_d(double d) {
    uint64_t b; memcpy(&b, &d, 8);
    unsigned e = (unsigned)((b >> 52) & 0x7FF); uint64_t m = b & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) return m ? B_FP_NAN : B_FP_INFINITE;
    if (e == 0) return m ? B_FP_SUBNORMAL : B_FP_ZERO;
    return B_FP_NORMAL;
}
static int fpclass_f(float f) {
    uint32_t b; memcpy(&b, &f, 4);
    unsigned e = (b >> 23) & 0xFF; uint32_t m = b & 0x7FFFFF;
    if (e == 0xFF) return m ? B_FP_NAN : B_FP_INFINITE;
    if (e == 0) return m ? B_FP_SUBNORMAL : B_FP_ZERO;
    return B_FP_NORMAL;
}
static int fpclass_q(kl_q q) {
    unsigned e = (unsigned)((q.hi >> 48) & 0x7FFF); int m = ((q.hi & 0xFFFFFFFFFFFFull) | q.lo) != 0;
    if (e == 0x7FFF) return m ? B_FP_NAN : B_FP_INFINITE;
    if (e == 0) return m ? B_FP_SUBNORMAL : B_FP_ZERO;
    return B_FP_NORMAL;
}
static int klx___fpclassifyd(double d) { return fpclass_d(d); }
static int klx___fpclassifyf(float f) { return fpclass_f(f); }
static int klx___fpclassifyl(kld x) { return fpclass_q(ld_in(x)); }
static int klx___isnormal(double d) { return fpclass_d(d) == B_FP_NORMAL; }
static int klx___isnormalf(float f) { return fpclass_f(f) == B_FP_NORMAL; }
static int klx___isnormall(kld x) { return fpclass_q(ld_in(x)) == B_FP_NORMAL; }
static int klx___isfinitel(kld x) { int c = fpclass_q(ld_in(x)); return c != B_FP_NAN && c != B_FP_INFINITE; }
static int klx___isinfl(kld x) { return fpclass_q(ld_in(x)) == B_FP_INFINITE; }
static int klx___isnanl(kld x) { return fpclass_q(ld_in(x)) == B_FP_NAN; }
static int klx___signbit(double d) { return signbit(d) != 0; }
static int klx___signbitf(float f) { return signbit(f) != 0; }
static int klx___signbitl(kld x) { return (int)(ld_in(x).hi >> 63); }
static int klx_isinf(double d) { return fpclass_d(d) == B_FP_INFINITE; }
static int klx_isinff(float f) { return fpclass_f(f) == B_FP_INFINITE; }
static int klx_isnanf(float f) { return fpclass_f(f) == B_FP_NAN; }
static int klx_finite(double d) { int c = fpclass_d(d); return c != B_FP_NAN && c != B_FP_INFINITE; }
static int klx_finitef(float f) { int c = fpclass_f(f); return c != B_FP_NAN && c != B_FP_INFINITE; }

// ======================================================================
// math: float/double routines Darwin has under another shape or not at all
// ======================================================================
// FreeBSD msun (bionic's libm) returns FP_ILOGB0 = -INT_MAX for 0 and INT_MAX for
// NaN and infinity; Darwin returns INT_MIN for 0 and NaN.
static int klx_ilogb(double x) {
    if (x == 0.0) return -INT_MAX;
    if (isnan(x) || isinf(x)) return INT_MAX;
    return ilogb(x);
}
static int klx_ilogbf(float x) {
    if (x == 0.0f) return -INT_MAX;
    if (isnan(x) || isinf(x)) return INT_MAX;
    return ilogbf(x);
}
static int klx_ilogbl(kld x) { return klx_ilogb(D(x)); }
static float klx_j0f(float x) { return (float)j0((double)x); }
static float klx_j1f(float x) { return (float)j1((double)x); }
static float klx_jnf(int n, float x) { return (float)jn(n, (double)x); }
static float klx_y0f(float x) { return (float)y0((double)x); }
static float klx_y1f(float x) { return (float)y1((double)x); }
static float klx_ynf(int n, float x) { return (float)yn(n, (double)x); }
static double klx_gamma(double x) { return lgamma(x); }
static float klx_gammaf(float x) { return lgammaf(x); }
static double klx_gamma_r(double x, int *s) { return lgamma_r(x, s); }
static float klx_gammaf_r(float x, int *s) { return lgammaf_r(x, s); }
static double klx_scalb(double x, double fn) {
    if (isnan(x) || isnan(fn)) return x * fn;
    if (!isfinite(fn)) {
        if (fn > 0) return x * fn;
        return x / -fn;
    }
    if (rint(fn) != fn) return (fn - fn) / (fn - fn);        // NaN, as msun
    if (fn > 65000.0) return scalbn(x, 65000);
    if (-fn > 65000.0) return scalbn(x, -65000);
    return scalbn(x, (int)fn);
}
static float klx_scalbf(float x, float fn) { return (float)klx_scalb((double)x, (double)fn); }
static double klx_drem(double x, double y) { return remainder(x, y); }
static float klx_dremf(float x, float y) { return remainderf(x, y); }
static double klx_significand(double x) {
    if (x == 0.0 || !isfinite(x)) return x;
    return scalbn(x, -ilogb(x));
}
static float klx_significandf(float x) {
    if (x == 0.0f || !isfinite(x)) return x;
    return scalbnf(x, -ilogbf(x));
}
static double klx_exp10(double x) { return pow(10.0, x); }
static float klx_exp10f(float x) { return powf(10.0f, x); }

// ======================================================================
// string / ctype / locale
// ======================================================================
static int klx_bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void klx_bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
static void klx_bzero(void *p, size_t n) { memset(p, 0, n); }
static char *klx_rindex(const char *s, int c) { return strrchr(s, c); }
// explicit_bzero / memset_explicit must not be elided: write through a volatile.
static void klx_explicit_bzero(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}
static void *klx_memset_explicit(void *p, int c, size_t n) {
    volatile unsigned char *v = p;
    for (size_t i = 0; i < n; i++) v[i] = (unsigned char)c;
    return p;
}
static void *klx_memccpy(void *dst, const void *src, int c, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src; unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) { d[i] = s[i]; if (s[i] == ch) return d + i + 1; }
    return NULL;
}
static void *klx_mempcpy(void *dst, const void *src, size_t n) { return (char *)memcpy(dst, src, n) + n; }
static char *klx_strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *r = malloc(l + 1);
    if (!r) return NULL;
    memcpy(r, s, l); r[l] = 0;
    return r;
}
static int klx_ffs(int v) { return __builtin_ffs(v); }
static int klx_ffsl(long v) { return __builtin_ffsl(v); }
static int klx_ffsll(long long v) { return __builtin_ffsll(v); }
static void klx_swab(const void *from, void *to, ssize_t n) {
    const unsigned char *f = from; unsigned char *t = to;
    for (; n > 1; n -= 2, f += 2, t += 2) { unsigned char a = f[0]; t[0] = f[1]; t[1] = a; }
}
static int klx_toascii(int c) { return c & 0x7F; }

// strverscmp: digit runs compare as numbers; a run with a leading zero is a fraction
// (the state machine of glibc, which bionic's matches).
static int klx_strverscmp(const char *s1, const char *s2) {
    enum { S_N = 0x0, S_I = 0x3, S_F = 0x6, S_Z = 0x9, CMP = 2, LEN = 3 };
    static const uint8_t next_state[] = {
        /*        x    d    0  */
        /* S_N */ S_N, S_I, S_Z,
        /* S_I */ S_N, S_I, S_I,
        /* S_F */ S_N, S_F, S_F,
        /* S_Z */ S_N, S_F, S_Z };
    static const int8_t result_type[] = {
        /*       x/x  x/d  x/0  d/x  d/d  d/0  0/x  0/d  0/0 */
        /* S_N */ CMP, CMP, CMP, CMP, LEN, CMP, CMP, CMP, CMP,
        /* S_I */ CMP, -1,  -1,  +1,  LEN, LEN, +1,  LEN, LEN,
        /* S_F */ CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
        /* S_Z */ CMP, +1,  +1,  -1,  CMP, CMP, -1,  CMP, CMP };
    const unsigned char *p1 = (const unsigned char *)s1, *p2 = (const unsigned char *)s2;
    if (p1 == p2) return 0;
    unsigned char c1 = *p1++, c2 = *p2++;
    int state = S_N | ((c1 == '0') + (isdigit(c1) != 0));
    int diff;
    while ((diff = c1 - c2) == 0) {
        if (c1 == '\0') return diff;
        state = next_state[state];
        c1 = *p1++; c2 = *p2++;
        state |= (c1 == '0') + (isdigit(c1) != 0);
    }
    state = result_type[state * 3 + ((c2 == '0') + (isdigit(c2) != 0))];
    switch (state) {
    case CMP: return diff;
    case LEN:
        while (isdigit(*p1++)) if (!isdigit(*p2++)) return 1;
        return isdigit(*p2) ? -1 : diff;
    default: return state;
    }
}

// bionic's *_l string/ctype calls take a locale_t and ignore it: its C library has no
// per-locale tables for the 8-bit classes.
static int klx_strcasecmp_l(const char *a, const char *b, void *l) { (void)l; return strcasecmp(a, b); }
static int klx_strncasecmp_l(const char *a, const char *b, size_t n, void *l) { (void)l; return strncasecmp(a, b, n); }
#define CT_L(n) static int klx_##n##_l(int c, void *l) { (void)l; return n(c); }
CT_L(isalnum) CT_L(isalpha) CT_L(isblank) CT_L(iscntrl) CT_L(isgraph) CT_L(isprint)
CT_L(ispunct) CT_L(isspace)
static int klx_isblank(int c) { return isblank(c); }
#define WCT_L(n) static int klx_##n##_l(wint_t c, void *l) { (void)l; return n(c); }
WCT_L(iswalnum) WCT_L(iswgraph)
static int klx_iswgraph(wint_t c) { return iswgraph(c); }
static int klx_iswctype_l(wint_t c, unsigned long t, void *l) { (void)l; return iswctype(c, (wctype_t)t); }
static unsigned long klx_wctrans(const char *name) { return (unsigned long)wctrans(name); }
static unsigned long klx_wctrans_l(const char *name, void *l) { (void)l; return (unsigned long)wctrans(name); }
static wint_t klx_towctrans(wint_t c, unsigned long d) { return towctrans(c, (wctrans_t)d); }
static wint_t klx_towctrans_l(wint_t c, unsigned long d, void *l) { (void)l; return towctrans(c, (wctrans_t)d); }
static unsigned long klx_wctype_l(const char *name, void *l) { (void)l; return (unsigned long)wctype(name); }

// locale-suffixed number parsers: bionic's strtol_l & co. ignore the locale.
static long klx_strtol_l(const char *s, char **e, int b, void *l) { (void)l; return strtol(s, e, b); }
static unsigned long klx_strtoul_l(const char *s, char **e, int b, void *l) { (void)l; return strtoul(s, e, b); }
static long long klx_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
static intmax_t klx_strtoimax(const char *s, char **e, int b) { return strtoimax(s, e, b); }
static uintmax_t klx_strtoumax(const char *s, char **e, int b) { return strtoumax(s, e, b); }
static long long klx_strtoq(const char *s, char **e, int b) { return strtoll(s, e, b); }
static unsigned long long klx_strtouq(const char *s, char **e, int b) { return strtoull(s, e, b); }
static long klx_wcstol_l(const wchar_t *s, wchar_t **e, int b, void *l) { (void)l; return wcstol(s, e, b); }
static long long klx_wcstoll_l(const wchar_t *s, wchar_t **e, int b, void *l) { (void)l; return wcstoll(s, e, b); }
static unsigned long klx_wcstoul_l(const wchar_t *s, wchar_t **e, int b, void *l) { (void)l; return wcstoul(s, e, b); }
static unsigned long long klx_wcstoull_l(const wchar_t *s, wchar_t **e, int b, void *l) { (void)l; return wcstoull(s, e, b); }
static float klx_wcstof_l(const wchar_t *s, wchar_t **e, void *l) { (void)l; return wcstof(s, e); }
static double klx_wcstod_l(const wchar_t *s, wchar_t **e, void *l) { (void)l; return wcstod(s, e); }
static intmax_t klx_wcstoimax(const wchar_t *s, wchar_t **e, int b) { return wcstoimax(s, e, b); }
static uintmax_t klx_wcstoumax(const wchar_t *s, wchar_t **e, int b) { return wcstoumax(s, e, b); }

// abs family: Darwin's are macros/inline in some headers but real symbols exist; the
// bionic ones are plain functions with the same contract.
static long klx_labs(long v) { return v < 0 ? -v : v; }
static long long klx_llabs(long long v) { return v < 0 ? -v : v; }
static intmax_t klx_imaxabs(intmax_t v) { return v < 0 ? -v : v; }
static ldiv_t klx_ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }
static imaxdiv_t klx_imaxdiv(intmax_t a, intmax_t b) { imaxdiv_t r = { a / b, a % b }; return r; }

// ---------------- wide strings (wchar_t is 32-bit on both) ----------------
static wchar_t *klx_wcpcpy(wchar_t *d, const wchar_t *s) {
    while ((*d = *s++)) d++;
    return d;
}
static wchar_t *klx_wcpncpy(wchar_t *d, const wchar_t *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    wchar_t *end = d + i;
    for (; i < n; i++) d[i] = 0;
    return end;
}
static wchar_t *klx_wcscat(wchar_t *d, const wchar_t *s) { wcscat(d, s); return d; }
static size_t klx_wcscspn(const wchar_t *s, const wchar_t *rej) { return wcscspn(s, rej); }
static wchar_t *klx_wcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *r = malloc(n * sizeof *r);
    if (r) memcpy(r, s, n * sizeof *r);
    return r;
}
static size_t klx_wcslcpy(wchar_t *d, const wchar_t *s, size_t n) {
    size_t l = wcslen(s);
    if (n) { size_t c = l >= n ? n - 1 : l; wmemcpy(d, s, c); d[c] = 0; }
    return l;
}
static size_t klx_wcslcat(wchar_t *d, const wchar_t *s, size_t n) {
    size_t dl = 0;
    while (dl < n && d[dl]) dl++;
    if (dl == n) return n + wcslen(s);
    return dl + klx_wcslcpy(d + dl, s, n - dl);
}
static size_t klx_wcsnlen(const wchar_t *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}
static wchar_t *klx_wcspbrk(const wchar_t *s, const wchar_t *set) { return wcspbrk(s, set); }
static size_t klx_wcsspn(const wchar_t *s, const wchar_t *set) { return wcsspn(s, set); }
static wchar_t *klx_wcsstr(const wchar_t *h, const wchar_t *n) { return wcsstr(h, n); }
static wchar_t *klx_wmempcpy(wchar_t *d, const wchar_t *s, size_t n) { return wmemcpy(d, s, n) + n; }
// bionic's locale-less case-insensitive compare lowercases with towlower.
static int wcsncase(const wchar_t *a, const wchar_t *b, size_t n, int bounded) {
    for (size_t i = 0; !bounded || i < n; i++) {
        wint_t x = towlower(a[i]), y = towlower(b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static int klx_wcscasecmp(const wchar_t *a, const wchar_t *b) { return wcsncase(a, b, 0, 0); }
static int klx_wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncase(a, b, n, 1); }
static int klx_wcscasecmp_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcsncase(a, b, 0, 0); }
static int klx_wcsncasecmp_l(const wchar_t *a, const wchar_t *b, size_t n, void *l) { (void)l; return wcsncase(a, b, n, 1); }

// wcwidth: East Asian wide = 2, combining/format = 0, controls = -1 (the table
// bionic ships comes from the same Unicode data; ranges here are the stable ones).
static int klx_wcwidth(wchar_t wc) {
    uint32_t c = (uint32_t)wc;
    if (c == 0) return 0;
    if (c < 32 || (c >= 0x7F && c < 0xA0)) return -1;
    static const struct { uint32_t lo, hi; } zero[] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
        {0x064B, 0x065F}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
        {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
        {0xE0100, 0xE01EF},
    };
    for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++)
        if (c >= zero[i].lo && c <= zero[i].hi) return 0;
    if (c >= 0x1100 && (c <= 0x115F || c == 0x2329 || c == 0x232A ||
        (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) ||
        (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF) ||
        (c >= 0x20000 && c <= 0x3FFFD)))
        return 2;
    return 1;
}
static int klx_wcswidth(const wchar_t *s, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int c = klx_wcwidth(s[i]);
        if (c < 0) return -1;
        w += c;
    }
    return w;
}

// ---------------- multibyte <-> wide: always UTF-8, like bionic ----------------
// bionic's multibyte functions are UTF-8 in every locale; Darwin's, in the "C"
// locale the host process runs in, treat bytes as Latin-1. The functions below
// replace the forwarded ones for exactly that reason, and use bionic's 8-byte
// mbstate_t (Darwin's is 128 bytes, so a forwarded call could overrun a guest's).
//   state: seq[0..2] = pending lead bytes, rsv[0] = pending count,
//          rsv[1] = 1 when a UTF-16 low surrogate is pending / 2 when a high one is held,
//          rsv[2..3] = that surrogate (little endian)
typedef struct { unsigned char seq[4]; unsigned char rsv[4]; } bmbstate;
static bmbstate g_mbs_private;           // the state mblen/mbtowc/wctomb share, as in bionic

// Decodes one scalar from `s` (n available bytes) continuing from `st` (may be NULL
// for stateless callers, which then see incomplete sequences as errors).
// Returns bytes consumed from s (0 for NUL), (size_t)-1 invalid, (size_t)-2 incomplete.
static size_t u8_decode(uint32_t *out, const char *s, size_t n, bmbstate *st) {
    unsigned char buf[4]; unsigned have = 0;
    if (st) { have = st->rsv[0]; if (have > 3) have = 0; memcpy(buf, st->seq, have); }
    size_t used = 0;
    if (have == 0) {
        if (n == 0) return (size_t)-2;
        buf[0] = (unsigned char)s[used++]; have = 1;
    }
    unsigned char l = buf[0];
    unsigned len;
    if (l < 0x80) len = 1;
    else if (l >= 0xC2 && l <= 0xDF) len = 2;
    else if (l >= 0xE0 && l <= 0xEF) len = 3;
    else if (l >= 0xF0 && l <= 0xF4) len = 4;
    else goto bad;
    if (len == 1) { *out = l; if (st) st->rsv[0] = 0; return l ? used : 0; }
    while (have < len) {
        if (used >= n) {                       // out of input mid-sequence: keep what we have
            if (st) { memcpy(st->seq, buf, have); st->rsv[0] = (unsigned char)have; }
            return (size_t)-2;
        }
        unsigned char c = (unsigned char)s[used];
        if ((c & 0xC0) != 0x80) goto bad;
        if (have == 1 && ((l == 0xE0 && c < 0xA0) || (l == 0xED && c > 0x9F) ||
                          (l == 0xF0 && c < 0x90) || (l == 0xF4 && c > 0x8F)))
            goto bad;                          // overlong, surrogate, beyond U+10FFFF
        buf[have++] = c; used++;
    }
    {
        uint32_t cp = len == 2 ? (l & 0x1Fu) : len == 3 ? (l & 0x0Fu) : (l & 0x07u);
        for (unsigned i = 1; i < len; i++) cp = (cp << 6) | (buf[i] & 0x3Fu);
        *out = cp;
        if (st) st->rsv[0] = 0;
        return used;
    }
bad:
    if (st) st->rsv[0] = 0;
    errno = EILSEQ;
    return (size_t)-1;
}
static size_t u8_encode(char *out, uint32_t c) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c >= 0xD800 && c <= 0xDFFF) return 0;
    if (c < 0x10000) { out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    if (c <= 0x10FFFF) { out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | (c >> 12 & 0x3F)); out[2] = (char)(0x80 | (c >> 6 & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F)); return 4; }
    return 0;
}

static size_t klx_mbrtowc(wchar_t *pwc, const char *s, size_t n, bmbstate *st) {
    if (!st) st = &g_mbs_private;
    if (!s) { memset(st, 0, sizeof *st); return 0; }
    uint32_t c; size_t r = u8_decode(&c, s, n, st);
    if (r == (size_t)-1 || r == (size_t)-2) return r;
    if (pwc) *pwc = (wchar_t)c;
    return r;
}
static size_t klx_wcrtomb(char *s, wchar_t wc, bmbstate *st) {
    (void)st;
    if (!s) return 1;
    size_t r = u8_encode(s, (uint32_t)wc);
    if (!r) { errno = EILSEQ; return (size_t)-1; }
    return r;
}
static int klx_mbsinit(const bmbstate *st) { return !st || st->rsv[0] == 0; }
static int klx_mblen(const char *s, size_t n) {
    if (!s) { memset(&g_mbs_private, 0, sizeof g_mbs_private); return 0; }
    uint32_t c; size_t r = u8_decode(&c, s, n, NULL);
    if (r == (size_t)-2) { errno = EILSEQ; return -1; }
    return r == (size_t)-1 ? -1 : (int)r;
}
static int klx_mbtowc(wchar_t *pwc, const char *s, size_t n) {
    if (!s) { memset(&g_mbs_private, 0, sizeof g_mbs_private); return 0; }
    uint32_t c; size_t r = u8_decode(&c, s, n, NULL);
    if (r == (size_t)-2) { errno = EILSEQ; return -1; }
    if (r == (size_t)-1) return -1;
    if (pwc) *pwc = (wchar_t)c;
    return (int)r;
}
static int klx_wctomb(char *s, wchar_t wc) {
    if (!s) return 0;
    size_t r = u8_encode(s, (uint32_t)wc);
    if (!r) { errno = EILSEQ; return -1; }
    return (int)r;
}
static size_t klx_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, bmbstate *st) {
    if (!st) st = &g_mbs_private;
    const char *s = *src; size_t count = 0;
    while (nms > 0 && (dst == NULL || count < len)) {
        uint32_t c; size_t r = u8_decode(&c, s, nms, st);
        if (r == (size_t)-1) { if (dst) *src = s; return r; }
        if (r == (size_t)-2) { if (dst) *src = s + nms; return count; }   // incomplete tail: consumed into state
        if (r == 0) { if (dst) { dst[count] = 0; *src = NULL; } memset(st, 0, sizeof *st); return count; }
        if (dst) dst[count] = (wchar_t)c;
        count++; s += r; nms -= r;
    }
    if (dst) *src = s;
    return count;
}
static size_t klx_mbsrtowcs(wchar_t *dst, const char **src, size_t len, bmbstate *st) {
    return klx_mbsnrtowcs(dst, src, (size_t)-1, len, st);
}
static size_t klx_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, bmbstate *st) {
    (void)st;
    const wchar_t *s = *src; size_t count = 0; char tmp[4];
    while (nwc > 0) {
        size_t k = u8_encode(tmp, (uint32_t)*s);
        if (!k) { errno = EILSEQ; if (dst) *src = s; return (size_t)-1; }
        if (*s == 0) { if (dst) { if (count < len) dst[count] = 0; *src = NULL; } return count; }
        if (dst) { if (count + k > len) break; memcpy(dst + count, tmp, k); }
        count += k; s++; nwc--;
    }
    if (dst) *src = s;
    return count;
}
static size_t klx_wcsrtombs(char *dst, const wchar_t **src, size_t len, bmbstate *st) {
    return klx_wcsnrtombs(dst, src, (size_t)-1, len, st);
}
static size_t klx_mbstowcs(wchar_t *dst, const char *src, size_t len) {
    bmbstate st; memset(&st, 0, sizeof st);
    const char *p = src;
    return klx_mbsrtowcs(dst, &p, len, &st);
}
static size_t klx_wcstombs(char *dst, const wchar_t *src, size_t len) {
    bmbstate st; memset(&st, 0, sizeof st);
    const wchar_t *p = src;
    return klx_wcsrtombs(dst, &p, len, &st);
}
static wint_t klx_btowc(int c) { return (c >= 0 && c < 0x80) ? (wint_t)c : (wint_t)WEOF; }
static int klx_wctob(wint_t c) { return c < 0x80 ? (int)c : EOF; }

// C11 <uchar.h>: bionic has all four (API 21+)
static size_t klx_mbrtoc32(uint32_t *out, const char *s, size_t n, bmbstate *st) {
    if (!st) st = &g_mbs_private;
    if (!s) { memset(st, 0, sizeof *st); return 0; }
    if (n == 0) return (size_t)-2;
    uint32_t c; size_t r = u8_decode(&c, s, n, st);
    if (r == (size_t)-1 || r == (size_t)-2) return r;
    if (out) *out = c;
    return r;
}
static size_t klx_mbrtoc16(uint16_t *out, const char *s, size_t n, bmbstate *st) {
    if (!st) st = &g_mbs_private;
    if (!s) { memset(st, 0, sizeof *st); return 0; }
    if (st->rsv[1] == 1) {                       // second half of a pair waiting
        if (out) *out = (uint16_t)(st->rsv[2] | st->rsv[3] << 8);
        st->rsv[1] = 0; st->rsv[2] = st->rsv[3] = 0;
        return (size_t)-3;
    }
    if (n == 0) return (size_t)-2;
    uint32_t c; size_t r = u8_decode(&c, s, n, st);
    if (r == (size_t)-1 || r == (size_t)-2) return r;
    if (c >= 0x10000) {
        uint32_t v = c - 0x10000;
        if (out) *out = (uint16_t)(0xD800 + (v >> 10));
        uint16_t lo = (uint16_t)(0xDC00 + (v & 0x3FF));
        st->rsv[1] = 1; st->rsv[2] = lo & 0xFF; st->rsv[3] = lo >> 8;
    } else if (out) *out = (uint16_t)c;
    return r;
}
static size_t klx_c32rtomb(char *s, uint32_t c, bmbstate *st) {
    (void)st;
    if (!s) return 1;
    size_t r = u8_encode(s, c);
    if (!r) { errno = EILSEQ; return (size_t)-1; }
    return r;
}
static size_t klx_c16rtomb(char *s, uint16_t c, bmbstate *st) {
    if (!st) st = &g_mbs_private;
    if (!s) { memset(st, 0, sizeof *st); return 1; }
    if (st->rsv[1] == 2) {                       // a high surrogate is held
        uint32_t hi = (uint32_t)(st->rsv[2] | st->rsv[3] << 8);
        st->rsv[1] = 0; st->rsv[2] = st->rsv[3] = 0;
        if (c < 0xDC00 || c > 0xDFFF) { errno = EILSEQ; return (size_t)-1; }
        uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (c - 0xDC00);
        return u8_encode(s, cp);
    }
    if (c >= 0xD800 && c <= 0xDBFF) { st->rsv[1] = 2; st->rsv[2] = c & 0xFF; st->rsv[3] = c >> 8; return 0; }
    if (c >= 0xDC00 && c <= 0xDFFF) { errno = EILSEQ; return (size_t)-1; }
    return u8_encode(s, c);
}

// nl_langinfo: bionic's item numbers (not glibc's, not Darwin's) and the "C" locale
// strings bionic returns. CODESET is "UTF-8".
static char *klx_nl_langinfo(int item) {
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const abdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mons[] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };
    static const char *const abmons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (item >= 8 && item <= 14) return (char *)days[item - 8];          // DAY_1..7
    if (item >= 15 && item <= 21) return (char *)abdays[item - 15];      // ABDAY_1..7
    if (item >= 22 && item <= 33) return (char *)mons[item - 22];        // MON_1..12
    if (item >= 34 && item <= 45) return (char *)abmons[item - 34];      // ABMON_1..12
    switch (item) {
    case 1:  return "UTF-8";                         // CODESET
    case 2:  return "%a %b %e %H:%M:%S %Y";          // D_T_FMT
    case 3:  return "%m/%d/%y";                      // D_FMT
    case 4:  return "%H:%M:%S";                      // T_FMT
    case 5:  return "%I:%M:%S %p";                   // T_FMT_AMPM
    case 6:  return "AM";
    case 7:  return "PM";
    case 46: case 47: case 48: case 49: case 50: return "";   // ERA*, ALT_DIGITS
    case 51: return ".";                             // RADIXCHAR
    case 52: return "";                              // THOUSEP
    case 53: return "^[yY]";                         // YESEXPR
    case 54: return "^[nN]";                         // NOEXPR
    case 55: return "";                              // CRNCYSTR
    default: return "";
    }
}
static char *klx_nl_langinfo_l(int item, void *l) { (void)l; return klx_nl_langinfo(item); }

// Symbols implemented in the other libc files (kl_pthread.c, kl_libc.c, kl_libc_slink.c). The
// host test (KL_EXTRA_TEST) builds this file alone, so their table rows vanish there.
#ifndef KL_EXTRA_TEST
#define XE(guest, sym) { guest, (void *)sym },
#define DECLX(sym) extern void sym(void);
#else
#define XE(guest, sym)
#define DECLX(sym)
#endif

// ======================================================================
// Android-only libc: device level, fdsan, liblog extras, fortify, small libc leftovers
// ======================================================================
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef KL_EXTRA_TEST
static int kl_jni_build_int(const char *f, int d) { (void)f; return d; }
#else
#include "kl_jni.h"
#endif

// One answer for "how new is this device": ro.build.version.sdk, which follows the JNI
// Build.VERSION.SDK_INT (and KL_SDK_INT).
// Quest 3 runs Android 12L (API 32); the JNI table currently presents 29, so this follows
// the table rather than inventing a second answer.
extern int klb_sysprop_get(const char *name, char *value);
static int klx_android_get_device_api_level(void) {
    char v[92] = "";
    klb_sysprop_get("ro.build.version.sdk", v);        // the property, so both doors agree (KL_SDK_INT included)
    int api = atoi(v);
    return api > 0 ? api : kl_jni_build_int("SDK_INT", 32);
}
static int klx_android_get_application_target_sdk_version(void) { return 34; }  // ApplicationInfo.targetSdkVersion in kl_jni.c

// ---- fdsan (API 29+) ----------------------------------------------------
// Tags: type in the top byte, value in the low 56 bits. The per-fd owner table lives here;
// close() is not instrumented, so a plain close() of an owned fd is not reported (Android
// would warn).
enum { FDSAN_DISABLED = 0, FDSAN_WARN_ONCE = 1, FDSAN_WARN_ALWAYS = 2, FDSAN_FATAL = 3 };
#define FDSAN_FDS 65536
static _Atomic uint64_t g_fdsan_tag[FDSAN_FDS];
static _Atomic int g_fdsan_level = FDSAN_WARN_ONCE;
static uint64_t klx_android_fdsan_create_owner_tag(int type, uint64_t value) {
    if (value == 0) return 0;
    if ((type & 0xff) != type) { fprintf(stderr, "invalid android_fdsan_owner_type value: %x\n", type); abort(); }
    return ((uint64_t)type << 56) | (value & ((1ull << 56) - 1));
}
static int klx_android_fdsan_get_tag_type(uint64_t tag) { return (int)(tag >> 56); }
static uint64_t klx_android_fdsan_get_tag_value(uint64_t tag) { return tag & ((1ull << 56) - 1); }
static uint64_t klx_android_fdsan_get_owner_tag(int fd) {
    if (fd < 0 || fd >= FDSAN_FDS) return 0;
    return atomic_load(&g_fdsan_tag[fd]);
}
static const char *fdsan_type_name(int t) {
    switch (t) {
    case 0: return "unowned"; case 1: return "FILE*"; case 2: return "DIR*"; case 3: return "unique_fd";
    case 4: return "FileInputStream"; case 5: return "FileOutputStream"; case 6: return "RandomAccessFile";
    case 7: return "ParcelFileDescriptor"; case 8: return "ArtFd"; case 9: return "DatagramSocketImpl";
    case 10: return "SocketImpl"; case 11: return "ZipArchive";
    default: return "native object of unknown type";
    }
}
static void fdsan_error(int fd, uint64_t expected, uint64_t actual, const char *what) {
    int level = atomic_load(&g_fdsan_level);
    if (level == FDSAN_DISABLED) return;
    static _Atomic int said;
    if (level == FDSAN_WARN_ONCE && atomic_exchange(&said, 1)) return;
    char buf[200];
    if (expected && !actual)
        snprintf(buf, sizeof buf, "attempted to %s file descriptor %d with tag %s 0x%llx, but it is not owned",
                 what, fd, fdsan_type_name((int)(expected >> 56)), (unsigned long long)(expected & ((1ull << 56) - 1)));
    else if (!expected && actual)
        snprintf(buf, sizeof buf, "attempted to %s file descriptor %d, expected to be unowned, actually owned by %s 0x%llx",
                 what, fd, fdsan_type_name((int)(actual >> 56)), (unsigned long long)(actual & ((1ull << 56) - 1)));
    else
        snprintf(buf, sizeof buf, "attempted to %s file descriptor %d, expected to be owned by %s 0x%llx, actually owned by %s 0x%llx",
                 what, fd, fdsan_type_name((int)(expected >> 56)), (unsigned long long)(expected & ((1ull << 56) - 1)),
                 fdsan_type_name((int)(actual >> 56)), (unsigned long long)(actual & ((1ull << 56) - 1)));
    fprintf(stderr, "  [fdsan] %s\n", buf);
    if (level == FDSAN_FATAL) { fprintf(stderr, "fdsan: %s\n", buf); abort(); }
}
static void klx_android_fdsan_exchange_owner_tag(int fd, uint64_t expected, uint64_t newtag) {
    if (fd < 0 || fd >= FDSAN_FDS) return;
    uint64_t cur = expected;
    if (!atomic_compare_exchange_strong(&g_fdsan_tag[fd], &cur, newtag)) {
        fdsan_error(fd, expected, cur, "change owner tag of");
        atomic_store(&g_fdsan_tag[fd], newtag);        // as bionic: reported, then the exchange is applied
    }
}
static int klx_android_fdsan_close_with_tag(int fd, uint64_t tag) {
    if (fd >= 0 && fd < FDSAN_FDS) {
        uint64_t cur = tag;
        if (!atomic_compare_exchange_strong(&g_fdsan_tag[fd], &cur, 0)) {
            fdsan_error(fd, tag, cur, "close");
            atomic_store(&g_fdsan_tag[fd], 0);
        }
    }
    return close(fd);          // errno stays Darwin's; klb_errno translates it for the guest
}
static int klx_android_fdsan_get_error_level(void) { return atomic_load(&g_fdsan_level); }
static int klx_android_fdsan_set_error_level(int lvl) { return atomic_exchange(&g_fdsan_level, lvl); }
static int klx_android_fdsan_set_error_level_from_property(int dflt) {
    char v[92] = "";
    klb_sysprop_get("debug.fdsan", v);
    int lvl = dflt;
    if (!strcmp(v, "warn_once")) lvl = FDSAN_WARN_ONCE;
    else if (!strcmp(v, "warn_always")) lvl = FDSAN_WARN_ALWAYS;
    else if (!strcmp(v, "fatal")) lvl = FDSAN_FATAL;
    else if (!strcmp(v, "disabled")) lvl = FDSAN_DISABLED;
    return atomic_exchange(&g_fdsan_level, lvl);
}

// bionic's __progname for an app process is its zygote fork's argv[0] basename.
static const char *g_progname = "app_process64";
static const char *klx_getprogname(void) { return g_progname; }
static void klx_setprogname(const char *n) {
    static char buf[256];
    if (!n) return;
    const char *b = strrchr(n, '/');
    snprintf(buf, sizeof buf, "%s", b ? b + 1 : n);
    g_progname = buf;
}

// ---- liblog extras (API 30) ---------------------------------------------
// struct __android_log_message, as kl_shim.c's kl_android_log_message.
typedef struct { size_t size; int32_t buffer_id; int32_t priority; const char *tag; const char *file;
                 uint32_t line; const char *message; } klx_log_message;
void (*kl_log_custom_logger)(const klx_log_message *);       // __android_log_set_logger; read by kl_shim.c
const char *kl_log_default_tag;                              // __android_log_set_default_tag
static void (*g_log_aborter)(const char *);
static void klx_android_log_set_logger(void (*f)(const klx_log_message *)) { kl_log_custom_logger = f; }
static void klx_android_log_set_default_tag(const char *tag) {
    static char buf[24];
    if (!tag) { kl_log_default_tag = NULL; return; }
    snprintf(buf, sizeof buf, "%s", tag);                    // bionic keeps at most 23 chars
    kl_log_default_tag = buf;
}
static void klx_android_log_set_aborter(void (*f)(const char *)) { g_log_aborter = f; }
static void klx_android_log_call_aborter(const char *msg) {
    if (g_log_aborter) g_log_aborter(msg);
    else { fprintf(stderr, "  [liblog] abort: %s\n", msg ? msg : ""); abort(); }
}
static void klx_android_log_stderr_logger(const klx_log_message *m) {
    struct tm now; time_t t = time(NULL); localtime_r(&t, &now);
    char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &now);
    static const char chars[] = "XXVDIWEF";
    char pc = (m->priority >= 0 && m->priority < 8) ? chars[m->priority] : '?';
    uint64_t tid = 0;
#ifdef __APPLE__
    pthread_threadid_np(NULL, &tid);
#endif
    const char *prog = klx_getprogname();
    if (m->file) fprintf(stderr, "%s %s %5d %5llu %c %-8s: %s:%u %s\n", prog ? prog : "?", ts, (int)getpid(),
                         (unsigned long long)tid, pc, m->tag ? m->tag : "nullptr", m->file, m->line, m->message);
    else fprintf(stderr, "%s %s %5d %5llu %c %-8s: %s\n", prog ? prog : "?", ts, (int)getpid(),
                 (unsigned long long)tid, pc, m->tag ? m->tag : "nullptr", m->message);
}
// logd's own logger is the default one: the shim's log write (kl_shim.c) is what prints.
static void klx_android_log_logd_logger(const klx_log_message *m) {
    fprintf(stderr, "[%d/%s] %s\n", m->priority, m->tag ? m->tag : "", m->message ? m->message : "");
}
static void klx_android_log_close(void) {}
// Binary event log: there is no event buffer to read here; the calls report the bytes
// they were handed, as a successful write to logd does.
static int klx_android_log_bwrite(int32_t tag, const void *payload, size_t len) { (void)tag; (void)payload; return (int)len; }
static int klx_android_log_btwrite(int32_t tag, int type, const void *payload, size_t len) { (void)tag; (void)type; (void)payload; return (int)len; }
static int klx_android_log_bswrite(int32_t tag, const char *payload) { (void)tag; return payload ? (int)strlen(payload) : -22; }
static int klx_android_log_error_write(int tag, const char *subtag, int32_t uid, const char *data, uint32_t len) {
    (void)tag; (void)subtag; (void)uid; (void)data; return (int)len;
}

// ---- fortify (_chk) leftovers --------------------------------------------
static void fortify_fatal(const char *fmt, const char *fn) {
    fprintf(stderr, "FORTIFY: ");
    fprintf(stderr, fmt, fn);
    fputc('\n', stderr);
    abort();
}
static void *klx___memrchr_chk(const void *s, int c, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented read past end of buffer", "memrchr");
    const unsigned char *p = s;
    while (n--) if (p[n] == (unsigned char)c) return (void *)(p + n);
    return NULL;
}
static char *klx___stpcpy_chk(char *d, const char *s, size_t cap) {
    size_t l = strlen(s);
    if (l >= cap) fortify_fatal("%s: prevented write past end of buffer", "stpcpy");
    memcpy(d, s, l + 1);
    return d + l;
}
static char *klx___stpncpy_chk(char *d, const char *s, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented write past end of buffer", "stpncpy");
    return stpncpy(d, s, n);
}
static char *klx___stpncpy_chk2(char *d, const char *s, size_t n, size_t dcap, size_t scap) {
    if (n > dcap) fortify_fatal("%s: prevented write past end of buffer", "stpncpy");
    if (scap < n && strnlen(s, n) >= scap) fortify_fatal("%s: prevented read past end of buffer", "stpncpy");
    return stpncpy(d, s, n);
}
static size_t klx___strlcpy_chk(char *d, const char *s, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented write past end of buffer", "strlcpy");
    return strlcpy(d, s, n);
}
static size_t klx___strlcat_chk(char *d, const char *s, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented write past end of buffer", "strlcat");
    return strlcat(d, s, n);
}
static int klx___umask_chk(unsigned mode) {
    if (mode & ~0777u) fortify_fatal("%s: called with invalid mode", "umask");
    return umask((mode_t)mode);
}
// fds_size is in bytes; a pollfd is 8 bytes in bionic.
static int klx___ppoll_chk(void *fds, unsigned long nfds, const void *tmo, const void *mask, size_t fds_size) {
    if (fds_size / 8 < nfds) fortify_fatal("%s: prevented write past end of buffer", "ppoll");
    int (*pp)(void *, unsigned long, const void *, const void *) = (int (*)(void *, unsigned long, const void *, const void *))kl_shim_lookup("ppoll");
    return pp(fds, nfds, tmo, mask);
}
static ssize_t klx___sendto_chk(int fd, const void *buf, size_t n, size_t cap, int flags, const void *addr, unsigned alen) {
    if (n > cap) fortify_fatal("%s: prevented read past end of buffer", "sendto");
    ssize_t (*st)(int, const void *, size_t, int, const void *, unsigned) = (ssize_t (*)(int, const void *, size_t, int, const void *, unsigned))kl_shim_lookup("sendto");
    return st(fd, buf, n, flags, addr, alen);
}
static ssize_t klx___readlink_chk(const char *path, char *buf, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented write past end of buffer", "readlink");
    ssize_t (*rl)(const char *, char *, size_t) = (ssize_t (*)(const char *, char *, size_t))kl_shim_lookup("readlink");
    return rl(path, buf, n);
}
static ssize_t klx___readlinkat_chk(int dirfd, const char *path, char *buf, size_t n, size_t cap) {
    if (n > cap) fortify_fatal("%s: prevented write past end of buffer", "readlinkat");
    ssize_t (*rl)(int, const char *, char *, size_t) = (ssize_t (*)(int, const char *, char *, size_t))kl_shim_lookup("readlinkat");
    return rl(dirfd, path, buf, n);
}

// ---- android_dlopen_ext / netid-aware resolver ---------------------------
// android_dlextinfo { uint64_t flags; void *reserved_addr; size_t reserved_size; int relro_fd;
//                     int library_fd; off64_t library_fd_offset; android_namespace_t *library_namespace; }
// The flags that ask for a placement (RESERVED_ADDRESS 0x1/HINT 0x2), a relro file or a library
// by fd (0x10) cannot be honoured by the loader here; the rest (namespace, use-fd-offset) do
// not change what gets loaded. A guest that relies on one of the former gets the load
// refused rather than a library mapped somewhere it did not ask for.
extern void *klb_dlopen(const char *path, int flags);
extern const char *klb_dlerror(void);
static void *klx_android_dlopen_ext(const char *path, int flags, const void *ext) {
    if (ext) {
        uint64_t f = *(const uint64_t *)ext;
        if (f & (0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100)) {
            fprintf(stderr, "  [dl] android_dlopen_ext(\"%s\"): flags 0x%llx (reserved address / relro / library fd) are not supported\n",
                    path ? path : "?", (unsigned long long)f);
            return NULL;
        }
    }
    return klb_dlopen(path, flags);
}
// The netid/mark arguments select an Android network; there is one network here.
static int klx_android_getaddrinfofornet(const char *host, const char *serv, const void *hints,
                                         unsigned netid, unsigned mark, void **res) {
    (void)netid; (void)mark;
    int (*gai)(const char *, const char *, const void *, void **) =
        (int (*)(const char *, const char *, const void *, void **))kl_shim_lookup("getaddrinfo");
    return gai ? gai(host, serv, hints, res) : -2;                      // EAI_NONAME... EAI_FAIL
}
static void *klx_android_gethostbynamefornet(const char *name, int af, unsigned netid, unsigned mark) {
    (void)netid; (void)mark;
    void *(*g)(const char *, int) = (void *(*)(const char *, int))kl_shim_lookup("gethostbyname2");
    return g ? g(name, af) : NULL;
}

// ---- small libc leftovers ---------------------------------------------------
static _Thread_local int g_h_errno;
static int *klx___get_h_errno(void) { return &g_h_errno; }
// cpu_set_t allocation (CPU_ALLOC / CPU_FREE expand to these)
static void *klx___sched_cpualloc(size_t count) { return malloc(((count + 63) / 64) * 8); }
static void klx___sched_cpufree(void *set) { free(set); }
static int klx___system_properties_init(void) { return 0; }

// base64 for the resolver (RFC 4648 alphabet, '=' padding), as in OpenBSD's b64_ntop/pton
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int klx___b64_ntop(const unsigned char *src, size_t len, char *dst, size_t size) {
    size_t o = 0;
    while (len >= 3) {
        if (o + 4 > size) return -1;
        dst[o++] = b64chars[src[0] >> 2];
        dst[o++] = b64chars[((src[0] & 3) << 4) | (src[1] >> 4)];
        dst[o++] = b64chars[((src[1] & 15) << 2) | (src[2] >> 6)];
        dst[o++] = b64chars[src[2] & 63];
        src += 3; len -= 3;
    }
    if (len) {
        if (o + 4 > size) return -1;
        dst[o++] = b64chars[src[0] >> 2];
        if (len == 1) { dst[o++] = b64chars[(src[0] & 3) << 4]; dst[o++] = '='; }
        else { dst[o++] = b64chars[((src[0] & 3) << 4) | (src[1] >> 4)]; dst[o++] = b64chars[(src[1] & 15) << 2]; }
        dst[o++] = '=';
    }
    if (o >= size) return -1;
    dst[o] = 0;
    return (int)o;
}
static int klx___b64_pton(const char *src, unsigned char *target, size_t targsize) {
    int tarindex = 0, state = 0, ch;
    const char *pos;
    while ((ch = (unsigned char)*src++) != '\0') {
        if (isspace(ch)) continue;
        if (ch == '=') break;
        pos = strchr(b64chars, ch);
        if (pos == NULL) return -1;
        unsigned v = (unsigned)(pos - b64chars);
        switch (state) {
        case 0:
            if (target) { if ((size_t)tarindex >= targsize) return -1; target[tarindex] = (unsigned char)(v << 2); }
            state = 1; break;
        case 1:
            if (target) { if ((size_t)tarindex + 1 >= targsize) return -1;
                          target[tarindex] |= v >> 4; target[tarindex + 1] = (unsigned char)((v & 0x0f) << 4); }
            tarindex++; state = 2; break;
        case 2:
            if (target) { if ((size_t)tarindex + 1 >= targsize) return -1;
                          target[tarindex] |= v >> 2; target[tarindex + 1] = (unsigned char)((v & 0x03) << 6); }
            tarindex++; state = 3; break;
        case 3:
            if (target) { if ((size_t)tarindex >= targsize) return -1; target[tarindex] |= v; }
            tarindex++; state = 0; break;
        }
    }
    if (ch == '=') {
        ch = (unsigned char)*src++;
        switch (state) {
        case 0: case 1: return -1;
        case 2:
            for (; ch != '\0'; ch = (unsigned char)*src++) if (!isspace(ch)) break;
            if (ch != '=') return -1;
            ch = (unsigned char)*src++;
            /* fall through */
        case 3:
            for (; ch != '\0'; ch = (unsigned char)*src++) if (!isspace(ch)) return -1;
            if (target && (size_t)tarindex < targsize && target[tarindex] != 0) return -1;
        }
    } else if (state != 0) return -1;
    return tarindex;
}

// sys_siglist: bionic's table, indexed by LINUX signal number.
static const char *const klx_sys_siglist[65] = {
    [0] = NULL, [1] = "Hangup", [2] = "Interrupt", [3] = "Quit", [4] = "Illegal instruction", [5] = "Trap",
    [6] = "Aborted", [7] = "Bus error", [8] = "Floating point exception", [9] = "Killed",
    [10] = "User defined signal 1", [11] = "Segmentation fault", [12] = "User defined signal 2",
    [13] = "Broken pipe", [14] = "Alarm clock", [15] = "Terminated", [16] = "Stack fault",
    [17] = "Child exited", [18] = "Continue", [19] = "Stopped (signal)", [20] = "Stopped",
    [21] = "Stopped (tty input)", [22] = "Stopped (tty output)", [23] = "Urgent I/O condition",
    [24] = "CPU time limit exceeded", [25] = "File size limit exceeded", [26] = "Virtual timer expired",
    [27] = "Profiling timer expired", [28] = "Window size changed", [29] = "I/O possible",
    [30] = "Power failure", [31] = "Bad system call",
};
static void klx__Exit(int status) { _exit(status); }
static int klx_atexit(void (*fn)(void)) { return atexit(fn); }

// ===== EXTERNS =====
DECLX(klb_pthread_mutex_timedlock)
DECLX(klb_pthread_mutex_timedlock_monotonic_np)
DECLX(klb_pthread_mutex_clocklock)
DECLX(klb_pthread_mutex_lock_timeout_np)
DECLX(klb_pthread_mutex_consistent)
DECLX(klb_pthread_rwlock_timedrdlock)
DECLX(klb_pthread_rwlock_timedwrlock)
DECLX(klb_pthread_rwlock_clockrdlock)
DECLX(klb_pthread_rwlock_clockwrlock)
DECLX(klb_pthread_rwlockattr_init)
DECLX(klb_pthread_rwlockattr_destroy)
DECLX(klb_pthread_rwlockattr_setpshared)
DECLX(klb_pthread_rwlockattr_getpshared)
DECLX(klb_pthread_rwlockattr_setkind_np)
DECLX(klb_pthread_rwlockattr_getkind_np)
DECLX(klb_pthread_cond_timedwait_monotonic_np)
DECLX(klb_pthread_cond_timeout_np)
DECLX(klb_pthread_condattr_getclock)
DECLX(klb_pthread_condattr_setpshared)
DECLX(klb_pthread_condattr_getpshared)
DECLX(klb_pthread_mutexattr_gettype)
DECLX(klb_pthread_mutexattr_setpshared)
DECLX(klb_pthread_mutexattr_getpshared)
DECLX(klb_pthread_mutexattr_setprotocol)
DECLX(klb_pthread_mutexattr_getprotocol)
DECLX(klb_pthread_spin_init)
DECLX(klb_pthread_spin_destroy)
DECLX(klb_pthread_spin_trylock)
DECLX(klb_pthread_spin_lock)
DECLX(klb_pthread_spin_unlock)
DECLX(klb_pthread_barrierattr_init)
DECLX(klb_pthread_barrierattr_destroy)
DECLX(klb_pthread_barrierattr_setpshared)
DECLX(klb_pthread_barrierattr_getpshared)
DECLX(klb_pthread_barrier_init)
DECLX(klb_pthread_barrier_destroy)
DECLX(klb_pthread_barrier_wait)
DECLX(klb_sem_clockwait)
DECLX(klb_sem_unlink)
DECLX(klb_pthread_gettid_np)
DECLX(klb_pthread_getname_np)
DECLX(klb_pthread_getcpuclockid)
DECLX(klb_sysprop_serial)
DECLX(klb_sysprop_read_callback)
DECLX(klb_sysprop_find_nth)
DECLX(klb_sysprop_foreach)
DECLX(klb_sysprop_set)
DECLX(klb_sysprop_update)
DECLX(klb_sysprop_add)
DECLX(klb_sysprop_area_init)
DECLX(klb_sysprop_set_filename)
DECLX(klb_sysprop_area_serial)
DECLX(klb_sysprop_wait)
DECLX(klb_sysprop_wait_any)
// ===== END EXTERNS =====

// ===== TABLE =====
#define XK(n) { #n, (void *)klx_##n }
static const kl_xentry g_extra[] = {
    // ---- math: direct (Darwin's routine has bionic's signature and results) ----
    X(floor), X(floorf), X(ceilf), X(round), X(roundf), X(rint), X(rintf),
    X(fma), X(fmaf), X(fmax), X(fmaxf), X(fmin), X(fminf),
    X(llrint), X(llrintf), X(llround), X(llroundf), X(lround), X(lroundf),
    X(nearbyint), X(remquo), X(remquof), X(j0), X(j1), X(jn), X(y0), X(y1), X(yn),
    X(tgamma), X(tgammaf), X(lgamma_r), X(lgammaf_r), X(scalbln), X(scalblnf),
    // ---- math: translated ----
    XK(ilogb), XK(ilogbf), XK(ilogbl), XK(j0f), XK(j1f), XK(jnf), XK(y0f), XK(y1f), XK(ynf),
    XK(gamma), XK(gammaf), XK(gamma_r), XK(gammaf_r), XK(scalb), XK(scalbf),
    XK(drem), XK(dremf), XK(significand), XK(significandf),
    XK(exp10), XK(exp10f), XW("pow10", klx_exp10), XW("pow10f", klx_exp10f), XK(exp10l),
    XW("pow10l", klx_exp10l),
    XK(__fpclassifyd), XK(__fpclassifyf), XK(__fpclassifyl),
    XW("__fpclassify", klx___fpclassifyd),
    XK(__isnormal), XK(__isnormalf), XK(__isnormall), XK(__isfinitel), XK(__isinfl), XK(__isnanl),
    XK(__signbit), XK(__signbitf), XK(__signbitl),
    XK(isinf), XK(isinff), XK(isnanf), XK(finite), XK(finitef),
    XW("isinfl", klx___isinfl), XW("isnanl", klx___isnanl), XW("finitel", klx___isfinitel),
    // ---- math: long double (binary128) ----
    XK(acosl), XK(acoshl), XK(asinl), XK(asinhl), XK(atanl), XK(atanhl), XK(cbrtl), XK(ceill),
    XK(cosl), XK(coshl), XK(erfl), XK(erfcl), XK(exp2l), XK(expl), XK(expm1l), XK(floorl),
    XK(log10l), XK(log1pl), XK(log2l), XK(logbl), XK(logl), XK(nearbyintl), XK(rintl), XK(roundl),
    XK(sinl), XK(sinhl), XK(sqrtl), XK(tanl), XK(tanhl), XK(tgammal), XK(lgammal),
    XK(atan2l), XK(copysignl), XK(fmaxl), XK(fminl), XK(fmodl), XK(hypotl), XK(nextafterl),
    XK(powl), XK(remainderl), XK(nexttowardl), XK(truncl), XK(fmal), XK(ldexpl), XK(scalbnl),
    XK(scalblnl), XK(frexpl), XK(nanl), XK(remquol), XK(lgammal_r), XK(sincosl),
    XK(lrintl), XK(llrintl), XK(lroundl), XK(llroundl),
    // ---- string / ctype / locale ----
    XK(bcmp), XK(bcopy), XK(bzero), XK(rindex), XK(explicit_bzero), XK(memset_explicit),
    XK(memccpy), X(memmem), XK(mempcpy), X(stpncpy), XK(strndup), XK(ffs), XK(ffsl), XK(ffsll),
    XK(swab), XK(toascii), XK(strverscmp), XK(strcasecmp_l), XK(strncasecmp_l),
    XK(isalnum_l), XK(isalpha_l), XK(isblank_l), XK(iscntrl_l), XK(isgraph_l), XK(isprint_l),
    XK(ispunct_l), XK(isspace_l), XK(isblank), XK(iswalnum_l), XK(iswgraph_l), XK(iswgraph),
    XK(iswctype_l), XK(wctrans), XK(wctrans_l), XK(towctrans), XK(towctrans_l), XK(wctype_l),
    XK(strtol_l), XK(strtoul_l), XK(strtoll_l), XK(strtoimax), XK(strtoumax), XK(strtoq), XK(strtouq),
    XK(wcstol_l), XK(wcstoll_l), XK(wcstoul_l), XK(wcstoull_l), XK(wcstof_l), XK(wcstod_l),
    XK(wcstoimax), XK(wcstoumax),
    XK(labs), XK(llabs), XK(imaxabs), XK(ldiv), XK(imaxdiv),
    XK(wcpcpy), XK(wcpncpy), XK(wcscat), XK(wcscspn), XK(wcsdup), XK(wcslcpy), XK(wcslcat),
    XK(wcsnlen), XK(wcspbrk), XK(wcsspn), XK(wcsstr), XK(wmempcpy), XK(wcscasecmp),
    XK(wcsncasecmp), XK(wcscasecmp_l), XK(wcsncasecmp_l), XK(wcwidth), XK(wcswidth),
    XK(mbrtowc), XK(wcrtomb), XK(mbsinit), XK(mblen), XK(mbtowc), XK(wctomb), XK(mbsnrtowcs),
    XK(mbsrtowcs), XK(wcsnrtombs), XK(wcsrtombs), XK(mbstowcs), XK(wcstombs), XK(btowc), XK(wctob),
    XK(mbrtoc32), XK(mbrtoc16), XK(c32rtomb), XK(c16rtomb),
    XK(nl_langinfo), XK(nl_langinfo_l),
    // ---- Android-only libc ----
    XK(android_get_device_api_level), XK(android_get_application_target_sdk_version),
    XK(android_fdsan_create_owner_tag), XK(android_fdsan_get_tag_type), XK(android_fdsan_get_tag_value),
    XK(android_fdsan_get_owner_tag), XK(android_fdsan_exchange_owner_tag), XK(android_fdsan_close_with_tag),
    XK(android_fdsan_get_error_level), XK(android_fdsan_set_error_level),
    XK(android_fdsan_set_error_level_from_property),
    XW("__android_log_set_logger", klx_android_log_set_logger),
    XW("__android_log_set_default_tag", klx_android_log_set_default_tag),
    XW("__android_log_set_aborter", klx_android_log_set_aborter),
    XW("__android_log_call_aborter", klx_android_log_call_aborter),
    XW("__android_log_stderr_logger", klx_android_log_stderr_logger),
    XW("__android_log_logd_logger", klx_android_log_logd_logger),
    XW("__android_log_close", klx_android_log_close),
    XW("__android_log_bwrite", klx_android_log_bwrite), XW("__android_log_btwrite", klx_android_log_btwrite),
    XW("__android_log_bswrite", klx_android_log_bswrite), XW("__android_log_error_write", klx_android_log_error_write),
    XK(__memrchr_chk), XK(__stpcpy_chk), XK(__stpncpy_chk), XK(__stpncpy_chk2), XK(__strlcpy_chk), XK(__strlcat_chk),
    XK(__umask_chk), XK(__ppoll_chk), XW("__ppoll64_chk", klx___ppoll_chk), XK(__sendto_chk),
    XK(__readlink_chk), XK(__readlinkat_chk),
    XK(__get_h_errno), XK(__sched_cpualloc), XK(__sched_cpufree), XK(__system_properties_init),
    XK(__b64_ntop), XK(__b64_pton), XW("sys_siglist", klx_sys_siglist),
    XK(android_getaddrinfofornet), XK(android_gethostbynamefornet), XK(android_dlopen_ext),
    XK(getprogname), XK(setprogname), XK(_Exit), XK(atexit), X(quick_exit), X(at_quick_exit), X(arc4random_uniform),
    // ---- pthread / semaphore (kl_pthread.c) ----
    // ---- system properties (kl_libc.c) ----
    // ===== SYSPROP ROWS =====
    XE("__system_property_serial", klb_sysprop_serial)
    XE("__system_property_read_callback", klb_sysprop_read_callback)
    XE("__system_property_find_nth", klb_sysprop_find_nth)
    XE("__system_property_foreach", klb_sysprop_foreach)
    XE("__system_property_set", klb_sysprop_set)
    XE("__system_property_update", klb_sysprop_update)
    XE("__system_property_add", klb_sysprop_add)
    XE("__system_property_area_init", klb_sysprop_area_init)
    XE("__system_property_set_filename", klb_sysprop_set_filename)
    XE("__system_property_area_serial", klb_sysprop_area_serial)
    XE("__system_property_wait", klb_sysprop_wait)
    XE("__system_property_wait_any", klb_sysprop_wait_any)
    // ===== END SYSPROP ROWS =====
    // ===== PTHREAD ROWS =====
    XE("pthread_mutex_timedlock", klb_pthread_mutex_timedlock)
    XE("pthread_mutex_timedlock_monotonic_np", klb_pthread_mutex_timedlock_monotonic_np)
    XE("pthread_mutex_clocklock", klb_pthread_mutex_clocklock)
    XE("pthread_mutex_lock_timeout_np", klb_pthread_mutex_lock_timeout_np)
    XE("pthread_mutex_consistent", klb_pthread_mutex_consistent)
    XE("pthread_rwlock_timedrdlock", klb_pthread_rwlock_timedrdlock)
    XE("pthread_rwlock_timedwrlock", klb_pthread_rwlock_timedwrlock)
    XE("pthread_rwlock_clockrdlock", klb_pthread_rwlock_clockrdlock)
    XE("pthread_rwlock_clockwrlock", klb_pthread_rwlock_clockwrlock)
    XE("pthread_rwlockattr_init", klb_pthread_rwlockattr_init)
    XE("pthread_rwlockattr_destroy", klb_pthread_rwlockattr_destroy)
    XE("pthread_rwlockattr_setpshared", klb_pthread_rwlockattr_setpshared)
    XE("pthread_rwlockattr_getpshared", klb_pthread_rwlockattr_getpshared)
    XE("pthread_rwlockattr_setkind_np", klb_pthread_rwlockattr_setkind_np)
    XE("pthread_rwlockattr_getkind_np", klb_pthread_rwlockattr_getkind_np)
    XE("pthread_cond_timedwait_monotonic_np", klb_pthread_cond_timedwait_monotonic_np)
    XE("pthread_cond_timeout_np", klb_pthread_cond_timeout_np)
    XE("pthread_condattr_getclock", klb_pthread_condattr_getclock)
    XE("pthread_condattr_setpshared", klb_pthread_condattr_setpshared)
    XE("pthread_condattr_getpshared", klb_pthread_condattr_getpshared)
    XE("pthread_mutexattr_gettype", klb_pthread_mutexattr_gettype)
    XE("pthread_mutexattr_setpshared", klb_pthread_mutexattr_setpshared)
    XE("pthread_mutexattr_getpshared", klb_pthread_mutexattr_getpshared)
    XE("pthread_mutexattr_setprotocol", klb_pthread_mutexattr_setprotocol)
    XE("pthread_mutexattr_getprotocol", klb_pthread_mutexattr_getprotocol)
    XE("pthread_spin_init", klb_pthread_spin_init)
    XE("pthread_spin_destroy", klb_pthread_spin_destroy)
    XE("pthread_spin_trylock", klb_pthread_spin_trylock)
    XE("pthread_spin_lock", klb_pthread_spin_lock)
    XE("pthread_spin_unlock", klb_pthread_spin_unlock)
    XE("pthread_barrierattr_init", klb_pthread_barrierattr_init)
    XE("pthread_barrierattr_destroy", klb_pthread_barrierattr_destroy)
    XE("pthread_barrierattr_setpshared", klb_pthread_barrierattr_setpshared)
    XE("pthread_barrierattr_getpshared", klb_pthread_barrierattr_getpshared)
    XE("pthread_barrier_init", klb_pthread_barrier_init)
    XE("pthread_barrier_destroy", klb_pthread_barrier_destroy)
    XE("pthread_barrier_wait", klb_pthread_barrier_wait)
    XE("sem_clockwait", klb_sem_clockwait)
    XE("sem_unlink", klb_sem_unlink)
    XE("pthread_gettid_np", klb_pthread_gettid_np)
    XE("pthread_getname_np", klb_pthread_getname_np)
    XE("pthread_getcpuclockid", klb_pthread_getcpuclockid)
    // ===== END PTHREAD ROWS =====
// ===== END TABLE =====
};

// Consulted by kl_shim_lookup / kl_shim_known BEFORE the generated table.
void *kl_bionic_extra_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_extra / sizeof g_extra[0]; i++)
        if (g_extra[i].name[0] == name[0] && strcmp(g_extra[i].name, name) == 0) return g_extra[i].fn;
    return NULL;
}
