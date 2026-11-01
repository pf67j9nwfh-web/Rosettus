/* oah_cam.h - the Mac's camera for PowerPC apps: a QuickTime video digitizer ('vdig') served by
   native AVFoundation capture.

   10.6's PowerPC QuickTime has digitizers for USB video-class and FireWire IIDC cameras only, and
   both talk to the device through IOKit user clients, which Mojave refuses to a PowerPC process
   (IOServiceOpen -> kIOReturnUnsupported). The built-in FaceTime HD camera is not a USB video-class
   device at all. So the Sequence Grabber (iChat-era video apps, QuickTime Broadcaster, Photo Booth
   style apps, the tests/periph/webcam test) found no video device: SGNewChannel(video) -> -9405.

   When an app starts QuickTime (EnterMovies) and the Mac has a camera, liboah registers a 'vdig'
   component whose routine is a two-word guest stub trapping into oah_fix_cam_vdig. The digitizer
   is a "compress only" one, like Apple's USB digitizer: the Sequence Grabber asks for frames with
   VDCompressOneFrameAsync / VDCompressDone and gets them as 'raw ' 32-bit ARGB data of the size it
   set with VDSetCompression; it decompresses them itself for preview and recording. Frames come
   from an AVCaptureSession (AVCaptureVideoDataOutput, BGRA) started on the first request and
   stopped when the component closes; they are scaled (nearest pixel) to the requested size.

   Component parameters (ComponentParameters, big-endian): the arguments packed in reverse order
   after the 4-byte header, shorts and Booleans in 2 bytes, then the instance. macOS asks for
   camera permission the first time (for the app that started Rosettus' process).
   OAH_NO_CAMERA=1 leaves the digitizer out; OAH_CAMLOG=1 logs the calls; OAH_CAMERA_ASK=1 opens the
   camera even when the app's own Info.plist has no NSCameraUsageDescription (an app bundle that
   starts rosettus and has one). */
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>

typedef void *oah_id;
/* objc_msgSend through pointer variables (a call through a cast of the function itself is
   compiled to a trap by this gcc) */
static oah_id (*g_cam_msg)(oah_id, SEL, ...) = (oah_id (*)(oah_id, SEL, ...))(void *)objc_msgSend;
static signed char (*g_cam_msgb)(oah_id, SEL, ...) = (signed char (*)(oah_id, SEL, ...))(void *)objc_msgSend;
static long (*g_cam_msgl)(oah_id, SEL, ...) = (long (*)(oah_id, SEL, ...))(void *)objc_msgSend;
#define CAM_MSG g_cam_msg
#define CAM_MSGB g_cam_msgb
#define CAM_MSGL g_cam_msgl
#define CAM_SEL(s) sel_registerName(s)
#define CAM_AVF "/System/Library/Frameworks/AVFoundation.framework/AVFoundation"
#define CAM_CMF "/System/Library/Frameworks/CoreMedia.framework/CoreMedia"
#define CAM_CVF "/System/Library/Frameworks/CoreVideo.framework/CoreVideo"

static unsigned int g_cam_stub;
static pthread_mutex_t g_cam_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_cam_raw;            /* latest camera frame, BGRA */
static int g_cam_rw, g_cam_rh, g_cam_rrb;
static unsigned int g_cam_seq;              /* frames received */
static oah_id g_cam_session;
static int g_cam_running, g_cam_failed;
/* the digitizer's state (one instance at a time, as with a real camera) */
static struct {
    int open, comp_on, want, w, h, depth;
    unsigned int timebase, last_seq, nbuf, frames;
    long fps_fixed;
    short dig[4];                           /* digitizer rect: top, left, bottom, right */
    unsigned char *buf[8];
    double t0;
    char name[64];
} g_vd;

static int oah_cam_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_CAMLOG") ? 1 : 0; return v; }
static void *oah_cam_sym(const char *lib, const char *s) { void *h = dlopen(lib, RTLD_LAZY | RTLD_GLOBAL); return h ? dlsym(h, s) : 0; }
static double oah_cam_now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }

