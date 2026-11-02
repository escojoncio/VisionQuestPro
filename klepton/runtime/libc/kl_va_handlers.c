// C handlers behind the asm thunks in kl_va_thunks.S. Each receives the guest's
#include <stdint.h>
// named arguments normally plus an AAPCS64 kl_va*, re-marshals into Darwin layout,
// and calls the host implementation. The host's printf/scanf engine does the
// actual formatting -- we never reimplement it.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "kl_va.h"
#include "klepton.h"

// Darwin arm64's va_list is a bare char*, so a marshalled buffer *is* a va_list.
#define KL_MARSHAL(fmt, va, mode)                                        \
    char _m[512] __attribute__((aligned(16)));                           \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, (mode)) == (size_t)-1)  \
        return -1;                                                       \
    va_list _ap = (va_list)_m

int klh_printf(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vprintf(fmt, _ap);
}
int klh_fprintf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vfprintf(kl_host_file(f), fmt, _ap);
}
int klh_sprintf(char *buf, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsprintf(buf, fmt, _ap);
}
int klh_snprintf(char *buf, size_t n, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsnprintf(buf, n, fmt, _ap);
}
int klh_asprintf(char **out, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vasprintf(out, fmt, _ap);
}
int klh_dprintf(int fd, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vdprintf(fd, fmt, _ap);
}
int klh_syslog(int pri, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fprintf(stderr, "[guest syslog %d] ", pri);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
// hl2 (HL2Q3VR): force-call the mod's CreateRenderTargets (libsourcevr 0xec64) on
// g_pSourceVR (libclient global 0xcce4a8), once, the moment VR activates. The engine's
// normal caller is a C++ virtual dispatch we could not trace and it never fires under
// Klepton, so the VR eye render targets (color/depth/gui/menu/scope/intro) are never
// created -> "Eye copy skipped: Source render target is unavailable" -> black. Addresses
// via radare2: g_pSourceVR is stored at libclient+0xcce4a8 right after
// CreateInterface("SourceVirtualReality001"); CreateRenderTargets(0xec64) fetches the
// material system itself, so it takes only `this`. Gated by KL_HL2_FORCERT + target hl2.
// EXPERIMENT: an out-of-context call — if the current thread's GL context is not the
// guest's, or the material system is not ready yet, it may fault. See
// hl2-quest-reference-trace.
// Called from klxr_WaitFrame (kl_openxr.c) — i.e. the guest's render/frame thread with its
// GL context, NOT the log handler (that ran on a worker thread and re-entered logging from
// CreateRenderTargets' own Msg, corrupting the FILE and faulting in fwrite).
void kl_hl2_force_create_rendertargets(void) {
    static int done;
    if (done || !getenv("KL_HL2_FORCERT")) return;
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "hl2") != 0) return;
    extern kl_image *kl_find_image(const char *);
    kl_image *cl = kl_find_image("libclient.so");
    kl_image *sv = kl_find_image("libsourcevr.so");
    kl_image *ms = kl_find_image("libmaterialsystem.so");
    if (!cl || !sv || !ms) { fprintf(stderr, "  [hl2rt] client/sourcevr/matsys not loaded yet\n"); return; }
    void **slot = (void **)((unsigned char *)kl_base(cl) + 0xcce4a8);
    void *sourcevr = *slot;
    if (!sourcevr) { fprintf(stderr, "  [hl2rt] g_pSourceVR NULL — waiting\n"); return; }
    // CreateRenderTargets takes (this, IMaterialSystem*) — matsys is arg1. matsys is the static
    // g_MaterialSystem singleton at libmaterialsystem+0x1b2150 (.bss). Found DETERMINISTICALLY:
    // the InterfaceReg static-init at libmaterialsystem 0xf2884 registers factory 0xe1df0 for
    // "VMaterialSystem081", and that factory is `adrp x0,0x1b2000; add x0,x0,0x150; ret` — i.e.
    // it returns &g_MaterialSystem. This is EXACTLY the object CreateInterface returns and the
    // engine passes to CreateRenderTargets. Its ctor runs at library load, so the vtable is set
    // well before the first frame — no scan (false transient-.data matches), no guest call
    // (fragile factory tail-call / registry .bss read-back-0 / host SIMD strcmp overread), no
    // dependence on libclient wiring timing (its materials globals were still NULL at frame 1).
    extern const char *kl_addr_image(const void *, size_t *);
    size_t off = 0;
    unsigned char *msb = (unsigned char *)kl_base(ms);
    void *matsys = (void *)(msb + 0x1b2150);   // g_MaterialSystem singleton (deterministic)
    // Vtable sanity: a real C++ object's vtable pointer (first word) must land inside its own
    // library's mapping. Verify both objects before we hand control to guest code.
    void *vt_sv = *(void **)sourcevr;
    void *vt_ms = *(void **)matsys;
    const char *own_sv = kl_addr_image(vt_sv, &off);
    const char *own_ms = kl_addr_image(vt_ms, &off);
    fprintf(stderr, "  [hl2rt] sanity: sourcevr=%p vtable=%p in %s | matsys=%p vtable=%p in %s\n",
            sourcevr, vt_sv, own_sv ? own_sv : "(unmapped!)",
            matsys, vt_ms, own_ms ? own_ms : "(unmapped!)");
    if (!own_sv || !strstr(own_sv, "libsourcevr")) {
        fprintf(stderr, "  [hl2rt] ABORT: sourcevr vtable not in libsourcevr — g_pSourceVR wrong\n");
        return;
    }
    if (!own_ms || !strstr(own_ms, "libmaterialsystem")) {
        fprintf(stderr, "  [hl2rt] ABORT: matsys vtable not in libmaterialsystem — CreateInterface wrong\n");
        return;
    }
    // The vtable POINTER being in libmaterialsystem is not enough — the bogus object from the
    // wrong CreateInterface address had exactly that, yet vtable[0x58]=0x232 (garbage) faulted
    // the blr at libsourcevr+0xec18. Validate the actual slot CreateRenderTargets dispatches.
    void *slot58 = *(void **)((unsigned char *)vt_ms + 0x58);
    const char *own_slot = kl_addr_image(slot58, &off);
    fprintf(stderr, "  [hl2rt] sanity: matsys vtable[0x58]=%p in %s\n",
            slot58, own_slot ? own_slot : "(unmapped!)");
    if (!own_slot || !strstr(own_slot, "libmaterialsystem")) {
        fprintf(stderr, "  [hl2rt] ABORT: matsys vtable[0x58] not a real method — wrong interface\n");
        return;
    }
    done = 1;
    // CreateRenderTargets entry = libsourcevr 0xec80 (re-derived for the CURRENT deployed APK —
    // hl2.apk 2026-08-30; earlier 0xebe4/0xec64 were from a STALE build and every forced call ran
    // garbage code, which is what produced the phantom 0x232 crashes and "deadlocks"). AAPCS
    // prologue at 0xec80: sub sp,0x70; stp x29/x30; mov x20,x1=matsys; mov x19,x0=this;
    // bl 0xf058 (cleanup); str x20,[x19,0x2c0]; cbz x20 -> "without material system"; then it
    // dispatches matsys->vtable[0x58]. Found via string xref (retained@0x4744, guard@0x8063).
    void (*fn)(void *, void *) = (void (*)(void *, void *))((unsigned char *)kl_base(sv) + 0xec80);
    unsigned char *svo = (unsigned char *)sourcevr;
    fprintf(stderr, "  [hl2rt] sourcevr fields: 2c0(matsys)=%p 2c8=%p 2d0=%p 2d8=%p 2e0=%p\n",
            *(void **)(svo + 0x2c0), *(void **)(svo + 0x2c8),
            *(void **)(svo + 0x2d0), *(void **)(svo + 0x2d8), *(void **)(svo + 0x2e0));
    // The RTs come back NULL unless the material system is inside a BeginRenderTargetAllocation/
    // EndRenderTargetAllocation block — CreateNamedRenderTargetTextureEx checks a byte flag at
    // matsys+0x3078 (libmaterialsystem 0xeb938: mov w8,0x3078; ldrb w8,[this,x8]; cbz -> "Tried
    // to create render target outside of ... block"). The engine brackets its CreateRenderTargets
    // call with matsys->Begin/EndRenderTargetAllocation(); we set the flag those toggle.
    unsigned char *alloc_flag = (unsigned char *)matsys + 0x3078;
    unsigned char saved_flag = *alloc_flag;
    *alloc_flag = 1;
    fprintf(stderr, "  [hl2rt] force CreateRenderTargets(sourcevr=%p, matsys=%p) fn=%p alloc_flag %d->1\n",
            sourcevr, matsys, (void *)fn, saved_flag);
    fn(sourcevr, matsys);
    *alloc_flag = saved_flag;
    fprintf(stderr, "  [hl2rt] CreateRenderTargets returned (alloc_flag restored to %d)\n", saved_flag);
}

