/* sys_misc: libc / libm / threads / Mach / Carbon odds and ends with deterministic answers.
   Build (10.6): gcc -arch ppc -O0 -o sys_misc sys_misc.c -framework CoreServices -framework Carbon */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <Carbon/Carbon.h>

static volatile long g_cnt; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *a) { int i; for (i = 0; i < 100000; i++) { pthread_mutex_lock(&mu); g_cnt++; pthread_mutex_unlock(&mu); } return a; }
static OSStatus mptask(void *a) { *(long *)a = 4242; return noErr; }
static int cmpi(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(void) {
    double xs[] = { 0.5, 1.0, 2.718281828, 10.0, -3.25, 1e-300, 1e300 };
    int i;
    for (i = 0; i < 7; i++)
        printf("libm %-10g sin %.17g exp %.17g log %.17g pow %.17g\n", xs[i], sin(xs[i]), exp(xs[i] > 700 ? 1 : xs[i]), log(fabs(xs[i])), pow(fabs(xs[i]), 0.37));
    printf("%-22s %.17g %.17g %.17g\n", "atan2/hypot/cbrt", atan2(1, -2), hypot(3e200, 4e200), cbrt(-27.5));
    printf("%-22s %ld %ld %g\n", "lrint/round", lrint(2.5), lrint(3.5), round(-2.5));
    printf("%-22s %.9g %d\n", "float ops", (float)(1.0f / 3.0f) * 3.0f, (int)(0.1 + 0.2 == 0.3));
    printf("%-22s %.17g %s\n", "strtod", strtod("1.7976931348623157e308", 0), (char *)(isnan(strtod("nan", 0)) ? "nan-ok" : "nan-bad"));
    { char b[64]; snprintf(b, sizeof b, "%08.3f|%-5d|%+e|%x|%s", 3.14159, 42, -1234.5, 0xbeef, "s"); printf("%-22s %s\n", "printf", b); }
    { int a[10] = { 9, 3, 7, 1, 8, 2, 6, 0, 5, 4 }; qsort(a, 10, sizeof(int), cmpi); printf("%-22s", "qsort"); for (i = 0; i < 10; i++) printf(" %d", a[i]); printf("\n"); }
    { struct tm tmv; time_t t = 1000000000; setenv("TZ", "UTC", 1); tzset(); gmtime_r(&t, &tmv); char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %a %j", &tmv); printf("%-22s %s\n", "strftime", b);
      setenv("TZ", "America/Chicago", 1); tzset(); localtime_r(&t, &tmv); printf("%-22s %d %ld %s\n", "localtime chicago", tmv.tm_hour, (long)tmv.tm_gmtoff, tmv.tm_zone); }
    { pthread_t th[4]; for (i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, 0); for (i = 0; i < 4; i++) pthread_join(th[i], 0); printf("%-22s %ld\n", "pthreads", g_cnt); }
    { MPTaskID tid; MPQueueID q; long out = 0; void *p1, *p2, *p3;
      MPCreateQueue(&q); OSStatus e = MPCreateTask(mptask, &out, 0, q, 0, 0, 0, &tid);
      MPWaitOnQueue(q, &p1, &p2, &p3, kDurationForever); printf("%-22s %d %ld\n", "MPTask", (int)e, out); }
    { struct utsname u; uname(&u); printf("%-22s %s %s\n", "uname", u.sysname, strncmp(u.release, "10.", 3) ? "other-release" : "10.x"); }
    { int mib[2] = { CTL_HW, HW_BYTEORDER }, v = 0; size_t l = sizeof v; sysctl(mib, 2, &v, &l, 0, 0); printf("%-22s %d\n", "hw.byteorder", v); }
    { char b[64]; size_t l = sizeof b; int r = sysctlbyname("hw.machine", b, &l, 0, 0); printf("%-22s %d %s\n", "hw.machine", r, r ? "" : b); }
    { int v = 0; size_t l = sizeof v; sysctlbyname("hw.cputype", &v, &l, 0, 0); printf("%-22s %d\n", "hw.cputype", v); }
    { int v = 0; size_t l = sizeof v; int r = sysctlbyname("hw.optional.altivec", &v, &l, 0, 0); printf("%-22s %d %d\n", "hw.optional.altivec", r, v); }
    { long v = 0; Gestalt(gestaltSysArchitecture, &v); printf("%-22s %ld\n", "Gestalt(sysa)", v);
      Gestalt(gestaltNativeCPUtype, &v); printf("%-22s %lx\n", "Gestalt(cput)", v);
      OSErr e = Gestalt(gestaltPowerPCProcessorFeatures, &v); printf("%-22s %d %lx\n", "Gestalt(ppcf)", (int)e, v);
      Gestalt(gestaltQuickTimeVersion, &v); printf("%-22s %lx\n", "Gestalt(qtim)", v >> 16);
      e = Gestalt('ram ', &v); printf("%-22s %d %d\n", "Gestalt(ram)", (int)e, v > 0); }
    { mach_port_t p; kern_return_t k = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p); printf("%-22s %d\n", "mach_port_allocate", k);
      vm_address_t a = 0; k = vm_allocate(mach_task_self(), &a, 65536, VM_FLAGS_ANYWHERE); printf("%-22s %d %d\n", "vm_allocate", k, (a & 0xfff) == 0); }
    { UInt32 r = 0x12345678; printf("%-22s %08lx %08lx\n", "endian", (unsigned long)r, (unsigned long)*(unsigned char *)&r); }
    { Str255 s; NumToString(-123456, s); printf("%-22s %.*s\n", "NumToString", s[0], s + 1); long n; StringToNum("\p987", &n); printf("%-22s %ld\n", "StringToNum", n); }
    { unsigned long dt = 0; DateTimeRec d = { 2004, 2, 29, 13, 45, 0, 0 }; DateToSeconds(&d, &dt); printf("%-22s %lu\n", "DateToSeconds", dt); }
    { UnsignedWide us; Microseconds(&us); AbsoluteTime t = UpTime(); printf("%-22s %d %d\n", "clocks", us.lo || us.hi, (t.lo | t.hi) != 0); }
    { char *p = NewPtrClear(100); printf("%-22s %d %ld\n", "NewPtrClear", MemError(), (long)GetPtrSize(p)); Handle h = NewHandle(10); SetHandleSize(h, 5000); printf("%-22s %d %ld\n", "SetHandleSize", MemError(), (long)GetHandleSize(h)); HLock(h); printf("%-22s %d\n", "HGetState-locked", (HGetState(h) & 0x80) != 0); }
    { TextEncoding te = CreateTextEncoding(kTextEncodingMacRoman, 0, 0); TECObjectRef tec; OSStatus e = TECCreateConverter(&tec, te, CreateTextEncoding(kTextEncodingUnicodeDefault, 0, kUnicodeUTF8Format));
      unsigned char in[] = { 0x8e, 'x', 0xd0 }; char out[32]; ByteCount ir = 0, ol = 0; TECConvertText(tec, in, 3, &ir, (TextPtr)out, sizeof out, &ol);
      printf("%-22s %d", "TEC macroman->utf8", (int)e); for (i = 0; i < (int)ol; i++) printf(" %02x", (unsigned char)out[i]); printf("\n"); }
    return 0;
}
