/* oah_qtswf.h - Flash (SWF) files as QuickTime movies again, for the guest's own QuickTime.

   Snow Leopard's QuickTime (7.6) has no Flash importer and its Flash media handler is disabled,
   so NewMovieFromFile on a .swf fails with -2048 (noMovieFound) - on a real Snow Leopard Mac as
   well. Kid Pix Deluxe 4 writes each animated sticker to a temporary .swf and opens it with
   OpenMovieFile + NewMovieFromFile; every sticker stayed an empty box.

   OpenMovieFile is watched (gtrace fix + fixret): when the file starts with "FWS", its resource
   reference number is remembered. NewMovieFromFile on such a reference renders the SWF here
   (oah_swf.h, at 2x for scaling up) and builds the movie with the guest's QuickTime: one video
   track of uncompressed 32-bit ARGB ('raw ') frames kept in a handle, straight alpha
   (transparent where the Flash movie shows its background), at the SWF's frame rate.
   OAH_NO_SWF=1 leaves NewMovieFromFile alone; OAH_SWF_OPAQUE=1 keeps the SWF background color;
   OAH_SWF_SCALE=n renders at n x (default 2); OAH_SWFTRACE=1 logs. */
#include "oah_swf.h"
#define OAH_SWF_REFS 16
static struct { short ref; char path[1024]; } g_swf_ref[OAH_SWF_REFS];
static unsigned int g_swf_pend_refp; static char g_swf_pend_path[1024];
static int oah_swftr(void) { static int t = -1; if (t < 0) t = getenv("OAH_SWFTRACE") ? 1 : 0; return t; }
/* With QuickTime 6's Flash added to the guest (tools/install_qtflash.sh) QuickTime plays .swf
   itself - all of Flash 5, sounds included - and this renderer stays out of the way, unless
   OAH_SWF=1 asks for it. */
