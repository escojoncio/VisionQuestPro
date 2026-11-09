// Host test (Linux or macOS) of the Unreal log scan behind the "[ue-log]" boot-log line (runtime/kl_uelog.h).
//   make uelog        (or tools/ci/host-tests.sh)
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "kl_uelog.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static char root[256];

static void mk(const char *rel) {   // mkdir -p root/rel
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *c = p + strlen(root) + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, size_t bytes, time_t mtime) {   // a file of `bytes` with that mtime, its folder made first
    char p[512], d[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", rel);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mk(d); }
    FILE *f = fopen(p, "wb");
    for (size_t i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
    struct utimbuf t = { mtime, mtime };
    utime(p, &t);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/t_uelog_XXXXXX");
    if (!mkdtemp(root)) return 2;
    const time_t now = time(NULL), since = now - 100;
    kl_uelog_res r;
    char line[600];

    // Nothing written yet: "none", with the folder and how much was looked at.
    kl_uelog_scan(root, since, &r);
    kl_uelog_line(line, sizeof line, root, &r);
    CHECK(r.logs == 0 && !strncmp(line, "[ue-log] none files=", 20) && strstr(line, "scanned=0"), "an empty folder says none");

    // Not logs / not this session's: a stale log, a -backup- copy, a Config ini, a hidden file, a log under a skipped folder.
    put("UnrealGame/P/P/Saved/Logs/Old.log", 10, now - 5000);
    put("UnrealGame/P/P/Saved/Logs/P-backup-2026.11.09-10.00.00.log", 10, now);
    put("UnrealGame/P/P/Saved/Config/Engine.log", 10, now);
    put("UnrealGame/P/P/Saved/Crashes/c.log", 10, now);
    put("UnrealGame/P/P/Saved/Logs/.hidden.log", 10, now);
    put("UnrealGame/P/P/Saved/Logs/notes.txt", 10, now);
    put("Engine/Saved/Logs/E.log", 10, now);
    put("Android/obb/pkg/x.log", 10, now);
    kl_uelog_scan(root, since, &r);
    kl_uelog_line(line, sizeof line, root, &r);
    CHECK(r.logs == 0 && strstr(line, "none") && r.scanned > 0, "stale, backup, Config, Crashes, hidden, non-log and skipped-folder files do not count");

    // The real one, and an older sibling: the newest is reported, depth = levels below <files>.
    put("UnrealGame/P/P/Saved/Logs/P.log", 123, now - 10);
    put("UnrealGame/P/P/Saved/Logs/Q.LOG", 7, now - 60);
    kl_uelog_scan(root, since, &r);
    kl_uelog_line(line, sizeof line, root, &r);
    CHECK(r.logs == 2 && r.newest_bytes == 123 && r.newest_depth == 6, "two logs, the newest is P.log at depth 6");
    CHECK(!strcmp(r.newest, "UnrealGame/P/P/Saved/Logs/P.log"), "the path is relative to <files>");
    CHECK(!strncmp(line, "[ue-log] found n=2 newest=UnrealGame/P/P/Saved/Logs/P.log bytes=123 depth=6 scanned=", 82), "the found line");

    // A log under Android/data/<pkg>/files is deeper than the app's scan (6) and still shows, with its depth.
    put("Android/data/pkg/files/UnrealGame/P/P/Saved/Logs/D.log", 5, now - 1);
    kl_uelog_scan(root, since, &r);
    CHECK(r.logs == 3 && r.newest_depth == 10, "a deep log is found and its depth reported");

    // Missing folder: none, no crash.
    kl_uelog_scan("/tmp/t_uelog_does_not_exist", since, &r);
    CHECK(r.logs == 0 && r.scanned == 0, "a missing folder is none");

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    if (fails) { printf("t_uelog: %d FAILED\n", fails); return 1; }
    printf("t_uelog: ok\n");
    return 0;
}
