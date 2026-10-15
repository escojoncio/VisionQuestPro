// The executable door: a guest built as an Android command-line program — a PIE executable with
// `main` exported (-Wl,--export-dynamic-symbol=main) — instead of an app library. The Khronos
// conformance runners are the reason it exists: deqp-vk built with DEQP_ANDROID_EXE runs the
// Vulkan CTS against whatever libvulkan.so it dlopens, which here is VPKEngine's driver through
// Klepton, so the tests exercise exactly the path a Quest game's Vulkan calls take.
//
// Mapping is the NativeActivity door's (kl_native_load: DT_NEEDED chain, static initializers).
// main() then runs on a guest thread with a desktop-sized stack (deqp and glslang recurse deeply;
// bionic's 1 MB default is not enough), once per command line:
//
//   KL_EXE_ARGS_FILE   one command line per line, arguments separated by TAB (argv[0] is added)
//   KL_EXE_ARGS        a single command line, same format (when there is no file)
//   KL_EXE_BUDGET      seconds; no new command line starts after this much time (default 900)
//   KL_EXE_MEM_MB      phys_footprint limit; no new command line starts above it (default 3000)
//   KL_EXE_STOP_FILE   if this file exists, no new command line starts (the app's Stop button)
//   KL_EXE_PROGRESS    file rewritten before/after each command line: "<index> running|done <rc>"
//   KL_EXE_CASE_TIMEOUT_S  a test that writes nothing to its log for this long (default 240) is
//                      interrupted on its own thread and recorded as Timeout (see kl_exe_pump)
//
// A command line that crashes takes the process with it; the app reads KL_EXE_PROGRESS and the
// runner's own result files on the next launch and carries on from there.
#include "kl_exe.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "klepton.h"
#include "kl_native.h"
#include "kl_fault.h"

typedef int (*exe_main_fn)(int, char **);

static exe_main_fn  g_main;
static char         g_name[256];
static pthread_t    g_thread;
static atomic_int   g_done;
static atomic_int   g_started;
static int          g_ran, g_lines, g_last_rc;
static char         g_stop_reason[160];
static double       g_budget = 900, g_t0;
static char         g_err[256];

const char *kl_exe_error(void) { return g_err; }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static unsigned footprint_mb(void) {
#ifdef __APPLE__
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS)
        return (unsigned)(vi.phys_footprint >> 20);
#endif
    return 0;
}

static void progress(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void progress(const char *fmt, ...) {
    const char *path = getenv("KL_EXE_PROGRESS");
    if (!path || !*path) return;
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

// Why no new command line may start, or NULL to go on.
static const char *should_stop(void) {
    if (g_budget >= 0 && now_s() - g_t0 >= g_budget) return "time budget reached";
    unsigned lim = kl_env_uint("KL_EXE_MEM_MB", 3000);
    unsigned fp = footprint_mb();
    if (lim && fp > lim) {
        snprintf(g_stop_reason, sizeof g_stop_reason, "memory %u MB > limit %u MB", fp, lim);
        return g_stop_reason;
    }
    const char *sf = getenv("KL_EXE_STOP_FILE");
    if (sf && *sf && access(sf, F_OK) == 0) return "stop requested";
    return NULL;
}

// Runs main() for one TAB-separated command line. The line is modified in place.
// ---- per-test crash recovery -------------------------------------------------------------
// A test that crashes (a Metal assertion, a guest fault, a Klepton fatal path) would end the whole
// session. The runner thread holds a recovery point instead (kl_fault_set_recovery): the crash
// report is written, the thread unwinds back here, the case that was running is closed in its
// .qpa as "#terminateTestCaseResult Crash", and main() runs again on the rest of the case list,
// with the results in a new part file (<log>.r<k>.qpa). What the crashed main() held is leaked;
// the memory limit between command lines still applies. KL_EXE_RECOVER=0 turns it off.

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n > 0 ? (size_t)n + 1 : 1);
    size_t got = b && n > 0 ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (b) b[got] = '\0';
    if (len) *len = got;
    return b;
}

