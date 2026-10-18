// bionic's system-call-shaped libc surface that Darwin lacks or shapes differently
// (second half of kl_bionic_extra.c, whose table lists these). Everything here runs on
// Darwin only — it speaks fcntl(F_PREALLOCATE), getxattr(position, options), mach VM —
// so it is compiled into the runtime but not into the host test.
//
// The same three rules as the other file:
//   * translated  — a Linux call with a Darwin equivalent under another name, number or
//                   argument list: xattr, rlimit resources, posix_fadvise, fallocate.
//   * emulated    — the operation is expressible with what Darwin has: copy_file_range,
//                   close_range, mremap (anonymous memory), process_vm_readv on ourselves.
//   * refused     — a privileged operation. An app has no CAP_SYS_ADMIN, so on Android
//                   mount/swapon/reboot/setns fail with EPERM; here they do too.
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include "klepton.h"

#define GUEST_PATH(p) char _kp[1024]; const char *_p = kl_guest_path((p), _kp, sizeof _kp)

// ---------------------------------------------------------------- xattr
// Linux: getxattr(path, name, value, size). Darwin: + position (resource forks only: 0) and
// options. The l* forms are XATTR_NOFOLLOW, the f* forms take an fd. Linux's XATTR_CREATE
// is 1 and XATTR_REPLACE is 2; Darwin's are 2 and 4. A missing attribute is ENODATA on
// Linux; Darwin's ENOATTR is mapped to it by the errno table (93 -> 61).
static int xattr_flags(int lx) {
    int d = 0;
    if (lx & 1) d |= XATTR_CREATE;
    if (lx & 2) d |= XATTR_REPLACE;
    return d;
}
ssize_t klb_getxattr(const char *path, const char *name, void *value, size_t size) {
    GUEST_PATH(path); return getxattr(_p, name, value, size, 0, 0);
}
ssize_t klb_lgetxattr(const char *path, const char *name, void *value, size_t size) {
    GUEST_PATH(path); return getxattr(_p, name, value, size, 0, XATTR_NOFOLLOW);
}
ssize_t klb_fgetxattr(int fd, const char *name, void *value, size_t size) {
    return fgetxattr(fd, name, value, size, 0, 0);
}
int klb_setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    GUEST_PATH(path); return setxattr(_p, name, value, size, 0, xattr_flags(flags));
}
int klb_lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    GUEST_PATH(path); return setxattr(_p, name, value, size, 0, xattr_flags(flags) | XATTR_NOFOLLOW);
}
int klb_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    return fsetxattr(fd, name, value, size, 0, xattr_flags(flags));
}
ssize_t klb_listxattr(const char *path, char *list, size_t size) {
    GUEST_PATH(path); return listxattr(_p, list, size, 0);
}
ssize_t klb_llistxattr(const char *path, char *list, size_t size) {
    GUEST_PATH(path); return listxattr(_p, list, size, XATTR_NOFOLLOW);
}
ssize_t klb_flistxattr(int fd, char *list, size_t size) { return flistxattr(fd, list, size, 0); }
int klb_removexattr(const char *path, const char *name) {
    GUEST_PATH(path); return removexattr(_p, name, 0);
}
int klb_lremovexattr(const char *path, const char *name) {
    GUEST_PATH(path); return removexattr(_p, name, XATTR_NOFOLLOW);
}
int klb_fremovexattr(int fd, const char *name) { return fremovexattr(fd, name, 0); }

