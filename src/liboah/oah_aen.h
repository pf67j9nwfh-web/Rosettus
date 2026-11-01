#include <sys/resource.h>
/* ===== Apple Events from native Mojave processes (osascript, Finder, the Dock) =====
   Mojave's senders look the target's port up in appleeventsd; the guest AE manager can't check in
   there (and on 10.6 it checked in with coreservicesd). So the translator takes the native side:
   native AEGetRegisteredMachPort() gives a port registered with appleeventsd for this process, a
   host thread receives on it and decodes with native AEDecodeMessage, and each event is rebuilt in
   the guest (parameters travel as flattened descriptors, which are big-endian on both sides) and
   self-sent there (kCurrentProcess: dispatched directly to the app's handler) on the main thread,
   from the event pull (CGSGetNextEventRecordInternal). The guest reply's parameters go back the same
   way into the native reply. OAH_NO_AENATIVE=1 disables, OAH_AETRACE=1 logs. */
#define OAH_AEN_MAXP 32
typedef struct { unsigned int descriptorType; void *dataHandle; } oah_aed_t;   /* native oah_aed_t */
typedef struct {
    unsigned int cls, id;
    int wantreply;                 /* the native sender waits for a reply */
    int np; unsigned int kw[OAH_AEN_MAXP]; void *d[OAH_AEN_MAXP]; unsigned int len[OAH_AEN_MAXP];   /* params in */
    int nr; unsigned int rkw[OAH_AEN_MAXP]; void *rd[OAH_AEN_MAXP]; unsigned int rlen[OAH_AEN_MAXP]; /* reply params out */
    int err, done;
    short rtid; int tran, hasrt;   /* keyReturnIDAttr / keyTransactionIDAttr of the native event */
    int picked;                    /* the guest has started handling it */
    int posted; double posted_at;  /* announced to the app as a Carbon event (oah_aen_dispatch) */
} oah_aen_t;
static pthread_mutex_t g_aen_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aen_cv = PTHREAD_COND_INITIALIZER;
static oah_aen_t *volatile g_aen_cur;          /* one event in flight at a time */
static volatile double g_aen_first_pull;           /* when the guest first pulled events */
static int oah_aetr(void) { static int t = -1; if (t < 0) t = (getenv("OAH_AETRACE") || access("/tmp/oah_aetrace", 0) == 0) ? 1 : 0; return t; }
static double g_aen_start;            /* when the event port was set up (process start, near enough) */
static void oah_ev_wake(void);
static struct {
    int (*decode)(mach_msg_header_t *, oah_aed_t *, oah_aed_t *);
    int (*send)(const oah_aed_t *, oah_aed_t *, int, long);
    int (*count)(const oah_aed_t *, long *);
    int (*nth)(const oah_aed_t *, long, unsigned int, unsigned int *, oah_aed_t *);
    long (*fsize)(const oah_aed_t *);
    int (*flat)(const oah_aed_t *, void *, long, long *);
    int (*unflat)(const void *, oah_aed_t *);
    int (*putp)(oah_aed_t *, unsigned int, const oah_aed_t *);
    int (*geta)(const oah_aed_t *, unsigned int, unsigned int, unsigned int *, void *, long, long *);
    int (*disp)(oah_aed_t *);
    mach_port_t (*regport)(void);
} g_nae;
static int oah_aen_nload(void) {
    void *ae = dlopen("/System/Library/Frameworks/CoreServices.framework/Frameworks/AE.framework/AE", RTLD_LAZY | RTLD_GLOBAL);
    if (!ae) return 0;
    *(void **)&g_nae.decode = dlsym(ae, "AEDecodeMessage");
    *(void **)&g_nae.send = dlsym(ae, "AESendMessage");
    *(void **)&g_nae.count = dlsym(ae, "AECountItems");
    *(void **)&g_nae.nth = dlsym(ae, "AEGetNthDesc");
    *(void **)&g_nae.fsize = dlsym(ae, "AESizeOfFlattenedDesc");
    *(void **)&g_nae.flat = dlsym(ae, "AEFlattenDesc");
    *(void **)&g_nae.unflat = dlsym(ae, "AEUnflattenDesc");
    *(void **)&g_nae.putp = dlsym(ae, "AEPutParamDesc");
    *(void **)&g_nae.geta = dlsym(ae, "AEGetAttributePtr");
    *(void **)&g_nae.disp = dlsym(ae, "AEDisposeDesc");
    *(void **)&g_nae.regport = dlsym(ae, "AEGetRegisteredMachPort");
    return g_nae.decode && g_nae.send && g_nae.count && g_nae.nth && g_nae.fsize && g_nae.flat && g_nae.unflat &&
           g_nae.putp && g_nae.geta && g_nae.disp && g_nae.regport;
}
/* Hand one event to the guest main thread and wait (at most `secs`). 1 = done; 0 = timed out (x is then
   owned by the guest side and must not be freed). */
static int oah_aen_run(oah_aen_t *x, int secs) {
    int ok;
    pthread_mutex_lock(&g_aen_mu);
    while (g_aen_cur) pthread_cond_wait(&g_aen_cv, &g_aen_mu);
    g_aen_cur = x;
    pthread_mutex_unlock(&g_aen_mu);
    oah_ev_wake();
    pthread_mutex_lock(&g_aen_mu);
    {   struct timespec ts; struct timeval tv; gettimeofday(&tv, 0);
        ts.tv_sec = tv.tv_sec + secs; ts.tv_nsec = tv.tv_usec * 1000;
        while (!x->done) if (pthread_cond_timedwait(&g_aen_cv, &g_aen_mu, &ts)) break; }
    ok = x->done;
    if (!ok) { x->err = -1712; if (g_aen_cur == x) g_aen_cur = 0; }
    pthread_mutex_unlock(&g_aen_mu);
    return ok;
}
static void oah_aen_free(oah_aen_t *x) {
    int i;
    for (i = 0; i < x->np; i++) free(x->d[i]);
    for (i = 0; i < x->nr; i++) free(x->rd[i]);
    free(x);
}
/* SL AppKit's raw odoc path (no Cocoa scripting loaded yet, or a list of files) left Pages spinning in a
   drawing loop; once the scripting terminology has been asked for ('ascr'/'gdte', as osascript does
   first), a single-file odoc runs as an "open" script command and works. */
static volatile int g_aen_gdte_done;
static void oah_aen_gdte_once(void) {
    oah_aen_t *x;
    if (g_aen_gdte_done || getenv("OAH_NO_GDTE")) return;
    g_aen_gdte_done = 1;
    x = (oah_aen_t *)calloc(1, sizeof *x);
    x->cls = 'ascr'; x->id = 'gdte';
    if (oah_aen_run(x, 60)) oah_aen_free(x);
}
/* Mojave's Launch Services passes documents as bookmark data ('bmrk', the 10.8+ "book" format).
   The 10.6 guest cannot resolve those: TextWrangler's odoc handler got no file and crashed, other
   apps silently opened nothing. Resolve them here and pass a file URL ('furl'), which the guest's
   AE coercions turn into an FSRef or alias as 10.6 apps expect. */
static void oah_aen_carbon_doc(oah_aed_t *d);
static void oah_aen_bmrk_to_furl(oah_aed_t *d) {
    static long (*dsz)(const oah_aed_t *);
    static int (*dget)(const oah_aed_t *, void *, long);
    static int (*mk)(unsigned int, const void *, long, oah_aed_t *);
    long n; void *b; CFDataRef bd; CFURLRef u; Boolean stale = 0;
    if (d->descriptorType != 'bmrk' || getenv("OAH_NO_BMRK")) return;
    if (!dsz) *(void **)&dsz = dlsym(RTLD_DEFAULT, "AEGetDescDataSize");
    if (!dget) *(void **)&dget = dlsym(RTLD_DEFAULT, "AEGetDescData");
    if (!mk) *(void **)&mk = dlsym(RTLD_DEFAULT, "AECreateDesc");
    if (!dsz || !dget || !mk || (n = dsz(d)) <= 0 || !(b = malloc(n))) return;
    if (dget(d, b, n)) { free(b); return; }
    bd = CFDataCreate(0, (const UInt8 *)b, n);
    free(b);
    u = bd ? CFURLCreateByResolvingBookmarkData(0, bd, (1UL << 8) | (1UL << 9) /* without UI, without mounting */, 0, 0, &stale, 0) : 0;
    if (bd) CFRelease(bd);
    if (u) {
        char path[PATH_MAX];
        if (CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path)) {
            CFURLRef f = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)path, strlen(path), false);
            CFStringRef us = f ? CFURLGetString(f) : 0;
            char ub[PATH_MAX * 3];
            oah_aed_t nd = { 0, 0 };
            if (us && CFStringGetCString(us, ub, sizeof ub, kCFStringEncodingUTF8)) {
                if (!strncmp(ub, "file://localhost/", 17)) memmove(ub + 7, ub + 16, strlen(ub + 16) + 1);
                if (!mk('furl', ub, (long)strlen(ub), &nd)) {
                    if (oah_aetr()) fprintf(stderr, "[aen] bookmark -> %s\n", ub);
                    g_nae.disp(d); *d = nd;
                }
            }
            if (f) CFRelease(f);
        }
        CFRelease(u);
    }
}
/* Microsoft OLE (Word with Equation Editor or Organization Chart) runs its RPC over Apple Events:
   a handler for an incoming 'LRPC'/'Sync' makes its own 'Sync' call and waits, in a nested event
   loop, for the other side's 'LRPC'/'Ack '. The AE Manager dispatches events re-entrantly there;
   we handled one event at a time and the receive thread waited for each, so the Ack was never
   delivered and the server hung with its window up. 'LRPC' events and queued replies
   ('aevt'/'ansr') are now waited for on their own thread, release the slot once the guest picks
   them up, and may be dispatched while another handler runs. OAH_NO_AENEST=1 disables. */