static int arg_index(char **argv, int argc, const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], prefix, n)) return i;
    return -1;
}

// After a crash: closes the open case in the .qpa (as `result`: Crash, Timeout, DeviceLost) and
// points argv at the rest of the list. 1 = run again, 0 = nothing left / cannot tell where it stopped.
static int prepare_resume(char **argv, int argc, int k, const char *result) {
    int il = arg_index(argv, argc, "--deqp-caselist-file="), iq = arg_index(argv, argc, "--deqp-log-filename=");
    if (il < 0 || iq < 0) return 0;
    const char *list = argv[il] + strlen("--deqp-caselist-file=");
    const char *qpa = argv[iq] + strlen("--deqp-log-filename=");
    char *log = slurp(qpa, NULL);
    // The last case the log mentions, and whether it was left open.
    char last[512] = "";
    int open = 0;
    for (char *p = log; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (!strncmp(p, "#beginTestCaseResult ", 21)) {
            snprintf(last, sizeof last, "%s", p + 21);
            size_t L = strlen(last);
            while (L && (last[L - 1] == '\r' || last[L - 1] == ' ')) last[--L] = '\0';
            open = 1;
        } else if (!strncmp(p, "#endTestCaseResult", 18) || !strncmp(p, "#terminateTestCaseResult", 24)) {
            open = 0;
        }
        p = nl ? nl + 1 : NULL;
    }
    free(log);
    if (!last[0]) {
        fprintf(stderr, "[exe] crash before the first case of this list: not resumable\n");
        return 0;
    }
    if (open) {
        FILE *f = fopen(qpa, "a");
        if (f) { fprintf(f, "\n#terminateTestCaseResult %s\n", result); fclose(f); }
        fprintf(stderr, "[exe] case %s ended early: recorded as %s\n", last, result);
    }
    // The rest of the list after `last`.
    char *cases = slurp(list, NULL);
    if (!cases) return 0;
    char *after = NULL;
    for (char *p = cases; p && *p; ) {
        char *nl = strchr(p, '\n');
        size_t L = nl ? (size_t)(nl - p) : strlen(p);
        while (L && (p[L - 1] == '\r' || p[L - 1] == ' ')) L--;
        if (L == strlen(last) && !strncmp(p, last, L)) { after = nl ? nl + 1 : p + strlen(p); break; }
        p = nl ? nl + 1 : NULL;
    }
    if (!after || !*after) { free(cases); return 0; }
    char base[1400];
    snprintf(base, sizeof base, "%s", qpa);
    size_t bl = strlen(base);
    if (bl > 4 && !strcmp(base + bl - 4, ".qpa")) base[bl - 4] = '\0';
    char nlist[1500], nqpa[1500];
    snprintf(nlist, sizeof nlist, "%s.r%d.txt", base, k);
    snprintf(nqpa, sizeof nqpa, "%s.r%d.qpa", base, k);
    FILE *f = fopen(nlist, "w");
    if (!f) { free(cases); return 0; }
    fputs(after, f);
    fclose(f);
    free(cases);
    char a1[1600], a2[1600];
    snprintf(a1, sizeof a1, "--deqp-caselist-file=%s", nlist);
    snprintf(a2, sizeof a2, "--deqp-log-filename=%s", nqpa);
    argv[il] = strdup(a1);   // leaked on purpose: a handful per session
    argv[iq] = strdup(a2);
    return 1;
}

static int g_recoveries;
static volatile double g_last_recovery_t;
static char g_cur_qpa[1500];   // the log the running main() writes (for the case watchdog)
// Case watchdog (kl_exe_pump): main() is running (between its start and its end or recovery), when
// it last started, and whether the watchdog has interrupted it (SIGABRT to the runner thread).
static atomic_int    g_in_main;
static volatile double g_main_t;
static volatile time_t g_main_wall;   // CLOCK_REALTIME seconds at that start (vs. the log's mtime)
static atomic_int    g_interrupted;
static volatile double g_interrupt_t;

