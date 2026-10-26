// UE receivers' initial dispatch (kl_jni_ue4.c, KL-TEST ue4recv block, cut out at build time): startReceiver reads the
// current state and calls the class's registered native. UE4 registers it on com/epicgames/ue4/<Receiver>, UE5 (ITR2,
// Wanderer, OLAR) on com/epicgames/unreal/<Receiver>; the dispatch must find either, or the engine never learns the state.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef union { uint64_t j; double d; void *l; } klj_val;
#define KLJ_LOG(...) do { fprintf(stderr, "[jni] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// Stand-in for kl_jni.c's native table.
static struct { const char *cls, *name, *sig; void *fn; } reg[8];
static int nreg;
static void add(const char *cls, const char *name, const char *sig, void *fn) { reg[nreg].cls = cls; reg[nreg].name = name; reg[nreg].sig = sig; reg[nreg++].fn = fn; }
void *kl_jni_native(const char *cls, const char *name, const char *sig) {
    for (int i = 0; i < nreg; i++)
        if (!strcmp(reg[i].cls, cls) && !strcmp(reg[i].name, name) && (!sig || !strcmp(reg[i].sig, sig))) return reg[i].fn;
    return NULL;
}

#include "ue4recv.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int got1[2], got3[4];
static void vol(void *e, void *c, int32_t v) { (void)e; (void)c; got1[0]++; got1[1] = v; }
static void bat(void *e, void *c, int32_t s, int32_t l, int32_t t) { (void)e; (void)c; got3[0]++; got3[1] = s; got3[2] = l; got3[3] = t; }

int main(void) {
    klj_val v = {.j = 7};
    klj_val b[3] = {{.j = 3}, {.j = 100}, {.j = 250}};

    // UE4: registered on the ue4 class.
    add("com/epicgames/ue4/VolumeReceiver", "volumeChanged", "(I)V", (void *)vol);
    klj_ue4_receiver_dispatch(NULL, "com/epicgames/ue4/VolumeReceiver", "volumeChanged", "(I)V", &v, 1);
    CHECK(got1[0] == 1 && got1[1] == 7, "ue4 class: volume dispatched");

    // UE5: the guest registered on the unreal class; the dispatch is asked with the ue4 spelling.
    add("com/epicgames/unreal/BatteryReceiver", "dispatchEvent", "(III)V", (void *)bat);
    klj_ue4_receiver_dispatch(NULL, "com/epicgames/ue4/BatteryReceiver", "dispatchEvent", "(III)V", b, 3);
    CHECK(got3[0] == 1 && got3[1] == 3 && got3[2] == 100 && got3[3] == 250, "unreal class: battery dispatched");

    // Nothing registered: no call, no crash.
    klj_ue4_receiver_dispatch(NULL, "com/epicgames/ue4/HeadsetReceiver", "stateChanged", "(I)V", &v, 1);
    CHECK(got1[0] == 1, "unregistered native: nothing called");

    if (!fails) puts("t_ue4_receivers: ok");
    return fails ? 1 : 0;
}