static int oah_aen_nestable(const oah_aen_t *x) {
    static int off = -1;
    if (off < 0) off = getenv("OAH_NO_AENEST") ? 1 : 0;
    return !off && (x->cls == 0x4c525043 /* LRPC */ || (x->cls == 0x61657674 && x->id == 0x616e7372 /* aevt/ansr */));
}
typedef struct { oah_aen_t *x; oah_aed_t ev, rep; } oah_aen_job_t;
static void *oah_aen_job(void *arg) {
    oah_aen_job_t *j = (oah_aen_job_t *)arg;
    oah_aen_t *x = j->x;
    int i;
    if (!oah_aen_run(x, 120)) x = 0;    /* timed out: the guest owns it now; leak */
    if (j->rep.descriptorType != 0x6e756c6c /* null */ && j->rep.descriptorType) {
        if (x) {
            for (i = 0; i < x->nr; i++) {
                oah_aed_t d = { 0, 0 };
                if (!g_nae.unflat(x->rd[i], &d)) { g_nae.putp(&j->rep, x->rkw[i], &d); g_nae.disp(&d); }
            }
        }
        g_nae.send(&j->rep, NULL, 1 /* kAENoReply */, -1 /* kAEDefaultTimeout */);
    }
    if (oah_aetr()) fprintf(stderr, "[aen] done (own thread): err %d, %d reply params\n", x ? x->err : -1712, x ? x->nr : 0);
    if (x) oah_aen_free(x);
    g_nae.disp(&j->ev); g_nae.disp(&j->rep);
    free(j);
    return NULL;
}
static void *oah_aen_thread(void *arg) {
    mach_port_t port = (mach_port_t)(uintptr_t)arg;
    size_t cap = 65536;
    mach_msg_header_t *h = (mach_msg_header_t *)malloc(cap);
    for (;;) {
        kern_return_t kr;
        oah_aed_t ev = { 0, 0 }, rep = { 0, 0 };
        oah_aen_t *x;
        long n = 0, i;
        unsigned int t; long sz;
        kr = mach_msg(h, MACH_RCV_MSG | MACH_RCV_LARGE, 0, (mach_msg_size_t)cap, port, 0, MACH_PORT_NULL);
        if (kr == MACH_RCV_TOO_LARGE) { cap = h->msgh_size + 1024; h = (mach_msg_header_t *)realloc(h, cap); continue; }
        if (kr) { if (oah_aetr()) fprintf(stderr, "[aen] receive failed 0x%x\n", kr); usleep(100000); continue; }
        if (g_nae.decode(h, &ev, &rep)) {
            if (oah_aetr()) {
                unsigned int k2, *w2 = (unsigned int *)h;
                fprintf(stderr, "[aen] AEDecodeMessage failed (id %d, size %u, bits %#x, reply port %#x):", h->msgh_id, h->msgh_size, h->msgh_bits, h->msgh_remote_port);
                for (k2 = 0; k2 < h->msgh_size / 4 && k2 < 40; k2++) fprintf(stderr, " %08x", w2[k2]);
                fprintf(stderr, "\n");
            }
            mach_msg_destroy(h); continue; }
        x = (oah_aen_t *)calloc(1, sizeof *x);
        x->wantreply = rep.descriptorType && rep.descriptorType != 0x6e756c6c /* null */;
        g_nae.geta(&ev, 0x6576636c /* evcl */, 0x74797065 /* type */, &t, &x->cls, 4, &sz);
        g_nae.geta(&ev, 0x65766964 /* evid */, 0x74797065, &t, &x->id, 4, &sz);
        {   short r16 = 0; int t32 = 0; long s2;
            x->hasrt = !g_nae.geta(&ev, 0x72746964 /* rtid */, 0x73686f72 /* shor */, &t, &r16, 2, &s2);
            x->rtid = r16;
            if (g_nae.geta(&ev, 0x7472616e /* tran */, 0x6c6f6e67 /* long */, &t, &t32, 4, &s2)) t32 = 0;
            x->tran = t32;
        }
        g_nae.count(&ev, &n);
        for (i = 1; i <= n && x->np < OAH_AEN_MAXP; i++) {
            unsigned int kw; oah_aed_t d = { 0, 0 }; long fs, got = 0;
            if (g_nae.nth(&ev, i, 0x2a2a2a2a, &kw, &d)) continue;
            fs = g_nae.fsize(&d);
            if (fs > 0 && (x->d[x->np] = malloc(fs)) && !g_nae.flat(&d, x->d[x->np], fs, &got)) {
                x->kw[x->np] = kw; x->len[x->np] = (unsigned int)got; x->np++;
            }
            g_nae.disp(&d);
        }
        if (oah_aetr()) fprintf(stderr, "[aen] event %.4s/%.4s with %d params\n", (char *)&(unsigned int){ OSSwapInt32(x->cls) },
                                (char *)&(unsigned int){ OSSwapInt32(x->id) }, x->np);
        if (oah_aen_nestable(x)) {
            oah_aen_job_t *j = (oah_aen_job_t *)calloc(1, sizeof *j);
            pthread_t th; pthread_attr_t at;
            j->x = x; j->ev = ev; j->rep = rep;
            pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (!pthread_create(&th, &at, oah_aen_job, j)) { pthread_attr_destroy(&at); continue; }
            pthread_attr_destroy(&at); free(j);
        }
        if (x->cls == 'ascr' && x->id == 'gdte') g_aen_gdte_done = 1;
        if (x->cls == 'GURL' && x->id == 'GURL' && x->np >= 1 && x->len[0] > 16 + 7 && !getenv("OAH_NO_GURLFILE") &&
            !memcmp((char *)x->d[0] + 16, "file://", 7)) {
            /* Finder / open(1) hand a document to an app that lists the file: URL scheme (Internet
               Explorer) as GURL file:///...; 10.6 sent such apps an odoc for a file. IE's GURL
               handler, given a file URL, waited forever for a reply from the Internet Config
               helper. Turn it into the odoc that 10.6 would have sent. */
            static int (*mkdesc)(unsigned int, const void *, long, oah_aed_t *);
            unsigned int L = OSSwapBigToHostInt32(*(unsigned int *)((char *)x->d[0] + 12));
            oah_aed_t fd = { 0, 0 }; long fs, got = 0; void *buf;
            if (!mkdesc) *(void **)&mkdesc = dlsym(RTLD_DEFAULT, "AECreateDesc");
            if (L > 7 && 16 + L <= x->len[0] && mkdesc && !mkdesc('furl', (char *)x->d[0] + 16, L, &fd)) {
                fs = g_nae.fsize(&fd);
                if (fs > 0 && (buf = malloc(fs)) && !g_nae.flat(&fd, buf, fs, &got)) {
                    if (oah_aetr()) fprintf(stderr, "[aen] GURL %.*s -> odoc\n", (int)L, (char *)x->d[0] + 16);
                    free(x->d[0]); x->d[0] = buf; x->len[0] = (unsigned int)got; x->kw[0] = '----';
                    x->cls = 'aevt'; x->id = 'odoc';
                }
                g_nae.disp(&fd);
            }
        }
        double rapp_at = CFAbsoluteTimeGetCurrent();
        /* a hidden app is past its start-up (it was hidden after it showed itself): its windows are
           off screen, so the test below would hold the reopen until 40 s after launch, and the app
           came back only then (Mactracker after Cmd-H) */
        if (x->cls == 'aevt' && x->id == 'rapp' && !getenv("OAH_NO_RAPPWAIT") && !(g_app_hidden && !getenv("OAH_RAPPWAIT_HIDDEN"))) {
            /* a reopen while the app is still starting (no document-sized window on screen yet; a
               splash screen does not count): hold it until one is shown, at most 40 s after the app began pulling events. Entourage 2004
               crashed in its reopen handler (NULL+0x6c) when the reopen came 12-16 s after launch,
               before its main window existed; under Rosetta on a native-speed Mac that window was
               up long before anyone could click the Dock icon again. */
            double t0 = CFAbsoluteTimeGetCurrent(), lim = (g_aen_first_pull ? g_aen_first_pull : t0) + 40;
            int i, any = 0;
            if (oah_aetr()) oah_cgs_dump_wins();
            while (CFAbsoluteTimeGetCurrent() < lim) {
                for (i = 0, any = 0; i < OAH_MAXWIN && !any; i++)
                    if (g_cgswin[i].wid && g_cgswin[i].ordered && !g_cgswin[i].ghost && g_cgswin[i].wid != g_menubar_wid &&
                        g_cgswin[i].level == 0 && g_cgswin[i].w > 480 && g_cgswin[i].h > 300) any = 1;   /* not a splash screen */
                if (any) break;
                usleep(200000);
            }
            if (CFAbsoluteTimeGetCurrent() - t0 > 0.5)
                fprintf(stderr, "[aen] rapp held %.1f s until the app %s\n", CFAbsoluteTimeGetCurrent() - t0, any ? "showed a window" : "had run 40 s");
        }
        if (x->cls == 'aevt' && x->id == 'rapp' && g_app_hidden && g_app_hide_at > rapp_at) {
            /* held (above) while the app started, and the user hid the app meanwhile: showing it
               now would undo that (the reopener has fronted it already; rlaunch shows a hidden
               app itself before it sends the event) */
            fprintf(stderr, "[aen] rapp held past a later hide: not shown\n");
        } else if (x->cls == 'aevt' && (x->id == 'rapp' || x->id == 'oapp') && g_app_hidden && !getenv("OAH_NO_REOPENSHOW")) {
            /* "open" / a Launch Services reopen of a hidden app: 10.6's Process Manager unhid and
               fronted it before the reopen event; Mojave's WindowServer won't front an app with no
               visible window, so nothing happened. Show the windows, then come to the front. */
            int (*gcp)(unsigned int *) = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
            int (*sfp)(const unsigned int *) = (int (*)(const unsigned int *))dlsym(RTLD_DEFAULT, "SetFrontProcess");
            unsigned int psn[2] = { 0, 0 };
            oah_lsn_post(0);
            usleep(250000);
            if (gcp && sfp && !gcp(psn)) sfp(psn);
            fprintf(stderr, "[aen] %.4s while hidden: shown and brought to the front\n", (char *)&(unsigned int){ OSSwapInt32(x->id) });
        } else if (x->cls == 'aevt' && x->id == 'rapp' && !getenv("OAH_NO_REOPENSHOW")) {
            /* the same for an app that is only in the background: open(1), a Finder double-click or
               Spotlight on the running app sent the reopen event but nothing came to the front
               (Mojave leaves activation on reopen to the app; AppKit does it) */
            int (*gcp)(unsigned int *) = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
            int (*sfp)(const unsigned int *) = (int (*)(const unsigned int *))dlsym(RTLD_DEFAULT, "SetFrontProcess");
            int (*gfp)(unsigned int *) = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetFrontProcess");
            unsigned int psn[2] = { 0, 0 }, f[2] = { 0, 0 };
            if (gcp && sfp && !gcp(psn) && (!gfp || gfp(f) || f[0] != psn[0] || f[1] != psn[1])) {
                sfp(psn);
                fprintf(stderr, "[aen] rapp in the background: brought to the front\n");
            }
        }
        if (x->cls == 'aevt' && (x->id == 'odoc' || x->id == 'pdoc')) {
            oah_aed_t dl = { 0, 0 }; long cnt = 0, j;
            static int (*getpd)(const oah_aed_t *, unsigned int, unsigned int, oah_aed_t *);
            if (!getpd) *(void **)&getpd = dlsym(RTLD_DEFAULT, "AEGetParamDesc");
            oah_aen_gdte_once();
            /* a list of files: one event per file */
            if (getpd && !getpd(&ev, '----', '****', &dl) && dl.descriptorType == 'list' && !g_nae.count(&dl, &cnt) && cnt >= 1) {
                if (oah_aetr()) fprintf(stderr, "[aen] splitting %.4s of %ld files\n", (char *)&(unsigned int){ OSSwapInt32(x->id) }, cnt);
                for (j = 1; j <= cnt; j++) {
                    unsigned int kw; oah_aed_t d = { 0, 0 }; long fs, got = 0;
                    oah_aen_t *y;
                    if (g_nae.nth(&dl, j, 0x2a2a2a2a, &kw, &d)) continue;
                    oah_aen_bmrk_to_furl(&d);
                    oah_aen_carbon_doc(&d);
                    y = (oah_aen_t *)calloc(1, sizeof *y);
                    y->cls = x->cls; y->id = x->id; y->wantreply = x->wantreply;
                    fs = g_nae.fsize(&d);
                    if (fs > 0 && (y->d[0] = malloc(fs)) && !g_nae.flat(&d, y->d[0], fs, &got)) { y->kw[0] = '----'; y->len[0] = got; y->np = 1; }
                    g_nae.disp(&d);
                    if (j < cnt) { if (oah_aen_run(y, 120)) oah_aen_free(y); }
                    else { oah_aen_free(x); x = y; }       /* the last one answers the sender */
                }
            }
            if (dl.descriptorType) g_nae.disp(&dl);
        }
        /* An open/print the app declined with errAEEventNotHandled is tried again each second for
           30 s: Excel 2004's handler refuses documents while its Project Gallery (shown at launch)
           is up, so a document double-clicked meanwhile was dropped; Word holds the event itself.
           OAH_NO_ODOCRETRY=1 disables. */
        {   int tries = 0;
            for (;;) {
                if (x->cls == 'aevt' && x->id == 'rapp' && getenv("OAH_RAPP_NODISPATCH")) {   /* diagnostic */
                    fprintf(stderr, "[aen] rapp not dispatched (OAH_RAPP_NODISPATCH)\n");
                    break;
                }
                if (!oah_aen_run(x, 120)) { x = 0; break; }    /* timed out: the guest owns it now; leak */
                /* OAH_AEN_EARLYRETRY=1: also any non-core event declined in the first 20 s (an
                   experiment for Organization Chart's DDE /EMBD launch event, which it reads as a
                   classic high-level event rather than through a handler: no help) */
                int early = x->err == -1708 && x->cls != 'aevt' && x->cls != 'ascr' && getenv("OAH_AEN_EARLYRETRY") &&
                            CFAbsoluteTimeGetCurrent() - g_aen_start < 20 && tries < 15;
                if (!((x->cls == 'aevt' && (x->id == 'odoc' || x->id == 'pdoc') && x->err == -1708) || early) ||
                    ++tries > 30 || getenv("OAH_NO_ODOCRETRY")) break;
                if (tries == 1) fprintf(stderr, "[aen] %.4s not handled (a modal dialog up?): trying again for 30 s\n",
                                        (char *)&(unsigned int){ OSSwapInt32(x->id) });
                for (i = 0; i < x->nr; i++) { free(x->rd[i]); x->rd[i] = 0; }
                x->nr = 0; x->err = 0; x->done = 0; x->picked = 0;
                if (early) usleep(300000); else sleep(1);
            }
            if (x && tries > 1 && !x->err) fprintf(stderr, "[aen] %.4s handled after %d tries\n", (char *)&(unsigned int){ OSSwapInt32(x->id) }, tries);
        }
        if (rep.descriptorType != 0x6e756c6c /* null */ && rep.descriptorType) {
            if (x) {
                for (i = 0; i < x->nr; i++) {
                    oah_aed_t d = { 0, 0 };
                    if (!g_nae.unflat(x->rd[i], &d)) { g_nae.putp(&rep, x->rkw[i], &d); g_nae.disp(&d); }
                }
            }
            g_nae.send(&rep, NULL, 1 /* kAENoReply */, -1 /* kAEDefaultTimeout */);
        }
        if (oah_aetr()) fprintf(stderr, "[aen] done: err %d, %d reply params\n", x ? x->err : -1712, x ? x->nr : 0);
        if (x) oah_aen_free(x);
        g_nae.disp(&ev); g_nae.disp(&rep);
    }
    return 0;
}
/* Main thread: run the pending event through the guest AE manager. */
static void oah_aen_dispatch_now(void) {
    static int busy;
    static unsigned int f[10];
    static const char *nm[10] = { "_AECreateDesc", "_AECreateAppleEvent", "_AEUnflattenDesc", "_AEPutParamDesc",
        "_AESendMessage", "_AECountItems", "_AEGetNthDesc", "_AESizeOfFlattenedDesc", "_AEFlattenDesc", "_AEDisposeDesc" };
    oah_aen_t *x = g_aen_cur;
    unsigned int psn[2], tgt[2], gev[2], grep[2], gd[2], a[6], be, i;
    int k, err;
    if (!g_aen_first_pull) g_aen_first_pull = CFAbsoluteTimeGetCurrent();
    {   static unsigned long nc;
        if (oah_aetr() && (++nc <= 3 || (nc % 500) == 0 || (x && !x->done)))
            fprintf(stderr, "[aen] pull #%lu (pending %p busy %d main %d)\n", nc, (void *)x, busy, pthread_main_np());
    }
    if (!x || x->done || x->picked || (busy && !oah_aen_nestable(x))) return;
    busy++;
    x->picked = 1;
    if (oah_aen_nestable(x)) {     /* let the next one in while this handler runs */
        pthread_mutex_lock(&g_aen_mu);
        if (g_aen_cur == x) g_aen_cur = 0;
        pthread_cond_broadcast(&g_aen_cv);
        pthread_mutex_unlock(&g_aen_mu);
        if (busy > 1 && oah_aetr()) fprintf(stderr, "[aen] nested dispatch (depth %d)\n", busy);
    }
    if (oah_aetr()) fprintf(stderr, "[aen] dispatching %.4s/%.4s in the guest\n", (char *)&(unsigned int){ OSSwapInt32(x->cls) }, (char *)&(unsigned int){ OSSwapInt32(x->id) });
    for (k = 0; k < 10; k++)
        if (!f[k] && !(f[k] = oah_guest_sym("AE", nm[k]))) { x->err = -1708; goto out; }
    psn[0] = 0; psn[1] = OSSwapHostToBigInt32(2);          /* kCurrentProcess */
    a[0] = 'psn '; a[1] = (unsigned int)psn; a[2] = 8; a[3] = (unsigned int)tgt;
    if ((err = (short)oah_callppc_n("AECreateDesc", f[0], 4, a))) { x->err = err; goto out; }
    a[0] = x->cls; a[1] = x->id; a[2] = (unsigned int)tgt; a[3] = (unsigned int)-1; a[4] = 0; a[5] = (unsigned int)gev;
    /* A queued reply ('aevt'/'ansr') keeps its return ID and transaction ID: the guest matches
       it to the request it answers (Microsoft OLE's RPC between Word and Equation Editor or
       Organization Chart waited forever for replies that came back under fresh IDs).
       OAH_NO_AERTID=1: fresh IDs as before. */
    if (x->hasrt && x->cls == 0x61657674 && x->id == 0x616e7372 && !getenv("OAH_NO_AERTID")) {
        a[3] = (unsigned int)(int)x->rtid; a[4] = (unsigned int)x->tran;
        if (oah_aetr()) fprintf(stderr, "[aen] reply keeps return ID %d transaction %d\n", x->rtid, x->tran);
    }
    err = (short)oah_callppc_n("AECreateAppleEvent", f[1], 6, a);
    a[0] = (unsigned int)tgt; oah_callppc_n("AEDisposeDesc", f[9], 1, a);
    if (err) { x->err = err; goto out; }
    for (k = 0; k < x->np; k++) {
        if (oah_aetr()) {
            unsigned int q; fprintf(stderr, "[aen]   param %.4s (%u bytes):", (char *)&(unsigned int){ OSSwapInt32(x->kw[k]) }, x->len[k]);
            for (q = 0; q < x->len[k] && q < 96; q++) fprintf(stderr, "%s%02x", q % 4 ? "" : " ", ((unsigned char *)x->d[k])[q]);
            fprintf(stderr, "\n");
        }
        a[0] = (unsigned int)x->d[k]; a[1] = (unsigned int)gd;
        if ((short)oah_callppc_n("AEUnflattenDesc", f[2], 2, a)) continue;
        a[0] = (unsigned int)gev; a[1] = x->kw[k]; a[2] = (unsigned int)gd;
        oah_callppc_n("AEPutParamDesc", f[3], 3, a);
        a[0] = (unsigned int)gd; oah_callppc_n("AEDisposeDesc", f[9], 1, a);
    }
    grep[0] = grep[1] = 0;
    /* Always wait for the handler (kAEWaitReply), as AEProcessAppleEvent would: with kAENoReply the
       handler gets a null reply descriptor, and TextWrangler's odoc handler crashed on it.
       OAH_AE_NOREPLY_IF_UNWANTED=1 sends kAENoReply when the native sender wants no reply. */
    a[0] = (unsigned int)gev; a[1] = (unsigned int)grep;
    a[2] = ((!x->wantreply && getenv("OAH_AE_NOREPLY_IF_UNWANTED")) ? 0x1 /* kAENoReply */ : 0x3 /* kAEWaitReply */) |
           (getenv("OAH_AE_NEVERINTERACT") ? 0x30 : 0x20) /* kAENeverInteract : kAECanInteract */;
    a[3] = 7200;
    x->err = (short)oah_callppc_n("AESendMessage", f[4], 4, a);
    if (oah_aetr()) fprintf(stderr, "[aen] guest AESendMessage -> %d\n", x->err);
    if (grep[0]) {
        a[0] = (unsigned int)grep; a[1] = (unsigned int)&be;
        if (!(short)oah_callppc_n("AECountItems", f[5], 2, a)) {
            unsigned int n = OSSwapBigToHostInt32(be);
            for (i = 1; i <= n && x->nr < OAH_AEN_MAXP; i++) {
                unsigned int kw = 0, fs, got = 0;
                a[0] = (unsigned int)grep; a[1] = i; a[2] = '****'; a[3] = (unsigned int)&kw; a[4] = (unsigned int)gd;
                if ((short)oah_callppc_n("AEGetNthDesc", f[6], 5, a)) continue;
                a[0] = (unsigned int)gd; fs = oah_callppc_n("AESizeOfFlattenedDesc", f[7], 1, a);
                if ((int)fs > 0 && fs < (16u << 20) && (x->rd[x->nr] = malloc(fs))) {
                    a[0] = (unsigned int)gd; a[1] = (unsigned int)x->rd[x->nr]; a[2] = fs; a[3] = (unsigned int)&got;
                    if (!(short)oah_callppc_n("AEFlattenDesc", f[8], 4, a)) {
                        x->rkw[x->nr] = OSSwapBigToHostInt32(kw); x->rlen[x->nr] = OSSwapBigToHostInt32(got);
                        if (oah_aetr()) {   /* the reply's parameters ('errn' of a handler that failed, 'errs') */
                            unsigned int k, nb = x->rlen[x->nr] < 48 ? x->rlen[x->nr] : 48;
                            fprintf(stderr, "[aen]   reply %.4s (%u bytes):", (char *)&kw, x->rlen[x->nr]);
                            for (k = 0; k + 4 <= nb; k += 4) fprintf(stderr, " %08x", OSSwapBigToHostInt32(*(unsigned int *)((char *)x->rd[x->nr] + k)));
                            fprintf(stderr, "\n");
                        }
                        x->nr++;
                    } else free(x->rd[x->nr]);
                }
                a[0] = (unsigned int)gd; oah_callppc_n("AEDisposeDesc", f[9], 1, a);
            }
        }
        a[0] = (unsigned int)grep; oah_callppc_n("AEDisposeDesc", f[9], 1, a);
    }
    a[0] = (unsigned int)gev; oah_callppc_n("AEDisposeDesc", f[9], 1, a);
out:
    pthread_mutex_lock(&g_aen_mu);
    x->done = 1;
    if (g_aen_cur == x) g_aen_cur = 0;
    pthread_cond_broadcast(&g_aen_cv);
    pthread_mutex_unlock(&g_aen_mu);
    busy--;
}
/* Where the event runs. The event pull (CGEventCreateNextEvent / CGSGetNextEventRecordInternal) is
   inside HIToolbox's event fetch; 10.6 ran an Apple Event later, when the app dispatched the event
   that announced it (AEProcessAppleEvent from the event loop). Mactracker (REALbasic) span at 100 %
   CPU after its reopen handler ran inside the fetch. So a Carbon app is first sent a private Carbon
   event ('Rost'/1, posted to the main queue at high priority); its handler, a guest stub trapping
   to oah_fix_aen_evh, runs the Apple Event when the app dispatches it - RunApplicationEventLoop and
   WaitNextEvent both hand such events to the application target. If the app has not dispatched it
   within 3 s (a modal loop that does not), the event runs in the fetch as before.
   Opt-in: OAH_AE_QUEUED=1 for Carbon apps, =2 for Cocoa apps too. Mactracker's spin turned out to be
   the stack (REALbasic's RuntimeStackCheck saw the callback stack and raised StackOverflowException
   in a loop through its unhandled-exception handler), cured by running guest calls on the main
   stack (g_align_sp in oah_callppc_n); the event-fetch dispatch stays the default. */
