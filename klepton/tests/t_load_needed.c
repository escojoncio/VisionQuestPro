// DT_NEEDED loading of kl_dl.c (kl_load_deps / kl_load_needed / kl_load_recursive,
// cut out at build time with the loader's collaborators replaced by a fake library
// tree). Regression: libUnreal (Into the Radius 2) NEEDs libOmniConnectShared, which
// kl_ue4_load never mapped -> its imports (__cxa_guard_acquire, ...) were left
// unresolved. kl_load_needed must map + register the entry's NEEDED that the fixed
// chain does not cover, deferring their init so the caller can order it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct kl_image kl_image;
struct kl_image { char name[128]; };

#define MAXLIB 16
typedef struct { const char *name; const char *needs[6]; int fail_load; } fakelib;
static fakelib g_tree[] = {
    { "libUnreal.so",          { "libc.so", "libc++_shared.so", "libOmniConnectShared.so",
                                 "libEOSSDK.so", "libOVRPlugin.so", "libopenxr_loader.so" } },
    { "libc++_shared.so",      { "libc.so" } },
    { "libEOSSDK.so",          { "libc.so" } },
    { "libOmniConnectShared.so", { "libc.so", "libomnidep.so" } },
    { "libomnidep.so",         { "libm.so" } },
    { "libbroken.so",          { "libomnidep.so", "libnotthere.so" } },
    { "libbad_dep_parent.so",  { "libbaddep.so" } },
    { "libbaddep.so",          { 0 }, 1 },
    { "libmixed.so",           { "libbaddep.so", "libomnidep.so" } },
    { "libdiamond.so",         { "libomnidep.so", "libOmniConnectShared.so" } },
};
static const char *g_refuse;      // substring the fake kl_dlopen_refused refuses

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static fakelib *lookup(const char *path) {
    const char *b = basename_of(path);
    for (unsigned i = 0; i < sizeof g_tree / sizeof *g_tree; i++)
        if (!strcmp(g_tree[i].name, b)) return &g_tree[i];
    return NULL;
}

// ---- collaborators ----
static char g_libdir[512] = "/lib";
static kl_image *g_reg[MAXLIB]; static int g_nreg;
static char g_init_log[MAXLIB][128]; static int g_ninit;
static char g_map_log[MAXLIB][128];  static int g_nmap;

static int kl_list_needed(const char *path, char names[][128], int max) {
    fakelib *l = lookup(path);
    if (!l) return -1;
    int n = 0;
    for (int i = 0; i < 6 && l->needs[i] && n < max; i++) snprintf(names[n++], 128, "%s", l->needs[i]);
    return n;
}
static int kl_can_load(const char *path) {
    fakelib *l = lookup(path);
    return l != NULL;
}
static int kl_openxr_claims(const char *so) { return !strcmp(so, "libopenxr_loader.so"); }
static int kl_env_on(const char *name, int dflt) {
    if (!strcmp(name, "KL_TRACE_NEEDED")) return 0;
    return dflt;
}
static int kl_dlopen_refused(const char *path) { return g_refuse && strstr(path, g_refuse); }
static const char *kl_error(void) { return "fake load failure"; }
static kl_image *kl_find_image(const char *so) {
    const char *b = basename_of(so);
    for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i]->name, b)) return g_reg[i];
    return NULL;
}
static void kl_register_image(const char *so, kl_image *img) {
    snprintf(img->name, sizeof img->name, "%s", basename_of(so));
    g_reg[g_nreg++] = img;
}
static kl_image *kl_load_auto(const char *path) {
    fakelib *l = lookup(path);
    if (!l || l->fail_load) return NULL;
    kl_image *img = calloc(1, sizeof *img);
    snprintf(img->name, sizeof img->name, "%s", l->name);
    snprintf(g_map_log[g_nmap++], 128, "%s", l->name);
    return img;
}
static void kl_run_init(kl_image *img) { snprintf(g_init_log[g_ninit++], 128, "%s", img->name); }
static void *kl_sym(kl_image *img, const char *n) { (void)img; (void)n; return NULL; }
static void *kl_jni_vm(void) { return NULL; }
// First claim per image wins, like the real one (kl_jni.c): the one JNI_OnLoad guard kl_dl.c now calls.
static int kl_jni_onload_claim(const void *image) {
    static const void *done[16]; static int n;
    for (int i = 0; i < n; i++) if (done[i] == image) return 0;
    if (n < 16) done[n++] = image;
    return 1;
}

#include "ldeps.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static void reset(void) {
    for (int i = 0; i < g_nreg; i++) free(g_reg[i]);
    g_nreg = g_ninit = g_nmap = 0;
    g_refuse = NULL;
}
static void preload_chain(void) {      // what kl_ue4_load has mapped before the entry
    static const char *const chain[] = { "libc++_shared.so", "libEOSSDK.so" };
    for (int i = 0; i < 2; i++) kl_register_image(chain[i], kl_load_auto(chain[i]));
    g_nmap = 0;
}

