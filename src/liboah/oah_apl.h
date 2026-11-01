/* oah_apl.h - native stand-in for SimCity 4's music helper, APlayer.app
   (included by liboah_mojave.c after oah_snd.h).

   SC4 plays its radio MP3s through a separate background-only PowerPC app,
   APlayer.app (creator 'i5AP').  APlayerInit() launches it with
   LSOpenFromRefSpec, then the game drives it with Apple Events of class
   'APlr':
       POpn  '----' typeFSRef (80 bytes), 'psn ' = game's PSN
       PPly  '----' typeBoolean = loop
       PStp, PCls
       PVol  '----' typeIEEE32BitFloatingPoint ('sing') volume 0..1
       PIsP  reply '----' typeBoolean
       aevt/quit (to 'i5AP')
   and APlayer sends 'APlr'/'PDon' back when a track ends; the game's handler
   clears its cached "playing" flag so it moves on to the next song.

   Mojave cannot launch a PowerPC helper, so the guest's AESend (otherwise a
   trace stub) and LSOpenFromRefSpec are handled here: events are decoded with
   the guest's own AE.framework, MP3s are decoded natively with ExtAudioFile on
   a feeder thread and mixed into the oah_snd output unit, and PDon is
   delivered by calling the guest's installed handler on the main thread. */
#include <AudioToolbox/ExtendedAudioFile.h>

static unsigned int oah_guest_sym(const char *lib, const char *sym);

#define APL_RING   (44100 * 2)          /* frames of decoded audio buffered (2 s) */
#define APL_CHUNK  4096
static float g_apl_ring[APL_RING * 2];

static struct {
    ExtAudioFileRef file;
    volatile unsigned int rd, wr;        /* frame counters; wr - rd = frames buffered */
    volatile int playing, loop, eof, done_pending, stop_feed;
    volatile float vol;
    pthread_t feeder;
    int feeder_running;
    pthread_mutex_t mx;                  /* guards file vs. feeder */
    char path[1024];
    int tr;
    unsigned long frames_out;
} g_apl = { 0, 0, 0, 0, 0, 0, 0, 0, 1.0f, 0, 0, PTHREAD_MUTEX_INITIALIZER, "", -1, 0 };

static OSStatus (*p_ExtOpenURL)(CFURLRef, ExtAudioFileRef *);
static OSStatus (*p_ExtSetProp)(ExtAudioFileRef, ExtAudioFilePropertyID, UInt32, const void *);
static OSStatus (*p_ExtRead)(ExtAudioFileRef, UInt32 *, AudioBufferList *);
static OSStatus (*p_ExtSeek)(ExtAudioFileRef, SInt64);
static OSStatus (*p_ExtDispose)(ExtAudioFileRef);

static int apl_tr(void) {
    if (g_apl.tr < 0) g_apl.tr = (getenv("OAH_SNDTRACE") || getenv("OAH_VERBOSE")) ? 1 : 0;
    return g_apl.tr;
}

/* ---- render thread: called from oah_snd_render ---- */
static void apl_mix(float *out, unsigned int n) {
    unsigned int i, avail;
    float v = g_apl.vol;
    if (!g_apl.playing) return;
    avail = g_apl.wr - g_apl.rd;
    if (avail > n) avail = n;
    for (i = 0; i < avail; i++) {
        unsigned int k = (g_apl.rd + i) % APL_RING;
        out[2 * i] += g_apl_ring[2 * k] * v;
        out[2 * i + 1] += g_apl_ring[2 * k + 1] * v;
    }
    __sync_synchronize();
    g_apl.rd += avail;
    g_apl.frames_out += avail;
    if (avail < n && g_apl.eof && g_apl.wr == g_apl.rd) { g_apl.playing = 0; g_apl.done_pending = 1; }
}

