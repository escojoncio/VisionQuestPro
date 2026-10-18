// Copias de klovrp_pred_frame_offset y klovrp_extrapolate_pose (kl_ovrp.c); mantener en sincronía. gcc -Wall pred_frame_offset_test.c && ./a.out
#include <stdio.h>
#include <math.h>
static double klovrp_pred_frame_offset(int on, int idx, int cur, float hz) {
    if (!on || cur < 0 || idx <= cur || !(hz > 1.0f)) return 0.0;
    int k = idx - cur; if (k > 3) k = 3;
    return (double)k / (double)hz;
}
static void klovrp_extrapolate_pose(float *f, double dt) {
    if (!(dt > 0.0)) return;
    if (dt > 0.05) dt = 0.05;
    float t = (float)dt;
    f[4] += f[7] * t; f[5] += f[8] * t; f[6] += f[9] * t;
    float wx = f[13] * t, wy = f[14] * t, wz = f[15] * t;
    float ang = sqrtf(wx * wx + wy * wy + wz * wz);
    if (ang < 1e-9f) return;
    float sh = sinf(ang * 0.5f) / ang, dw = cosf(ang * 0.5f);
    float dx = wx * sh, dy = wy * sh, dz = wz * sh;
    float qx = f[0], qy = f[1], qz = f[2], qw = f[3];
    f[0] = dw * qx + dx * qw + dy * qz - dz * qy;
    f[1] = dw * qy - dx * qz + dy * qw + dz * qx;
    f[2] = dw * qz + dx * qy - dy * qx + dz * qw;
    f[3] = dw * qw - dx * qx - dy * qy - dz * qz;
}
#define CK(c) do{ if(!(c)){printf("FAIL %s\n",#c);f=1;} }while(0)
int main(void){int f=0;
CK(klovrp_pred_frame_offset(0,11,10,90)==0);
CK(klovrp_pred_frame_offset(1,10,10,90)==0);
CK(klovrp_pred_frame_offset(1,9,10,90)==0);
CK(klovrp_pred_frame_offset(1,11,10,90)==1.0/90);
CK(klovrp_pred_frame_offset(1,99,10,90)==3.0/90);
CK(klovrp_pred_frame_offset(1,11,-1,90)==0);
{ float p[16]={0,0,0,1, 0,0,0, 1,0,0, 0,0,0, 0,(float)M_PI,0};
  klovrp_extrapolate_pose(p,0.5); /* clamps to 50 ms */
  CK(fabsf(p[4]-0.05f)<1e-6f);
  CK(fabsf(p[1]-sinf((float)M_PI*0.025f))<1e-5f && fabsf(p[3]-cosf((float)M_PI*0.025f))<1e-5f);
  float q[16]={0,0,0,1}; klovrp_extrapolate_pose(q,0.01); CK(q[3]==1&&q[4]==0); }
puts(f?"FALLA":"OK");return f;}
