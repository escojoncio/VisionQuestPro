// Host-side check of the translation logic in kl_bionic_extra.c (builds on Linux:
//   cc -DKL_EXTRA_TEST -I.. t_bionic_extra.c -lm -o t && ./t
// It includes the implementation file so the static wrappers are reachable.
#include "../kl_bionic_extra.c"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); bad++; } } while (0)

static void t_math(void) {
    CHECK(klx_ilogb(0.0) == -INT_MAX);
    CHECK(klx_ilogb(NAN) == INT_MAX);
    CHECK(klx_ilogb(INFINITY) == INT_MAX);
    CHECK(klx_ilogb(8.0) == 3);
    CHECK(klx___fpclassifyd(0.0) == 0x10);
    CHECK(klx___fpclassifyd(1.0) == 0x04);
    CHECK(klx___fpclassifyd(5e-324) == 0x08);
    CHECK(klx___fpclassifyd(INFINITY) == 0x01);
    CHECK(klx___fpclassifyd(NAN) == 0x02);
    CHECK(klx___fpclassifyf(1e-45f) == 0x08);
    CHECK(klx___fpclassifyl(Q(1.0)) == 0x04);
    kl_q big = { 0, ((uint64_t)(16383 + 5000)) << 48 };    // 2^5000: finite as a quad
    CHECK(klx___fpclassifyl(ld_out(big)) == 0x04);
    CHECK(klx___isfinitel(ld_out(big)) == 1);
    CHECK(klx___isinfl(ld_out(big)) == 0);
    CHECK(klx___signbitl(Q(-2.0)) == 1);
    CHECK(klx_isinf(-INFINITY) == 1);
    CHECK(klx_finite(3.0) == 1 && klx_finite(NAN) == 0);
    CHECK(D(klx_sqrtl(Q(16.0))) == 4.0);
    CHECK(D(klx_powl(Q(2.0), Q(10.0))) == 1024.0);
    CHECK(klx_lrintl(Q(2.5)) == 2);
    CHECK(klx_llroundl(Q(-2.5)) == -3);
    int e; CHECK(D(klx_frexpl(Q(8.0), &e)) == 0.5 && e == 4);
    CHECK(D(klx_ldexpl(Q(0.5), 4)) == 8.0);
    CHECK(D(klx_truncl(Q(-2.7))) == -2.0);
    CHECK(klx_scalb(3.0, 2.0) == 12.0);
    CHECK(klx_significand(12.0) == 1.5);
    CHECK(klx_exp10(3.0) == 1000.0);
    CHECK(fabs(klx_j0f(1.0f) - 0.7651977f) < 1e-6);
}

static void t_str(void) {
    CHECK(klx_strverscmp("a1", "a2") < 0);
    CHECK(klx_strverscmp("a10", "a9") > 0);
    CHECK(klx_strverscmp("a007", "a07") < 0 || klx_strverscmp("a007", "a07") > 0);
    CHECK(klx_strverscmp("foo", "foo") == 0);
    CHECK(klx_strverscmp("1.0.10", "1.0.9") > 0);
    CHECK(klx_ffs(0) == 0 && klx_ffs(8) == 4);
    char buf[8]; CHECK(klx_memccpy(buf, "abc", 'b', 3) == buf + 2);
    CHECK(klx_memccpy(buf, "abc", 'z', 3) == NULL);
    char *d = klx_strndup("hello", 3); CHECK(d && !strcmp(d, "hel")); free(d);
    CHECK(klx_wcwidth(L'a') == 1 && klx_wcwidth(0x4E2D) == 2 && klx_wcwidth(0x0301) == 0 && klx_wcwidth(7) == -1);
    wchar_t w[8]; CHECK(klx_wcslcpy(w, L"abcdef", 4) == 6 && !wcscmp(w, L"abc"));
    CHECK(klx_wcscasecmp(L"AbC", L"aBc") == 0 && klx_wcsncasecmp(L"abcx", L"ABCy", 3) == 0);
    CHECK(!strcmp(klx_nl_langinfo(1), "UTF-8"));
    CHECK(!strcmp(klx_nl_langinfo(8), "Sunday") && !strcmp(klx_nl_langinfo(45), "Dec"));
}

