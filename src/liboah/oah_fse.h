/* ---- FSEvents for PowerPC programs ----
   The guest's CarbonCore talks to the 10.6 fseventsd protocol, which Mojave's fseventsd does not
   speak: a stream started fine and never delivered an event. The FSEventStream calls are answered
   with native streams instead. A guest stream ref is a struct of ours; the native stream runs on a
   dispatch queue of its own and queues its events; a guest thread (hook "RosettusFSEventsPump")
   signals a guest run-loop source on each run loop the stream was scheduled on, and that source's
   perform routine (hook "RosettusFSEventsPerform") calls the guest's callback there, on the thread
   that runs the loop, as FSEvents does. OAH_NO_FSEVENTS=1 leaves FSEvents to the guest;
   OAH_FSELOG=1 logs the calls. */
static int oah_want_fsevents(void) { return !getenv("OAH_NO_FSEVENTS"); }
static int oah_fse_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_FSELOG") ? 1 : 0; return v; }
static int g_fse_pump_idx = -1, g_fse_perf_idx = -1;
typedef struct oah_fse_ev { struct oah_fse_ev *next; char *path; UInt32 flags; UInt64 id; } oah_fse_ev;
#define OAH_FSE_MAGIC 0x46534552u
#define OAH_FSE_RL 8
typedef struct oah_fse {
    unsigned int magic; int refs, started, invalid, signaled;
    FSEventStreamRef n;
    unsigned int cb, info, crel; UInt32 flags;
    CFArrayRef paths; FSEventStreamEventId since; CFTimeInterval latency;
    unsigned int src, rl[OAH_FSE_RL]; int nrl;
    unsigned int dq;                       /* a guest dispatch queue (FSEventStreamSetDispatchQueue: fontd) */
    oah_fse_ev *head, *tail;
    struct oah_fse *nextall;
} oah_fse;
static pthread_mutex_t g_fse_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fse_cv = PTHREAD_COND_INITIALIZER;
static oah_fse *g_fse_all;
static int g_fse_thread, g_fse_wake;
static unsigned int g_fse_perfstub, g_fse_async;
static dispatch_queue_t g_fse_q;
enum { GF_COUNT, GF_GET, GF_SRCCREATE, GF_ADDSRC, GF_REMSRC, GF_SIGNAL, GF_WAKE, GF_SRCINVAL, GF_RELEASE, GF_STRCREATE, GF_N };
static unsigned int g_gf[GF_N];
static int oah_fse_gcf(void) {
    static const char *n[GF_N] = { "_CFArrayGetCount", "_CFArrayGetValueAtIndex", "_CFRunLoopSourceCreate", "_CFRunLoopAddSource",
        "_CFRunLoopRemoveSource", "_CFRunLoopSourceSignal", "_CFRunLoopWakeUp", "_CFRunLoopSourceInvalidate", "_CFRelease",
        "_CFStringCreateWithCString" };
    int i;
    for (i = 0; i < GF_N; i++) if (!g_gf[i] && !(g_gf[i] = oah_guest_sym("CoreFoundation", n[i]))) return 0;
    return 1;
}
static unsigned int oah_gf(int f, int n, unsigned int a0, unsigned int a1, unsigned int a2) {
    unsigned int a[3]; a[0] = a0; a[1] = a1; a[2] = a2;
    return oah_callppc_n("fse_cf", g_gf[f], n, a);
}
static unsigned int oah_fse_be(unsigned int a) { return (a && oah_readable(a, 4)) ? OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)a) : 0; }
static oah_fse *oah_fse_get(unsigned int g) {
    oah_fse *s = (oah_fse *)(uintptr_t)g;
    if (!g || !oah_readable(g, 4) || s->magic != OAH_FSE_MAGIC) return NULL;
    return s;
}
/* native: events arrive on our dispatch queue */
static void oah_fse_ncb(ConstFSEventStreamRef ns, void *info, size_t n, void *paths, const FSEventStreamEventFlags fl[], const FSEventStreamEventId ids[]) {
    oah_fse *s = (oah_fse *)info; size_t i; char **p = (char **)paths;
    (void)ns;
    pthread_mutex_lock(&g_fse_mu);
    for (i = 0; i < n; i++) {
        oah_fse_ev *e = (oah_fse_ev *)calloc(1, sizeof *e);
        if (!e) break;
        e->path = strdup(p[i] ? p[i] : ""); e->flags = fl[i]; e->id = ids[i];
        if (s->tail) s->tail->next = e; else s->head = e;
        s->tail = e;
    }
    g_fse_wake = 1;
    pthread_cond_signal(&g_fse_cv);
    pthread_mutex_unlock(&g_fse_mu);
}
/* the guest pump thread: wakes the run loops of streams with events (never returns) */
static void oah_fix_fse_pump(unsigned int *R) {
    (void)R;
    for (;;) {
        unsigned int sig[64], rl[64 * OAH_FSE_RL], dq[64], dqs[64]; int ns = 0, nr = 0, nd = 0, i, k;
        oah_fse *s;
        pthread_mutex_lock(&g_fse_mu);
        while (!g_fse_wake) pthread_cond_wait(&g_fse_cv, &g_fse_mu);
        g_fse_wake = 0;
        for (s = g_fse_all; s && ns < 64 && nd < 64; s = s->nextall)
            if (s->head && !s->signaled && !s->invalid) {
                if (s->dq) { s->signaled = 1; dq[nd] = s->dq; dqs[nd++] = (unsigned int)(uintptr_t)s; }
                else if (s->src) {
                    s->signaled = 1; sig[ns++] = s->src;
                    for (k = 0; k < s->nrl; k++) rl[nr++] = s->rl[k];
                }
            }
        pthread_mutex_unlock(&g_fse_mu);
        for (i = 0; i < nd; i++) {   /* dispatch_async_f(queue, stream, perform) */
            unsigned int a[3]; a[0] = dq[i]; a[1] = dqs[i]; a[2] = g_fse_perfstub;
            if (g_fse_async && g_fse_perfstub) oah_callppc_n("fse_async", g_fse_async, 3, a);
        }
        for (i = 0; i < ns; i++) oah_gf(GF_SIGNAL, 1, sig[i], 0, 0);
        for (i = 0; i < nr; i++) oah_gf(GF_WAKE, 1, rl[i], 0, 0);
    }
}
static void oah_fse_start_thread(void) {
    unsigned int pc, a[4], *t;
    static unsigned int tid;
    if (g_fse_thread || g_fse_pump_idx < 0) return;
    g_fse_thread = 2;
    if (!(pc = oah_guest_sym("libSystem", "_pthread_create")) || !(t = (unsigned int *)oah_guest_code_alloc(8))) return;
    t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_fse_pump_idx));
    t[1] = OSSwapHostToBigInt32(0x4e800020);
    a[0] = (unsigned int)(uintptr_t)&tid; a[1] = 0; a[2] = (unsigned int)(uintptr_t)t; a[3] = 0;
    if (!oah_callppc_n("fse_pthread_create", pc, 4, a)) g_fse_thread = 1;
    if (oah_fse_log()) fprintf(stderr, "[fsevents] pump thread %s\n", g_fse_thread == 1 ? "started" : "could not start");
}
/* delivers a stream's queued events to the guest callback (on the guest thread running its loop) */
static void oah_fse_deliver(oah_fse *s) {
    oah_fse_ev *e, *list; unsigned int n = 0, i, a[6], gpaths = 0;
    unsigned int *pp = NULL, *fl = NULL; UInt64 *ids = NULL; CFMutableArrayRef ca = NULL;
    pthread_mutex_lock(&g_fse_mu);
    list = s->head; s->head = s->tail = NULL; s->signaled = 0;
    pthread_mutex_unlock(&g_fse_mu);
    for (e = list; e; e = e->next) n++;
    if (!n) return;
    fl = (unsigned int *)calloc(n, 4); ids = (UInt64 *)calloc(n, 8);
    if (s->flags & kFSEventStreamCreateFlagUseCFTypes) ca = CFArrayCreateMutable(NULL, n, &kCFTypeArrayCallBacks);
    else pp = (unsigned int *)calloc(n, 4);
    for (i = 0, e = list; e; e = e->next, i++) {
        fl[i] = OSSwapHostToBigInt32(e->flags); ids[i] = OSSwapHostToBigInt64(e->id);
        if (ca) { CFStringRef p = CFStringCreateWithCString(NULL, e->path, kCFStringEncodingUTF8); if (p) { CFArrayAppendValue(ca, p); CFRelease(p); } }
        else pp[i] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)e->path);
    }
    gpaths = ca ? oah_midi_gplist(ca) : (unsigned int)(uintptr_t)pp;
    if (oah_fse_log()) fprintf(stderr, "[fsevents] %u event(s) to the guest (first %s)\n", n, list->path);
    if (s->cb && gpaths && !s->invalid) {
        a[0] = (unsigned int)(uintptr_t)s; a[1] = s->info; a[2] = n; a[3] = gpaths; a[4] = (unsigned int)(uintptr_t)fl; a[5] = (unsigned int)(uintptr_t)ids;
        oah_callppc_n("fse_callback", s->cb, 6, a);
    }
    if (ca) { if (gpaths) oah_gf(GF_RELEASE, 1, gpaths, 0, 0); CFRelease(ca); }
    while (list) { e = list->next; free(list->path); free(list); list = e; }
    free(pp); free(fl); free(ids);
}
/* the guest run-loop source's perform routine: info = our stream */
static void oah_fix_fse_perform(unsigned int *R) {
    oah_fse *s = oah_fse_get(R[3]);
    if (s) oah_fse_deliver(s);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
/* a guest CFArray of CFStrings as a native one */
static CFArrayRef oah_fse_npaths(unsigned int g) {
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    unsigned int c, i;
    if (!g || !oah_fse_gcf()) return a;
    c = oah_gf(GF_COUNT, 1, g, 0, 0);
    for (i = 0; i < c && i < 4096; i++) {
        CFStringRef s = oah_midi_nstr(oah_gf(GF_GET, 2, g, i, 0));
        if (s) { CFArrayAppendValue(a, s); CFRelease(s); }
    }
    return a;
}
static void oah_fse_invalidate(oah_fse *s) {
    if (s->invalid) return;
    s->invalid = 1;
    if (s->n) { if (s->started) FSEventStreamStop(s->n); FSEventStreamInvalidate(s->n); FSEventStreamRelease(s->n); s->n = NULL; }
    if (s->src) {
        oah_gf(GF_SRCINVAL, 1, s->src, 0, 0); oah_gf(GF_RELEASE, 1, s->src, 0, 0); s->src = 0;
    }
    s->nrl = 0;
}
/* the perform routine as a guest function (a run-loop source's perform, or a dispatch function) */
static unsigned int oah_fse_perfstub(void) {
    unsigned int *stub;
    if (!g_fse_perfstub && (stub = (unsigned int *)oah_guest_code_alloc(8))) {
        stub[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_fse_perf_idx));
        stub[1] = OSSwapHostToBigInt32(0x4e800020);
        g_fse_perfstub = (unsigned int)(uintptr_t)stub;
    }
    return g_fse_perfstub;
}
enum { FS_SETQUEUE, FS_CREATE, FS_SCHEDULE, FS_UNSCHEDULE, FS_START, FS_STOP, FS_INVALIDATE, FS_RELEASE, FS_RETAIN, FS_LATEST, FS_CURRENT,
       FS_PATHS, FS_FLUSHSYNC, FS_FLUSHASYNC, FS_DEVICE, FS_DESCR, FS_SHOW, FS_N };