// ---------------------------------------------------------------- rlimit
// Linux resource numbers (CPU 0 FSIZE 1 DATA 2 STACK 3 CORE 4 RSS 5 NPROC 6 NOFILE 7
// MEMLOCK 8 AS 9 LOCKS 10 SIGPENDING 11 MSGQUEUE 12 NICE 13 RTPRIO 14 RTTIME 15) are not
// Darwin's (NPROC 7, NOFILE 8, MEMLOCK 6, AS 5, RSS 5), and RLIM_INFINITY is ~0 on Linux but
// 2^63-1 on Darwin. The forwarded getrlimit answered a different resource entirely.
typedef struct { uint64_t cur, max; } lx_rlimit;
#define LX_RLIM_INFINITY (~0ULL)
static int rlim_res(int lx) {
    switch (lx) {
    case 0: return RLIMIT_CPU;    case 1: return RLIMIT_FSIZE;  case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;  case 4: return RLIMIT_CORE;   case 5: return RLIMIT_RSS;
    case 6: return RLIMIT_NPROC;  case 7: return RLIMIT_NOFILE; case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
    }
    return -1;      // LOCKS / SIGPENDING / MSGQUEUE / NICE / RTPRIO / RTTIME: no Darwin counterpart
}
static uint64_t rl_to_lx(rlim_t v) { return v == RLIM_INFINITY ? LX_RLIM_INFINITY : (uint64_t)v; }
static rlim_t rl_from_lx(uint64_t v) { return v == LX_RLIM_INFINITY ? RLIM_INFINITY : (rlim_t)v; }
int klb_getrlimit(int res, lx_rlimit *out) {
    int d = rlim_res(res);
    if (d < 0) {
        // The limits Linux has and Darwin lacks are "unlimited" on Android apps except these.
        if (res == 11 || res == 12 || res == 15) { out->cur = out->max = LX_RLIM_INFINITY; return 0; }
        if (res == 10) { out->cur = out->max = LX_RLIM_INFINITY; return 0; }
        if (res == 13 || res == 14) { out->cur = out->max = 0; return 0; }
        errno = EINVAL; return -1;
    }
    struct rlimit r;
    if (getrlimit(d, &r) != 0) return -1;
    out->cur = rl_to_lx(r.rlim_cur); out->max = rl_to_lx(r.rlim_max);
    return 0;
}
int klb_setrlimit(int res, const lx_rlimit *in) {
    int d = rlim_res(res);
    if (d < 0) { errno = EINVAL; return -1; }
    struct rlimit r = { rl_from_lx(in->cur), rl_from_lx(in->max) };
    // macOS refuses a RLIMIT_NOFILE soft limit above OPEN_MAX unless the hard limit is
    // infinity; Linux has no such cap. Clamp rather than fail a guest that asks for 1 M fds.
    if (d == RLIMIT_NOFILE && r.rlim_cur > (rlim_t)OPEN_MAX && r.rlim_cur != RLIM_INFINITY) r.rlim_cur = OPEN_MAX;
    return setrlimit(d, &r);
}
// prlimit(pid, resource, new, old): only ourselves (0 or our pid) can be reached.
int klb_prlimit(int pid, int res, const lx_rlimit *nw, lx_rlimit *old) {
    if (pid != 0 && pid != (int)getpid()) { errno = pid < 0 ? ESRCH : EPERM; return -1; }
    if (old && klb_getrlimit(res, old) != 0) return -1;
    if (nw) return klb_setrlimit(res, nw);
    return 0;
}

// ---------------------------------------------------------------- memory
// mremap: Linux grows/shrinks/moves a mapping in place of an munmap+mmap copy. Shrinking
// unmaps the tail; growing a private mapping is a fresh mapping plus a copy (what the
// kernel's move amounts to); without MREMAP_MAYMOVE (1) a grow that cannot happen in
// place fails with ENOMEM, as it does when the neighbour address range is taken.
// MREMAP_FIXED (2) / DONTUNMAP (4) are not offered.
void *klb_mremap(void *old, size_t old_size, size_t new_size, int flags, ...) {
    if (flags & ~1) { errno = EINVAL; return MAP_FAILED; }
    if (new_size == 0 || ((uintptr_t)old & (getpagesize() - 1))) { errno = EINVAL; return MAP_FAILED; }
    size_t pg = (size_t)getpagesize();
    old_size = (old_size + pg - 1) & ~(pg - 1);
    new_size = (new_size + pg - 1) & ~(pg - 1);
    if (new_size == old_size) return old;
    if (new_size < old_size) {
        if (munmap((char *)old + new_size, old_size - new_size) != 0) return MAP_FAILED;
        return old;
    }
    if (!(flags & 1)) { errno = ENOMEM; return MAP_FAILED; }
    void *n = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (n == MAP_FAILED) return MAP_FAILED;
    memcpy(n, old, old_size);
    munmap(old, old_size);
    return n;
}
// mlock2(addr, len, MLOCK_ONFAULT): the lock-on-fault flag has no Darwin form; plain mlock.
int klb_mlock2(const void *addr, size_t len, unsigned flags) {
    if (flags & ~1u) { errno = EINVAL; return -1; }
    return mlock(addr, len);
}
int klb_munlock(const void *addr, size_t len) { return munlock(addr, len); }
// An app's RLIMIT_MEMLOCK is 64 KiB, so locking everything fails with ENOMEM on Android.
int klb_mlockall(int flags) {
    if (flags & ~7) { errno = EINVAL; return -1; }
    errno = ENOMEM; return -1;
}
int klb_munlockall(void) { return 0; }

