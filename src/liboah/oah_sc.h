/* oah_sc.h - the System Configuration dynamic store for PowerPC apps, from macOS' own.

   10.6's PowerPC SystemConfiguration could not reach Mojave's configd: SCDynamicStoreCreate
   returned NULL and SCDynamicStoreCopyComputerName failed with kSCStatusNoStoreServer (2002), so
   apps saw no computer name, no network state, no interfaces (network reachability worked; it
   does not use configd). The store calls are answered with the native ones: create (the store
   handed to the guest is a guest proxy object), copy value / multiple / key list, computer and
   local host name, console user. Notification keys are accepted and run loop sources are real
   guest sources that never fire: change notifications are not delivered.
   OAH_NO_SCSTORE=1 leaves it to the guest; OAH_SCLOG=1 logs. Uses oah_kc.h's proxies and
   oah_midi.h's guest-CF helpers. */
#include <SystemConfiguration/SystemConfiguration.h>

static int oah_sc_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_SCLOG") ? 1 : 0; return v; }
static int oah_want_scstore(void) { return !getenv("OAH_NO_SCSTORE"); }
/* the native store for a guest argument (NULL: a session store of ours) */
static SCDynamicStoreRef oah_sc_store(unsigned int g) {
    static SCDynamicStoreRef def;
    CFTypeRef n = oah_kc_unwrap(g);
    if (n) return (SCDynamicStoreRef)n;
    if (!def) def = SCDynamicStoreCreate(NULL, CFSTR("Rosettus"), NULL, NULL);
    return def;
}
/* a guest CFArray of CFStrings as a native array (NULL for NULL) */
static CFArrayRef oah_sc_strarray(unsigned int g) {
    static unsigned int fcnt, fget;
    CFMutableArrayRef a; unsigned int n, i, args[2];
    if (!g) return NULL;
    if (!fcnt) { fcnt = oah_guest_sym("CoreFoundation", "_CFArrayGetCount"); fget = oah_guest_sym("CoreFoundation", "_CFArrayGetValueAtIndex"); }
    if (!fcnt || !fget) return NULL;
    args[0] = g; n = oah_callppc_n("sc_count", fcnt, 1, args);
    a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (i = 0; i < n && i < 4096; i++) {
        CFStringRef s;
        args[0] = g; args[1] = i;
        if ((s = oah_midi_nstr(oah_callppc_n("sc_get", fget, 2, args)))) { CFArrayAppendValue(a, s); CFRelease(s); }
    }
    return a;
}
/* a guest run loop source that never fires (version 0, no callbacks) */
static unsigned int oah_sc_dummy_source(void) {
    static unsigned int fcs;
    static unsigned int ctx[10];   /* CFRunLoopSourceContext, all zero (version 0) */
    unsigned int a[3];
    if (!fcs) fcs = oah_guest_sym("CoreFoundation", "_CFRunLoopSourceCreate");
    if (!fcs) return 0;
    a[0] = 0; a[1] = 0; a[2] = (unsigned int)(uintptr_t)ctx;
    return oah_callppc_n("sc_source", fcs, 3, a);
}
enum { SC_CREATE, SC_CREATEOPT, SC_VALUE, SC_MULTIPLE, SC_KEYLIST, SC_COMPUTER, SC_LOCALHOST, SC_CONSOLE, SC_NOTIFYKEYS, SC_SOURCE,
       SC_ADDVALUE, SC_ADDTEMP, SC_SETVALUE, SC_REMOVE, SC_NOTIFYVALUE, SC_TOUCH, SC_SETMULTI, SC_NOTIFIED, SC_PROXIES, SC_LOCATION,
       SC_DHCP, SC_DISPATCH, SC_WATCH, SC_UNWATCH, SC_LOCK, SC_UNLOCK, SC_NOTIFYOTHER, SC_TYPEID, SC_N };
