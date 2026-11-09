// Unreal's own log files under the guest's files folder (<files>/<UE4Game|UnrealGame>/<Project>/<Project>/Saved/Logs/*.log, every
// external-storage path of the guest maps under <files>). The heartbeat prints ONE line about them to the boot log ("[ue-log] none"
// when the game wrote none), so a shared Registros bundle says whether the app's game-*.log copies (app/Sources/VQLog.swift
// newestGameLogs) had anything to find. Same filters as that scan: *.log, not -backup-, not older than `since`, skipping the folders it
// skips; deeper (KL_UELOG_MAX_DEPTH) so a log the app's depth limit would miss (depth > 6) still shows, with its depth. Pure libc: tests/t_uelog.c
// runs the shipping code on Linux.
#ifndef KL_UELOG_H
#define KL_UELOG_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define KL_UELOG_MAX_DEPTH  12
#define KL_UELOG_MAX_VISITS 40000

typedef struct {
    int       logs;           // qualifying *.log files found
    int       scanned;        // directory entries looked at
    int       newest_depth;   // level of the newest one (<files>/a/b.log = 2)
    long long newest_bytes;
    time_t    newest_mtime;
    char      newest[512];    // its path relative to <files>
} kl_uelog_res;

static inline int kl_uelog_skip_dir(const char *name, const char *parent) {
    static const char *const skip[] = { "Cache", "tree", "obb", "assets", "Engine", "Content" };
    for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++)
        if (!strcmp(name, skip[i])) return 1;
    return (!strcmp(name, "Config") || !strcmp(name, "Crashes")) && !strcmp(parent, "Saved");
}

static inline void kl_uelog_walk(const char *abs, const char *rel, const char *parent, int level, time_t since,
                                 kl_uelog_res *r) {
    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && r->scanned < KL_UELOG_MAX_VISITS) {
        if (e->d_name[0] == '.') continue;
        r->scanned++;
        char a2[1024], r2[512];
        if (snprintf(a2, sizeof a2, "%s/%s", abs, e->d_name) >= (int)sizeof a2) continue;
        if (snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof r2) continue;
        struct stat st;
        if (lstat(a2, &st) != 0) continue;   // lstat: the short-path alias (.f) and other links are not followed
        if (S_ISDIR(st.st_mode)) {
            if (level < KL_UELOG_MAX_DEPTH && !kl_uelog_skip_dir(e->d_name, parent))
                kl_uelog_walk(a2, r2, e->d_name, level + 1, since, r);
            continue;
        }
        const char *dot = strrchr(e->d_name, '.');
        if (!S_ISREG(st.st_mode) || !dot || strcasecmp(dot, ".log") || strstr(e->d_name, "-backup-") || st.st_mtime < since)
            continue;
        r->logs++;
        if (r->logs == 1 || st.st_mtime > r->newest_mtime) {
            r->newest_mtime = st.st_mtime, r->newest_bytes = (long long)st.st_size, r->newest_depth = level;
            snprintf(r->newest, sizeof r->newest, "%s", r2);
        }
    }
    closedir(d);
}

static inline void kl_uelog_scan(const char *files, time_t since, kl_uelog_res *r) {
    memset(r, 0, sizeof *r);
    kl_uelog_walk(files, "", "", 1, since, r);
}

// "[ue-log] none files=<root> scanned=<n>" or "[ue-log] found n=<k> newest=<rel> bytes=<b> depth=<d> scanned=<n>".
static inline void kl_uelog_line(char *out, size_t cap, const char *files, const kl_uelog_res *r) {
    if (!r->logs)
        snprintf(out, cap, "[ue-log] none files=%s scanned=%d", files, r->scanned);
    else
        snprintf(out, cap, "[ue-log] found n=%d newest=%s bytes=%lld depth=%d scanned=%d", r->logs, r->newest,
                 r->newest_bytes, r->newest_depth, r->scanned);
}

#endif
