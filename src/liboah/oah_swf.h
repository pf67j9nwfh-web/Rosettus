/* oah_swf.h - a small SWF (Flash 3/4) renderer: shapes, gradients, display list, frames.

   QuickTime 7.3.1 and later (so Snow Leopard's, and Mojave's) no longer import Flash files, and
   their Flash media handler draws nothing. Kid Pix Deluxe 4 keeps its animated stickers ("Toons")
   as SWF 3/4 movies and opens each one with OpenMovieFile + NewMovieFromFile: the call failed with
   -2048 and the sticker stayed an empty box. The stickers use only DefineShape / DefineShape2,
   PlaceObject2 / RemoveObject2, ShowFrame and SetBackgroundColor, rendered here with CoreGraphics
   into one 32-bit ARGB frame per ShowFrame (oah_qtswf.h makes a QuickTime movie of them).

   Not supported: bitmaps, text, buttons, sprites, morph shapes, sound, actions (the movie loops). */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
/* CoreGraphics through dlsym (liboah does not link ApplicationServices; its headers clash with
   liboah's own declarations). 32-bit: CGFloat is float. */
typedef float swf_cgf;
typedef struct { swf_cgf a, b, c, d, tx, ty; } swf_xf;
typedef struct { swf_cgf x, y, w, h; } swf_rc;
typedef struct { swf_cgf x, y; } swf_pt;
static struct {
    void *(*CGColorSpaceCreateDeviceRGB)(void);
    void (*CGColorSpaceRelease)(void *);
    void *(*CGBitmapContextCreate)(void *, size_t, size_t, size_t, size_t, void *, unsigned int);
    void (*CGContextRelease)(void *);
    void (*CGContextSetShouldAntialias)(void *, int);
    void (*CGContextSaveGState)(void *);
    void (*CGContextRestoreGState)(void *);
    void (*CGContextConcatCTM)(void *, swf_xf);
    void (*CGContextTranslateCTM)(void *, swf_cgf, swf_cgf);
    void (*CGContextScaleCTM)(void *, swf_cgf, swf_cgf);
    void (*CGContextSetRGBFillColor)(void *, swf_cgf, swf_cgf, swf_cgf, swf_cgf);
    void (*CGContextSetRGBStrokeColor)(void *, swf_cgf, swf_cgf, swf_cgf, swf_cgf);
    void (*CGContextFillRect)(void *, swf_rc);
    void (*CGContextClearRect)(void *, swf_rc);
    void (*CGContextBeginPath)(void *);
    void (*CGContextMoveToPoint)(void *, swf_cgf, swf_cgf);
    void (*CGContextAddLineToPoint)(void *, swf_cgf, swf_cgf);
    void (*CGContextAddQuadCurveToPoint)(void *, swf_cgf, swf_cgf, swf_cgf, swf_cgf);
    void (*CGContextClosePath)(void *);
    void (*CGContextEOFillPath)(void *);
    void (*CGContextEOClip)(void *);
    void (*CGContextStrokePath)(void *);
    void (*CGContextSetLineWidth)(void *, swf_cgf);
    void (*CGContextSetLineCap)(void *, int);
    void (*CGContextSetLineJoin)(void *, int);
    void (*CGContextFlush)(void *);
    void *(*CGGradientCreateWithColorComponents)(void *, const swf_cgf *, const swf_cgf *, size_t);
    void (*CGGradientRelease)(void *);
    void (*CGContextDrawLinearGradient)(void *, void *, swf_pt, swf_pt, unsigned int);
    void (*CGContextDrawRadialGradient)(void *, void *, swf_pt, swf_cgf, swf_pt, swf_cgf, unsigned int);
} G;
static int swf_cg_load(void) {
    static int ok = -1; void *h;
    if (ok >= 0) return ok;
    h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY);
    ok = 0;
    if (!h) return 0;
