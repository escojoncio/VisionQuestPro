// Darwin-only half of kl_bionic_extra3.c: the pieces that need kqueue, termios, the host's FILE
// internals or Darwin path calls. The rows are served through kl_bionic_extra4_lookup, which
// kl_bionic_extra3_lookup consults after its own table. Same three rules as the other files:
// forward what is identical, translate what differs, answer honestly for what an app cannot have.
//
// Descriptors that Linux makes up (timerfd, signalfd, inotify, pidfd) are real descriptors here
// too: a self-connected AF_UNIX datagram socket (the eventfd construction in kl_libc_slink.c) the
// guest reads with plain read()/poll()/epoll, fed by a thread or a signal handler of ours. A
// small registry maps the fd to its state, and klb_close calls kl_extra4_fd_closed so the
// feeder stops before the descriptor number can be reused.
//
// Signal numbers follow the convention klb_sigaction and klb_pthread_kill already set: the
// guest's number is handed to Darwin as it is.
#define _DARWIN_C_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include "klepton.h"

int kl_open_flags(int lx);                      // kl_libc.c
int kl_clock_to_darwin(int clk);                // kl_libc.c
void *kl_shim_lookup(const char *name);         // kl_shim.c

#define GUEST_PATH(p) char _kp[1024]; const char *_p = kl_guest_path((p), _kp, sizeof _kp)
#define LX_AT_FDCWD (-100)
#define LX_O_NONBLOCK 0x800
#define LX_O_CLOEXEC 0x80000
static int at_fd(int d) { return d == LX_AT_FDCWD ? AT_FDCWD : d; }

typedef struct { const char *name; void *fn; } kl4_entry;
#define E4(n, f) { n, (void *)(f) }

// A function that takes anything and fails the same way, bound under several names.
static long kl4_enosys(void) { errno = ENOSYS; return -1; }
static long kl4_eperm(void) { errno = EPERM; return -1; }
static long kl4_enomem(void) { errno = ENOMEM; return -1; }
static long kl4_enoent(void) { errno = ENOENT; return -1; }
static long kl4_enotty(void) { errno = ENOTTY; return -1; }

// ======================================================================
// the fd registry (feeder lifetime) and the datagram "event" socket
// ======================================================================
enum { KF_TIMERFD = 1, KF_SIGNALFD, KF_INOTIFY, KF_PIDFD };
typedef struct { int fd, kind; void *obj; } kreg;
#define KREG_N 128
static kreg g_kreg[KREG_N];
static pthread_mutex_t g_kreg_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_kreg_count;

static int kreg_add(int fd, int kind, void *obj) {
    int ok = 0;
    pthread_mutex_lock(&g_kreg_mu);
    for (int i = 0; i < KREG_N; i++)
        if (!g_kreg[i].kind) { g_kreg[i].fd = fd; g_kreg[i].kind = kind; g_kreg[i].obj = obj; ok = 1; atomic_fetch_add(&g_kreg_count, 1); break; }
    pthread_mutex_unlock(&g_kreg_mu);
    return ok;
}
static void *kreg_find(int fd, int kind) {
    void *o = NULL;
    pthread_mutex_lock(&g_kreg_mu);
    for (int i = 0; i < KREG_N; i++) if (g_kreg[i].kind == kind && g_kreg[i].fd == fd) { o = g_kreg[i].obj; break; }
    pthread_mutex_unlock(&g_kreg_mu);
    return o;
}
static void tfd_destroy(void *o);
static void sfd_destroy(int fd);
static void ino_destroy(void *o);
// Called by klb_close BEFORE the descriptor is closed.
void kl_extra4_fd_closed(int fd) {
    if (!atomic_load(&g_kreg_count)) return;
    int kind = 0; void *obj = NULL;
    pthread_mutex_lock(&g_kreg_mu);
    for (int i = 0; i < KREG_N; i++)
        if (g_kreg[i].kind && g_kreg[i].fd == fd) { kind = g_kreg[i].kind; obj = g_kreg[i].obj; g_kreg[i].kind = 0; atomic_fetch_sub(&g_kreg_count, 1); break; }
    pthread_mutex_unlock(&g_kreg_mu);
    switch (kind) {
    case KF_TIMERFD: tfd_destroy(obj); break;
    case KF_SIGNALFD: sfd_destroy(fd); break;
    case KF_INOTIFY: ino_destroy(obj); break;
    default: break;
    }
}

// One descriptor that is readable once something is written to it: a datagram socket connected
// to itself through a name that is unlinked straight away (see klb_eventfd for the reasoning).
static int mk_selfsock(int lxflags) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    static _Atomic unsigned seq;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(a.sun_path, sizeof a.sun_path, "%s/kl-ev-%d-%u", tmp, (int)getpid(), atomic_fetch_add(&seq, 1));
    unlink(a.sun_path);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno; unlink(a.sun_path); close(s); errno = e;
        return -1;
    }
    unlink(a.sun_path);
    int buf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    if (lxflags & LX_O_CLOEXEC) fcntl(s, F_SETFD, FD_CLOEXEC);
    if (lxflags & LX_O_NONBLOCK) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

// ======================================================================
// timerfd: one thread per armed timer; the guest reads a uint64 expiration count.
// ======================================================================
typedef struct {
    int fd, dclk;
    pthread_mutex_t m; pthread_cond_t c;
    int armed, stop, started;
    struct timespec expire, interval;      // expire is absolute on dclk
    pthread_t th;
} ktimer;
static void ts_add4(struct timespec *a, const struct timespec *b) {
    a->tv_sec += b->tv_sec; a->tv_nsec += b->tv_nsec;
    if (a->tv_nsec >= 1000000000L) { a->tv_nsec -= 1000000000L; a->tv_sec++; }
}
static int ts_cmp4(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    return a->tv_nsec < b->tv_nsec ? -1 : a->tv_nsec > b->tv_nsec ? 1 : 0;
}
static void drain_sock(int fd, uint64_t *sum) {
    uint64_t v;
    while (recv(fd, &v, sizeof v, MSG_DONTWAIT) == (ssize_t)sizeof v) if (sum) *sum += v;
}
// Deliver `n` expirations as ONE datagram, folding in whatever the guest has not read yet.
static void tfd_deliver(int fd, uint64_t n) {
    drain_sock(fd, &n);
    send(fd, &n, sizeof n, MSG_DONTWAIT);
}
static void *tfd_thread(void *arg) {
    ktimer *t = arg;
    pthread_mutex_lock(&t->m);
    while (!t->stop) {
        if (!t->armed) { pthread_cond_wait(&t->c, &t->m); continue; }
        struct timespec now;
        clock_gettime((clockid_t)t->dclk, &now);
        if (ts_cmp4(&now, &t->expire) < 0) {
            struct timespec rel = { t->expire.tv_sec - now.tv_sec, t->expire.tv_nsec - now.tv_nsec };
            if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
            pthread_cond_timedwait_relative_np(&t->c, &t->m, &rel);
            continue;
        }
        uint64_t n = 1;
        if (t->interval.tv_sec || t->interval.tv_nsec) {
            ts_add4(&t->expire, &t->interval);
            while (ts_cmp4(&now, &t->expire) >= 0) { ts_add4(&t->expire, &t->interval); n++; }
        } else t->armed = 0;
        int fd = t->fd;
        pthread_mutex_unlock(&t->m);
        tfd_deliver(fd, n);
        pthread_mutex_lock(&t->m);
    }
    pthread_mutex_unlock(&t->m);
    return NULL;
}
static void tfd_destroy(void *o) {
    ktimer *t = o;
    pthread_mutex_lock(&t->m); t->stop = 1; t->armed = 0; pthread_cond_signal(&t->c); pthread_mutex_unlock(&t->m);
    if (t->started) pthread_join(t->th, NULL);
    pthread_mutex_destroy(&t->m); pthread_cond_destroy(&t->c);
    free(t);
}
int kl4_timerfd_create(int clk, int flags) {
    if (flags & ~(LX_O_NONBLOCK | LX_O_CLOEXEC)) { errno = EINVAL; return -1; }
    if (clk == 8 || clk == 9) { errno = EPERM; return -1; }                 // the *_ALARM clocks need CAP_WAKE_ALARM
    if (clk != 0 && clk != 1 && clk != 7) { errno = EINVAL; return -1; }
    ktimer *t = calloc(1, sizeof *t);
    if (!t) { errno = ENOMEM; return -1; }
    t->dclk = kl_clock_to_darwin(clk);
    pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->c, NULL);
    t->fd = mk_selfsock(flags);
    if (t->fd < 0) { int e = errno; free(t); errno = e; return -1; }
    if (!kreg_add(t->fd, KF_TIMERFD, t)) { close(t->fd); free(t); errno = EMFILE; return -1; }
    return t->fd;
}
typedef struct { struct timespec interval, value; } kitimerspec;
static void tfd_remaining(ktimer *t, kitimerspec *out) {
    memset(out, 0, sizeof *out);
    if (!t->armed) return;
    struct timespec now; clock_gettime((clockid_t)t->dclk, &now);
    if (ts_cmp4(&now, &t->expire) < 0) {
        out->value.tv_sec = t->expire.tv_sec - now.tv_sec;
        out->value.tv_nsec = t->expire.tv_nsec - now.tv_nsec;
        if (out->value.tv_nsec < 0) { out->value.tv_nsec += 1000000000L; out->value.tv_sec--; }
    } else out->value.tv_nsec = 1;
    out->interval = t->interval;
}
int kl4_timerfd_gettime(int fd, kitimerspec *out) {
    ktimer *t = kreg_find(fd, KF_TIMERFD);
    if (!t) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&t->m); tfd_remaining(t, out); pthread_mutex_unlock(&t->m);
    return 0;
}
int kl4_timerfd_settime(int fd, int flags, const kitimerspec *nv, kitimerspec *ov) {
    ktimer *t = kreg_find(fd, KF_TIMERFD);
    if (!t || !nv) { errno = t ? EFAULT : EINVAL; return -1; }
    if (flags & ~3) { errno = EINVAL; return -1; }                          // TFD_TIMER_ABSTIME 1, TFD_TIMER_CANCEL_ON_SET 2
    if (nv->value.tv_nsec < 0 || nv->value.tv_nsec >= 1000000000L || nv->interval.tv_nsec < 0 ||
        nv->interval.tv_nsec >= 1000000000L || nv->value.tv_sec < 0 || nv->interval.tv_sec < 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&t->m);
    if (ov) tfd_remaining(t, ov);
    if (nv->value.tv_sec == 0 && nv->value.tv_nsec == 0) t->armed = 0;
    else {
        if (flags & 1) t->expire = nv->value;
        else { clock_gettime((clockid_t)t->dclk, &t->expire); ts_add4(&t->expire, &nv->value); }
        t->interval = nv->interval;
        t->armed = 1;
        if (!t->started) {
            if (pthread_create(&t->th, NULL, tfd_thread, t) != 0) { t->armed = 0; pthread_mutex_unlock(&t->m); errno = EAGAIN; return -1; }
            t->started = 1;
        }
    }
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    drain_sock(fd, NULL);                                                   // re-arming discards unread expirations
    return 0;
}

