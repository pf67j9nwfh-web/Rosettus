/* oah_au.h - CoreAudio HAL properties and output AudioUnits for PowerPC guests (included by
   liboah_mojave.c).

   Snow Leopard's PowerPC CoreAudio cannot reach Mojave's coreaudiod: the default output device
   is 0, device properties fail with '!dev', and an output AudioUnit opens, "starts" and never
   calls its render callback (no music in Bejeweled 2, whose BASSMOD plays through the default
   output unit). So:
     - AudioHardwareGetProperty(Info) / AudioDeviceGetProperty(Info) / AudioDeviceSetProperty
       answer from Mojave's HAL (values byte-swapped by type);
     - AudioUnit calls on an output unit (type 'auou', subtype 'def ' / 'sys ' / 'ahal') are served
       by a native default output unit; every other AudioUnit runs the original guest code
       (oah_guest_orig: the two instructions the bridge mark replaced, from LIBRARY.prebridge).
   The guest render callback is guest code and cannot run on CoreAudio's real-time thread: the
   native unit plays from a ring buffer that oah_au_pump() refills by calling the guest callback
   at bridge entries (any guest thread, never re-entered). OAH_NO_AUBRIDGE=1 disables,
   OAH_AUTRACE=1 logs. */
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

static int oah_autr(void) { static int t = -1; if (t < 0) t = getenv("OAH_AUTRACE") ? 1 : 0; return t; }
static inline unsigned int au_be32(unsigned int p) { return OSSwapBigToHostInt32(*(volatile unsigned int *)p); }
static inline void au_put32(unsigned int p, unsigned int v) { *(volatile unsigned int *)p = OSSwapHostToBigInt32(v); }
static inline double au_bef64(unsigned int p) { unsigned long long u = OSSwapBigToHostInt64(*(volatile unsigned long long *)p); double d; memcpy(&d, &u, 8); return d; }
static inline void au_putf64(unsigned int p, double d) { unsigned long long u; memcpy(&u, &d, 8); *(volatile unsigned long long *)p = OSSwapHostToBigInt64(u); }
static inline float au_bef32(unsigned int p) { unsigned int u = au_be32(p); float f; memcpy(&f, &u, 4); return f; }

/* ---- calling the original (unbridged) guest function ---- */
static const char *oah_guest_image_name(unsigned int addr, unsigned int *base_out);
/* Executable memory for small guest code stubs. Allocated with the guest's own mmap, so translate
   records the region: a page from a native mmap after startup lies outside translate's region list and
   the first guest jump into it ends in translate's fatal() -> exit(1) (blender: OpenAL's 3D mixer
   through the AudioUnitSetProperty trampoline). Falls back to a native page. Carves 64-byte slots. */
