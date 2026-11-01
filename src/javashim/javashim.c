#include <stdlib.h>
/* libjavashim: compatibility functions for Apple's PowerPC Java on Snow Leopard's PowerPC frameworks.
   Apple's Java 1.4.2 and 5.0 for Tiger (libawt.jnilib) call CoreText as it was in Mac OS X 10.4,
   before CoreText became public in 10.5:
     CTFontGetGraphicsFont          renamed CTFontCopyGraphicsFont (10.6 has no "Get" version: libawt
                                    did not load at all)
     CTFontGetAdvancesForGlyphs     10.4: CGSize f(font, glyphs, advances, count) (struct return);
                                    10.5+: double f(font, orientation, glyphs, advances, count)
     CTLineGetTypographicBounds     10.4: double f(line, CFRange, ascent, descent, leading);
                                    10.5+: double f(line, ascent, descent, leading)
   With the new functions under the old calls, Java 1.4.2's text measuring spun forever and 5.0 could
   write through a range length taken as a pointer. tools/javafix.py binds these imports of the Java
   libraries to this library (only those), which implements the 10.4 behaviour with the 10.6 calls.
   The real functions are looked up in ApplicationServices with dlsym. No CoreText header is included:
   the exported names are the same as CoreText's, with other prototypes.
   Build (Snow Leopard, PowerPC compiler): see build.sh. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <string.h>

typedef float rj_float;                                   /* CGFloat on 32-bit */
typedef struct { rj_float width, height; } rj_size;       /* CGSize */
typedef unsigned short rj_glyph;                          /* CGGlyph */
typedef const void *rj_ref;                               /* CTFontRef, CTLineRef, CGFontRef */

static void *rj_sym(const char *name) {
    static void *as;
    if (!as) as = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_LOCAL);
    return as ? dlsym(as, name) : 0;
}

rj_ref rj_get_graphics_font(rj_ref font, rj_ref *attributes) __asm__("_CTFontGetGraphicsFont");
rj_ref rj_get_graphics_font(rj_ref font, rj_ref *attributes) {
    /* "Get": not retained for the caller (libawt retains it). The CTFont keeps its CGFont alive,
       so the reference from the Copy call is dropped right away. */
    static rj_ref (*copy)(rj_ref, rj_ref *);
    rj_ref g;
    if (!copy) copy = (rj_ref (*)(rj_ref, rj_ref *))rj_sym("CTFontCopyGraphicsFont");
    if (!font || !copy) return 0;
    g = copy(font, attributes);
    if (g) CFRelease(g);
    return g;
}

rj_size rj_get_advances(rj_ref font, const rj_glyph *glyphs, rj_size *advances, CFIndex count) __asm__("_CTFontGetAdvancesForGlyphs");
rj_size rj_get_advances(rj_ref font, const rj_glyph *glyphs, rj_size *advances, CFIndex count) {
    static double (*adv)(rj_ref, unsigned int, const rj_glyph *, rj_size *, CFIndex);
    rj_size total = { 0, 0 };
    if (!adv) adv = (double (*)(rj_ref, unsigned int, const rj_glyph *, rj_size *, CFIndex))rj_sym("CTFontGetAdvancesForGlyphs");
    if (!adv || !font || !glyphs || count <= 0) return total;
    total.width = (rj_float)adv(font, 0 /* kCTFontDefaultOrientation */, glyphs, advances, count);
    return total;
}

