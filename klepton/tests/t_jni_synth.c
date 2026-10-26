// Synthetic object returns of kl_jni.c (KL-TEST synthobj block, cut out at build time).
// A Unity 6 player calls createTelemetryHandler and uses the result without a null check:
// the call must answer an inert object of the class its own signature names, never null.
#include <stdio.h>
#include <string.h>

#include "synthobj.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    char c[64];
    CHECK(klj_synth_object_class("createTelemetryHandler", "()Lcom/unity3d/player/TelemetryHandler;", c, sizeof c) &&
          !strcmp(c, "com/unity3d/player/TelemetryHandler"), "class taken from the return type");
    CHECK(klj_synth_object_class("createTelemetryHandler", "(Landroid/content/Context;I)Lcom/x/Y;", c, sizeof c) &&
          !strcmp(c, "com/x/Y"), "arguments are skipped");
    CHECK(klj_synth_object_class("createTelemetryHandler", "()Ljava.lang.Object;", c, sizeof c) &&
          !strcmp(c, "java/lang/Object"), "dots from a C# spelling become slashes");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "()V", c, sizeof c), "void is not an object");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "()I", c, sizeof c), "int is not an object");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "()[B", c, sizeof c), "an array is not synthesised");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "()Lcom/x/Y", c, sizeof c), "unterminated class");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "no paren", c, sizeof c), "no signature");
    CHECK(!klj_synth_object_class("createTelemetryHandler", "()Lcom/unity3d/player/TelemetryHandler;", c, 8),
          "too small a buffer is refused, not truncated");
    CHECK(!klj_synth_object_class("createSomethingElse", "()Lcom/x/Y;", c, sizeof c), "only the named method");
    if (!fails) puts("t_jni_synth: ok");
    return fails ? 1 : 0;
}