// ---------------------------------------------------------------- file hints / allocation
// posix_fadvise returns the error NUMBER (not -1). ADVICE: NORMAL 0 RANDOM 1 SEQUENTIAL 2
// WILLNEED 3 DONTNEED 4 NOREUSE 5.
int klb_posix_fadvise(int fd, off_t off, off_t len, int advice) {
    struct stat st;
    if (fstat(fd, &st) != 0) return errno;
    if (S_ISFIFO(st.st_mode)) return ESPIPE;
    if (advice < 0 || advice > 5) return EINVAL;
    switch (advice) {
    case 1: fcntl(fd, F_RDAHEAD, 0); break;
    case 2: fcntl(fd, F_RDAHEAD, 1); break;
    case 3: {
        struct radvisory ra = { off, (int)(len > 0 && len < INT_MAX ? len : (off_t)INT_MAX) };
        fcntl(fd, F_RDADVISE, &ra);          // a hint: failure is not reported by posix_fadvise
        break;
    }
    default: break;                           // DONTNEED / NOREUSE / NORMAL: the cache is the OS's
    }
    return 0;
}
ssize_t klb_readahead(int fd, off_t off, size_t count) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) { errno = EINVAL; return -1; }
    struct radvisory ra = { off, (int)(count < INT_MAX ? count : (size_t)INT_MAX) };
    fcntl(fd, F_RDADVISE, &ra);
    return 0;
}
// Allocation: reserve the blocks (F_PREALLOCATE), then extend the size. keep_size = 0 grows
// st_size to off+len like posix_fallocate; with FALLOC_FL_KEEP_SIZE only the blocks are reserved.
static int fallocate_impl(int fd, off_t off, off_t len, int keep_size) {
    if (off < 0 || len <= 0) return EINVAL;
    struct stat st;
    if (fstat(fd, &st) != 0) return errno;
    if (S_ISFIFO(st.st_mode)) return ESPIPE;
    if (!S_ISREG(st.st_mode)) return ENODEV;
    off_t end = off + len;
    if (end < off) return EFBIG;
    if (end > st.st_size) {
        fstore_t fs = { F_ALLOCATEALL, F_PEOFPOSMODE, 0, end - st.st_size, 0 };
        if (fcntl(fd, F_PREALLOCATE, &fs) == -1) {
            if (errno == ENOSPC) return ENOSPC;
            // other failures (file systems without preallocation): fall back to the size change
        }
        if (!keep_size && ftruncate(fd, end) != 0) return errno;
    }
    return 0;
}
int klb_posix_fallocate(int fd, off_t off, off_t len) { return fallocate_impl(fd, off, len, 0); }
// fallocate(fd, mode, off, len): mode 0 and FALLOC_FL_KEEP_SIZE (1) only. PUNCH_HOLE / ZERO_RANGE /
// COLLAPSE_RANGE / INSERT_RANGE are EOPNOTSUPP, which is what a file system without them says.
int klb_fallocate(int fd, int mode, off_t off, off_t len) {
    if (mode & ~1) { errno = ENOTSUP; return -1; }
    int e = fallocate_impl(fd, off, len, mode & 1);
    if (e) { errno = e; return -1; }
    return 0;
}
// sync_file_range flags: WAIT_BEFORE 1, WRITE 2, WAIT_AFTER 4. Darwin has no range sync; fsync
// covers every combination (it can only be stronger than asked).
int klb_sync_file_range(int fd, off_t off, off_t n, unsigned flags) {
    (void)off; (void)n;
    if (flags & ~7u) { errno = EINVAL; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode) && !S_ISDIR(st.st_mode)) { errno = ESPIPE; return -1; }
    return flags ? fsync(fd) : 0;
}
int klb_syncfs(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    sync();
    return 0;
}
// copy_file_range(fd_in, *off_in, fd_out, *off_out, len, flags): flags must be 0. An offset
// pointer means pread/pwrite at that position, advanced; NULL uses and moves the descriptor's.
ssize_t klb_copy_file_range(int in, int64_t *off_in, int out, int64_t *off_out, size_t len, unsigned flags) {
    if (flags) { errno = EINVAL; return -1; }
    char buf[65536];
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total < sizeof buf ? len - total : sizeof buf;
        ssize_t r = off_in ? pread(in, buf, chunk, *off_in) : read(in, buf, chunk);
        if (r < 0) return total ? (ssize_t)total : -1;
        if (r == 0) break;
        ssize_t done = 0;
        while (done < r) {
            ssize_t w = off_out ? pwrite(out, buf + done, (size_t)(r - done), *off_out + done)
                                : write(out, buf + done, (size_t)(r - done));
            if (w < 0) return total + (size_t)done ? (ssize_t)(total + (size_t)done) : -1;
            done += w;
        }
        if (off_in) *off_in += r;
        if (off_out) *off_out += r;
        total += (size_t)r;
    }
    return (ssize_t)total;
}
// close_range(first, last, flags): CLOSE_RANGE_UNSHARE (2) is a no-op for a process whose fd table
// is not shared; CLOSE_RANGE_CLOEXEC (4) marks instead of closing.
int klb_close_range(unsigned first, unsigned last, unsigned flags) {
    if (first > last || (flags & ~6u)) { errno = EINVAL; return -1; }
    int max = getdtablesize();
    unsigned hi = last < (unsigned)max ? last : (unsigned)max - 1;
    for (unsigned fd = first; fd <= hi; fd++) {
        if (flags & 4u) { int f = fcntl((int)fd, F_GETFD); if (f >= 0) fcntl((int)fd, F_SETFD, f | FD_CLOEXEC); }
        else close((int)fd);
    }
    return 0;
}
// splice / vmsplice: Darwin has no pipe splicing; the data is moved through a buffer. That
// is observable only as extra copies. tee duplicates pipe contents without consuming them,
// which cannot be done to a pipe from outside — ENOSYS, the honest answer.
ssize_t klb_splice(int in, int64_t *off_in, int out, int64_t *off_out, size_t len, unsigned flags) {
    if (flags & ~15u) { errno = EINVAL; return -1; }
    struct stat si, so;
    if (fstat(in, &si) != 0 || fstat(out, &so) != 0) return -1;
    if (!S_ISFIFO(si.st_mode) && !S_ISFIFO(so.st_mode)) { errno = EINVAL; return -1; }   // one end must be a pipe
    if ((S_ISFIFO(si.st_mode) && off_in) || (S_ISFIFO(so.st_mode) && off_out)) { errno = ESPIPE; return -1; }
    char buf[65536];
    size_t chunk = len < sizeof buf ? len : sizeof buf;
    ssize_t r = off_in ? pread(in, buf, chunk, *off_in) : read(in, buf, chunk);
    if (r <= 0) return r;
    ssize_t done = 0;
    while (done < r) {
        ssize_t w = off_out ? pwrite(out, buf + done, (size_t)(r - done), *off_out + done) : write(out, buf + done, (size_t)(r - done));
        if (w < 0) return done ? done : -1;
        done += w;
    }
    if (off_in) *off_in += r;
    if (off_out) *off_out += done;
    return done;
}
ssize_t klb_vmsplice(int fd, const struct iovec *iov, unsigned long n, unsigned flags) {
    if (flags & ~15u) { errno = EINVAL; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (!S_ISFIFO(st.st_mode)) { errno = EBADF; return -1; }
    int fl = fcntl(fd, F_GETFL);
    // the pipe's end decides the direction: a write end takes the user pages, a read end fills them
    return (fl & O_ACCMODE) == O_WRONLY ? writev(fd, iov, (int)n) : readv(fd, iov, (int)n);
}
ssize_t klb_tee(int in, int out, size_t len, unsigned flags) {
    (void)in; (void)out; (void)len; (void)flags;
    errno = ENOSYS; return -1;
}

// ---------------------------------------------------------------- ids, scheduling
// An app cannot change its uid/gid: setres* is allowed only when every argument is -1
// (unchanged) or already the current value, otherwise EPERM — exactly what a process
// without CAP_SETUID gets. The "real, effective, saved" triple is one uid here.
int klb_getresuid(unsigned *r, unsigned *e, unsigned *s) { *r = getuid(); *e = geteuid(); *s = *e; return 0; }
int klb_getresgid(unsigned *r, unsigned *e, unsigned *s) { *r = getgid(); *e = getegid(); *s = *e; return 0; }
static int id_ok(unsigned want, unsigned cur) { return want == (unsigned)-1 || want == cur; }
int klb_setresuid(unsigned r, unsigned e, unsigned s) {
    unsigned u = getuid(), eu = geteuid();
    if (id_ok(r, u) && id_ok(e, eu) && id_ok(s, eu)) return 0;
    errno = EPERM; return -1;
}
int klb_setresgid(unsigned r, unsigned e, unsigned s) {
    unsigned g = getgid(), eg = getegid();
    if (id_ok(r, g) && id_ok(e, eg) && id_ok(s, eg)) return 0;
    errno = EPERM; return -1;
}
// setfsuid/setfsgid return the PREVIOUS value and never report failure.
int klb_setfsuid(unsigned uid) { (void)uid; return (int)geteuid(); }
int klb_setfsgid(unsigned gid) { (void)gid; return (int)getegid(); }

// sched_*: policies are Linux numbers (OTHER 0, FIFO 1, RR 2, BATCH 3, IDLE 5). Real-time
// policies are accepted the way pthread_setschedparam accepts them (kl_pthread.c): they lift the
// thread's QoS, since Darwin has no SCHED_FIFO for apps.
static int sched_pid_ok(int pid) { return pid == 0 || pid == (int)getpid(); }
int klb_sched_getscheduler(int pid) {
    if (!sched_pid_ok(pid)) { errno = ESRCH; return -1; }
    return 0;
}
int klb_sched_setscheduler(int pid, int policy, const int *param) {
    if (!sched_pid_ok(pid)) { errno = ESRCH; return -1; }
    int prio = param ? param[0] : 0;
    switch (policy) {
    case 0: case 3: case 5: if (prio != 0) { errno = EINVAL; return -1; } return 0;
    case 1: case 2:
        if (prio < 1 || prio > 99) { errno = EINVAL; return -1; }
        {
            uint64_t id = 0;
            if (pthread_threadid_np(NULL, &id) == 0 && id) kl_pthread_boost_qos(id);
        }
        return 0;
    }
    errno = EINVAL; return -1;
}
int klb_sched_setparam(int pid, const int *param) {
    if (!sched_pid_ok(pid)) { errno = ESRCH; return -1; }
    return (param && param[0] == 0) ? 0 : (errno = EINVAL, -1);
}

// ---------------------------------------------------------------- privileged / absent
static int denied(const char *what) {
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 8)
        fprintf(stderr, "  [klb] %s: refused with EPERM (an app has no such privilege)\n", what);
    errno = EPERM;
    return -1;
}
int klb_mount(const char *s, const char *t, const char *f, unsigned long fl, const void *d) {
    (void)s; (void)t; (void)f; (void)fl; (void)d; return denied("mount");
}
int klb_umount(const char *t) { (void)t; return denied("umount"); }
int klb_umount2(const char *t, int fl) { (void)t; (void)fl; return denied("umount2"); }
int klb_swapon(const char *p, int fl) { (void)p; (void)fl; return denied("swapon"); }
int klb_swapoff(const char *p) { (void)p; return denied("swapoff"); }
int klb_setns(int fd, int nstype) { (void)fd; (void)nstype; return denied("setns"); }
int klb_unshare(int fl) { (void)fl; return denied("unshare"); }
int klb_acct(const char *f) { (void)f; return denied("acct"); }
int klb_vhangup(void) { return denied("vhangup"); }
int klb_reboot(int cmd) { (void)cmd; return denied("reboot"); }
// personality(0xffffffff) queries; PER_LINUX (0) is the only persona an app can hold.
int klb_personality(unsigned long persona) {
    if (persona == 0xffffffffUL || persona == 0) return 0;
    errno = EINVAL; return -1;
}
// ptrace: nothing can trace an app from inside it. PTRACE_TRACEME (0) succeeds as it does for
// any process that is not yet traced; every other request needs a tracee we cannot have.
long klb_ptrace(int req, ...) {
    if (req == 0) return 0;
    errno = EPERM; return -1;
}
// process_vm_readv/writev on our own address space is a copy; any other pid is out of reach.
static ssize_t pvm(int pid, const struct iovec *loc, unsigned long ln, const struct iovec *rem, unsigned long rn, int write_) {
    if (pid != (int)getpid()) { errno = (kill(pid, 0) == 0 || errno == EPERM) ? EPERM : ESRCH; return -1; }
    size_t done = 0; unsigned long ri = 0; size_t roff = 0;
    for (unsigned long li = 0; li < ln; li++) {
        size_t lo = 0;
        while (lo < loc[li].iov_len) {
            while (ri < rn && roff == rem[ri].iov_len) { ri++; roff = 0; }
            if (ri >= rn) return (ssize_t)done;
            size_t n = loc[li].iov_len - lo;
            if (n > rem[ri].iov_len - roff) n = rem[ri].iov_len - roff;
            char *l = (char *)loc[li].iov_base + lo, *r = (char *)rem[ri].iov_base + roff;
            if (write_) memcpy(r, l, n); else memcpy(l, r, n);
            lo += n; roff += n; done += n;
        }
    }
    return (ssize_t)done;
}
ssize_t klb_process_vm_readv(int pid, const struct iovec *l, unsigned long ln, const struct iovec *r, unsigned long rn, unsigned long fl) {
    if (fl) { errno = EINVAL; return -1; }
    return pvm(pid, l, ln, r, rn, 0);
}
ssize_t klb_process_vm_writev(int pid, const struct iovec *l, unsigned long ln, const struct iovec *r, unsigned long rn, unsigned long fl) {
    if (fl) { errno = EINVAL; return -1; }
    return pvm(pid, l, ln, r, rn, 1);
}