static void note_current_log(char **argv, int argc) {
    int iq = arg_index(argv, argc, "--deqp-log-filename=");
    snprintf(g_cur_qpa, sizeof g_cur_qpa, "%s", iq >= 0 ? argv[iq] + strlen("--deqp-log-filename=") : "");
}

static int run_line(char *line, int index) {
    static char *argv[512];
    int argc = 0;
    argv[argc++] = g_name;
    for (char *p = line, *next; p && *p && argc < 511; p = next) {
        next = strchr(p, '\t');
        if (next) *next++ = '\0';
        if (*p) argv[argc++] = p;
    }
    argv[argc] = NULL;
    fprintf(stderr, "\n[exe] command line %d: %d argument(s)", index + 1, argc - 1);
    for (int i = 1; i < argc && i < 4; i++) fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "%s\n", argc > 4 ? " ..." : "");
    fflush(NULL);
    progress("%d running\n", index);
    double t = now_s();
    const int recover = kl_env_on("KL_EXE_RECOVER", 1);
    const unsigned max_rec = kl_env_uint("KL_EXE_MAX_RECOVERIES", 50);
    static sigjmp_buf jb;
    static volatile int rc;
    rc = 0;
    note_current_log(argv, argc);
    for (;;) {
        if (sigsetjmp(jb, 1) == 0) {
            if (recover) kl_fault_set_recovery(&jb);
            g_main_t = now_s();
            { struct timespec w; clock_gettime(CLOCK_REALTIME, &w); g_main_wall = w.tv_sec; }
            atomic_store(&g_in_main, 1);
            rc = g_main(argc, argv);
            // The recovery point stays armed until the watchdog is off: a SIGABRT it sent just
            // as main() returned still unwinds here instead of ending the process.
            atomic_store(&g_in_main, 0);
            if (atomic_load(&g_interrupted)) usleep(200 * 1000);   // let that signal land
            kl_fault_set_recovery(NULL);
            atomic_store(&g_interrupted, 0);
            break;
        }
        // Back from a crash inside main(): a fault, a lost GPU device, or the case watchdog.
        atomic_store(&g_in_main, 0);
        kl_futex_forget_thread();      // a hung case is often parked in a futex wait
        kl_fault_set_recovery(NULL);   // kl_fault_try_recover already disarmed it (one jump per point)
        const int sig = kl_fault_recovered_signal();
        const int timed_out = atomic_exchange(&g_interrupted, 0);
        const char *result = timed_out ? "Timeout" : sig == KL_RECOVER_DEVICE_LOST ? "DeviceLost" : "Crash";
        g_recoveries++;
        g_last_recovery_t = now_s();
        fprintf(stderr, "[exe] command line %d: %s after %.1f s, recovery %d of %u "
                        "(footprint %u MB)\n", index + 1,
                timed_out ? "case timed out" : sig == KL_RECOVER_DEVICE_LOST ? "GPU device lost" : "crash",
                now_s() - t, g_recoveries, max_rec, footprint_mb());
        if (!timed_out && sig != KL_RECOVER_DEVICE_LOST)
            fprintf(stderr, "[exe] (signal %d)\n", sig);
        rc = sig == KL_RECOVER_DEVICE_LOST ? 128 + 64 : 128 + sig;
        if ((unsigned)g_recoveries > max_rec) {
            fprintf(stderr, "[exe] too many crashes this session: stopping here\n");
            break;
        }
        const char *why = should_stop();
        if (why) {
            if (why != g_stop_reason) snprintf(g_stop_reason, sizeof g_stop_reason, "%s", why);
            fprintf(stderr, "[exe] not resuming after the crash: %s\n", g_stop_reason);
            break;
        }
        if (!prepare_resume(argv, argc, g_recoveries, result)) break;
        note_current_log(argv, argc);
        progress("%d running\n", index);
    }
    fflush(NULL);
    fprintf(stderr, "[exe] command line %d finished: main() returned %d after %.1f s "
                    "(footprint %u MB)\n", index + 1, rc, now_s() - t, footprint_mb());
    progress("%d done %d\n", index, rc);
    return rc;
}