static int oah_want_swf(void) {
    if (getenv("OAH_NO_SWF")) return 0;
    if (!getenv("OAH_SWF") && !getenv("OAH_NO_QTFLASH")) {
        char t[1200]; struct stat sb;
        snprintf(t, sizeof t, "%s/System/Library/QuickTime/RosettusFlash.component/Contents/MacOS/RosettusFlash", g_rosetta_root);
        if (!stat(t, &sb)) return 0;
    }
    return 1;
}
static unsigned int oah_swf_g(const char *lib, const char *sym) { return oah_guest_sym(lib, sym); }
static unsigned int oah_swf_call(const char *lib, const char *sym, int n, unsigned int a0, unsigned int a1, unsigned int a2,
                                 unsigned int a3, unsigned int a4, unsigned int a5, unsigned int a6, unsigned int a7) {
    unsigned int a[8], f = oah_swf_g(lib, sym);
    if (!f) { fprintf(stderr, "[swf] guest %s not found\n", sym); return 0; }
    a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; a[5] = a5; a[6] = a6; a[7] = a7;
    return oah_callppc_n(sym, f, n, a);
}
#define GQT(sym, n, ...) oah_swf_call("QuickTime", sym, n, __VA_ARGS__)
static unsigned int oah_swf_newhandle(unsigned int size, unsigned char **p) {
    unsigned int h = oah_swf_call("CarbonCore", "_NewHandle", 1, size, 0, 0, 0, 0, 0, 0, 0), mp;
    if (!h) return 0;
    mp = OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)h);
    *p = (unsigned char *)(uintptr_t)mp;
    return mp ? h : 0;
}
static void oah_fix_swf_open(unsigned int *R) {            /* OpenMovieFile(const FSSpec *, short *ref, SInt8 perm) */
    char path[1024]; unsigned char head[4]; FILE *f;
    g_swf_pend_refp = 0;
    if (!R[3] || !R[4] || oah_qt_spec_path(R[3], path, sizeof path)) return;
    if (!(f = fopen(path, "rb"))) return;
    if (fread(head, 1, 4, f) == 4 && !memcmp(head, "FWS", 3)) { g_swf_pend_refp = R[4]; snprintf(g_swf_pend_path, sizeof g_swf_pend_path, "%s", path); }
    fclose(f);
}
static void oah_fixret_swf_open(unsigned int *R, unsigned int a0) {
    short ref; int i, slot = 0;
    (void)a0;
    if (!g_swf_pend_refp) return;
    if (R[3] == 0) {
        ref = (short)OSSwapBigToHostInt16(*(unsigned short *)(uintptr_t)g_swf_pend_refp);
        for (i = 0; i < OAH_SWF_REFS; i++) if (g_swf_ref[i].ref == ref || !g_swf_ref[i].ref) { slot = i; break; }
        g_swf_ref[slot].ref = ref;
        snprintf(g_swf_ref[slot].path, sizeof g_swf_ref[slot].path, "%s", g_swf_pend_path);
        if (oah_swftr()) fprintf(stderr, "[swf] OpenMovieFile \"%s\" -> ref %d\n", g_swf_pend_path, ref);
    }
    g_swf_pend_refp = 0;
}
static void oah_fix_swf_newmovie(unsigned int *R) {        /* NewMovieFromFile(Movie *, short ref, short *resId, StringPtr, short flags, Boolean *) */
    short ref = (short)R[4]; int i; FILE *f; long len; unsigned char *d;
    swf_movie mv; double scale = getenv("OAH_SWF_SCALE") ? atof(getenv("OAH_SWF_SCALE")) : 2.0;
    unsigned int data, dref, desc, movie, track, media, mh, k, total, fdur, nw, nh;
    unsigned char *dp, *rp, *ip; unsigned short opc[3] = { 0, 0, 0 };
    int opaque = getenv("OAH_SWF_OPAQUE") != 0;
    for (i = 0; i < OAH_SWF_REFS; i++) if (g_swf_ref[i].ref == ref && g_swf_ref[i].path[0]) break;
    if (i == OAH_SWF_REFS || !R[3]) return;
    if (!(f = fopen(g_swf_ref[i].path, "rb"))) return;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc(len > 0 ? len : 1);
    if (!d || fread(d, 1, len, f) != (size_t)len) { fclose(f); free(d); return; }
    fclose(f);
    if (scale < 1 || scale > 4) scale = 2;
    if (swf_render(d, len, scale, !opaque, &mv)) { free(d); free(mv.pix); if (oah_swftr()) fprintf(stderr, "[swf] %s: not rendered\n", g_swf_ref[i].path); return; }
    free(d);
    if (!opaque) {                       /* Kid Pix draws the track with graphicsModeStraightAlpha: un-premultiply */
        size_t n = (size_t)mv.w * mv.h * mv.frames, p;
        for (p = 0; p < n; p++) {
            unsigned char *px = mv.pix + 4 * p; unsigned int a = px[0], c;
            if (a == 0 || a == 255) continue;
            for (c = 1; c < 4; c++) { unsigned int v = (px[c] * 255 + a / 2) / a; px[c] = v > 255 ? 255 : v; }
        }
    }
    total = (unsigned int)mv.w * mv.h * 4 * mv.frames;
    nw = (unsigned int)(mv.w / scale + 0.5); nh = (unsigned int)(mv.h / scale + 0.5);
    data = oah_swf_newhandle(total, &dp);
    dref = oah_swf_newhandle(4, &rp);
    desc = oah_swf_newhandle(86, &ip);
    if (!data || !dref || !desc) { free(mv.pix); fprintf(stderr, "[swf] out of guest memory\n"); return; }
    memcpy(dp, mv.pix, total);
    free(mv.pix);
    *(unsigned int *)rp = OSSwapHostToBigInt32(data);
    memset(ip, 0, 86);
    *(unsigned int *)(ip + 0) = OSSwapHostToBigInt32(86);
    memcpy(ip + 4, "raw ", 4);
    *(unsigned short *)(ip + 14) = OSSwapHostToBigInt16(1);           /* dataRefIndex */
    memcpy(ip + 20, "appl", 4);
    *(unsigned int *)(ip + 28) = OSSwapHostToBigInt32(0x400);         /* codecHighQuality */
    *(unsigned short *)(ip + 32) = OSSwapHostToBigInt16((unsigned short)mv.w);
    *(unsigned short *)(ip + 34) = OSSwapHostToBigInt16((unsigned short)mv.h);
    *(unsigned int *)(ip + 36) = OSSwapHostToBigInt32(72 << 16);
    *(unsigned int *)(ip + 40) = OSSwapHostToBigInt32(72 << 16);
    *(unsigned short *)(ip + 48) = OSSwapHostToBigInt16(1);           /* frameCount */
    ip[50] = 4; memcpy(ip + 51, "None", 4);
    *(unsigned short *)(ip + 82) = OSSwapHostToBigInt16(32);
    *(unsigned short *)(ip + 84) = OSSwapHostToBigInt16(0xffff);      /* clutID -1 */
    movie = GQT("_NewMovie", 1, 1 /* newMovieActive */, 0, 0, 0, 0, 0, 0, 0);
    track = movie ? GQT("_NewMovieTrack", 4, movie, nw << 16, nh << 16, 0, 0, 0, 0, 0) : 0;
    media = track ? GQT("_NewTrackMedia", 5, track, 'vide', 600, dref, 'hndl', 0, 0, 0) : 0;
    if (!media) { fprintf(stderr, "[swf] guest QuickTime would not make the movie (%08x %08x %08x)\n", movie, track, media); return; }
    fdur = (unsigned int)(600.0 / mv.fps + 0.5); if (!fdur) fdur = 1;
    GQT("_BeginMediaEdits", 1, media, 0, 0, 0, 0, 0, 0, 0);
    for (k = 0; k < (unsigned int)mv.frames; k++)
        GQT("_AddMediaSampleReference", 8, media, k * (unsigned int)mv.w * mv.h * 4, (unsigned int)mv.w * mv.h * 4, fdur, desc, 1, 0, 0);
    GQT("_EndMediaEdits", 1, media, 0, 0, 0, 0, 0, 0, 0);
    GQT("_InsertMediaIntoTrack", 5, track, 0, 0, GQT("_GetMediaDuration", 1, media, 0, 0, 0, 0, 0, 0, 0), 0x10000, 0, 0, 0);
    mh = GQT("_GetMediaHandler", 1, media, 0, 0, 0, 0, 0, 0, 0);
    if (mh && !opaque) GQT("_MediaSetGraphicsMode", 3, mh, 256 /* graphicsModeStraightAlpha */, (unsigned int)(uintptr_t)opc, 0, 0, 0, 0, 0);
    *(unsigned int *)(uintptr_t)R[3] = OSSwapHostToBigInt32(movie);
    if (R[8]) *(unsigned char *)(uintptr_t)R[8] = 0;
    if (oah_swftr() || 1) fprintf(stderr, "[swf] %s: %d frames %dx%d at %.0f fps -> movie %08x (duration %u)\n",
                                  g_swf_ref[i].path, mv.frames, nw, nh, mv.fps, movie, GQT("_GetMovieDuration", 1, movie, 0, 0, 0, 0, 0, 0, 0));
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