/* ---- feeder thread: keeps the ring full ---- */
static void *apl_feed(void *unused) {
    static float tmp[APL_CHUNK * 2];
    (void)unused;
    while (!g_apl.stop_feed) {
        unsigned int space = APL_RING - (g_apl.wr - g_apl.rd), want, i;
        UInt32 got;
        AudioBufferList abl;
        if (!g_apl.playing || g_apl.eof || space < APL_CHUNK) { usleep(20000); continue; }
        want = APL_CHUNK;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = want * 8;
        abl.mBuffers[0].mData = tmp;
        got = want;
        pthread_mutex_lock(&g_apl.mx);
        if (!g_apl.file || p_ExtRead(g_apl.file, &got, &abl)) got = 0;
        if (!got && g_apl.file && g_apl.loop) {            /* end of file: loop */
            p_ExtSeek(g_apl.file, 0);
            got = want; abl.mBuffers[0].mDataByteSize = want * 8;
            if (p_ExtRead(g_apl.file, &got, &abl)) got = 0;
        }
        if (!got) { g_apl.eof = 1; pthread_mutex_unlock(&g_apl.mx); continue; }
        for (i = 0; i < got; i++) {
            unsigned int k = (g_apl.wr + i) % APL_RING;
            g_apl_ring[2 * k] = tmp[2 * i];
            g_apl_ring[2 * k + 1] = tmp[2 * i + 1];
        }
        __sync_synchronize();
        g_apl.wr += got;
        pthread_mutex_unlock(&g_apl.mx);
    }
    return 0;
}

static int apl_load_syms(void) {
    void *h;
    if (p_ExtOpenURL) return 1;
    h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) return 0;
    p_ExtOpenURL = (OSStatus (*)(CFURLRef, ExtAudioFileRef *))dlsym(h, "ExtAudioFileOpenURL");
    p_ExtSetProp = (OSStatus (*)(ExtAudioFileRef, ExtAudioFilePropertyID, UInt32, const void *))dlsym(h, "ExtAudioFileSetProperty");
    p_ExtRead = (OSStatus (*)(ExtAudioFileRef, UInt32 *, AudioBufferList *))dlsym(h, "ExtAudioFileRead");
    p_ExtSeek = (OSStatus (*)(ExtAudioFileRef, SInt64))dlsym(h, "ExtAudioFileSeek");
    p_ExtDispose = (OSStatus (*)(ExtAudioFileRef))dlsym(h, "ExtAudioFileDispose");
    if (!p_ExtOpenURL || !p_ExtSetProp || !p_ExtRead || !p_ExtSeek || !p_ExtDispose) { p_ExtOpenURL = 0; return 0; }
    return 1;
}

static void apl_close(void) {
    g_apl.playing = 0;
    pthread_mutex_lock(&g_apl.mx);
    if (g_apl.file) { p_ExtDispose(g_apl.file); g_apl.file = 0; }
    g_apl.rd = g_apl.wr = 0; g_apl.eof = 0; g_apl.done_pending = 0;
    pthread_mutex_unlock(&g_apl.mx);
}

static int apl_open(const char *path) {
    CFURLRef url;
    AudioStreamBasicDescription f;
    OSStatus e;
    ExtAudioFileRef ref = 0;
    if (!apl_load_syms()) { fprintf(stderr, "[apl] ExtAudioFile not available\n"); return -1; }
    oah_snd_start_au();
    apl_close();
    url = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)path, strlen(path), false);
    if (!url) return -43;
    e = p_ExtOpenURL(url, &ref);
    CFRelease(url);
    if (e || !ref) { fprintf(stderr, "[apl] cannot open \"%s\": %d\n", path, (int)e); return -43; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = OAH_SND_OUTRATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 8; f.mFramesPerPacket = 1; f.mBytesPerFrame = 8;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    if ((e = p_ExtSetProp(ref, kExtAudioFileProperty_ClientDataFormat, sizeof f, &f))) {
        fprintf(stderr, "[apl] client format -> %d\n", (int)e);
        p_ExtDispose(ref);
        return -206;
    }
    pthread_mutex_lock(&g_apl.mx);
    g_apl.file = ref;
    pthread_mutex_unlock(&g_apl.mx);
    strncpy(g_apl.path, path, sizeof g_apl.path - 1);
    if (!g_apl.feeder_running) {
        g_apl.stop_feed = 0;
        if (!pthread_create(&g_apl.feeder, 0, apl_feed, 0)) g_apl.feeder_running = 1;
    }
    g_snd_music_mix = apl_mix;
    return 0;
}