// ======================================================================
// signalfd: our handler queues one 128-byte signalfd_siginfo per signal on every signalfd whose
// mask holds it. The signals named are taken over from any handler the guest installed (a signalfd
// user blocks them for exactly this purpose); closing the last signalfd restores the old one.
// ======================================================================
#define SFD_MAX 16
static struct { _Atomic int fd; _Atomic uint64_t mask; } g_sfd[SFD_MAX];
static struct sigaction g_sfd_old[32];
static int g_sfd_installed[32];
static pthread_mutex_t g_sfd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_sfd_once = PTHREAD_ONCE_INIT;
static void sfd_init(void) { for (int i = 0; i < SFD_MAX; i++) atomic_store(&g_sfd[i].fd, -1); }
static void sfd_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    int saved = errno;
    uint8_t rec[128];
    memset(rec, 0, sizeof rec);
    uint32_t s = (uint32_t)sig, pid = si ? (uint32_t)si->si_pid : 0, uid = si ? (uint32_t)si->si_uid : 0;
    memcpy(rec, &s, 4); memcpy(rec + 12, &pid, 4); memcpy(rec + 16, &uid, 4);    // ssi_signo, ssi_pid, ssi_uid
    for (int i = 0; i < SFD_MAX; i++) {
        int fd = atomic_load(&g_sfd[i].fd);
        if (fd >= 0 && (atomic_load(&g_sfd[i].mask) >> (sig - 1) & 1)) send(fd, rec, sizeof rec, MSG_DONTWAIT);
    }
    errno = saved;
}
// Recompute which signals need the handler and install/restore accordingly. Caller holds g_sfd_mu.
static void sfd_apply(void) {
    uint64_t want = 0;
    for (int i = 0; i < SFD_MAX; i++) if (atomic_load(&g_sfd[i].fd) >= 0) want |= atomic_load(&g_sfd[i].mask);
    for (int sig = 1; sig < 32; sig++) {
        int need = (want >> (sig - 1)) & 1;
        if (need && !g_sfd_installed[sig]) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = sfd_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            if (sigaction(sig, &sa, &g_sfd_old[sig]) == 0) g_sfd_installed[sig] = 1;
        } else if (!need && g_sfd_installed[sig]) {
            sigaction(sig, &g_sfd_old[sig], NULL);
            g_sfd_installed[sig] = 0;
        }
    }
}
static void sfd_destroy(int fd) {
    pthread_mutex_lock(&g_sfd_mu);
    for (int i = 0; i < SFD_MAX; i++) if (atomic_load(&g_sfd[i].fd) == fd) { atomic_store(&g_sfd[i].fd, -1); atomic_store(&g_sfd[i].mask, 0); }
    sfd_apply();
    pthread_mutex_unlock(&g_sfd_mu);
}
// signalfd(fd, mask, flags): fd == -1 makes a new one, otherwise the mask of an existing one is replaced.
int kl4_signalfd(int fd, const uint64_t *mask, int flags) {
    pthread_once(&g_sfd_once, sfd_init);
    if (!mask || (flags & ~(LX_O_NONBLOCK | LX_O_CLOEXEC))) { errno = EINVAL; return -1; }
    uint64_t m = *mask & 0x7FFFFFFFull & ~((uint64_t)1 << 8) & ~((uint64_t)1 << 16);   // SIGKILL (9) and Darwin's SIGSTOP (17) are ignored
    pthread_mutex_lock(&g_sfd_mu);
    int slot = -1;
    if (fd >= 0) {
        for (int i = 0; i < SFD_MAX; i++) if (atomic_load(&g_sfd[i].fd) == fd) { slot = i; break; }
        if (slot < 0) { pthread_mutex_unlock(&g_sfd_mu); errno = EINVAL; return -1; }
    } else {
        for (int i = 0; i < SFD_MAX; i++) if (atomic_load(&g_sfd[i].fd) < 0) { slot = i; break; }
        if (slot < 0) { pthread_mutex_unlock(&g_sfd_mu); errno = EMFILE; return -1; }
        fd = mk_selfsock(flags);
        if (fd < 0) { int e = errno; pthread_mutex_unlock(&g_sfd_mu); errno = e; return -1; }
        if (!kreg_add(fd, KF_SIGNALFD, NULL)) { close(fd); pthread_mutex_unlock(&g_sfd_mu); errno = EMFILE; return -1; }
        atomic_store(&g_sfd[slot].fd, fd);
    }
    atomic_store(&g_sfd[slot].mask, m);
    sfd_apply();
    pthread_mutex_unlock(&g_sfd_mu);
    return fd;
}

// ======================================================================
// inotify over kqueue (EVFILT_VNODE). kqueue watches an open vnode, not a path, and says WHAT
// kind of change happened but not to which child; so
//   * a watched FILE maps directly: NOTE_WRITE/EXTEND -> IN_MODIFY, ATTRIB/LINK -> IN_ATTRIB,
//     DELETE -> IN_DELETE_SELF, RENAME -> IN_MOVE_SELF;
//   * a watched DIRECTORY reports NOTE_WRITE when its entries change; the watcher then re-reads
//     the listing and diffs it against the last one to produce IN_CREATE / IN_DELETE /
//     IN_MOVED_FROM+TO (same inode under a new name, joined by a cookie), and each regular child
//     (up to KI_KIDS per directory) carries its own vnode watch so IN_MODIFY / IN_ATTRIB on a
//     child arrive with its name.
// Not obtainable from kqueue, so never delivered: IN_ACCESS, IN_OPEN, IN_CLOSE_WRITE,
// IN_CLOSE_NOWRITE (a caller that waits for IN_CLOSE_WRITE sees IN_MODIFY instead), and
// modifications to children beyond KI_KIDS in a directory.
// ======================================================================
#define IN_ACCESS 0x1u
#define IN_MODIFY 0x2u
#define IN_ATTRIB 0x4u
#define IN_MOVED_FROM 0x40u
#define IN_MOVED_TO 0x80u
#define IN_CREATE 0x100u
#define IN_DELETE 0x200u
#define IN_DELETE_SELF 0x400u
#define IN_MOVE_SELF 0x800u
#define IN_UNMOUNT 0x2000u
#define IN_Q_OVERFLOW 0x4000u
#define IN_IGNORED 0x8000u
#define IN_ONLYDIR 0x01000000u
#define IN_MASK_CREATE 0x10000000u
#define IN_MASK_ADD 0x20000000u
#define IN_ISDIR 0x40000000u
#define IN_ONESHOT 0x80000000u
#define IN_ALL_EVENTS_USED 0x00000FFFu
#define KI_KIDS 128
#define KI_NOTES (NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_REVOKE)