/* -[RosettusCameraDelegate captureOutput:didOutputSampleBuffer:fromConnection:] on the capture queue */
static void oah_cam_frame(oah_id self, SEL cmd, oah_id out, void *sb, oah_id conn) {
    static void *(*getib)(void *);
    static int (*lock)(void *, unsigned long long), (*unlock)(void *, unsigned long long);
    static size_t (*gw)(void *), (*gh)(void *), (*grb)(void *);
    static void *(*gbase)(void *);
    void *pb; unsigned char *base; size_t w, h, rb, y;
    (void)self; (void)cmd; (void)out; (void)conn;
    if (!getib) {
        getib = (void *(*)(void *))oah_cam_sym(CAM_CMF, "CMSampleBufferGetImageBuffer");
        lock = (int (*)(void *, unsigned long long))oah_cam_sym(CAM_CVF, "CVPixelBufferLockBaseAddress");
        unlock = (int (*)(void *, unsigned long long))oah_cam_sym(CAM_CVF, "CVPixelBufferUnlockBaseAddress");
        gw = (size_t (*)(void *))oah_cam_sym(CAM_CVF, "CVPixelBufferGetWidth");
        gh = (size_t (*)(void *))oah_cam_sym(CAM_CVF, "CVPixelBufferGetHeight");
        grb = (size_t (*)(void *))oah_cam_sym(CAM_CVF, "CVPixelBufferGetBytesPerRow");
        gbase = (void *(*)(void *))oah_cam_sym(CAM_CVF, "CVPixelBufferGetBaseAddress");
    }
    if (!getib || !lock || !gbase || !(pb = getib(sb))) return;
    if (lock(pb, 1 /* read only */)) return;
    w = gw(pb); h = gh(pb); rb = grb(pb); base = (unsigned char *)gbase(pb);
    if (base && w && h && w <= 8192 && h <= 8192) {
        pthread_mutex_lock(&g_cam_mu);
        if ((int)w != g_cam_rw || (int)h != g_cam_rh) {
            free(g_cam_raw); g_cam_raw = (unsigned char *)malloc(w * h * 4);
            g_cam_rw = (int)w; g_cam_rh = (int)h; g_cam_rrb = (int)(w * 4);
        }
        if (g_cam_raw) { for (y = 0; y < h; y++) memcpy(g_cam_raw + y * w * 4, base + y * rb, w * 4); g_cam_seq++; }
        pthread_mutex_unlock(&g_cam_mu);
    }
    unlock(pb, 1);
}

static oah_id oah_cam_device(void) {
    oah_id cls, *mt;
    if (!oah_cam_sym(CAM_AVF, "AVMediaTypeVideo")) return 0;
    cls = (oah_id)objc_getClass("AVCaptureDevice");
    mt = (oah_id *)oah_cam_sym(CAM_AVF, "AVMediaTypeVideo");
    if (!cls || !mt || !*mt) return 0;
    return CAM_MSG(cls, CAM_SEL("defaultDeviceWithMediaType:"), *mt);
}

/* Does the app macOS holds responsible for this process (the Rosettus launcher whose translate
   runs it, or a patched app) explain its use of the camera? CFBundleGetMainBundle names the guest
   app here (CFProcessPath), so look at the bundle around the real executable too. */