static void *exe_thread(void *arg) {
    (void)arg;
    char *text = NULL;
    const char *file = getenv("KL_EXE_ARGS_FILE");
    int file_failed = 0;
    if (file && *file) {
        FILE *f = fopen(file, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            if (n < 0) n = 0;
            fseek(f, 0, SEEK_SET);
            text = malloc((size_t)n + 1);
            size_t got = text ? fread(text, 1, (size_t)n, f) : 0;
            if (text) text[got] = '\0';
            fclose(f);
        } else {
            fprintf(stderr, "[exe] KL_EXE_ARGS_FILE %s: %s — nothing to run\n", file, strerror(errno));
            file_failed = 1;
        }
    }
    if (!text) text = strdup(!file_failed && getenv("KL_EXE_ARGS") ? getenv("KL_EXE_ARGS") : "");

    // The command lines: one per line, '#' comments and blank lines skipped. None at all means
    // main() once with no arguments, as a shell would run it.
    enum { MAX_LINES = 4096 };
    static char *lines[MAX_LINES];
    int count = 0;
    for (char *p = text, *next; p && count < MAX_LINES; p = next) {
        next = strchr(p, '\n');
        if (next) *next++ = '\0';
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r') p[--len] = '\0';
        if (len && p[0] != '#') lines[count++] = p;
        if (!next) break;
    }
    // A set-but-empty list (every batch already done) runs nothing.
    static char empty[1];
    int had_list = (file && *file) || getenv("KL_EXE_ARGS");
    if (!count && !had_list) lines[count++] = empty;
    g_lines = count;
    fprintf(stderr, "[exe] %s: %d command line(s), budget %.0f s, memory limit %u MB\n",
            g_name, g_lines, g_budget, kl_env_uint("KL_EXE_MEM_MB", 3000));

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            const char *why = should_stop();
            if (why) {
                if (why != g_stop_reason) snprintf(g_stop_reason, sizeof g_stop_reason, "%s", why);
                fprintf(stderr, "[exe] stopping before command line %d: %s\n", i + 1, g_stop_reason);
                break;
            }
        }
        g_last_rc = run_line(lines[i], i);
        g_ran++;
    }
    free(text);
    progress("%d finished\n", g_ran);
    atomic_store(&g_done, 1);
    return NULL;
}

int kl_exe_begin(FILE *out) {
    kl_image *img = kl_native_image();
    if (!img) { snprintf(g_err, sizeof g_err, "the executable is not mapped"); return 1; }
    // vq_exe_main: the exported C entry the conformance build wraps main's body in
    // (conformance/vulkan/patch_deqp.py); a plain `main` exported with
    // -Wl,--export-dynamic-symbol=main works too.
    g_main = (exe_main_fn)kl_sym(img, "vq_exe_main");
    if (!g_main) g_main = (exe_main_fn)kl_sym(img, "main");
    if (!g_main) {
        snprintf(g_err, sizeof g_err, "the executable exports neither vq_exe_main nor main() "
                 "(link it with -Wl,--export-dynamic-symbol=main)");
        return 1;
    }
    const char *entry = getenv("KL_GUEST_ENTRY");
    snprintf(g_name, sizeof g_name, "%s", entry && *entry ? entry : "guest");
    const char *b = getenv("KL_EXE_BUDGET");
    g_budget = b && *b ? strtod(b, NULL) : 900.0;
    g_t0 = now_s();
    size_t stack = (size_t)kl_env_uint("KL_EXE_STACK_MB", 64) << 20;
    int rc = kl_guest_thread_create(&g_thread, stack, exe_thread, NULL);
    if (rc) { snprintf(g_err, sizeof g_err, "pthread_create: %s", strerror(rc)); return 1; }
    atomic_store(&g_started, 1);
    if (out) {
        fprintf(out, "\n=== main() of %s on a guest thread (%zu MB stack) ===\n", g_name, stack >> 20);
        fflush(out);
    }
    return 0;
}

