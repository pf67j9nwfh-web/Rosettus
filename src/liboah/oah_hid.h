/* oah_hid.h - HID devices (game pads, joysticks, keyboards, ...) for PowerPC apps through the
   IOHIDManager / IOHIDDevice / IOHIDElement / IOHIDValue / IOHIDQueue calls, served by macOS' own.

   10.6's PowerPC IOKit reaches a HID device through IOHIDLib's user client, and Mojave's kernel
   refuses user clients to a process of the other byte order (IOServiceOpen: kIOReturnUnsupported):
   PowerPC apps saw the devices in the registry but could not open or read them. The C calls are
   answered with the native ones:
   - managers, devices, elements and queues handed to the guest are proxies (oah_kc.h);
   - values are guest CFData objects that carry the value itself (element, integer and scaled
     values, time stamp, bytes), so the guest owns and frees them like real ones;
   - the native objects are scheduled on a run loop thread of ours; their callbacks queue the
     event, and a guest thread of ours signals a guest run-loop source on each run loop the app
     scheduled the object on; the source's perform routine calls the app's callback there.
   Answered with kIOReturnUnsupported / NULL: the multiple-value calls, the report calls with
   callbacks, element attach / detach / create, IOHIDManagerSaveToPropertyDomain. Not answered: the
   old IOHIDLib plug-in interface (IOHIDDeviceInterface, used through IOCreatePlugInInterfaceForService).
   OAH_NO_HID=1 leaves HID to the guest; OAH_HIDLOG=1 logs the calls. */
#include <IOKit/hid/IOHIDLib.h>
static int oah_want_hid(void) { return !getenv("OAH_NO_HID"); }
static int oah_hid_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_HIDLOG") ? 1 : 0; return v; }

/* ---- calls into the guest on the run loops it chose (a queue per object) ---- */
static int g_gq_pump_idx = -1, g_gq_perf_idx = -1;
enum { GQ_PLAIN, GQ_DEVICE, GQ_VALUE, GQ_REPORT };
typedef struct oah_gq_item {
    struct oah_gq_item *next;
    int kind, n; unsigned int fn, a[8];
    CFTypeRef o1, o2;              /* GQ_DEVICE: sender, device; GQ_VALUE: sender (or NULL: a[2] stays), value */
    unsigned char *buf; unsigned int len, gbuf, glen;   /* GQ_REPORT: the report, the app's buffer */
} oah_gq_item;
#define OAH_GQ_MAGIC 0x47515545u
#define OAH_GQ_RL 8
typedef struct oah_gq {
    unsigned int magic, src, rl[OAH_GQ_RL]; int nrl, signaled, count;
    oah_gq_item *head, *tail;
    struct oah_gq *nextall;
} oah_gq;
static pthread_mutex_t g_gq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gq_cv = PTHREAD_COND_INITIALIZER;
static oah_gq *g_gq_all;
static int g_gq_thread, g_gq_wake;
static oah_gq *oah_gq_new(void) {
    oah_gq *q = (oah_gq *)calloc(1, sizeof *q);
    q->magic = OAH_GQ_MAGIC;
    pthread_mutex_lock(&g_gq_mu); q->nextall = g_gq_all; g_gq_all = q; pthread_mutex_unlock(&g_gq_mu);
    return q;
}
static void oah_gq_free_item(oah_gq_item *it) {
    if (it->o1) CFRelease(it->o1);
    if (it->o2) CFRelease(it->o2);
    free(it->buf); free(it);
}
/* any thread: queue a call (the objects in it are retained by the item) */
static void oah_gq_post(oah_gq *q, oah_gq_item *it) {
    oah_gq_item *drop = NULL;
    pthread_mutex_lock(&g_gq_mu);
    if (q->count >= 4096) { drop = q->head; q->head = drop->next; if (!q->head) q->tail = NULL; q->count--; }   /* nobody runs the loop */
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it; q->count++;
    g_gq_wake = 1;
    pthread_cond_signal(&g_gq_cv);
    pthread_mutex_unlock(&g_gq_mu);
    if (drop) oah_gq_free_item(drop);
}
static void oah_fix_gq_pump(unsigned int *R) {
    (void)R;
    for (;;) {
        unsigned int sig[128], rl[128]; int ns = 0, nr = 0, i, k;
        oah_gq *q;
        pthread_mutex_lock(&g_gq_mu);
        while (!g_gq_wake) pthread_cond_wait(&g_gq_cv, &g_gq_mu);
        g_gq_wake = 0;
        for (q = g_gq_all; q && ns < 128; q = q->nextall)
            if (q->head && !q->signaled && q->src && q->nrl) {
                q->signaled = 1; sig[ns++] = q->src;
                for (k = 0; k < q->nrl && nr < 128; k++) rl[nr++] = q->rl[k];
            }
        pthread_mutex_unlock(&g_gq_mu);
        for (i = 0; i < ns; i++) oah_gf(GF_SIGNAL, 1, sig[i], 0, 0);
        for (i = 0; i < nr; i++) oah_gf(GF_WAKE, 1, rl[i], 0, 0);
    }
}
static void oah_gq_start_thread(void) {
    unsigned int pc, a[4], *t;
    static unsigned int tid;
    if (g_gq_thread || g_gq_pump_idx < 0) return;
    g_gq_thread = 2;
    if (!(pc = oah_guest_sym("libSystem", "_pthread_create")) || !(t = (unsigned int *)oah_guest_code_alloc(8))) return;
    t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_gq_pump_idx));
    t[1] = OSSwapHostToBigInt32(0x4e800020);
    a[0] = (unsigned int)(uintptr_t)&tid; a[1] = 0; a[2] = (unsigned int)(uintptr_t)t; a[3] = 0;
    if (!oah_callppc_n("gq_pthread_create", pc, 4, a)) g_gq_thread = 1;
}
/* guest thread: add the queue's source to a run loop */
static void oah_gq_schedule(oah_gq *q, unsigned int rl, unsigned int mode) {
    if (!q || !rl || !oah_fse_gcf()) return;
    if (!q->src) {
        unsigned int *ctx = (unsigned int *)calloc(10, 4), *stub;
        if (!ctx || !(stub = (unsigned int *)oah_guest_code_alloc(8))) { free(ctx); return; }
        stub[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_gq_perf_idx));
        stub[1] = OSSwapHostToBigInt32(0x4e800020);
        ctx[1] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)q); ctx[9] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)stub);
        q->src = oah_gf(GF_SRCCREATE, 3, 0, 0, (unsigned int)(uintptr_t)ctx);
        free(ctx);
        if (oah_hid_log()) fprintf(stderr, "[hid] queue %p: source %08x, perform stub %p (entry %d of %d)\n", (void *)q, q->src, (void *)stub, g_gq_perf_idx, g_gt_n);
        if (!q->src) return;
    }
    oah_gf(GF_ADDSRC, 3, rl, q->src, mode);
    pthread_mutex_lock(&g_gq_mu);
    {   int k, have = 0;
        for (k = 0; k < q->nrl; k++) if (q->rl[k] == rl) have = 1;
        if (!have && q->nrl < OAH_GQ_RL) q->rl[q->nrl++] = rl;
        if (q->head) { q->signaled = 0; g_gq_wake = 1; pthread_cond_signal(&g_gq_cv); } }
    pthread_mutex_unlock(&g_gq_mu);
    oah_gq_start_thread();
}
static void oah_gq_unschedule(oah_gq *q, unsigned int rl, unsigned int mode) {
    int k;
    if (!q || !q->src || !oah_fse_gcf()) return;
    oah_gf(GF_REMSRC, 3, rl, q->src, mode);
    pthread_mutex_lock(&g_gq_mu);
    for (k = 0; k < q->nrl; k++) if (q->rl[k] == rl) { q->rl[k] = q->rl[--q->nrl]; break; }
    pthread_mutex_unlock(&g_gq_mu);
}
static unsigned int oah_hid_gvalue(IOHIDValueRef v);
static unsigned int oah_hid_g(CFTypeRef n);
/* the source's perform routine: run the queued calls on this (the app's) thread */
static void oah_fix_gq_perform(unsigned int *R) {
    oah_gq *q = (oah_gq *)(uintptr_t)R[3];
    if (!q || q->magic != OAH_GQ_MAGIC) { g_gt_skip = 1; g_gt_skip_ret = 0; return; }
    if (oah_hid_log()) fprintf(stderr, "[hid] perform %p: %d queued\n", (void *)q, q->count);
    for (;;) {
        oah_gq_item *it; unsigned int gv = 0;
        pthread_mutex_lock(&g_gq_mu);
        it = q->head;
        if (it) { q->head = it->next; if (!q->head) q->tail = NULL; q->count--; }
        else q->signaled = 0;
        pthread_mutex_unlock(&g_gq_mu);
        if (!it) break;
        switch (it->kind) {
        case GQ_DEVICE: if (it->o1) it->a[2] = oah_hid_g(it->o1); it->a[3] = oah_hid_g(it->o2); break;
        case GQ_VALUE: if (it->o1) it->a[2] = oah_hid_g(it->o1); it->a[3] = gv = oah_hid_gvalue((IOHIDValueRef)it->o2); break;
        case GQ_REPORT:
            if (it->o1) it->a[2] = oah_hid_g(it->o1);
            if (it->gbuf) { unsigned int n = it->len < it->glen ? it->len : it->glen; if (oah_readable(it->gbuf, n ? n : 1)) memcpy((void *)(uintptr_t)it->gbuf, it->buf, n); it->a[5] = it->gbuf; it->a[6] = n; }
            else { it->a[5] = (unsigned int)(uintptr_t)it->buf; it->a[6] = it->len; }
            break;
        }
        if (it->fn && (it->kind != GQ_VALUE || gv)) oah_callppc_n("hid_callback", it->fn, it->n, it->a);
        if (gv) oah_gf(GF_RELEASE, 1, gv, 0, 0);
        oah_gq_free_item(it);
    }
    /* last: the guest calls above run hooked functions, and each of those clears the (global) skip flag */
    g_gt_skip = 1; g_gt_skip_ret = 0;
}

