/* oah_qt.h - QuickTime movie playback through Mojave's native 32-bit QuickTime.

   The 10.6 PPC QuickTime cannot work here (its regions/ports collide with the
   bridged, native QuickDraw), but Mojave's own QuickTime 7 framework still
   decodes SimCity's intro movies (Sorenson Video 1 + IMA audio), so the movie
   calls the game makes (CQTPlayer) are answered natively:
     - Movie / Track / Media / MediaHandler values are native pointers; the
       game only passes them back, it never dereferences them.
     - Ports are already native (window ports come from bridged calls).
     - The FSSpec comes from the guest File Manager, whose vRefNum is its own;
       it is turned into a path with the guest's FSpMakeFSRef + FSRefMakePath
       and then into a native FSSpec.
     - OSErr results are sign-extended; short* / Rect* / Movie* are big-endian.
   Native QuickDraw on Mojave crashes building a fallback port if nothing has
   set a current port, so EnterMovies sets an offscreen port first when the
   game has not set one yet.  OAH_NO_QT=1 disables the bridge (calls fail). */

static void *g_qt_lib;
static unsigned int g_qt_last_port;          /* last native port the game set */
static int g_qt_verbose = -1;

static void *oah_qt_sym(const char *name) {
    if (!g_qt_lib) {
        g_qt_lib = dlopen("/System/Library/Frameworks/QuickTime.framework/QuickTime", RTLD_LAZY | RTLD_GLOBAL);
        if (!g_qt_lib) { fprintf(stderr, "[qt] dlopen QuickTime failed: %s\n", dlerror()); return 0; }
    }
    return dlsym(g_qt_lib, name);
}
static void *oah_qt_carbon(const char *name) {
    if (!g_carbon_handle)
        g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
    return g_carbon_handle ? dlsym(g_carbon_handle, name) : 0;
}
#define QTV (g_qt_verbose > 0)
#define QTFN(var, ret, args, name) static ret (*var) args; if (!var) var = (ret (*) args)oah_qt_sym(name); if (!var) { R[3] = (unsigned int)-2048; return 1; }

/* Called from the generic bridge for SetPort / SetPortWindowPort / SetGWorld-like
   calls so EnterMovies knows whether native QuickDraw has a current port. */
static void oah_qt_note_port(const char *name, unsigned int *R) {
    if (!strcmp(name, "SetPort")) g_qt_last_port = R[3];
    else if (!strcmp(name, "SetPortWindowPort")) g_qt_last_port = R[3] ? 1 : g_qt_last_port;
}

/* guest FSSpec -> POSIX path via the guest's own File Manager */
static int oah_qt_spec_path(unsigned int gspec, char *path, unsigned int cap) {
    static unsigned int fsp2ref, ref2path;
    static unsigned char ref[80];
    unsigned int args[3];
    int e;
    if (!fsp2ref) fsp2ref = oah_guest_sym("CarbonCore", "_FSpMakeFSRef");
    if (!ref2path) ref2path = oah_guest_sym("CarbonCore", "_FSRefMakePath");
    if (!fsp2ref || !ref2path) return -1;
    args[0] = gspec; args[1] = (unsigned int)ref;
    e = (short)oah_callppc_n("oah_qt_fsp2ref", fsp2ref, 2, args);
    if (e) return e;
    args[0] = (unsigned int)ref; args[1] = (unsigned int)path; args[2] = cap;
    return (int)oah_callppc_n("oah_qt_ref2path", ref2path, 3, args);
}