static const char *const g_sc_name[SC_N] = { "SCDynamicStoreCreate", "SCDynamicStoreCreateWithOptions", "SCDynamicStoreCopyValue",
    "SCDynamicStoreCopyMultiple", "SCDynamicStoreCopyKeyList", "SCDynamicStoreCopyComputerName", "SCDynamicStoreCopyLocalHostName",
    "SCDynamicStoreCopyConsoleUser", "SCDynamicStoreSetNotificationKeys", "SCDynamicStoreCreateRunLoopSource",
    "SCDynamicStoreAddValue", "SCDynamicStoreAddTemporaryValue", "SCDynamicStoreSetValue", "SCDynamicStoreRemoveValue",
    "SCDynamicStoreNotifyValue", "SCDynamicStoreTouchValue", "SCDynamicStoreSetMultiple", "SCDynamicStoreCopyNotifiedKeys",
    "SCDynamicStoreCopyProxies", "SCDynamicStoreCopyLocation", "SCDynamicStoreCopyDHCPInfo", "SCDynamicStoreSetDispatchQueue",
    "SCDynamicStoreAddWatchedKey", "SCDynamicStoreRemoveWatchedKey", "SCDynamicStoreLock", "SCDynamicStoreUnlock",
    "SCDynamicStoreNotify*", "SCDynamicStoreGetTypeID" };
CFPropertyListRef oah_hid_nplist_fwd(unsigned int g);   /* oah_hid.h */
/* the rest of the store calls: the guest's own would read our proxy as a store (fontd crashed in
   SCDynamicStoreSetDispatchQueue) */