/* ---- the native side: a run loop thread, and per object state ---- */
static CFRunLoopRef g_hid_rl;
static void *oah_hid_thread(void *x) {
    CFRunLoopSourceContext c; CFRunLoopSourceRef s;
    memset(&c, 0, sizeof c);
    s = CFRunLoopSourceCreate(NULL, 0, &c);   /* keeps the loop running */
    CFRunLoopAddSource(CFRunLoopGetCurrent(), s, kCFRunLoopDefaultMode);
    g_hid_rl = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
    dispatch_semaphore_signal((dispatch_semaphore_t)x);
    for (;;) CFRunLoopRun();
    return NULL;
}
static CFRunLoopRef oah_hid_rl(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!g_hid_rl) {
        pthread_t t; dispatch_semaphore_t sm = dispatch_semaphore_create(0);
        if (!pthread_create(&t, NULL, oah_hid_thread, sm)) dispatch_semaphore_wait(sm, DISPATCH_TIME_FOREVER);
    }
    pthread_mutex_unlock(&mu);
    return g_hid_rl;
}
typedef struct oah_hids {
    CFTypeRef n; unsigned int g; int sched;
    oah_gq *q;
    unsigned int matchfn, matchctx, remfn, remctx, valfn, valctx, repfn, repctx, repgbuf, repglen, availfn, availctx;
    unsigned char *repbuf;
} oah_hids;
static CFMutableDictionaryRef g_hids;
static pthread_mutex_t g_hids_mu = PTHREAD_MUTEX_INITIALIZER;
static oah_hids *oah_hid_state(CFTypeRef n) {
    oah_hids *s;
    if (!n) return NULL;
    pthread_mutex_lock(&g_hids_mu);
    if (!g_hids) { CFDictionaryKeyCallBacks kcb = kCFTypeDictionaryKeyCallBacks; kcb.equal = NULL; kcb.hash = NULL; g_hids = CFDictionaryCreateMutable(NULL, 0, &kcb, NULL); }
    s = (oah_hids *)CFDictionaryGetValue(g_hids, n);
    if (!s) { s = (oah_hids *)calloc(1, sizeof *s); s->n = n; s->q = oah_gq_new(); CFDictionarySetValue(g_hids, n, s); }
    pthread_mutex_unlock(&g_hids_mu);
    return s;
}
/* a native object's proxy (guest thread) */
static unsigned int oah_hid_g(CFTypeRef n) { return n ? oah_kc_wrap(n) : 0; }
static unsigned int oah_hid_own(CFTypeRef n) {   /* for a Create / Copy result: the caller's own retain */
    unsigned int g = oah_hid_g(n), a[1];
    static unsigned int frt;
    if (!frt) frt = oah_guest_sym("CoreFoundation", "_CFRetain");
    if (g && frt) { a[0] = g; oah_callppc_n("hid_own", frt, 1, a); }
    return g;
}
static IOHIDDeviceRef oah_hid_dev(unsigned int g) { CFTypeRef n = oah_kc_unwrap(g); return n && CFGetTypeID(n) == IOHIDDeviceGetTypeID() ? (IOHIDDeviceRef)n : NULL; }
static IOHIDManagerRef oah_hid_mgr(unsigned int g) { CFTypeRef n = oah_kc_unwrap(g); return n && CFGetTypeID(n) == IOHIDManagerGetTypeID() ? (IOHIDManagerRef)n : NULL; }
static IOHIDElementRef oah_hid_elem(unsigned int g) { CFTypeRef n = oah_kc_unwrap(g); return n && CFGetTypeID(n) == IOHIDElementGetTypeID() ? (IOHIDElementRef)n : NULL; }
static IOHIDQueueRef oah_hid_queue(unsigned int g) { CFTypeRef n = oah_kc_unwrap(g); return n && CFGetTypeID(n) == IOHIDQueueGetTypeID() ? (IOHIDQueueRef)n : NULL; }