int main(void) {
    kl_image *out[8];

    // 1. The bug: the entry's NEEDED outside the chain gets mapped and registered,
    //    leaves first, with NO init run and the entry itself untouched.
    reset(); preload_chain();
    int n = kl_load_needed("/lib/libUnreal.so", out, 8, NULL);
    CHECK(n == 2, "libomnidep + libOmniConnectShared mapped (chain members and shims skipped)");
    CHECK(n == 2 && !strcmp(out[0]->name, "libomnidep.so") &&
          !strcmp(out[1]->name, "libOmniConnectShared.so"), "returned leaves first");
    CHECK(kl_find_image("/lib/libOmniConnectShared.so") != NULL, "libOmniConnectShared registered");
    CHECK(kl_find_image("/lib/libomnidep.so") != NULL, "its own dependency registered");
    CHECK(kl_find_image("/lib/libUnreal.so") == NULL, "entry itself not loaded");
    CHECK(g_ninit == 0, "init deferred to the caller");
    CHECK(g_nmap == 2, "chain members not mapped twice (EOSSDK / libc++_shared)");

    // 2. Idempotent: everything is registered now, nothing more to map.
    CHECK(kl_load_needed("/lib/libUnreal.so", out, 8, NULL) == 0, "second call maps nothing");

    // 3. kl_load_recursive unchanged: deps first, each init'd right after its map,
    //    the root last.
    reset();
    kl_image *root = kl_load_recursive("/lib/libOmniConnectShared.so");
    CHECK(root != NULL, "recursive load ok");
    CHECK(g_ninit == 2 && !strcmp(g_init_log[0], "libomnidep.so") &&
          !strcmp(g_init_log[1], "libOmniConnectShared.so"), "init in dependency order, immediately");
    CHECK(kl_load_recursive("/lib/libOmniConnectShared.so") == root && g_ninit == 2,
          "already loaded: same image, no re-init");

    // 4. Diamond: a shared dependency is mapped once.
    reset();
    CHECK(kl_load_recursive("/lib/libdiamond.so") != NULL && g_nmap == 3, "diamond maps each lib once");

    // 5. kl_load_recursive still fails as a whole when a dependency fails.
    reset();
    CHECK(kl_load_recursive("/lib/libbad_dep_parent.so") == NULL, "dependency failing to load fails the parent");

    // 6. kl_load_needed does NOT abort: the failing dependency is skipped and
    //    counted, the others still load.
    int nf = -1;
    reset();
    n = kl_load_needed("/lib/libbad_dep_parent.so", out, 8, &nf);
    CHECK(n == 0 && nf == 1, "only dependency fails: nothing mapped, 1 failure reported, no abort");
    reset(); nf = -1;
    n = kl_load_needed("/lib/libmixed.so", out, 8, &nf);
    CHECK(n == 1 && nf == 1 && !strcmp(out[0]->name, "libomnidep.so"),
          "failing dependency skipped, the other one still mapped");
    reset(); preload_chain(); nf = -1;
    n = kl_load_needed("/lib/libUnreal.so", out, 8, &nf);
    CHECK(nf == 0 && n == 2, "no failure reported when all load");

    // 7. Overflow of out[]: reports how many there were, maps them all, no abort.
    reset(); preload_chain();
    n = kl_load_needed("/lib/libUnreal.so", out, 1, NULL);
    CHECK(n == 2 && !strcmp(out[0]->name, "libomnidep.so"), "overflow: full count returned, first fits");
    CHECK(kl_find_image("/lib/libOmniConnectShared.so") != NULL, "overflow: the extra is still mapped and registered");

    // 8. Unloadable NEEDED: warn, skip, carry on.
    reset();
    n = kl_load_needed("/lib/libbroken.so", out, 8, NULL);
    CHECK(n == 1 && !strcmp(out[0]->name, "libomnidep.so"), "unloadable NEEDED is skipped, rest still loaded");

    // 9. Refusals of the dlopen door (kl_dlopen_refused: EOS, AMFI list, KL_DLOPEN_REFUSE)
    //    apply to NEEDED edges too, in both loaders.
    reset(); g_refuse = "libEOSSDK";
    n = kl_load_needed("/lib/libUnreal.so", out, 8, NULL);
    CHECK(kl_find_image("/lib/libEOSSDK.so") == NULL, "refused EOSSDK not loaded via NEEDED");
    CHECK(n == 3, "libc++_shared + omnidep + OmniConnectShared mapped when nothing was preloaded");
    reset(); g_refuse = "OmniConnect";
    n = kl_load_needed("/lib/libUnreal.so", out, 8, NULL);
    CHECK(kl_find_image("/lib/libOmniConnectShared.so") == NULL, "refused library not loaded via NEEDED");
    CHECK(kl_find_image("/lib/libomnidep.so") == NULL, "...and neither is what only it needed");
    reset(); g_refuse = "libomnidep";
    CHECK(kl_load_recursive("/lib/libOmniConnectShared.so") != NULL && g_nmap == 1,
          "kl_load_recursive honours refusals of its dependencies too");

    reset();
    if (fails) return 1;
    printf("t_load_needed ok\n");
    return 0;
}
