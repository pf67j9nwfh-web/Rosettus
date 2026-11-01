/* oah_cvl.h - CVDisplayLink for the guest.

   The 10.6 PPC CoreVideo builds a display link from the display's IOFramebuffer (VBL timing in
   shared memory). On Mojave that fails (kCVReturnInvalidDisplay, -6670), so Keynote's transition
   previews in the Inspector never animated (BGBlackOpenGLView stayed black) and anything else
   driven by a display link never ran.

   Emulated here: a link is a guest CFMutableData (so CFRetain/CFRelease/CFGetTypeID work on it)
   whose state lives in a native table. Start creates a guest pthread whose start routine is the
   guest's own CVDisplayLinkGetTypeID - bridged here: entered with a link that is waiting for its
   thread, it becomes the display-link thread, calling the output callback at the display's
   refresh rate (CVTimeStamps written big-endian) until Stop; any other call answers
   CFDataGetTypeID(). OAH_NO_CVLINK=1 leaves the guest CoreVideo alone. OAH_CVTRACE=1 logs. */
#include <mach/mach_time.h>
#define OAH_CVL_MAX 32
typedef struct {
    unsigned int obj;              /* guest CFMutableData */
    unsigned int cb, ctx;          /* output callback + refcon (guest) */
    unsigned int display;
    volatile int running, thread;  /* thread: 0 none, 1 starting, 2 running */
    unsigned long long vt;         /* video time */
    /* the link thread run as guest code (oah_cvl_guest_loop) */
    pthread_t nthr;                /* the native thread under the guest link thread */
    unsigned int retlr;            /* where its start routine returns (thread exit) */
    unsigned long long next, per_abs, nframes;
    unsigned int scale, period;
    unsigned char ts[2][80];
    unsigned int fo[2];
} oah_cvl_t;
static oah_cvl_t g_cvl[OAH_CVL_MAX];
static pthread_mutex_t g_cvl_mu = PTHREAD_MUTEX_INITIALIZER;
static int oah_cvtr(void) { static int t = -1; if (t < 0) t = getenv("OAH_CVTRACE") ? 1 : 0; return t; }
static oah_cvl_t *oah_cvl_find(unsigned int obj) {
    int i;
    if (!obj) return 0;
    for (i = 0; i < OAH_CVL_MAX; i++) if (g_cvl[i].obj == obj) return &g_cvl[i];
    return 0;
}
static double oah_cvl_hz(unsigned int display) {
    double hz = 0;
    void *(*copym)(unsigned int) = (void *(*)(unsigned int))dlsym(RTLD_DEFAULT, "CGDisplayCopyDisplayMode");
    double (*rate)(void *) = (double (*)(void *))dlsym(RTLD_DEFAULT, "CGDisplayModeGetRefreshRate");
    void (*rel)(void *) = (void (*)(void *))dlsym(RTLD_DEFAULT, "CGDisplayModeRelease");
    unsigned int (*mainid)(void) = (unsigned int (*)(void))dlsym(RTLD_DEFAULT, "CGMainDisplayID");
    if (copym && rate) {
        void *m = copym(display ? display : (mainid ? mainid() : 0));
        if (m) { hz = rate(m); if (rel) rel(m); }
    }
    if (hz < 20 || hz > 250) hz = 60;     /* built-in panels report 0 */
    return hz;
}
static void oah_put64be(unsigned char *p, unsigned long long v) { int i; for (i = 7; i >= 0; i--) { p[i] = (unsigned char)v; v >>= 8; } }
static void oah_put32be(unsigned char *p, unsigned int v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
/* 10.6 CVTimeStamp (80 bytes): version, videoTimeScale, videoTime, hostTime, rateScalar, videoRefreshPeriod,
   CVSMPTETime (24), flags, reserved */
static void oah_cvl_stamp(unsigned char *ts, unsigned long long vt, unsigned long long host, unsigned int scale, unsigned int period) {
    double one = 1.0; unsigned long long rb;
    memset(ts, 0, 80);
    oah_put32be(ts + 4, scale);
    oah_put64be(ts + 8, vt);
    oah_put64be(ts + 16, host);
    memcpy(&rb, &one, 8); oah_put64be(ts + 24, rb);
    oah_put64be(ts + 32, period);
    oah_put64be(ts + 64, 0x1 | 0x2 | 0x8 | 0x10);   /* video time, host time, refresh period, rate scalar valid */
}
static void oah_cvl_thread(oah_cvl_t *l) {
    mach_timebase_info_data_t tb;
    double hz = oah_cvl_hz(l->display);
    unsigned int scale = (unsigned int)(hz * 1000 + 0.5), period = 1000;   /* videoTime in 1/(hz*1000) s */
    unsigned long long per_abs, next, now;
    unsigned char ts[2][80];
    unsigned int a[7], fo[2];
    unsigned long n = 0;
    mach_timebase_info(&tb);
    per_abs = (unsigned long long)(1e9 / hz * tb.denom / tb.numer);
    next = mach_absolute_time() + per_abs;
    if (oah_cvtr()) fprintf(stderr, "[cv] link %08x thread running at %.2f Hz, callback %08x\n", l->obj, hz, l->cb);
    l->thread = 2;
    while (l->running) {
        now = mach_absolute_time();
        if (now < next) mach_wait_until(next);
        now = mach_absolute_time();
        if (now > next + 4 * per_abs) next = now;     /* fell behind: don't burst */
        next += per_abs;
        l->vt += period;
        if (!l->cb) continue;
        oah_cvl_stamp(ts[0], l->vt, now, scale, period);
        oah_cvl_stamp(ts[1], l->vt + period, next, scale, period);
        fo[0] = fo[1] = 0;
        a[0] = l->obj; a[1] = (unsigned int)ts[0]; a[2] = (unsigned int)ts[1];
        a[3] = 0; a[4] = 0;                            /* CVOptionFlags flagsIn (64-bit, two GPRs) */
        a[5] = (unsigned int)fo; a[6] = l->ctx;
        oah_callppc_n("cvlink_cb", l->cb, 7, a);
        n++;
    }
    if (oah_cvtr()) fprintf(stderr, "[cv] link %08x thread done after %lu frames\n", l->obj, n);
    l->thread = 0;
}
/* The link thread used to run its whole loop inside one bridge call, calling the output callback
   with oah_callppc_n: the callback ran holding the guest-call lock. Keynote's theme chooser
   deadlocked at launch: QuartzCore's view_display_link (the callback) waited in pthread_mutex_lock
   for a mutex the main thread held while the main thread, in the CGSSetWindowShape bridge, waited
   for the guest-call lock to read its window region (the chooser stayed an invisible window; the
   behaviour sweep failed Keynote's launch). Now, as the guest audio thread does, the link thread
   runs a small guest loop and calls the callback as plain guest code:
       +0  twi (hook RosettusCVLinkWait)  wait for the frame; r3..r9 = arguments, r12 = callback,
                                          LR = +8 (or, the link stopped: LR = the thread's return)
       +4  blr
       +8  mtctr r12 ; +12 bctrl          the callback, without the lock
       +16 b +0
   OAH_CVL_CALLPPC=1: the old way. */
static unsigned int oah_guest_code_alloc(unsigned int bytes);   /* oah_au.h */
static int g_cvl_wait_idx = -1;                 /* the hook's gtrace index (oah_gtrace.h) */
static unsigned int g_cvl_stub;
static oah_cvl_t *oah_cvl_by_thread(void) {
    int i; pthread_t me = pthread_self();
    for (i = 0; i < OAH_CVL_MAX; i++) if (g_cvl[i].obj && g_cvl[i].thread == 2 && pthread_equal(g_cvl[i].nthr, me)) return &g_cvl[i];
    return 0;
}
/* entered from the link thread's start routine (bridged CVDisplayLinkGetTypeID): 1 = the guest loop
   is set up (the bridge returns into it), 0 = run the old native loop */
static int oah_cvl_guest_loop(oah_cvl_t *l, unsigned int *R) {
    mach_timebase_info_data_t tb; double hz;
    if (g_cvl_wait_idx < 0 || getenv("OAH_CVL_CALLPPC")) return 0;
    if (!g_cvl_stub) {
        unsigned int *c = (unsigned int *)oah_guest_code_alloc(5 * 4);
        if (!c) return 0;
        c[0] = OSSwapHostToBigInt32(0x0FE00000 | (0x6000 /* GT_ENTRY */ + (unsigned int)g_cvl_wait_idx));
        c[1] = OSSwapHostToBigInt32(0x4E800020);    /* blr */
        c[2] = OSSwapHostToBigInt32(0x7D8903A6);    /* mtctr r12 */
        c[3] = OSSwapHostToBigInt32(0x4E800421);    /* bctrl */
        c[4] = OSSwapHostToBigInt32(0x4BFFFFF0);    /* b .-16 */
        g_cvl_stub = (unsigned int)c;
    }
    hz = oah_cvl_hz(l->display);
    mach_timebase_info(&tb);
    l->scale = (unsigned int)(hz * 1000 + 0.5); l->period = 1000;
    l->per_abs = (unsigned long long)(1e9 / hz * tb.denom / tb.numer);
    l->next = mach_absolute_time() + l->per_abs;
    l->nframes = 0;
    l->nthr = pthread_self();
    l->retlr = R[0x90 / 4];
    l->thread = 2;
    R[0x90 / 4] = g_cvl_stub;                     /* the blr after the bridge enters the loop */
    if (oah_cvtr()) fprintf(stderr, "[cv] link %08x thread running at %.2f Hz as guest code, callback %08x\n", l->obj, hz, l->cb);
    return 1;
}
/* the hook: wait for the next frame, then hand the callback over (returns r3) */
static unsigned int oah_cvl_wait(unsigned int *R) {
    oah_cvl_t *l = oah_cvl_by_thread();
    unsigned long long now;
    if (!l) { R[3] = 0; return 0; }               /* cannot happen: leave through LR as it is */
    for (;;) {
        if (!l->running) {
            if (oah_cvtr()) fprintf(stderr, "[cv] link %08x thread done after %llu frames\n", l->obj, l->nframes);
            R[0x90 / 4] = l->retlr;
            l->thread = 0;
            return 0;
        }
        now = mach_absolute_time();
        if (now < l->next) mach_wait_until(l->next);
        now = mach_absolute_time();
        if (now > l->next + 4 * l->per_abs) l->next = now;
        l->next += l->per_abs;
        l->vt += l->period;
        if (l->cb) break;
    }
    oah_cvl_stamp(l->ts[0], l->vt, now, l->scale, l->period);
    oah_cvl_stamp(l->ts[1], l->vt + l->period, l->next, l->scale, l->period);
    l->fo[0] = l->fo[1] = 0;
    R[3] = l->obj; R[4] = (unsigned int)l->ts[0]; R[5] = (unsigned int)l->ts[1];
    R[6] = 0; R[7] = 0;                           /* CVOptionFlags flagsIn (64-bit, two GPRs) */
    R[8] = (unsigned int)l->fo; R[9] = l->ctx;
    R[12] = l->cb;
    R[0x90 / 4] = g_cvl_stub + 8;
    l->nframes++;
    return R[3];
}
static int oah_cvl_create(unsigned int *R, unsigned int outp, unsigned int display) {
    static unsigned int fdc;
    unsigned int a[2], obj; int i;
    if (!fdc) fdc = oah_guest_sym("CoreFoundation", "_CFDataCreateMutable");
    if (!fdc) return -6660;
    a[0] = 0; a[1] = 64;
    obj = oah_callppc_n("cvl_obj", fdc, 2, a);
    if (!obj) return -6660;
    pthread_mutex_lock(&g_cvl_mu);
    for (i = 0; i < OAH_CVL_MAX && g_cvl[i].obj; i++) ;
    if (i == OAH_CVL_MAX) {   /* reuse a slot whose thread has finished */
        for (i = 0; i < OAH_CVL_MAX && (g_cvl[i].running || g_cvl[i].thread); i++) ;
    }
    if (i < OAH_CVL_MAX) { memset(&g_cvl[i], 0, sizeof g_cvl[i]); g_cvl[i].obj = obj; g_cvl[i].display = display; }
    pthread_mutex_unlock(&g_cvl_mu);
    if (i == OAH_CVL_MAX) return -6660;
    if (outp) *(unsigned int *)outp = OSSwapHostToBigInt32(obj);
    if (oah_cvtr()) fprintf(stderr, "[cv] create (display %u) -> link %08x\n", display, obj);
    (void)R;
    return 0;
}
static int oah_cvl_call(const char *n, unsigned int *R) {
    static int off = -1;
    double *F = (double *)((unsigned char *)R + 0x1e8);
    oah_cvl_t *l;
    if (strncmp(n, "CVDisplayLink", 13)) return 0;
    if (off < 0) off = getenv("OAH_NO_CVLINK") ? 1 : 0;
    if (off) return 0;
    n += 13;
    if (!strcmp(n, "GetTypeID")) {
        l = oah_cvl_find(R[3]);
        if (l && l->thread == 1) {                       /* the link's thread */
            if (!oah_cvl_guest_loop(l, R)) { oah_cvl_thread(l); R[3] = 0; }
            return 1;
        }
        {   static unsigned int f; if (!f) f = oah_guest_sym("CoreFoundation", "_CFDataGetTypeID");
            R[3] = f ? oah_callppc_n("cvl_tid", f, 0, R) : 0; }
        return 1;
    }
    if (!strcmp(n, "CreateWithCGDisplay")) { R[3] = (unsigned int)oah_cvl_create(R, R[4], R[3]); return 1; }
    if (!strcmp(n, "CreateWithActiveCGDisplays")) { R[3] = (unsigned int)oah_cvl_create(R, R[3], 0); return 1; }
    if (!strcmp(n, "CreateWithCGDisplays")) {          /* (CGDirectDisplayID *, count, link *) */
        unsigned int d = R[3] && R[4] ? OSSwapBigToHostInt32(*(unsigned int *)R[3]) : 0;
        R[3] = (unsigned int)oah_cvl_create(R, R[5], d); return 1;
    }
    if (!strcmp(n, "CreateWithOpenGLDisplayMask")) { R[3] = (unsigned int)oah_cvl_create(R, R[4], 0); return 1; }
    if (!strcmp(n, "Retain") || !strcmp(n, "Release")) {
        static unsigned int fr, fl;
        if (!fr) { fr = oah_guest_sym("CoreFoundation", "_CFRetain"); fl = oah_guest_sym("CoreFoundation", "_CFRelease"); }
        l = oah_cvl_find(R[3]);
        if (!R[3]) return 1;
        if (n[2] == 'l' && l) {     /* Release: last reference? (CFGetRetainCount) */
            static unsigned int frc; unsigned int a[1];
            if (!frc) frc = oah_guest_sym("CoreFoundation", "_CFGetRetainCount");
            a[0] = R[3];
            if (frc && (int)oah_callppc_n("cvl_rc", frc, 1, a) <= 1) {
                l->running = 0;
                while (l->thread) usleep(2000);
                if (oah_cvtr()) fprintf(stderr, "[cv] link %08x released\n", l->obj);
                l->obj = 0;
            }
        }
        {   unsigned int a[1]; a[0] = R[3];
            oah_callppc_n("cvl_rr", n[2] == 't' ? fr : fl, 1, a); }
        if (n[2] == 'l') R[3] = 0;
        return 1;
    }
    if (!(l = oah_cvl_find(R[n[0] == 'G' && !strcmp(n, "GetNominalOutputVideoRefreshPeriod") ? 4 :
                              (!strcmp(n, "GetOutputVideoLatency") ? 4 : 3)]))) {
        fprintf(stderr, "[cv] %s on a link we don't know (%08x)\n", n, R[3]);
        R[3] = (unsigned int)-6661;                      /* kCVReturnInvalidArgument */
        return 1;
    }
    if (!strcmp(n, "SetOutputCallback")) {             /* (link, cb, ctx) */
        l->cb = R[4]; l->ctx = R[5];
        if (oah_cvtr()) fprintf(stderr, "[cv] link %08x callback %08x ctx %08x\n", l->obj, l->cb, l->ctx);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "Start")) {
        static unsigned int fpc;
        unsigned int a[4], tid = 0;
        if (l->running) { R[3] = 0; return 1; }
        while (l->thread) usleep(1000);                 /* a previous thread still winding down */
        if (!fpc) fpc = oah_guest_sym("libSystem", "_pthread_create");
        {   static unsigned int fgt; if (!fgt) fgt = oah_guest_sym("CoreVideo", "_CVDisplayLinkGetTypeID");
            if (!fpc || !fgt) { R[3] = (unsigned int)-6660; return 1; }
            l->running = 1; l->thread = 1;
            a[0] = (unsigned int)&tid; a[1] = 0; a[2] = fgt; a[3] = l->obj;
            if (oah_callppc_n("cvl_pthread_create", fpc, 4, a)) { l->running = 0; l->thread = 0; R[3] = (unsigned int)-6660; return 1; }
        }
        if (oah_cvtr()) fprintf(stderr, "[cv] link %08x started\n", l->obj);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "Stop")) {
        l->running = 0;
        /* don't wait when Stop comes from the callback itself (it runs on the link thread) */
        if (oah_cvtr()) fprintf(stderr, "[cv] link %08x stopped\n", l->obj);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "IsRunning")) { R[3] = l->running ? 1 : 0; return 1; }
    if (!strcmp(n, "SetPaused")) { R[3] = 0; return 1; }
    if (!strcmp(n, "SetCurrentCGDisplay")) { l->display = R[4]; R[3] = 0; return 1; }
    if (!strcmp(n, "SetCurrentCGDisplayFromOpenGLContext")) { R[3] = 0; return 1; }
    if (!strcmp(n, "GetCurrentCGDisplay")) {
        unsigned int (*mainid)(void) = (unsigned int (*)(void))dlsym(RTLD_DEFAULT, "CGMainDisplayID");
        R[3] = l->display ? l->display : (mainid ? mainid() : 0); return 1;
    }
    if (!strcmp(n, "GetNominalOutputVideoRefreshPeriod") || !strcmp(n, "GetOutputVideoLatency")) {
        /* CVTime (16 bytes) returned through the hidden pointer in r3: timeValue, timeScale, flags */
        unsigned char *o = (unsigned char *)R[3];
        if (o) {
            memset(o, 0, 16);
            if (n[3] == 'N') { oah_put64be(o, 1000); oah_put32be(o + 8, (unsigned int)(oah_cvl_hz(l->display) * 1000 + 0.5)); }
            else oah_put32be(o + 8, 1000);
        }
        return 1;
    }
    if (!strcmp(n, "GetActualOutputVideoRefreshPeriod")) { F[1] = 1.0 / oah_cvl_hz(l->display); return 1; }
    if (!strcmp(n, "GetCurrentTime")) {                /* (link, CVTimeStamp *out) */
        double hz = oah_cvl_hz(l->display);
        if (R[4]) oah_cvl_stamp((unsigned char *)R[4], l->vt, mach_absolute_time(), (unsigned int)(hz * 1000 + 0.5), 1000);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "TranslateTime")) {                 /* (link, const CVTimeStamp *in, CVTimeStamp *out) */
        if (R[4] && R[5]) memcpy((void *)R[5], (void *)R[4], 80);
        R[3] = 0; return 1;
    }
    R[3] = 0;
    return 1;
}