/* ---- values: guest CFData objects that carry the value ---- */
#define OAH_HIDV_MAGIC 0x48494456u
typedef struct { unsigned int magic, ge; IOHIDElementRef ne; int iv, bytes; UInt64 ts; double sc, sp; unsigned int len; unsigned char b[4]; } oah_hidv;
static unsigned int oah_hid_gvalue(IOHIDValueRef v) {
    static unsigned int fdc;
    oah_hidv *h; CFIndex len; unsigned int g, a[3];
    if (!v) return 0;
    if (!fdc) fdc = oah_guest_sym("CoreFoundation", "_CFDataCreate");
    if (!fdc) return 0;
    len = IOHIDValueGetLength(v);
    if (len < 0 || len > 65536) len = 0;
    h = (oah_hidv *)calloc(1, sizeof *h + (size_t)len);
    h->magic = OAH_HIDV_MAGIC; h->ne = IOHIDValueGetElement(v); h->ge = oah_hid_g(h->ne);
    h->ts = IOHIDValueGetTimeStamp(v); h->len = (unsigned int)len;
    if (len) memcpy(h->b, IOHIDValueGetBytePtr(v), (size_t)len);
    h->bytes = len > 4;
    h->iv = (int)IOHIDValueGetIntegerValue(v);
    h->sc = IOHIDValueGetScaledValue(v, kIOHIDValueScaleTypeCalibrated);
    h->sp = IOHIDValueGetScaledValue(v, kIOHIDValueScaleTypePhysical);
    a[0] = 0; a[1] = (unsigned int)(uintptr_t)h; a[2] = (unsigned int)(sizeof *h + (size_t)len);
    g = oah_callppc_n("hid_value", fdc, 3, a);
    free(h);
    return g;
}
static oah_hidv *oah_hid_v(unsigned int g) {
    static unsigned int fbp;
    unsigned int a[1]; oah_hidv *h;
    if (!g) return NULL;
    if (!fbp) fbp = oah_guest_sym("CoreFoundation", "_CFDataGetBytePtr");
    if (!fbp) return NULL;
    a[0] = g; h = (oah_hidv *)(uintptr_t)oah_callppc_n("hid_vptr", fbp, 1, a);
    return h && oah_readable((unsigned int)(uintptr_t)h, sizeof *h) && h->magic == OAH_HIDV_MAGIC ? h : NULL;
}
/* a native value from one of ours (for SetValue) */
static IOHIDValueRef oah_hid_nvalue(unsigned int g) {
    oah_hidv *h = oah_hid_v(g);
    if (!h || !h->ne) return NULL;
    return h->bytes ? IOHIDValueCreateWithBytes(NULL, h->ne, h->ts, h->b, h->len) : IOHIDValueCreateWithIntegerValue(NULL, h->ne, h->ts, h->iv);
}
/* unowned results (Get): kept a while, then released */
static void oah_hid_unowned(unsigned int g) {
    static unsigned int ring[256]; static int k;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    unsigned int old;
    pthread_mutex_lock(&mu); old = ring[k & 255]; ring[k & 255] = g; k++; pthread_mutex_unlock(&mu);
    if (old) oah_gf(GF_RELEASE, 1, old, 0, 0);
}

/* ---- guest property lists and arrays as native ones ---- */
static CFPropertyListRef oah_hid_nplist(unsigned int g) {
    static unsigned int fx, fbp, fln;
    unsigned int a[2], x, p, n; CFDataRef d; CFPropertyListRef r = NULL;
    if (!g) return NULL;
    if (!fx) { fx = oah_guest_sym("CoreFoundation", "_CFPropertyListCreateXMLData"); fbp = oah_guest_sym("CoreFoundation", "_CFDataGetBytePtr"); fln = oah_guest_sym("CoreFoundation", "_CFDataGetLength"); }
    if (!fx || !fbp || !fln || !oah_fse_gcf()) return NULL;
    a[0] = 0; a[1] = g;
    if (!(x = oah_callppc_n("hid_xml", fx, 2, a))) return NULL;
    a[0] = x; p = oah_callppc_n("hid_xmlp", fbp, 1, a); n = oah_callppc_n("hid_xmll", fln, 1, a);
    if (p && n && oah_readable(p, n) && (d = CFDataCreate(NULL, (const UInt8 *)(uintptr_t)p, n))) {
        r = CFPropertyListCreateFromXMLData(NULL, d, kCFPropertyListImmutable, NULL);
        CFRelease(d);
    }
    oah_gf(GF_RELEASE, 1, x, 0, 0);
    return r;
}
CFPropertyListRef oah_hid_nplist_fwd(unsigned int g) { return oah_hid_nplist(g); }   /* for oah_sc.h */
/* a native array of objects as an owned guest CFArray (or CFSet) of their proxies */
static unsigned int oah_hid_garray(CFArrayRef l, int set) {
    static unsigned int fca, fcb, fsc, fsb;
    unsigned int *v, a[4], r; CFIndex n, k;
    if (!fca) { fca = oah_guest_sym("CoreFoundation", "_CFArrayCreate"); fcb = oah_guest_sym("CoreFoundation", "_kCFTypeArrayCallBacks");
                fsc = oah_guest_sym("CoreFoundation", "_CFSetCreate"); fsb = oah_guest_sym("CoreFoundation", "_kCFTypeSetCallBacks"); }
    if (!l || !(set ? fsc && fsb : fca && fcb)) return 0;
    n = CFArrayGetCount(l);
    v = (unsigned int *)calloc(n ? n : 1, 4);
    for (k = 0; k < n; k++) v[k] = OSSwapHostToBigInt32(oah_hid_g(CFArrayGetValueAtIndex(l, k)));
    a[0] = 0; a[1] = (unsigned int)(uintptr_t)v; a[2] = (unsigned int)n; a[3] = set ? fsb : fcb;
    r = oah_callppc_n("hid_array", set ? fsc : fca, 4, a);
    free(v);
    return r;
}
/* unowned guest copies of a native object's arrays (element children), kept for the process */
static unsigned int oah_hid_children(IOHIDElementRef e) {
    static CFMutableDictionaryRef m; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    const void *v; unsigned int g;
    CFArrayRef c = IOHIDElementGetChildren(e);
    if (!c) return 0;
    pthread_mutex_lock(&mu);
    if (!m) { CFDictionaryKeyCallBacks kcb = kCFTypeDictionaryKeyCallBacks; kcb.equal = NULL; kcb.hash = NULL; m = CFDictionaryCreateMutable(NULL, 0, &kcb, NULL); }
    if (CFDictionaryGetValueIfPresent(m, e, &v)) { pthread_mutex_unlock(&mu); return (unsigned int)(uintptr_t)v; }
    pthread_mutex_unlock(&mu);
    g = oah_hid_garray(c, 0);
    pthread_mutex_lock(&mu); CFDictionarySetValue(m, e, (const void *)(uintptr_t)g); pthread_mutex_unlock(&mu);
    return g;
}