double rj_line_bounds(rj_ref line, CFRange range, rj_float *ascent, rj_float *descent, rj_float *leading) __asm__("_CTLineGetTypographicBounds");
double rj_line_bounds(rj_ref line, CFRange range, rj_float *ascent, rj_float *descent, rj_float *leading) {
    static double (*bounds)(rj_ref, rj_float *, rj_float *, rj_float *);
    static CFRange (*srange)(rj_ref);
    static rj_float (*offset)(rj_ref, CFIndex, rj_float *);
    double w;
    CFRange all;
    if (!bounds) {
        bounds = (double (*)(rj_ref, rj_float *, rj_float *, rj_float *))rj_sym("CTLineGetTypographicBounds");
        srange = (CFRange (*)(rj_ref))rj_sym("CTLineGetStringRange");
        offset = (rj_float (*)(rj_ref, CFIndex, rj_float *))rj_sym("CTLineGetOffsetForStringIndex");
    }
    if (!bounds || !line) return 0;
    w = bounds(line, ascent, descent, leading);
    /* a part of the line: the distance between the offsets of its ends */
    if (srange && offset && range.length > 0) {
        all = srange(line);
        if (range.location != all.location || range.length != all.length)
            w = offset(line, range.location + range.length, 0) - offset(line, range.location, 0);
    }
    return w;
}

/* ---- Java 1.3.1 (libgraphics.dylib, librealawt.jnilib): CoreGraphics and Process Manager calls of
   Mac OS X 10.1-10.4 that Snow Leopard no longer exports ---- */
typedef struct { unsigned int hi, lo; } rj_psn;
rj_ref rj_cgfont_copy(rj_ref font) __asm__("_CGFontCopy");
rj_ref rj_cgfont_copy(rj_ref font) {   /* a CGFont is immutable: a retained reference serves as a copy */
    static rj_ref (*retain)(rj_ref);
    if (!retain) retain = (rj_ref (*)(rj_ref))rj_sym("CGFontRetain");
    return font && retain ? retain(font) : 0;
}
/* the font's names as C strings: [0] PostScript name, [1] family name, [2] full name (AWT copies
   [0] and [1] right away) */