static unsigned int oah_sc_more(int which, unsigned int *R) {
    SCDynamicStoreRef st = oah_sc_store(R[3]);
    CFStringRef k = NULL; CFPropertyListRef v = NULL; unsigned int r = 0;
    switch (which) {
    case SC_ADDVALUE: case SC_ADDTEMP: case SC_SETVALUE:
        k = oah_midi_nstr(R[4]); v = oah_hid_nplist_fwd(R[5]);
        if (k && v) r = which == SC_ADDVALUE ? SCDynamicStoreAddValue(st, k, v) : which == SC_ADDTEMP ? SCDynamicStoreAddTemporaryValue(st, k, v) : SCDynamicStoreSetValue(st, k, v);
        break;
    case SC_REMOVE: case SC_NOTIFYVALUE: case SC_TOUCH:
        k = oah_midi_nstr(R[4]);
        if (k) r = which == SC_REMOVE ? SCDynamicStoreRemoveValue(st, k) : SCDynamicStoreNotifyValue(st, k);
        break;
    case SC_SETMULTI: {
        CFPropertyListRef a = oah_hid_nplist_fwd(R[4]), b = oah_hid_nplist_fwd(R[5]), c = oah_hid_nplist_fwd(R[6]);
        r = SCDynamicStoreSetMultiple(st, (CFDictionaryRef)a, (CFArrayRef)b, (CFArrayRef)c);
        if (a) CFRelease(a); if (b) CFRelease(b); if (c) CFRelease(c);
        break; }
    case SC_NOTIFIED: { CFArrayRef e = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks); r = oah_midi_gplist(e); CFRelease(e); break; }   /* nothing is delivered */
    case SC_PROXIES: { CFDictionaryRef d = SCDynamicStoreCopyProxies(st); r = d ? oah_midi_gplist(d) : 0; if (d) CFRelease(d); break; }
    case SC_LOCATION: { CFStringRef l = SCDynamicStoreCopyLocation(st); r = l ? oah_midi_gstr(l) : 0; if (l) CFRelease(l); break; }
    case SC_DHCP: { CFDictionaryRef d; k = oah_midi_nstr(R[4]); d = SCDynamicStoreCopyDHCPInfo(st, k); r = d ? oah_midi_gplist(d) : 0; if (d) CFRelease(d); break; }
    case SC_DISPATCH: case SC_WATCH: case SC_UNWATCH: case SC_LOCK: case SC_UNLOCK: r = 1; break;   /* accepted; no notifications */
    case SC_NOTIFYOTHER: r = 0; break;                                                              /* notification ports, signals, waits: refused */
    case SC_TYPEID: { static unsigned int ftid; unsigned int a[1] = { 0 };
        if (!ftid) ftid = oah_guest_sym("CoreFoundation", "_CFDataGetTypeID");
        r = ftid ? oah_callppc_n("sc_typeid", ftid, 0, a) : 0; break; }
    }
    if (k) CFRelease(k);
    if (v) CFRelease(v);
    return r;
}
static void oah_sc_call(int which, unsigned int *R) {
    unsigned int r = 0;
    CFStringRef s = NULL;
    g_gt_skip = 1;
    switch (which) {
    case SC_CREATE: case SC_CREATEOPT: {   /* (alloc, name, [options,] callout, context) */
        SCDynamicStoreRef st;
        s = oah_midi_nstr(R[4]);
        st = SCDynamicStoreCreate(NULL, s ? s : CFSTR("PowerPC app"), NULL, NULL);
        r = st ? oah_kc_wrap_own(st) : 0;
        if (st) CFRelease(st);
        break; }
    case SC_VALUE: { CFPropertyListRef v; s = oah_midi_nstr(R[4]);
        v = s ? SCDynamicStoreCopyValue(oah_sc_store(R[3]), s) : NULL;
        r = v ? oah_midi_gplist(v) : 0; if (v) CFRelease(v); break; }
    case SC_MULTIPLE: { CFArrayRef k = oah_sc_strarray(R[4]), p = oah_sc_strarray(R[5]); CFDictionaryRef v;
        v = SCDynamicStoreCopyMultiple(oah_sc_store(R[3]), k, p);
        r = v ? oah_midi_gplist(v) : 0;
        if (v) CFRelease(v); if (k) CFRelease(k); if (p) CFRelease(p);
        break; }
    case SC_KEYLIST: { CFArrayRef v; s = oah_midi_nstr(R[4]);
        v = s ? SCDynamicStoreCopyKeyList(oah_sc_store(R[3]), s) : NULL;
        r = v ? oah_midi_gplist(v) : 0; if (v) CFRelease(v); break; }
    case SC_COMPUTER: { CFStringEncoding enc = 0; CFStringRef v = SCDynamicStoreCopyComputerName(oah_sc_store(R[3]), &enc);
        if (R[4] && oah_readable(R[4], 4)) *(unsigned int *)(uintptr_t)R[4] = OSSwapHostToBigInt32(enc);
        r = v ? oah_midi_gstr(v) : 0; if (v) CFRelease(v); break; }
    case SC_LOCALHOST: { CFStringRef v = SCDynamicStoreCopyLocalHostName(oah_sc_store(R[3]));
        r = v ? oah_midi_gstr(v) : 0; if (v) CFRelease(v); break; }
    case SC_CONSOLE: { uid_t u = 0; gid_t gg = 0; CFStringRef v = SCDynamicStoreCopyConsoleUser(oah_sc_store(R[3]), &u, &gg);
        if (R[4] && oah_readable(R[4], 4)) *(unsigned int *)(uintptr_t)R[4] = OSSwapHostToBigInt32((unsigned int)u);
        if (R[5] && oah_readable(R[5], 4)) *(unsigned int *)(uintptr_t)R[5] = OSSwapHostToBigInt32((unsigned int)gg);
        r = v ? oah_midi_gstr(v) : 0; if (v) CFRelease(v); break; }
    case SC_NOTIFYKEYS: r = 1; break;                                 /* accepted; nothing is delivered */
    case SC_SOURCE: r = oah_sc_dummy_source(); break;
    default: r = oah_sc_more(which, R); break;
    }
    if (s) CFRelease(s);
    if (oah_sc_log()) fprintf(stderr, "[scstore] %s -> %08x\n", g_sc_name[which], r);
    g_gt_skip_ret = r;
}
#define OAH_SC_FIX(n) static void oah_fix_sc_##n(unsigned int *R) { oah_sc_call(SC_##n, R); }
OAH_SC_FIX(CREATE) OAH_SC_FIX(CREATEOPT) OAH_SC_FIX(VALUE) OAH_SC_FIX(MULTIPLE) OAH_SC_FIX(KEYLIST) OAH_SC_FIX(COMPUTER)
OAH_SC_FIX(LOCALHOST) OAH_SC_FIX(CONSOLE) OAH_SC_FIX(NOTIFYKEYS) OAH_SC_FIX(SOURCE)
OAH_SC_FIX(ADDVALUE) OAH_SC_FIX(ADDTEMP) OAH_SC_FIX(SETVALUE) OAH_SC_FIX(REMOVE) OAH_SC_FIX(NOTIFYVALUE) OAH_SC_FIX(TOUCH)
OAH_SC_FIX(SETMULTI) OAH_SC_FIX(NOTIFIED) OAH_SC_FIX(PROXIES) OAH_SC_FIX(LOCATION) OAH_SC_FIX(DHCP) OAH_SC_FIX(DISPATCH)
OAH_SC_FIX(WATCH) OAH_SC_FIX(UNWATCH) OAH_SC_FIX(LOCK) OAH_SC_FIX(UNLOCK) OAH_SC_FIX(NOTIFYOTHER) OAH_SC_FIX(TYPEID)