/* ---- native callbacks: queue the call for the guest ---- */
static oah_gq_item *oah_hid_item(int kind, unsigned int fn, int n, unsigned int ctx, unsigned int result, unsigned int sender) {
    oah_gq_item *it = (oah_gq_item *)calloc(1, sizeof *it);
    it->kind = kind; it->fn = fn; it->n = n; it->a[0] = ctx; it->a[1] = result; it->a[2] = sender;
    return it;
}
static void oah_hid_cb_match(void *c, IOReturn r, void *sender, IOHIDDeviceRef d) {
    oah_hids *s = (oah_hids *)c; oah_gq_item *it;
    (void)sender;
    if (!s->matchfn) return;
    it = oah_hid_item(GQ_DEVICE, s->matchfn, 4, s->matchctx, (unsigned int)r, s->g);
    it->o2 = CFRetain(d);
    oah_gq_post(s->q, it);
}
static void oah_hid_cb_removal_m(void *c, IOReturn r, void *sender, IOHIDDeviceRef d) {
    oah_hids *s = (oah_hids *)c; oah_gq_item *it;
    (void)sender;
    if (!s->remfn) return;
    it = oah_hid_item(GQ_DEVICE, s->remfn, 4, s->remctx, (unsigned int)r, s->g);
    it->o2 = CFRetain(d);
    oah_gq_post(s->q, it);
}
static void oah_hid_cb_removal_d(void *c, IOReturn r, void *sender) {   /* a device's own: (context, result, sender) */
    oah_hids *s = (oah_hids *)c;
    (void)sender;
    if (s->remfn) oah_gq_post(s->q, oah_hid_item(GQ_PLAIN, s->remfn, 3, s->remctx, (unsigned int)r, s->g));
}
static void oah_hid_cb_value(void *c, IOReturn r, void *sender, IOHIDValueRef v) {
    oah_hids *s = (oah_hids *)c; oah_gq_item *it;
    if (!s->valfn || !v) return;
    it = oah_hid_item(GQ_VALUE, s->valfn, 4, s->valctx, (unsigned int)r, s->g);
    /* a manager's values come with the device as the sender */
    if (sender && sender != s->n && CFGetTypeID((CFTypeRef)sender) == IOHIDDeviceGetTypeID()) it->o1 = CFRetain((CFTypeRef)sender);
    it->o2 = CFRetain(v);
    oah_gq_post(s->q, it);
}
static void oah_hid_cb_report(void *c, IOReturn r, void *sender, IOHIDReportType type, uint32_t id, uint8_t *rep, CFIndex len) {
    oah_hids *s = (oah_hids *)c; oah_gq_item *it;
    if (!s->repfn || len < 0) return;
    it = oah_hid_item(GQ_REPORT, s->repfn, 7, s->repctx, (unsigned int)r, s->g);
    if (sender && sender != s->n && CFGetTypeID((CFTypeRef)sender) == IOHIDDeviceGetTypeID()) it->o1 = CFRetain((CFTypeRef)sender);
    it->a[3] = (unsigned int)type; it->a[4] = id;
    it->buf = (unsigned char *)malloc(len ? (size_t)len : 1); if (len) memcpy(it->buf, rep, (size_t)len);
    it->len = (unsigned int)len; it->gbuf = s->repgbuf; it->glen = s->repglen;
    oah_gq_post(s->q, it);
}
static void oah_hid_cb_avail(void *c, IOReturn r, void *sender) {
    oah_hids *s = (oah_hids *)c;
    (void)sender;
    if (s->availfn) oah_gq_post(s->q, oah_hid_item(GQ_PLAIN, s->availfn, 3, s->availctx, (unsigned int)r, s->g));
}
/* the native object on our run loop (once) */
static void oah_hid_native_schedule(oah_hids *s) {
    CFRunLoopRef rl;
    if (s->sched || !(rl = oah_hid_rl())) return;
    s->sched = 1;
    if (CFGetTypeID(s->n) == IOHIDManagerGetTypeID()) IOHIDManagerScheduleWithRunLoop((IOHIDManagerRef)s->n, rl, kCFRunLoopDefaultMode);
    else if (CFGetTypeID(s->n) == IOHIDDeviceGetTypeID()) IOHIDDeviceScheduleWithRunLoop((IOHIDDeviceRef)s->n, rl, kCFRunLoopDefaultMode);
    else if (CFGetTypeID(s->n) == IOHIDQueueGetTypeID()) IOHIDQueueScheduleWithRunLoop((IOHIDQueueRef)s->n, rl, kCFRunLoopDefaultMode);
}