// ---------------------------------------------------------------- varargs consumers, wide printf, err.h
#include <wchar.h>
#include <locale.h>
#include <xlocale.h>
#include "kl_va.h"

FILE *kl_host_file(void *guest);
void klb_exit(int status);

#define KL_MARSHAL2(fmt, va, mode, fail)                                     \
    char _m[512] __attribute__((aligned(16)));                               \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, (mode)) == (size_t)-1)     \
        return fail;                                                         \
    va_list _ap = (va_list)_m

int klx2_vscanf(const char *fmt, kl_va *va) {
    KL_MARSHAL2(fmt, va, KL_VA_SCANF, -1);
    return vscanf(fmt, _ap);
}
int klx2_vfscanf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL2(fmt, va, KL_VA_SCANF, -1);
    return vfscanf(kl_host_file(f), fmt, _ap);
}
int klx2_vdprintf(int fd, const char *fmt, kl_va *va) {
    KL_MARSHAL2(fmt, va, KL_VA_PRINTF, -1);
    return vdprintf(fd, fmt, _ap);
}
// vsyslog: same destination as the variadic syslog (klh_syslog in kl_va_handlers.c).
void klx2_vsyslog(int pri, const char *fmt, kl_va *va) {
    KL_MARSHAL2(fmt, va, KL_VA_PRINTF, );
    fprintf(stderr, "[guest syslog %d] ", pri);
    vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
}