static unsigned int oah_guest_code_alloc(unsigned int bytes);   /* oah_au.h */
static int g_aen_evh_state;                     /* 0 not tried, 1 installed, -1 unavailable */
static int oah_aen_queued(void) {
    static int m = -1;
    if (m < 0) {
        const char *q = getenv("OAH_AE_QUEUED");
        if (!q || g_aen_evh_idx < 0) m = 0;
        /* a Cocoa app by its Info.plist (NSPrincipalClass), not by AppKit being loaded: Mactracker
           (REALbasic, Carbon) loads AppKit for Sparkle and its plugins */
        else m = oah_clip_cocoa_app() ? !strcmp(q, "2") : 1;
        if (oah_aetr()) fprintf(stderr, "[aen] Apple Events run %s\n", m ? "from the app's event dispatch" : "in the event fetch");
    }
    return m;
}
static int oah_aen_evh_install(void) {
    static unsigned int spec[2];
    unsigned int fT, fI, *stub, a[6], tgt, err;
    if (g_aen_evh_state) return g_aen_evh_state > 0;
    g_aen_evh_state = -1;
    fT = oah_guest_sym("HIToolbox", "_GetApplicationEventTarget");
    fI = oah_guest_sym("HIToolbox", "_InstallEventHandler");
    if (!fT || !fI || !(stub = (unsigned int *)oah_guest_code_alloc(2 * 4))) return 0;
    stub[0] = OSSwapHostToBigInt32(0x0FE00000 | (0x6000 /* GT_ENTRY */ + (unsigned int)g_aen_evh_idx));   /* twi 31,r0,hook */
    stub[1] = OSSwapHostToBigInt32(0x4E800020);                                                          /* blr */
    spec[0] = OSSwapHostToBigInt32('Rost'); spec[1] = OSSwapHostToBigInt32(1);
    tgt = oah_callppc_n("GetApplicationEventTarget", fT, 0, a);
    a[0] = tgt; a[1] = (unsigned int)stub; a[2] = 1; a[3] = (unsigned int)spec; a[4] = 0; a[5] = 0;
    err = oah_callppc_n("InstallEventHandler", fI, 6, a);
    if (oah_aetr()) fprintf(stderr, "[aen] event handler for queued Apple Events: target %08x stub %08x -> %d\n", tgt, (unsigned int)stub, (int)err);
    if (!tgt || err) return 0;
    g_aen_evh_state = 1;
    return 1;
}
static int oah_aen_post(void) {
    static unsigned int fC, fQ, fP, fR;
    unsigned int a[8], ev = 0, q, err;
    if (!fC) { fC = oah_guest_sym("HIToolbox", "_CreateEvent"); fQ = oah_guest_sym("HIToolbox", "_GetMainEventQueue");
               fP = oah_guest_sym("HIToolbox", "_PostEventToQueue"); fR = oah_guest_sym("HIToolbox", "_ReleaseEvent"); }
    if (!fC || !fQ || !fP || !fR || !oah_aen_evh_install()) return 0;
    /* CreateEvent(allocator, class, kind, EventTime when, attributes, &event): `when` is a double in
       f1, which a guest call does not set; a stub loads 0.0 ("now") into f1 and jumps on */
    {   static unsigned int *ce;
        if (!ce && (ce = (unsigned int *)oah_guest_code_alloc(8 * 4))) {
            unsigned int c[8] = { 0x38000000u, 0x9001FFF8u, 0x9001FFFCu, 0xC821FFF8u,   /* li r0,0; stw r0,-8(r1); stw r0,-4(r1); lfd f1,-8(r1) */
                                  0x3D800000u | (fC >> 16), 0x618C0000u | (fC & 0xffff), /* lis r12,hi; ori r12,r12,lo */
                                  0x7D8903A6u, 0x4E800420u };                            /* mtctr r12; bctr */
            int k; for (k = 0; k < 8; k++) ce[k] = OSSwapHostToBigInt32(c[k]);
        }
        if (!ce) return 0;
        /* GPRs: r3 allocator, r4 class, r5 kind, (r6, r7: the double's slots), r8 attributes, r9 &event */
        a[0] = 0; a[1] = 'Rost'; a[2] = 1; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = (unsigned int)&ev;
        if ((err = oah_callppc_n("CreateEvent", (unsigned int)ce, 7, a)) || !ev) return 0;
    }
    ev = OSSwapBigToHostInt32(ev);
    q = oah_callppc_n("GetMainEventQueue", fQ, 0, a);
    a[0] = q; a[1] = ev; a[2] = 2;          /* kEventPriorityHigh */
    err = oah_callppc_n("PostEventToQueue", fP, 3, a);
    a[0] = ev; oah_callppc_n("ReleaseEvent", fR, 1, a);
    return err == 0;
}
static void oah_aen_dispatch(void) {
    oah_aen_t *x = g_aen_cur;
    if (x && !x->done && !x->picked && oah_aen_queued()) {
        if (!x->posted) {
            if (oah_aen_post()) {
                x->posted = 1; x->posted_at = CFAbsoluteTimeGetCurrent();
                if (oah_aetr()) fprintf(stderr, "[aen] %.4s/%.4s announced to the app's event loop\n",
                                        (char *)&(unsigned int){ OSSwapInt32(x->cls) }, (char *)&(unsigned int){ OSSwapInt32(x->id) });
                return;
            }
        } else if (CFAbsoluteTimeGetCurrent() - x->posted_at < 3.0) return;
        else if (x->posted == 1) {
            x->posted = 2;
            fprintf(stderr, "[aen] %.4s not dispatched by the app in 3 s: running it in the event fetch\n",
                    (char *)&(unsigned int){ OSSwapInt32(x->id) });
        }
    }
    oah_aen_dispatch_now();
}
static void oah_fix_aen_evh(unsigned int *R) {
    /* the live register file: r1 here is the app's own stack, where its event dispatch runs (a guest
       call made from this hook runs just below it, see g_align_sp in oah_callppc_n) */
    if (oah_aetr()) fprintf(stderr, "[aen] the app dispatched our event (r1 %08x): running the Apple Event\n", R[1]);
    oah_aen_dispatch_now();
    g_gt_skip = 1; g_gt_skip_ret = 0;      /* noErr */
}
/* OAH_ODOC_ALIAS=1 (experimental): give Carbon apps odoc / pdoc with a list of aliases, as 10.6's
   Finder did, instead of a single file URL. Cocoa apps always keep the single 'furl' (SL AppKit spun
   on a list of URLs). Neither form made TextWrangler's or Word 2004's open handler work. */