/* ---- guest Apple Event helpers (calls into the guest's AE.framework) ---- */
static unsigned int g_ae_getattr, g_ae_getparam, g_ae_putparam, g_ae_createlist, g_ae_gethandler, g_fs_refpath;
static int apl_syms(void) {
    if (!g_ae_getattr) g_ae_getattr = oah_guest_sym("/AE.framework/", "_AEGetAttributePtr");
    if (!g_ae_getparam) g_ae_getparam = oah_guest_sym("/AE.framework/", "_AEGetParamPtr");
    if (!g_ae_putparam) g_ae_putparam = oah_guest_sym("/AE.framework/", "_AEPutParamPtr");
    if (!g_ae_createlist) g_ae_createlist = oah_guest_sym("/AE.framework/", "_AECreateList");
    if (!g_ae_gethandler) g_ae_gethandler = oah_guest_sym("/AE.framework/", "_AEGetEventHandler");
    if (!g_fs_refpath) g_fs_refpath = oah_guest_sym("CarbonCore", "_FSRefMakePath");
    return g_ae_getattr && g_ae_getparam;
}
/* Read an attribute or parameter into buf (raw big-endian bytes).  Returns size or -1. */
static int apl_ae_get(int attr, unsigned int evt, unsigned int key, unsigned int want, void *buf, unsigned int max) {
    static unsigned int type_be, size_be;
    unsigned int args[7];
    unsigned int fn = attr ? g_ae_getattr : g_ae_getparam;
    if (!fn) return -1;
    args[0] = evt; args[1] = key; args[2] = want; args[3] = (unsigned int)&type_be;
    args[4] = (unsigned int)buf; args[5] = max; args[6] = (unsigned int)&size_be;
    if ((short)oah_callppc_n("oah_apl_get", fn, 7, args)) return -1;
    return (int)OSSwapBigToHostInt32(size_be);
}
static unsigned int be4(const void *p) { return OSSwapBigToHostInt32(*(const unsigned int *)p); }
/* Attribute of a real AppleEvent, or the same key stored as a parameter in
   the AERecords our AECreateAppleEvent makes. */
static int apl_ae_attr(unsigned int evt, unsigned int key, unsigned int want, void *buf, unsigned int max) {
    int n = apl_ae_get(0, evt, key, want, buf, max);   /* our records first */
    if (n < 0) n = apl_ae_get(1, evt, key, want, buf, max);
    return n;
}

/* AECreateAppleEvent(class, id, const AEAddressDesc *target, returnID, transactionID, AppleEvent *result).
   The guest's own version aborts on Mojave (it cannot reach the Apple Event
   server).  Build an AERecord instead, holding 'evcl', 'evid' and, for a
   signature target, 'addr'; AEPutParamPtr / AEGetParamPtr / AEDisposeDesc on
   it work in the guest as usual, and our AESend reads it back. */
static int apl_create_event(unsigned int *R) {
    static unsigned int ae_getdescdata;
    static unsigned char tbuf[8];
    unsigned int cls = R[3], id = R[4], target = R[5], result = R[8], args[5], v;
    if (!apl_syms() || !g_ae_createlist || !g_ae_putparam || !result) { R[3] = (unsigned int)-1708; return 1; }
    if (!ae_getdescdata) ae_getdescdata = oah_guest_sym("/AE.framework/", "_AEGetDescData");
    args[0] = 0; args[1] = 0; args[2] = 1; args[3] = result;
    if ((short)oah_callppc_n("oah_apl_list", g_ae_createlist, 4, args)) { R[3] = (unsigned int)-108; return 1; }
    v = OSSwapHostToBigInt32(cls);
    args[0] = result; args[1] = 'evcl'; args[2] = 'type'; args[3] = (unsigned int)&v; args[4] = 4;
    oah_callppc_n("oah_apl_put", g_ae_putparam, 5, args);
    v = OSSwapHostToBigInt32(id);
    args[1] = 'evid';
    oah_callppc_n("oah_apl_put", g_ae_putparam, 5, args);
    if (target && be4((void *)target) != 'sign' && be4((void *)target) != 'null' &&
        !(be4((void *)target) == 'psn ' && ae_getdescdata && (args[0] = target, args[1] = (unsigned int)tbuf, args[2] = 8,
          !(short)oah_callppc_n("oah_apl_dd", ae_getdescdata, 3, args)) && be4(tbuf) == 0 && be4(tbuf + 4) == 2)) {
        /* other targets (not kCurrentProcess): kept for AESendMessage (oah_aen.h) */
        static unsigned int fpd;
        if (!fpd) fpd = oah_guest_sym("/AE.framework/", "_AEPutParamDesc");
        if (fpd) { args[0] = result; args[1] = 'addr'; args[2] = target; oah_callppc_n("oah_apl_pd", fpd, 3, args); }
    }
    if (target && be4((void *)target) == 'sign' && ae_getdescdata) {
        args[0] = target; args[1] = (unsigned int)tbuf; args[2] = 4;
        if (!(short)oah_callppc_n("oah_apl_dd", ae_getdescdata, 3, args)) {
            args[0] = result; args[1] = 'addr'; args[2] = 'sign'; args[3] = (unsigned int)tbuf; args[4] = 4;
            oah_callppc_n("oah_apl_put", g_ae_putparam, 5, args);
        }
    }
    if (apl_tr()) fprintf(stderr, "[apl] AECreateAppleEvent %.4s/%.4s target=%.4s -> record\n",
                          (char *)&(unsigned int){OSSwapHostToBigInt32(cls)}, (char *)&(unsigned int){OSSwapHostToBigInt32(id)},
                          target && be4((void *)target) == 'sign' ? (char *)tbuf : "----");
    R[3] = 0;
    return 1;
}