// Diagnosis of "the fight does not end / checkpoints reload": the game's own logcat lines that mention a run-state
// word (checkpoint, respawn, death, die, fail, mission, tutorial, combat; plus subshaders, fallback shader, this GPU
// for Unity's unsupported-shader lines), matched case-insensitively at the start of a word: "died", "failed",
// "combatant" count) are copied once more to our log as "[unity-kw] P/tag: text", so they can be
// grepped on their own. Only INFO and above, or any priority of the Unity tag. Capped (KL_LOG_KW_MAX, default 1500;
// 0 = off; the words of the block below have a budget of their own). Called from __android_log_print / __android_log_write; never changes what the guest sees.
/* KL-TEST-BEGIN kwcopy */
// Shader-cache / pipeline-cache / passthrough / anchor words (Unreal's PSO precompile and the MR features) come with
// their own sub-cap per word (KL_KW_SUB lines) and their own budget (KL_KW_NEW_MAX lines in all, g_kw_new_n, at most
// KL_LOG_KW_MAX), apart from the run-state words' g_kw_n, so they can never use up the global cap and push the run-state
// words above out, whether or not they are exhausted. Any priority of tag UE4 / UE is allowed for them (Unreal logs at
// DEBUG); the run-state words keep the INFO-and-above rule.
#define KL_KW_SUB 100
#define KL_KW_NEW_MAX 1000
static const struct { const char *w; int sub; } g_kw[] = {
    { "checkpoint", 0 }, { "respawn", 0 }, { "death", 0 }, { "die", 0 }, { "fail", 0 }, { "mission", 0 },
    { "tutorial", 0 }, { "combat", 0 }, { "subshaders", 0 }, { "fallback shader", 0 }, { "this GPU", 0 },
    { "PipelineCache", KL_KW_SUB }, { "upipelinecache", KL_KW_SUB }, { "ShaderPipelineCache", KL_KW_SUB },
    { "PSO", KL_KW_SUB }, { "precompil", KL_KW_SUB }, { "Shader Cache", KL_KW_SUB }, { "Passthrough", KL_KW_SUB },
    { "OVRManager", KL_KW_SUB }, { "MRUK", KL_KW_SUB }, { "anchor", KL_KW_SUB },
};
#define KL_KW_N (sizeof g_kw / sizeof *g_kw)
static int g_kw_cap = -1;
static int g_kw_n;       // lines printed for the run-state words (sub 0): budget g_kw_cap
static int g_kw_new_n;   // lines printed for the words with a sub-cap: budget kl_kw_new_cap()
static int g_kw_sub_n[KL_KW_N];
static int kl_kw_new_cap(void) { return g_kw_cap < KL_KW_NEW_MAX ? g_kw_cap : KL_KW_NEW_MAX; }
int kl_log_keyword_open(void) {
    if (g_kw_cap < 0) {
        const char *e = getenv("KL_LOG_KW_MAX");
        g_kw_cap = e && *e ? atoi(e) : 1500;
        if (g_kw_cap < 0) g_kw_cap = 0;
    }
    return __atomic_load_n(&g_kw_n, __ATOMIC_RELAXED) < g_kw_cap ||
           __atomic_load_n(&g_kw_new_n, __ATOMIC_RELAXED) < kl_kw_new_cap();
}
// Index into g_kw of the word this line counts against, or -1. A word with a sub-cap that is used up, or any word whose
// budget (run-state / new words) is used up, is skipped, so the line can still count against a later word that has room.
static int kl_kw_find(int prio, const char *tag, const char *msg) {
    int low = prio < 4;
    int unity = tag && strstr(tag, "Unity");
    int ue = tag && (!strcmp(tag, "UE4") || !strcmp(tag, "UE"));
    int old_ok = __atomic_load_n(&g_kw_n, __ATOMIC_RELAXED) < g_kw_cap;
    int new_ok = __atomic_load_n(&g_kw_new_n, __ATOMIC_RELAXED) < kl_kw_new_cap();
    for (const char *c = msg; *c; c++) {
        if (c != msg && ((c[-1] >= '0' && c[-1] <= '9') || ((c[-1] | 0x20) >= 'a' && (c[-1] | 0x20) <= 'z'))) continue;
        for (unsigned k = 0; k < KL_KW_N; k++) {
            if (low && !unity && !(g_kw[k].sub && ue)) continue;
            if (g_kw[k].sub ? !new_ok : !old_ok) continue;
            if (g_kw[k].sub && __atomic_load_n(&g_kw_sub_n[k], __ATOMIC_RELAXED) >= g_kw[k].sub) continue;
            if (strncasecmp(c, g_kw[k].w, strlen(g_kw[k].w)) == 0) return (int)k;
        }
    }
    return -1;
}
void kl_log_keyword_copy(int prio, const char *tag, const char *msg) {
    if (!msg || !kl_log_keyword_open()) return;
    int k = kl_kw_find(prio, tag, msg);
    if (k < 0) return;
    int cap = g_kw[k].sub ? kl_kw_new_cap() : g_kw_cap;
    int n = __atomic_fetch_add(g_kw[k].sub ? &g_kw_new_n : &g_kw_n, 1, __ATOMIC_RELAXED);
    if (n >= cap) return;
    int sub_last = g_kw[k].sub && __atomic_add_fetch(&g_kw_sub_n[k], 1, __ATOMIC_RELAXED) == g_kw[k].sub;
    static const char lv[] = "??VDIWEF";
    fprintf(stderr, "  [unity-kw] %c/%s: %.400s%s\n", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "", msg,
            n + 1 == cap ? (g_kw[k].sub ? " (kw new-word line cap reached)" : " (kw line cap reached)") : sub_last ? " (kw word cap reached)" : "");
}
/* KL-TEST-END kwcopy */