static void oah_aen_carbon_doc(oah_aed_t *d) {
    static int (*coerce)(const oah_aed_t *, unsigned int, oah_aed_t *);
    static int (*mklist)(const void *, long, unsigned char, oah_aed_t *);
    static int (*putdesc)(oah_aed_t *, long, const oah_aed_t *);
    static int cocoa = -1;
    oah_aed_t al = { 0, 0 }, li = { 0, 0 };
    if (d->descriptorType != 'furl' || getenv("OAH_ODOC_FURL")) return;
    if (cocoa < 0) {
        /* AppleScript Studio apps (AppleScriptKit's ASKInitialize) hand the event to an AppleScript
           "on open" handler, which wants the list of aliases 10.6's Finder sent: given a file URL,
           xACT's stopped with "Can't make file ... into type reference" (-1700); given aliases it
           lists the file for encoding. They get aliases unless OAH_ODOC_FURL=1. */
        const char *oa = getenv("OAH_ODOC_ALIAS");
        int studio = oah_guest_sym("AppleScriptKit", "_ASKInitialize") ? 1 : 0;
        /* Adobe's scripting layer (SCL: GoLive CS2) handles every 'aevt' event itself;
           SCLApplication::ASReqOpenDocuments opens a direct parameter of type 'list' (of 'fss ' / 'alis'
           / 'TEXT' paths) or a single 'fss ', 'alis' or 'TEXT', and answers noErr without opening
           anything for a 'furl'. Such apps get aliases too (OAH_ODOC_FURL=1: file URL). */
        if (!studio && oah_guest_sym("SCL", "__ZN14SCLApplication18ASReqOpenDocumentsEP12SCLASMessage")) {
            studio = 1;
            if (oah_aetr()) fprintf(stderr, "[aen] Adobe SCL app: documents as aliases\n");
        }
        cocoa = ((oah_guest_sym("AppKit", "_NSApplicationMain") ? 1 : 0) && !(oa && !strcmp(oa, "2")) && !studio) || (!oa && !studio);   /* experimental, opt-in; 2: Cocoa apps too */
        if (oah_aetr() && studio && !oah_guest_sym("SCL", "__ZN14SCLApplication18ASReqOpenDocumentsEP12SCLASMessage")) fprintf(stderr, "[aen] AppleScript Studio app: documents as aliases\n");
        if (oah_aetr()) fprintf(stderr, "[aen] guest is %s\n", cocoa ? "Cocoa: documents stay file URLs" : "Carbon");
    }
    if (cocoa) return;
    if (!coerce) *(void **)&coerce = dlsym(RTLD_DEFAULT, "AECoerceDesc");
    if (!mklist) *(void **)&mklist = dlsym(RTLD_DEFAULT, "AECreateList");
    if (!putdesc) *(void **)&putdesc = dlsym(RTLD_DEFAULT, "AEPutDesc");
    if (!coerce || !mklist || !putdesc) return;
    {   int e = coerce(d, 'alis', &al);
        if (e) { if (oah_aetr()) fprintf(stderr, "[aen] furl -> alis coercion failed: %d\n", e); return; } }
    if (!mklist(0, 0, 0, &li) && !putdesc(&li, 1, &al)) {
        g_nae.disp(d); *d = li;
        if (oah_aetr()) fprintf(stderr, "[aen] document as a list of aliases (Carbon app)\n");
    } else if (li.descriptorType) g_nae.disp(&li);
    g_nae.disp(&al);
}
/* The launch document as the app's first Apple Event. 10.6's AE framework, while HIToolbox starts
   (aeInitializeFromHIToolbox), asks LaunchServices for the process's launch modifiers and queues the
   Apple Event found there (Finder's odoc) as the first high-level event; only when there is none does
   it send the app 'oapp'. Mojave's LaunchServices has none for us, so a Carbon app always got oapp
   first and our odoc later: Word 2004 put up its Project Gallery on oapp and declined the odoc
   (-1708) while that dialog was up. For a non-AppKit app launched with documents, answer
   _LSCopyLaunchModifiers with one modifier holding a flattened odoc for the first document; the
   docs thread then skips that document. OAH_NO_LAUNCHAE=1 disables. */