/* Deliver PDon by calling the guest's installed 'APlr'/'PDon' handler. */
static void apl_send_done(void) {
    static unsigned int handler_be, refcon_be;
    static unsigned char evt[8], rep[8];
    unsigned int args[5];
    if (!g_ae_gethandler) return;
    args[0] = 'APlr'; args[1] = 'PDon'; args[2] = (unsigned int)&handler_be; args[3] = (unsigned int)&refcon_be; args[4] = 0;
    if ((short)oah_callppc_n("oah_apl_geth", g_ae_gethandler, 5, args) || !handler_be) {
        if (apl_tr()) fprintf(stderr, "[apl] no PDon handler installed\n");
        return;
    }
    memset(evt, 0, sizeof evt); memset(rep, 0, sizeof rep);
    args[0] = (unsigned int)evt; args[1] = (unsigned int)rep; args[2] = OSSwapBigToHostInt32(refcon_be);
    oah_callppc_n("oah_apl_done", OSSwapBigToHostInt32(handler_be), 3, args);
    if (apl_tr()) fprintf(stderr, "[apl] track finished -> PDon handler 0x%08x\n", OSSwapBigToHostInt32(handler_be));
}

/* Called at every gen bridge entry on the main thread (next to oah_snd_pump). */
static void oah_apl_poll(void) {
    static int busy;
    if (!g_apl.done_pending || busy || !pthread_main_np()) return;
    busy = 1;
    g_apl.done_pending = 0;
    apl_send_done();
    busy = 0;
}

/* Guest FSRef -> POSIX path via the guest's FSRefMakePath. */
static int apl_fsref_path(unsigned int ref, char *out, unsigned int max) {
    unsigned int args[3];
    if (!g_fs_refpath) g_fs_refpath = oah_guest_sym("CarbonCore", "_FSRefMakePath");
    if (!g_fs_refpath || !ref) return -1;
    args[0] = ref; args[1] = (unsigned int)out; args[2] = max;
    out[0] = 0;
    return (short)oah_callppc_n("oah_apl_path", g_fs_refpath, 3, args);
}

