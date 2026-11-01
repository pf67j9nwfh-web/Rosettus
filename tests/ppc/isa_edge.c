/* isa_edge: PowerPC instruction edge cases, printed for comparison with real hardware (G4) and the translator. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
typedef unsigned int u32;
static const u32 iv[] = { 0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x12345678, 0x80000001, 31, 32, 33, 63, 64 };
#define NI (sizeof iv / sizeof iv[0])
static u32 xer_get(void) { u32 x; asm volatile("mfxer %0" : "=r"(x)); return x; }
static void xer_set(u32 x) { asm volatile("mtxer %0" :: "r"(x)); }
static u32 cr_get(void) { u32 x; asm volatile("mfcr %0" : "=r"(x)); return x; }
#define BIN(name, insn) static void t_##name(void) { u32 i, j, h = 0; \
    for (i = 0; i < NI; i++) for (j = 0; j < NI; j++) { u32 a = iv[i], b = iv[j], d, x, c; xer_set(0); \
        asm volatile(insn " %0,%3,%4\n\tmfxer %1\n\tmfcr %2" : "=&r"(d), "=&r"(x), "=&r"(c) : "r"(a), "r"(b) : "cr0", "xer"); \
        h = h * 31 + d; h = h * 31 + (x & 0xe0000000); h = h * 31 + (c >> 28); \
        if ((i == 4 && j == 5) || (j == 0) ) printf("  %-8s %08x,%08x -> %08x xer %08x cr0 %x\n", insn, a, b, d, x & 0xe0000000, c >> 28); } \
    printf("%-8s hash %08x\n", insn, h); }
BIN(add, "addo.") BIN(subf, "subfo.") BIN(mullw, "mullwo.") BIN(mulhw, "mulhw.") BIN(mulhwu, "mulhwu.")
BIN(divw, "divwo.") BIN(divwu, "divwuo.") BIN(addc, "addco.") BIN(subfc, "subfco.") BIN(slw, "slw.") BIN(srw, "srw.") BIN(sraw, "sraw.")
BIN(rlwnm, "rlwnm.") /* rlwnm needs mb,me: handled below instead */
static void t_misc(void) {
    u32 i, h = 0;
    for (i = 0; i < NI; i++) { u32 a = iv[i], d, x;
        xer_set(0x20000000); asm volatile("adde %0,%2,%2\n\tmfxer %1" : "=&r"(d), "=&r"(x) : "r"(a) : "xer"); h = h*31+d; h = h*31+x;
        xer_set(0x20000000); asm volatile("addme %0,%2\n\tmfxer %1" : "=&r"(d), "=&r"(x) : "r"(a) : "xer"); h = h*31+d; h = h*31+x;
        xer_set(0); asm volatile("addze %0,%2\n\tmfxer %1" : "=&r"(d), "=&r"(x) : "r"(a) : "xer"); h = h*31+d; h = h*31+x;
        xer_set(0); asm volatile("srawi %0,%2,4\n\tmfxer %1" : "=&r"(d), "=&r"(x) : "r"(a) : "xer"); h = h*31+d; h = h*31+x;
        asm volatile("cntlzw %0,%1" : "=r"(d) : "r"(a)); h = h*31+d;
        asm volatile("extsb %0,%1" : "=r"(d) : "r"(a)); h = h*31+d;
        asm volatile("extsh %0,%1" : "=r"(d) : "r"(a)); h = h*31+d;
        asm volatile("rlwinm %0,%1,7,30,3" : "=r"(d) : "r"(a)); h = h*31+d;
        asm volatile("rlwnm %0,%1,%2,5,27" : "=r"(d) : "r"(a), "r"(iv[(i+3)%NI])); h = h*31+d;
        asm volatile("nego %0,%2\n\tmfxer %1" : "=&r"(d), "=&r"(x) : "r"(a) : "xer"); h = h*31+d; h = h*31+(x&0xc0000000);
    }
    printf("misc     hash %08x\n", h);
}
static void t_fp(void) {
    static const double fv[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 3.0, 1e-310, -1e-310, 1e308, DBL_MAX, 1.0/3.0, 2.0, 0.1, 1e-38, 1e38, 3.5e38 };
    unsigned i, j; unsigned long long h = 0;
    for (i = 0; i < sizeof fv / sizeof fv[0]; i++) {
        double a = fv[i], r; float fr; union { double d; unsigned long long u; } u; union { float f; u32 u; } uf;
        asm volatile("fres %0,%1" : "=f"(r) : "f"(a)); u.d = r; printf("  fres(%g) = %016llx\n", a, u.u);
        asm volatile("frsqrte %0,%1" : "=f"(r) : "f"(a)); u.d = r; printf("  frsqrte(%g) = %016llx\n", a, u.u);
        asm volatile("frsp %0,%1" : "=f"(fr) : "f"(a)); uf.f = fr; printf("  frsp(%g) = %08x\n", a, uf.u);
        { int iw; double t; asm volatile("fctiwz %0,%1" : "=f"(t) : "f"(a)); u.d = t; iw = (int)(u.u & 0xffffffff); printf("  fctiwz(%g) = %08x\n", a, iw); }
        for (j = 0; j < sizeof fv / sizeof fv[0]; j++) { double b = fv[j];
            asm volatile("fmadd %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(fv[(i+j)%16])); u.d = r; h = h*1000003 + u.u;
            asm volatile("fdivs %0,%1,%2" : "=f"(r) : "f"(a), "f"(b)); u.d = r; h = h*1000003 + u.u;
            asm volatile("fnmsubs %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(fv[(i*3+j)%16])); u.d = r; h = h*1000003 + u.u;
            asm volatile("fsel %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(fv[(j+5)%16])); u.d = r; h = h*1000003 + u.u;
        }
    }
    printf("fp       hash %016llx\n", h);
    { u32 fpscr[2]; double z = 0.0, o = 1.0, r; asm volatile("mtfsfi 7,0\n\tmtfsb0 3\n\tfdiv %0,%1,%2" : "=f"(r) : "f"(o), "f"(z)); 
      asm volatile("mffs %0" : "=f"(r)); memcpy(fpscr, &r, 8); printf("fpscr after 1/0: %08x\n", fpscr[1]);
      asm volatile("mtfsfi 7,0\n\tfdiv %0,%1,%2" : "=f"(r) : "f"(z), "f"(z)); asm volatile("mffs %0" : "=f"(r)); memcpy(fpscr, &r, 8); printf("fpscr after 0/0: %08x\n", fpscr[1]);
      asm volatile("mtfsf 0xff,%0" :: "f"(z)); }
}
static void t_mem(void) {
    static unsigned char buf[64] __attribute__((aligned(16)));
    u32 i, r[8], h = 0;
    for (i = 0; i < 64; i++) buf[i] = (unsigned char)(i * 7 + 3);
    { u32 x = 13; register u32 r5 asm("r5"), r6 asm("r6"), r7 asm("r7"), r8 asm("r8");
      r5 = r6 = r7 = r8 = 0xdeadbeef; xer_set(x);
      asm volatile("lswx r5,0,%4" : "+r"(r5), "+r"(r6), "+r"(r7), "+r"(r8) : "b"(buf + 1) : "memory");
      printf("lswx 13: %08x %08x %08x %08x\n", r5, r6, r7, r8); }
    { u32 v; asm volatile("lwbrx %0,0,%1" : "=r"(v) : "b"(buf + 3)); printf("lwbrx unaligned: %08x\n", v); }
    { unsigned short v; asm volatile("lhbrx %0,0,%1" : "=r"(v) : "b"(buf + 5)); printf("lhbrx: %04x\n", v); }
    { u32 v; asm volatile("lwz %0,0(%1)" : "=r"(v) : "b"(buf + 2)); printf("lwz unaligned: %08x\n", v); }
    { double d; union { double d; unsigned long long u; } u; asm volatile("lfd %0,0(%1)" : "=f"(d) : "b"(buf + 4)); u.d = d; printf("lfd unaligned: %016llx\n", u.u); }
    { u32 old, ok, v = 0x11111111, nv = 0x22222222, res;
      asm volatile("1: lwarx %0,0,%3\n\tstwcx. %4,0,%3\n\tbne- 1b\n\tmfcr %1\n\tlwz %2,0(%3)" : "=&r"(old), "=&r"(ok), "=&r"(res) : "b"(&v), "r"(nv) : "cr0", "memory");
      printf("lwarx/stwcx: old %08x new %08x cr0 %x\n", old, res, ok >> 28); }
    (void)r; (void)h;
}
int main(void) {
    t_add(); t_subf(); t_mullw(); t_mulhw(); t_mulhwu(); t_divw(); t_divwu(); t_addc(); t_subfc(); t_slw(); t_srw(); t_sraw();
    t_misc(); t_fp(); t_mem();
    return 0;
}