static char g_aen_launchdocs[4096];         /* OAH_OPEN_DOCS, saved before it is unset */
static int g_aen_launchdoc_taken;
/* A launching PowerPC app's launchAppParameters (oah_gtrace.h, LaunchApplication): the parent writes
   the flattened Apple Event to a file named in OAH_LAUNCH_AE; it becomes this process's launch
   modifier, the first high-level event, as the 10.6 Process Manager delivered it. */
static char g_aen_launchae[1024];
static void oah_aen_take_launchae(void) {
    static int done;
    const char *f;
    if (done) return;
    done = 1;
    if ((f = getenv("OAH_LAUNCH_AE")) && *f) { strlcpy(g_aen_launchae, f, sizeof g_aen_launchae); unsetenv("OAH_LAUNCH_AE"); }
}
static int oah_want_launchmods(void) { return !getenv("OAH_NO_LAUNCHAE"); }
static void oah_fixret_launchmods_one(unsigned int *R, unsigned int a0);
/* Two threads can ask at once (StuffIt Expander: a secondary thread asked first, and the main
   thread's AE initialisation, asking while the event was still being built, got nothing and sent
   oapp, so the archive opened at launch was never expanded): a caller on another thread waits for
   the one building it (up to 5 s) and then gets the same modifiers. */
