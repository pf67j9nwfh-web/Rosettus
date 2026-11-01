/* oah_hidcom.h - the old IOHIDLib plug-in interface (IOHIDDeviceInterface122, IOHIDQueueInterface,
   IOHIDOutputTransactionInterface) for PowerPC apps, on top of oah_hid.h's native calls.

   Games and HID Utilities before 10.5 open a device with IOCreatePlugInInterfaceForService(service,
   kIOHIDDeviceUserClientTypeID, kIOCFPlugInInterfaceID, ...) and QueryInterface for
   kIOHIDDeviceInterfaceID(121/122). The guest's IOHIDLib plug-in cannot open the device (see
   oah_hid.h). For that plug-in type the call now returns COM objects of ours: their vtables point
   at three-word guest stubs ("li r11,method ; twi ; blr") that all trap into oah_hidcom_call, which
   answers with the native IOHIDDevice / IOHIDQueue calls. Elements are found by cookie. Callbacks
   (removal, queue callouts, interrupt reports, set/get with a callback) go through the run-loop
   source the app gets from createAsyncEventSource and adds to its own run loop: the source's
   schedule routine (another stub) tells us the loop. OAH_NO_HIDCOM=1 leaves the plug-in to the guest. */
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
static int oah_want_hidcom(void) { return oah_want_hid() && !getenv("OAH_NO_HIDCOM"); }
static int g_hidcom_idx = -1, g_gq_sched_idx = -1, g_gq_cancel_idx = -1;