static int oah_cam_app_says_why(void) {
    char p[PATH_MAX], *m; uint32_t n = sizeof p; int r = 0;
    if (CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("NSCameraUsageDescription"))) return 1;
    if (!_NSGetExecutablePath(p, &n) && (m = strstr(p, ".app/Contents/MacOS/"))) {
        CFURLRef u; CFBundleRef b;
        m[4] = 0;
        u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)p, strlen(p), true);
        b = u ? CFBundleCreate(NULL, u) : NULL;
        if (b && CFBundleGetValueForInfoDictionaryKey(b, CFSTR("NSCameraUsageDescription"))) r = 1;
        if (b) CFRelease(b);
        if (u) CFRelease(u);
        if (oah_cam_log()) fprintf(stderr, "[cam] app bundle %s: %s\n", p, r ? "explains the camera" : "no NSCameraUsageDescription");
    }
    return r;
}
static int oah_cam_start(void) {
    oah_id pool, dev, in, out, sess, del, err = 0, *preset;
    static Class dcls;
    CFMutableDictionaryRef set;
    CFStringRef *kfmt;
    int fmt = 'BGRA';
    if (g_cam_running) return 0;
    if (g_cam_failed) return -1;
    pool = CAM_MSG(CAM_MSG((oah_id)objc_getClass("NSAutoreleasePool"), CAM_SEL("alloc")), CAM_SEL("init"));
    dev = oah_cam_device();
    if (!dev) goto fail;
    {   /* macOS 10.14 ends a process that opens the camera before the user was asked, when the app
           responsible for it does not say why it wants the camera (NSCameraUsageDescription): the
           Rosettus launchers say it; rosettus run from Terminal or ssh does not */
        oah_id *mt = (oah_id *)oah_cam_sym(CAM_AVF, "AVMediaTypeVideo");
        long st = 3;
        if (mt && class_getClassMethod((Class)objc_getClass("AVCaptureDevice"), CAM_SEL("authorizationStatusForMediaType:")))
            st = CAM_MSGL((oah_id)objc_getClass("AVCaptureDevice"), CAM_SEL("authorizationStatusForMediaType:"), *mt);
        if (oah_cam_log()) fprintf(stderr, "[cam] device %p, camera permission status %ld (0 not asked yet, 2 denied, 3 allowed)\n", dev, st);
        if (st == 1 || st == 2) { fprintf(stderr, "[cam] camera access is not allowed for this app (System Preferences > Security & Privacy > Camera)\n"); goto fail; }
        if (st == 0 && !getenv("OAH_CAMERA_ASK") && !oah_cam_app_says_why()) {
            fprintf(stderr, "[cam] the camera needs the app's permission, which macOS asks only for an app that explains its use: start it from its Rosettus launcher\n");
            goto fail;
        }
    }
    in = CAM_MSG((oah_id)objc_getClass("AVCaptureDeviceInput"), CAM_SEL("deviceInputWithDevice:error:"), dev, &err);
    if (!in) { fprintf(stderr, "[cam] the camera cannot be opened (no permission, or in use)\n"); goto fail; }
    sess = CAM_MSG(CAM_MSG((oah_id)objc_getClass("AVCaptureSession"), CAM_SEL("alloc")), CAM_SEL("init"));
    preset = (oah_id *)oah_cam_sym(CAM_AVF, "AVCaptureSessionPreset640x480");
    if (preset && *preset && CAM_MSGB(sess, CAM_SEL("canSetSessionPreset:"), *preset)) CAM_MSG(sess, CAM_SEL("setSessionPreset:"), *preset);
    if (!CAM_MSGB(sess, CAM_SEL("canAddInput:"), in)) goto fail;
    CAM_MSG(sess, CAM_SEL("addInput:"), in);
    out = CAM_MSG(CAM_MSG((oah_id)objc_getClass("AVCaptureVideoDataOutput"), CAM_SEL("alloc")), CAM_SEL("init"));
    kfmt = (CFStringRef *)oah_cam_sym(CAM_CVF, "kCVPixelBufferPixelFormatTypeKey");
    set = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (kfmt && *kfmt) { CFNumberRef n = CFNumberCreate(0, kCFNumberSInt32Type, &fmt); CFDictionarySetValue(set, *kfmt, n); CFRelease(n); }
    CAM_MSG(out, CAM_SEL("setVideoSettings:"), set);
    CFRelease(set);
    CAM_MSG(out, CAM_SEL("setAlwaysDiscardsLateVideoFrames:"), (int)1);
    if (!dcls) {
        dcls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "RosettusCameraDelegate", 0);
        if (dcls) {
            class_addMethod(dcls, CAM_SEL("captureOutput:didOutputSampleBuffer:fromConnection:"), (IMP)oah_cam_frame, "v@:@^v@");
            objc_registerClassPair(dcls);
        } else dcls = (Class)objc_getClass("RosettusCameraDelegate");
    }
    del = CAM_MSG(CAM_MSG((oah_id)dcls, CAM_SEL("alloc")), CAM_SEL("init"));
    CAM_MSG(out, CAM_SEL("setSampleBufferDelegate:queue:"), del, dispatch_queue_create("com.rosettus.camera", 0));
    if (!CAM_MSGB(sess, CAM_SEL("canAddOutput:"), out)) goto fail;
    CAM_MSG(sess, CAM_SEL("addOutput:"), out);
    CAM_MSG(sess, CAM_SEL("startRunning"));
    g_cam_session = sess; g_cam_running = 1;
    if (oah_cam_log()) fprintf(stderr, "[cam] capture session started\n");
    CAM_MSG(pool, CAM_SEL("drain"));
    return 0;