static void oah_fixret_launchmods(unsigned int *R, unsigned int a0) {
    static pthread_t builder; static volatile int building;
    int k;
    if (R[3]) { oah_fixret_launchmods_one(R, a0); return; }
    if (__sync_bool_compare_and_swap(&building, 0, 1)) {
        builder = pthread_self();
        oah_fixret_launchmods_one(R, a0);
        building = 0;
        return;
    }
    if (!pthread_equal(builder, pthread_self()))
        for (k = 0; k < 500 && building; k++) usleep(10000);
    oah_fixret_launchmods_one(R, a0);
}
static void oah_fixret_launchmods_one(unsigned int *R, unsigned int a0) {
    static int done;
    static int (*mkae)(unsigned int, unsigned int, const oah_aed_t *, short, int, oah_aed_t *);
    static int (*mkdesc)(unsigned int, const void *, long, oah_aed_t *);
    const char *docs = g_aen_launchdocs[0] ? g_aen_launchdocs : getenv("OAH_OPEN_DOCS");
    unsigned int psn[2] = { 0, 2 };          /* kCurrentProcess */
    oah_aed_t addr = { 0, 0 }, ae = { 0, 0 }, fd = { 0, 0 };
    unsigned int gdata = 0, gdict = 0, garr = 0, kdata, ktype, vae, fdata;
    char path[2048]; const char *nl; size_t len; UInt8 b[4096]; CFIndex L = 0; CFURLRef u;
    long fs = 0, got = 0; void *buf = 0;
    static unsigned int cached, fretain;
    (void)a0;
    if (oah_aetr()) fprintf(stderr, "[aen] _LSCopyLaunchModifiers -> %08x (cached %08x done %d docs %s) thread %x lr %08x\n", R[3], cached, done, docs ? "yes" : "no", pthread_mach_thread_np(pthread_self()), R[0x90 / 4]);
    if (R[3]) return;
    if (cached) {   /* every caller gets it (HIToolbox asks before the AE framework does) */
        if (!fretain) fretain = oah_guest_sym("CoreFoundation", "_CFRetain");
        if (fretain) { unsigned int a1[1]; a1[0] = cached; R[3] = oah_callppc_n("CFRetain", fretain, 1, a1); }
        return;
    }
    oah_aen_take_launchae();
    if (!done && g_aen_launchae[0] && !getenv("OAH_NO_LAUNCHAEMOD")) {   /* launchAppParameters from a PowerPC parent */
        FILE *fp = fopen(g_aen_launchae, "rb");
        unsigned char *fb = 0; long n = 0;
        unsigned int fdc = oah_guest_sym("CoreFoundation", "_CFDataCreate"),
                     kd = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataKey"),
                     kt = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataTypeKey"),
                     va = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataTypeAppleEventKey");
        done = 1;
        if (fp) {
            if (!fseek(fp, 0, SEEK_END) && (n = ftell(fp)) > 0 && n < 0x100000 && !fseek(fp, 0, SEEK_SET) &&
                (fb = (unsigned char *)malloc((size_t)n)) && fread(fb, 1, (size_t)n, fp) != (size_t)n) n = 0;
            fclose(fp);
        }
        if (access("/tmp/oah_launchae_keep", 0)) unlink(g_aen_launchae);   /* debugging: touch it to keep the file */
        if (fb && n > 0 && fdc && kd && kt && va && oah_guest_cf_load()) {
            unsigned int a[3];
            a[0] = 0; a[1] = (unsigned int)fb; a[2] = (unsigned int)n;
            gdata = oah_callppc_n("CFDataCreate", fdc, 3, a);
            garr = gdata ? gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0) : 0;
            gdict = garr ? gcall(G_DICT_NEW, 4, 0, 0, g_cf[G_DKEYCB], g_cf[G_DVALCB]) : 0;
            if (gdict) {
                gcall(G_DICT_SET, 3, gdict, OSSwapBigToHostInt32(*(unsigned int *)kd), gdata, 0);
                gcall(G_DICT_SET, 3, gdict, OSSwapBigToHostInt32(*(unsigned int *)kt), OSSwapBigToHostInt32(*(unsigned int *)va), 0);
                gcall(G_ARR_ADD, 2, garr, gdict, 0, 0);
                gcall(G_RELEASE, 1, gdict, 0, 0, 0);
                R[3] = garr;
                cached = garr; { unsigned int a1[1]; unsigned int fr = oah_guest_sym("CoreFoundation", "_CFRetain"); a1[0] = garr; if (fr) oah_callppc_n("CFRetain", fr, 1, a1); }
            } else if (garr) gcall(G_RELEASE, 1, garr, 0, 0, 0);
            if (gdata) gcall(G_RELEASE, 1, gdata, 0, 0, 0);
        }
        free(fb);
        fprintf(stderr, "[aen] launch Apple Event from the launching app (%ld bytes): %s\n", n, R[3] ? "in the launch modifiers" : "not built");
        return;
    }
    if (done || !docs || !*docs) return;
    done = 1;
    /* a Cocoa app is one whose bundle names an NSPrincipalClass: Carbon apps load AppKit too (GoLive CS2,
       for its panels), and GoLive, treated as Cocoa, got its document later as a separate odoc and
       opened nothing (OAH_LAUNCHAE_APPKIT=1: the old test, AppKit loaded) */
    if (getenv("OAH_LAUNCHAE_APPKIT") ? oah_guest_sym("AppKit", "_NSApplicationMain") != 0 : oah_clip_cocoa_app()) {
        if (oah_aetr()) fprintf(stderr, "[aen] launch modifiers: Cocoa app\n");
        return;
    }
    if (!oah_aen_nload() || !oah_guest_cf_load()) { if (oah_aetr()) fprintf(stderr, "[aen] launch modifiers: AE/CF not loaded\n"); return; }
    if (!mkae) *(void **)&mkae = dlsym(RTLD_DEFAULT, "AECreateAppleEvent");
    if (!mkdesc) *(void **)&mkdesc = dlsym(RTLD_DEFAULT, "AECreateDesc");
    kdata = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataKey");
    ktype = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataTypeKey");
    vae = oah_guest_sym("LaunchServices", "__kLSLaunchModifierDataTypeAppleEventKey");
    fdata = oah_guest_sym("CoreFoundation", "_CFDataCreate");
    if (!mkae || !mkdesc || !kdata || !ktype || !vae || !fdata) return;
    nl = strchr(docs, '\n'); len = nl ? (size_t)(nl - docs) : strlen(docs);
    if (!len || len >= sizeof path) return;
    memcpy(path, docs, len); path[len] = 0;
    if ((u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, len, false))) {
        CFURLRef a = CFURLCopyAbsoluteURL(u);
        CFStringRef us = a ? CFURLGetString(a) : 0;
        if (us && CFStringGetCString(us, (char *)b, sizeof b, kCFStringEncodingUTF8)) {
            if (!strncmp((char *)b, "file://localhost/", 17)) memmove(b + 7, b + 16, strlen((char *)b + 16) + 1);
            L = (CFIndex)strlen((char *)b);
        }
        if (a) CFRelease(a);
        CFRelease(u);
    }
    if (L <= 0 || mkdesc('psn ', psn, 8, &addr)) return;
    /* the same document form as a later odoc: aliases for the apps that need them (Adobe SCL, AppleScript
       Studio, OAH_ODOC_ALIAS), else the file URL */
    if (!mkae('aevt', 'odoc', &addr, -1, 0, &ae) && !mkdesc('furl', b, L, &fd) && (oah_aen_carbon_doc(&fd), 1) &&
        !g_nae.putp(&ae, '----', &fd) &&
        (fs = g_nae.fsize(&ae)) > 0 && (buf = malloc(fs)) && !g_nae.flat(&ae, buf, fs, &got)) {
        unsigned int a[4];
        a[0] = 0; a[1] = (unsigned int)buf; a[2] = (unsigned int)got;
        gdata = oah_callppc_n("CFDataCreate", fdata, 3, a);
        garr = gdata ? gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0) : 0;
        gdict = garr ? gcall(G_DICT_NEW, 4, 0, 0, g_cf[G_DKEYCB], g_cf[G_DVALCB]) : 0;
        if (gdict) {
            gcall(G_DICT_SET, 3, gdict, OSSwapBigToHostInt32(*(unsigned int *)kdata), gdata, 0);
            gcall(G_DICT_SET, 3, gdict, OSSwapBigToHostInt32(*(unsigned int *)ktype), OSSwapBigToHostInt32(*(unsigned int *)vae), 0);
            gcall(G_ARR_ADD, 2, garr, gdict, 0, 0);
            gcall(G_RELEASE, 1, gdict, 0, 0, 0);
            R[3] = garr;
            cached = garr; { unsigned int a1[1]; unsigned int fr = oah_guest_sym("CoreFoundation", "_CFRetain"); a1[0] = garr; if (fr) oah_callppc_n("CFRetain", fr, 1, a1); }
            g_aen_launchdoc_taken = 1;
        } else if (garr) gcall(G_RELEASE, 1, garr, 0, 0, 0);
        if (gdata) gcall(G_RELEASE, 1, gdata, 0, 0, 0);
    }
    free(buf);
    if (fd.descriptorType) g_nae.disp(&fd);
    if (ae.descriptorType) g_nae.disp(&ae);
    g_nae.disp(&addr);
    if (oah_aetr()) fprintf(stderr, "[aen] launch Apple Event: odoc %.*s %s\n", (int)L, (char *)b, R[3] ? "in the launch modifiers" : "not built");
}
/* Documents LaunchServices gave the wrapper's native front end (rlaunch: OAH_OPEN_DOCS / OAH_PRINT_DOCS,
   newline-separated paths): delivered to the guest as odoc / pdoc once it pulls events. */
static void *oah_aen_docs_thread(void *arg) {
    static int (*mklist)(const void *, long, unsigned char, oah_aed_t *);
    static int (*putptr)(oah_aed_t *, long, unsigned int, const void *, long);
    char *docs = (char *)arg, *p, *nl;
    int pass;
    /* "\001paths": files dropped on a window (oah_dnd.h): deliver now, the app is running */
    int drop = docs[0] == 1;
    char *base = docs + drop;
    /* not during launch: an odoc handled inside the first event pulls (AppKit still finishing
       launching) left Pages spinning; once the app has been pulling events for a while it opens
       like any other odoc */
    /* OAH_LAUNCHDOC_NOW=1 (experimental): Carbon apps get the launch document at the first event
       pull instead of after the app settles (Word 2004 still showed its Project Gallery first). */
    while (!g_aen_first_pull) usleep(50000);
    int carbon = drop || (getenv("OAH_LAUNCHDOC_NOW") && !oah_guest_sym("AppKit", "_NSApplicationMain"));   /* experimental, opt-in */
    if (oah_aetr()) fprintf(stderr, "[aen] launch documents: %s\n", carbon ? "delivered at the first event pull" : "delivered once the app settles");
    {   double dl = getenv("OAH_LAUNCHDOC_DELAY") ? atof(getenv("OAH_LAUNCHDOC_DELAY")) : 4.0;
        while (!carbon && CFAbsoluteTimeGetCurrent() - g_aen_first_pull < dl) usleep(200000); }
    {   /* ...and has settled: process CPU below 40% for two seconds in a row (at most 90 s) */
        int quiet = 0, k;
        for (k = 0; k < 90 && quiet < 2 && !carbon; k++) {
            struct rusage r0, r1; double c0, c1;
            getrusage(RUSAGE_SELF, &r0); sleep(1); getrusage(RUSAGE_SELF, &r1);
            c0 = r0.ru_utime.tv_sec + r0.ru_stime.tv_sec + (r0.ru_utime.tv_usec + r0.ru_stime.tv_usec) / 1e6;
            c1 = r1.ru_utime.tv_sec + r1.ru_stime.tv_sec + (r1.ru_utime.tv_usec + r1.ru_stime.tv_usec) / 1e6;
            quiet = (c1 - c0) < 0.4 ? quiet + 1 : 0;
        }
        if (oah_aetr() && !carbon) fprintf(stderr, "[aen] app settled after %d s\n", k);
    }
    if (!mklist) *(void **)&mklist = dlsym(RTLD_DEFAULT, "AECreateList");
    if (!putptr) *(void **)&putptr = dlsym(RTLD_DEFAULT, "AEPutPtr");
    oah_aen_gdte_once();
    /* one event per file with a single typeFileURL direct object: SL AppKit spun forever on an
       odoc whose direct object was a list of 'furl' items (osascript `open {file}` too) */
    for (pass = 0; pass < 2 && mklist && putptr; pass++) {
        static int (*mkdesc)(unsigned int, const void *, long, oah_aed_t *);
        if (!mkdesc) *(void **)&mkdesc = dlsym(RTLD_DEFAULT, "AECreateDesc");
        p = base + (pass ? strlen(base) + 1 : 0);          /* "open\0print\0" */
        while (mkdesc && *p) {
            CFURLRef u; UInt8 b[4096]; CFIndex L = 0;
            oah_aed_t d = { 0, 0 };
            oah_aen_t *x;
            long fs, got = 0;
            size_t len;
            if (!(nl = strchr(p, '\n'))) nl = p + strlen(p);
            len = nl - p;
            u = len ? CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)p, len, false) : 0;
            if (u) {   /* "file:///path" (CFURLGetBytes can give the file://localhost/ form) */
                CFStringRef us = CFURLGetString(CFURLCopyAbsoluteURL(u));
                if (us && CFStringGetCString(us, (char *)b, sizeof b, kCFStringEncodingUTF8)) {
                    if (!strncmp((char *)b, "file://localhost/", 17)) memmove(b + 7, b + 16, strlen((char *)b + 16) + 1);
                    L = (CFIndex)strlen((char *)b);
                }
                CFRelease(u);
            }
            p = *nl ? nl + 1 : nl;
            if (!pass && !drop && g_aen_launchdoc_taken == 1) {   /* already the app's launch event */
                g_aen_launchdoc_taken = 2;
                if (oah_aetr()) fprintf(stderr, "[aen] launch document %.*s went with the launch event\n", (int)L, (char *)b);
                continue;
            }
            if (L <= 0 || mkdesc('furl', b, L, &d)) continue;
            oah_aen_carbon_doc(&d);
            x = (oah_aen_t *)calloc(1, sizeof *x);
            x->cls = 'aevt'; x->id = pass ? 'pdoc' : 'odoc';
            fs = g_nae.fsize(&d);
            if (fs > 0 && (x->d[0] = malloc(fs)) && !g_nae.flat(&d, x->d[0], fs, &got)) { x->kw[0] = '----'; x->len[0] = got; x->np = 1; }
            g_nae.disp(&d);
            if (!x->np) { free(x->d[0]); free(x); continue; }
            if (oah_aetr()) fprintf(stderr, "[aen] launch document: %s %.*s\n", pass ? "pdoc" : "odoc", (int)L, (char *)b);
            /* declined with errAEEventNotHandled: tried again each second for 60 s, like documents
               that arrive later (Photoshop CS2 refuses them while its Welcome Screen is up; a picture
               it was started with was dropped). OAH_NO_ODOCRETRY=1 disables. */
            {   int tries;
                for (tries = 0; ; tries++) {
                    pthread_mutex_lock(&g_aen_mu);
                    while (g_aen_cur) pthread_cond_wait(&g_aen_cv, &g_aen_mu);
                    x->done = 0; x->err = 0; x->picked = 0;
                    g_aen_cur = x;
                    pthread_mutex_unlock(&g_aen_mu);
                    oah_ev_wake();
                    pthread_mutex_lock(&g_aen_mu);
                    while (!x->done) pthread_cond_wait(&g_aen_cv, &g_aen_mu);
                    pthread_mutex_unlock(&g_aen_mu);
                    if (x->err != -1708 || tries >= 60 || getenv("OAH_NO_ODOCRETRY")) break;
                    { int i2; for (i2 = 0; i2 < x->nr; i2++) { free(x->rd[i2]); x->rd[i2] = 0; } x->nr = 0; }
                    if (oah_aetr() && !tries) fprintf(stderr, "[aen] launch document declined (-1708): retrying each second\n");
                    sleep(1);
                }
                if (tries && oah_aetr()) fprintf(stderr, "[aen] launch document after %d retries\n", tries);
            }
            if (oah_aetr()) fprintf(stderr, "[aen] launch document delivered: err %d\n", x->err);
            free(x->d[0]);
            { int i2; for (i2 = 0; i2 < x->nr; i2++) free(x->rd[i2]); }
            free(x);
        }
    }
    free(docs);
    return 0;
}
static void oah_ae_checkin(void) {
    static int done;
    mach_port_t port;
    pthread_t t;
    if (!__sync_bool_compare_and_swap(&done, 0, 1)) return;   /* early thread vs. make_foreground */
    if (getenv("OAH_NO_AENATIVE") || !oah_aen_nload()) return;
    port = g_nae.regport();
    if (!port) { fprintf(stderr, "[aen] no native AE port\n"); return; }
    pthread_create(&t, 0, oah_aen_thread, (void *)(uintptr_t)port);
    pthread_detach(t);
    g_aen_start = CFAbsoluteTimeGetCurrent();
    fprintf(stderr, "[aen] receiving native Apple Events on port 0x%x\n", port);
    {   const char *od = getenv("OAH_OPEN_DOCS"), *pd = getenv("OAH_PRINT_DOCS");
        if ((od && *od) || (pd && *pd)) {
            size_t a = od ? strlen(od) : 0, b = pd ? strlen(pd) : 0;
            char *buf = (char *)calloc(1, a + b + 3);
            if (a) memcpy(buf, od, a);
            if (a && !g_aen_launchdocs[0]) strlcpy(g_aen_launchdocs, od, sizeof g_aen_launchdocs);
            if (b) memcpy(buf + a + 1, pd, b);
            unsetenv("OAH_OPEN_DOCS"); unsetenv("OAH_PRINT_DOCS");
            pthread_create(&t, 0, oah_aen_docs_thread, buf);
            pthread_detach(t);
        }
    }
}

