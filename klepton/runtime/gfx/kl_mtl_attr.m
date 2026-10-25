// Live Metal objects by creation site (see kl_mtl_attr.h). Written to compile with and without ARC:
// the xros archive is built without -fobjc-arc, the host drivers with it.
#include "kl_mtl_attr.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <os/lock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_feature(objc_arc)
#define KL_KEEP(x) (x)
#define KL_DROP(x) ((void)(x))
#else
#define KL_KEEP(x) [(x) retain]
#define KL_DROP(x) [(x) release]
#endif

size_t kl_vulkan_mem_stats(char *out, size_t cap);   // kl_vulkan.c: VPKEngine's counters (0 without VPVulkan)

enum { KIND_RES, KIND_COUNT, KIND_OBJ };
enum { MAX_SITES = 160, MAX_DEVS = 4, BIG_MAX = 200 };
#define BIG_ALLOC (32ull << 20)

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;   // everything below
static char *g_tag[MAX_SITES];                         // copies: Swift hands a temporary C string
static int g_kind[MAX_SITES];
static uint64_t g_delta[MAX_SITES];                    // KIND_OBJ: allocation growth around the creations
static NSHashTable *g_live[MAX_SITES];                 // weak, by pointer
static int g_n;
static id<MTLDevice> g_dev[MAX_DEVS];                  // distinct devices seen
static int g_ndev;
static int g_big;

static NSHashTable *new_table(void) {
    return KL_KEEP([NSHashTable hashTableWithOptions:NSPointerFunctionsWeakMemory |
                                                     NSPointerFunctionsObjectPointerPersonality]);
}

// Index of `tag`'s site, created on first use; -1 when the table is full. g_lock held.
static int site_locked(const char *tag, int kind) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_tag[i], tag)) return i;
    if (g_n == MAX_SITES) return -1;
    g_tag[g_n] = strdup(tag);
    g_kind[g_n] = kind;
    g_live[g_n] = new_table();
    return g_n++;
}

static void note_device_locked(id dev) {
    if (!dev || g_ndev == MAX_DEVS) return;
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i] == dev) return;
    g_dev[g_ndev++] = KL_KEEP(dev);
}

uint64_t kl_mtl_attr_probe(id device) {
    return device ? (uint64_t)[(id<MTLDevice>)device currentAllocatedSize] : 0;
}

static void big_warn(const char *tag, id<MTLResource> r, uint64_t bytes) {
    static const char *const stor[] = {"shared", "managed", "private", "memoryless"};
    unsigned st = (unsigned)r.storageMode;
    char lbl[64];
    snprintf(lbl, sizeof lbl, "%s", r.label ? r.label.UTF8String : "");
    for (char *c = lbl; *c; c++)
        if (*c == ' ') *c = '_';
    char b[320];
    if ([r respondsToSelector:@selector(pixelFormat)]) {
        id<MTLTexture> t = (id<MTLTexture>)r;
        snprintf(b, sizeof b, "type=tex mib=%.1f dims=%lux%lux%lux%lu samples=%lu levels=%lu pf=%lu storage=%s usage=0x%lx label=%s",
                 (double)bytes / 1048576.0, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                 (unsigned long)t.arrayLength, (unsigned long)t.sampleCount, (unsigned long)t.mipmapLevelCount,
                 (unsigned long)t.pixelFormat, st < 4 ? stor[st] : "?", (unsigned long)t.usage, lbl);
    } else {
        snprintf(b, sizeof b, "type=buf mib=%.1f length=%lu storage=%s label=%s", (double)bytes / 1048576.0,
                 (unsigned long)((id<MTLBuffer>)r).length, st < 4 ? stor[st] : "?", lbl);
    }
    os_unfair_lock_lock(&g_lock);
    int n = ++g_big;
    os_unfair_lock_unlock(&g_lock);
    if (n <= BIG_MAX) fprintf(stderr, "  [mtl] bigalloc tag=%s %s\n", tag, b);
    else if (n == BIG_MAX + 1) fprintf(stderr, "  [mtl] bigalloc (limit of %d reached, no more lines)\n", BIG_MAX);
}