#define OAH_CODE_CHUNK 65536   /* one guest mmap serves many stubs: a later one may collide with a busy guest */
/* a fresh OAH_CODE_CHUNK of code memory: the guest's mmap when the guest call lock is free, else native */
static unsigned int oah_guest_code_page(void) {
    static unsigned int gmm;
    unsigned int p = 0, a[7], fmm;
    int k;
    if (!gmm) gmm = oah_guest_sym("libSystem", "_mmap");
    fmm = gmm;
    pthread_once(&g_callppc_once, oah_callppc_mu_init);
    for (k = 0; fmm && k < 500; k++) {
        if (pthread_mutex_trylock(&g_callppc_mu) == 0) { pthread_mutex_unlock(&g_callppc_mu); break; }
        usleep(2000);
    }
    if (fmm && pthread_mutex_trylock(&g_callppc_mu) != 0) {
        fprintf(stderr, "[codepage] guest calls busy on another thread: native page\n");
        fmm = 0;
    } else if (fmm) pthread_mutex_unlock(&g_callppc_mu);
    if (fmm) {
        a[0] = 0; a[1] = OAH_CODE_CHUNK; a[2] = PROT_READ | PROT_WRITE | PROT_EXEC; a[3] = MAP_ANON | MAP_PRIVATE;
        a[4] = (unsigned int)-1; a[5] = 0; a[6] = 0;
        p = oah_callppc_n("oah_codepage", fmm, 7, a);
        if (p == (unsigned int)-1) p = 0;
    }
    if (!p) {
        void *n = mmap(0, OAH_CODE_CHUNK, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        p = n == MAP_FAILED ? 0 : (unsigned int)n;
        fprintf(stderr, "[codepage] guest mmap unavailable: native page %08x\n", p);
    }
    return p;
}
static unsigned int oah_guest_code_alloc(unsigned int bytes) {
    static unsigned int page, used;
    static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    unsigned int r = 0;
    bytes = (bytes + 63) & ~63u;
    if (bytes > 4096) return 0;
    pthread_mutex_lock(&mx);
    if (page && used + bytes <= OAH_CODE_CHUNK) {
        r = page + used; used += bytes;
        pthread_mutex_unlock(&mx);
        return r;
    }
    /* A new page: not under mx. Getting it calls the guest's mmap, and holding mx through that
       call deadlocked the G4 demos at launch: one thread held mx in the guest call, the main
       thread waited for mx in a gtrace hook (_LSCopyLaunchModifiers needed its trampoline) while
       the guest call could not proceed. OAH_CODEALLOC_OLD=1: the old way (lock held). */
    if (!getenv("OAH_CODEALLOC_OLD")) {
        static volatile int filling;
        unsigned int p;
        pthread_mutex_unlock(&mx);
        {   int k = 0;
            while (!__sync_bool_compare_and_swap(&filling, 0, 1)) {   /* one filler at a time; others wait for it */
                usleep(1000);
                pthread_mutex_lock(&mx);
                if (page && used + bytes <= OAH_CODE_CHUNK) { r = page + used; used += bytes; pthread_mutex_unlock(&mx); return r; }
                pthread_mutex_unlock(&mx);
                if (++k == 100) {
                    /* the filler's guest call may be waiting for this thread (it holds the guest call
                       lock, which is recursive): get a page itself, from the guest's mmap when it can
                       (a native page is unknown to translate as code: fontworker died of that) */
                    unsigned int p2 = oah_guest_code_page();
                    fprintf(stderr, "[codepage] another thread is getting a page: own page %08x\n", p2);
                    if (!p2) return 0;
                    pthread_mutex_lock(&mx);
                    page = p2; used = bytes; r = page;
                    pthread_mutex_unlock(&mx);
                    return r;
                }
            }
        }
        pthread_mutex_lock(&mx);
        if (page && used + bytes <= OAH_CODE_CHUNK) {   /* another thread filled it meanwhile */
            r = page + used; used += bytes; pthread_mutex_unlock(&mx); filling = 0; return r;
        }
        pthread_mutex_unlock(&mx);
        {   struct timeval t0, t1;
            gettimeofday(&t0, 0);
            p = oah_guest_code_page();
            gettimeofday(&t1, 0);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000 > 500)
                fprintf(stderr, "[codepage] page %08x took %ld ms (thread %x)\n", p,
                        (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000), pthread_mach_thread_np(pthread_self()));
        }
        pthread_mutex_lock(&mx);
        if (p) { page = p; used = 0; }
        if (page && used + bytes <= OAH_CODE_CHUNK) { r = page + used; used += bytes; }
        pthread_mutex_unlock(&mx);
        filling = 0;
        return r;
    }
    if (!page || used + bytes > OAH_CODE_CHUNK) {
        static unsigned int gmm;
        unsigned int p = 0, a[7], fmm;
        if (!gmm) gmm = oah_guest_sym("libSystem", "_mmap");
        fmm = gmm;
        /* Another thread may be inside a nested guest call (holding the call lock) and waiting on a
           guest lock this thread holds: fontworker deadlocked so, one thread in FSNode_VolumeMounted
           (universe lock held) reaching an IOKit bridge that needed a fresh code page, the other in
           a nested call waiting for the universe lock. Do not wait for the guest's mmap then: take
           a native page. */
        pthread_once(&g_callppc_once, oah_callppc_mu_init);
        {   /* a busy lock is usually just another thread's short guest call: give it up to 1 s */
            int k;
            for (k = 0; fmm && k < 500; k++) {
                if (pthread_mutex_trylock(&g_callppc_mu) == 0) { pthread_mutex_unlock(&g_callppc_mu); break; }
                usleep(2000);
            }
        }
        if (fmm && pthread_mutex_trylock(&g_callppc_mu) != 0) {
            fprintf(stderr, "[codepage] guest calls busy on another thread: native page\n");
            fmm = 0;
        } else if (fmm) pthread_mutex_unlock(&g_callppc_mu);
        if (fmm) {
            a[0] = 0; a[1] = OAH_CODE_CHUNK; a[2] = PROT_READ | PROT_WRITE | PROT_EXEC; a[3] = MAP_ANON | MAP_PRIVATE;
            a[4] = (unsigned int)-1; a[5] = 0; a[6] = 0;
            p = oah_callppc_n("oah_codepage", fmm, 7, a);
            if (p == (unsigned int)-1) p = 0;
        }
        if (!p) {
            void *n = mmap(0, OAH_CODE_CHUNK, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
            p = n == MAP_FAILED ? 0 : (unsigned int)n;
            fprintf(stderr, "[codepage] guest mmap unavailable: native page %08x\n", p);
        }
        page = p; used = 0;
    }
    if (page) { r = page + used; used += bytes; }
    pthread_mutex_unlock(&mx);
    return r;
}
/* Trampoline running the guest function's original first two instructions, then the rest of it.
   0 if they cannot be relocated (a relative branch). */
static unsigned int oah_guest_orig(const char *lib, const char *sym) {
    unsigned int addr = oah_guest_sym(lib, sym), base = 0, w[2], *t;
    const char *img;
    char path[1200], root[1024];
    Dl_info di;
    FILE *f;
    int k;
    if (!addr || !(img = oah_guest_image_name(addr, &base)) || !base) return 0;
    if (!dladdr((void *)oah_guest_orig, &di) || !di.dli_fname) return 0;
    snprintf(root, sizeof root, "%s", di.dli_fname);
    if (strrchr(root, '/')) *strrchr(root, '/') = 0;
    snprintf(path, sizeof path, "%s%s.prebridge", root, img);
    if (!(f = fopen(path, "rb"))) { fprintf(stderr, "[au] %s: no %s\n", sym, path); return 0; }
    {   unsigned int hdr[2], slice = 0, n, i, fa[5];
        if (fread(hdr, 4, 2, f) != 2) { fclose(f); return 0; }
        if (OSSwapBigToHostInt32(hdr[0]) == 0xcafebabeu) {
            n = OSSwapBigToHostInt32(hdr[1]);
            for (i = 0; i < n; i++) {
                if (fread(fa, 4, 5, f) != 5) break;
                if (OSSwapBigToHostInt32(fa[0]) == 18) { slice = OSSwapBigToHostInt32(fa[2]); break; }
            }
        }
        if (fseek(f, (long)(slice + (addr - base)), SEEK_SET) || fread(w, 4, 2, f) != 2) { fclose(f); return 0; }
        fclose(f);
    }
    for (k = 0; k < 2; k++) {
        unsigned int op = OSSwapBigToHostInt32(w[k]) >> 26;
        if (op == 16 || op == 18) { fprintf(stderr, "[au] %s: original starts with a branch, not relocatable\n", sym); return 0; }
    }
    t = (unsigned int *)oah_guest_code_alloc(6 * 4);
    if (!t) return 0;
    addr += 8;
    t[0] = w[0]; t[1] = w[1];                                     /* already big-endian */
    t[2] = OSSwapHostToBigInt32(0x3d800000 | (addr >> 16));        /* lis r12,hi */
    t[3] = OSSwapHostToBigInt32(0x618c0000 | (addr & 0xffff));     /* ori r12,r12,lo */
    t[4] = OSSwapHostToBigInt32(0x7d8903a6);                       /* mtctr r12 */
    t[5] = OSSwapHostToBigInt32(0x4e800420);                       /* bctr */
    return (unsigned int)t;
}

/* ---- HAL properties ---- */
static int au_prop_is_f64(unsigned int p) {
    return p == 'nsrt' || p == 'nsr#' || p == 'fsz#' || p == 'srat' || p == 'asrt' || p == 'vfsz';
}
static void au_swap_out(unsigned int prop, unsigned int gdata, const unsigned char *nat, unsigned int size) {
    unsigned int i;
    if (!gdata) return;
    if (prop == 'sfmt' || prop == 'pft ' || prop == 'sfma') {      /* AudioStreamBasicDescription(s), 40 bytes each */
        for (i = 0; i + 40 <= size; i += 40) {
            double sr; memcpy(&sr, nat + i, 8); au_putf64(gdata + i, sr);
            { unsigned int k; for (k = 8; k < 40; k += 4) { unsigned int v; memcpy(&v, nat + i + k, 4); au_put32(gdata + i + k, v); } }
        }
        return;
    }
    if (au_prop_is_f64(prop)) { for (i = 0; i + 8 <= size; i += 8) { double d; memcpy(&d, nat + i, 8); au_putf64(gdata + i, d); } return; }
    if (prop == 'name' || prop == 'makr') { memcpy((void *)gdata, nat, size); return; }   /* C strings */
    for (i = 0; i + 4 <= size; i += 4) { unsigned int v; memcpy(&v, nat + i, 4); au_put32(gdata + i, v); }
}
static CFStringRef oah_guest_str_native(unsigned int gs);
/* Properties whose data is an AudioValueTranslation {inData*, inSize, outData*, outSize} (all
   big-endian guest words): the native HAL read the guest's pointers as its own and crashed
   (ScreenFlowHelper's "New Recording" asks 'lscn' for each input source: fault at 'imic').
   kind 1: UInt32 in, CFString out; 2: UInt32 in, C string out; 3: CFString in, UInt32 out. */
static int au_xlat_kind(unsigned int p) {
    if (p == 'lscn' || p == 'lcsn' || p == 'lcnl' || p == 'hcfn' || p == 'mddc') return 1;
    if (p == 'sscn' || p == 'cscn') return 2;
    if (p == 'duid' || p == 'tdid' || p == 'puid' || p == 'cuid') return 3;
    return 0;
}
/* get(ctx, &size, data) runs the native call. Returns the status; fills the guest translation. */
static OSStatus au_xlat(unsigned int kind, unsigned int g, OSStatus (*get)(void *, UInt32 *, void *), void *ctx) {
    struct { void *in; UInt32 insz; void *out; UInt32 outsz; } t;
    unsigned int gin, gout, goutsz, v = 0, idout = 0;
    CFStringRef s = 0, sin = 0;
    char cs[256];
    UInt32 sz = sizeof t;
    OSStatus e;
    if (!g || !oah_readable(g, 16)) return kAudioHardwareBadPropertySizeError;
    gin = au_be32(g); gout = au_be32(g + 8); goutsz = au_be32(g + 12);
    if (kind == 3) { sin = gin ? oah_guest_str_native(au_be32(gin)) : 0; t.in = &sin; t.insz = sizeof sin; t.out = &idout; t.outsz = 4; }
    else {
        v = gin ? au_be32(gin) : 0; t.in = &v; t.insz = 4;
        if (kind == 1) { t.out = &s; t.outsz = sizeof s; }
        else { t.out = cs; t.outsz = goutsz < sizeof cs ? goutsz : sizeof cs; cs[0] = 0; }
    }
    e = get(ctx, &sz, &t);
    if (!e && gout) {
        if (kind == 1) au_put32(gout, s ? oah_cf_to_guest(s) : 0);
        else if (kind == 2) { memcpy((void *)gout, cs, t.outsz); au_put32(g + 12, t.outsz); }
        else au_put32(gout, idout);
    }
    if (s) CFRelease(s);
    if (sin) CFRelease(sin);
    return e;
}
typedef struct { unsigned int dev, ch, in, prop; } au_devctx;
static OSStatus au_xlat_dev(void *c, UInt32 *sz, void *d) {
    au_devctx *x = (au_devctx *)c;
    return AudioDeviceGetProperty(x->dev, x->ch, (Boolean)x->in, x->prop, sz, d);
}
typedef struct { unsigned int obj; const AudioObjectPropertyAddress *ad; UInt32 qs; const void *q; } au_objctx;
static OSStatus au_xlat_obj(void *c, UInt32 *sz, void *d) {
    au_objctx *x = (au_objctx *)c;
    return AudioObjectGetPropertyData(x->obj, x->ad, x->qs, x->q, sz, d);
}
static OSStatus au_xlat_hw(void *c, UInt32 *sz, void *d) {
    return AudioHardwareGetProperty(*(unsigned int *)c, sz, d);
}
/* kAudioHardwarePropertyRunLoop. The native HAL answered NULL (or a native pointer): QuickTime's
   PrerollMovie CFReleases the answer without a NULL check and trapped (Mactracker's Startup Chime,
   second play). Answer the guest's main run loop, the 10.5 default. QuickTime releases it each
   time, so it is retained ahead once, on the first HAL call (before any render callback can hold
   the guest-call lock); later calls never call into the guest. */
static unsigned int g_au_rl, g_au_rl_left;
static void au_guest_runloop_retain(int wait) {
    static unsigned int gm, rt;
    static int tried;
    unsigned int a[1] = { 0 }, i;
    if (!tried) { tried = 1; gm = oah_guest_sym("CoreFoundation", "_CFRunLoopGetMain"); rt = oah_guest_sym("CoreFoundation", "_CFRetain"); }
    if (!gm || !rt) return;
    pthread_once(&g_callppc_once, oah_callppc_mu_init);
    if (!wait) { if (pthread_mutex_trylock(&g_callppc_mu)) return; pthread_mutex_unlock(&g_callppc_mu); }
    if (!g_au_rl && !(g_au_rl = oah_callppc_n("oah_au_rl", gm, 0, a))) return;
    for (i = 0; i < 4096; i++) { a[0] = g_au_rl; oah_callppc_n("oah_au_rl", rt, 1, a); }
    g_au_rl_left += 4096;
}
static unsigned int au_guest_runloop(void) {
    if (g_au_rl_left < 2) au_guest_runloop_retain(!g_au_rl);
    if (!g_au_rl || !g_au_rl_left) return 0;
    g_au_rl_left--;
    return g_au_rl;
}
static int oah_au_hal_call(const char *n, unsigned int *R) {
    if (n[0] != 'A' || strncmp(n, "Audio", 5)) return 0;
    if (!g_au_rl && !getenv("OAH_NO_AURUNLOOP")) au_guest_runloop_retain(0);   /* early, while no render runs */
    if (!strcmp(n, "AudioHardwareGetProperty") || !strcmp(n, "AudioHardwareGetPropertyInfo")) {
        /* (prop, UInt32 *ioSize, void *data) / (prop, UInt32 *outSize, Boolean *outWritable) */
        unsigned int prop = R[3];
        int info = n[24] == 'I';
        unsigned char buf[512]; UInt32 sz = sizeof buf; Boolean wr = 0; OSStatus e;
        if (info) {
            e = AudioHardwareGetPropertyInfo(prop, &sz, &wr);
            if (!e && R[4]) au_put32(R[4], sz);
            if (!e && R[5]) *(volatile unsigned char *)R[5] = wr;
        } else {
            UInt32 want = R[4] ? au_be32(R[4]) : 0;
            if (prop == 'cfst' || prop == 'svna') { R[3] = (unsigned int)kAudioHardwareUnknownPropertyError; return 1; }
            if (prop == 'rnlp') {                    /* kAudioHardwarePropertyRunLoop: see au_guest_runloop */
                unsigned int rl = au_guest_runloop();
                if (R[5] && want >= 4) au_put32(R[5], rl);
                if (R[4]) au_put32(R[4], 4);
                if (oah_autr()) fprintf(stderr, "[au] AudioHardwareGetProperty('rnlp') -> guest main run loop %08x\n", rl);
                R[3] = 0; return 1;
            }
            if (au_xlat_kind(prop)) {
                e = au_xlat(au_xlat_kind(prop), R[5], au_xlat_hw, &prop);
                sz = 16;
                if (!e && R[4]) au_put32(R[4], sz);
            } else {
            if (want < sz) sz = want;
            e = AudioHardwareGetProperty(prop, &sz, buf);
            if (!e) { au_swap_out(prop, R[5], buf, sz); au_put32(R[4], sz); }
            }
        }
        if (oah_autr()) fprintf(stderr, "[au] %s('%.4s') -> %d (size %u)\n", n, (char *)&(unsigned int){ OSSwapHostToBigInt32(prop) }, (int)e, (unsigned)sz);
        R[3] = (unsigned int)e;
        return 1;
    }
    if (!strcmp(n, "AudioDeviceGetProperty") || !strcmp(n, "AudioDeviceGetPropertyInfo") || !strcmp(n, "AudioDeviceSetProperty")) {
        /* Get:  (dev, ch, isInput, prop, UInt32 *ioSize, void *data)
           Info: (dev, ch, isInput, prop, UInt32 *outSize, Boolean *outWritable)
           Set:  (dev, const AudioTimeStamp *when, ch, isInput, prop, UInt32 size, const void *data) */
        unsigned char buf[2048]; UInt32 sz = sizeof buf; Boolean wr = 0; OSStatus e;
        unsigned int prop;
        if (n[11] == 'S') {
            unsigned int size = R[8], data = R[9], i;
            prop = R[7];
            if (size > sizeof buf) size = sizeof buf;
            if (au_prop_is_f64(prop)) for (i = 0; i + 8 <= size; i += 8) { double d = au_bef64(data + i); memcpy(buf + i, &d, 8); }
            else for (i = 0; i + 4 <= size; i += 4) { unsigned int v = au_be32(data + i); memcpy(buf + i, &v, 4); }
            e = AudioDeviceSetProperty(R[3], 0, R[5], (Boolean)R[6], prop, size, buf);
        } else if (n[22] == 'I') {
            prop = R[6];
            e = AudioDeviceGetPropertyInfo(R[3], R[4], (Boolean)R[5], prop, &sz, &wr);
            if (!e && R[7]) au_put32(R[7], sz);
            if (!e && R[8]) *(volatile unsigned char *)R[8] = wr;
        } else {
            UInt32 want = R[7] ? au_be32(R[7]) : 0;
            prop = R[6];
            if (prop == 'lnam' || prop == 'lmak' || prop == 'uid ' || prop == 'muid') {   /* CFStrings */
                CFStringRef s = 0; sz = sizeof s;
                e = AudioDeviceGetProperty(R[3], R[4], (Boolean)R[5], prop, &sz, &s);
                if (!e && R[8]) au_put32(R[8], s ? oah_cf_to_guest(s) : 0);
                if (s) CFRelease(s);
                sz = 4;
            } else if (au_xlat_kind(prop)) {
                au_devctx x = { R[3], R[4], R[5], prop };
                e = au_xlat(au_xlat_kind(prop), R[8], au_xlat_dev, &x);
                sz = 16;
            } else {
                if (want < sz) sz = want;
                e = AudioDeviceGetProperty(R[3], R[4], (Boolean)R[5], prop, &sz, buf);
                if (!e) au_swap_out(prop, R[8], buf, sz);
            }
            if (!e && R[7]) au_put32(R[7], sz);
        }
        if (oah_autr()) fprintf(stderr, "[au] %s(dev %u, '%.4s') -> %d (size %u)\n", n, R[3], (char *)&(unsigned int){ OSSwapHostToBigInt32(prop) }, (int)e, (unsigned)sz);
        R[3] = (unsigned int)e;
        return 1;
    }
    if (!strncmp(n, "AudioObject", 11)) {
        /* 10.5+ HAL API, used inside the guest CoreAudio/AudioUnit code itself (the 3D mixer asks for
           the default output with AudioObjectGetPropertyData; the guest HAL's MIG 43000 to coreaudiod
           fails on Mojave and the guest HAL then exit(1)s). Answer from the native HAL. */
        AudioObjectPropertyAddress ad;
        unsigned char buf[4096], qb[256]; UInt32 sz = sizeof buf, qs = 0; OSStatus e = 0;
        const void *q = 0; const char *op = n + 11;
        unsigned int ga = R[4], i, obj = R[3];
        if (!ga) { R[3] = (unsigned int)kAudioHardwareIllegalOperationError; return 1; }
        ad.mSelector = au_be32(ga); ad.mScope = au_be32(ga + 4); ad.mElement = au_be32(ga + 8);
        if (!strcmp(op, "GetPropertyData") || !strcmp(op, "GetPropertyDataSize") || !strcmp(op, "SetPropertyData")) {
            qs = R[5];
            if (qs && R[6] && qs <= sizeof qb) {
                if (ad.mSelector == 'uidd' || ad.mSelector == 'uidp' || ad.mSelector == 'uidb' || ad.mSelector == 'bidp' || ad.mSelector == 'tidp') {
                    CFStringRef s = oah_guest_str_native(au_be32(R[6]));   /* CFString qualifier (retained) */
                    memcpy(qb, &s, sizeof s); qs = sizeof s;
                } else for (i = 0; i + 4 <= qs; i += 4) { unsigned int v = au_be32(R[6] + i); memcpy(qb + i, &v, 4); }
                q = qb;
            } else qs = 0;
        }
        if (!strcmp(op, "HasProperty")) {
            R[3] = AudioObjectHasProperty(R[3], &ad) ? 1 : 0;
        } else if (!strcmp(op, "IsPropertySettable")) {
            Boolean wr = 0; e = AudioObjectIsPropertySettable(R[3], &ad, &wr);
            if (!e && R[5]) *(volatile unsigned char *)R[5] = wr;
            R[3] = (unsigned int)e;
        } else if (!strcmp(op, "GetPropertyDataSize")) {
            e = AudioObjectGetPropertyDataSize(R[3], &ad, qs, q, &sz);
            if (!e && R[7]) au_put32(R[7], sz);
            R[3] = (unsigned int)e;
        } else if (!strcmp(op, "GetPropertyData")) {
            unsigned int sel = ad.mSelector, out = R[8];
            UInt32 want = R[7] ? au_be32(R[7]) : 0;
            if (sel == 'lnam' || sel == 'lmak' || sel == 'uid ' || sel == 'muid' || sel == 'modl' || sel == 'lcnm' || sel == 'lmkr' || sel == 'lsnm') {
                CFStringRef s = 0; sz = sizeof s;
                e = AudioObjectGetPropertyData(R[3], &ad, qs, q, &sz, &s);
                if (!e && out) au_put32(out, s ? oah_cf_to_guest(s) : 0);
                if (s) CFRelease(s);
                sz = 4;
            } else if (au_xlat_kind(sel)) {
                au_objctx x = { R[3], &ad, qs, q };
                e = au_xlat(au_xlat_kind(sel), out, au_xlat_obj, &x);
                sz = 16;
            } else {
                if (want < sz) sz = want;
                e = AudioObjectGetPropertyData(R[3], &ad, qs, q, &sz, buf);
                if (!e) au_swap_out(sel, out, buf, sz);
            }
            if (!e && R[7]) au_put32(R[7], sz);
            R[3] = (unsigned int)e;
        } else if (!strcmp(op, "SetPropertyData")) {
            unsigned int size = R[7], data = R[8];
            if (size > sizeof buf) size = sizeof buf;
            if (au_prop_is_f64(ad.mSelector)) for (i = 0; i + 8 <= size; i += 8) { double d = au_bef64(data + i); memcpy(buf + i, &d, 8); }
            else for (i = 0; i + 4 <= size; i += 4) { unsigned int v = au_be32(data + i); memcpy(buf + i, &v, 4); }
            e = AudioObjectSetPropertyData(R[3], &ad, qs, q, size, buf);
            R[3] = (unsigned int)e;
        } else if (!strcmp(op, "AddPropertyListener") || !strcmp(op, "RemovePropertyListener")) {
            R[3] = 0;     /* guest listener procs are not called (no change notifications) */
        } else return 0;
        if (oah_autr()) fprintf(stderr, "[au] AudioObject%s(%u, '%.4s'/'%.4s'/%u) -> %d (size %u)\n", op, obj,
                                (char *)&(unsigned int){ OSSwapHostToBigInt32(ad.mSelector) }, (char *)&(unsigned int){ OSSwapHostToBigInt32(ad.mScope) },
                                (unsigned)ad.mElement, (int)e, (unsigned)sz);
        if (qs && q == qb && (ad.mSelector == 'uidd' || ad.mSelector == 'uidp' || ad.mSelector == 'uidb' || ad.mSelector == 'bidp' || ad.mSelector == 'tidp')) {
            CFStringRef s; memcpy(&s, qb, sizeof s); if (s) CFRelease(s);
        }
        return 1;
    }
    return 0;
}

/* ---- output unit proxy ---- */
#define AU_RING 16384                            /* stereo frames (power of two) */
typedef struct oah_au {
    struct oah_au *next;
    unsigned int gci;                             /* guest ComponentInstance */
    AudioUnit nau;                                /* native default output */
    int initialized, running;
    unsigned int cbproc, cbref, oldcb;            /* guest render callback (oldcb: SetInputCallback) */
    double rate; unsigned int fmtflags, ch, bits, bpf, nonint;   /* guest input format */
    unsigned int maxframes;
    float vol;
    float ring[AU_RING * 2];
    volatile unsigned int rd, wr;                 /* frames, free-running */
    double sampletime;
    unsigned long renders, underruns, pulls;
    unsigned int gbuf, gbufsize, gabl, gflags, gts; /* guest-side call scratch */
    /* input (AUHAL with EnableIO on input element 1): the native unit records into iring; the guest's
       input callback is called (like a render callback) when 512 frames are there, and its
       AudioUnitRender(bus 1) copies them out in the guest's format */
    int in_on, out_off, cur_in;
    unsigned int incb, incbref;
    double irate; unsigned int iflags, ich, ibits, inonint;   /* guest format of the recorded data */
    float iring[AU_RING * 2];
    volatile unsigned int ird, iwr;
    unsigned int igbuf, igbufsize;                /* data buffer handed out when the guest passes mData NULL */
    unsigned long icalls, ioverruns;
} oah_au;
static oah_au *g_aus;
static pthread_mutex_t g_au_mx = PTHREAD_MUTEX_INITIALIZER;
static OSStatus au_native_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    oah_au *a = (oah_au *)ref;
    float *out = (float *)io->mBuffers[0].mData;
    unsigned int avail = a->wr - a->rd, i, take = n < avail ? n : avail;
    (void)fl; (void)ts; (void)bus;
    for (i = 0; i < take; i++) {
        unsigned int k = (a->rd + i) & (AU_RING - 1);
        out[2 * i] = a->ring[2 * k] * a->vol; out[2 * i + 1] = a->ring[2 * k + 1] * a->vol;
    }
    for (; i < n; i++) out[2 * i] = out[2 * i + 1] = 0;
    __sync_synchronize();
    a->rd += take;
    if (take < n && a->running) a->underruns++;
    a->renders++;
    return noErr;
}
static OSStatus au_native_input(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *unused) {
    oah_au *a = (oah_au *)ref;
    static float tmp[2 * 4096];
    AudioBufferList bl;
    unsigned int i, space;
    (void)bus; (void)unused;
    if (n > 4096) n = 4096;
    bl.mNumberBuffers = 1; bl.mBuffers[0].mNumberChannels = 2; bl.mBuffers[0].mDataByteSize = n * 8; bl.mBuffers[0].mData = tmp;
    if (AudioUnitRender(a->nau, fl, ts, 1, n, &bl)) return noErr;
    space = AU_RING - (a->iwr - a->ird);
    if (n > space) { a->ioverruns++; n = space; }
    for (i = 0; i < n; i++) { unsigned int k = (a->iwr + i) & (AU_RING - 1); a->iring[2 * k] = tmp[2 * i]; a->iring[2 * k + 1] = tmp[2 * i + 1]; }
    __sync_synchronize();
    a->iwr += n;
    a->renders++;
    return noErr;
}
static oah_au *au_find(unsigned int gci) { oah_au *a; for (a = g_aus; a; a = a->next) if (a->gci == gci) return a; return 0; }
/* Is this guest ComponentInstance an output unit? (guest GetComponentInfo) */
static int au_is_output(unsigned int gci) {
    static unsigned int gci_fn; static unsigned int known[64], verdict[64]; static int nk;
    unsigned int desc[5], a[5], i;
    for (i = 0; i < (unsigned int)nk; i++) if (known[i] == gci) return verdict[i];
    if (!gci_fn) gci_fn = oah_guest_sym("CarbonCore", "_GetComponentInfo");
    if (!gci_fn) return 0;
    memset(desc, 0, sizeof desc);
    a[0] = gci; a[1] = (unsigned int)desc; a[2] = 0; a[3] = 0; a[4] = 0;
    oah_callppc_n("oah_au", gci_fn, 5, a);
    {   unsigned int ty = OSSwapBigToHostInt32(desc[0]), st = OSSwapBigToHostInt32(desc[1]);
        int v = ty == 'auou' && (st == 'def ' || st == 'sys ' || st == 'ahal');
        if (nk < 64) { known[nk] = gci; verdict[nk] = v; nk++; }
        if (oah_autr()) fprintf(stderr, "[au] instance %08x is '%.4s'/'%.4s'%s\n", gci, (char *)&desc[0], (char *)&desc[1], v ? ": native output" : "");
        return v;
    }
}
static oah_au *au_get(unsigned int gci) {
    oah_au *a;
    pthread_mutex_lock(&g_au_mx);
    a = au_find(gci);
    pthread_mutex_unlock(&g_au_mx);
    if (a || !au_is_output(gci)) return a;
    a = (oah_au *)calloc(1, sizeof *a);
    a->gci = gci; a->rate = 44100; a->fmtflags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsBigEndian | kAudioFormatFlagIsPacked;
    a->ch = 2; a->bits = 32; a->bpf = 8; a->maxframes = 1156; a->vol = 1.0f;
    a->irate = 44100; a->iflags = a->fmtflags; a->ich = 2; a->ibits = 32;
    {   AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent c = AudioComponentFindNext(NULL, &d);
        if (!c || AudioComponentInstanceNew(c, &a->nau)) { fprintf(stderr, "[au] no native default output\n"); a->nau = 0; }
    }
    pthread_mutex_lock(&g_au_mx);
    a->next = g_aus; g_aus = a;
    pthread_mutex_unlock(&g_au_mx);
    return a;
}
static void au_native_setup(oah_au *a) {
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb;
    if (a->in_on) {                                 /* recording: a native AUHAL on the default input device */
        AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent c = AudioComponentFindNext(NULL, &d);
        AudioDeviceID dev = 0; UInt32 sz = sizeof dev, one = 1, zero = 0; Float64 drate = 44100;
        OSStatus e = -1;
        if (a->nau) { AudioComponentInstanceDispose(a->nau); a->nau = 0; }
        if (c && !AudioComponentInstanceNew(c, &a->nau)) {
            AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &sz, &dev);
            sz = sizeof drate; AudioDeviceGetProperty(dev, 0, true, kAudioDevicePropertyNominalSampleRate, &sz, &drate);
            AudioUnitSetProperty(a->nau, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
            AudioUnitSetProperty(a->nau, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof zero);
            e = AudioUnitSetProperty(a->nau, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
            memset(&f, 0, sizeof f);
            f.mSampleRate = drate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
            f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mBytesPerFrame = f.mBytesPerPacket = 8;
            if (!e) e = AudioUnitSetProperty(a->nau, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
            cb.inputProc = au_native_input; cb.inputProcRefCon = a;
            if (!e) e = AudioUnitSetProperty(a->nau, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
        }
        if (oah_autr() || e) fprintf(stderr, "[au] %08x: input from device %u at %.0f Hz -> %d\n", a->gci, (unsigned)dev, drate, (int)e);
        return;
    }
    if (!a->nau) return;
    memset(&f, 0, sizeof f);
    f.mSampleRate = a->rate > 1000 ? a->rate : 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mBytesPerFrame = f.mBytesPerPacket = 8;
    AudioUnitSetProperty(a->nau, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    cb.inputProc = au_native_render; cb.inputProcRefCon = a;
    AudioUnitSetProperty(a->nau, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
}
/* Read guest sample i of channel c from the guest buffer(s). */
static float au_gsample(oah_au *a, unsigned int abl, unsigned int frame, unsigned int c) {
    unsigned int nb = au_be32(abl), buf, ch, p;
    int be = (a->fmtflags & kAudioFormatFlagIsBigEndian) != 0;
    if (a->nonint) { if (c >= nb) c = nb - 1; buf = abl + 4 + 12 * c; ch = 1; c = 0; }
    else { buf = abl + 4; ch = a->ch ? a->ch : 2; if (c >= ch) c = ch - 1; }
    p = au_be32(buf + 8) + (frame * ch + c) * (a->bits / 8);
    if (a->fmtflags & kAudioFormatFlagIsFloat) {
        unsigned int u = *(volatile unsigned int *)p; float f;
        u = be ? OSSwapBigToHostInt32(u) : OSSwapLittleToHostInt32(u); memcpy(&f, &u, 4); return f;
    }
    if (a->bits == 16) { unsigned short u = *(volatile unsigned short *)p; u = be ? OSSwapBigToHostInt16(u) : OSSwapLittleToHostInt16(u); return (short)u / 32768.f; }
    if (a->bits == 32) { unsigned int u = *(volatile unsigned int *)p; u = be ? OSSwapBigToHostInt32(u) : OSSwapLittleToHostInt32(u);
        return (a->fmtflags & 0x1f80) ? (int)u / 16777216.f : (int)u / 2147483648.f; }   /* 8.24 fixed if a fraction shift is set */
    if (a->bits == 8) return ((int)*(volatile unsigned char *)p - 128) / 128.f;
    return 0;
}
/* One render of 512 frames: set up the guest buffers (au_prep) and the callback's arguments,
   then, after the guest callback ran, copy its samples into the ring (au_take). */
static unsigned int au_prep(oah_au *a, unsigned int *need_out, unsigned int args[6], unsigned int *proc) {
    unsigned int n = 512, i, nb = a->nonint ? (a->ch ? a->ch : 2) : 1;
    unsigned int need = n * (a->bits / 8) * (a->nonint ? 1 : (a->ch ? a->ch : 2));
    if (a->gbufsize < need * nb) {
        free((void *)a->gbuf); a->gbuf = (unsigned int)calloc(1, need * nb); a->gbufsize = need * nb;
        if (!a->gabl) { a->gabl = (unsigned int)calloc(1, 4 + 12 * 8); a->gflags = (unsigned int)calloc(1, 4); a->gts = (unsigned int)calloc(1, 64); }
    }
    au_put32(a->gabl, nb);
    for (i = 0; i < nb; i++) {
        au_put32(a->gabl + 4 + 12 * i, a->nonint ? 1 : (a->ch ? a->ch : 2));
        au_put32(a->gabl + 8 + 12 * i, need);
        au_put32(a->gabl + 12 + 12 * i, a->gbuf + need * i);
    }
    au_put32(a->gflags, 0);
    memset((void *)a->gts, 0, 64);
    au_putf64(a->gts, a->sampletime);
    {   unsigned long long ht = mach_absolute_time(); *(volatile unsigned long long *)(a->gts + 8) = OSSwapHostToBigInt64(ht); }
    /* mRateScalar too, as AUHAL gives it (OAH_NO_AURATESCALAR=1: sample and host time only) */
    {   static int nors = -1; if (nors < 0) nors = getenv("OAH_NO_AURATESCALAR") != 0;
        if (nors) au_put32(a->gts + 56, kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid);
        else { au_putf64(a->gts + 16, 1.0);
               au_put32(a->gts + 56, kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid | kAudioTimeStampRateScalarValid); } }
    if (a->in_on) {                                 /* input callback: (ref, flags, ts, bus 1, frames, NULL) */
        a->cur_in = 1;
        args[0] = a->incbref; args[1] = a->gflags; args[2] = a->gts; args[3] = 1; args[4] = n; args[5] = 0;
        *proc = a->incb; *need_out = 0; return 6;
    }
    a->cur_in = 0;
    if (a->cbproc) {
        args[0] = a->cbref; args[1] = a->gflags; args[2] = a->gts; args[3] = 0; args[4] = n; args[5] = a->gabl;
        *proc = a->cbproc; *need_out = need; return 6;
    }
    /* old SetInputCallback: (ref, flags, ts, bus, AudioBuffer *) */
    args[0] = a->cbref; args[1] = a->gflags; args[2] = a->gts; args[3] = 0; args[4] = a->gabl + 4; args[5] = 0;
    *proc = a->oldcb; *need_out = need; return 5;
}
static void au_take(oah_au *a, unsigned int need) {
    unsigned int n = 512, i, bytes;
    a->pulls++;
    if (a->cur_in) {                                /* the guest took its frames in AudioUnitRender */
        a->icalls++; a->sampletime += 512;
        if (a->iwr - a->ird > AU_RING - 1024) a->ird = a->iwr - 2048;   /* far behind: drop old data */
        return;
    }
    bytes = au_be32(a->gabl + 8);
    if (bytes && bytes < need) n = bytes / ((a->bits / 8) * (a->nonint ? 1 : (a->ch ? a->ch : 2)));
    for (i = 0; i < n; i++) {
        unsigned int k = (a->wr + i) & (AU_RING - 1);
        float l = au_gsample(a, a->gabl, i, 0), r = (a->ch ? a->ch : 2) > 1 ? au_gsample(a, a->gabl, i, 1) : l;
        a->ring[2 * k] = l; a->ring[2 * k + 1] = r;
    }
    __sync_synchronize();
    a->wr += n;
    a->sampletime += n;
    if (oah_autr()) {
        static time_t last; time_t now = time(0);
        if (now - last >= 5) { last = now; fprintf(stderr, "[au] %08x: pulls=%lu renders=%lu underruns=%lu queued=%u\n", a->gci, a->pulls, a->renders, a->underruns, a->wr - a->rd); }
    }
}
static int au_wants(oah_au *a) {
    if (a->in_on) return a->running && a->incb && a->iwr - a->ird >= 512;   /* recorded data waiting */
    return a->running && (a->cbproc || a->oldcb) && a->wr - a->rd < 4096;   /* keep ~90 ms queued */
}
/* Pull guest audio into the ring through the guest-call lock. Called at bridge entries while
   there is no guest audio thread, and to prime a unit off the main thread; never re-entered. */
static volatile int g_au_pumping;
static pthread_t g_au_pump_thr;                    /* holder of g_au_pumping in oah_au_pump */
static void oah_au_pump(void) {
    oah_au *a;
    if (!g_aus) return;
    if (!__sync_bool_compare_and_swap(&g_au_pumping, 0, 1)) return;
    g_au_pump_thr = pthread_self();
    for (a = g_aus; a; a = a->next) {
        int loops = 0;
        while (au_wants(a) && loops++ < 8) {
            unsigned int args[6], proc, need, na = au_prep(a, &need, args, &proc);
            oah_callppc_n("oah_au_render", proc, na, args);
            au_take(a, need);
        }
    }
    g_au_pump_thr = 0;
    __sync_lock_release(&g_au_pumping);
}
/* The guest audio thread runs this guest code (au_start_thread):
       +0   bcl 20,31,.+4      LR = +4
       +4   twi 31,r0,14       br_au_wait: nothing to render -> sleep 5 ms, LR stays +4;
       +8   blr                  a unit needs data -> r3..r8 = arguments, r12 = callback, LR = +12
       +12  mtctr r12
       +16  bctrl              the render callback, as guest code on this guest thread
       +20  twi 31,r0,15       br_au_done: samples into the ring, LR = +4
       +24  blr
   The callback runs without the guest-call lock. Called through the lock (as before), it held
   the lock for the whole render, and QuickTime's render waited for a QuickTime mutex that the
   main thread held while it waited for the lock (any bridge that calls into the guest, e.g.
   GetComponentInfo for a new AudioUnit): Mactracker hung on its Startup Chime.
   OAH_AU_CALLPPC_PUMP=1 calls it through the lock. */
static unsigned int g_au_stub;
static oah_au *volatile g_au_cur;
static pthread_t g_au_thr;                         /* the native thread running the guest audio thread */
static unsigned int g_au_cur_need;
static double g_au_t0;
static double au_now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }
static void oah_au_thread_wait(unsigned int *R) {
    oah_au *a;
    static int viacall = -1;
    if (viacall < 0) viacall = getenv("OAH_AU_CALLPPC_PUMP") ? 1 : 0;
    R[0x90 / 4] = g_au_stub + 4;
    if (viacall) { usleep(5000); oah_au_pump(); return; }
    if (!__sync_bool_compare_and_swap(&g_au_pumping, 0, 1)) { usleep(5000); return; }
    g_au_thr = pthread_self();
    for (a = g_aus; a; a = a->next) if (au_wants(a)) break;
    if (!a) { __sync_lock_release(&g_au_pumping); usleep(5000); return; }
    /* Claim the unit, then look again: a stop between au_wants and the claim found nothing to
       wait for, the guest freed the callback's data (QuickTime closing SimCity 4's intro movie)
       and the render ran on it: SIGBUS at 0x24 in the guest audio thread. */
    g_au_cur = a;
    __sync_synchronize();
    {   static int noclaim = -1; if (noclaim < 0) noclaim = getenv("OAH_NO_AUCLAIM") != 0;
        if (noclaim) goto claimed; }
    if (!a->running) { g_au_cur = 0; __sync_lock_release(&g_au_pumping); usleep(5000); return; }
claimed:
    {   unsigned int args[6], proc, i;
        au_prep(a, &g_au_cur_need, args, &proc);
        for (i = 0; i < 6; i++) R[3 + i] = args[i];
        R[12] = proc;
        R[0x90 / 4] = g_au_stub + 12;              /* g_au_pumping stays taken until br_au_done */
        if (oah_autr()) g_au_t0 = au_now();
        {   static int nlog;
            if (oah_autr() && nlog++ < 5) fprintf(stderr, "[au] render %08x(%08x ...) on the audio thread (r1 %08x)\n", proc, args[0], R[1]); }
    }
}
static void oah_au_thread_done(unsigned int *R) {
    static int nlog;
    if (oah_autr() && nlog++ < 5) fprintf(stderr, "[au] render done (stub %08x, lr %08x, r1 %08x, unit %p)\n", g_au_stub, R[0x90 / 4], R[1], (void *)g_au_cur);
    if (oah_autr() && g_au_t0) { double d = au_now() - g_au_t0; if (d > 0.05) fprintf(stderr, "[au] slow render: %.0f ms (%u bytes)\n", d * 1000, g_au_cur_need); }
    if (g_au_cur) { au_take(g_au_cur, g_au_cur_need); g_au_cur = 0; }
    __sync_lock_release(&g_au_pumping);
    R[0x90 / 4] = g_au_stub + 4;
}
/* A guest thread whose body is "twi 31,r0,14 ; b .-4": bridge 14 (br_au_wait) sleeps 5 ms and
   pumps, so render callbacks keep coming while the app's own threads sleep (as CoreAudio's I/O
   thread called them on 10.6). Started with the guest's pthread_create on the first start. */
static volatile int g_au_thread;
static int g_au_thread_ok(void) { return !getenv("OAH_NO_AUTHREAD"); }
static void au_start_thread(void) {
    static unsigned int *stub;
    unsigned int pc, args[4];
    static unsigned int tid;
    if (g_au_thread || getenv("OAH_NO_AUTHREAD")) return;
    pc = oah_guest_sym("libSystem", "_pthread_create");
    if (!pc) return;
    stub = (unsigned int *)oah_guest_code_alloc(7 * 4);
    if (!stub) return;
    /* see oah_au_thread_wait: after each trap the following blr goes to LR, which the bridges set */
    stub[0] = OSSwapHostToBigInt32(0x429F0005);   /* bcl 20,31,.+4  (LR = stub+4) */
    stub[1] = OSSwapHostToBigInt32(0x0FE0000E);   /* twi 31,r0,14 */
    stub[2] = OSSwapHostToBigInt32(0x4E800020);   /* blr */
    stub[3] = OSSwapHostToBigInt32(0x7D8903A6);   /* mtctr r12 */
    stub[4] = OSSwapHostToBigInt32(0x4E800421);   /* bctrl */
    stub[5] = OSSwapHostToBigInt32(0x0FE0000F);   /* twi 31,r0,15 */
    stub[6] = OSSwapHostToBigInt32(0x4E800020);   /* blr */
    g_au_stub = (unsigned int)stub;
    g_au_thread = 1;
    args[0] = (unsigned int)&tid; args[1] = 0; args[2] = (unsigned int)stub; args[3] = 0;
    {   unsigned int e = oah_callppc_n("oah_au_thread", pc, 4, args);
        if (oah_autr() || e) fprintf(stderr, "[au] guest audio thread -> %u\n", e);
        if (e) g_au_thread = 0; }
}
static void au_gput(oah_au *a, unsigned int p, float v) {
    int be = (a->iflags & kAudioFormatFlagIsBigEndian) != 0;
    if (v > 1) v = 1; else if (v < -1) v = -1;
    if (a->iflags & kAudioFormatFlagIsFloat) {
        unsigned int u; memcpy(&u, &v, 4); *(volatile unsigned int *)p = be ? OSSwapHostToBigInt32(u) : OSSwapHostToLittleInt32(u);
    } else if (a->ibits == 16) {
        unsigned short u = (unsigned short)(short)(v * 32767.f); *(volatile unsigned short *)p = be ? OSSwapHostToBigInt16(u) : OSSwapHostToLittleInt16(u);
    } else if (a->ibits == 32) {
        unsigned int u = (unsigned int)(int)(v * ((a->iflags & 0x1f80) ? 16777216.f : 2147483647.f));
        *(volatile unsigned int *)p = be ? OSSwapHostToBigInt32(u) : OSSwapHostToLittleInt32(u);
    } else if (a->ibits == 8) *(volatile unsigned char *)p = (unsigned char)(128 + (int)(v * 127.f));
}
/* AudioUnitRender(unit, flags, ts, bus, frames, AudioBufferList *) from the guest's input callback:
   recorded frames from iring, in the guest's format (silence for frames not recorded yet) */
static OSStatus au_render_input(oah_au *a, unsigned int n, unsigned int abl) {
    unsigned int nb, b, f, ch, bps = a->ibits / 8, avail = a->iwr - a->ird, take = n < avail ? n : avail;
    if (!abl) return -50;
    nb = au_be32(abl);
    if (nb > 8) nb = 8;
    for (b = 0; b < nb; b++) {
        unsigned int bp = abl + 4 + 12 * b, nch = a->inonint ? 1 : (a->ich ? a->ich : 2), data = au_be32(bp + 8), need = n * nch * bps;
        if (!data) {                                 /* the guest asks the unit for the buffer */
            if (a->igbufsize < need * nb) { free((void *)a->igbuf); a->igbuf = (unsigned int)calloc(1, need * nb); a->igbufsize = need * nb; }
            data = a->igbuf + need * b; au_put32(bp + 8, data);
        }
        au_put32(bp, nch); au_put32(bp + 4, need);
        for (f = 0; f < n; f++) for (ch = 0; ch < nch; ch++) {
            unsigned int src = a->inonint ? b : ch, k = (a->ird + f) & (AU_RING - 1);
            float v = f < take ? a->iring[2 * k + (src ? 1 : 0)] : 0;
            au_gput(a, data + (f * nch + ch) * bps, v);
        }
    }
    __sync_synchronize();
    a->ird += take;
    return 0;
}
/* After a stop, the guest may free the callback's data at once (AudioQueueDispose): wait for a
   callback the guest audio thread is running for this unit (not when we are that thread). */
static void au_wait_idle(oah_au *a) {
    int i;
    __sync_synchronize();
    if (g_au_thread && pthread_equal(pthread_self(), g_au_thr)) return;   /* stopped from its own callback */
    if (g_au_pump_thr && pthread_equal(pthread_self(), g_au_pump_thr)) return;   /* ... or from a pumped one */
    /* g_au_cur == 0 while pumping: the audio thread may be between choosing a unit and claiming it */
    for (i = 0; i < 250 && g_au_pumping && (g_au_cur == a || !g_au_cur); i++) usleep(2000);
}
static void oah_au_closed(unsigned int gci) {
    oah_au *a, **pa;
    pthread_mutex_lock(&g_au_mx);
    a = au_find(gci);
    pthread_mutex_unlock(&g_au_mx);
    if (!a) return;
    if (a->running && a->nau) AudioOutputUnitStop(a->nau);
    a->running = 0;
    au_wait_idle(a);
    pthread_mutex_lock(&g_au_mx);
    for (pa = &g_aus; *pa; pa = &(*pa)->next) if (*pa == a) { *pa = a->next; break; }
    pthread_mutex_unlock(&g_au_mx);
    if (a->nau) { AudioUnitUninitialize(a->nau); AudioComponentInstanceDispose(a->nau); a->nau = 0; }
    if (oah_autr()) fprintf(stderr, "[au] %08x closed\n", gci);
    /* the struct itself is left allocated: the audio thread may still hold a pointer to it */
}
/* kAudioDevicePropertyDeviceIsRunning ('goin') asked of the output unit (10.6's AUHAL passes device
   properties through; QuickTime asks it when a movie stops): answered from the unit's own running
   state rather than the device's (other device properties go to the native device, see
   AudioUnitGetProperty below). OAH_NO_AUGOIN=1: passed to the device too. */
static int au_no_goin(void) { static int v = -1; if (v < 0) v = getenv("OAH_NO_AUGOIN") != 0; return v; }
static int oah_au_unit_call(const char *n, unsigned int *R) {
    oah_au *a;
    static int off = -1;
    if (off < 0) off = getenv("OAH_NO_AUBRIDGE") ? 1 : 0;
    if (n[0] != 'A' || strncmp(n, "Audio", 5) || (strncmp(n + 5, "Unit", 4) && strncmp(n + 5, "OutputUnit", 10))) return 0;
    a = off ? 0 : au_get(R[3]);
    /* AudioUnitRender is ours only for the input element of a recording unit; mixers, converters
       and the rest (OpenAL, QuickTime graphs) render in their own guest code */
    if (a && !strcmp(n, "AudioUnitRender") && !(a->in_on && R[6] == 1)) a = 0;
    if (!a) {                                        /* not an output unit: the original guest code */
        static const char *names[16]; static unsigned int tr[16]; static int nt;
        unsigned int t = 0, args[8]; int i;
        for (i = 0; i < nt; i++) if (!strcmp(names[i], n)) { t = tr[i]; break; }
        if (i == nt && nt < 16) {
            char sym[64]; snprintf(sym, sizeof sym, "_%s", n);
            t = oah_guest_orig("AudioUnit", sym); names[nt] = n; tr[nt] = t; nt++;
        }
        if (!t) { R[3] = (unsigned int)kAudioUnitErr_InvalidProperty; return 1; }
        {   static int tr2 = -1; if (tr2 < 0) tr2 = getenv("OAH_AUTRACE2") != 0;
            if (tr2 && strcmp(n, "AudioUnitRender"))
                fprintf(stderr, "[au2] %s(%08x, %u, %u, %u) thread %p\n", n, R[3], R[4], R[5], R[6], (void *)pthread_self()); }
        /* Continue in the original guest code on this guest thread instead of calling it through
           the guest-call lock: the bridge returns with blr to LR, so LR is pointed at a stub that
           restores the caller's LR (passed in r0, volatile at a call) and jumps to the trampoline.
           Calling it natively took the lock, and QuickTime's render callback on the guest audio
           thread (holding the lock) waited for a QuickTime mutex this thread held:
           AudioUnitUninitialize of QuickTime's own units hung Mactracker's Startup Chime.
           OAH_AU_ORIG_CALLPPC=1 calls through the lock as before. */
        if (!getenv("OAH_AU_ORIG_CALLPPC")) {
            static unsigned int jt[16], js[16]; static int nj;
            unsigned int j = 0;
            pthread_mutex_lock(&g_au_mx);
            for (i = 0; i < nj; i++) if (jt[i] == t) { j = js[i]; break; }
            if (!j && nj < 16 && (j = oah_guest_code_alloc(5 * 4))) {
                unsigned int *s = (unsigned int *)j;
                s[0] = OSSwapHostToBigInt32(0x7c0803a6);                    /* mtlr r0 */
                s[1] = OSSwapHostToBigInt32(0x3d800000 | (t >> 16));        /* lis r12,hi */
                s[2] = OSSwapHostToBigInt32(0x618c0000 | (t & 0xffff));     /* ori r12,r12,lo */
                s[3] = OSSwapHostToBigInt32(0x7d8903a6);                    /* mtctr r12 */
                s[4] = OSSwapHostToBigInt32(0x4e800420);                    /* bctr */
                jt[nj] = t; js[nj] = j; nj++;
            }
            pthread_mutex_unlock(&g_au_mx);
            if (j) { R[0] = R[0x90 / 4]; R[0x90 / 4] = j; return 1; }
        }
        for (i = 0; i < 8; i++) args[i] = R[3 + i];
        if (!strcmp(n, "AudioUnitSetParameter")) {   /* (unit, id, scope, elem, Float32 value in f1, offset) */
            double *F = (double *)((unsigned char *)R + 0x1e8);
            (void)F;                                 /* f1 is left as is for the original code */
        }
        R[3] = oah_callppc_n("oah_au_orig", t, 8, args);
        return 1;
    }
    if (!strcmp(n, "AudioUnitSetProperty")) {        /* (unit, prop, scope, elem, const void *data, UInt32 size) */
        unsigned int prop = R[4], scope = R[5], data = R[7], size = R[8];
        OSStatus e = 0;
        if (prop == kAudioOutputUnitProperty_EnableIO && data) {
            unsigned int v = au_be32(data);
            if (scope == kAudioUnitScope_Input && R[6] == 1) a->in_on = v != 0;
            else if (scope == kAudioUnitScope_Output && R[6] == 0) a->out_off = v == 0;
        } else if (prop == kAudioOutputUnitProperty_SetInputCallback && data) {
            a->incb = au_be32(data); a->incbref = au_be32(data + 4);
        } else if (prop == kAudioOutputUnitProperty_CurrentDevice) {
            /* the default devices are used */
        } else if (prop == kAudioUnitProperty_StreamFormat && data && size >= 40 && scope == kAudioUnitScope_Output && R[6] == 1) {
            a->irate = au_bef64(data); a->iflags = au_be32(data + 12); a->ich = au_be32(data + 28); a->ibits = au_be32(data + 32);
            a->inonint = (a->iflags & kAudioFormatFlagIsNonInterleaved) != 0;
            if (a->ibits != 8 && a->ibits != 16 && a->ibits != 32) a->ibits = 32;
        } else if (prop == kAudioUnitProperty_StreamFormat && data && size >= 40) {
            if (scope == kAudioUnitScope_Input) {
                a->rate = au_bef64(data); a->fmtflags = au_be32(data + 12); a->bpf = au_be32(data + 24);
                a->ch = au_be32(data + 28); a->bits = au_be32(data + 32);
                a->nonint = (a->fmtflags & kAudioFormatFlagIsNonInterleaved) != 0;
                if (a->bits != 8 && a->bits != 16 && a->bits != 32) a->bits = 32;
                if (a->running) au_native_setup(a);
            }
        } else if (prop == kAudioUnitProperty_SetRenderCallback && data) {
            a->cbproc = au_be32(data); a->cbref = au_be32(data + 4); a->oldcb = 0;
        } else if (prop == 7 /* SetInputCallback (old API) */ && data) {
            a->oldcb = au_be32(data); a->cbref = au_be32(data + 4); a->cbproc = 0;
        } else if (prop == kAudioUnitProperty_MaximumFramesPerSlice && data) {
            a->maxframes = au_be32(data);
        }
        if (oah_autr()) fprintf(stderr, "[au] SetProperty(%08x, %u, scope %u) -> %d%s\n", a->gci, prop, scope, (int)e,
                                prop == kAudioUnitProperty_StreamFormat ? " (format)" : prop == 23 || prop == 7 ? " (callback)" : "");
        R[3] = (unsigned int)e;
        return 1;
    }
    if (!strcmp(n, "AudioUnitGetProperty") || !strcmp(n, "AudioUnitGetPropertyInfo")) {
        /* Get: (unit, prop, scope, elem, void *data, UInt32 *ioSize); Info: (unit, prop, scope, elem, UInt32 *size, Boolean *wr) */
        unsigned int prop = R[4], info = n[20] == 'I', size = 0;
        OSStatus e = 0;
        if (prop == kAudioUnitProperty_StreamFormat) size = 40;
        else if (prop == kAudioOutputUnitProperty_CurrentDevice || prop == kAudioUnitProperty_MaximumFramesPerSlice ||
                 prop == kAudioOutputUnitProperty_IsRunning || prop == kAudioOutputUnitProperty_EnableIO ||
                 prop == kAudioOutputUnitProperty_HasIO || (prop == 'goin' && !au_no_goin())) size = 4;
        else if (prop == kAudioUnitProperty_SampleRate || prop == kAudioUnitProperty_Latency) size = 8;
        else e = kAudioUnitErr_InvalidProperty;
        /* AudioDevice properties (four-character codes: 'fsiz', 'ltnc', 'saft', 'vfsz', 'srnd'...): 10.6's
           AUHAL answers them from its device. QuickTime sizes its renders from 'fsiz'; with the error we
           gave, its AudioContextProcessor rendered 0 frames at a time, forever: movies had no sound, the
           render never returned, and it ran into the movie's units while DisposeMovie uninitialized them
           (SIGBUS at 0x24 in the guest matrix mixer: SimCity 4's intro, tests/periph/qt_play). Answered by
           the native default device. OAH_NO_AUDEVPROPS=1: the error, as before. */
        if (e && prop >= 0x20202020 && !getenv("OAH_NO_AUDEVPROPS")) {
            AudioDeviceID dev = 0; UInt32 dsz = sizeof dev; unsigned char buf[1024]; UInt32 sz = sizeof buf; Boolean wr = 0; OSStatus de;
            AudioHardwareGetProperty(a->in_on ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice, &dsz, &dev);
            if (info) {
                de = AudioDeviceGetPropertyInfo(dev, 0, a->in_on ? 1 : 0, prop, &sz, &wr);
                if (!de) { if (R[7]) au_put32(R[7], sz); if (R[8]) *(volatile unsigned char *)R[8] = wr; }
            } else {
                UInt32 want = R[8] ? au_be32(R[8]) : 0;
                if (want < sz) sz = want;
                de = AudioDeviceGetProperty(dev, 0, a->in_on ? 1 : 0, prop, &sz, buf);
                if (!de) { au_swap_out(prop, R[7], buf, sz); if (R[8]) au_put32(R[8], sz); }
            }
            if (oah_autr()) fprintf(stderr, "[au] %s(%08x, '%.4s') from device %u -> %d (size %u)\n", n, a->gci,
                                    (char *)&(unsigned int){ OSSwapHostToBigInt32(prop) }, (unsigned)dev, (int)de, (unsigned)sz);
            R[3] = (unsigned int)de;
            return 1;
        }
        if (info) {
            if (!e && R[7]) au_put32(R[7], size);
            if (!e && R[8]) *(volatile unsigned char *)R[8] = prop != kAudioOutputUnitProperty_IsRunning && prop != 'goin';
        } else if (!e && R[7]) {
            unsigned int d = R[7];
            if (prop == kAudioUnitProperty_StreamFormat && R[6] == 1) {   /* the input element */
                unsigned int bpf = (a->ibits / 8) * (a->inonint ? 1 : a->ich);
                au_putf64(d, a->irate); au_put32(d + 8, kAudioFormatLinearPCM); au_put32(d + 12, a->iflags);
                au_put32(d + 16, bpf); au_put32(d + 20, 1); au_put32(d + 24, bpf);
                au_put32(d + 28, a->ich); au_put32(d + 32, a->ibits); au_put32(d + 36, 0);
            } else if (prop == kAudioUnitProperty_StreamFormat) {
                au_putf64(d, a->rate); au_put32(d + 8, kAudioFormatLinearPCM); au_put32(d + 12, a->fmtflags);
                au_put32(d + 16, a->bpf); au_put32(d + 20, 1); au_put32(d + 24, a->bpf);
                au_put32(d + 28, a->ch); au_put32(d + 32, a->bits); au_put32(d + 36, 0);
            } else if (prop == kAudioOutputUnitProperty_CurrentDevice) {
                AudioDeviceID dev = 0; UInt32 sz = sizeof dev;
                AudioHardwareGetProperty(a->in_on ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice, &sz, &dev);
                au_put32(d, dev);
            } else if (prop == kAudioUnitProperty_MaximumFramesPerSlice) au_put32(d, a->maxframes);
            else if (prop == kAudioOutputUnitProperty_IsRunning || prop == 'goin') au_put32(d, a->running);
            else if (prop == kAudioOutputUnitProperty_EnableIO) au_put32(d, R[5] == kAudioUnitScope_Input ? (unsigned)a->in_on : (unsigned)!a->out_off);
            else if (prop == kAudioOutputUnitProperty_HasIO) au_put32(d, 1);
            else if (prop == kAudioUnitProperty_SampleRate) au_putf64(d, a->rate);
            else if (prop == kAudioUnitProperty_Latency) au_putf64(d, 4096.0 / (a->rate > 1000 ? a->rate : 44100));
            if (R[8]) au_put32(R[8], size);
        }
        if (oah_autr() && e) fprintf(stderr, "[au] %s(%08x, %u) not handled\n", n, a->gci, prop);
        R[3] = (unsigned int)e;
        return 1;
    }
    if (!strcmp(n, "AudioUnitInitialize")) { if (!a->initialized && a->nau) { au_native_setup(a); AudioUnitInitialize(a->nau); } a->initialized = 1; R[3] = 0; return 1; }
    if (!strcmp(n, "AudioUnitUninitialize")) {
        if (a->running && a->nau) AudioOutputUnitStop(a->nau);
        a->running = 0;
        au_wait_idle(a);
        if (a->initialized && a->nau) AudioUnitUninitialize(a->nau);
        a->initialized = 0; R[3] = 0; return 1;
    }
    if (!strcmp(n, "AudioOutputUnitStart")) {
        OSStatus e = 0;
        if (!a->initialized && a->nau) { au_native_setup(a); AudioUnitInitialize(a->nau); a->initialized = 1; }
        a->rd = a->wr = 0; a->ird = a->iwr = 0;
        a->running = 1;
        /* prime before the device asks - not on the main thread: QuickTime's render callback
           (AudioContextProcessor, Mactracker's Startup Chime) waits for data that the main
           thread's MoviesTask provides, and spun forever inside AudioOutputUnitStart. The guest
           audio thread pumps within 5 ms anyway. OAH_AU_MAINPRIME=1 primes there too. */
        if (!pthread_main_np() || !g_au_thread_ok() || getenv("OAH_AU_MAINPRIME")) oah_au_pump();
        au_start_thread();
        if (a->nau) e = AudioOutputUnitStart(a->nau);
        if (oah_autr()) fprintf(stderr, "[au] %08x started (%.0f Hz, %u ch, %u bits, flags %x) -> %d\n", a->gci, a->rate, a->ch, a->bits, a->fmtflags, (int)e);
        R[3] = (unsigned int)e; return 1;
    }
    if (!strcmp(n, "AudioOutputUnitStop")) {
        if (a->running && a->nau) AudioOutputUnitStop(a->nau);
        a->running = 0;
        au_wait_idle(a);
        if (oah_autr()) fprintf(stderr, "[au] %08x stopped (input calls %lu, overruns %lu)\n", a->gci, a->icalls, a->ioverruns);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "AudioUnitSetParameter")) {      /* (unit, id, scope, elem, Float32 value (f1), offset) */
        double *F = (double *)((unsigned char *)R + 0x1e8);
        if (R[4] == kHALOutputParam_Volume) a->vol = (float)F[1];
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "AudioUnitGetParameter")) {      /* (unit, id, scope, elem, Float32 *value) */
        if (R[7]) { float v = R[4] == kHALOutputParam_Volume ? a->vol : 0; unsigned int u; memcpy(&u, &v, 4); au_put32(R[7], u); }
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "AudioUnitReset")) { a->rd = a->wr; a->ird = a->iwr; R[3] = 0; return 1; }
    if (!strcmp(n, "AudioUnitRender")) {            /* (unit, flags, ts, bus, frames, AudioBufferList *) */
        R[3] = (unsigned int)au_render_input(a, R[7], R[8]);
        return 1;
    }
    R[3] = 0;
    return 1;
}
