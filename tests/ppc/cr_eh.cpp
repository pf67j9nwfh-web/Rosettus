// cr_eh: condition-register fields 2-4 (non-volatile) across calls and C++ exception unwinding,
// and mtcrf with single-field masks.
// Snow Leopard Rosetta prints 288 for the catch block: 10.6's libunwind restores only cr2 from the saved
// CR word that GCC describes as "cr2" in its CFI, so cr3/cr4 kept the thrower's values (TextWrangler
// crashed on a landing pad that tested cr4). Rosettus patches libSystem to restore cr2-cr4, as libgcc's
// unwinder did; cr_eh.sl.txt holds that expected value (234), not the Snow Leopard output.
#include <stdio.h>
static unsigned getcr(void) { unsigned r; asm volatile("mfcr %0" : "=r"(r)); return r; }
__attribute__((noinline)) void clobber_and_maybe_throw(int k) {
    asm volatile("lis r11,0x0088\n\tori r11,r11,0x8000\n\tmtcrf 0x38,r11" ::: "r11", "cr2", "cr3", "cr4");
    if (k) throw k;
}
__attribute__((noinline)) unsigned call_plain(void) {
    asm volatile("lis r11,0x0023\n\tori r11,r11,0x4000\n\tmtcrf 0x38,r11" ::: "r11", "cr2", "cr3", "cr4");
    clobber_and_maybe_throw(0);
    return (getcr() >> 12) & 0xfff;
}
__attribute__((noinline)) unsigned call_throw(void) {
    unsigned v = 0;
    asm volatile("lis r11,0x0023\n\tori r11,r11,0x4000\n\tmtcrf 0x38,r11" ::: "r11", "cr2", "cr3", "cr4");
    try { clobber_and_maybe_throw(1); }
    catch (int) { v = (getcr() >> 12) & 0xfff; }
    return v;
}
int main(void) {
    static const unsigned masks[] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x38, 0xff, 0x0c };
    unsigned i;
    for (i = 0; i < sizeof masks / sizeof masks[0]; i++) {
        unsigned r;
        asm volatile("li r0,0\n\tmtcrf 0xff,r0" ::: "r0", "cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7");
        switch (masks[i]) {
#define M(m) case m: asm volatile("lis r11,0x1234\n\tori r11,r11,0x5678\n\tmtcrf " #m ",r11\n\tmfcr %0" : "=r"(r) :: "r11","cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7"); break;
        M(0x80) M(0x40) M(0x20) M(0x10) M(0x08) M(0x04) M(0x02) M(0x01) M(0x38) M(0xff) M(0x0c)
        }
        printf("mtcrf %02x -> cr %08x\n", masks[i], r);
    }
    printf("cr2-4 after a plain call: %03x (expect 234)\n", call_plain());
    printf("cr2-4 in a catch block:   %03x (expect 234)\n", call_throw());
    return 0;
}