void kl_mtl_track(const char *tag, id obj) {
    if (!tag || !obj) return;
    @autoreleasepool {
        id<MTLResource> r = obj;
        uint64_t bytes = r.allocatedSize;
        id dev = r.device;
        os_unfair_lock_lock(&g_lock);
        int i = site_locked(tag, KIND_RES);
        if (i >= 0) [g_live[i] addObject:obj];
        note_device_locked(dev);
        os_unfair_lock_unlock(&g_lock);
        if (bytes >= BIG_ALLOC) big_warn(tag, r, bytes);
    }
}

void kl_mtl_track_count(const char *tag, id obj) {
    if (!tag || !obj) return;
    os_unfair_lock_lock(&g_lock);
    int i = site_locked(tag, KIND_COUNT);
    if (i >= 0) [g_live[i] addObject:obj];
    os_unfair_lock_unlock(&g_lock);
}

void kl_mtl_track_obj(const char *tag, id obj, uint64_t before) {
    if (!tag || !obj) return;
    @autoreleasepool {
        id dev = [obj respondsToSelector:@selector(device)] ? [(id<MTLResource>)obj device] : nil;   // libraries, pipelines too
        uint64_t now = kl_mtl_attr_probe(dev);
        os_unfair_lock_lock(&g_lock);
        int i = site_locked(tag, KIND_OBJ);
        if (i >= 0) {
            [g_live[i] addObject:obj];
            if (now > before) g_delta[i] += now - before;
        }
        note_device_locked(dev);
        os_unfair_lock_unlock(&g_lock);
    }
}

typedef struct { const char *tag; int kind; uint64_t bytes, live; } Row;

// Fills `rows` (one per site) and returns the count; *tracked = bytes of the KIND_RES sites still alive.
// The tables are copied under the lock, the sums run outside it.
static int collect(Row *rows, uint64_t *tracked) {
    NSMutableArray *arrs = [NSMutableArray array];
    int n;
    os_unfair_lock_lock(&g_lock);
    n = g_n;
    for (int i = 0; i < n; i++) {
        rows[i].tag = g_tag[i];
        rows[i].kind = g_kind[i];
        rows[i].bytes = g_delta[i];
        NSArray *alive = g_live[i].allObjects;
        // Dead weak entries pile up under churn: rebuild the table with the living only.
        NSUInteger held = g_live[i].count;
        if (held > 256 && held > 2 * alive.count) {
            NSHashTable *old = g_live[i], *t = new_table();
            for (id o in alive) [t addObject:o];
            g_live[i] = t;
            KL_DROP(old);
        }
        [arrs addObject:alive];
    }
    os_unfair_lock_unlock(&g_lock);
    *tracked = 0;
    for (int i = 0; i < n; i++) {
        NSArray *a = arrs[i];
        rows[i].live = a.count;
        if (rows[i].kind != KIND_RES) continue;
        for (id<MTLResource> r in a) rows[i].bytes += r.allocatedSize;
        *tracked += rows[i].bytes;
    }
    return n;
}

// Appends "<key>tag:MiB/live,..." (key = " kl_top=" etc.) (the `max` largest of `kind` with something alive; by live for KIND_COUNT).
static void put_list(char *out, size_t cap, const char *key, const Row *rows, int n, int kind, int max) {
    char used[MAX_SITES] = {0};
    size_t len = strlen(out);
    for (int k = 0; k < max; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used[i] || rows[i].kind != kind || !rows[i].live) continue;
            uint64_t a = kind == KIND_COUNT ? rows[i].live : rows[i].bytes;
            uint64_t b = best < 0 ? 0 : kind == KIND_COUNT ? rows[best].live : rows[best].bytes;
            if (best < 0 || a > b) best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        const Row *r = &rows[best];
        if (len + 1 >= cap) break;
        const char *sep = k ? "," : key;
        if (kind == KIND_COUNT)
            len += snprintf(out + len, cap - len, "%s%s:%llu", sep, r->tag, (unsigned long long)r->live);
        else
            len += snprintf(out + len, cap - len, "%s%s:%s%.1fM/%llu", sep, r->tag, kind == KIND_OBJ ? "~" : "",
                            (double)r->bytes / 1048576.0, (unsigned long long)r->live);
    }
}