static void t_utf8(void) {
    bmbstate st; memset(&st, 0, sizeof st);
    wchar_t wc = 0;
    CHECK(klx_mbrtowc(&wc, "\xC3\xA9", 2, &st) == 2 && wc == 0xE9);
    CHECK(klx_mbrtowc(&wc, "\xE2\x82\xAC", 3, &st) == 3 && wc == 0x20AC);
    CHECK(klx_mbrtowc(&wc, "\xF0\x9F\x98\x80", 4, &st) == 4 && wc == 0x1F600);
    // split sequence across calls
    CHECK(klx_mbrtowc(&wc, "\xE2\x82", 2, &st) == (size_t)-2 && !klx_mbsinit(&st));
    CHECK(klx_mbrtowc(&wc, "\xAC", 1, &st) == 1 && wc == 0x20AC && klx_mbsinit(&st));
    CHECK(klx_mbrtowc(&wc, "\xC0\x80", 2, &st) == (size_t)-1);          // overlong
    CHECK(klx_mbrtowc(&wc, "\xED\xA0\x80", 3, &st) == (size_t)-1);      // surrogate
    CHECK(klx_mbrtowc(&wc, "\xF4\x90\x80\x80", 4, &st) == (size_t)-1);  // > U+10FFFF
    CHECK(klx_mbrtowc(&wc, "", 1, &st) == 0 && wc == 0);
    char o[4];
    CHECK(klx_wcrtomb(o, 0x20AC, &st) == 3 && !memcmp(o, "\xE2\x82\xAC", 3));
    CHECK(klx_wcrtomb(o, 0xD800, &st) == (size_t)-1);
    CHECK(klx_mblen("\xC3\xA9", 2) == 2 && klx_mblen("a", 1) == 1 && klx_mblen("\xC3", 1) == -1);
    wchar_t out[8]; const char *src = "h\xC3\xA9llo";
    CHECK(klx_mbstowcs(out, src, 8) == 5 && out[1] == 0xE9 && out[5] == 0);
    CHECK(klx_mbstowcs(NULL, src, 0) == 5);
    char nb[16]; CHECK(klx_wcstombs(nb, out, 16) == 6 && !strcmp(nb, src));
    CHECK(klx_wcstombs(NULL, out, 0) == 6);
    CHECK(klx_wcstombs(nb, out, 2) == 1);                                // 'h' fits, the 2-byte e-acute does not
    CHECK(klx_btowc(0x41) == 0x41 && klx_btowc(0xE9) == (wint_t)WEOF && klx_wctob(0xE9) == EOF);
    // C11
    uint16_t c16; uint32_t c32;
    CHECK(klx_mbrtoc32(&c32, "\xF0\x9F\x98\x80", 4, &st) == 4 && c32 == 0x1F600);
    memset(&st, 0, sizeof st);
    CHECK(klx_mbrtoc16(&c16, "\xF0\x9F\x98\x80", 4, &st) == 4 && c16 == 0xD83D);
    CHECK(klx_mbrtoc16(&c16, "", 0, &st) == (size_t)-3 && c16 == 0xDE00);
    memset(&st, 0, sizeof st);
    CHECK(klx_c16rtomb(o, 0xD83D, &st) == 0);
    CHECK(klx_c16rtomb(o, 0xDE00, &st) == 4 && !memcmp(o, "\xF0\x9F\x98\x80", 4));
    CHECK(klx_c32rtomb(o, 0x110000, &st) == (size_t)-1);
}

int main(void) {
    t_math(); t_str(); t_utf8();
#ifdef T_EXTRA_MORE
    t_more();
#endif
    printf(bad ? "bionic_extra: %d FAILED\n" : "bionic_extra: all ok\n", bad);
    return bad != 0;
}