typedef struct kkid { char name[256]; ino_t ino; int vfd; struct timespec mtime; off_t size; int isdir; } kkid;
typedef struct kwatch {
    int wd, vfd, is_dir;
    uint32_t mask;
    char *path;
    dev_t dev; ino_t ino;
    kkid *kids; int nkids;
    struct kwatch *next;
} kwatch;
typedef struct {
    int fd, kq, wake[2];
    pthread_t th;
    pthread_mutex_t m;
    kwatch *w;
    int next_wd;
    uint32_t cookie;
    int overflowed;
} kino;

static void ino_emit(kino *k, int wd, uint32_t mask, uint32_t cookie, const char *name) {
    uint8_t buf[16 + 256 + 16];
    uint32_t len = 0;
    if (name && *name) { size_t n = strlen(name) + 1; len = (uint32_t)((n + 15) & ~(size_t)15); if (len > 272) len = 272; }
    memset(buf, 0, 16 + len);
    memcpy(buf, &wd, 4); memcpy(buf + 4, &mask, 4); memcpy(buf + 8, &cookie, 4); memcpy(buf + 12, &len, 4);
    if (len) { size_t n = strlen(name); if (n > len - 1) n = len - 1; memcpy(buf + 16, name, n); }
    if (send(k->fd, buf, 16 + len, MSG_DONTWAIT) < 0 && !k->overflowed) {
        k->overflowed = 1;                                        // queue full: say so once, as the kernel does
        int w = -1; uint32_t m = IN_Q_OVERFLOW, z = 0;
        uint8_t ov[16]; memcpy(ov, &w, 4); memcpy(ov + 4, &m, 4); memcpy(ov + 8, &z, 4); memcpy(ov + 12, &z, 4);
        send(k->fd, ov, sizeof ov, MSG_DONTWAIT);
    }
}
static void kids_free(kwatch *w) {
    for (int i = 0; i < w->nkids; i++) if (w->kids[i].vfd >= 0) close(w->kids[i].vfd);
    free(w->kids); w->kids = NULL; w->nkids = 0;
}
static void kid_watch(kino *k, kkid *c, const char *dir) {
    c->vfd = -1;
    if (c->isdir) return;
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, c->name);
    int fd = open(p, O_EVTONLY);
    if (fd < 0) return;
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, NULL);
    if (kevent(k->kq, &ev, 1, NULL, 0, NULL) < 0) { close(fd); return; }
    c->vfd = fd;
}
// Reads the directory into a fresh kid array (names, inodes, times); vfds are filled by the caller.
static kkid *dir_list(const char *path, int *n) {
    DIR *d = opendir(path);
    if (!d) { *n = 0; return NULL; }
    kkid *a = NULL; int cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 255) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 32; kkid *g = realloc(a, (size_t)cap * sizeof *a); if (!g) break; a = g; }
        kkid *c = &a[cnt++];
        memset(c, 0, sizeof *c);
        strcpy(c->name, e->d_name); c->vfd = -1;
        char p[1400]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (lstat(p, &st) == 0) { c->ino = st.st_ino; c->mtime = st.st_mtimespec; c->size = st.st_size; c->isdir = S_ISDIR(st.st_mode); }
    }
    closedir(d);
    *n = cnt;
    return a;
}
static void dir_rescan(kino *k, kwatch *w) {
    int nn; kkid *nw = dir_list(w->path, &nn);
    kkid *old = w->kids; int on = w->nkids;
    uint8_t *old_used = calloc((size_t)(on ? on : 1), 1), *new_used = calloc((size_t)(nn ? nn : 1), 1);
    if (!old_used || !new_used) { free(old_used); free(new_used); free(nw); return; }
    // unchanged names keep their vnode watch; a name whose inode changed is a replacement
    for (int i = 0; i < nn; i++)
        for (int j = 0; j < on; j++)
            if (!old_used[j] && !strcmp(nw[i].name, old[j].name)) {
                if (nw[i].ino == old[j].ino) {
                    old_used[j] = 1; new_used[i] = 1;
                    nw[i].vfd = old[j].vfd; old[j].vfd = -1;
                    if ((nw[i].mtime.tv_sec != old[j].mtime.tv_sec || nw[i].mtime.tv_nsec != old[j].mtime.tv_nsec || nw[i].size != old[j].size) && (w->mask & IN_MODIFY))
                        ino_emit(k, w->wd, IN_MODIFY | (nw[i].isdir ? IN_ISDIR : 0), 0, nw[i].name);
                }
                break;
            }
    // what is left on each side: pair equal inodes as a move, the rest are creates/deletes
    for (int j = 0; j < on; j++) {
        if (old_used[j]) continue;
        int moved = -1;
        for (int i = 0; i < nn; i++) if (!new_used[i] && nw[i].ino == old[j].ino) { moved = i; break; }
        uint32_t dirf = old[j].isdir ? IN_ISDIR : 0;
        if (moved >= 0) {
            uint32_t ck = ++k->cookie;
            if (w->mask & IN_MOVED_FROM) ino_emit(k, w->wd, IN_MOVED_FROM | dirf, ck, old[j].name);
            if (w->mask & IN_MOVED_TO) ino_emit(k, w->wd, IN_MOVED_TO | dirf, ck, nw[moved].name);
            new_used[moved] = 1;
            nw[moved].vfd = old[j].vfd; old[j].vfd = -1;
        } else if (w->mask & IN_DELETE) ino_emit(k, w->wd, IN_DELETE | dirf, 0, old[j].name);
        old_used[j] = 1;
    }
    for (int i = 0; i < nn; i++)
        if (!new_used[i] && (w->mask & IN_CREATE)) ino_emit(k, w->wd, IN_CREATE | (nw[i].isdir ? IN_ISDIR : 0), 0, nw[i].name);
    for (int j = 0; j < on; j++) if (old[j].vfd >= 0) close(old[j].vfd);
    free(old);
    int watched = 0;
    for (int i = 0; i < nn; i++) if (nw[i].vfd >= 0) watched++;
    for (int i = 0; i < nn; i++) if (nw[i].vfd < 0 && watched < KI_KIDS) { kid_watch(k, &nw[i], w->path); if (nw[i].vfd >= 0) watched++; }
    w->kids = nw; w->nkids = nn;
    free(old_used); free(new_used);
}
static void watch_remove(kino *k, kwatch *w, int emit_ignored) {
    kwatch **pp = &k->w;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    if (emit_ignored) ino_emit(k, w->wd, IN_IGNORED, 0, NULL);
    kids_free(w);
    if (w->vfd >= 0) close(w->vfd);
    free(w->path); free(w);
}
static void ino_handle(kino *k, const struct kevent *ev) {
    int id = (int)ev->ident;
    for (kwatch *w = k->w; w; w = w->next) {
        if (w->vfd == id) {
            uint32_t f = ev->fflags, isd = w->is_dir ? IN_ISDIR : 0;
            if (w->is_dir && (f & NOTE_WRITE)) dir_rescan(k, w);
            if (!w->is_dir && (f & (NOTE_WRITE | NOTE_EXTEND)) && (w->mask & IN_MODIFY)) ino_emit(k, w->wd, IN_MODIFY, 0, NULL);
            if ((f & (NOTE_ATTRIB | NOTE_LINK)) && (w->mask & IN_ATTRIB)) ino_emit(k, w->wd, IN_ATTRIB | isd, 0, NULL);
            if ((f & NOTE_RENAME) && (w->mask & IN_MOVE_SELF)) ino_emit(k, w->wd, IN_MOVE_SELF | isd, 0, NULL);
            if (f & (NOTE_DELETE | NOTE_REVOKE)) {
                if (w->mask & IN_DELETE_SELF) ino_emit(k, w->wd, IN_DELETE_SELF | isd, 0, NULL);
                watch_remove(k, w, 1);
                return;
            }
            if (w->mask & IN_ONESHOT) { watch_remove(k, w, 1); }
            return;
        }
        for (int i = 0; i < w->nkids; i++) {
            if (w->kids[i].vfd != id) continue;
            uint32_t f = ev->fflags;
            if ((f & (NOTE_WRITE | NOTE_EXTEND)) && (w->mask & IN_MODIFY)) ino_emit(k, w->wd, IN_MODIFY, 0, w->kids[i].name);
            if ((f & NOTE_ATTRIB) && (w->mask & IN_ATTRIB)) ino_emit(k, w->wd, IN_ATTRIB, 0, w->kids[i].name);
            struct stat st;                                               // keep the diff baseline current
            char p[1400]; snprintf(p, sizeof p, "%s/%s", w->path, w->kids[i].name);
            if (lstat(p, &st) == 0) { w->kids[i].mtime = st.st_mtimespec; w->kids[i].size = st.st_size; }
            return;
        }
    }
}
static void *ino_thread(void *arg) {
    kino *k = arg;
    struct kevent evs[16];
    for (;;) {
        int n = kevent(k->kq, NULL, 0, evs, 16, NULL);
        if (n < 0) { if (errno == EINTR) continue; break; }
        int stop = 0;
        pthread_mutex_lock(&k->m);
        for (int i = 0; i < n; i++) {
            if ((int)evs[i].ident == k->wake[0] && evs[i].filter == EVFILT_READ) { stop = 1; break; }
            if (evs[i].filter == EVFILT_VNODE) ino_handle(k, &evs[i]);
        }
        pthread_mutex_unlock(&k->m);
        if (stop) break;
    }
    return NULL;
}
static void ino_destroy(void *o) {
    kino *k = o;
    char b = 1;
    (void)!write(k->wake[1], &b, 1);
    pthread_join(k->th, NULL);
    pthread_mutex_lock(&k->m);
    while (k->w) watch_remove(k, k->w, 0);
    pthread_mutex_unlock(&k->m);
    close(k->wake[0]); close(k->wake[1]); close(k->kq);
    pthread_mutex_destroy(&k->m);
    free(k);
}
int kl4_inotify_init1(int flags) {
    if (flags & ~(LX_O_NONBLOCK | LX_O_CLOEXEC)) { errno = EINVAL; return -1; }
    kino *k = calloc(1, sizeof *k);
    if (!k) { errno = ENOMEM; return -1; }
    pthread_mutex_init(&k->m, NULL);
    k->wake[0] = k->wake[1] = -1; k->next_wd = 1;
    k->kq = kqueue();
    k->fd = k->kq >= 0 ? mk_selfsock(flags) : -1;
    if (k->fd < 0 || pipe(k->wake) < 0) {
        int e = errno;
        if (k->kq >= 0) close(k->kq);
        if (k->fd >= 0) close(k->fd);
        free(k); errno = e; return -1;
    }
    struct kevent ev;
    EV_SET(&ev, k->wake[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(k->kq, &ev, 1, NULL, 0, NULL);
    if (!kreg_add(k->fd, KF_INOTIFY, k) || pthread_create(&k->th, NULL, ino_thread, k) != 0) {
        close(k->wake[0]); close(k->wake[1]); close(k->kq); close(k->fd); free(k);
        errno = EMFILE; return -1;
    }
    return k->fd;
}
int kl4_inotify_init(void) { return kl4_inotify_init1(0); }
int kl4_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    kino *k = kreg_find(fd, KF_INOTIFY);
    if (!k) { errno = EINVAL; return -1; }
    if (!path || !(mask & (IN_ALL_EVENTS_USED | IN_ONESHOT))) { errno = EINVAL; return -1; }
    GUEST_PATH(path);
    struct stat st;
    if (stat(_p, &st) < 0) return -1;
    int isdir = S_ISDIR(st.st_mode);
    if ((mask & IN_ONLYDIR) && !isdir) { errno = ENOTDIR; return -1; }
    pthread_mutex_lock(&k->m);
    for (kwatch *w = k->w; w; w = w->next)
        if (w->dev == st.st_dev && w->ino == st.st_ino) {
            if (mask & IN_MASK_CREATE) { pthread_mutex_unlock(&k->m); errno = EEXIST; return -1; }
            w->mask = (mask & IN_MASK_ADD) ? (w->mask | mask) : mask;
            int wd = w->wd;
            pthread_mutex_unlock(&k->m);
            return wd;
        }
    kwatch *w = calloc(1, sizeof *w);
    if (!w) { pthread_mutex_unlock(&k->m); errno = ENOMEM; return -1; }
    w->path = strdup(_p);
    w->vfd = open(_p, O_EVTONLY);
    if (!w->path || w->vfd < 0) { int e = w->vfd < 0 ? errno : ENOMEM; free(w->path); free(w); pthread_mutex_unlock(&k->m); errno = e; return -1; }
    struct kevent ev;
    EV_SET(&ev, w->vfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, KI_NOTES, 0, NULL);
    if (kevent(k->kq, &ev, 1, NULL, 0, NULL) < 0) { int e = errno; close(w->vfd); free(w->path); free(w); pthread_mutex_unlock(&k->m); errno = e; return -1; }
    w->wd = k->next_wd++; w->mask = mask & ~IN_MASK_ADD; w->is_dir = isdir; w->dev = st.st_dev; w->ino = st.st_ino;
    w->next = k->w; k->w = w;
    if (isdir) {                                                         // baseline listing + child vnode watches
        w->kids = dir_list(w->path, &w->nkids);
        int watched = 0;
        for (int i = 0; i < w->nkids && watched < KI_KIDS; i++) { kid_watch(k, &w->kids[i], w->path); if (w->kids[i].vfd >= 0) watched++; }
    }
    int wd = w->wd;
    pthread_mutex_unlock(&k->m);
    return wd;
}
int kl4_inotify_rm_watch(int fd, int wd) {
    kino *k = kreg_find(fd, KF_INOTIFY);
    if (!k) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&k->m);
    for (kwatch *w = k->w; w; w = w->next)
        if (w->wd == wd) { watch_remove(k, w, 1); pthread_mutex_unlock(&k->m); return 0; }
    pthread_mutex_unlock(&k->m);
    errno = EINVAL;
    return -1;
}

