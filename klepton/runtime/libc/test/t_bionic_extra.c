// Host-side check of the translation logic in kl_bionic_extra.c (builds on Linux:
//   cc -DKL_EXTRA_TEST -I.. t_bionic_extra.c -lm -o t && ./t
// It includes the implementation file so the static wrappers are reachable.
#include "../kl_bionic_extra.c"
void *kl_shim_lookup(const char *n) { (void)n; return 0; }
long klb_sysconf(int n) { return n; }
int klb_sysprop_get(const char *n, char *v) { (void)n; v[0] = 0; return 0; }
void *klb_dlopen(const char *p, int f) { (void)p; (void)f; return 0; }

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

static void t_android(void) {
    CHECK(klx_android_fdsan_create_owner_tag(3, 0x1234) == ((3ull << 56) | 0x1234));
    CHECK(klx_android_fdsan_create_owner_tag(3, 0) == 0);
    uint64_t tag = klx_android_fdsan_create_owner_tag(3, 77);
    CHECK(klx_android_fdsan_get_tag_type(tag) == 3 && klx_android_fdsan_get_tag_value(tag) == 77);
    int fd = dup(1);
    klx_android_fdsan_exchange_owner_tag(fd, 0, tag);
    CHECK(klx_android_fdsan_get_owner_tag(fd) == tag);
    CHECK(klx_android_fdsan_close_with_tag(fd, tag) == 0);
    CHECK(klx_android_fdsan_get_owner_tag(fd) == 0);
    char o[64]; unsigned char back[32];
    CHECK(klx___b64_ntop((const unsigned char *)"foobar", 6, o, sizeof o) == 8 && !strcmp(o, "Zm9vYmFy"));
    CHECK(klx___b64_ntop((const unsigned char *)"fo", 2, o, sizeof o) == 4 && !strcmp(o, "Zm8="));
    CHECK(klx___b64_ntop((const unsigned char *)"f", 1, o, sizeof o) == 4 && !strcmp(o, "Zg=="));
    CHECK(klx___b64_ntop((const unsigned char *)"foobar", 6, o, 8) == -1);
    CHECK(klx___b64_pton("Zm9vYmFy", back, sizeof back) == 6 && !memcmp(back, "foobar", 6));
    CHECK(klx___b64_pton("Zm8=", back, sizeof back) == 2 && !memcmp(back, "fo", 2));
    CHECK(klx___b64_pton("Zg==", back, sizeof back) == 1 && back[0] == 'f');
    CHECK(klx___b64_pton("Zm8", back, sizeof back) == -1);
    CHECK(klx___b64_pton("Zm$=", back, sizeof back) == -1);
    CHECK(!strcmp(klx_sys_siglist[10], "User defined signal 1") && !strcmp(klx_sys_siglist[31], "Bad system call"));
    char d[8]; CHECK(klx___stpcpy_chk(d, "abc", 8) == d + 3);
    CHECK(klx___strlcpy_chk(d, "abcdefghij", 8, 8) == 10 && !strcmp(d, "abcdefg"));
    { const char *hay = "abcabc"; CHECK(klx___memrchr_chk(hay, 'b', 6, 6) == (void *)(hay + 4)); }
    CHECK(!strcmp(klx_getprogname(), "app_process64")); klx_setprogname("/x/y/foo"); CHECK(!strcmp(klx_getprogname(), "foo"));
    klx_android_log_set_default_tag("tagtagtagtagtagtagtagtagtag");
    CHECK(strlen(kl_log_default_tag) == 23);
    CHECK(klx_android_get_device_api_level() == 32);
}

static void t_accounts(void) {
    bpasswd *p = klx_getpwuid(0); CHECK(p && !strcmp(p->pw_name, "root") && !strcmp(p->pw_dir, "/"));
    p = klx_getpwuid(10123); CHECK(p && !strcmp(p->pw_name, "u0_a123") && p->pw_gid == 10123 && !strcmp(p->pw_dir, "/data"));
    p = klx_getpwnam("u0_a7"); CHECK(p && p->pw_uid == 10007);
    CHECK(klx_getpwnam("u0_a7x") == NULL && klx_getpwuid(55555) == NULL && klx_getpwnam("nosuch") == NULL);
    p = klx_getpwnam("shell"); CHECK(p && p->pw_uid == 2000 && !strcmp(p->pw_shell, "/system/bin/sh"));
    bpasswd pw, *res; char buf[160];
    CHECK(klx_getpwnam_r("system", &pw, buf, sizeof buf, &res) == 0 && res == &pw && pw.pw_uid == 1000);
    CHECK(klx_getpwnam_r("system", &pw, buf, 4, &res) == 34 && res == NULL);
    CHECK(klx_getpwnam_r("zzz", &pw, buf, sizeof buf, &res) == 0 && res == NULL);
    bgroup *g = klx_getgrgid(2001); CHECK(g && !strcmp(g->gr_name, "cache") && g->gr_gid == 2001 && !strcmp(g->gr_mem[0], "cache") && !g->gr_mem[1]);
    bgroup gr, *gres; char gbuf[160];
    CHECK(klx_getgrnam_r("inet", &gr, gbuf, sizeof gbuf, &gres) == 0 && gres == &gr && gr.gr_gid == 3003);
    int n = 0; klx_setpwent(); while (klx_getpwent()) n++; CHECK(n == (int)N_AIDS);
    char dn[8]; CHECK(klx_getdomainname(dn, sizeof dn) == 0 && !strcmp(dn, "(none)") && klx_getdomainname(dn, 3) == -1);
    kl_spawnattr *sa; CHECK(klx_posix_spawnattr_init(&sa) == 0);
    short fl; CHECK(klx_posix_spawnattr_setflags(&sa, 0x4) == 0 && klx_posix_spawnattr_getflags(&sa, &fl) == 0 && fl == 4);
    CHECK(klx_posix_spawnattr_setflags(&sa, 0x1000) == 22);
    CHECK(klx_posix_spawnattr_destroy(&sa) == 0 && sa == NULL);
    setenv("KLX_A", "1", 1); setenv("KLX_B", "2", 1); CHECK(klx_clearenv() == 0 && getenv("KLX_A") == NULL);
}

static void t_table(void) {
    size_t n = sizeof g_extra / sizeof g_extra[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) if (!strcmp(g_extra[i].name, g_extra[j].name)) { printf("duplicate table name %s\n", g_extra[i].name); bad++; }
    }
    CHECK(kl_bionic_extra_lookup("floorl") == NULL);       // not a bionic export: must not be served
    CHECK(kl_bionic_extra_lookup("pthread_cancel") == NULL);
    CHECK(kl_bionic_extra_lookup("exp10l") != NULL && kl_bionic_extra_lookup("nosuchsym") == NULL);
}

int main(void) {
    t_table();
    t_math(); t_str(); t_utf8(); t_android(); t_accounts();
#ifdef T_EXTRA_MORE
    t_more();
#endif
    printf(bad ? "bionic_extra: %d FAILED\n" : "bionic_extra: all ok\n", bad);
    return bad != 0;
}
