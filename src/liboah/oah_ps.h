/* oah_ps.h - power sources (IOPowerSources) for PowerPC apps, from macOS' own.
   10.6's PowerPC IOKit read the power sources from configd's store in its own format, which
   Mojave's does not have: a MacBook's battery was not listed (0 sources), so battery meters and
   "on battery" checks saw a desktop. The calls are answered natively: the info blob and the
   power-source refs handed to the guest are proxies (oah_kc.h), descriptions are guest copies
   (cached by content: Get returns an unowned dictionary). Change notifications are not delivered
   (the run loop source never fires). OAH_NO_POWERSOURCES=1 leaves it to the guest. */
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
CFStringRef IOPSGetProvidingPowerSourceType(CFTypeRef snapshot);   /* in 10.6's IOKit, declared from 10.7 */
static int oah_want_ps(void) { return !getenv("OAH_NO_POWERSOURCES"); }
static int oah_ps_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_PSLOG") ? 1 : 0; return v; }
/* an unowned guest copy of a native property list: one per distinct value, kept */
static unsigned int oah_ps_unowned(CFTypeRef v) {
    static struct { CFTypeRef n; unsigned int g; } c[64]; static int nc, next;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    unsigned int g; int i;
    if (!v) return 0;
    pthread_mutex_lock(&mu);
    for (i = 0; i < nc; i++) if (CFEqual(c[i].n, v)) { g = c[i].g; pthread_mutex_unlock(&mu); return g; }
    pthread_mutex_unlock(&mu);
    g = CFGetTypeID(v) == CFStringGetTypeID() ? oah_midi_gstr((CFStringRef)v) : oah_midi_gplist(v);
    if (!g) return 0;
    pthread_mutex_lock(&mu);
    if (nc < 64) i = nc++; else { i = next++ % 64; CFRelease(c[i].n); }   /* the oldest is replaced (its guest copy stays) */
    c[i].n = CFRetain(v); c[i].g = g;
    pthread_mutex_unlock(&mu);
    return g;
}
enum { PS_INFO, PS_LIST, PS_DESCR, PS_PROVIDING, PS_WARNING, PS_ADAPTER, PS_NOTIFY, PS_NULL, PS_N };
static const char *const g_ps_name[PS_N] = { "IOPSCopyPowerSourcesInfo", "IOPSCopyPowerSourcesList", "IOPSGetPowerSourceDescription",
    "IOPSGetProvidingPowerSourceType", "IOPSGetBatteryWarningLevel", "IOPSCopyExternalPowerAdapterDetails", "IOPSNotificationCreateRunLoopSource", "IOPS (private, NULL)" };
static void oah_ps_call(int which, unsigned int *R) {
    unsigned int r = 0;
    g_gt_skip = 1;
    switch (which) {
    case PS_INFO: { CFTypeRef i = IOPSCopyPowerSourcesInfo(); r = i ? oah_kc_wrap_own(i) : 0; if (i) CFRelease(i); break; }
    case PS_LIST: {
        static unsigned int fca, fcb;
        CFTypeRef i = oah_kc_unwrap(R[3]); CFArrayRef l = i ? IOPSCopyPowerSourcesList(i) : NULL;
        unsigned int *v, a[4]; CFIndex n, k;
        if (!fca) { fca = oah_guest_sym("CoreFoundation", "_CFArrayCreate"); fcb = oah_guest_sym("CoreFoundation", "_kCFTypeArrayCallBacks"); }
        if (!l || !fca || !fcb) { if (l) CFRelease(l); break; }
        n = CFArrayGetCount(l);
        v = (unsigned int *)calloc(n ? n : 1, 4);
        for (k = 0; k < n; k++) v[k] = OSSwapHostToBigInt32(oah_kc_wrap(CFArrayGetValueAtIndex(l, k)));
        a[0] = 0; a[1] = (unsigned int)(uintptr_t)v; a[2] = (unsigned int)n; a[3] = fcb;
        r = oah_callppc_n("ps_list", fca, 4, a);
        free(v); CFRelease(l);
        break; }
    case PS_DESCR: {
        CFTypeRef i = oah_kc_unwrap(R[3]), ps = oah_kc_unwrap(R[4]);
        CFDictionaryRef d = i && ps ? IOPSGetPowerSourceDescription(i, ps) : NULL;
        r = oah_ps_unowned(d);
        break; }
    case PS_PROVIDING: {
        CFTypeRef i = oah_kc_unwrap(R[3]);
        r = oah_ps_unowned(i ? IOPSGetProvidingPowerSourceType(i) : NULL);
        break; }
    case PS_WARNING: r = (unsigned int)IOPSGetBatteryWarningLevel(); break;
    case PS_ADAPTER: { CFDictionaryRef d = IOPSCopyExternalPowerAdapterDetails(); r = d ? oah_midi_gplist(d) : 0; if (d) CFRelease(d); break; }
    case PS_NOTIFY: r = oah_sc_dummy_source(); break;
    case PS_NULL: r = 0; break;   /* the private calls on the info blob: the guest's would read our proxy */
    }
    if (oah_ps_log()) fprintf(stderr, "[power] %s -> %08x\n", g_ps_name[which], r);
    g_gt_skip_ret = r;
}
#define OAH_PS_FIX(n) static void oah_fix_ps_##n(unsigned int *R) { oah_ps_call(PS_##n, R); }
OAH_PS_FIX(INFO) OAH_PS_FIX(LIST) OAH_PS_FIX(DESCR) OAH_PS_FIX(PROVIDING) OAH_PS_FIX(WARNING) OAH_PS_FIX(ADAPTER) OAH_PS_FIX(NOTIFY) OAH_PS_FIX(NULL)