// ======================================================================
// termios. bionic's struct termios is the kernel's: four 32-bit flag words, c_line, c_cc[19]
// (the baud rate lives in c_cflag's CBAUD bits). Darwin's has 64-bit flag words, c_cc[20] and
// separate speeds, and every flag has a different value. A forwarded tcgetattr filled the guest's
// struct with Darwin's layout.
// ======================================================================
typedef struct { uint32_t c_iflag, c_oflag, c_cflag, c_lflag; uint8_t c_line; uint8_t c_cc[19]; } lx_termios;
static const struct { uint32_t lx; unsigned long dw; } T_IFLAG[] = {
    {0x1, IGNBRK}, {0x2, BRKINT}, {0x4, IGNPAR}, {0x8, PARMRK}, {0x10, INPCK}, {0x20, ISTRIP}, {0x40, INLCR},
    {0x80, IGNCR}, {0x100, ICRNL}, {0x400, IXON}, {0x800, IXANY}, {0x1000, IXOFF}, {0x2000, IMAXBEL}, {0x4000, IUTF8},
};
static const struct { uint32_t lx; unsigned long dw; } T_OFLAG[] = {
    {0x1, OPOST}, {0x4, ONLCR}, {0x8, OCRNL}, {0x10, ONOCR}, {0x20, ONLRET},
};
static const struct { uint32_t lx; unsigned long dw; } T_CFLAG[] = {
    {0x40, CSTOPB}, {0x80, CREAD}, {0x100, PARENB}, {0x200, PARODD}, {0x400, HUPCL}, {0x800, CLOCAL},
};
static const struct { uint32_t lx; unsigned long dw; } T_LFLAG[] = {
    {0x1, ISIG}, {0x2, ICANON}, {0x8, ECHO}, {0x10, ECHOE}, {0x20, ECHOK}, {0x40, ECHONL}, {0x80, NOFLSH},
    {0x100, TOSTOP}, {0x200, ECHOCTL}, {0x400, ECHOPRT}, {0x800, ECHOKE}, {0x8000, IEXTEN},
};
#define NTAB(t) (sizeof(t) / sizeof((t)[0]))
// Linux c_cc index -> Darwin c_cc index (-1: Darwin has no such character)
static const int T_CC[17] = { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, -1, VSTART, VSTOP, VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };
static const struct { uint32_t lx; unsigned long dw; } T_BAUD[] = {
    {0, 0}, {1, 50}, {2, 75}, {3, 110}, {4, 134}, {5, 150}, {6, 200}, {7, 300}, {8, 600}, {9, 1200}, {10, 1800},
    {11, 2400}, {12, 4800}, {13, 9600}, {14, 19200}, {15, 38400}, {0x1001, 57600}, {0x1002, 115200}, {0x1003, 230400},
};
static unsigned long baud_to_dw(uint32_t lx) { for (size_t i = 0; i < NTAB(T_BAUD); i++) if (T_BAUD[i].lx == lx) return T_BAUD[i].dw; return 38400; }
static uint32_t baud_to_lx(unsigned long dw) { for (size_t i = 0; i < NTAB(T_BAUD); i++) if (T_BAUD[i].dw == dw) return T_BAUD[i].lx; return 15; }
static void term_to_lx(const struct termios *d, lx_termios *l) {
    memset(l, 0, sizeof *l);
    for (size_t i = 0; i < NTAB(T_IFLAG); i++) if (d->c_iflag & T_IFLAG[i].dw) l->c_iflag |= T_IFLAG[i].lx;
    for (size_t i = 0; i < NTAB(T_OFLAG); i++) if (d->c_oflag & T_OFLAG[i].dw) l->c_oflag |= T_OFLAG[i].lx;
    for (size_t i = 0; i < NTAB(T_CFLAG); i++) if (d->c_cflag & T_CFLAG[i].dw) l->c_cflag |= T_CFLAG[i].lx;
    for (size_t i = 0; i < NTAB(T_LFLAG); i++) if (d->c_lflag & T_LFLAG[i].dw) l->c_lflag |= T_LFLAG[i].lx;
    switch (d->c_cflag & CSIZE) { case CS6: l->c_cflag |= 0x10; break; case CS7: l->c_cflag |= 0x20; break; case CS8: l->c_cflag |= 0x30; break; default: break; }
    l->c_cflag |= baud_to_lx(d->c_ospeed);
    for (int i = 0; i < 17; i++) if (T_CC[i] >= 0) l->c_cc[i] = d->c_cc[T_CC[i]];
}
static void term_from_lx(const lx_termios *l, struct termios *d) {
    d->c_iflag = d->c_oflag = d->c_cflag = d->c_lflag = 0;
    for (size_t i = 0; i < NTAB(T_IFLAG); i++) if (l->c_iflag & T_IFLAG[i].lx) d->c_iflag |= T_IFLAG[i].dw;
    for (size_t i = 0; i < NTAB(T_OFLAG); i++) if (l->c_oflag & T_OFLAG[i].lx) d->c_oflag |= T_OFLAG[i].dw;
    for (size_t i = 0; i < NTAB(T_CFLAG); i++) if (l->c_cflag & T_CFLAG[i].lx) d->c_cflag |= T_CFLAG[i].dw;
    for (size_t i = 0; i < NTAB(T_LFLAG); i++) if (l->c_lflag & T_LFLAG[i].lx) d->c_lflag |= T_LFLAG[i].dw;
    switch (l->c_cflag & 0x30) { case 0x10: d->c_cflag |= CS6; break; case 0x20: d->c_cflag |= CS7; break; case 0x30: d->c_cflag |= CS8; break; default: d->c_cflag |= CS5; break; }
    unsigned long sp = baud_to_dw(l->c_cflag & 0x100F);
    d->c_ispeed = d->c_ospeed = sp;
    for (int i = 0; i < 17; i++) if (T_CC[i] >= 0) d->c_cc[T_CC[i]] = l->c_cc[i];
}
int kl4_tcgetattr(int fd, lx_termios *l) {
    struct termios d;
    if (tcgetattr(fd, &d) < 0) return -1;
    term_to_lx(&d, l);
    return 0;
}
int kl4_tcsetattr(int fd, int act, const lx_termios *l) {
    if (act < 0 || act > 2) { errno = EINVAL; return -1; }                      // TCSANOW 0, TCSADRAIN 1, TCSAFLUSH 2: Darwin's numbers match
    struct termios d;
    if (tcgetattr(fd, &d) < 0) return -1;                                       // start from the real one: untranslated bits survive
    term_from_lx(l, &d);
    return tcsetattr(fd, act, &d);
}
// queue selectors: Linux TCIFLUSH 0, TCOFLUSH 1, TCIOFLUSH 2; Darwin 1, 2, 3. TCOOFF 0, TCOON 1, TCIOFF 2, TCION 3 against Darwin 1..4.
int kl4_tcflush(int fd, int q) { if (q < 0 || q > 2) { errno = EINVAL; return -1; } return tcflush(fd, q + 1); }
int kl4_tcflow(int fd, int a) { if (a < 0 || a > 3) { errno = EINVAL; return -1; } return tcflow(fd, a + 1); }
int kl4_tcdrain(int fd) { return tcdrain(fd); }
int kl4_tcsendbreak(int fd, int d) { return tcsendbreak(fd, d); }
int kl4_tcgetsid(int fd) { pid_t p = tcgetpgrp(fd); if (p < 0) return -1; pid_t s = getsid(p); return (int)s; }