// The wide printf family. The marshaller walks a narrow format, but a wide format's specifiers
// are ASCII, so a narrowed copy (anything above 0x7F becomes '?', which is no specifier) drives
// the argument walk and Darwin formats from the original wide string. The wide functions convert
// narrow strings (%s) with the thread's locale: bionic converts as UTF-8, and the host runs in
// the "C" locale, so the call is made under a UTF-8 locale when the system has one.
static locale_t utf8_locale(void) {
    static locale_t loc;
    static _Atomic int tried;
    if (!atomic_load(&tried)) {
        loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", NULL);
        if (!loc) loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL);
        atomic_store(&tried, 1);
    }
    return loc;
}
static int wmarshal(const wchar_t *wfmt, kl_va *va, char *out, size_t cap) {
    size_t n = wcslen(wfmt);
    char *nf = malloc(n + 1);
    if (!nf) return -1;
    for (size_t i = 0; i < n; i++) nf[i] = (wfmt[i] > 0 && wfmt[i] < 0x80) ? (char)wfmt[i] : '?';
    nf[n] = 0;
    size_t r = kl_va_marshal(nf, va, out, cap, KL_VA_PRINTF);
    free(nf);
    return r == (size_t)-1 ? -1 : 0;
}
int klx2_vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, kl_va *va) {
    char m[1024] __attribute__((aligned(16)));
    if (wmarshal(fmt, va, m, sizeof m) != 0) return -1;
    locale_t l = utf8_locale(), old = l ? uselocale(l) : 0;
    int r = vswprintf(s, n, fmt, (va_list)m);
    if (l) uselocale(old);
    return r;
}
int klx2_vfwprintf(void *f, const wchar_t *fmt, kl_va *va) {
    char m[1024] __attribute__((aligned(16)));
    if (wmarshal(fmt, va, m, sizeof m) != 0) return -1;
    locale_t l = utf8_locale(), old = l ? uselocale(l) : 0;
    int r = vfwprintf(kl_host_file(f), fmt, (va_list)m);
    if (l) uselocale(old);
    return r;
}
int klx2_vwprintf(const wchar_t *fmt, kl_va *va) { return klx2_vfwprintf(stdout, fmt, va); }