fail:
    g_cam_failed = 1;
    CAM_MSG(pool, CAM_SEL("drain"));
    return -1;
}
static void oah_cam_stop(void) {
    if (!g_cam_running) return;
    CAM_MSG(g_cam_session, CAM_SEL("stopRunning"));
    g_cam_running = 0;
    if (oah_cam_log()) fprintf(stderr, "[cam] capture session stopped (%u frames received)\n", g_cam_seq);
}

/* argument k (0 = first after the instance) of a call with n arguments of the given sizes */
static unsigned int oah_vd_arg(unsigned int P, const unsigned char *sz, int n, int k) {
    unsigned int off = 4; int i;
    const unsigned char *p;
    for (i = n - 1; i > k; i--) off += sz[i];
    p = (const unsigned char *)(uintptr_t)(P + off);
    if (sz[k] == 2) return (unsigned int)((p[0] << 8) | p[1]);
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}
static void oah_vd_put16(unsigned int a, unsigned int v) { if (a && oah_readable(a, 2)) { unsigned char *p = (unsigned char *)(uintptr_t)a; p[0] = v >> 8; p[1] = v; } }
static void oah_vd_put32(unsigned int a, unsigned int v) { if (a && oah_readable(a, 4)) *(unsigned int *)(uintptr_t)a = OSSwapHostToBigInt32(v); }
static void oah_vd_rect(unsigned int a, int t, int l, int b, int r) { oah_vd_put16(a, t); oah_vd_put16(a + 2, l); oah_vd_put16(a + 4, b); oah_vd_put16(a + 6, r); }
static void oah_vd_pstr(unsigned int a, const char *s, int max) {
    int n = (int)strlen(s); if (n > max) n = max;
    if (!a || !oah_readable(a, n + 1)) return;
    *(unsigned char *)(uintptr_t)a = (unsigned char)n; memcpy((char *)(uintptr_t)a + 1, s, n);
}
/* resize a guest handle and return its (big-endian stored) master pointer's target */
static unsigned int oah_vd_handle(unsigned int h, unsigned int size) {
    static unsigned int shs;
    unsigned int a[2], mp;
    if (!h || !oah_readable(h, 4)) return 0;
    if (!shs) shs = oah_guest_sym("CarbonCore", "_SetHandleSize");
    if (shs) { a[0] = h; a[1] = size; oah_callppc_n("SetHandleSize", shs, 2, a); }
    mp = OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)h);
    return (mp && oah_readable(mp, size)) ? mp : 0;
}