#define L(n) if (!(*(void **)&G.n = dlsym(h, #n))) return 0;
    L(CGColorSpaceCreateDeviceRGB) L(CGColorSpaceRelease) L(CGBitmapContextCreate) L(CGContextRelease)
    L(CGContextSetShouldAntialias) L(CGContextSaveGState) L(CGContextRestoreGState) L(CGContextConcatCTM)
    L(CGContextTranslateCTM) L(CGContextScaleCTM) L(CGContextSetRGBFillColor) L(CGContextSetRGBStrokeColor)
    L(CGContextFillRect) L(CGContextClearRect) L(CGContextBeginPath) L(CGContextMoveToPoint) L(CGContextAddLineToPoint)
    L(CGContextAddQuadCurveToPoint) L(CGContextClosePath) L(CGContextEOFillPath) L(CGContextEOClip) L(CGContextStrokePath)
    L(CGContextSetLineWidth) L(CGContextSetLineCap) L(CGContextSetLineJoin) L(CGContextFlush)
    L(CGGradientCreateWithColorComponents) L(CGGradientRelease) L(CGContextDrawLinearGradient) L(CGContextDrawRadialGradient)
#undef L
    ok = 1; return 1;
}

typedef struct { const unsigned char *p; size_t len, pos; int bit; } swf_bs;   /* pos in bytes, bit within */
static unsigned int swf_ub(swf_bs *b, int n) {
    unsigned int v = 0;
    while (n-- > 0) {
        if (b->pos >= b->len) return v << (n + 1);
        v = (v << 1) | ((b->p[b->pos] >> (7 - b->bit)) & 1);
        if (++b->bit == 8) { b->bit = 0; b->pos++; }
    }
    return v;
}
static int swf_sb(swf_bs *b, int n) { unsigned int v = swf_ub(b, n); if (n > 0 && n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n; return (int)v; }
static void swf_align(swf_bs *b) { if (b->bit) { b->bit = 0; b->pos++; } }
static unsigned int swf_u8(swf_bs *b) { swf_align(b); return b->pos < b->len ? b->p[b->pos++] : 0; }
static unsigned int swf_u16(swf_bs *b) { unsigned int a = swf_u8(b); return a | (swf_u8(b) << 8); }
static unsigned int swf_u32(swf_bs *b) { unsigned int a = swf_u16(b); return a | (swf_u16(b) << 16); }

typedef struct { double a, b, c, d, tx, ty; } swf_mat;     /* x' = a*x + c*y + tx, y' = b*x + d*y + ty (twips) */
static swf_mat swf_matrix(swf_bs *b) {
    swf_mat m = { 1, 0, 0, 1, 0, 0 }; int n;
    swf_align(b);
    if (swf_ub(b, 1)) { n = swf_ub(b, 5); m.a = swf_sb(b, n) / 65536.0; m.d = swf_sb(b, n) / 65536.0; }
    if (swf_ub(b, 1)) { n = swf_ub(b, 5); m.b = swf_sb(b, n) / 65536.0; m.c = swf_sb(b, n) / 65536.0; }
    n = swf_ub(b, 5); m.tx = swf_sb(b, n); m.ty = swf_sb(b, n);
    swf_align(b);
    return m;
}
typedef struct { double m[4], a[4]; } swf_cx;                /* r g b a multiply / add (0..255) */
static swf_cx swf_cxform(swf_bs *b, int alpha) {
    swf_cx c = { { 1, 1, 1, 1 }, { 0, 0, 0, 0 } }; int add, mul, n, k, cnt = alpha ? 4 : 3;
    swf_align(b);
    add = swf_ub(b, 1); mul = swf_ub(b, 1); n = swf_ub(b, 4);
    if (mul) for (k = 0; k < cnt; k++) c.m[k] = swf_sb(b, n) / 256.0;
    if (add) for (k = 0; k < cnt; k++) c.a[k] = swf_sb(b, n);
    swf_align(b);
    return c;
}
typedef struct { int type; unsigned char rgba[4]; swf_mat m; int ng; unsigned char gr[16]; unsigned char gc[16][4]; } swf_fill;
typedef struct { int width; unsigned char rgba[4]; } swf_line;
typedef struct { int x0, y0, x1, y1, cx, cy, curve, fill0, fill1, line; } swf_edge;   /* style indices into the shape's global arrays (1-based, 0 none) */
typedef struct {
    int id, used;
    swf_fill *fills; int nfill;
    swf_line *lines; int nline;
    swf_edge *edges; int nedge, cap;
} swf_shape;

static void swf_rgba(swf_bs *b, unsigned char *c, int alpha) { c[0] = swf_u8(b); c[1] = swf_u8(b); c[2] = swf_u8(b); c[3] = alpha ? swf_u8(b) : 255; }
static int swf_styles(swf_bs *b, swf_shape *s, int ver, int *fbase, int *lbase) {
    int n, k, j;
    n = swf_u8(b); if (n == 0xff && ver >= 2) n = swf_u16(b);
    *fbase = s->nfill;
    s->fills = (swf_fill *)realloc(s->fills, (s->nfill + n + 1) * sizeof *s->fills);
    for (k = 0; k < n; k++) {
        swf_fill *f = &s->fills[s->nfill++];
        memset(f, 0, sizeof *f);
        f->type = swf_u8(b);
        if (f->type == 0x00) swf_rgba(b, f->rgba, ver >= 3);
        else if (f->type == 0x10 || f->type == 0x12) {
            f->m = swf_matrix(b);
            f->ng = swf_u8(b) & 15;
            for (j = 0; j < f->ng; j++) { f->gr[j] = swf_u8(b); swf_rgba(b, f->gc[j], ver >= 3); }
        } else if (f->type >= 0x40 && f->type <= 0x43) {
            swf_u16(b); f->m = swf_matrix(b); f->rgba[0] = f->rgba[1] = f->rgba[2] = 128; f->rgba[3] = 255; f->type = 0;
        } else return 0;
    }
    n = swf_u8(b); if (n == 0xff && ver >= 2) n = swf_u16(b);
    *lbase = s->nline;
    s->lines = (swf_line *)realloc(s->lines, (s->nline + n + 1) * sizeof *s->lines);
    for (k = 0; k < n; k++) { swf_line *l = &s->lines[s->nline++]; l->width = swf_u16(b); swf_rgba(b, l->rgba, ver >= 3); }
    return 1;
}
static void swf_add_edge(swf_shape *s, swf_edge *e) {
    if (s->nedge == s->cap) { s->cap = s->cap ? 2 * s->cap : 64; s->edges = (swf_edge *)realloc(s->edges, s->cap * sizeof *s->edges); }
    s->edges[s->nedge++] = *e;
}
static int swf_define_shape(swf_bs *b, swf_shape *s, int ver) {
    int fb = 0, lb = 0, nfb, nlb, x = 0, y = 0, f0 = 0, f1 = 0, ln = 0;
    s->id = swf_u16(b);
    { int n = swf_ub(b, 5); swf_ub(b, 4 * n); swf_align(b); }   /* bounds */
    if (!swf_styles(b, s, ver, &fb, &lb)) return 0;
    nfb = swf_ub(b, 4); nlb = swf_ub(b, 4);
    for (;;) {
        if (b->pos >= b->len) break;
        if (!swf_ub(b, 1)) {                       /* style change / end */
            int fl = swf_ub(b, 5);
            if (!fl) break;
            if (fl & 1) { int n = swf_ub(b, 5); x = swf_sb(b, n); y = swf_sb(b, n); }
            if (fl & 2) { int v = swf_ub(b, nfb); f0 = v ? fb + v : 0; }
            if (fl & 4) { int v = swf_ub(b, nfb); f1 = v ? fb + v : 0; }
            if (fl & 8) { int v = swf_ub(b, nlb); ln = v ? lb + v : 0; }
            if (fl & 16) { if (!swf_styles(b, s, ver, &fb, &lb)) return 0; nfb = swf_ub(b, 4); nlb = swf_ub(b, 4); }
        } else {
            swf_edge e; int n;
            memset(&e, 0, sizeof e);
            e.fill0 = f0; e.fill1 = f1; e.line = ln; e.x0 = x; e.y0 = y;
            if (swf_ub(b, 1)) {                    /* straight */
                n = swf_ub(b, 4) + 2;
                if (swf_ub(b, 1)) { x += swf_sb(b, n); y += swf_sb(b, n); }
                else if (swf_ub(b, 1)) y += swf_sb(b, n); else x += swf_sb(b, n);
            } else {
                n = swf_ub(b, 4) + 2;
                e.curve = 1; e.cx = x + swf_sb(b, n); e.cy = y + swf_sb(b, n);
                x = e.cx + swf_sb(b, n); y = e.cy + swf_sb(b, n);
            }
            e.x1 = x; e.y1 = y;
            swf_add_edge(s, &e);
        }
    }
    s->used = 1;
    return 1;
}
static void swf_color(void * cg, const unsigned char *c, const swf_cx *x) {
    double v[4]; int k;
    for (k = 0; k < 4; k++) { v[k] = c[k] * x->m[k] + x->a[k]; if (v[k] < 0) v[k] = 0; if (v[k] > 255) v[k] = 255; }
    G.CGContextSetRGBFillColor(cg, v[0] / 255, v[1] / 255, v[2] / 255, v[3] / 255);
    G.CGContextSetRGBStrokeColor(cg, v[0] / 255, v[1] / 255, v[2] / 255, v[3] / 255);
}
/* chain the directed edges of one fill style into closed sub-paths */
static void swf_fill_path(void * cg, swf_edge *ed, int n) {
    char *used = (char *)calloc(n, 1); int k, start;
    for (start = 0; start < n; start++) {
        int cur = start, guard = 0;
        if (used[start]) continue;
        G.CGContextMoveToPoint(cg, ed[cur].x0, ed[cur].y0);
        for (;;) {
            used[cur] = 1;
            if (ed[cur].curve) G.CGContextAddQuadCurveToPoint(cg, ed[cur].cx, ed[cur].cy, ed[cur].x1, ed[cur].y1);
            else G.CGContextAddLineToPoint(cg, ed[cur].x1, ed[cur].y1);
            if (ed[cur].x1 == ed[start].x0 && ed[cur].y1 == ed[start].y0) break;
            for (k = 0; k < n; k++) if (!used[k] && ed[k].x0 == ed[cur].x1 && ed[k].y0 == ed[cur].y1) break;
            if (k == n || ++guard > n) break;
            cur = k;
        }
        G.CGContextClosePath(cg);
    }
    free(used);
}
static void swf_draw_shape(void * cg, swf_shape *s, const swf_mat *m, const swf_cx *cx) {
    int fi, k, n;
    swf_edge *tmp = (swf_edge *)malloc((s->nedge + 1) * sizeof *tmp);
    swf_xf t; t.a = m->a; t.b = m->b; t.c = m->c; t.d = m->d; t.tx = m->tx; t.ty = m->ty;
    G.CGContextSaveGState(cg);
    G.CGContextConcatCTM(cg, t);
    for (fi = 1; fi <= s->nfill; fi++) {
        swf_fill *f = &s->fills[fi - 1];
        for (n = 0, k = 0; k < s->nedge; k++) {
            swf_edge e = s->edges[k];
            if (e.fill1 == fi) tmp[n++] = e;
            if (e.fill0 == fi) {                  /* reversed */
                int x = e.x0, y = e.y0; e.x0 = e.x1; e.y0 = e.y1; e.x1 = x; e.y1 = y; tmp[n++] = e;
            }
        }
        if (!n) continue;
        G.CGContextBeginPath(cg);
        swf_fill_path(cg, tmp, n);
        if (f->type == 0) { swf_color(cg, f->rgba, cx); G.CGContextEOFillPath(cg); }
        else {                                     /* gradient: clip, then draw in gradient space */
            void * cs = G.CGColorSpaceCreateDeviceRGB();
            swf_cgf comp[16 * 4], loc[16]; int j, q;
            void * g;
            for (j = 0; j < f->ng; j++) {
                for (q = 0; q < 4; q++) { double v = f->gc[j][q] * cx->m[q] + cx->a[q]; comp[4 * j + q] = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0; }
                loc[j] = f->gr[j] / 255.0;
            }
            g = G.CGGradientCreateWithColorComponents(cs, comp, loc, f->ng);
            G.CGContextSaveGState(cg);
            G.CGContextEOClip(cg);
            { swf_xf gx; gx.a = f->m.a; gx.b = f->m.b; gx.c = f->m.c; gx.d = f->m.d; gx.tx = f->m.tx; gx.ty = f->m.ty; G.CGContextConcatCTM(cg, gx); }
            if (f->type == 0x10) { swf_pt p0 = { -16384, 0 }, p1 = { 16384, 0 }; G.CGContextDrawLinearGradient(cg, g, p0, p1, 3); }
            else { swf_pt c0 = { 0, 0 }; G.CGContextDrawRadialGradient(cg, g, c0, 0, c0, 16384, 3); }
            G.CGContextRestoreGState(cg);
            G.CGGradientRelease(g); G.CGColorSpaceRelease(cs);
        }
    }
    /* outlines on top, in edge order */
    G.CGContextSetLineCap(cg, 1); G.CGContextSetLineJoin(cg, 1);
    for (k = 0; k < s->nedge; k++) {
        swf_edge *e = &s->edges[k]; swf_line *l;
        if (!e->line || e->line > s->nline) continue;
        l = &s->lines[e->line - 1];
        swf_color(cg, l->rgba, cx);
        G.CGContextSetLineWidth(cg, l->width < 20 ? 20 : l->width);
        G.CGContextBeginPath(cg);
        G.CGContextMoveToPoint(cg, e->x0, e->y0);
        if (e->curve) G.CGContextAddQuadCurveToPoint(cg, e->cx, e->cy, e->x1, e->y1); else G.CGContextAddLineToPoint(cg, e->x1, e->y1);
        G.CGContextStrokePath(cg);
    }
    G.CGContextRestoreGState(cg);
    free(tmp);
}

#define SWF_MAXCH 2048
#define SWF_MAXDEPTH 1024
typedef struct { int ch; swf_mat m; swf_cx cx; } swf_place;
typedef struct {
    int w, h, frames;          /* output size in pixels (already scaled) and frame count */
    double fps;
    unsigned char bg[4];       /* background RGB(A) */
    unsigned char *pix;        /* frames * w * h * 4, ARGB, big-endian byte order (A,R,G,B) */
} swf_movie;

/* Render every frame of an uncompressed SWF ("FWS") at the given scale, on a transparent background
   (antialiased) or on the movie's background color (not antialiased). Frames are premultiplied ARGB.
   Returns 0 on success. */
static int swf_render(const unsigned char *d, size_t len, double scale, int transparent, swf_movie *mv) {
    swf_bs b = { d, len, 0, 0 };
    int ver, x0, x1, y0, y1, n, frames_hdr, cap, fr = 0;
    swf_shape *sh; swf_place *dl;
    void * cs; void * cg; unsigned char *buf;
    memset(mv, 0, sizeof *mv);
    if (!swf_cg_load()) return -5;
    if (len < 21 || memcmp(d, "FWS", 3)) return -1;
    ver = d[3];
    b.pos = 8;
    n = swf_ub(&b, 5); x0 = swf_sb(&b, n); x1 = swf_sb(&b, n); y0 = swf_sb(&b, n); y1 = swf_sb(&b, n);
    swf_align(&b);
    mv->fps = swf_u16(&b) / 256.0; if (mv->fps <= 0) mv->fps = 12;
    frames_hdr = swf_u16(&b);
    mv->w = (int)((x1 - x0) / 20.0 * scale + 0.5); mv->h = (int)((y1 - y0) / 20.0 * scale + 0.5);
    if (mv->w <= 0 || mv->h <= 0 || mv->w > 4096 || mv->h > 4096) return -2;
    mv->bg[0] = mv->bg[1] = mv->bg[2] = 255; mv->bg[3] = 255;
    cap = frames_hdr > 0 ? frames_hdr : 16;
    mv->pix = (unsigned char *)malloc((size_t)cap * mv->w * mv->h * 4);
    sh = (swf_shape *)calloc(SWF_MAXCH, sizeof *sh);
    dl = (swf_place *)calloc(SWF_MAXDEPTH, sizeof *dl);
    cs = G.CGColorSpaceCreateDeviceRGB();
    buf = (unsigned char *)calloc((size_t)mv->w * mv->h, 4);
    cg = G.CGBitmapContextCreate(buf, mv->w, mv->h, 8, mv->w * 4, cs, 2 /* kCGImageAlphaPremultipliedFirst */ | (4 << 12) /* kCGBitmapByteOrder32Big */);
    if (!mv->pix || !sh || !dl || !cg) { free(sh); free(dl); free(buf); if (cg) G.CGContextRelease(cg); G.CGColorSpaceRelease(cs); return -3; }
    G.CGContextSetShouldAntialias(cg, transparent ? 1 : 0);
    while (b.pos + 2 <= len) {
        unsigned int h = swf_u16(&b), code = h >> 6, tl = h & 63; size_t end;
        if (tl == 63) tl = swf_u32(&b);
        end = b.pos + tl; if (end > len) break;
        if (code == 0) break;
        if (code == 9) { mv->bg[0] = d[b.pos]; mv->bg[1] = d[b.pos + 1]; mv->bg[2] = d[b.pos + 2]; }
        else if (code == 2 || code == 22 || code == 32) {
            swf_bs sb = { d, end, b.pos, 0 }; int id = d[b.pos] | (d[b.pos + 1] << 8);
            if (id < SWF_MAXCH) { swf_shape *s = &sh[id]; s->nfill = s->nline = s->nedge = 0; swf_define_shape(&sb, s, code == 2 ? 1 : code == 22 ? 2 : 3); }
        } else if (code == 26) {                   /* PlaceObject2 */
            swf_bs pb = { d, end, b.pos, 0 }; int fl = swf_u8(&pb), depth = swf_u16(&pb);
            if (depth < SWF_MAXDEPTH) {
                swf_place *p = &dl[depth];
                if (!(fl & 1)) { memset(p, 0, sizeof *p); }                 /* not a move: new placement */
                if (fl & 2) { int ch = swf_u16(&pb); if (!(fl & 1) || p->ch != ch) { p->m.a = p->m.d = 1; p->cx.m[0] = p->cx.m[1] = p->cx.m[2] = p->cx.m[3] = 1; } p->ch = ch; }
                if (!(fl & 1) && !(fl & 2)) p->ch = 0;
                if (fl & 4) p->m = swf_matrix(&pb);
                else if (!(fl & 1)) { p->m.a = p->m.d = 1; }
                if (fl & 8) p->cx = swf_cxform(&pb, 1);
                else if (!(fl & 1)) { p->cx.m[0] = p->cx.m[1] = p->cx.m[2] = p->cx.m[3] = 1; }
            }
        } else if (code == 4) {                    /* PlaceObject (Flash 1) */
            swf_bs pb = { d, end, b.pos, 0 }; int ch = swf_u16(&pb), depth = swf_u16(&pb);
            if (depth < SWF_MAXDEPTH) { swf_place *p = &dl[depth]; p->ch = ch; p->m = swf_matrix(&pb); p->cx.m[0] = p->cx.m[1] = p->cx.m[2] = p->cx.m[3] = 1;
                if (pb.pos < end) p->cx = swf_cxform(&pb, 0); }
        } else if (code == 28 || code == 5) {      /* RemoveObject2 (depth) / RemoveObject (id, depth) */
            int depth = code == 28 ? (d[b.pos] | (d[b.pos + 1] << 8)) : (d[b.pos + 2] | (d[b.pos + 3] << 8));
            if (depth < SWF_MAXDEPTH) memset(&dl[depth], 0, sizeof dl[depth]);
        } else if (code == 1) {                    /* ShowFrame */
            int k;
            if (fr == cap) { unsigned char *np = (unsigned char *)realloc(mv->pix, (size_t)cap * 2 * mv->w * mv->h * 4); if (!np) break; mv->pix = np; cap *= 2; }
            G.CGContextSaveGState(cg);
            {   swf_rc r = { 0, 0, mv->w, mv->h };
                if (transparent) G.CGContextClearRect(cg, r);
                else { G.CGContextSetRGBFillColor(cg, mv->bg[0] / 255.0, mv->bg[1] / 255.0, mv->bg[2] / 255.0, 1); G.CGContextFillRect(cg, r); } }
            /* SWF y grows down; CG's bitmap origin is at the bottom left */
            G.CGContextTranslateCTM(cg, 0, mv->h); G.CGContextScaleCTM(cg, scale / 20.0, -scale / 20.0);
            G.CGContextTranslateCTM(cg, -x0, -y0);
            for (k = 1; k < SWF_MAXDEPTH; k++)
                if (dl[k].ch > 0 && dl[k].ch < SWF_MAXCH && sh[dl[k].ch].used) swf_draw_shape(cg, &sh[dl[k].ch], &dl[k].m, &dl[k].cx);
            G.CGContextRestoreGState(cg);
            G.CGContextFlush(cg);
            memcpy(mv->pix + (size_t)fr * mv->w * mv->h * 4, buf, (size_t)mv->w * mv->h * 4);
            fr++;
        }
        b.pos = end; b.bit = 0;
    }
    mv->frames = fr;
    { int k; for (k = 0; k < SWF_MAXCH; k++) { free(sh[k].fills); free(sh[k].lines); free(sh[k].edges); } }
    free(sh); free(dl); free(buf);
    G.CGContextRelease(cg); G.CGColorSpaceRelease(cs);
    return fr > 0 ? 0 : -4;
}