/* the run-loop source for a queue of ours, with schedule / cancel routines (an app adds it itself) */
static unsigned int oah_gq_stub(int idx) {
    unsigned int *t;
    if (idx < 0 || !(t = (unsigned int *)oah_guest_code_alloc(8))) return 0;
    t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)idx));
    t[1] = OSSwapHostToBigInt32(0x4e800020);
    return (unsigned int)(uintptr_t)t;
}
static unsigned int oah_gq_source(oah_gq *q) {
    static unsigned int perf, sched, cancel;
    unsigned int *ctx;
    if (q->src) return q->src;
    if (!oah_fse_gcf()) return 0;
    if (!perf) { perf = oah_gq_stub(g_gq_perf_idx); sched = oah_gq_stub(g_gq_sched_idx); cancel = oah_gq_stub(g_gq_cancel_idx); }
    if (!perf || !(ctx = (unsigned int *)calloc(10, 4))) return 0;
    ctx[1] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)q);
    ctx[7] = OSSwapHostToBigInt32(sched); ctx[8] = OSSwapHostToBigInt32(cancel); ctx[9] = OSSwapHostToBigInt32(perf);
    q->src = oah_gf(GF_SRCCREATE, 3, 0, 0, (unsigned int)(uintptr_t)ctx);
    free(ctx);
    oah_gq_start_thread();
    return q->src;
}
/* schedule(info, runLoop, mode) / cancel(info, runLoop, mode): CF calls them as the app adds or removes the source */
static void oah_fix_gq_sched(unsigned int *R) {
    oah_gq *q = (oah_gq *)(uintptr_t)R[3]; int k, have = 0;
    if (q && q->magic == OAH_GQ_MAGIC && R[4]) {
        pthread_mutex_lock(&g_gq_mu);
        for (k = 0; k < q->nrl; k++) if (q->rl[k] == R[4]) have = 1;
        if (!have && q->nrl < OAH_GQ_RL) q->rl[q->nrl++] = R[4];
        if (q->head) { q->signaled = 0; g_gq_wake = 1; pthread_cond_signal(&g_gq_cv); }
        pthread_mutex_unlock(&g_gq_mu);
    }
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static void oah_fix_gq_cancel(unsigned int *R) {
    oah_gq *q = (oah_gq *)(uintptr_t)R[3]; int k;
    if (q && q->magic == OAH_GQ_MAGIC) {
        pthread_mutex_lock(&g_gq_mu);
        for (k = 0; k < q->nrl; k++) if (q->rl[k] == R[4]) { q->rl[k] = q->rl[--q->nrl]; break; }
        pthread_mutex_unlock(&g_gq_mu);
    }
    g_gt_skip = 1; g_gt_skip_ret = 0;
}

/* ---- the objects ---- */
#define OAH_HC_MAGIC 0x48434f4du
enum { HC_PLUGIN, HC_DEVICE, HC_QUEUE, HC_TRANS };
struct oah_hc;
typedef struct oah_hc {
    unsigned int vtbl;                         /* big-endian: what the guest's "interface pointer" points at */
    unsigned int magic; int kind, refs;
    struct oah_hc *dev;                        /* a plug-in's, queue's or transaction's device object */
    IOHIDDeviceRef nd; IOHIDQueueRef nq;
    CFMutableDictionaryRef els;                /* cookie -> element (device) */
    oah_gq *q;
    unsigned int remfn, remtarget, remrefcon;  /* removal callback */
    unsigned int cofn, cotarget, corefcon;     /* queue event callout */
    unsigned int repfn, reptarget, reprefcon, repgbuf, repglen; unsigned char *repbuf;
    mach_port_t port;
    CFMutableDictionaryRef tvals, tdefs;       /* transaction: cookie -> value, cookie -> default */
} oah_hc;
static unsigned int g_hc_vt[4];                /* vtables (guest memory, big-endian) per kind */
static unsigned int oah_hc_vtable(int kind) {
    static const int n[4] = { 8, 22, 18, 19 };
    unsigned int *vt, *stubs; int i;
    if (g_hc_vt[kind]) return g_hc_vt[kind];
    if (g_hidcom_idx < 0 || !(stubs = (unsigned int *)oah_guest_code_alloc(12 * n[kind]))) return 0;
    vt = (unsigned int *)calloc(n[kind], 4);
    for (i = 1; i < n[kind]; i++) {
        unsigned int id = (unsigned int)(kind * 32 + i), *s = stubs + 3 * i;
        if (kind == HC_PLUGIN && i == 4) { vt[i] = OSSwapHostToBigInt32(0x00010000u); continue; }   /* version 1, revision 0 */
        s[0] = OSSwapHostToBigInt32(0x39600000u | id);                     /* li r11,id */
        s[1] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_hidcom_idx));
        s[2] = OSSwapHostToBigInt32(0x4e800020);
        vt[i] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)s);
    }
    g_hc_vt[kind] = (unsigned int)(uintptr_t)vt;
    return g_hc_vt[kind];
}
static oah_hc *oah_hc_new(int kind, oah_hc *dev) {
    oah_hc *o; unsigned int vt = oah_hc_vtable(kind);
    if (!vt || !(o = (oah_hc *)calloc(1, sizeof *o))) return NULL;
    o->vtbl = OSSwapHostToBigInt32(vt); o->magic = OAH_HC_MAGIC; o->kind = kind; o->refs = 1; o->dev = dev;
    if (dev) { o->nd = dev->nd; dev->refs++; }
    o->q = oah_gq_new();
    return o;
}
static oah_hc *oah_hc_get(unsigned int g) {
    oah_hc *o = (oah_hc *)(uintptr_t)g;
    return g && oah_readable(g, sizeof *o) && o->magic == OAH_HC_MAGIC ? o : NULL;
}
static IOHIDElementRef oah_hc_elem(oah_hc *d, unsigned int cookie) {
    if (!d || !d->nd) return NULL;
    if (!d->els) {
        CFArrayRef l = IOHIDDeviceCopyMatchingElements(d->nd, NULL, kIOHIDOptionsTypeNone); CFIndex i;
        d->els = CFDictionaryCreateMutable(NULL, 0, NULL, &kCFTypeDictionaryValueCallBacks);
        for (i = 0; l && i < CFArrayGetCount(l); i++) {
            IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(l, i);
            CFDictionarySetValue(d->els, (const void *)(uintptr_t)IOHIDElementGetCookie(e), e);
        }
        if (l) CFRelease(l);
    }
    return (IOHIDElementRef)CFDictionaryGetValue(d->els, (const void *)(uintptr_t)cookie);
}
/* an IOHIDEventStruct (big-endian) from a native value; long values in guest-malloc memory */
static void oah_hc_event(unsigned int ev, IOHIDValueRef v) {
    static unsigned int fmalloc;
    unsigned int *w = (unsigned int *)(uintptr_t)ev; IOHIDElementRef e; UInt64 ts; CFIndex len;
    if (!ev || !oah_readable(ev, 28) || !v) return;
    e = IOHIDValueGetElement(v); ts = IOHIDValueGetTimeStamp(v); len = IOHIDValueGetLength(v);
    w[0] = OSSwapHostToBigInt32((unsigned int)IOHIDElementGetType(e));
    w[1] = OSSwapHostToBigInt32((unsigned int)IOHIDElementGetCookie(e));
    w[2] = OSSwapHostToBigInt32((unsigned int)IOHIDValueGetIntegerValue(v));
    w[3] = OSSwapHostToBigInt32((unsigned int)(ts >> 32)); w[4] = OSSwapHostToBigInt32((unsigned int)ts);
    w[5] = 0; w[6] = 0;
    if (len > 4) {   /* the app frees longValue with free(): the guest's malloc */
        unsigned int a[1], p;
        if (!fmalloc) fmalloc = oah_guest_sym("libSystem", "_malloc");
        a[0] = (unsigned int)len;
        if (fmalloc && (p = oah_callppc_n("hc_malloc", fmalloc, 1, a))) {
            memcpy((void *)(uintptr_t)p, IOHIDValueGetBytePtr(v), (size_t)len);
            w[5] = OSSwapHostToBigInt32((unsigned int)len); w[6] = OSSwapHostToBigInt32(p);
        }
    }
}
/* a native value for an element from a guest IOHIDEventStruct */
static IOHIDValueRef oah_hc_nvalue(IOHIDElementRef e, unsigned int ev) {
    unsigned int *w = (unsigned int *)(uintptr_t)ev, n, p;
    if (!e || !ev || !oah_readable(ev, 28)) return NULL;
    n = OSSwapBigToHostInt32(w[5]); p = OSSwapBigToHostInt32(w[6]);
    if (n && p && n < (1u << 20) && oah_readable(p, n)) return IOHIDValueCreateWithBytes(NULL, e, 0, (const uint8_t *)(uintptr_t)p, (CFIndex)n);
    return IOHIDValueCreateWithIntegerValue(NULL, e, 0, (CFIndex)(int)OSSwapBigToHostInt32(w[2]));
}
/* an element as the old interface describes it (copyMatchingElements) */
static CFDictionaryRef oah_hc_eldict(IOHIDElementRef e) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    struct { CFStringRef k; long v; } n[] = {
        { CFSTR(kIOHIDElementCookieKey), (long)IOHIDElementGetCookie(e) }, { CFSTR(kIOHIDElementTypeKey), IOHIDElementGetType(e) },
        { CFSTR(kIOHIDElementUsagePageKey), (long)IOHIDElementGetUsagePage(e) }, { CFSTR(kIOHIDElementUsageKey), (long)IOHIDElementGetUsage(e) },
        { CFSTR(kIOHIDElementMinKey), IOHIDElementGetLogicalMin(e) }, { CFSTR(kIOHIDElementMaxKey), IOHIDElementGetLogicalMax(e) },
        { CFSTR(kIOHIDElementScaledMinKey), IOHIDElementGetPhysicalMin(e) }, { CFSTR(kIOHIDElementScaledMaxKey), IOHIDElementGetPhysicalMax(e) },
        { CFSTR(kIOHIDElementSizeKey), (long)(IOHIDElementGetReportSize(e) * IOHIDElementGetReportCount(e)) },
        { CFSTR(kIOHIDElementReportSizeKey), (long)IOHIDElementGetReportSize(e) }, { CFSTR(kIOHIDElementReportCountKey), (long)IOHIDElementGetReportCount(e) },
        { CFSTR(kIOHIDElementReportIDKey), (long)IOHIDElementGetReportID(e) }, { CFSTR(kIOHIDElementUnitKey), (long)IOHIDElementGetUnit(e) },
        { CFSTR(kIOHIDElementUnitExponentKey), (long)IOHIDElementGetUnitExponent(e) },
        { CFSTR(kIOHIDElementCollectionTypeKey), IOHIDElementGetType(e) == kIOHIDElementTypeCollection ? (long)IOHIDElementGetCollectionType(e) : -1 } };
    struct { CFStringRef k; Boolean v; } b[] = {
        { CFSTR(kIOHIDElementIsArrayKey), IOHIDElementIsArray(e) }, { CFSTR(kIOHIDElementIsRelativeKey), IOHIDElementIsRelative(e) },
        { CFSTR(kIOHIDElementIsWrappingKey), IOHIDElementIsWrapping(e) }, { CFSTR(kIOHIDElementIsNonLinearKey), IOHIDElementIsNonLinear(e) },
        { CFSTR(kIOHIDElementHasPreferredStateKey), IOHIDElementHasPreferredState(e) }, { CFSTR(kIOHIDElementHasNullStateKey), IOHIDElementHasNullState(e) } };
    unsigned int i; CFStringRef nm = IOHIDElementGetName(e);
    for (i = 0; i < sizeof n / sizeof *n; i++) {
        CFNumberRef x;
        if (n[i].k == CFSTR(kIOHIDElementCollectionTypeKey) && n[i].v < 0) continue;
        x = CFNumberCreate(NULL, kCFNumberLongType, &n[i].v); CFDictionarySetValue(d, n[i].k, x); CFRelease(x);
    }
    for (i = 0; i < sizeof b / sizeof *b; i++) CFDictionarySetValue(d, b[i].k, b[i].v ? kCFBooleanTrue : kCFBooleanFalse);
    if (nm) CFDictionarySetValue(d, CFSTR(kIOHIDElementNameKey), nm);
    return d;
}
/* native callbacks: queue the call for the guest on the object's source */
static void oah_hc_cb_removal(void *c, IOReturn r, void *sender) {
    oah_hc *o = (oah_hc *)c; oah_gq_item *it;
    (void)sender;
    if (!o->remfn) return;
    it = oah_hid_item(GQ_PLAIN, o->remfn, 4, o->remtarget, (unsigned int)r, o->remrefcon); it->a[3] = (unsigned int)(uintptr_t)o;
    oah_gq_post(o->q, it);
}
static void oah_hc_cb_avail(void *c, IOReturn r, void *sender) {
    oah_hc *o = (oah_hc *)c; oah_gq_item *it;
    (void)sender;
    if (!o->cofn) return;
    it = oah_hid_item(GQ_PLAIN, o->cofn, 4, o->cotarget, (unsigned int)r, o->corefcon); it->a[3] = (unsigned int)(uintptr_t)o;
    oah_gq_post(o->q, it);
}
static void oah_hc_cb_report(void *c, IOReturn r, void *sender, IOHIDReportType type, uint32_t id, uint8_t *rep, CFIndex len) {
    oah_hc *o = (oah_hc *)c; oah_gq_item *it;
    (void)sender; (void)type; (void)id;
    if (!o->repfn || len < 0) return;
    /* IOHIDReportCallbackFunction(target, result, refcon, sender, bufferSize): the report goes into the app's buffer */
    it = oah_hid_item(GQ_REPORT, o->repfn, 5, o->reptarget, (unsigned int)r, o->reprefcon);
    it->a[3] = (unsigned int)(uintptr_t)o;
    it->buf = (unsigned char *)malloc(len ? (size_t)len : 1); if (len) memcpy(it->buf, rep, (size_t)len);
    it->len = (unsigned int)len; it->gbuf = o->repgbuf; it->glen = o->repglen;
    it->kind = GQ_PLAIN;   /* copied here: the perform routine's report handling is for the 7-argument form */
    if (it->gbuf && oah_readable(it->gbuf, 1)) { unsigned int n = it->len < it->glen ? it->len : it->glen; memcpy((void *)(uintptr_t)it->gbuf, it->buf, n); it->a[4] = n; }
    else it->a[4] = it->len;
    oah_gq_post(o->q, it);
}
static void oah_hc_native_schedule(oah_hc *o) {
    CFRunLoopRef rl = oah_hid_rl();
    static CFMutableSetRef done;
    if (!rl) return;
    if (!done) done = CFSetCreateMutable(NULL, 0, NULL);
    if (o->nq && !CFSetContainsValue(done, o->nq)) { CFSetAddValue(done, o->nq); IOHIDQueueScheduleWithRunLoop(o->nq, rl, kCFRunLoopDefaultMode); }
    if (o->nd && !CFSetContainsValue(done, o->nd)) { CFSetAddValue(done, o->nd); IOHIDDeviceScheduleWithRunLoop(o->nd, rl, kCFRunLoopDefaultMode); }
}
static int oah_hc_uuid(const unsigned char *b, CFUUIDRef u) { CFUUIDBytes x = CFUUIDGetUUIDBytes(u); return !memcmp(b, &x, 16); }