/* one frame, scaled to w x h, BGRA -> ARGB; 0 when no new frame */
static unsigned char *oah_vd_take(void) {
    unsigned char *d; int x, y, w = g_vd.w, h = g_vd.h;
    pthread_mutex_lock(&g_cam_mu);
    if (!g_cam_raw || g_cam_seq == g_vd.last_seq) { pthread_mutex_unlock(&g_cam_mu); return 0; }
    d = g_vd.buf[g_vd.nbuf++ & 7];
    if (!d) d = g_vd.buf[(g_vd.nbuf - 1) & 7] = (unsigned char *)malloc((size_t)w * h * 4);
    if (d) for (y = 0; y < h; y++) {
        const unsigned char *s = g_cam_raw + (size_t)(y * g_cam_rh / h) * g_cam_rrb;
        unsigned char *o = d + (size_t)y * w * 4;
        for (x = 0; x < w; x++) { const unsigned char *px = s + (x * g_cam_rw / w) * 4; o[0] = 0xff; o[1] = px[2]; o[2] = px[1]; o[3] = px[0]; o += 4; }
    }
    g_vd.last_seq = g_cam_seq;
    pthread_mutex_unlock(&g_cam_mu);
    return d;
}
static void oah_vd_setsize(int w, int h) {
    int i;
    if (w < 16 || h < 16 || w > 4096 || h > 4096) return;
    if (w == g_vd.w && h == g_vd.h) return;
    for (i = 0; i < 8; i++) { free(g_vd.buf[i]); g_vd.buf[i] = 0; }
    g_vd.w = w; g_vd.h = h;
}