/* KL-TEST-BEGIN lring */
// The guest's last errors, so a fatal can say WHY. UE4's fatal handler ends in AndroidThunkJava_ForceQuit (ignored here)
// and the game then dies of a SIGSEGV that says nothing; the reason was one log line earlier. Kept: every ERROR/FATAL
// line, plus the lines of tag UE4 / UE / *Unity* that say "Fatal", "Assertion failed" or "Error:" at any priority.
// Last KL_LRING_N lines; a line equal to the previous one only bumps its " (xN)" counter, so an error per frame does
// not push the real reason out. Writers take a tiny spin lock and publish the count AFTER the slot is written; the
// reader (kl_log_ring_dump, called from the fatal-signal handler) takes no lock and calls no libc formatter: the
// slots hold the final text and the dump copies bytes. A slot rewritten while it is read shows a mixed line at worst.
#define KL_LRING_N 32
#define KL_LRING_LEN 256
#define KL_LRING_BASE (KL_LRING_LEN - 16)   // room left for " (x4294967295)"
static struct { char text[KL_LRING_LEN]; unsigned blen, rep; } g_lring[KL_LRING_N];
static unsigned g_lring_seq;   // lines ever stored
static char g_lring_lock;

static int kl_lring_tag(const char *tag) {
    return tag && (!strcmp(tag, "UE4") || !strcmp(tag, "UE") || strstr(tag, "Unity"));
}
int kl_log_ring_wants(int prio, const char *tag) { return prio >= 6 || kl_lring_tag(tag); }
static void kl_lring_suffix(char *text, unsigned blen, unsigned rep) {
    char d[12];
    int nd = 0;
    for (unsigned v = rep; v; v /= 10) d[nd++] = (char)('0' + v % 10);
    char *p = text + blen;
    *p++ = ' '; *p++ = '('; *p++ = 'x';
    while (nd) *p++ = d[--nd];
    *p++ = ')'; *p = 0;
}
void kl_log_ring_note(int prio, const char *tag, const char *msg) {
    if (!msg || !kl_log_ring_wants(prio, tag)) return;
    if (prio < 6 && !strstr(msg, "Fatal") && !strstr(msg, "Assertion failed") && !strstr(msg, "Error:")) return;
    static const char lv[] = "??VDIWEF";
    char t[KL_LRING_BASE + 1];
    int n = snprintf(t, sizeof t, "%c/%s: %s", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "", msg);
    if (n < 0) return;
    if (n > KL_LRING_BASE) n = KL_LRING_BASE;
    for (int i = 0; i < n; i++) if (t[i] == '\n' || t[i] == '\r') t[i] = ' ';
    while (n > 0 && t[n - 1] == ' ') n--;
    while (__atomic_test_and_set(&g_lring_lock, __ATOMIC_ACQUIRE)) {}
    unsigned seq = g_lring_seq;
    if (seq && g_lring[(seq - 1) % KL_LRING_N].blen == (unsigned)n &&
        !memcmp(g_lring[(seq - 1) % KL_LRING_N].text, t, (size_t)n)) {
        unsigned r = ++g_lring[(seq - 1) % KL_LRING_N].rep;
        kl_lring_suffix(g_lring[(seq - 1) % KL_LRING_N].text, (unsigned)n, r);
    } else {
        unsigned i = seq % KL_LRING_N;
        memcpy(g_lring[i].text, t, (size_t)n);
        g_lring[i].text[n] = 0;
        g_lring[i].blen = (unsigned)n;
        g_lring[i].rep = 1;
        __atomic_store_n(&g_lring_seq, seq + 1, __ATOMIC_RELEASE);
    }
    __atomic_clear(&g_lring_lock, __ATOMIC_RELEASE);
}
// "[fatal] last guest errors:" and the lines, oldest first, through `emit` (one call per line). Async-signal-safe:
// byte copies and emit only.
void kl_log_ring_dump(void (*emit)(const char *, size_t)) {
    unsigned seq = __atomic_load_n(&g_lring_seq, __ATOMIC_ACQUIRE);
    unsigned cnt = seq < KL_LRING_N ? seq : KL_LRING_N;
    char b[KL_LRING_LEN + 4];
    size_t l = 0;
    for (const char *h = "[fatal] last guest errors:"; *h; h++) b[l++] = *h;
    if (!cnt) for (const char *h = " (none logged)"; *h; h++) b[l++] = *h;
    b[l++] = '\n';
    emit(b, l);
    for (unsigned k = 0; k < cnt; k++) {
        const char *t = g_lring[(seq - cnt + k) % KL_LRING_N].text;
        l = 0;
        b[l++] = ' '; b[l++] = ' ';
        while (l < KL_LRING_LEN + 2 && *t) b[l++] = *t++;
        b[l++] = '\n';
        emit(b, l);
    }
}
/* KL-TEST-END lring */