// ======================================================================
// stdio_ext (<stdio_ext.h>) on the host FILE
// ======================================================================
size_t kl4___fbufsize(void *f) { FILE *h = kl_host_file(f); return (size_t)h->_bf._size; }
size_t kl4___fpending(void *f) { FILE *h = kl_host_file(f); return (h->_flags & __SWR) && h->_p && h->_bf._base ? (size_t)(h->_p - h->_bf._base) : 0; }
int kl4___flbf(void *f) { return (kl_host_file(f)->_flags & __SLBF) != 0; }
int kl4___freadable(void *f) { return (kl_host_file(f)->_flags & (__SRD | __SRW)) != 0; }
int kl4___fwritable(void *f) { return (kl_host_file(f)->_flags & (__SWR | __SRW)) != 0; }
int kl4___freading(void *f) { return (kl_host_file(f)->_flags & __SRD) != 0; }
int kl4___fwriting(void *f) { return (kl_host_file(f)->_flags & __SWR) != 0; }
size_t kl4___freadahead(void *f) { FILE *h = kl_host_file(f); return (h->_flags & __SRD) ? (size_t)(h->_r > 0 ? h->_r : 0) : 0; }
void kl4___fpurge(void *f) { fpurge(kl_host_file(f)); }
void kl4___fseterr(void *f) { kl_host_file(f)->_flags |= __SERR; }
int kl4___fsetlocking(void *f, int type) { (void)f; (void)type; return 1; }          // FSETLOCKING_INTERNAL: stdio always locks itself
void kl4__flushlbf(void) { fflush(NULL); }
// getline & the unlocked leftovers
ssize_t kl4_getline(char **lp, size_t *n, void *f) { return getline(lp, n, kl_host_file(f)); }
ssize_t kl4_getdelim(char **lp, size_t *n, int d, void *f) { return getdelim(lp, n, d, kl_host_file(f)); }
int kl4_fgetc_unlocked(void *f) { return getc_unlocked(kl_host_file(f)); }
int kl4_fputc_unlocked(int c, void *f) { return putc_unlocked(c, kl_host_file(f)); }
int kl4_fputs_unlocked(const char *s, void *f) { return fputs(s, kl_host_file(f)); }
char *kl4_fgets_unlocked(char *s, int n, void *f) { return fgets(s, n, kl_host_file(f)); }
size_t kl4_fwrite_unlocked(const void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, kl_host_file(f)); }
int kl4_ftrylockfile(void *f) { return ftrylockfile(kl_host_file(f)); }
// gets reads a whole line from stdin with no bound (bionic keeps the symbol for old binaries).
char *kl4_gets(char *s) {
    char *p = s; int c;
    while ((c = getchar_unlocked()) != EOF && c != '\n') *p++ = (char)c;
    if (c == EOF && p == s) return NULL;
    *p = 0;
    return s;
}
// malloc_info(options, FILE*): the XML skeleton of an allocator report (this allocator has no arenas to list).
int kl4_malloc_info(int opt, void *f) {
    if (opt != 0) { errno = EINVAL; return -1; }
    fputs("<malloc version=\"1\">\n</malloc>\n", kl_host_file(f));
    return 0;
}

// ======================================================================
// directories
// ======================================================================
typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; } lx_dirent;
int kl4_dirfd(void *d) { return dirfd((DIR *)d); }
long kl4_telldir(void *d) { return telldir((DIR *)d); }
void kl4_seekdir(void *d, long pos) { seekdir((DIR *)d, pos); }
// readdir_r returns an errno number; it fills the caller's entry from the bionic-layout readdir.
int kl4_readdir_r(void *d, lx_dirent *entry, lx_dirent **result) {
    static void *rd; if (!rd) rd = kl_shim_lookup("readdir");
    if (!rd) { *result = NULL; return 5; }
    lx_dirent *e = ((lx_dirent *(*)(void *))rd)(d);
    if (!e) { *result = NULL; return 0; }
    memcpy(entry, e, sizeof *entry);
    *result = entry;
    return 0;
}
int kl4_scandirat(int dfd, const char *path, lx_dirent ***list, int (*filter)(const lx_dirent *), int (*cmp)(const lx_dirent **, const lx_dirent **)) {
    static void *sd; if (!sd) sd = kl_shim_lookup("scandir");
    if (!sd) { errno = ENOSYS; return -1; }
    char full[1400];
    if (dfd != LX_AT_FDCWD && path && path[0] != '/') {
        char base[1024];
        if (fcntl(dfd, F_GETPATH, base) < 0) return -1;
        snprintf(full, sizeof full, "%s/%s", base, path);
        path = full;
    }
    return ((int (*)(const char *, lx_dirent ***, void *, void *))sd)(path, list, filter, cmp);
}