/* ---- the calls ---- */
enum {
    HM_CREATE, HM_OPEN, HM_CLOSE, HM_GETPROP, HM_SETPROP, HM_MATCH, HM_MATCHMULTI, HM_COPYDEVICES, HM_REGMATCH, HM_REGREMOVAL,
    HM_REGREPORT, HM_REGVALUE, HM_VALMATCH, HM_VALMATCHMULTI, HM_SCHEDULE, HM_UNSCHEDULE, HM_TYPEID,
    HD_CREATE, HD_OPEN, HD_CLOSE, HD_CONFORMS, HD_GETPROP, HD_SETPROP, HD_COPYELEMS, HD_SCHEDULE, HD_UNSCHEDULE, HD_REGREMOVAL,
    HD_REGVALUE, HD_REGREPORT, HD_VALMATCH, HD_VALMATCHMULTI, HD_SETVALUE, HD_GETVALUE, HD_SETVALUECB, HD_GETVALUECB,
    HD_SETREPORT, HD_GETREPORT, HD_SERVICE, HD_TYPEID,
    HE_COOKIE, HE_TYPE, HE_COLLTYPE, HE_PAGE, HE_USAGE, HE_VIRTUAL, HE_RELATIVE, HE_WRAPPING, HE_ARRAY, HE_NONLINEAR, HE_PREFSTATE,
    HE_NULLSTATE, HE_NAME, HE_REPORTID, HE_REPORTSIZE, HE_REPORTCOUNT, HE_UNIT, HE_UNITEXP, HE_LMIN, HE_LMAX, HE_PMIN, HE_PMAX,
    HE_DUPINDEX, HE_DEVICE, HE_PARENT, HE_CHILDREN, HE_GETPROP, HE_SETPROP, HE_TYPEID,
    HV_CREATEINT, HV_CREATEBYTES, HV_CREATEBYTESNC, HV_ELEMENT, HV_TIMESTAMP, HV_LENGTH, HV_BYTEPTR, HV_INT, HV_SCALED, HV_TYPEID,
    HQ_CREATE, HQ_DEVICE, HQ_GETDEPTH, HQ_SETDEPTH, HQ_ADD, HQ_REMOVE, HQ_CONTAINS, HQ_START, HQ_STOP, HQ_REGAVAIL, HQ_SCHEDULE,
    HQ_UNSCHEDULE, HQ_COPYNEXT, HQ_COPYNEXTTO, HQ_TYPEID, HX_UNSUP, HX_NULL, HID_N };