// Value of "key=" in `s` (at the start or after a space), up to the next space, into out. 1 if found.
static int kv(const char *s, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = s; (p = strstr(p, key)); p += kl)
        if (p == s || p[-1] == ' ') {
            const char *e = p + kl;
            size_t n = strcspn(e, " ");
            if (n >= cap) n = cap - 1;
            memcpy(out, e, n);
            out[n] = 0;
            return 1;
        }
    out[0] = 0;
    return 0;
}

// The buffers are static (this runs on the game thread too: ~14 KB of stack otherwise), one printer at a time.
static os_unfair_lock g_print_lock = OS_UNFAIR_LOCK_INIT;
static struct {
    char gb[4096];
    Row rows[MAX_SITES];
    char vtop[768], vobj[512], vcnt[512], line[3072];
} P;

void kl_mtl_attr_print(const char *prefix, const char *vpk) {
    os_unfair_lock_lock(&g_print_lock);
    @autoreleasepool {
        if (!vpk) {
            P.gb[0] = 0;
            kl_vulkan_mem_stats(P.gb, sizeof P.gb);
            vpk = P.gb;
        }
        uint64_t kl_tracked = 0;
        int n = collect(P.rows, &kl_tracked);
        char tok[64];
        kv(vpk, "attr_tracked=", tok, sizeof tok);
        unsigned long long vpk_tracked = strtoull(tok, NULL, 10);
        kv(vpk, "alloc=", tok, sizeof tok);
        unsigned long long vpk_alloc = strtoull(tok, NULL, 10);
        kv(vpk, "attr_top=", P.vtop, sizeof P.vtop);
        kv(vpk, "attr_obj=", P.vobj, sizeof P.vobj);
        kv(vpk, "attr_cnt=", P.vcnt, sizeof P.vcnt);
        unsigned long long dev_alloc[MAX_DEVS] = {0};
        int ndev;
        os_unfair_lock_lock(&g_lock);
        ndev = g_ndev;
        id<MTLDevice> d[MAX_DEVS];
        for (int i = 0; i < ndev; i++) d[i] = g_dev[i];
        os_unfair_lock_unlock(&g_lock);
        for (int i = 0; i < ndev; i++) dev_alloc[i] = (unsigned long long)d[i].currentAllocatedSize;
        unsigned long long total = ndev ? dev_alloc[0] : vpk_alloc;
        long long untracked = (long long)total - (long long)kl_tracked - (long long)vpk_tracked;
        char *line = P.line;
        int len = snprintf(line, sizeof P.line, "total=%lluM tracked=%lluM (vpk=%lluM kl=%lluM) untracked=%lldM devs=%d",
                           total >> 20, (kl_tracked + vpk_tracked) >> 20, vpk_tracked >> 20,
                           (unsigned long long)(kl_tracked >> 20), untracked / 1048576, ndev);
        if (ndev > 1) {
            len += snprintf(line + len, sizeof P.line - len, " dev_alloc=");
            for (int i = 0; i < ndev; i++)
                len += snprintf(line + len, sizeof P.line - len, "%s%lluM", i ? "/" : "", dev_alloc[i] >> 20);
        }
        if (vpk_alloc && vpk_alloc != total)
            len += snprintf(line + len, sizeof P.line - len, " vpk_alloc=%lluM", vpk_alloc >> 20);
        (void)len;
        put_list(line, sizeof P.line, " kl_top=", P.rows, n, KIND_RES, 12);
        put_list(line, sizeof P.line, " kl_obj=", P.rows, n, KIND_OBJ, 6);
        put_list(line, sizeof P.line, " kl_cnt=", P.rows, n, KIND_COUNT, 6);
        size_t l = strlen(line);
        if (P.vtop[0]) l += snprintf(line + l, sizeof P.line - l, " vpk_top=%s", P.vtop);
        if (P.vobj[0]) l += snprintf(line + l, sizeof P.line - l, " vpk_obj=%s", P.vobj);
        if (P.vcnt[0]) snprintf(line + l, sizeof P.line - l, " vpk_cnt=%s", P.vcnt);
        fprintf(stderr, "%s gpu.attr %s\n", prefix ? prefix : "[hb]", line);
        fflush(stderr);
    }
    os_unfair_lock_unlock(&g_print_lock);
}
