// Copia de klovrp_pred_frame_offset (kl_ovrp.c); mantener en sincronía. gcc -Wall pred_frame_offset_test.c && ./a.out
#include <stdio.h>
static double klovrp_pred_frame_offset(int on, int idx, int cur, float hz) {
    if (!on || cur < 0 || idx <= cur || !(hz > 1.0f)) return 0.0;
    int k = idx - cur; if (k > 3) k = 3;
    return (double)k / (double)hz;
}
#define CK(c) do{ if(!(c)){printf("FAIL %s\n",#c);f=1;} }while(0)
int main(void){int f=0;
CK(klovrp_pred_frame_offset(0,11,10,90)==0);
CK(klovrp_pred_frame_offset(1,10,10,90)==0);
CK(klovrp_pred_frame_offset(1,9,10,90)==0);
CK(klovrp_pred_frame_offset(1,11,10,90)==1.0/90);
CK(klovrp_pred_frame_offset(1,99,10,90)==3.0/90);
CK(klovrp_pred_frame_offset(1,11,-1,90)==0);
puts(f?"FALLA":"OK");return f;}
