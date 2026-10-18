// Darwin-only half of kl_bionic_extra3.c: the pieces that need kqueue, termios, mach or the host's
// FILE internals. Rows are served through kl_bionic_extra4_lookup, which kl_bionic_extra3_lookup
// consults after its own table.
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <string.h>

typedef struct { const char *name; void *fn; } kl4_entry;
static const kl4_entry g_extra4[] = {
    { "", 0 },
};

void *kl_bionic_extra4_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_extra4 / sizeof g_extra4[0]; i++)
        if (g_extra4[i].name[0] && !strcmp(g_extra4[i].name, name)) return g_extra4[i].fn;
    return NULL;
}
