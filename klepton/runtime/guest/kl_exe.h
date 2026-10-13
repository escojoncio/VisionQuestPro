// The executable door (KL_GUEST_EXE): see kl_exe.c.
#ifndef KL_EXE_H
#define KL_EXE_H
#include <stdio.h>

int         kl_exe_begin(FILE *out);                       // main() starts on its own guest thread
double      kl_exe_pump(double seconds, const volatile int *quit);   // until main() is done, quit, or seconds
void        kl_exe_report(FILE *out);
int         kl_exe_finished(void);
const char *kl_exe_error(void);
#endif