#define VD_UNIMP 0xfffff767u                /* digiUnimpErr (-2201) */
#define VD_BADSEL 0x80008002u               /* badComponentSelector */
static void oah_fix_cam_vdig(unsigned int *R) {
    unsigned int P = R[3], r = 0;
    short what; int psize;
    static const unsigned char s4[] = { 4, 4, 4, 4, 4, 4, 4, 4 };
    g_gt_skip = 1; g_gt_skip_ret = VD_BADSEL;
    if (!P || !oah_readable(P, 4)) return;
    psize = ((unsigned char *)(uintptr_t)P)[1];
    what = (short)((((unsigned char *)(uintptr_t)P)[2] << 8) | ((unsigned char *)(uintptr_t)P)[3]);
    if (!oah_readable(P, 4 + psize)) return;
#define A4(n, k) oah_vd_arg(P, s4, n, k)
    switch (what) {
    case -1: /* open */
        g_vd.open++; g_vd.t0 = oah_cam_now(); if (!g_vd.w) oah_vd_setsize(640, 480);
        if (!g_vd.dig[3]) { g_vd.dig[2] = 480; g_vd.dig[3] = 640; }
        break;
    case -2: /* close */
        if (g_vd.open > 0 && !--g_vd.open) { oah_cam_stop(); g_vd.want = 0; }
        break;
    case -3: { /* can do */
        static const short ok[] = { -1, -2, -3, -4, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 25, 26, 59, 60, 61, 62, 63,
                                    64, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 81, 82, 85, 90, 91, 92, 94, 95, 0 };
        short s = (short)oah_vd_arg(P, (const unsigned char *)"\2", 1, 0); int i;
        for (i = 0, r = 0; ok[i]; i++) if (ok[i] == s) r = 1;
        break; }
    case -4: r = 0x00010001; break;                                   /* version */
    case 1: case 2: { static const unsigned char sz[] = { 2, 4 };     /* VDGetMaxSrcRect / VDGetActiveSrcRect (short std, Rect *) */
        oah_vd_rect(oah_vd_arg(P, sz, 2, 1), 0, 0, 480, 640); break; }
    case 3: { unsigned int a = A4(1, 0);                              /* VDSetDigitizerRect (Rect *) */
        if (a && oah_readable(a, 8)) { int k; for (k = 0; k < 4; k++) g_vd.dig[k] = (short)((((unsigned char *)(uintptr_t)a)[2 * k] << 8) | ((unsigned char *)(uintptr_t)a)[2 * k + 1]); }
        break; }
    case 4: oah_vd_rect(A4(1, 0), g_vd.dig[0], g_vd.dig[1], g_vd.dig[2], g_vd.dig[3]); break;   /* VDGetDigitizerRect */
    case 5: { static const unsigned char sz[] = { 2, 4 }; oah_vd_rect(oah_vd_arg(P, sz, 2, 1), 0, 0, 0, 0); break; }   /* VDGetVBlankRect */
    case 12: case 14: case 15: case 16: case 17: break;               /* VDSetBrightness... (unsigned short *): accepted */
    case 13: case 18: case 19: case 20: case 21: oah_vd_put16(A4(1, 0), 32768); break;
    case 25: { unsigned int a = A4(1, 0);                             /* VDGetDigitizerInfo (DigitizerInfo *, mac68k alignment) */
        if (a && oah_readable(a, 42)) {
            memset((void *)(uintptr_t)a, 0, 42);
            oah_vd_put16(a, 0);                                       /* vdTypeBasic */
            oah_vd_put32(a + 2, 0x1000 | 0x1);                        /* digiInDoesColor | digiInDoesNTSC */
            oah_vd_put32(a + 6, 0x80000000u | 0x10000000 | 0x8000000 | 0x20);   /* doesn't need a copy, compress only, compress, 32-bit */
            oah_vd_put32(a + 10, 0x1000 | 0x1);
            oah_vd_put32(a + 14, 0x80000000u | 0x10000000 | 0x8000000 | 0x20);
            oah_vd_put16(a + 28, 16); oah_vd_put16(a + 30, 16); oah_vd_put16(a + 32, 1080); oah_vd_put16(a + 34, 1920);
        }
        break; }
    case 26: oah_vd_put32(A4(2, 0), 0x1001); oah_vd_put32(A4(2, 1), 0x80000000u | 0x10000000 | 0x8000000 | 0x20); break;   /* VDGetCurrentFlags */
    case 59: { int k; for (k = 0; k < 7; k++) oah_vd_put16(A4(7, k), 32768); break; }      /* VDGetVideoDefaults */
    case 60: oah_vd_put16(A4(1, 0), 0); break;                        /* VDGetNumberOfInputs: one (inputs - 1) */
    case 61: { static const unsigned char sz[] = { 2, 4 }; oah_vd_put16(oah_vd_arg(P, sz, 2, 1), 0); break; }   /* VDGetInputFormat: compositeIn */
    case 62: case 64: break;                                          /* VDSetInput, VDSetInputStandard */
    case 63: oah_vd_put16(A4(1, 0), 0); break;                        /* VDGetInput */
    case 68: { static const unsigned char sz[] = { 4, 2, 4, 4, 4, 4 };   /* VDSetCompression (type, depth, Rect *bounds, spatialQ, temporalQ, keyFrameRate) */
        unsigned int type = oah_vd_arg(P, sz, 6, 0), b = oah_vd_arg(P, sz, 6, 2);
        g_vd.depth = (int)oah_vd_arg(P, sz, 6, 1);
        if (b && oah_readable(b, 8)) {
            const unsigned char *q = (const unsigned char *)(uintptr_t)b;
            int t = (short)((q[0] << 8) | q[1]), l = (short)((q[2] << 8) | q[3]), bo = (short)((q[4] << 8) | q[5]), ri = (short)((q[6] << 8) | q[7]);
            oah_vd_setsize(ri - l, bo - t);
        }
        if (oah_cam_log()) fprintf(stderr, "[cam] VDSetCompression '%c%c%c%c' depth %d -> %dx%d (frames are 'raw ' ARGB)\n", type >> 24, type >> 16, type >> 8, type, g_vd.depth, g_vd.w, g_vd.h);
        break; }
    case 69:                                                          /* VDCompressOneFrameAsync */
        g_vd.want = 1;
        if (oah_cam_start()) r = 0xfffff768u;                         /* digiUnimpErr - 1: no camera */
        break;
    case 70: { static const unsigned char sz[] = { 4, 4, 4, 4, 4 };   /* VDCompressDone (UInt8 *queued, Ptr *data, long *size, UInt8 *similarity, TimeRecord *) */
        unsigned int q = oah_vd_arg(P, sz, 5, 0), d = oah_vd_arg(P, sz, 5, 1), s = oah_vd_arg(P, sz, 5, 2), sim = oah_vd_arg(P, sz, 5, 3), t = oah_vd_arg(P, sz, 5, 4);
        unsigned char *f = g_vd.want ? oah_vd_take() : 0;
        if (q && oah_readable(q, 1)) *(unsigned char *)(uintptr_t)q = f ? 1 : 0;
        if (f) {
            static unsigned int gtbt;
            g_vd.want = 0; g_vd.frames++;
            oah_vd_put32(d, (unsigned int)(uintptr_t)f);
            oah_vd_put32(s, (unsigned int)(g_vd.w * g_vd.h * 4));
            if (sim && oah_readable(sim, 1)) *(unsigned char *)(uintptr_t)sim = 0;
            if (t && oah_readable(t, 16)) {
                if (!gtbt) gtbt = oah_guest_sym("QuickTime", "_GetTimeBaseTime");
                if (g_vd.timebase && gtbt) { unsigned int a[3]; a[0] = g_vd.timebase; a[1] = 600; a[2] = t; oah_callppc_n("GetTimeBaseTime", gtbt, 3, a); }
                else { unsigned long long v = (unsigned long long)((oah_cam_now() - g_vd.t0) * 600.0);
                       oah_vd_put32(t, (unsigned int)(v >> 32)); oah_vd_put32(t + 4, (unsigned int)v); oah_vd_put32(t + 8, 600); oah_vd_put32(t + 12, 0); }
            }
            if (oah_cam_log() && g_vd.frames <= 3) fprintf(stderr, "[cam] frame %u to the guest (%dx%d)\n", g_vd.frames, g_vd.w, g_vd.h);
        }
        break; }
    case 71: case 73: break;                                          /* VDReleaseCompressBuffer, VDResetCompressSequence */
    case 72: { unsigned int id = oah_vd_handle(A4(1, 0), 86);         /* VDGetImageDescription (ImageDescriptionHandle) */
        if (id) {
            memset((void *)(uintptr_t)id, 0, 86);
            oah_vd_put32(id, 86); oah_vd_put32(id + 4, 'raw ');
            oah_vd_put16(id + 16, 1); oah_vd_put32(id + 20, 'appl');   /* version, vendor */
            oah_vd_put32(id + 28, 0x200);                             /* spatial quality: normal */
            oah_vd_put16(id + 32, g_vd.w); oah_vd_put16(id + 34, g_vd.h);
            oah_vd_put32(id + 36, 72 << 16); oah_vd_put32(id + 40, 72 << 16);
            oah_vd_put16(id + 48, 1);                                 /* frame count */
            oah_vd_pstr(id + 50, "None", 31);
            oah_vd_put16(id + 82, 32); oah_vd_put16(id + 84, 0xffff);
        }
        break; }
    case 74: g_vd.comp_on = oah_vd_arg(P, (const unsigned char *)"\2", 1, 0) >> 8; break;   /* VDSetCompressionOnOff (Boolean) */
    case 75: { unsigned int e = oah_vd_handle(A4(1, 0), 148);         /* VDGetCompressionTypes (VDCompressionListHandle) */
        if (e) {
            memset((void *)(uintptr_t)e, 0, 148);
            oah_vd_put32(e + 4, 'raw '); oah_vd_pstr(e + 8, "None", 63); oah_vd_pstr(e + 72, "Camera", 63);
        }
        break; }
    case 76: g_vd.timebase = A4(1, 0); break;                         /* VDSetTimeBase */
    case 77: g_vd.fps_fixed = (long)A4(1, 0); break;                  /* VDSetFrameRate (Fixed) */
    case 78: oah_vd_put32(A4(3, 0), 33); oah_vd_put32(A4(3, 1), 30 << 16); oah_vd_put32(A4(3, 2), (unsigned int)(g_vd.w * g_vd.h * 4 * 30)); break;   /* VDGetDataRate */
    case 81: oah_vd_put32(A4(1, 0), 600); break;                      /* VDGetPreferredTimeScale */
    case 82: break;                                                   /* VDReleaseAsyncBuffers */
    case 85: r = VD_UNIMP; break;                                     /* VDGetTimeCode */
    case 90: oah_vd_setsize((int)A4(2, 0), (int)A4(2, 1)); break;    /* VDSetPreferredImageDimensions */
    case 91: oah_vd_put32(A4(2, 0), (unsigned int)g_vd.w); oah_vd_put32(A4(2, 1), (unsigned int)g_vd.h); break;
    case 92: oah_vd_pstr(A4(2, 1), "Camera", 255); break;             /* VDGetInputName (long input, Str255) */
    case 94: oah_vd_pstr(A4(2, 0), g_vd.name[0] ? g_vd.name : "Camera", 255); oah_vd_put32(A4(2, 1), 0); break;   /* VDGetDeviceNameAndFlags */
    case 95: break;                                                   /* VDCaptureStateChanging */
    default: r = VD_BADSEL; break;
    }
#undef A4
    if (oah_cam_log() && what != 70) fprintf(stderr, "[cam] vdig selector %d (params %d bytes) -> %d\n", what, psize, (int)r);
    g_gt_skip_ret = r;
}