/* ---- the methods: r11 = kind * 32 + vtable slot, r3 = self ---- */
static void oah_fix_hidcom(unsigned int *R) {
    unsigned int id = R[11], r = kIOReturnSuccess;
    int kind = (int)(id / 32), m = (int)(id % 32);
    oah_hc *o = oah_hc_get(R[3]), *d;
    if (!o) { g_gt_skip = 1; g_gt_skip_ret = kIOReturnBadArgument; return; }
    d = o->kind == HC_DEVICE ? o : o->dev;
    if (m == 1) {   /* QueryInterface(self, iid (16 bytes by value: r4-r7), void **ppv (r8)) */
        unsigned char b[16]; unsigned int *ppv = (unsigned int *)(uintptr_t)R[8]; oah_hc *x = NULL;
        int k; for (k = 0; k < 4; k++) { unsigned int w = OSSwapHostToBigInt32(R[4 + k]); memcpy(b + 4 * k, &w, 4); }
        if (oah_hc_uuid(b, CFUUIDGetConstantUUIDWithBytes(NULL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46))) x = o;   /* IUnknown */
        else if (kind == HC_PLUGIN && oah_hc_uuid(b, kIOCFPlugInInterfaceID)) x = o;
        else if ((kind == HC_PLUGIN || kind == HC_DEVICE) && (oah_hc_uuid(b, kIOHIDDeviceInterfaceID) || oah_hc_uuid(b, kIOHIDDeviceInterfaceID121) || oah_hc_uuid(b, kIOHIDDeviceInterfaceID122))) {
            if (kind == HC_DEVICE) x = o;
            else if ((x = oah_hc_new(HC_DEVICE, NULL))) { x->nd = o->nd; CFRetain(x->nd); }
        }
        else if (kind == HC_QUEUE && oah_hc_uuid(b, kIOHIDQueueInterfaceID)) x = o;
        else if (kind == HC_TRANS && oah_hc_uuid(b, kIOHIDOutputTransactionInterfaceID)) x = o;
        if (x && x == o) o->refs++;
        if (ppv && oah_readable(R[8], 4)) *ppv = OSSwapHostToBigInt32((unsigned int)(uintptr_t)x);
        r = x ? 0 : 0x80000004u;   /* E_NOINTERFACE */
        if (oah_hid_log()) fprintf(stderr, "[hidcom] QueryInterface(%d) %02x%02x%02x%02x -> %p\n", kind, b[0], b[1], b[2], b[3], (void *)x);
    } else if (m == 2) r = (unsigned int)++o->refs;                      /* AddRef */
    else if (m == 3) {                                                    /* Release */
        r = (unsigned int)(o->refs > 0 ? --o->refs : 0);
        if (!o->refs) {
            if (o->kind == HC_QUEUE && o->nq) { IOHIDQueueRegisterValueAvailableCallback(o->nq, NULL, NULL); IOHIDQueueStop(o->nq); }
            if (o->kind == HC_DEVICE && o->nd) { IOHIDDeviceRegisterRemovalCallback(o->nd, NULL, NULL); if (o->repbuf) IOHIDDeviceRegisterInputReportCallback(o->nd, o->repbuf, (CFIndex)o->repglen, NULL, NULL); }
            o->remfn = o->cofn = o->repfn = 0;   /* the struct stays: a late callback may still name it */
        }
    } else if (kind == HC_PLUGIN) r = 0;                                  /* Probe, Start, Stop */
    else if (m == 4 || m == 5) {                                          /* createAsyncEventSource(self, &src) / getAsyncEventSource(self) */
        unsigned int src = oah_gq_source(o->q);
        oah_hc_native_schedule(o);
        if (m == 4) {
            unsigned int a[1]; static unsigned int frt;
            if (!frt) frt = oah_guest_sym("CoreFoundation", "_CFRetain");
            if (src && frt) { a[0] = src; oah_callppc_n("hc_src", frt, 1, a); }   /* the caller's own */
            if (R[4] && oah_readable(R[4], 4)) *(unsigned int *)(uintptr_t)R[4] = OSSwapHostToBigInt32(src);
            r = src ? 0 : kIOReturnNoResources;
        } else r = src;
    } else if (m == 6 || m == 7) {                                        /* createAsyncPort(self, &port) / getAsyncPort(self) */
        if (!o->port) mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &o->port);
        if (m == 6) { if (R[4] && oah_readable(R[4], 4)) *(unsigned int *)(uintptr_t)R[4] = OSSwapHostToBigInt32(o->port); r = 0; }
        else r = o->port;
    } else if (kind == HC_DEVICE) {
        switch (m) {
        case 8: r = o->nd ? IOHIDDeviceOpen(o->nd, (R[4] & kIOHIDOptionsTypeSeizeDevice) ? kIOHIDOptionsTypeSeizeDevice : kIOHIDOptionsTypeNone) : kIOReturnNotOpen;
            oah_hc_native_schedule(o); break;
        case 9: r = o->nd ? IOHIDDeviceClose(o->nd, kIOHIDOptionsTypeNone) : kIOReturnNotOpen; break;
        case 10: o->remfn = R[4]; o->remtarget = R[5]; o->remrefcon = R[6];   /* setRemovalCallback(self, cb, target, refcon) */
            oah_hc_native_schedule(o);
            IOHIDDeviceRegisterRemovalCallback(o->nd, R[4] ? oah_hc_cb_removal : NULL, o); break;
        case 11: case 13: {   /* getElementValue(self, cookie, event*) / queryElementValue(self, cookie, event*, timeout, cb, target, refcon) */
            IOHIDElementRef e = oah_hc_elem(o, R[4]); IOHIDValueRef v = NULL;
            r = !e ? kIOReturnBadArgument : IOHIDDeviceGetValue(o->nd, e, &v);
            if (!r && v) oah_hc_event(R[5], v);
            if (m == 13 && R[7]) {   /* IOHIDElementCallbackFunction(target, result, refcon, sender, cookie) */
                oah_gq_item *it = oah_hid_item(GQ_PLAIN, R[7], 5, R[8], r, R[9]); it->a[3] = R[3]; it->a[4] = R[4];
                oah_gq_post(o->q, it);
            }
            break; }
        case 12: {   /* setElementValue(self, cookie, event*, timeout, cb, target, refcon) */
            IOHIDElementRef e = oah_hc_elem(o, R[4]); IOHIDValueRef v = oah_hc_nvalue(e, R[5]);
            r = e && v ? IOHIDDeviceSetValue(o->nd, e, v) : kIOReturnBadArgument;
            if (v) CFRelease(v);
            if (R[7]) { oah_gq_item *it = oah_hid_item(GQ_PLAIN, R[7], 5, R[8], r, R[9]); it->a[3] = R[3]; it->a[4] = R[4]; oah_gq_post(o->q, it); }
            break; }
        case 14: case 15: r = 0; break;                                   /* start/stopAllQueues: each queue starts itself */
        case 16: case 17: {                                               /* allocQueue / allocOutputTransaction */
            oah_hc *x = oah_hc_new(m == 16 ? HC_QUEUE : HC_TRANS, o);
            r = (unsigned int)(uintptr_t)x; break; }
        case 18: {   /* setReport(self, type, id, buf, size, timeout, cb, target, refcon (stack)) */
            unsigned int cb = R[9], tg = R[10], rc = oah_kc_arg(R, 8);
            r = o->nd && (!R[7] || oah_readable(R[6], R[7])) ? IOHIDDeviceSetReport(o->nd, (IOHIDReportType)R[4], (CFIndex)R[5], (const uint8_t *)(uintptr_t)R[6], (CFIndex)R[7]) : kIOReturnBadArgument;
            if (cb) { oah_gq_item *it = oah_hid_item(GQ_PLAIN, cb, 5, tg, r, rc); it->a[3] = R[3]; it->a[4] = R[7]; oah_gq_post(o->q, it); }
            break; }
        case 19: {   /* getReport(self, type, id, buf, &size, timeout, cb, target, refcon (stack)) */
            unsigned int cb = R[9], tg = R[10], rc = oah_kc_arg(R, 8);
            CFIndex n = R[7] && oah_readable(R[7], 4) ? (CFIndex)OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)R[7]) : 0;
            r = o->nd && n > 0 && oah_readable(R[6], (unsigned int)n) ? IOHIDDeviceGetReport(o->nd, (IOHIDReportType)R[4], (CFIndex)R[5], (uint8_t *)(uintptr_t)R[6], &n) : kIOReturnBadArgument;
            if (!r) *(unsigned int *)(uintptr_t)R[7] = OSSwapHostToBigInt32((unsigned int)n);
            if (cb) { oah_gq_item *it = oah_hid_item(GQ_PLAIN, cb, 5, tg, r, rc); it->a[3] = R[3]; it->a[4] = (unsigned int)n; oah_gq_post(o->q, it); }
            break; }
        case 20: {   /* copyMatchingElements(self, matching, CFArrayRef *elements) */
            CFDictionaryRef mt = (CFDictionaryRef)oah_hid_nplist(R[4]);
            CFArrayRef l = o->nd ? IOHIDDeviceCopyMatchingElements(o->nd, mt, kIOHIDOptionsTypeNone) : NULL;
            CFMutableArrayRef out = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks); CFIndex i; unsigned int g;
            for (i = 0; l && i < CFArrayGetCount(l); i++) { CFDictionaryRef x = oah_hc_eldict((IOHIDElementRef)CFArrayGetValueAtIndex(l, i)); CFArrayAppendValue(out, x); CFRelease(x); }
            g = oah_midi_gplist(out);
            if (R[5] && oah_readable(R[5], 4)) *(unsigned int *)(uintptr_t)R[5] = OSSwapHostToBigInt32(g);
            r = g ? 0 : kIOReturnNoMemory;
            CFRelease(out); if (l) CFRelease(l); if (mt) CFRelease(mt);
            break; }
        case 21: {   /* setInterruptReportHandlerCallback(self, buf, size, cb, target, refcon) */
            o->repgbuf = R[4]; o->repglen = R[5]; o->repfn = R[6]; o->reptarget = R[7]; o->reprefcon = R[8];
            oah_hc_native_schedule(o);
            if (R[6] && R[5] && R[5] < (1u << 20)) {
                free(o->repbuf); o->repbuf = (unsigned char *)malloc(R[5]);
                IOHIDDeviceRegisterInputReportCallback(o->nd, o->repbuf, (CFIndex)R[5], oah_hc_cb_report, o);
            }
            break; }
        default: r = kIOReturnUnsupported;
        }
    } else if (kind == HC_QUEUE) {
        switch (m) {
        case 8: if (!o->nq && o->nd) o->nq = IOHIDQueueCreate(NULL, o->nd, R[5] ? (CFIndex)R[5] : 8, kIOHIDOptionsTypeNone);   /* create(self, flags, depth) */
            r = o->nq ? 0 : kIOReturnNoMemory; break;
        case 9: if (o->nq) { IOHIDQueueStop(o->nq); IOHIDQueueRegisterValueAvailableCallback(o->nq, NULL, NULL); CFRelease(o->nq); o->nq = NULL; } break;   /* dispose */
        case 10: case 11: case 12: {   /* addElement(self, cookie, flags) / removeElement / hasElement */
            IOHIDElementRef e = oah_hc_elem(d, R[4]);
            if (!o->nq || !e) { r = m == 12 ? 0 : kIOReturnBadArgument; break; }
            if (m == 10) IOHIDQueueAddElement(o->nq, e); else if (m == 11) IOHIDQueueRemoveElement(o->nq, e); else r = IOHIDQueueContainsElement(o->nq, e);
            break; }
        case 13: if (o->nq) IOHIDQueueStart(o->nq); else r = kIOReturnNotOpen; break;
        case 14: if (o->nq) IOHIDQueueStop(o->nq); break;
        case 15: {   /* getNextEvent(self, event*, maxTime (r5:r6), timeoutMS) */
            IOHIDValueRef v = o->nq ? IOHIDQueueCopyNextValueWithTimeout(o->nq, 0) : NULL;
            if (v) { oah_hc_event(R[4], v); CFRelease(v); r = 0; } else r = kIOReturnUnderrun;
            break; }
        case 16: o->cofn = R[4]; o->cotarget = R[5]; o->corefcon = R[6];   /* setEventCallout(self, cb, target, refcon) */
            if (o->nq) { oah_hc_native_schedule(o); IOHIDQueueRegisterValueAvailableCallback(o->nq, R[4] ? oah_hc_cb_avail : NULL, o); }
            break;
        case 17: {   /* getEventCallout(self, &cb, &target, &refcon) */
            unsigned int v[3]; int k; v[0] = o->cofn; v[1] = o->cotarget; v[2] = o->corefcon;
            for (k = 0; k < 3; k++) if (R[4 + k] && oah_readable(R[4 + k], 4)) *(unsigned int *)(uintptr_t)R[4 + k] = OSSwapHostToBigInt32(v[k]);
            break; }
        default: r = kIOReturnUnsupported;
        }
    } else {   /* output transactions: values kept here, set one by one at commit */
        if (!o->tvals) { o->tvals = CFDictionaryCreateMutable(NULL, 0, NULL, &kCFTypeDictionaryValueCallBacks); o->tdefs = CFDictionaryCreateMutable(NULL, 0, NULL, &kCFTypeDictionaryValueCallBacks); }
        switch (m) {
        case 8: case 9: case 18: CFDictionaryRemoveAllValues(o->tvals); if (m == 9) CFDictionaryRemoveAllValues(o->tdefs); break;   /* create, dispose, clear */
        case 10: case 11: case 12: {   /* addElement(self, cookie) / removeElement / hasElement */
            IOHIDElementRef e = oah_hc_elem(d, R[4]);
            if (m == 10) { if (!e) r = kIOReturnBadArgument; else if (!CFDictionaryContainsKey(o->tdefs, (const void *)(uintptr_t)R[4])) CFDictionarySetValue(o->tdefs, (const void *)(uintptr_t)R[4], e); }
            else if (m == 11) { CFDictionaryRemoveValue(o->tdefs, (const void *)(uintptr_t)R[4]); CFDictionaryRemoveValue(o->tvals, (const void *)(uintptr_t)R[4]); }
            else r = CFDictionaryContainsKey(o->tdefs, (const void *)(uintptr_t)R[4]);
            break; }
        case 13: case 15: {   /* setElementDefault / setElementValue(self, cookie, event*) */
            IOHIDElementRef e = oah_hc_elem(d, R[4]); IOHIDValueRef v = oah_hc_nvalue(e, R[5]);
            if (!v) { r = kIOReturnBadArgument; break; }
            CFDictionarySetValue(m == 13 ? o->tdefs : o->tvals, (const void *)(uintptr_t)R[4], v); CFRelease(v);
            break; }
        case 14: case 16: {   /* getElementDefault / getElementValue(self, cookie, event*) */
            CFTypeRef v = CFDictionaryGetValue(m == 14 ? o->tdefs : o->tvals, (const void *)(uintptr_t)R[4]);
            if (v && CFGetTypeID(v) == IOHIDValueGetTypeID()) oah_hc_event(R[5], (IOHIDValueRef)v); else r = kIOReturnBadArgument;
            break; }
        case 17: {   /* commit(self, timeout, cb, target, refcon) */
            CFIndex n = CFDictionaryGetCount(o->tdefs), k; const void **keys = (const void **)calloc(n ? n : 1, sizeof *keys);
            CFDictionaryGetKeysAndValues(o->tdefs, keys, NULL);
            for (k = 0; k < n; k++) {
                CFTypeRef v = CFDictionaryGetValue(o->tvals, keys[k]); IOHIDElementRef e = oah_hc_elem(d, (unsigned int)(uintptr_t)keys[k]);
                if (!v) { v = CFDictionaryGetValue(o->tdefs, keys[k]); if (v && CFGetTypeID(v) != IOHIDValueGetTypeID()) v = NULL; }
                if (v && e && IOHIDDeviceSetValue(o->nd, e, (IOHIDValueRef)v)) r = kIOReturnError;
            }
            free(keys);
            CFDictionaryRemoveAllValues(o->tvals);
            if (R[5]) { oah_gq_item *it = oah_hid_item(GQ_PLAIN, R[5], 4, R[6], r, R[7]); it->a[3] = R[3]; oah_gq_post(o->q, it); }
            break; }
        default: r = kIOReturnUnsupported;
        }
    }
    if (oah_hid_log()) fprintf(stderr, "[hidcom] %d.%d(%08x %08x %08x) -> %08x\n", kind, m, R[3], R[4], R[5], r);
    g_gt_skip = 1; g_gt_skip_ret = r;
}
/* IOCreatePlugInInterfaceForService(service, pluginType, interfaceType, &interface, &score): ours for HID devices */
static void oah_fix_hidcom_create(unsigned int *R) {
    unsigned char pt[16], it[16];
    if (!R[4] || !R[5] || !oah_readable(R[4] + 8, 16) || !oah_readable(R[5] + 8, 16)) return;
    memcpy(pt, (void *)(uintptr_t)(R[4] + 8), 16); memcpy(it, (void *)(uintptr_t)(R[5] + 8), 16);   /* a guest CFUUID: CFRuntimeBase, then the bytes */
    if (!oah_hc_uuid(pt, kIOHIDDeviceUserClientTypeID) || !oah_hc_uuid(it, kIOCFPlugInInterfaceID)) return;   /* not a HID device: the guest's */
    {   IOHIDDeviceRef nd = IOHIDDeviceCreate(NULL, (io_service_t)R[3]); oah_hc *p;
        if (!nd) return;
        if (!(p = oah_hc_new(HC_PLUGIN, NULL))) { CFRelease(nd); return; }
        p->nd = nd;
        if (R[6] && oah_readable(R[6], 4)) *(unsigned int *)(uintptr_t)R[6] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)p);
        if (R[7] && oah_readable(R[7], 4)) *(unsigned int *)(uintptr_t)R[7] = 0;
        if (oah_hid_log()) fprintf(stderr, "[hidcom] plug-in for service %x: %p\n", R[3], (void *)p);
        g_gt_skip = 1; g_gt_skip_ret = 0;
    }
}
/* IODestroyPlugInInterface(interface) */
static void oah_fix_hidcom_destroy(unsigned int *R) {
    oah_hc *o = oah_hc_get(R[3]);
    if (!o) return;
    if (o->refs > 0) o->refs--;
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