int klh_android_log_print(int prio, const char *tag, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    if (kl_log_keyword_open() && (prio >= 4 || (tag && strstr(tag, "Unity")))) {   // a copy of the run-state lines (see kl_log_keyword_copy)
        char kwtext[1024];
        va_list kwcp;
        va_copy(kwcp, _ap);
        vsnprintf(kwtext, sizeof kwtext, fmt, kwcp);
        va_end(kwcp);
        kl_log_keyword_copy(prio, tag, kwtext);
    }
    if (kl_log_ring_wants(prio, tag)) {   // the guest's last errors, for the [fatal] dump (kl_fault.c)
        char rt[KL_LRING_LEN];
        va_list rcp;
        va_copy(rcp, _ap);
        vsnprintf(rt, sizeof rt, fmt, rcp);
        va_end(rcp);
        kl_log_ring_note(prio, tag, rt);
    }
    static const char lv[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: ", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "");
    // The OpenXR CTS's progress, for the compositor's HUD (kl_openxr_status_text).
    if (tag && strcmp(tag, "OpenXR_Conformance") == 0) {
        char text[1024];
        va_list cp;
        va_copy(cp, _ap);
        vsnprintf(text, sizeof text, fmt, cp);
        va_end(cp);
        extern void kl_openxr_status_log(const char *, const char *);
        kl_openxr_status_log(tag, text);
    }
    if (tag && prio >= 6 && strcmp(tag, "Unity") == 0) {   // the engine's graphics error, for the launcher
        char text[512];
        va_list cp;
        va_copy(cp, _ap);
        vsnprintf(text, sizeof text, fmt, cp);
        va_end(cp);
        extern void kl_jni_note_log(int, const char *, const char *);
        kl_jni_note_log(prio, tag, text);
    }
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    // hl2 eye-copy RT probe: the guest logs "began eye submission" on the eye-submission
    // thread right before it reads its render target and (currently) reports it
    // unavailable. Sample the real ANGLE FBO state here — a reliable trigger, since the
    // eye-copy's own glGetIntegerv bypasses the klfb wrapper. See kl_glfb_probe_current_rt.
    if (fmt && (strstr(fmt, "began eye submission") ||
                strstr(fmt, "render target is unavailable"))) {
        extern void kl_glfb_probe_current_rt(const char *);
        kl_glfb_probe_current_rt(strstr(fmt, "unavailable") ? "at 'RT unavailable'"
                                                            : "at 'began eye submission'");
    }
    return n;
}
// BSD err.h, which bionic has and the generated table cannot reach: Darwin
// declares warnx in <err.h>, kl_shim.c does not include it, and the generator's
// answer to "does Darwin declare this" is therefore no. It would be a variadic
// anyway, so a direct forward was never available (AAPCS64 registers vs
// Darwin's stack slots).
//
// The real warnx prefixes the program name and appends a newline, and does NOT
// append strerror the way warn does. OpenJK uses it for its own diagnostics, so
// the prefix says which side of the shim the line came from rather than
// restating a host program name the guest never chose.
int klh_warnx(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fputs("[guest warnx] ", stderr);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
int klh_sscanf(const char *s, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vsscanf(s, fmt, _ap);
}
int klh_fscanf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vfscanf(kl_host_file(f), fmt, _ap);
}

int klh_scanf(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vscanf(fmt, _ap);
}

// ---- wide printf, err.h: the work is in kl_bionic_extra2.c, which also serves the v* forms ----
int klx2_vwprintf(const wchar_t *fmt, kl_va *va);
int klx2_vfwprintf(void *f, const wchar_t *fmt, kl_va *va);
int klx2_vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, kl_va *va);
void klx2_verr(int eval, const char *fmt, kl_va *va, int with_errno, int do_exit);
int klh_wprintf(const wchar_t *fmt, kl_va *va)                    { return klx2_vwprintf(fmt, va); }
int klh_fwprintf(void *f, const wchar_t *fmt, kl_va *va)          { return klx2_vfwprintf(f, fmt, va); }
int klh_swprintf(wchar_t *s, size_t n, const wchar_t *fmt, kl_va *va) { return klx2_vswprintf(s, n, fmt, va); }
void klh_err(int eval, const char *fmt, kl_va *va)  { klx2_verr(eval, fmt, va, 1, 1); }
void klh_errx(int eval, const char *fmt, kl_va *va) { klx2_verr(eval, fmt, va, 0, 1); }
void klh_warn(const char *fmt, kl_va *va)           { klx2_verr(0, fmt, va, 1, 0); }
// glibc's error()/error_at_line() and the wide scanf entry points (bodies in kl_bionic_extra2.c)
void klx2_error(int status, int errnum, const char *file, unsigned line, int with_loc, const char *fmt, kl_va *va);
int klx2_vswscanf(const wchar_t *s, const wchar_t *fmt, kl_va *va);
int klx2_vfwscanf(void *f, const wchar_t *fmt, kl_va *va);
int klx2_vwscanf(const wchar_t *fmt, kl_va *va);
void klh_error(int status, int errnum, const char *fmt, kl_va *va) { klx2_error(status, errnum, NULL, 0, 0, fmt, va); }
void klh_error_at_line(int status, int errnum, const char *file, unsigned line, const char *fmt, kl_va *va) {
    klx2_error(status, errnum, file, line, 1, fmt, va);
}
int klh_wscanf(const wchar_t *fmt, kl_va *va)                    { return klx2_vwscanf(fmt, va); }
int klh_fwscanf(void *f, const wchar_t *fmt, kl_va *va)          { return klx2_vfwscanf(f, fmt, va); }
int klh_swscanf(const wchar_t *s, const wchar_t *fmt, kl_va *va) { return klx2_vswscanf(s, fmt, va); }

