// AndroidThunkJava_* table of kl_jni_ue4.c (KL-TEST ue4thunk block, cut out at build time): the values a Quest
// offline gives for the state thunks, the clean failure by signature for the online ones, and that a name or a
// return type the table does not know is left to the generic paths.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ue4thunk.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static const klj_ue4_env OFFLINE = {1, 545, "1.28.0", "/data/cache", "/sdcard/files", 8ull << 30};
static const klj_ue4_env ONLINE  = {0, 545, "1.28.0", "/data/cache", "/sdcard/files", 8ull << 30};

// find + value in one step; returns the row index, -1 for "left to the generic paths".
static int ask(const char *name, const char *sig, const klj_ue4_env *e, int64_t *iv, const char **sv) {
    int i = klj_ue4_thunk_find(name, sig);
    if (i >= 0) klj_ue4_thunk_value(i, klj_ue4_ret(sig), e, iv, sv);
    return i;
}

int main(void) {
    int64_t iv; const char *sv;

    // S: state
    CHECK(ask("AndroidThunkJava_IsAllowedRemoteNotifications", "()Z", &ONLINE, &iv, &sv) >= 0 && iv == 0, "no push, even online");
    CHECK(ask("AndroidThunkJava_HasActiveWiFiConnection", "()Z", &OFFLINE, &iv, &sv) >= 0 && iv == 0, "no wifi offline");
    CHECK(ask("AndroidThunkJava_HasActiveWiFiConnection", "()Z", &ONLINE, &iv, &sv) >= 0 && iv == 1, "wifi when KL_NET_OFFLINE=0");
    CHECK(ask("AndroidThunkJava_IsWifiConnected", "()Z", &OFFLINE, &iv, &sv) >= 0 && iv == 0, "IsWifiConnected offline");
    CHECK(ask("AndroidThunkJava_GetAppVersionName", "()Ljava/lang/String;", &OFFLINE, &iv, &sv) >= 0 && !strcmp(sv, "1.28.0"), "version name");
    CHECK(ask("AndroidThunkJava_GetAppVersionCode", "()I", &OFFLINE, &iv, &sv) >= 0 && iv == 545, "version code (int)");
    CHECK(ask("AndroidThunkJava_GetAppVersionCode", "()J", &OFFLINE, &iv, &sv) >= 0 && iv == 545, "version code (long)");
    CHECK(ask("AndroidThunkJava_GetCacheDir", "()Ljava/lang/String;", &OFFLINE, &iv, &sv) >= 0 && !strcmp(sv, "/data/cache"), "cache dir");
    CHECK(ask("AndroidThunkJava_GetExternalFilesDir", "()Ljava.lang.String;", &OFFLINE, &iv, &sv) >= 0 && !strcmp(sv, "/sdcard/files"), "ext dir, dotted spelling");
    CHECK(ask("AndroidThunkJava_GetDeviceMemory", "()J", &OFFLINE, &iv, &sv) >= 0 && iv == (int64_t)(8ull << 30), "memory as bytes for a long");
    CHECK(ask("AndroidThunkJava_GetDeviceMemory", "()I", &OFFLINE, &iv, &sv) >= 0 && iv == 8192, "memory as MiB for an int");
    CHECK(ask("AndroidThunkJava_GetCPUFamily", "()Ljava/lang/String;", &OFFLINE, &iv, &sv) >= 0 && !strcmp(sv, "arm64-v8a"), "cpu abi");

    // a state row asked with another return type is NOT answered by the wrong shape
    CHECK(ask("AndroidThunkJava_GetAppVersionCode", "()Ljava/lang/String;", &OFFLINE, &iv, &sv) < 0, "int row, string asked");
    CHECK(ask("AndroidThunkJava_HasActiveWiFiConnection", "()I", &OFFLINE, &iv, &sv) < 0, "bool row, int asked");
    CHECK(ask("AndroidThunkJava_GetCacheDir", "()V", &OFFLINE, &iv, &sv) < 0, "string row, void asked");

    // H: host-owned void
    CHECK(ask("AndroidThunkJava_Vibrate", "(I)V", &OFFLINE, &iv, &sv) >= 0 && iv == 0 && sv == NULL, "vibrate is a void no-op");
    CHECK(ask("AndroidThunkJava_Minimize", "()V", &OFFLINE, &iv, &sv) >= 0, "minimize");

    // O: clean failure by signature, whatever the parameter list
    CHECK(ask("AndroidThunkJava_IsInterstitialAdAvailable", "()Z", &ONLINE, &iv, &sv) >= 0 && iv == 0, "no ad: false");
    CHECK(ask("AndroidThunkJava_ShowAdBanner", "(Ljava/lang/String;Z)V", &ONLINE, &iv, &sv) >= 0 && iv == 0, "ad banner void, 2 params");
    CHECK(ask("AndroidThunkJava_ShowAdBanner", "()V", &ONLINE, &iv, &sv) >= 0, "ad banner void, no params");
    CHECK(ask("AndroidThunkJava_IapBeginPurchase", "(Ljava/lang/String;)Z", &ONLINE, &iv, &sv) >= 0 && iv == 0, "purchase refused");
    CHECK(ask("AndroidThunkJava_IapQueryExistingPurchases", "()I", &ONLINE, &iv, &sv) >= 0 && iv == 0, "iap as int: 0");
    CHECK(ask("AndroidThunkJava_LocalNotificationGetLaunchNotification", "()Ljava/lang/String;", &ONLINE, &iv, &sv) >= 0
          && sv && sv[0] == 0, "a String clean failure is empty, not null");
    CHECK(ask("AndroidThunkJava_LocalNotificationGetLaunchNotification", "()Lcom/x/Y;", &ONLINE, &iv, &sv) >= 0
          && sv == NULL && iv == 0, "an object clean failure is null");
    CHECK(ask("AndroidThunkJava_RegisterForRemoteNotifications", "()[B", &ONLINE, &iv, &sv) >= 0 && sv == NULL, "an array clean failure is null");
    CHECK(ask("AndroidThunkJava_IapIsAllowedToMakePurchases", "()F", &ONLINE, &iv, &sv) >= 0 && iv == 0, "float: zero");

    // not ours
    CHECK(ask("AndroidThunkJava_NoSuchThing", "()Z", &ONLINE, &iv, &sv) < 0, "unknown name");
    CHECK(ask("GetAppVersionName", "()Ljava/lang/String;", &ONLINE, &iv, &sv) < 0, "prefix required");
    CHECK(ask("AndroidThunkJava_GetCacheDir", "no paren", &ONLINE, &iv, &sv) < 0, "not a signature");
    CHECK(ask("AndroidThunkJava_GetCacheDir", "()Q", &ONLINE, &iv, &sv) < 0, "bad return type");
    CHECK(ask(NULL, "()V", &ONLINE, &iv, &sv) < 0 && ask("AndroidThunkJava_Vibrate", NULL, &ONLINE, &iv, &sv) < 0, "NULLs");
    // rows that have their own binding (kl_jni_ue4.c table) must not be shadowed by this one
    CHECK(ask("AndroidThunkJava_GetAndroidId", "()Ljava/lang/String;", &ONLINE, &iv, &sv) < 0, "bound rows stay out of the table");
    CHECK(ask("AndroidThunkJava_ForceQuit", "()V", &ONLINE, &iv, &sv) < 0, "ForceQuit stays bound");

    // table shape: unique names, known class and kind, S rows typed, O rows kind '*', counts per class
    int n[256] = {0};
    for (int i = 0; i < KLJ_UE4_NTHUNKS; i++) {
        const klj_ue4_thunk *t = &g_ue4_thunks[i];
        n[(unsigned char)t->cls]++;
        CHECK(strchr("SHO", t->cls) != NULL, "class is S, H or O");
        CHECK(strchr("ZNSV*", t->kind) != NULL, "kind is known");
        CHECK(t->cls != 'S' || t->kind != '*', "a state row names its type");
        CHECK(t->cls != 'O' || t->kind == '*', "an online row fails clean for any type");
        for (int j = 0; j < i; j++) CHECK(strcmp(t->name, g_ue4_thunks[j].name) != 0, "unique names");
    }
    CHECK(n['S'] == 10 && n['H'] == 5 && n['O'] == 21, "10 state / 5 host-owned / 21 online rows");
    if (!fails) puts("t_ue4_thunk: ok");
    return fails ? 1 : 0;
}
