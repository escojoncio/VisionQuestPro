// Guest-thread sampler (KL_SAMPLE_MS). See kl_sample.c.
#ifndef KL_SAMPLE_H
#define KL_SAMPLE_H

#include <stdio.h>

// Start a thread that, every interval_ms, captures pc + an x29-chained
// backtrace of every thread executing guest code, resolving pcs to
// module+offset and, inside libil2cpp's generated code, to managed method
// names (kl_il2cpp.c, fed global-metadata.dat from metadata_path).
// Returns 1 when the sampler thread started.
int  kl_sample_start(unsigned interval_ms, const char *metadata_path);

// Stop sampling and print the aggregate report: top leaf frames and top
// first-guest frames overall, then per-thread run-state and hot frames.
// Safe to call when never started (prints nothing). Also called from
// t_boot's fault handler on SIGALRM so a watchdog kill still yields data.
void kl_sample_stop_report(FILE *out);

// Print the current aggregates without stopping. The sampler calls this on
// itself every KL_SAMPLE_REPORT_S seconds (default 30), because the runs that
// matter die by guest abort — a path that resets the signal handlers and
// never reaches stop_report.
void kl_sample_report(FILE *out);

// One-shot: print the current frame chain of every thread that is in guest
// code (module+offset, managed method names in libil2cpp). Independent of the
// periodic sampler. `why` labels the event.
void kl_sample_snapshot(FILE *out, const char *why);

// Like kl_sample_snapshot but for EVERY thread, guest or not: tid/name, Mach run
// state, pc/lr and frame chain symbolized, the shim primitive each is blocked on
// (kl_wait.h), its EGL context / in-GL-call, then the futex words, the pending
// Oculus platform requests and the EGL/GL call log.
void kl_sample_snapshot_all(FILE *out, const char *why);

// Memory-jump diagnostics: cumulative CPU per Mach thread. _baseline is cheap and meant to run about once a second;
// _hot_threads prints every thread with its name, CPU and CPU since the last baseline, plus the stacks (image+offset)
// of the 3 most active ones. Not suspending anything; safe on any thread.
void kl_sample_threads_baseline(void);
void kl_sample_hot_threads(FILE *out, const char *why);

#endif