static const char *const g_fs_name[FS_N] = { "FSEventStreamSetDispatchQueue", "FSEventStreamCreate", "FSEventStreamScheduleWithRunLoop", "FSEventStreamUnscheduleFromRunLoop",
    "FSEventStreamStart", "FSEventStreamStop", "FSEventStreamInvalidate", "FSEventStreamRelease", "FSEventStreamRetain",
    "FSEventStreamGetLatestEventId", "FSEventsGetCurrentEventId", "FSEventStreamCopyPathsBeingWatched", "FSEventStreamFlushSync",
    "FSEventStreamFlushAsync", "FSEventStreamGetDeviceBeingWatched", "FSEventStreamCopyDescription", "FSEventStreamShow" };
static void oah_fse_call(int which, unsigned int *R) {
    unsigned int r = 0; oah_fse *s = which == FS_CREATE || which == FS_CURRENT ? NULL : oah_fse_get(R[3]);
    g_gt_skip = 1;
    if (which != FS_CREATE && which != FS_CURRENT && !s) {   /* not one of ours (a stream made before the hooks armed): the guest's */
        g_gt_skip = 0;
        if (oah_fse_log()) fprintf(stderr, "[fsevents] %s(%08x): not ours\n", g_fs_name[which], R[3]);
        return;
    }
    if (!oah_fse_gcf()) { g_gt_skip = 0; return; }
    switch (which) {
    case FS_CREATE: {   /* (alloc, callback, context*, paths, sinceWhen (r7:r8), latency (f1), flags (stack)) */
        double *F = (double *)(R + 0x1e8 / 4);
        unsigned int ctx = R[5];
        s = (oah_fse *)calloc(1, sizeof *s);
        if (!s) break;
        s->magic = OAH_FSE_MAGIC; s->refs = 1; s->cb = R[4];
        s->since = ((UInt64)R[7] << 32) | R[8]; s->latency = F[1];
        s->flags = oah_fse_be(R[1] + 24 + 4 * 8);
        if (ctx) {
            unsigned int ret = oah_fse_be(ctx + 8);
            s->info = oah_fse_be(ctx + 4); s->crel = oah_fse_be(ctx + 12);
            if (ret) { unsigned int a[1]; a[0] = s->info; s->info = oah_callppc_n("fse_ctx_retain", ret, 1, a); }
        }
        s->paths = oah_fse_npaths(R[6]);
        pthread_mutex_lock(&g_fse_mu); s->nextall = g_fse_all; g_fse_all = s; pthread_mutex_unlock(&g_fse_mu);
        r = (unsigned int)(uintptr_t)s;
        if (oah_fse_log()) fprintf(stderr, "[fsevents] stream %08x: %d path(s), latency %g, flags %x\n", r, (int)CFArrayGetCount(s->paths), s->latency, (unsigned)s->flags);
        break; }
    case FS_SCHEDULE: {   /* (stream, runLoop, mode) */
        if (s->invalid) break;
        if (!s->src) {
            unsigned int *ctx = (unsigned int *)calloc(10, 4), stub = oah_fse_perfstub();
            if (!ctx || !stub) { free(ctx); break; }
            ctx[1] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)s); ctx[9] = OSSwapHostToBigInt32(stub);
            s->src = oah_gf(GF_SRCCREATE, 3, 0, 0, (unsigned int)(uintptr_t)ctx);   /* CF copies the context */
            free(ctx);
        }
        if (s->src) {
            oah_gf(GF_ADDSRC, 3, R[4], s->src, R[5]);
            pthread_mutex_lock(&g_fse_mu);
            if (s->nrl < OAH_FSE_RL) s->rl[s->nrl++] = R[4];
            pthread_mutex_unlock(&g_fse_mu);
        }
        oah_fse_start_thread();
        break; }
    case FS_UNSCHEDULE: {
        int k;
        if (s->src) oah_gf(GF_REMSRC, 3, R[4], s->src, R[5]);
        pthread_mutex_lock(&g_fse_mu);
        for (k = 0; k < s->nrl; k++) if (s->rl[k] == R[4]) { s->rl[k] = s->rl[--s->nrl]; break; }
        pthread_mutex_unlock(&g_fse_mu);
        break; }
    case FS_START:
        if (s->invalid) break;
        if (!s->n) {
            FSEventStreamContext c = { 0, s, NULL, NULL, NULL };
            FSEventStreamCreateFlags nf = s->flags & (kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot |
                                                      kFSEventStreamCreateFlagIgnoreSelf | 0x10 /* FileEvents (10.7) */);
            if (!g_fse_q) g_fse_q = dispatch_queue_create("com.rosettus.fsevents", NULL);
            s->n = FSEventStreamCreate(NULL, oah_fse_ncb, &c, s->paths, s->since, s->latency, nf);
            if (s->n) FSEventStreamSetDispatchQueue(s->n, g_fse_q);
        }
        r = s->n && FSEventStreamStart(s->n);
        if (r) s->started = 1;
        break;
    case FS_SETQUEUE: {   /* (stream, queue): the callbacks go to the app's dispatch queue */
        static unsigned int fret, frel;
        unsigned int a[1];
        if (!g_fse_async) { g_fse_async = oah_guest_sym("libSystem", "_dispatch_async_f"); fret = oah_guest_sym("libSystem", "_dispatch_retain"); frel = oah_guest_sym("libSystem", "_dispatch_release"); }
        if (R[4] && (!g_fse_async || !oah_fse_perfstub())) break;
        if (R[4] && fret) { a[0] = R[4]; oah_callppc_n("fse_dq_retain", fret, 1, a); }
        if (s->dq && frel) { a[0] = s->dq; oah_callppc_n("fse_dq_release", frel, 1, a); }
        s->dq = R[4];
        if (s->dq) oah_fse_start_thread();
        break; }
    case FS_STOP: if (s->n && s->started) { FSEventStreamStop(s->n); s->started = 0; } break;
    case FS_INVALIDATE: oah_fse_invalidate(s); break;
    case FS_RETAIN: s->refs++; break;
    case FS_RELEASE:
        if (--s->refs <= 0) {
            oah_fse_invalidate(s);
            if (s->crel) { unsigned int a[1]; a[0] = s->info; oah_callppc_n("fse_ctx_release", s->crel, 1, a); s->crel = 0; }
            if (s->paths) { CFRelease(s->paths); s->paths = NULL; }
            s->magic = 0;   /* the struct stays (the pump may still hold it): a few bytes per stream */
        }
        break;
    case FS_LATEST: { UInt64 v = s->n ? FSEventStreamGetLatestEventId(s->n) : s->since; r = (unsigned int)(v >> 32); R[4] = (unsigned int)v; break; }
    case FS_CURRENT: { UInt64 v = FSEventsGetCurrentEventId(); r = (unsigned int)(v >> 32); R[4] = (unsigned int)v; break; }
    case FS_PATHS: r = oah_midi_gplist(s->paths); break;
    case FS_FLUSHSYNC:
        if (s->n) FSEventStreamFlushSync(s->n);
        dispatch_sync(g_fse_q, ^{});   /* the callbacks for the flushed events have queued them */
        oah_fse_deliver(s);
        break;
    case FS_FLUSHASYNC: { UInt64 v = s->n ? FSEventStreamFlushAsync(s->n) : 0; r = (unsigned int)(v >> 32); R[4] = (unsigned int)v; break; }
    case FS_DEVICE: r = 0; break;
    case FS_DESCR: r = oah_gf(GF_STRCREATE, 3, 0, (unsigned int)(uintptr_t)"FSEventStream (Rosettus)", kCFStringEncodingUTF8); break;
    case FS_SHOW: break;
    }
    if (oah_fse_log() && which != FS_RETAIN) fprintf(stderr, "[fsevents] %s -> %x\n", g_fs_name[which], r);
    g_gt_skip_ret = r;
}
#define OAH_FS_FIX(n) static void oah_fix_fs_##n(unsigned int *R) { oah_fse_call(FS_##n, R); }
OAH_FS_FIX(SETQUEUE) OAH_FS_FIX(CREATE) OAH_FS_FIX(SCHEDULE) OAH_FS_FIX(UNSCHEDULE) OAH_FS_FIX(START) OAH_FS_FIX(STOP) OAH_FS_FIX(INVALIDATE)
OAH_FS_FIX(RELEASE) OAH_FS_FIX(RETAIN) OAH_FS_FIX(LATEST) OAH_FS_FIX(CURRENT) OAH_FS_FIX(PATHS) OAH_FS_FIX(FLUSHSYNC)
OAH_FS_FIX(FLUSHASYNC) OAH_FS_FIX(DEVICE) OAH_FS_FIX(DESCR) OAH_FS_FIX(SHOW)
