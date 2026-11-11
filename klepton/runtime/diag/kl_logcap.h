// kl_logcap.h - volume rules of the permanent diagnostic lines ([sp-life], [life], [net], [text]). Pure; header only.
//
// A line kind is numbered 1, 2, 3, ... as it occurs. It is written for the first `first` occurrences and then once in
// `every` (the line carries its number, so the gap is visible); never per frame.
#ifndef KL_LOGCAP_H
#define KL_LOGCAP_H
#include <stdint.h>

// n: 1-based occurrence number. every == 0: only the first `first`. n == 0 is never written.
static inline int kl_logcap_pass(unsigned n, unsigned first, unsigned every) {
    return n != 0 && (n <= first || (every != 0 && n % every == 0));
}
// Counts one occurrence in *ctr (any thread) and says whether it is written; *nout (may be NULL) gets its number.
static inline int kl_logcap_hit(unsigned *ctr, unsigned first, unsigned every, unsigned *nout) {
    unsigned n = __atomic_add_fetch(ctr, 1, __ATOMIC_RELAXED);
    if (nout) *nout = n;
    return kl_logcap_pass(n, first, every);
}
// A request opened at t0_ns is reported once when it is still open limit_ns later (clock steps backwards: never).
static inline int kl_logcap_overdue(uint64_t now_ns, uint64_t t0_ns, uint64_t limit_ns, int already_reported) {
    return !already_reported && now_ns >= t0_ns && now_ns - t0_ns >= limit_ns;
}
#endif