double kl_exe_pump(double seconds, const volatile int *quit) {
    double t0 = now_s(), el = 0;
    const double case_timeout = (double)kl_env_uint("KL_EXE_CASE_TIMEOUT_S", 240);
    double last_pass = t0, resumed = 0;
    while (atomic_load(&g_started) && !atomic_load(&g_done) && (quit ? !*quit : 1)
           && (seconds < 0 || el < seconds)) {
        usleep(100 * 1000);
        el = now_s() - t0;
        // The app was suspended (headset off, app in the background): the time it was not running
        // is not a stuck case.
        if (now_s() - last_pass > 5.0) resumed = now_s();
        last_pass = now_s();
        // Case watchdog. With --deqp-log-flush the log is written at the end of every case, so a
        // log that has not changed for KL_EXE_CASE_TIMEOUT_S seconds (240) means one case is stuck
        // (a deadlock, an endless loop; GPU waits end on their own as a lost device). The runner
        // thread is sent SIGABRT: its crash report shows where it was stuck, and it unwinds to its
        // recovery point, which records the case as Timeout and goes on with the next one. If that
        // does not bring the log back to life within 60 s (a lock the abandoned main() held), the
        // process ends; the next session resumes after the open case.
        if (!atomic_load(&g_in_main) && !atomic_load(&g_interrupted)) continue;
        double quiet = now_s() - (g_main_t > g_last_recovery_t ? g_main_t : g_last_recovery_t);
        struct stat st;
        int log_written = 0;   // this main() has written its log since it started
        if (g_cur_qpa[0] && stat(g_cur_qpa, &st) == 0) {
            struct timespec wall;
            clock_gettime(CLOCK_REALTIME, &wall);
            double age = (double)wall.tv_sec - (double)st.st_mtime;
            if (age < quiet) quiet = age;
            log_written = st.st_mtime >= g_main_wall;
        }
        if (resumed > 0 && now_s() - resumed < quiet) quiet = now_s() - resumed;
        if (atomic_load(&g_interrupted)) {
            if (now_s() - g_interrupt_t > 60.0 && quiet > 60.0) {
                // write(2), not stdio: a lock held by the abandoned main() may be stdio's own.
                static const char m[] = "[exe] the timed-out case did not unwind: ending the session "
                                        "(the next one resumes after it)\n";
                ssize_t w = write(2, m, sizeof m - 1); (void)w;
                _exit(0);
            }
            continue;
        }
        // A main() that started after a recovered crash and never wrote its log is most likely
        // blocked on a lock the abandoned one held (malloc, stdio): interrupting it would only
        // repeat that, so the process ends instead; the next session resumes.
        if (case_timeout > 0 && quiet > case_timeout && g_recoveries > 0 && !log_written) {
            char m[200];
            int n = snprintf(m, sizeof m, "[exe] no progress for %.0f s after a recovered crash: "
                             "ending the session (the next one resumes)\n", quiet);
            if (n > 0) { ssize_t w = write(2, m, (size_t)n); (void)w; }
            _exit(0);
        }
        if (case_timeout > 0 && quiet > case_timeout && kl_env_on("KL_EXE_RECOVER", 1)
            && atomic_load(&g_in_main)) {
            char m[200];
            int n = snprintf(m, sizeof m, "[exe] no log progress for %.0f s: interrupting the stuck case "
                             "(SIGABRT to the test thread; recorded as Timeout)\n", quiet);
            if (n > 0) { ssize_t w = write(2, m, (size_t)n); (void)w; }
            g_interrupt_t = now_s();
            atomic_store(&g_interrupted, 1);
            pthread_kill(g_thread, SIGABRT);
        }
    }
    if (atomic_load(&g_done)) pthread_join(g_thread, NULL);
    return now_s() - t0;
}

void kl_exe_report(FILE *out) {
    if (!out) return;
    fprintf(out, "  %d of %d command line(s) ran, last main() returned %d%s%s; footprint %u MB\n",
            g_ran, g_lines, g_last_rc, g_stop_reason[0] ? "; stopped: " : "", g_stop_reason,
            footprint_mb());
    fflush(out);
}

int kl_exe_finished(void) { return atomic_load(&g_done); }