// ---- non-printf variadics: exactly one trailing argument, no format string ----
int kl_open_flags(int lx);          // kl_libc.c
int klh_open(const char *path, int flags, kl_va *va) {
    // Guest passes Linux O_* values, which differ from Darwin's; translate.
    // The mode argument only matters with O_CREAT, but reading it is harmless.
    // The path goes through the /proc rewrite for the same reason fopen does.
    char kp[1024];
    // Xash's con_logfile / -log open their .log via open() with a RELATIVE name
    // against a non-writable CWD, so it fails and the whole server console (incl.
    // scripted_sequence diagnostics) is lost. Rebase a .log WRITE to the writable
    // basedir so kl_xash_tail_enginelog streams it. (Mirrors the fopen path in
    // kl_libc.c.) 3 = O_ACCMODE in the guest's Linux numbering; nonzero = write.
    const char *xbase = getenv("XASH3D_BASEDIR");
    if (xbase && *xbase && path && (flags & 3) != 0) {
        size_t n = strlen(path);
        if (n >= 4 && !strcmp(path + n - 4, ".log")) {
            const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
            char rp[1100];
            snprintf(rp, sizeof rp, "%s/%s", xbase, bn);
            int lfd = open(rp, kl_open_flags(flags), (int)kl_va_gp(va));
            if (lfd >= 0) {
                static int said;
                if (!said++) fprintf(stderr, "  [xash] log open %s -> %s\n", path, rp);
                kl_fs_trace_open(path, flags, lfd);
                return lfd;
            }
        }
    }
    int fd = open(kl_guest_path(path, kp, sizeof kp), kl_open_flags(flags), (int)kl_va_gp(va));
    kl_fs_trace_open(path, flags, fd);
    return fd;
}
// cmd/req and the trailing argument are TRANSLATED, never forwarded raw: the
// two numberings are close enough that a wrong one succeeds meaning something
// else. F_SETFL takes the
// same flag word open() takes — Linux O_NONBLOCK is Darwin O_EXCL — so the
// translation lives next to kl_open_flags in kl_libc.c rather than here.
int kl_fcntl(int fd, int cmd, uintptr_t arg);         // kl_libc.c
int kl_ioctl(int fd, unsigned long req, uintptr_t arg);
int klh_fcntl(int fd, int cmd, kl_va *va) {
    return kl_fcntl(fd, cmd, (uintptr_t)kl_va_gp(va));
}
int klh_ioctl(int fd, unsigned long req, kl_va *va) {
    return kl_ioctl(fd, req, (uintptr_t)kl_va_gp(va));
}

// __android_log_buf_print(bufID, prio, tag, fmt, ...): the buffer id is irrelevant here.
int klh_android_log_buf_print(int buf, int prio, const char *tag, const char *fmt, kl_va *va) {
    (void)buf;
    return klh_android_log_print(prio, tag, fmt, va);
}

// __android_log_assert(cond, tag, fmt, ...): logs and aborts, like liblog.
int klh_android_log_assert(const char *cond, const char *tag, const char *fmt, kl_va *va) {
    fprintf(stderr, "F/%s: assert%s%s: ", tag ? tag : "", cond ? " " : "", cond ? cond : "");
    char note[KL_LRING_LEN];
    snprintf(note, sizeof note, "assert %s", cond ? cond : "");
    if (fmt) {
        KL_MARSHAL(fmt, va, KL_VA_PRINTF);
        size_t nl = strlen(note);
        va_list ncp;
        va_copy(ncp, _ap);
        vsnprintf(note + nl, sizeof note - nl, fmt, ncp);   // "assert <cond>" + the message: the abort that follows is the SIGABRT dump
        va_end(ncp);
        vfprintf(stderr, fmt, _ap);
    }
    kl_log_ring_note(7, tag, note);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