// ======================================================================
// files: statx, renameat2, mkstemps family, lchmod/lutimes/futimes, preadv2/pwritev2
// ======================================================================
typedef struct { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; } lx_statx_ts;
typedef struct {
    uint32_t stx_mask, stx_blksize; uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid; uint16_t stx_mode, pad1;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    lx_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t spare[14];
} lx_statx;
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH 0x1000
static void put_ts(lx_statx_ts *o, struct timespec t) { o->tv_sec = t.tv_sec; o->tv_nsec = (uint32_t)t.tv_nsec; o->pad = 0; }
int kl4_statx(int dfd, const char *path, int flags, unsigned mask, lx_statx *out) {
    (void)mask;
    struct stat st;
    int r;
    if ((flags & LX_AT_EMPTY_PATH) && (!path || !*path)) r = fstat(dfd, &st);
    else {
        if (!path) { errno = EFAULT; return -1; }
        GUEST_PATH(path);
        r = fstatat(at_fd(dfd), _p, &st, (flags & LX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0);
    }
    if (r < 0) return -1;
    memset(out, 0, sizeof *out);
    out->stx_mask = 0x7FFu | 0x800u;                                     // STATX_BASIC_STATS | STATX_BTIME
    out->stx_blksize = (uint32_t)st.st_blksize; out->stx_nlink = st.st_nlink;
    out->stx_uid = st.st_uid; out->stx_gid = st.st_gid; out->stx_mode = st.st_mode;
    out->stx_ino = st.st_ino; out->stx_size = (uint64_t)st.st_size; out->stx_blocks = (uint64_t)st.st_blocks;
    put_ts(&out->stx_atime, st.st_atimespec); put_ts(&out->stx_btime, st.st_birthtimespec);
    put_ts(&out->stx_ctime, st.st_ctimespec); put_ts(&out->stx_mtime, st.st_mtimespec);
    out->stx_rdev_major = (uint32_t)major(st.st_rdev); out->stx_rdev_minor = (uint32_t)minor(st.st_rdev);
    out->stx_dev_major = (uint32_t)major(st.st_dev); out->stx_dev_minor = (uint32_t)minor(st.st_dev);
    return 0;
}
// RENAME_NOREPLACE 1 / RENAME_EXCHANGE 2 / RENAME_WHITEOUT 4 against Darwin's RENAME_EXCL / RENAME_SWAP.
int kl4_renameat2(int od, const char *o, int nd, const char *n, unsigned flags) {
    char k1[1024], k2[1024];
    const char *a = kl_guest_path(o, k1, sizeof k1), *b = kl_guest_path(n, k2, sizeof k2);
    if (flags & ~3u) { errno = EINVAL; return -1; }
    if ((flags & 3u) == 3u) { errno = EINVAL; return -1; }
    if (!flags) return renameat(at_fd(od), a, at_fd(nd), b);
    return renameatx_np(at_fd(od), a, at_fd(nd), b, (flags & 1u) ? RENAME_EXCL : RENAME_SWAP);
}
static void fill_x(char *x, size_t n) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint8_t r[16]; arc4random_buf(r, sizeof r);
    for (size_t i = 0; i < n; i++) x[i] = cs[r[i % sizeof r] % 62];
}
// Shared by the mk*temp family: tries up to 100 names; `mode` 0 file (open), 1 directory, 2 name only.
static int mktemp_core(char *tmpl, int suffixlen, int oflags, int mode) {
    size_t len = tmpl ? strlen(tmpl) : 0;
    if (suffixlen < 0 || len < 6 + (size_t)suffixlen) { errno = EINVAL; return -1; }
    char *x = tmpl + len - (size_t)suffixlen - 6;
    for (int i = 0; i < 6; i++) if (x[i] != 'X') { errno = EINVAL; return -1; }
    for (int attempt = 0; attempt < 100; attempt++) {
        fill_x(x, 6);
        GUEST_PATH(tmpl);
        if (mode == 0) {
            int fd = open(_p, O_RDWR | O_CREAT | O_EXCL | kl_open_flags(oflags), 0600);
            if (fd >= 0) return fd;
            if (errno != EEXIST) return -1;
        } else if (mode == 1) {
            if (mkdir(_p, 0700) == 0) return 0;
            if (errno != EEXIST) return -1;
        } else {
            struct stat st;
            if (lstat(_p, &st) < 0) { if (errno == ENOENT) return 0; return -1; }
        }
    }
    errno = EEXIST;
    return -1;
}
int kl4_mkstemps(char *t, int suf) { return mktemp_core(t, suf, 0, 0); }
int kl4_mkostemp(char *t, int fl) { return mktemp_core(t, 0, fl, 0); }
int kl4_mkostemps(char *t, int suf, int fl) { return mktemp_core(t, suf, fl, 0); }
char *kl4_mkdtemp(char *t) { return mktemp_core(t, 0, 0, 1) == 0 ? t : NULL; }
char *kl4_mktemp(char *t) { if (mktemp_core(t, 0, 0, 2) != 0) { if (t) *t = 0; } return t; }
static const char *tmp_dir(void) { const char *d = getenv("TMPDIR"); return d && *d ? d : "/data/local/tmp"; }
char *kl4_tmpnam(char *s) {
    static char buf[PATH_MAX];
    char *o = s ? s : buf;
    snprintf(o, PATH_MAX, "%s/tmp.XXXXXXXXXX", tmp_dir());
    size_t len = strlen(o);
    for (int a = 0; a < 100; a++) {
        fill_x(o + len - 10, 10);
        GUEST_PATH(o);
        struct stat st;
        if (lstat(_p, &st) < 0 && errno == ENOENT) return o;
    }
    return NULL;
}
char *kl4_tempnam(const char *dir, const char *pfx) {
    const char *d = getenv("TMPDIR"); if (!d || !*d) d = dir; if (!d || !*d) d = "/data/local/tmp";
    char *o = malloc(strlen(d) + (pfx ? strlen(pfx) : 0) + 16);
    if (!o) return NULL;
    sprintf(o, "%s/%s", d, pfx ? pfx : "");
    size_t base = strlen(o);
    strcat(o, "XXXXXX");
    for (int a = 0; a < 100; a++) {
        fill_x(o + base, 6);
        GUEST_PATH(o);
        struct stat st;
        if (lstat(_p, &st) < 0 && errno == ENOENT) return o;
    }
    free(o);
    errno = EEXIST;
    return NULL;
}
int kl4_lchmod(const char *path, unsigned mode) { GUEST_PATH(path); return lchmod(_p, (mode_t)mode); }
int kl4_lutimes(const char *path, const struct timeval *tv) { GUEST_PATH(path); return lutimes(_p, tv); }
int kl4_futimes(int fd, const struct timeval *tv) { return futimes(fd, tv); }
int kl4_futimesat(int dfd, const char *path, const struct timeval *tv) {
    if (!path) return futimes(dfd, tv);
    GUEST_PATH(path);
    struct timespec ts[2];
    if (tv) { for (int i = 0; i < 2; i++) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = tv[i].tv_usec * 1000L; } }
    return utimensat(at_fd(dfd), _p, tv ? ts : NULL, 0);
}
// preadv2 / pwritev2: offset -1 means "current position"; RWF_HIPRI is a hint, RWF_DSYNC / RWF_SYNC
// flush after a write, the rest (NOWAIT, APPEND) have no Darwin counterpart.
ssize_t kl4_preadv2(int fd, const struct iovec *iov, int cnt, off_t off, int flags) {
    if (flags & ~1) { errno = EOPNOTSUPP; return -1; }
    return off == -1 ? readv(fd, iov, cnt) : preadv(fd, iov, cnt, off);
}
ssize_t kl4_pwritev2(int fd, const struct iovec *iov, int cnt, off_t off, int flags) {
    if (flags & ~7) { errno = EOPNOTSUPP; return -1; }
    ssize_t r = off == -1 ? writev(fd, iov, cnt) : pwritev(fd, iov, cnt, off);
    if (r >= 0 && (flags & 6)) fsync(fd);
    return r;
}
// pselect(nfds, r, w, e, const timespec*, const sigset_t* (bionic: 8 bytes; Darwin: 4)).
int kl4_pselect(int n, fd_set *r, fd_set *w, fd_set *e, const struct timespec *ts, const uint64_t *mask) {
    sigset_t m;
    if (mask) m = (sigset_t)(*mask & 0xFFFFFFFFu);
    return pselect(n, r, w, e, ts, mask ? &m : NULL);
}
// epoll_pwait(epfd, events, max, timeout_ms, sigmask): epoll_wait with the mask applied around it.
int kl4_epoll_pwait(int epfd, void *events, int max, int ms, const uint64_t *mask) {
    static void *ew; if (!ew) ew = kl_shim_lookup("epoll_wait");
    if (!ew) { errno = ENOSYS; return -1; }
    if (!mask) return ((int (*)(int, void *, int, int))ew)(epfd, events, max, ms);
    sigset_t m = (sigset_t)(*mask & 0xFFFFFFFFu), old;
    pthread_sigmask(SIG_SETMASK, &m, &old);
    int r = ((int (*)(int, void *, int, int))ew)(epfd, events, max, ms);
    int er = errno;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = er;
    return r;
}

