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
//
// A command line that crashes takes the process with it; the app reads KL_EXE_PROGRESS and the
// runner's own result files on the next launch and carries on from there.
#include "kl_exe.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "klepton.h"
#include "kl_native.h"

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
static int run_line(char *line, int index) {
    char *argv[512];
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
    int rc = g_main(argc, argv);
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
    g_main = (exe_main_fn)kl_sym(img, "main");
    if (!g_main) {
        snprintf(g_err, sizeof g_err, "the executable exports no main() (link it with "
                 "-Wl,--export-dynamic-symbol=main)");
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
    while (atomic_load(&g_started) && !atomic_load(&g_done) && (quit ? !*quit : 1)
           && (seconds < 0 || el < seconds)) {
        usleep(100 * 1000);
        el = now_s() - t0;
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