static int oah_qt_call(const char *name, unsigned int *R) {
    if (name[0] < 'C' || name[0] > 'U') return 0;
    if (g_qt_verbose < 0) g_qt_verbose = getenv("OAH_VERBOSE") || getenv("OAH_QTTRACE") ? 1 : 0;

    if (!strcmp(name, "EnterMovies")) {
        QTFN(fn, short, (void), "EnterMovies");
        if (getenv("OAH_NO_QT")) { R[3] = (unsigned int)-2048; return 1; }
        if (!g_qt_last_port) {
            /* no native current port yet: give QuickDraw an offscreen one */
            static void *pre;
            short (*newgw)(void **, int, const short *, void *, void *, unsigned int) =
                (short (*)(void **, int, const short *, void *, void *, unsigned int))oah_qt_carbon("NewGWorld");
            void (*setgw)(void *, void *) = (void (*)(void *, void *))oah_qt_carbon("SetGWorld");
            short r[4] = { 0, 0, 16, 16 };
            if (!pre && newgw) newgw(&pre, 32, r, 0, 0, 0);
            if (pre && setgw) setgw(pre, 0);
            if (QTV) fprintf(stderr, "[qt] EnterMovies: no game port yet, using offscreen port %p\n", pre);
        }
        R[3] = (unsigned int)(int)fn();
        fprintf(stderr, "[qt] EnterMovies -> %d (native QuickTime)\n", (int)R[3]);
        return 1;
    }
    if (!strcmp(name, "OpenMovieFile")) {
        /* OSErr OpenMovieFile(const FSSpec *spec, short *resRefNum, SInt8 perm) */
        QTFN(fn, short, (const void *, short *, signed char), "OpenMovieFile");
        static unsigned char spec[70], ref[80];
        static char path[1024];
        short (*mkref)(const char *, void *, unsigned char *) = (short (*)(const char *, void *, unsigned char *))oah_qt_carbon("FSPathMakeRef");
        short (*cat)(const void *, unsigned int, void *, void *, void *, void *) =
            (short (*)(const void *, unsigned int, void *, void *, void *, void *))oah_qt_carbon("FSGetCatalogInfo");
        short ref_num = 0, e;
        if (getenv("OAH_NO_QT")) { R[3] = (unsigned int)-43; return 1; }
        /* With the guest toolbox (the default since SimCity was last tested) the game's ports are
           guest QuickDraw ports, which native QuickTime cannot draw into: SetMovieGWorld crashed in
           GetPixBaseAddr on the intro movie. Report the movie missing; the game skips it.
           OAH_QT_GTB=1 tries anyway. */
        if (getenv("OAH_GTB") && !getenv("OAH_QT_GTB")) {
            fprintf(stderr, "[qt] OpenMovieFile: movie skipped (guest toolbox ports cannot be handed to native QuickTime)\n");
            R[3] = (unsigned int)-43;
            return 1;
        }
        e = (short)oah_qt_spec_path(R[3], path, sizeof path);
        if (!e && mkref && cat) {
            e = mkref(path, ref, 0);
            if (!e) e = cat(ref, 0, 0, 0, spec, 0);
            if (!e) e = fn(spec, &ref_num, (signed char)R[5]);
        } else if (!e) e = -2048;
        if (R[4]) *(unsigned short *)R[4] = OSSwapHostToBigInt16((unsigned short)ref_num);
        fprintf(stderr, "[qt] OpenMovieFile \"%s\" -> %d ref=%d\n", path, e, ref_num);
        R[3] = (unsigned int)(int)e;
        return 1;
    }
    if (!strcmp(name, "NewMovieFromFile")) {
        /* OSErr NewMovieFromFile(Movie *m, short ref, short *resId, StringPtr name, short flags, Boolean *changed) */
        QTFN(fn, short, (void **, short, short *, void *, short, unsigned char *), "NewMovieFromFile");
        void *m = 0; short rid = 0, e; unsigned char chg = 0;
        if (R[5]) rid = (short)OSSwapBigToHostInt16(*(unsigned short *)R[5]);
        e = fn(&m, (short)R[4], R[5] ? &rid : 0, 0, (short)R[7], R[8] ? &chg : 0);
        if (R[3]) *(unsigned int *)R[3] = OSSwapHostToBigInt32((unsigned int)m);
        if (R[5]) *(unsigned short *)R[5] = OSSwapHostToBigInt16((unsigned short)rid);
        if (R[8]) *(unsigned char *)R[8] = chg;
        fprintf(stderr, "[qt] NewMovieFromFile(ref=%d) -> %d movie=%p\n", (short)R[4], e, m);
        R[3] = (unsigned int)(int)e;
        return 1;
    }
    if (!strcmp(name, "CloseMovieFile")) {
        QTFN(fn, short, (short), "CloseMovieFile");
        R[3] = (unsigned int)(int)fn((short)R[3]);
        return 1;
    }
    if (!strcmp(name, "GetMovieBox") || !strcmp(name, "SetMovieBox")) {
        int get = name[0] == 'G';
        short r[4] = { 0, 0, 0, 0 }; int k;
        if (get) {
            QTFN(fn, void, (void *, short *), "GetMovieBox");
            fn((void *)R[3], r);
            if (R[4]) for (k = 0; k < 4; k++) ((unsigned short *)R[4])[k] = OSSwapHostToBigInt16((unsigned short)r[k]);
        } else {
            QTFN(fn, void, (void *, const short *), "SetMovieBox");
            if (R[4]) for (k = 0; k < 4; k++) r[k] = (short)OSSwapBigToHostInt16(((unsigned short *)R[4])[k]);
            fn((void *)R[3], r);
        }
        if (QTV) fprintf(stderr, "[qt] %s(%08x) (%d,%d,%d,%d)\n", name, R[3], r[0], r[1], r[2], r[3]);
        return 1;
    }
    if (!strcmp(name, "SetMovieGWorld")) {
        QTFN(fn, void, (void *, unsigned int, unsigned int), "SetMovieGWorld");
        fn((void *)R[3], R[4], R[5] ? oah_native_handle(R[5]) : 0);
        fprintf(stderr, "[qt] SetMovieGWorld(movie=%08x port=%08x gd=%08x)\n", R[3], R[4], R[5]);
        return 1;
    }
    if (!strcmp(name, "GetMovieTime")) {
        /* TimeValue GetMovieTime(Movie, TimeRecord *out) - TimeRecord = wide, scale, base */
        QTFN(fn, int, (void *, unsigned int *), "GetMovieTime");
        unsigned int tr[4] = { 0, 0, 0, 0 };
        int t = fn((void *)R[3], R[4] ? tr : 0);
        if (R[4]) {   /* wide is {SInt32 hi; UInt32 lo} big-endian on PPC */
            unsigned int *o = (unsigned int *)R[4];
            o[0] = OSSwapHostToBigInt32(tr[1]); o[1] = OSSwapHostToBigInt32(tr[0]);
            o[2] = OSSwapHostToBigInt32(tr[2]); o[3] = OSSwapHostToBigInt32(tr[3]);
        }
        R[3] = (unsigned int)t;
        return 1;
    }
    if (!strcmp(name, "QTGetTimeUntilNextTask")) {
        /* OSErr QTGetTimeUntilNextTask(long *duration, long scale). QTKit's +[QTMovie_QuickTime
           idleAllMovies:] reschedules its NSTimer with this; the guest QuickTime (never entered)
           said 0, so the timer ran with a 0.1 ms interval and kept every iWork app's main run
           loop spinning (~120 wake-ups/s, a menu bar flush each). Native QuickTime answers for
           the native movies MoviesTask drives; never less than 1/30 s. */
        QTFN(fn, short, (int *, int), "QTGetTimeUntilNextTask");
        int d = 0, scale = (int)R[4] > 0 ? (int)R[4] : 60;
        short e = fn(&d, scale);
        static int nlog;
        if (e || d < scale / 30) { if (d < scale / 30) d = scale / 30; if (d < 1) d = 1; e = 0; }
        if (R[3]) *(unsigned int *)R[3] = OSSwapHostToBigInt32((unsigned int)d);
        if (nlog++ < 3 || QTV) fprintf(stderr, "[qt] QTGetTimeUntilNextTask(scale %d) -> %d\n", scale, d);
        R[3] = (unsigned int)(int)e;
        return 1;
    }
    if (!strcmp(name, "GetMediaSampleDescription")) {
        /* Only used to read the audio sample rate for a pitch adjustment; with an
           empty handle the game skips that adjustment, so leave it empty. */
        R[3] = 0;
        return 1;
    }
    {   /* simple pass-through calls: (name, kind) kind: v void, e OSErr, i int, b Boolean */
        static const struct { const char *n; char k; char nargs; } T[] = {
            { "DisposeMovie", 'v', 1 }, { "GoToBeginningOfMovie", 'v', 1 }, { "StartMovie", 'v', 1 },
            { "StopMovie", 'v', 1 }, { "GetMoviePreferredRate", 'i', 1 }, { "PrerollMovie", 'e', 3 },
            { "UpdateMovie", 'e', 1 }, { "MoviesTask", 'v', 2 }, { "IsMovieDone", 'b', 1 },
            { "GetMovieIndTrackType", 'i', 4 }, { "GetTrackMedia", 'i', 1 }, { "SetTrackVolume", 'v', 2 },
            { "GetMediaHandler", 'i', 1 }, { "MediaSetSoundBalance", 'i', 2 }, { "SetMovieRate", 'v', 2 },
        };
        static void *F[sizeof T / sizeof T[0]];
        unsigned int k, a[4];
        for (k = 0; k < sizeof T / sizeof T[0]; k++) if (!strcmp(name, T[k].n)) break;
        if (k == sizeof T / sizeof T[0]) return 0;
        if (!F[k]) F[k] = oah_qt_sym(name);
        if (!F[k]) { R[3] = (unsigned int)-2048; return 1; }
        a[0] = R[3]; a[1] = R[4]; a[2] = R[5]; a[3] = R[6];
        /* short arguments (SetTrackVolume, MediaSetSoundBalance volume/balance) are
           already sign-extended in the GPR; i386 cdecl promotes them to int anyway. */
        if (T[k].k == 'b') {
            unsigned char b = ((unsigned char (*)(unsigned int))F[k])(a[0]);
            R[3] = b ? 1 : 0;
        } else if (T[k].k == 'e') {
            short e = ((short (*)(unsigned int, unsigned int, unsigned int, unsigned int))F[k])(a[0], a[1], a[2], a[3]);
            R[3] = (unsigned int)(int)e;
        } else {
            unsigned int r = ((unsigned int (*)(unsigned int, unsigned int, unsigned int, unsigned int))F[k])(a[0], a[1], a[2], a[3]);
            if (T[k].k != 'v') R[3] = r;
        }
        if (QTV && strcmp(name, "MoviesTask") && strcmp(name, "IsMovieDone"))
            fprintf(stderr, "[qt] %s(%08x %08x %08x) -> %08x\n", name, a[0], a[1], a[2], R[3]);
        if (!strcmp(name, "StartMovie") || !strcmp(name, "DisposeMovie") || !strcmp(name, "StopMovie"))
            fprintf(stderr, "[qt] %s(%08x)\n", name, a[0]);
        return 1;
    }
}