// ======================================================================
// sendmmsg / recvmmsg over the sendmsg / recvmsg the shim already translates (msghdr, sockaddr,
// cmsg and flag numbering all differ from Darwin's). struct mmsghdr is { msghdr (56 bytes); u32 msg_len }.
// ======================================================================
#define LX_MMSG_STRIDE 64
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_WAITFORONE 0x10000
int kl4_sendmmsg(int fd, void *vec, unsigned vlen, int flags) {
    static void *sm; if (!sm) sm = kl_shim_lookup("sendmsg");
    if (!sm) { errno = ENOSYS; return -1; }
    unsigned i = 0;
    for (; i < vlen; i++) {
        uint8_t *m = (uint8_t *)vec + (size_t)i * LX_MMSG_STRIDE;
        ssize_t r = ((ssize_t (*)(int, const void *, int))sm)(fd, m, flags);
        if (r < 0) { if (i == 0) return -1; break; }       // a later failure is reported by the next call
        uint32_t len = (uint32_t)r; memcpy(m + 56, &len, 4);
    }
    return (int)i;
}
int kl4_recvmmsg(int fd, void *vec, unsigned vlen, int flags, const struct timespec *timeout) {
    static void *rm; if (!rm) rm = kl_shim_lookup("recvmsg");
    if (!rm) { errno = ENOSYS; return -1; }
    struct timespec end = {0, 0};
    if (timeout) { clock_gettime(CLOCK_MONOTONIC, &end); end.tv_sec += timeout->tv_sec; end.tv_nsec += timeout->tv_nsec; if (end.tv_nsec >= 1000000000L) { end.tv_nsec -= 1000000000L; end.tv_sec++; } }
    unsigned i = 0;
    int f = flags & ~LX_MSG_WAITFORONE;
    for (; i < vlen; i++) {
        uint8_t *m = (uint8_t *)vec + (size_t)i * LX_MMSG_STRIDE;
        ssize_t r = ((ssize_t (*)(int, void *, int))rm)(fd, m, f);
        if (r < 0) { if (i == 0) return -1; break; }
        uint32_t len = (uint32_t)r; memcpy(m + 56, &len, 4);
        if (flags & LX_MSG_WAITFORONE) f |= LX_MSG_DONTWAIT;
        if (timeout) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) { i++; break; } }
    }
    return (int)i;
}

// ======================================================================
// pidfd. Linux 5.10 (the Quest's kernel) has them, but an app can only see its own process
// and the siblings of its uid; on visionOS that is just ourselves. pidfd_open(own pid) returns
// a real descriptor (the read end of a pipe whose write end is kept open, so it never becomes
// readable while we live); any other pid is ESRCH, which is what an unprivileged process sees.
// ======================================================================
int kl4_pidfd_open(int pid, unsigned flags) {
    if (flags & ~(unsigned)LX_O_NONBLOCK) { errno = EINVAL; return -1; }
    if (pid <= 0) { errno = EINVAL; return -1; }
    if (pid != (int)getpid()) { errno = ESRCH; return -1; }
    int p[2];
    if (pipe(p) < 0) return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);
    if (flags & LX_O_NONBLOCK) fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    if (!kreg_add(p[0], KF_PIDFD, (void *)(intptr_t)p[1])) { close(p[0]); close(p[1]); errno = EMFILE; return -1; }
    return p[0];
}
int kl4_pidfd_send_signal(int pidfd, int sig, const void *info, unsigned flags) {
    if (flags || info) { errno = EINVAL; return -1; }          // siginfo injection and the scope flags are not offered
    if (!kreg_find(pidfd, KF_PIDFD)) { errno = EBADF; return -1; }
    return kill(getpid(), sig);
}
int kl4_pidfd_getfd(int pidfd, int targetfd, unsigned flags) {
    if (flags) { errno = EINVAL; return -1; }
    if (!kreg_find(pidfd, KF_PIDFD)) { errno = EBADF; return -1; }
    int fd = dup(targetfd);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);               // pidfd_getfd returns the copy close-on-exec
    return fd;
}

// ======================================================================
// signals the Darwin libc lacks or spells differently
// ======================================================================
// sigqueue: Darwin has no queued signals with a payload, so the signal is sent and the value is dropped.
int kl4_sigqueue(int pid, int sig, uint64_t value) { (void)value; return kill(pid, sig); }
static void fill_info(void *info, int sig) {
    if (!info) return;
    memset(info, 0, 128);                                       // siginfo_t: { signo, errno, code, pad, pid, uid, ... }
    int32_t s = sig, pid = (int32_t)getpid(), uid = (int32_t)getuid();
    memcpy(info, &s, 4); memcpy((uint8_t *)info + 16, &pid, 4); memcpy((uint8_t *)info + 20, &uid, 4);
}
// Waits for a pending (blocked) signal of the set. Polled every millisecond: there is no sigtimedwait.
static int sigwait_poll(const uint64_t *set, void *info, const struct timespec *ts) {
    sigset_t want = (sigset_t)(*set & 0xFFFFFFFFu);
    struct timespec end = {0, 0};
    if (ts) {
        if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L || ts->tv_sec < 0) { errno = EINVAL; return -1; }
        clock_gettime(CLOCK_MONOTONIC, &end); end.tv_sec += ts->tv_sec; end.tv_nsec += ts->tv_nsec;
        if (end.tv_nsec >= 1000000000L) { end.tv_nsec -= 1000000000L; end.tv_sec++; }
    }
    for (;;) {
        sigset_t pend;
        sigpending(&pend);
        sigset_t hit = pend & want;
        if (hit) {
            int sig = __builtin_ctz((unsigned)hit) + 1, got = 0;
            sigset_t one = (sigset_t)1 << (sig - 1);
            if (sigwait(&one, &got) == 0) { fill_info(info, got); return got; }
        }
        if (ts) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) { errno = EAGAIN; return -1; }
        }
        struct timespec nap = {0, 1000000};
        if (nanosleep(&nap, NULL) < 0 && errno == EINTR) return -1;
    }
}
int kl4_sigtimedwait(const uint64_t *set, void *info, const struct timespec *ts) { return sigwait_poll(set, info, ts); }
int kl4_sigwaitinfo(const uint64_t *set, void *info) { return sigwait_poll(set, info, NULL); }
// BSD / XSI mask calls over sigprocmask (the Darwin copies are deprecated or absent on some SDKs).
int kl4_sigblock(int mask) { sigset_t n = (sigset_t)mask, o = 0; sigprocmask(SIG_BLOCK, &n, &o); return (int)o; }
int kl4_sigsetmask(int mask) { sigset_t n = (sigset_t)mask, o = 0; sigprocmask(SIG_SETMASK, &n, &o); return (int)o; }
int kl4_sigpause(int mask) { sigset_t n = (sigset_t)mask; return sigsuspend(&n); }
int kl4_sighold(int sig) { sigset_t n = (sigset_t)1 << (sig - 1); return sigprocmask(SIG_BLOCK, &n, NULL); }
int kl4_sigrelse(int sig) { sigset_t n = (sigset_t)1 << (sig - 1); return sigprocmask(SIG_UNBLOCK, &n, NULL); }
int kl4_sigignore(int sig) { return signal(sig, SIG_IGN) == SIG_ERR ? -1 : 0; }
int kl4_siginterrupt(int sig, int flag) {
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) < 0) return -1;
    if (flag) sa.sa_flags &= ~SA_RESTART; else sa.sa_flags |= SA_RESTART;
    return sigaction(sig, &sa, NULL);
}
// sigset(sig, disp): XSI; SIG_HOLD is 2 in bionic's <signal.h>.
void (*kl4_sigset(int sig, void (*disp)(int)))(int) {
    sigset_t cur, one = (sigset_t)1 << (sig - 1);
    sigprocmask(SIG_BLOCK, NULL, &cur);
    int was_blocked = (cur & one) != 0;
    if (disp == (void (*)(int))2) { sigprocmask(SIG_BLOCK, &one, NULL); return was_blocked ? (void (*)(int))2 : SIG_DFL; }
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = disp; sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, &old) < 0) return SIG_ERR;
    sigprocmask(SIG_UNBLOCK, &one, NULL);
    return was_blocked ? (void (*)(int))2 : old.sa_handler;
}
// pthread_sigqueue(thread, sig, value): pthread_kill without the payload.
int kl4_pthread_sigqueue(unsigned long th, int sig, uint64_t value) {
    (void)value;
    static void *pk; if (!pk) pk = kl_shim_lookup("pthread_kill");
    return pk ? ((int (*)(unsigned long, int))pk)(th, sig) : 38;
}
// Affinity of a thread: the host schedules, so the answer is "every CPU", as sched_getaffinity says.
int kl4_pthread_getaffinity_np(unsigned long th, size_t sz, void *set) {
    (void)th;
    static void *ga; if (!ga) ga = kl_shim_lookup("sched_getaffinity");
    return ga && ((int (*)(int, size_t, void *))ga)(0, sz, set) == 0 ? 0 : 22;
}
int kl4_pthread_setaffinity_np(unsigned long th, size_t sz, const void *set) {
    (void)th; (void)sz; (void)set;                               // accepted and ignored, like sched_setaffinity
    return 0;
}
// *_monotonic_np: the absolute deadline is on CLOCK_MONOTONIC (1), which the clock* forms take.
int kl4_sem_timedwait_monotonic_np(void *s, const struct timespec *ts) {
    static void *f; if (!f) f = kl_shim_lookup("sem_clockwait");
    return f ? ((int (*)(void *, int, const struct timespec *))f)(s, 1, ts) : -1;
}
int kl4_pthread_rwlock_timedrdlock_monotonic_np(void *rw, const struct timespec *ts) {
    static void *f; if (!f) f = kl_shim_lookup("pthread_rwlock_clockrdlock");
    return f ? ((int (*)(void *, int, const struct timespec *))f)(rw, 1, ts) : 38;
}
int kl4_pthread_rwlock_timedwrlock_monotonic_np(void *rw, const struct timespec *ts) {
    static void *f; if (!f) f = kl_shim_lookup("pthread_rwlock_clockwrlock");
    return f ? ((int (*)(void *, int, const struct timespec *))f)(rw, 1, ts) : 38;
}

