// The headset's thermal state for AThermal (kl_android.c): NSProcessInfo's
// thermalState, which is what the compositor already logs as "thermal state".
// 0 nominal, 1 fair, 2 serious, 3 critical — NSProcessInfoThermalState's own numbers.
#import <Foundation/Foundation.h>

int kl_host_thermal_state(void) {
    return (int)[[NSProcessInfo processInfo] thermalState];
}