static int oah_apl_call(const char *name, unsigned int *R) {
    if (!strcmp(name, "AECreateAppleEvent")) return apl_create_event(R);
    if (!strcmp(name, "LSOpenFromRefSpec")) {
        /* (const LSLaunchFSRefSpec *spec, FSRef *outLaunched): spec->appRef is the first word */
        static char path[1024];
        unsigned int spec = R[3], app = spec ? be4((void *)spec) : 0;
        int e = apl_fsref_path(app, path, sizeof path);
        size_t L = strlen(path);
        if (!e && L >= 11 && !strcmp(path + L - 11, "APlayer.app")) {
            if (apl_tr()) fprintf(stderr, "[apl] LSOpenFromRefSpec(%s) -> handled natively\n", path);
            if (R[4]) memcpy((void *)R[4], (void *)app, 80);
            R[3] = 0;
            return 1;
        }
        fprintf(stderr, "[apl] LSOpenFromRefSpec(%s) err=%d -> not supported\n", path, e);
        R[3] = (unsigned int)-10810;    /* kLSUnknownErr */
        return 1;
    }
    if (!strcmp(name, "AESend")) {
        /* (const AppleEvent *evt, AppleEvent *reply, AESendMode, priority, timeout, idle, filter) */
        static unsigned char buf[96];
        unsigned int evt = R[3], reply = R[4], mode = R[5], cls, id, n;
        int e = 0;
        if (!evt || !apl_syms()) { R[3] = 0; return 1; }
        if (apl_ae_attr(evt, 'evcl', 'type', buf, 4) != 4) { R[3] = 0; return 1; }
        cls = be4(buf);
        if (apl_ae_attr(evt, 'evid', 'type', buf, 4) != 4) { R[3] = 0; return 1; }
        id = be4(buf);
        if (cls == 'aevt' && id == 'quit') {
            /* only APlayer's quit: target signature 'i5AP' */
            if (apl_ae_attr(evt, 'addr', 'sign', buf, 4) == 4 && be4(buf) == 'i5AP') {
                apl_close();
                if (apl_tr()) fprintf(stderr, "[apl] quit\n");
            }
            R[3] = 0;
            return 1;
        }
        if (cls != 'APlr') {
            /* anything else: HIToolbox AESend = AESendMessage (bridged in oah_aen.h) */
            static unsigned int fsm;
            unsigned int args[4];
            if (!fsm) fsm = oah_guest_sym("/AE.framework/", "_AESendMessage");
            args[0] = evt; args[1] = reply; args[2] = mode; args[3] = R[7];
            R[3] = fsm ? oah_callppc_n("AESendMessage", fsm, 4, args) : 0;
            if (apl_tr()) fprintf(stderr, "[apl] AESend %.4s/%.4s -> AESendMessage %d\n", (char *)&(unsigned int){OSSwapHostToBigInt32(cls)},
                                  (char *)&(unsigned int){OSSwapHostToBigInt32(id)}, (short)R[3]);
            return 1;
        }
        switch (id) {
        case 'POpn': {
            static char path[1024];
            if (apl_ae_get(0, evt, '----', 'fsrf', buf, 80) != 80) { e = -1701; break; }
            if (apl_fsref_path((unsigned int)buf, path, sizeof path)) { e = -43; break; }
            e = apl_open(path);
            if (apl_tr()) fprintf(stderr, "[apl] open \"%s\" -> %d\n", path, e);
            break;
        }
        case 'PPly':
            n = apl_ae_get(0, evt, '----', 'bool', buf, 1);
            g_apl.loop = n == 1 ? buf[0] != 0 : 0;
            if (g_apl.file) { g_apl.done_pending = 0; g_apl.playing = 1; }
            if (apl_tr()) fprintf(stderr, "[apl] play loop=%d file=%s\n", g_apl.loop, g_apl.file ? "yes" : "no");
            break;
        case 'PStp':
            g_apl.playing = 0;
            if (apl_tr()) fprintf(stderr, "[apl] stop\n");
            break;
        case 'PCls':
            apl_close();
            if (apl_tr()) fprintf(stderr, "[apl] close\n");
            break;
        case 'PVol':
            if (apl_ae_get(0, evt, '----', 'sing', buf, 4) == 4) {
                union { unsigned int u; float f; } v;
                v.u = be4(buf);
                if (v.f != v.f || v.f < 0) v.f = 0;
                if (v.f > 1) v.f = 1;
                g_apl.vol = v.f;
                if (apl_tr()) fprintf(stderr, "[apl] volume %.2f\n", v.f);
            }
            break;
        case 'PIsP': {
            static unsigned char b;
            b = g_apl.playing ? 1 : 0;
            if (reply && (mode & 3) == 3 && g_ae_createlist && g_ae_putparam) {
                unsigned int args[5];
                args[0] = 0; args[1] = 0; args[2] = 1; args[3] = reply;
                if (!(short)oah_callppc_n("oah_apl_list", g_ae_createlist, 4, args)) {
                    args[0] = reply; args[1] = '----'; args[2] = 'bool'; args[3] = (unsigned int)&b; args[4] = 1;
                    oah_callppc_n("oah_apl_put", g_ae_putparam, 5, args);
                }
            }
            break;
        }
        default:
            { char t[16]; unsigned int b = OSSwapHostToBigInt32(id); snprintf(t, sizeof t, "event %.4s", (char *)&b); oah_tel("apl", t); }
            if (apl_tr()) fprintf(stderr, "[apl] unknown APlr event %.4s\n", (char *)&(unsigned int){OSSwapHostToBigInt32(id)});
        }
        R[3] = (unsigned int)(e < 0 ? e : 0);
        return 1;
    }
    return 0;
}