static void rj_cstr(CFStringRef s, char *buf, size_t n) {
    buf[0] = 0;
    if (s) { if (!CFStringGetCString(s, buf, n, kCFStringEncodingUTF8)) buf[0] = 0; CFRelease(s); }
}
const char **rj_cgfont_monikers(rj_ref font) __asm__("_CGFontGetMonikers");
const char **rj_cgfont_monikers(rj_ref font) {
    static CFStringRef (*psname)(rj_ref), (*fullname)(rj_ref), (*family)(rj_ref);
    static rj_ref (*ctcreate)(rj_ref, double, const void *, rj_ref);
    static char buf[8][3][256];
    static const char *names[8][4];
    static unsigned int next;
    unsigned int k, i;
    rj_ref ct;
    if (!psname) {
        psname = (CFStringRef (*)(rj_ref))rj_sym("CGFontCopyPostScriptName");
        fullname = (CFStringRef (*)(rj_ref))rj_sym("CGFontCopyFullName");
        family = (CFStringRef (*)(rj_ref))rj_sym("CTFontCopyFamilyName");
        ctcreate = (rj_ref (*)(rj_ref, double, const void *, rj_ref))rj_sym("CTFontCreateWithGraphicsFont");
    }
    if (!font || !psname) return 0;
    k = __sync_fetch_and_add(&next, 1) & 7;
    rj_cstr(psname(font), buf[k][0], sizeof buf[k][0]);
    buf[k][1][0] = 0;
    /* family: the PostScript name up to its style suffix ("Helvetica-Bold" -> "Helvetica").
       CTFontCreateWithGraphicsFont crashed in CoreText (NULL+0x18) for the fonts Java 1.3.1's AWT
       gets from the obsolete CGFontFindByName. OAH_JAVASHIM_CTFAMILY=1 asks CoreText instead. */
    if (getenv("OAH_JAVASHIM_CTFAMILY") && family && ctcreate && (ct = ctcreate(font, 12.0, 0, 0))) { rj_cstr(family(ct), buf[k][1], sizeof buf[k][1]); CFRelease(ct); }
    else { char *d; memcpy(buf[k][1], buf[k][0], sizeof buf[k][1]); if ((d = strchr(buf[k][1], '-'))) *d = 0; }
    rj_cstr(fullname ? fullname(font) : 0, buf[k][2], sizeof buf[k][2]);
    if (!buf[k][1][0]) memcpy(buf[k][1], buf[k][0], sizeof buf[k][1]);
    if (!buf[k][2][0]) memcpy(buf[k][2], buf[k][0], sizeof buf[k][2]);
    for (i = 0; i < 3; i++) names[k][i] = buf[k][i];
    names[k][3] = 0;
    return names[k];
}
CFIndex rj_cgfont_mapsize(rj_ref font) __asm__("_CGFontGetUnicodeToGlyphMapSize");
CFIndex rj_cgfont_mapsize(rj_ref font) {
    static size_t (*nglyphs)(rj_ref);
    if (!nglyphs) nglyphs = (size_t (*)(rj_ref))rj_sym("CGFontGetNumberOfGlyphs");
    return font && nglyphs ? (CFIndex)nglyphs(font) : 0;
}
/* rendering hints Snow Leopard sets per context instead: accepted and ignored */
static Boolean g_rj_intadv;
Boolean rj_cgfont_get_intadv(rj_ref font) __asm__("_CGFontGetUseIntegerAdvances");
Boolean rj_cgfont_get_intadv(rj_ref font) { (void)font; return g_rj_intadv; }
void rj_cgfont_set_intadv(rj_ref font, Boolean on) __asm__("_CGFontSetUseIntegerAdvances");
void rj_cgfont_set_intadv(rj_ref font, Boolean on) { (void)font; g_rj_intadv = on; }
void rj_cgfont_set_bitmaps(rj_ref font, Boolean on) __asm__("_CGFontSetDisplayBitmaps");
void rj_cgfont_set_bitmaps(rj_ref font, Boolean on) { (void)font; (void)on; }
void rj_cgfont_set_aa(rj_ref font, Boolean on) __asm__("_CGFontSetShouldAntialias");
void rj_cgfont_set_aa(rj_ref font, Boolean on) { (void)font; (void)on; }
/* the 10.1-era call AWT made to turn a command-line Java program into a foreground (Dock) app */
int rj_enable_fg(const void *a, const void *b, const void *c) __asm__("_EnableForegroundWithDockInfo");
int rj_enable_fg(const void *a, const void *b, const void *c) {
    static int (*transform)(const rj_psn *, unsigned int);
    static int (*front)(const rj_psn *);
    rj_psn me = { 0, 2 /* kCurrentProcess */ };
    (void)a; (void)b; (void)c;
    if (!transform) {
        transform = (int (*)(const rj_psn *, unsigned int))rj_sym("TransformProcessType");
        front = (int (*)(const rj_psn *))rj_sym("SetFrontProcess");
    }
    if (transform) transform(&me, 1 /* kProcessTransformToForegroundApplication */);
    if (front) front(&me);
    return 0;
}
/* 10.1-10.4: a pointer to the font's ATSFontRef. 10.6's private function of that name has another
   signature (it returned NULL here and AWT dereferenced it). */
const unsigned int *rj_cgfont_platform(rj_ref font) __asm__("_CGFontGetPlatformFont");
const unsigned int *rj_cgfont_platform(rj_ref font) {
    static rj_ref (*ctcreate)(rj_ref, double, const void *, rj_ref);
    static unsigned int (*ctplatform)(rj_ref, rj_ref *);
    static unsigned int ring[16];
    static unsigned int next;
    rj_ref ct;
    unsigned int k;
    if (!ctcreate) {
        ctcreate = (rj_ref (*)(rj_ref, double, const void *, rj_ref))rj_sym("CTFontCreateWithGraphicsFont");
        ctplatform = (unsigned int (*)(rj_ref, rj_ref *))rj_sym("CTFontGetPlatformFont");
    }
    if (!font || !ctcreate || !ctplatform || !(ct = ctcreate(font, 12.0, 0, 0))) return 0;
    k = __sync_fetch_and_add(&next, 1) & 15;
    ring[k] = ctplatform(ct, 0);
    CFRelease(ct);
    return ring[k] ? &ring[k] : 0;
}