/* ---- guest -> other processes ----
   The guest's own AECreateAppleEvent can't work on Mojave, so it is bridged (oah_apl.h) to build an
   AERecord holding 'evcl', 'evid' and the target as an 'addr' parameter. Sending such a record
   through SL AESendMessage works for the app itself but crashed in EventAddress::isSelfProcess for
   any other target (and couldn't reach it anyway: no AECSD service). AESendMessage is bridged here:
   self targets (no 'addr', kCurrentProcess, our PSN or pid) run the original guest code through a
   trampoline (its first two instructions + a jump past the bridge mark); others are rebuilt natively
   (class, id, address, parameters as flattened descriptors) and sent with Mojave's AESendMessage,
   and the native reply's parameters are put into a guest reply record. OAH_AETRACE=1 logs. */
static unsigned int oah_aesm_orig(unsigned int *a4) {
    static unsigned int tramp;
    if (!tramp) {
        unsigned int base = oah_guest_sym("AE", "_AESendMessage") + 8;
        unsigned int *t = (unsigned int *)oah_guest_code_alloc(6 * 4);   /* oah_au.h: a page translate knows */
        if (!t || base == 8) return (unsigned int)-1708;
        t[0] = OSSwapHostToBigInt32(0x7c0802a6);                     /* mflr r0 */
        t[1] = OSSwapHostToBigInt32(0xbda1ffac);                     /* stmw r13,-84(r1) */
        t[2] = OSSwapHostToBigInt32(0x3d800000 | (base >> 16));      /* lis r12,hi */
        t[3] = OSSwapHostToBigInt32(0x618c0000 | (base & 0xffff));   /* ori r12,r12,lo */
        t[4] = OSSwapHostToBigInt32(0x7d8903a6);                     /* mtctr r12 */
        t[5] = OSSwapHostToBigInt32(0x4e800420);                     /* bctr */
        tramp = (unsigned int)t;
    }
    return oah_callppc_n("AESendMessage(orig)", tramp, 4, a4);
}
static int g_aeout_fixmode;
static int oah_aeout_call(const char *n, unsigned int *R);
/* AESendMessage at its guest entry (gtrace fix table). The bridge table only catches calls bound
   through dyld; CFM applications (Office 2004) reach AE's AESendMessage directly, so their events
   to other processes went through Snow Leopard's own sending code, which has no AECSD service to
   talk to: procNotFound. Word could not start Clip Gallery ("Word cannot launch Microsoft Clip
   Gallery"). Events to other processes are rebuilt natively here; events to ourselves run on in
   the guest. OAH_NO_AESMFIX=1 disables. */
