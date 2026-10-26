// UE command-line override path of guest/kl_ue4.c (KL-TEST ue4cmd block, cut out at build time).
// The engine reads <files>/<native_dir>/<Project>/<name>, not <files>/<native_dir>/<name>:
// the override carrying -stdout was written one directory too high and never read.
#include <stdio.h>
#include <string.h>

#include "ue4cmd.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    char p[256];
    CHECK(ue4_cmdline_path(p, sizeof p, "/f", "UE4Game", "Maestro", "UE4CommandLine.txt") == 0 &&
          !strcmp(p, "/f/UE4Game/Maestro/UE4CommandLine.txt"), "UE4 project path");
    CHECK(ue4_cmdline_path(p, sizeof p, "/f", "UnrealGame", "Wanderer", "UECommandLine.txt") == 0 &&
          !strcmp(p, "/f/UnrealGame/Wanderer/UECommandLine.txt"), "UE5 project path");
    char dir[256];
    CHECK(ue4_cmdline_dir(dir, sizeof dir, "/f", "UE4Game", "Maestro") == 0 &&
          !strcmp(dir, "/f/UE4Game/Maestro"), "project dir");
    char small[8];
    CHECK(ue4_cmdline_path(small, sizeof small, "/f", "UE4Game", "Maestro", "UE4CommandLine.txt") != 0,
          "truncation reported");
    CHECK(ue4_extras_target("olar", 0) == NULL, "extras off by default");
    CHECK(ue4_extras_target("olar", 1) != NULL && !strcmp(ue4_extras_target("olar", 1), "olar"),
          "extras on with knob");
    if (fails) return 1;
    printf("t_ue4_cmdline ok\n");
    return 0;
}