// BSD err.h: "<progname>: <message>[: <strerror(errno)>]\n" on stderr, then exit(eval) for err/errx.
// The program name is bionic's __progname (see getprogname in kl_bionic_extra.c).
extern const char *klb_getprogname_for_err(void);
void klx2_verr(int eval, const char *fmt, kl_va *va, int with_errno, int do_exit) {
    int saved = errno;
    fprintf(stderr, "%s: ", klb_getprogname_for_err());
    if (fmt) {
        char m[512] __attribute__((aligned(16)));
        if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) != (size_t)-1) vfprintf(stderr, fmt, (va_list)m);
        if (with_errno) fputs(": ", stderr);
    }
    if (with_errno) fputs(strerror(saved), stderr);
    fputc('\n', stderr);
    if (do_exit) klb_exit(eval);
}
// verr/verrx/vwarn/vwarnx take the descriptor by reference already
void klx2_verr_(int eval, const char *fmt, kl_va *va) { klx2_verr(eval, fmt, va, 1, 1); }
void klx2_verrx_(int eval, const char *fmt, kl_va *va) { klx2_verr(eval, fmt, va, 0, 1); }
void klx2_vwarn_(const char *fmt, kl_va *va) { klx2_verr(0, fmt, va, 1, 0); }
void klx2_vwarnx_(const char *fmt, kl_va *va) { klx2_verr(0, fmt, va, 0, 0); }