/* at EnterMovies (oah_fix_entermovies): register the digitizer when the Mac has a camera */
static void oah_cam_register(void) {
    static int done;
    unsigned int rc, *t, a[6];
    static unsigned int cd[5];
    oah_id dev, nm;
    if (done) return;
    done = 1;
    if (g_cam_idx < 0 || getenv("OAH_NO_CAMERA")) return;
    if (!objc_getClass("NSAutoreleasePool")) dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY | RTLD_GLOBAL);
    {   oah_id pool = CAM_MSG(CAM_MSG((oah_id)objc_getClass("NSAutoreleasePool"), CAM_SEL("alloc")), CAM_SEL("init"));
        dev = oah_cam_device();
        if (dev && (nm = CAM_MSG(dev, CAM_SEL("localizedName"))))
            CFStringGetCString((CFStringRef)nm, g_vd.name, sizeof g_vd.name, kCFStringEncodingMacRoman);
        CAM_MSG(pool, CAM_SEL("drain"));
    }
    if (!dev) { if (oah_cam_log()) fprintf(stderr, "[cam] no camera\n"); return; }
    rc = oah_guest_sym("CarbonCore", "_RegisterComponent");
    if (!rc || !(t = (unsigned int *)oah_guest_code_alloc(8))) return;
    t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_cam_idx));
    t[1] = OSSwapHostToBigInt32(0x4e800020);                          /* blr */
    g_cam_stub = (unsigned int)(uintptr_t)t;
    cd[0] = OSSwapHostToBigInt32('vdig'); cd[1] = OSSwapHostToBigInt32('rcam'); cd[2] = OSSwapHostToBigInt32('RSTS');
    cd[3] = 0; cd[4] = 0;
    a[0] = (unsigned int)(uintptr_t)cd; a[1] = g_cam_stub; a[2] = 1 /* registerComponentGlobal */; a[3] = 0; a[4] = 0; a[5] = 0;
    rc = oah_callppc_n("RegisterComponent", rc, 6, a);
    fprintf(stderr, "[cam] camera \"%s\" registered as a video digitizer (component %08x)\n", g_vd.name, rc);
}