// ======================================================================
// what an app cannot have: pseudo-terminals, exec, SysV IPC, clock/capability syscalls
// ======================================================================
static void warn_once4(const char *what) {
    static const char *seen[16]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 16) seen[n++] = what;
    fprintf(stderr, "  [klepton] %s() refused — an app has no such facility\n", what);
}
static long kl4_exec_refused(void) { warn_once4("exec"); errno = ENOSYS; return -1; }       // execle/execlp/execvp/execvpe/fexecve
// /dev/ptmx does not exist for an app, which is what these report.
static long kl4_ptsname_r(void) { return 25; }                                              // ENOTTY (the return value is the errno)
// adjtimex/clock_adjtime: reading (modes == 0) is allowed to anyone and reports a clean state.
int kl4_adjtimex(void *tx) {
    uint32_t modes; memcpy(&modes, tx, 4);
    if (modes) { errno = EPERM; return -1; }
    memset(tx, 0, 208);
    return 0;                                                                               // TIME_OK
}
int kl4_clock_adjtime(int clk, void *tx) { (void)clk; return kl4_adjtimex(tx); }
// capget: an app holds no capabilities. header { u32 version; int pid }, data[2] { effective, permitted, inheritable }.
int kl4_capget(void *hdr, void *data) {
    uint32_t v; memcpy(&v, hdr, 4);
    if (v != 0x20080522u && v != 0x20071026u && v != 0x19980330u) { uint32_t cur = 0x20080522u; memcpy(hdr, &cur, 4); errno = EINVAL; return -1; }
    if (data) memset(data, 0, (v == 0x19980330u ? 1 : 2) * 12);
    return 0;
}
// sched_getattr(pid, attr, size, flags): SCHED_OTHER, nice 0. struct sched_attr is 48 bytes.
int kl4_sched_getattr(int pid, void *attr, unsigned size, unsigned flags) {
    (void)pid;
    if (flags || !attr || size < 48) { errno = EINVAL; return -1; }
    memset(attr, 0, 48);
    uint32_t sz = 48; memcpy(attr, &sz, 4);
    return 0;
}
int kl4_sched_rr_get_interval(int pid, struct timespec *ts) { (void)pid; ts->tv_sec = 0; ts->tv_nsec = 0; return 0; }

// ftok: key from the file's inode and device, as the usual libc does.
int kl4_ftok(const char *path, int proj) {
    GUEST_PATH(path);
    struct stat st;
    if (stat(_p, &st) < 0) return -1;
    return (int)(((unsigned)(proj & 0xFF) << 24) | (((unsigned)st.st_dev & 0xFF) << 16) | ((unsigned)st.st_ino & 0xFFFF));
}

// ======================================================================
// the table
// ======================================================================
int klx2_vswscanf(const wchar_t *s, const wchar_t *fmt, void *va);
int klx2_vfwscanf(void *f, const wchar_t *fmt, void *va);
int klx2_vwscanf(const wchar_t *fmt, void *va);
extern void klv_error(void), klv_error_at_line(void), klv_wscanf(void), klv_fwscanf(void), klv_swscanf(void);
extern unsigned int kl_error_message_count; extern int kl_error_one_per_line; extern void (*kl_error_print_progname)(void);
#define L4(n) { #n, (void *)kl4_##n }
static const kl4_entry g_extra4[] = {
    L4(timerfd_create), L4(timerfd_settime), L4(timerfd_gettime), L4(signalfd),
    L4(inotify_init), L4(inotify_init1), L4(inotify_add_watch), L4(inotify_rm_watch),
    L4(tcgetattr), L4(tcsetattr), L4(tcflush), L4(tcflow), L4(tcdrain), L4(tcsendbreak), L4(tcgetsid),
    L4(__fbufsize), L4(__fpending), L4(__flbf), L4(__freadable), L4(__fwritable), L4(__freading), L4(__fwriting),
    L4(__freadahead), L4(__fpurge), L4(__fseterr), L4(__fsetlocking), L4(_flushlbf),
    L4(getline), L4(getdelim), L4(fgetc_unlocked), L4(fputc_unlocked), L4(fputs_unlocked), L4(fgets_unlocked),
    L4(fwrite_unlocked), L4(ftrylockfile), L4(gets), L4(malloc_info),
    L4(dirfd), L4(telldir), L4(seekdir), L4(readdir_r), L4(scandirat),
    L4(statx), L4(renameat2), L4(mkstemps), L4(mkostemp), L4(mkostemps), L4(mkdtemp), L4(mktemp), L4(tmpnam),
    L4(tempnam), L4(lchmod), L4(lutimes), L4(futimes), L4(futimesat), L4(preadv2), L4(pwritev2), L4(pselect),
    L4(epoll_pwait), L4(sendmmsg), L4(recvmmsg), L4(pidfd_open), L4(pidfd_send_signal), L4(pidfd_getfd),
    L4(sigqueue), L4(sigtimedwait), L4(sigwaitinfo), L4(sigblock), L4(sigsetmask), L4(sigpause), L4(sighold),
    L4(sigrelse), L4(sigignore), L4(siginterrupt), L4(sigset), L4(pthread_sigqueue),
    L4(pthread_getaffinity_np), L4(pthread_setaffinity_np), L4(sem_timedwait_monotonic_np),
    L4(pthread_rwlock_timedrdlock_monotonic_np), L4(pthread_rwlock_timedwrlock_monotonic_np),
    L4(adjtimex), L4(clock_adjtime), L4(capget), L4(sched_getattr), L4(sched_rr_get_interval), L4(ftok),
    // exec: refused like execv/execl; pty: there is no /dev/ptmx; SysV IPC: seccomp denies it to apps
    E4("execle", kl4_exec_refused), E4("execlp", kl4_exec_refused), E4("execvp", kl4_exec_refused),
    E4("execvpe", kl4_exec_refused), E4("fexecve", kl4_exec_refused),
    E4("openpty", kl4_enoent), E4("forkpty", kl4_enoent), E4("posix_openpt", kl4_enoent), E4("getpt", kl4_enoent),
    E4("grantpt", kl4_enotty), E4("unlockpt", kl4_enotty), E4("login_tty", kl4_enotty), E4("ptsname", kl4_enotty),
    E4("ptsname_r", kl4_ptsname_r),
    E4("msgctl", kl4_enosys), E4("msgget", kl4_enosys), E4("msgrcv", kl4_enosys), E4("msgsnd", kl4_enosys),
    E4("semtimedop", kl4_enosys), E4("shmat", kl4_enosys), E4("shmctl", kl4_enosys), E4("shmdt", kl4_enosys),
    E4("shmget", kl4_enosys), E4("mseal", kl4_enosys),
    E4("init_module", kl4_eperm), E4("delete_module", kl4_eperm), E4("klogctl", kl4_eperm), E4("quotactl", kl4_eperm),
    E4("settimeofday", kl4_eperm), E4("capset", kl4_eperm), E4("process_madvise", kl4_eperm), E4("sched_setattr", kl4_eperm),
    E4("brk", kl4_enomem),
    // variadic entry points (kl_va_thunks.S)
    E4("error", klv_error), E4("error_at_line", klv_error_at_line), E4("wscanf", klv_wscanf),
    E4("fwscanf", klv_fwscanf), E4("swscanf", klv_swscanf),
    E4("vswscanf", klx2_vswscanf), E4("vfwscanf", klx2_vfwscanf), E4("vwscanf", klx2_vwscanf),
    E4("error_message_count", &kl_error_message_count), E4("error_one_per_line", &kl_error_one_per_line),
    E4("error_print_progname", &kl_error_print_progname),
};

void *kl_bionic_extra4_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_extra4 / sizeof g_extra4[0]; i++)
        if (g_extra4[i].name[0] == name[0] && !strcmp(g_extra4[i].name, name)) return g_extra4[i].fn;
    return NULL;
}