static void oah_fix_aesendmessage(unsigned int *R) {
    int r;
    g_aeout_fixmode = 1;
    r = oah_aeout_call("AESendMessage", R);
    g_aeout_fixmode = 0;
    if (r) { g_gt_skip = 1; g_gt_skip_ret = R[3]; }
}
static int oah_want_aesmfix(void) { return !getenv("OAH_NO_AESMFIX"); }
static int oah_aeout_call(const char *n, unsigned int *R) {
    static unsigned int f[12];
    static const char *nm[12] = { "_AEGetParamDesc", "_AEGetDescDataSize", "_AECountItems", "_AEGetNthDesc",
        "_AESizeOfFlattenedDesc", "_AEFlattenDesc", "_AEUnflattenDesc", "_AEPutParamDesc", "_AEDisposeDesc",
        "_AECreateList", "_AEGetDescData", "_AEGetParamPtr" };
    static int (*ncreate)(unsigned int, unsigned int, const oah_aed_t *, short, int, oah_aed_t *);
    unsigned int a[7], be, sz, gd[2], ad[2], atype = 0, adata[64];
    int k, self = 1;
    if (!strcmp(n, "AESend")) {
        /* AESend(event, reply, mode, priority, timeout, idleProc, filterProc): the same as
           AESendMessage(event, reply, mode, timeout) for us. Office (CFM) calls AESend, whose
           guest code reaches AESendMessage inside AE without passing this bridge. */
        static int off = -1;
        if (off < 0) off = getenv("OAH_NO_AESENDBRIDGE") ? 1 : 0;
        if (off) return 0;
        R[6] = R[7];
        n = "AESendMessage";
    }
    if (strcmp(n, "AESendMessage")) return 0;
    for (k = 0; k < 12; k++)
        if (!f[k] && !(f[k] = oah_guest_sym("AE", nm[k]))) { R[3] = (unsigned int)-1708; return 1; }
    /* where is it going? */
    a[0] = R[3]; a[1] = 'addr'; a[2] = '****'; a[3] = (unsigned int)ad;
    {   static unsigned int fattr;
        int got = R[3] && !(short)oah_callppc_n("AEGetParamDesc", f[0], 4, a);
        if (!got && R[3]) {      /* an event made by AECreateAppleEvent keeps its target as an attribute */
            if (!fattr) fattr = oah_guest_sym("AE", "_AEGetAttributeDesc");
            a[0] = R[3]; a[1] = 'addr'; a[2] = '****'; a[3] = (unsigned int)ad;
            got = fattr && !(short)oah_callppc_n("AEGetAttributeDesc", fattr, 4, a);
        }
        if (!got) ad[0] = 0;
    }
    if (ad[0]) {
        unsigned int dsz;
        atype = OSSwapBigToHostInt32(ad[0]);
        a[0] = (unsigned int)ad; dsz = oah_callppc_n("AEGetDescDataSize", f[1], 1, a);
        if (dsz > sizeof adata) dsz = sizeof adata;
        a[0] = (unsigned int)ad; a[1] = (unsigned int)adata; a[2] = dsz;
        oah_callppc_n("AEGetDescData", f[10], 3, a);
        if (atype == 'psn ' && dsz >= 8) {
            unsigned int hi = OSSwapBigToHostInt32(adata[0]), lo = OSSwapBigToHostInt32(adata[1]);
            static unsigned int me[2]; static int got;
            if (!got) { int (*gcp)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "GetCurrentProcess"); got = 1; if (gcp) gcp(me); }
            self = (hi == 0 && (lo == 2 || lo == 0)) || (hi == me[0] && lo == me[1]);
        } else if (atype == 'kpid' && dsz >= 4) self = (int)OSSwapBigToHostInt32(adata[0]) == getpid();
        else self = 0;                       /* signature, bundle id, URL...: let Mojave resolve it */
    } else ad[0] = 0;
    if (self && g_aeout_fixmode) {   /* entry fix (oah_fix_aesendmessage): the guest's own code goes on */
        if (oah_aetr()) fprintf(stderr, "[aeout] self/unaddressed send (addr %.4s) left to the guest\n", (char *)&(unsigned int){ OSSwapInt32(atype) });
        if (ad[0]) { a[0] = (unsigned int)ad; oah_callppc_n("AEDisposeDesc", f[8], 1, a); }
        return 0;
    }
    if (self) {
        if (ad[0]) { a[0] = (unsigned int)ad; oah_callppc_n("AEDisposeDesc", f[8], 1, a); }
        if (oah_aetr()) {
            unsigned int cls = 0, id = 0, t, b2;
            a[0] = R[3]; a[1] = 'evcl'; a[2] = 'type'; a[3] = (unsigned int)&t; a[4] = (unsigned int)&cls; a[5] = 4; a[6] = (unsigned int)&b2;
            oah_callppc_n("AEGetParamPtr", f[11], 7, a);
            a[1] = 'evid'; a[4] = (unsigned int)&id;
            oah_callppc_n("AEGetParamPtr", f[11], 7, a);
            fprintf(stderr, "[aeout] self send %.4s/%.4s (addr %.4s)\n", (char *)&cls, (char *)&id, (char *)&(unsigned int){ OSSwapInt32(atype) });
        }
        a[0] = R[3]; a[1] = R[4]; a[2] = R[5]; a[3] = R[6];
        R[3] = oah_aesm_orig(a);
        return 1;
    }
    if (!oah_aen_nload()) { R[3] = (unsigned int)-1708; return 1; }
    if (!ncreate) *(void **)&ncreate = dlsym(RTLD_DEFAULT, "AECreateAppleEvent");
    {
        unsigned int gev = R[3], grep = R[4], mode = R[5], cls = 0, id = 0, i, cnt, t;
        long timeout = (long)(int)R[6];
        short grt = -1; int gtr = 0;
        oah_aed_t addr = { 0, 0 }, nev = { 0, 0 }, nrep = { 0, 0 };
        int err;
        void *buf;
        if (grep && grep > 0x1000) { ((unsigned int *)grep)[0] = OSSwapHostToBigInt32('null'); ((unsigned int *)grep)[1] = 0; }   /* see below */
        a[0] = gev; a[1] = 'evcl'; a[2] = 'type'; a[3] = (unsigned int)&t; a[4] = (unsigned int)&cls; a[5] = 4; a[6] = (unsigned int)&be;
        oah_callppc_n("AEGetParamPtr", f[11], 7, a);
        a[1] = 'evid'; a[4] = (unsigned int)&id;
        oah_callppc_n("AEGetParamPtr", f[11], 7, a);
        if (!cls || !id) {       /* AECreateAppleEvent's events keep class and ID as attributes */
            static unsigned int fap;
            if (!fap) fap = oah_guest_sym("AE", "_AEGetAttributePtr");
            if (fap) {
                a[0] = gev; a[1] = 'evcl'; a[2] = 'type'; a[3] = (unsigned int)&t; a[4] = (unsigned int)&cls; a[5] = 4; a[6] = (unsigned int)&be;
                oah_callppc_n("AEGetAttributePtr", fap, 7, a);
                a[1] = 'evid'; a[4] = (unsigned int)&id;
                oah_callppc_n("AEGetAttributePtr", fap, 7, a);
            }
        }
        cls = OSSwapBigToHostInt32(cls); id = OSSwapBigToHostInt32(id);
        a[0] = (unsigned int)ad; sz = oah_callppc_n("AESizeOfFlattenedDesc", f[4], 1, a);
        buf = malloc(sz + 8);
        a[0] = (unsigned int)ad; a[1] = (unsigned int)buf; a[2] = sz; a[3] = (unsigned int)&be;
        err = (short)oah_callppc_n("AEFlattenDesc", f[5], 4, a);
        a[0] = (unsigned int)ad; oah_callppc_n("AEDisposeDesc", f[8], 1, a);
        if (!err) err = g_nae.unflat(buf, &addr);
        free(buf);
        {   /* keep the guest event's return ID and transaction ID, so that a queued reply ('ansr')
               comes back with the ID the guest waits for (see the receiving side) */
            static unsigned int fap2;
            unsigned short b16 = 0; unsigned int b32 = 0;
            if (!fap2) fap2 = oah_guest_sym("AE", "_AEGetAttributePtr");
            if (fap2 && !getenv("OAH_NO_AERTID")) {
                a[0] = gev; a[1] = 'rtid'; a[2] = 'shor'; a[3] = (unsigned int)&t; a[4] = (unsigned int)&b16; a[5] = 2; a[6] = (unsigned int)&be;
                if (!(short)oah_callppc_n("AEGetAttributePtr", fap2, 7, a)) grt = (short)OSSwapBigToHostInt16(b16);
                a[0] = gev; a[1] = 'tran'; a[2] = 'long'; a[3] = (unsigned int)&t; a[4] = (unsigned int)&b32; a[5] = 4; a[6] = (unsigned int)&be;
                if (!(short)oah_callppc_n("AEGetAttributePtr", fap2, 7, a)) gtr = (int)OSSwapBigToHostInt32(b32);
            }
        }
        if (!err) err = ncreate(cls, id, &addr, grt, gtr, &nev);
        g_nae.disp(&addr);
        if (err) { if (oah_aetr()) fprintf(stderr, "[aeout] can't rebuild the event (%d)\n", err); R[3] = (unsigned int)err; return 1; }
        a[0] = gev; a[1] = (unsigned int)&be;
        cnt = (short)oah_callppc_n("AECountItems", f[2], 2, a) ? 0 : OSSwapBigToHostInt32(be);
        for (i = 1; i <= cnt; i++) {
            unsigned int kw = 0; oah_aed_t d = { 0, 0 };
            a[0] = gev; a[1] = i; a[2] = '****'; a[3] = (unsigned int)&kw; a[4] = (unsigned int)gd;
            if ((short)oah_callppc_n("AEGetNthDesc", f[3], 5, a)) continue;
            kw = OSSwapBigToHostInt32(kw);
            if (kw != 'evcl' && kw != 'evid' && kw != 'addr') {
                a[0] = (unsigned int)gd; sz = oah_callppc_n("AESizeOfFlattenedDesc", f[4], 1, a);
                if ((int)sz > 0 && (buf = malloc(sz))) {
                    a[0] = (unsigned int)gd; a[1] = (unsigned int)buf; a[2] = sz; a[3] = (unsigned int)&be;
                    if (!(short)oah_callppc_n("AEFlattenDesc", f[5], 4, a) && !g_nae.unflat(buf, &d)) {
                        g_nae.putp(&nev, kw, &d); g_nae.disp(&d);
                    }
                    free(buf);
                }
            }
            a[0] = (unsigned int)gd; oah_callppc_n("AEDisposeDesc", f[8], 1, a);
        }
        err = g_nae.send(&nev, &nrep, (int)mode, timeout);
        /* a process we just started (LaunchApplication -> launchProcessSN) is known to the window
           server before appleeventsd takes events for it: procNotFound for the first second or two.
           Word sends Clip Gallery its "launched by Office" event right after the launch.
           OAH_NO_AEPSNRETRY=1 disables. */
        if (err == -600 && atype == 'psn ' && !getenv("OAH_NO_AEPSNRETRY")) {
            int tries = 0;
            while (err == -600 && tries++ < 40) { usleep(100000); err = g_nae.send(&nev, &nrep, (int)mode, timeout); }
            if (oah_aetr() || tries > 1) fprintf(stderr, "[aeout] procNotFound retried %d times -> %d\n", tries, err);
        }
        if (oah_aetr()) fprintf(stderr, "[aeout] %.4s/%.4s to %.4s with %u params -> native AESendMessage %d\n",
                                (char *)&(unsigned int){ OSSwapInt32(cls) }, (char *)&(unsigned int){ OSSwapInt32(id) },
                                (char *)&(unsigned int){ OSSwapInt32(atype) }, cnt, err);
        /* AESendMessage always leaves a valid reply descriptor: a null one ('null', NULL) when there is
           no reply or the send failed. Left as the caller's uninitialised stack words, the reply was
           disposed of as garbage after a failed send (Illustrator CS2 at start: core/getd to an
           application that is not running, -600), which ended in a crash. */
        if (grep && grep > 0x1000) { ((unsigned int *)grep)[0] = OSSwapHostToBigInt32('null'); ((unsigned int *)grep)[1] = 0; }
        if (grep && nrep.descriptorType && nrep.descriptorType != 'null') {
            long rn = 0, j;
            a[0] = 0; a[1] = 0; a[2] = 1; a[3] = grep;
            if (!(short)oah_callppc_n("AECreateList", f[9], 4, a)) {
                g_nae.count(&nrep, &rn);
                for (j = 1; j <= rn; j++) {
                    unsigned int kw; oah_aed_t d = { 0, 0 }; long fs, got = 0;
                    if (g_nae.nth(&nrep, j, '****', &kw, &d)) continue;
                    fs = g_nae.fsize(&d);
                    if (fs > 0 && (buf = malloc(fs))) {
                        if (!g_nae.flat(&d, buf, fs, &got)) {
                            a[0] = (unsigned int)buf; a[1] = (unsigned int)gd;
                            if (!(short)oah_callppc_n("AEUnflattenDesc", f[6], 2, a)) {
                                a[0] = grep; a[1] = kw; a[2] = (unsigned int)gd;
                                oah_callppc_n("AEPutParamDesc", f[7], 3, a);
                                a[0] = (unsigned int)gd; oah_callppc_n("AEDisposeDesc", f[8], 1, a);
                            }
                        }
                        free(buf);
                    }
                    g_nae.disp(&d);
                }
            }
        }
        g_nae.disp(&nev); g_nae.disp(&nrep);
        R[3] = (unsigned int)err;
    }
    return 1;
}