static void oah_hid_call(int w, unsigned int *R) {
    double *F = (double *)(R + 0x1e8 / 4);
    unsigned int r = 0;
    oah_hids *s;
    if (!oah_fse_gcf()) return;
    switch (w) {
    /* managers */
    case HM_CREATE: { IOHIDManagerRef m = IOHIDManagerCreate(NULL, R[4]);
        if (m) { s = oah_hid_state(m); r = oah_hid_own(m); s->g = r; oah_hid_native_schedule(s); CFRelease(m); }
        break; }
    case HM_OPEN: case HM_CLOSE: { IOHIDManagerRef m = oah_hid_mgr(R[3]);
        r = !m ? kIOReturnBadArgument : w == HM_OPEN ? IOHIDManagerOpen(m, R[4]) : IOHIDManagerClose(m, R[4]); break; }
    case HM_GETPROP: case HD_GETPROP: case HE_GETPROP: {
        CFTypeRef o = oah_kc_unwrap(R[3]), v = NULL; CFStringRef k = oah_midi_nstr(R[4]);
        if (o && k) v = w == HM_GETPROP ? IOHIDManagerGetProperty((IOHIDManagerRef)o, k) : w == HD_GETPROP ? IOHIDDeviceGetProperty((IOHIDDeviceRef)o, k) : IOHIDElementGetProperty((IOHIDElementRef)o, k);
        r = v ? oah_ps_unowned(v) : 0;
        if (k) CFRelease(k);
        break; }
    case HM_SETPROP: case HD_SETPROP: case HE_SETPROP: {
        CFTypeRef o = oah_kc_unwrap(R[3]), v = oah_hid_nplist(R[5]); CFStringRef k = oah_midi_nstr(R[4]);
        if (o && k && v) r = w == HM_SETPROP ? IOHIDManagerSetProperty((IOHIDManagerRef)o, k, v) : w == HD_SETPROP ? IOHIDDeviceSetProperty((IOHIDDeviceRef)o, k, v) : IOHIDElementSetProperty((IOHIDElementRef)o, k, v);
        if (k) CFRelease(k);
        if (v) CFRelease(v);
        break; }
    case HM_MATCH: case HM_MATCHMULTI: case HM_VALMATCH: case HM_VALMATCHMULTI: {
        IOHIDManagerRef m = oah_hid_mgr(R[3]); CFTypeRef d = oah_hid_nplist(R[4]);
        if (m) {
            if (w == HM_MATCH) IOHIDManagerSetDeviceMatching(m, (CFDictionaryRef)d);
            else if (w == HM_MATCHMULTI) IOHIDManagerSetDeviceMatchingMultiple(m, (CFArrayRef)d);
            else if (w == HM_VALMATCH) IOHIDManagerSetInputValueMatching(m, (CFDictionaryRef)d);
            else IOHIDManagerSetInputValueMatchingMultiple(m, (CFArrayRef)d);
        }
        if (d) CFRelease(d);
        break; }
    case HM_COPYDEVICES: { IOHIDManagerRef m = oah_hid_mgr(R[3]); CFSetRef st = m ? IOHIDManagerCopyDevices(m) : NULL;
        if (st) {
            CFIndex n = CFSetGetCount(st); const void **v = (const void **)calloc(n ? n : 1, sizeof *v); CFArrayRef l;
            CFSetGetValues(st, v); l = CFArrayCreate(NULL, v, n, &kCFTypeArrayCallBacks);
            r = oah_hid_garray(l, 1);
            CFRelease(l); free(v); CFRelease(st);
        }
        break; }
    case HM_REGMATCH: case HM_REGREMOVAL: case HM_REGVALUE: case HM_REGREPORT: {
        IOHIDManagerRef m = oah_hid_mgr(R[3]);
        if (!m) break;
        s = oah_hid_state(m); s->g = R[3];
        if (w == HM_REGMATCH) { s->matchfn = R[4]; s->matchctx = R[5]; IOHIDManagerRegisterDeviceMatchingCallback(m, R[4] ? oah_hid_cb_match : NULL, s); }
        else if (w == HM_REGREMOVAL) { s->remfn = R[4]; s->remctx = R[5]; IOHIDManagerRegisterDeviceRemovalCallback(m, R[4] ? oah_hid_cb_removal_m : NULL, s); }
        else if (w == HM_REGVALUE) { s->valfn = R[4]; s->valctx = R[5]; IOHIDManagerRegisterInputValueCallback(m, R[4] ? oah_hid_cb_value : NULL, s); }
        else { s->repfn = R[4]; s->repctx = R[5]; s->repgbuf = 0; IOHIDManagerRegisterInputReportCallback(m, R[4] ? oah_hid_cb_report : NULL, s); }
        break; }
    case HM_SCHEDULE: case HM_UNSCHEDULE: case HD_SCHEDULE: case HD_UNSCHEDULE: case HQ_SCHEDULE: case HQ_UNSCHEDULE: {
        CFTypeRef o = oah_kc_unwrap(R[3]);
        if (!o) break;
        s = oah_hid_state(o); s->g = R[3];
        if (w == HM_SCHEDULE || w == HD_SCHEDULE || w == HQ_SCHEDULE) { oah_hid_native_schedule(s); oah_gq_schedule(s->q, R[4], R[5]); }
        else oah_gq_unschedule(s->q, R[4], R[5]);
        break; }
    case HM_TYPEID: case HD_TYPEID: case HE_TYPEID: case HV_TYPEID: case HQ_TYPEID: {   /* our proxies are CFData objects */
        static unsigned int ftid; unsigned int a[1] = { 0 };
        if (!ftid) ftid = oah_guest_sym("CoreFoundation", "_CFDataGetTypeID");
        r = ftid ? oah_callppc_n("hid_typeid", ftid, 0, a) : 0;
        break; }
    /* devices */
    case HD_CREATE: { IOHIDDeviceRef d = IOHIDDeviceCreate(NULL, (io_service_t)R[4]);
        if (d) { r = oah_hid_own(d); CFRelease(d); }
        break; }
    case HD_OPEN: case HD_CLOSE: { IOHIDDeviceRef d = oah_hid_dev(R[3]);
        r = !d ? kIOReturnBadArgument : w == HD_OPEN ? IOHIDDeviceOpen(d, R[4]) : IOHIDDeviceClose(d, R[4]); break; }
    case HD_CONFORMS: { IOHIDDeviceRef d = oah_hid_dev(R[3]); r = d && IOHIDDeviceConformsTo(d, R[4], R[5]); break; }
    case HD_COPYELEMS: { IOHIDDeviceRef d = oah_hid_dev(R[3]); CFDictionaryRef mt = (CFDictionaryRef)oah_hid_nplist(R[4]);
        CFArrayRef l = d ? IOHIDDeviceCopyMatchingElements(d, mt, R[5]) : NULL;
        if (l) { r = oah_hid_garray(l, 0); CFRelease(l); }
        if (mt) CFRelease(mt);
        break; }
    case HD_REGREMOVAL: case HD_REGVALUE: { IOHIDDeviceRef d = oah_hid_dev(R[3]);
        if (!d) break;
        s = oah_hid_state(d); s->g = R[3];
        if (w == HD_REGREMOVAL) { s->remfn = R[4]; s->remctx = R[5]; IOHIDDeviceRegisterRemovalCallback(d, R[4] ? oah_hid_cb_removal_d : NULL, s); }
        else { s->valfn = R[4]; s->valctx = R[5]; IOHIDDeviceRegisterInputValueCallback(d, R[4] ? oah_hid_cb_value : NULL, s); }
        break; }
    case HD_REGREPORT: {   /* (device, report buffer, length, callback, context) */
        IOHIDDeviceRef d = oah_hid_dev(R[3]);
        if (!d) break;
        s = oah_hid_state(d); s->g = R[3];
        s->repfn = R[6]; s->repctx = R[7]; s->repgbuf = R[4]; s->repglen = R[5];
        if (R[6] && R[5] && R[5] < (1u << 20)) {
            free(s->repbuf); s->repbuf = (unsigned char *)malloc(R[5]);
            IOHIDDeviceRegisterInputReportCallback(d, s->repbuf, (CFIndex)R[5], oah_hid_cb_report, s);
        } else if (s->repbuf) IOHIDDeviceRegisterInputReportCallback(d, s->repbuf, (CFIndex)s->repglen, NULL, s);
        break; }
    case HD_VALMATCH: case HD_VALMATCHMULTI: { IOHIDDeviceRef d = oah_hid_dev(R[3]); CFTypeRef m = oah_hid_nplist(R[4]);
        if (d) { if (w == HD_VALMATCH) IOHIDDeviceSetInputValueMatching(d, (CFDictionaryRef)m); else IOHIDDeviceSetInputValueMatchingMultiple(d, (CFArrayRef)m); }
        if (m) CFRelease(m);
        break; }
    case HD_SETVALUE: case HD_SETVALUECB: {   /* (device, element, value[, timeout (f1), callback, context]) */
        IOHIDDeviceRef d = oah_hid_dev(R[3]); IOHIDElementRef e = oah_hid_elem(R[4]); IOHIDValueRef v = oah_hid_nvalue(R[5]);
        r = d && e && v ? IOHIDDeviceSetValue(d, e, v) : kIOReturnBadArgument;
        /* with a callback: answered at once, the callback not called (as macOS and 10.6 do when the
           device answers right away) */
        if (v) CFRelease(v);
        break; }
    case HD_GETVALUE: case HD_GETVALUECB: {   /* (device, element, &value[, timeout (f1), callback, context]) */
        IOHIDDeviceRef d = oah_hid_dev(R[3]); IOHIDElementRef e = oah_hid_elem(R[4]); IOHIDValueRef v = NULL; unsigned int gv = 0;
        r = d && e ? IOHIDDeviceGetValue(d, e, &v) : kIOReturnBadArgument;
        if (!r && v) { gv = oah_hid_gvalue(v); oah_hid_unowned(gv); }
        if (R[5] && oah_readable(R[5], 4)) *(unsigned int *)(uintptr_t)R[5] = OSSwapHostToBigInt32(gv);
        break; }
    case HD_SETREPORT: { IOHIDDeviceRef d = oah_hid_dev(R[3]);   /* (device, type, reportID, report, length) */
        r = d && (!R[7] || oah_readable(R[6], R[7])) ? IOHIDDeviceSetReport(d, (IOHIDReportType)R[4], (CFIndex)R[5], (const uint8_t *)(uintptr_t)R[6], (CFIndex)R[7]) : kIOReturnBadArgument;
        break; }
    case HD_GETREPORT: { IOHIDDeviceRef d = oah_hid_dev(R[3]);   /* (device, type, reportID, report, &length) */
        CFIndex n = R[7] && oah_readable(R[7], 4) ? (CFIndex)OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)R[7]) : 0;
        r = d && n > 0 && oah_readable(R[6], (unsigned int)n) ? IOHIDDeviceGetReport(d, (IOHIDReportType)R[4], (CFIndex)R[5], (uint8_t *)(uintptr_t)R[6], &n) : kIOReturnBadArgument;
        if (!r) *(unsigned int *)(uintptr_t)R[7] = OSSwapHostToBigInt32((unsigned int)n);
        break; }
    case HD_SERVICE: { IOHIDDeviceRef d = oah_hid_dev(R[3]); r = d ? IOHIDDeviceGetService(d) : 0; break; }
    /* elements */
    case HE_COOKIE: case HE_TYPE: case HE_COLLTYPE: case HE_PAGE: case HE_USAGE: case HE_VIRTUAL: case HE_RELATIVE: case HE_WRAPPING:
    case HE_ARRAY: case HE_NONLINEAR: case HE_PREFSTATE: case HE_NULLSTATE: case HE_REPORTID: case HE_REPORTSIZE: case HE_REPORTCOUNT:
    case HE_UNIT: case HE_UNITEXP: case HE_LMIN: case HE_LMAX: case HE_PMIN: case HE_PMAX: case HE_DUPINDEX: case HE_NAME:
    case HE_DEVICE: case HE_PARENT: case HE_CHILDREN: {
        IOHIDElementRef e = oah_hid_elem(R[3]);
        if (!e) break;
        switch (w) {
        case HE_COOKIE: r = (unsigned int)IOHIDElementGetCookie(e); break;
        case HE_TYPE: r = IOHIDElementGetType(e); break;
        case HE_COLLTYPE: r = IOHIDElementGetCollectionType(e); break;
        case HE_PAGE: r = IOHIDElementGetUsagePage(e); break;
        case HE_USAGE: r = IOHIDElementGetUsage(e); break;
        case HE_VIRTUAL: r = IOHIDElementIsVirtual(e); break;
        case HE_RELATIVE: r = IOHIDElementIsRelative(e); break;
        case HE_WRAPPING: r = IOHIDElementIsWrapping(e); break;
        case HE_ARRAY: r = IOHIDElementIsArray(e); break;
        case HE_NONLINEAR: r = IOHIDElementIsNonLinear(e); break;
        case HE_PREFSTATE: r = IOHIDElementHasPreferredState(e); break;
        case HE_NULLSTATE: r = IOHIDElementHasNullState(e); break;
        case HE_REPORTID: r = IOHIDElementGetReportID(e); break;
        case HE_REPORTSIZE: r = IOHIDElementGetReportSize(e); break;
        case HE_REPORTCOUNT: r = IOHIDElementGetReportCount(e); break;
        case HE_UNIT: r = IOHIDElementGetUnit(e); break;
        case HE_UNITEXP: r = IOHIDElementGetUnitExponent(e); break;
        case HE_LMIN: r = (unsigned int)IOHIDElementGetLogicalMin(e); break;
        case HE_LMAX: r = (unsigned int)IOHIDElementGetLogicalMax(e); break;
        case HE_PMIN: r = (unsigned int)IOHIDElementGetPhysicalMin(e); break;
        case HE_PMAX: r = (unsigned int)IOHIDElementGetPhysicalMax(e); break;
        case HE_DUPINDEX: r = IOHIDElementGetDuplicateIndex(e); break;
        case HE_NAME: { CFStringRef n = IOHIDElementGetName(e); r = n ? oah_ps_unowned(n) : 0; break; }
        case HE_DEVICE: r = oah_hid_g(IOHIDElementGetDevice(e)); break;
        case HE_PARENT: r = oah_hid_g(IOHIDElementGetParent(e)); break;
        case HE_CHILDREN: r = oah_hid_children(e); break;
        }
        break; }
    /* values */
    case HV_CREATEINT: case HV_CREATEBYTES: case HV_CREATEBYTESNC: {   /* (alloc, element, timeStamp (r5:r6), value | bytes, length) */
        IOHIDElementRef e = oah_hid_elem(R[4]); IOHIDValueRef v = NULL; UInt64 ts = ((UInt64)R[5] << 32) | R[6];
        if (e) {
            if (w == HV_CREATEINT) v = IOHIDValueCreateWithIntegerValue(NULL, e, ts, (CFIndex)(int)R[7]);
            else if (R[8] < (1u << 20) && oah_readable(R[7], R[8] ? R[8] : 1)) v = IOHIDValueCreateWithBytes(NULL, e, ts, (const uint8_t *)(uintptr_t)R[7], (CFIndex)R[8]);
        }
        if (v) { r = oah_hid_gvalue(v); CFRelease(v); }
        break; }
    case HV_ELEMENT: case HV_TIMESTAMP: case HV_LENGTH: case HV_BYTEPTR: case HV_INT: case HV_SCALED: {
        oah_hidv *h = oah_hid_v(R[3]);
        if (!h) { if (w == HV_SCALED) F[1] = 0; if (w == HV_TIMESTAMP) R[4] = 0; break; }
        switch (w) {
        case HV_ELEMENT: r = h->ge; break;
        case HV_TIMESTAMP: r = (unsigned int)(h->ts >> 32); R[4] = (unsigned int)h->ts; break;
        case HV_LENGTH: r = h->len; break;
        case HV_BYTEPTR: r = (unsigned int)(uintptr_t)h->b; break;
        case HV_INT: r = (unsigned int)h->iv; break;
        case HV_SCALED: F[1] = R[4] == kIOHIDValueScaleTypePhysical ? h->sp : h->sc; break;
        }
        break; }
    /* queues */
    case HQ_CREATE: { IOHIDDeviceRef d = oah_hid_dev(R[4]); IOHIDQueueRef q = d ? IOHIDQueueCreate(NULL, d, (CFIndex)R[5], R[6]) : NULL;
        if (q) { s = oah_hid_state(q); r = oah_hid_own(q); s->g = r; CFRelease(q); }
        break; }
    case HQ_DEVICE: { IOHIDQueueRef q = oah_hid_queue(R[3]); r = q ? oah_hid_g(IOHIDQueueGetDevice(q)) : 0; break; }
    case HQ_GETDEPTH: { IOHIDQueueRef q = oah_hid_queue(R[3]); r = q ? (unsigned int)IOHIDQueueGetDepth(q) : 0; break; }
    case HQ_SETDEPTH: { IOHIDQueueRef q = oah_hid_queue(R[3]); if (q) IOHIDQueueSetDepth(q, (CFIndex)R[4]); break; }
    case HQ_ADD: case HQ_REMOVE: case HQ_CONTAINS: { IOHIDQueueRef q = oah_hid_queue(R[3]); IOHIDElementRef e = oah_hid_elem(R[4]);
        if (q && e) { if (w == HQ_ADD) IOHIDQueueAddElement(q, e); else if (w == HQ_REMOVE) IOHIDQueueRemoveElement(q, e); else r = IOHIDQueueContainsElement(q, e); }
        break; }
    case HQ_START: case HQ_STOP: { IOHIDQueueRef q = oah_hid_queue(R[3]); if (q) { if (w == HQ_START) IOHIDQueueStart(q); else IOHIDQueueStop(q); } break; }
    case HQ_REGAVAIL: { IOHIDQueueRef q = oah_hid_queue(R[3]);
        if (!q) break;
        s = oah_hid_state(q); s->g = R[3]; s->availfn = R[4]; s->availctx = R[5];
        IOHIDQueueRegisterValueAvailableCallback(q, R[4] ? oah_hid_cb_avail : NULL, s);
        break; }
    /* calls not answered: the guest's own would read our proxies as its objects */
    case HX_UNSUP: r = kIOReturnUnsupported; break;
    case HX_NULL: r = 0; break;
    case HQ_COPYNEXT: case HQ_COPYNEXTTO: { IOHIDQueueRef q = oah_hid_queue(R[3]); IOHIDValueRef v = NULL;
        if (q) v = w == HQ_COPYNEXT ? IOHIDQueueCopyNextValue(q) : IOHIDQueueCopyNextValueWithTimeout(q, F[1]);
        if (v) { r = oah_hid_gvalue(v); CFRelease(v); }
        break; }
    }
    if (oah_hid_log() && !(w >= HE_COOKIE && w <= HE_TYPEID) && !(w >= HV_ELEMENT && w <= HV_TYPEID))
        fprintf(stderr, "[hid] call %d(%08x %08x %08x) -> %08x\n", w, R[3], R[4], R[5], r);
    g_gt_skip = 1;   /* set last: guest calls made above may run hooked functions, which clear it */
    g_gt_skip_ret = r;
}
#define OAH_HID_FIX(n) static void oah_fix_hid_##n(unsigned int *R) { oah_hid_call(n, R); }
OAH_HID_FIX(HM_CREATE) OAH_HID_FIX(HM_OPEN) OAH_HID_FIX(HM_CLOSE) OAH_HID_FIX(HM_GETPROP) OAH_HID_FIX(HM_SETPROP) OAH_HID_FIX(HM_MATCH)
OAH_HID_FIX(HM_MATCHMULTI) OAH_HID_FIX(HM_COPYDEVICES) OAH_HID_FIX(HM_REGMATCH) OAH_HID_FIX(HM_REGREMOVAL) OAH_HID_FIX(HM_REGREPORT)
OAH_HID_FIX(HM_REGVALUE) OAH_HID_FIX(HM_VALMATCH) OAH_HID_FIX(HM_VALMATCHMULTI) OAH_HID_FIX(HM_SCHEDULE) OAH_HID_FIX(HM_UNSCHEDULE)
OAH_HID_FIX(HM_TYPEID) OAH_HID_FIX(HD_CREATE) OAH_HID_FIX(HD_OPEN) OAH_HID_FIX(HD_CLOSE) OAH_HID_FIX(HD_CONFORMS) OAH_HID_FIX(HD_GETPROP)
OAH_HID_FIX(HD_SETPROP) OAH_HID_FIX(HD_COPYELEMS) OAH_HID_FIX(HD_SCHEDULE) OAH_HID_FIX(HD_UNSCHEDULE) OAH_HID_FIX(HD_REGREMOVAL)
OAH_HID_FIX(HD_REGVALUE) OAH_HID_FIX(HD_REGREPORT) OAH_HID_FIX(HD_VALMATCH) OAH_HID_FIX(HD_VALMATCHMULTI) OAH_HID_FIX(HD_SETVALUE)
OAH_HID_FIX(HD_GETVALUE) OAH_HID_FIX(HD_SETVALUECB) OAH_HID_FIX(HD_GETVALUECB) OAH_HID_FIX(HD_SETREPORT) OAH_HID_FIX(HD_GETREPORT)
OAH_HID_FIX(HD_SERVICE) OAH_HID_FIX(HD_TYPEID) OAH_HID_FIX(HE_COOKIE) OAH_HID_FIX(HE_TYPE) OAH_HID_FIX(HE_COLLTYPE) OAH_HID_FIX(HE_PAGE)
OAH_HID_FIX(HE_USAGE) OAH_HID_FIX(HE_VIRTUAL) OAH_HID_FIX(HE_RELATIVE) OAH_HID_FIX(HE_WRAPPING) OAH_HID_FIX(HE_ARRAY) OAH_HID_FIX(HE_NONLINEAR)
OAH_HID_FIX(HE_PREFSTATE) OAH_HID_FIX(HE_NULLSTATE) OAH_HID_FIX(HE_NAME) OAH_HID_FIX(HE_REPORTID) OAH_HID_FIX(HE_REPORTSIZE)
OAH_HID_FIX(HE_REPORTCOUNT) OAH_HID_FIX(HE_UNIT) OAH_HID_FIX(HE_UNITEXP) OAH_HID_FIX(HE_LMIN) OAH_HID_FIX(HE_LMAX) OAH_HID_FIX(HE_PMIN)
OAH_HID_FIX(HE_PMAX) OAH_HID_FIX(HE_DUPINDEX) OAH_HID_FIX(HE_DEVICE) OAH_HID_FIX(HE_PARENT) OAH_HID_FIX(HE_CHILDREN) OAH_HID_FIX(HE_GETPROP)
OAH_HID_FIX(HE_SETPROP) OAH_HID_FIX(HE_TYPEID) OAH_HID_FIX(HV_CREATEINT) OAH_HID_FIX(HV_CREATEBYTES) OAH_HID_FIX(HV_CREATEBYTESNC)
OAH_HID_FIX(HV_ELEMENT) OAH_HID_FIX(HV_TIMESTAMP) OAH_HID_FIX(HV_LENGTH) OAH_HID_FIX(HV_BYTEPTR) OAH_HID_FIX(HV_INT) OAH_HID_FIX(HV_SCALED)
OAH_HID_FIX(HV_TYPEID) OAH_HID_FIX(HQ_CREATE) OAH_HID_FIX(HQ_DEVICE) OAH_HID_FIX(HQ_GETDEPTH) OAH_HID_FIX(HQ_SETDEPTH) OAH_HID_FIX(HQ_ADD)
OAH_HID_FIX(HQ_REMOVE) OAH_HID_FIX(HQ_CONTAINS) OAH_HID_FIX(HQ_START) OAH_HID_FIX(HQ_STOP) OAH_HID_FIX(HQ_REGAVAIL) OAH_HID_FIX(HQ_SCHEDULE)
OAH_HID_FIX(HQ_UNSCHEDULE) OAH_HID_FIX(HQ_COPYNEXT) OAH_HID_FIX(HQ_COPYNEXTTO) OAH_HID_FIX(HQ_TYPEID) OAH_HID_FIX(HX_UNSUP) OAH_HID_FIX(HX_NULL)
