/* vec_edge: AltiVec edge cases, printed for comparison with real hardware (G4) and the translator. */
#include <stdio.h>
#include <string.h>
#include <altivec.h>
typedef union { vector float f; vector signed int i; vector unsigned int u; vector signed short s; vector unsigned char c; float ff[4]; unsigned int uu[4]; unsigned char cc[16]; short ss[8]; } V;
static void pv(const char *n, V v) { printf("%-12s %08x %08x %08x %08x\n", n, v.uu[0], v.uu[1], v.uu[2], v.uu[3]); }
static void vscr(const char *n) { V v; v.u = (vector unsigned int)vec_mfvscr(); printf("%-12s vscr %08x\n", n, v.uu[3]); }
int main(void) {
    V a, b, c, r;
    float af[4] = { 1.0f, 3.0f, 1e-40f, -0.0f }, bf[4] = { 2.0f, -0.5f, 1e38f, 1e-45f }, cf[4] = { 0.5f, 1e30f, -1e30f, 7.0f };
    int i;
    memcpy(a.ff, af, 16); memcpy(b.ff, bf, 16); memcpy(c.ff, cf, 16);
    vscr("start");
    r.f = vec_madd(a.f, b.f, c.f); pv("vmaddfp", r);
    r.f = vec_nmsub(a.f, b.f, c.f); pv("vnmsubfp", r);
    r.f = vec_re(a.f); pv("vrefp", r);
    r.f = vec_rsqrte(a.f); pv("vrsqrtefp", r);
    r.f = vec_expte(a.f); pv("vexptefp", r);
    r.f = vec_loge(a.f); pv("vlogefp", r);
    r.f = vec_add(a.f, b.f); pv("vaddfp", r);
    r.f = vec_max(a.f, b.f); pv("vmaxfp", r);
    r.i = vec_cts(c.f, 0); pv("vctsxs", r); vscr("after cts");
    r.u = vec_ctu(c.f, 2); pv("vctuxs", r);
    r.f = vec_ctf(vec_splat_s32(-7), 1); pv("vcfsx", r);
    r.f = vec_round(c.f); pv("vrfin", r);
    r.f = vec_floor(b.f); pv("vrfim", r);
    { vector signed int x = { 0x7fffffff, -2147483647 - 1, 70000, -70000 };
      r.s = vec_packs(x, x); pv("vpkswss", r); vscr("after packs");
      r.s = (vector signed short)vec_packsu(x, x); pv("vpkswus", r); }
    { vector unsigned char p = vec_lvsl(3, (unsigned char *)0); r.c = p; pv("lvsl 3", r);
      p = vec_lvsr(5, (unsigned char *)0); r.c = p; pv("lvsr 5", r); }
    { vector signed short x = { 32767, -32768, 1000, -1000, 1, 2, 3, 4 }, y = { 2, 2, 40, 40, -1, -2, -3, -4 };
      r.s = vec_madds(x, y, x); pv("vmhaddshs", r); vscr("after madds");
      r.i = vec_msums(x, y, vec_splat_s32(-1)); pv("vmsumshs", r); }
    { unsigned char buf[32] __attribute__((aligned(16))); for (i = 0; i < 32; i++) buf[i] = i * 9;
      r.c = vec_ld(5, buf); pv("lvx 5", r); r.c = vec_lde(7, buf); pv("lvebx 7", r);
      r.c = vec_perm(vec_ld(0, buf), vec_ld(16, buf), vec_lvsl(3, buf)); pv("perm", r); }
    { V d; d.ff[0] = 1e-40f; d.ff[1] = -1e-40f; d.ff[2] = 1.0f; d.ff[3] = 1e-39f;
      r.f = vec_add(d.f, d.f); pv("denorm add", r);
      vec_mtvscr((vector unsigned short)vec_splat_u32(0)); vscr("nj=0");
      r.f = vec_add(d.f, d.f); pv("denorm nj0", r);
      vec_mtvscr((vector unsigned short)(vector unsigned int){0,0,0,0x10000}); vscr("nj=1"); }
    return 0;
}
