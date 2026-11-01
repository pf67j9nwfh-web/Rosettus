/* draw_qd_cg: offscreen QuickDraw and Quartz drawing, checksummed.  No windows.
   Build (10.6): gcc -arch ppc -o draw_qd_cg draw_qd_cg.c -framework ApplicationServices -framework Carbon */
#include <stdio.h>
#include <string.h>
#include <Carbon/Carbon.h>

static unsigned long sum(const unsigned char *p, long rb, int w, int h, int bpp) {
    unsigned long s = 5381; int y, x;
    for (y = 0; y < h; y++) for (x = 0; x < w * bpp; x++) s = s * 33 + p[y * rb + x];
    return s;
}
int main(void) {
    {   /* QuickDraw GWorld */
        GWorldPtr gw = 0; Rect r = { 0, 0, 64, 64 }; CGrafPtr oldp; GDHandle oldd; OSErr e;
        e = NewGWorld(&gw, 32, &r, 0, 0, 0); printf("%-22s %d\n", "NewGWorld", (int)e);
        if (gw) {
            PixMapHandle pm = GetGWorldPixMap(gw); LockPixels(pm);
            GetGWorld(&oldp, &oldd); SetGWorld(gw, 0);
            EraseRect(&r);
            { RGBColor c = { 0xffff, 0, 0 }; RGBForeColor(&c); }
            { Rect a = { 8, 8, 30, 40 }; PaintRect(&a); }
            { RGBColor c = { 0, 0, 0xffff }; RGBForeColor(&c); }
            { Rect o = { 20, 20, 60, 56 }; PenSize(2, 2); FrameOval(&o); }
            MoveTo(2, 62); LineTo(62, 2);
            TextFont(kFontIDGeneva); TextSize(9); MoveTo(4, 50); DrawString("\pQD!");
            SetGWorld(oldp, oldd);
            { Ptr base = GetPixBaseAddr(pm); long rb = GetPixRowBytes(pm);
              printf("%-22s rb %ld sum %08lx px(10,10) %08lx\n", "GWorld", rb > 0 ? 1L : 0L, sum((unsigned char *)base, rb, 64, 64, 4),
                     (unsigned long)*(UInt32 *)(base + 10 * rb + 40)); }
            { RGBColor c; SetGWorld(gw, 0); GetCPixel(10, 10, &c); SetGWorld(oldp, oldd); printf("%-22s %04x %04x %04x\n", "GetCPixel", c.red, c.green, c.blue); }
            { Rect rr = { 0, 0, 10, 10 }; RgnHandle rg = NewRgn(); RectRgn(rg, &rr); { Rect b2 = { 5, 5, 20, 20 }; RgnHandle r2 = NewRgn(); RectRgn(r2, &b2); UnionRgn(rg, r2, rg); }
              GetRegionBounds(rg, &rr); printf("%-22s %d,%d,%d,%d rect %d\n", "region", rr.top, rr.left, rr.bottom, rr.right, IsRegionRectangular(rg)); }
            UnlockPixels(pm); DisposeGWorld(gw);
        }
    }
    {   /* Quartz bitmap */
        unsigned char *buf = calloc(64 * 64, 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(buf, 64, 64, 8, 256, cs, kCGImageAlphaPremultipliedLast);
        printf("%-22s %d\n", "CGBitmapContext", c != 0);
        CGContextSetRGBFillColor(c, 0, 0.5, 1, 1); CGContextFillRect(c, CGRectMake(4, 4, 30, 20));
        CGContextSetRGBStrokeColor(c, 1, 0, 0, 1); CGContextSetLineWidth(c, 3);
        CGContextStrokeEllipseInRect(c, CGRectMake(20, 20, 40, 36));
        CGContextBeginPath(c); CGContextMoveToPoint(c, 0, 0); CGContextAddCurveToPoint(c, 10, 60, 50, 0, 64, 64); CGContextStrokePath(c);
        { CGAffineTransform t = CGContextGetCTM(c); printf("%-22s %g %g %g %g\n", "ctm", t.a, t.d, t.tx, t.ty); }
        CGContextSetShouldAntialias(c, false);
        CGContextSetRGBFillColor(c, 0, 0, 0, 1);
        CGContextSelectFont(c, "Helvetica", 12, kCGEncodingMacRoman);
        CGContextShowTextAtPoint(c, 6, 40, "CG", 2);
        { CGPoint p = CGContextGetTextPosition(c); printf("%-22s %.2f %.2f\n", "text-advance", p.x, p.y); }
        CGContextFlush(c);
        printf("%-22s %08lx px(10,10) %02x%02x%02x%02x\n", "CG sum", sum(buf, 256, 64, 64, 4), buf[(63 - 10) * 256 + 40], buf[(63 - 10) * 256 + 41], buf[(63 - 10) * 256 + 42], buf[(63 - 10) * 256 + 43]);
        {   CGImageRef im = CGBitmapContextCreateImage(c);
            printf("%-22s %zu x %zu bpc %zu bpp %zu info %x\n", "CGImage", CGImageGetWidth(im), CGImageGetHeight(im), CGImageGetBitsPerComponent(im), CGImageGetBitsPerPixel(im), (unsigned)CGImageGetBitmapInfo(im)); }
    }
    {   /* geometry helpers */
        CGRect a = CGRectMake(0, 0, 10, 10), b = CGRectMake(5, -5, 10, 10), u = CGRectUnion(a, b), i = CGRectIntersection(a, b);
        printf("%-22s %g %g %g %g / %g %g %g %g\n", "rects", u.origin.x, u.origin.y, u.size.width, u.size.height, i.origin.x, i.origin.y, i.size.width, i.size.height);
        { CGAffineTransform t = CGAffineTransformRotate(CGAffineTransformMakeScale(2, 3), 0.5); CGPoint p = CGPointApplyAffineTransform(CGPointMake(1, 1), t);
          printf("%-22s %.6f %.6f\n", "affine", p.x, p.y); }
    }
    {   /* ATSUI text measurement */
        ATSUStyle st; ATSUTextLayout tl; UniChar txt[] = { 'R', 'o', 's', 'e', 't', 't', 'a' };
        ATSUFontID fid = 0; Fixed sz = Long2Fix(18);
        ATSUFindFontFromName("Helvetica", 9, kFontPostscriptName, kFontNoPlatformCode, kFontNoScriptCode, kFontNoLanguageCode, &fid);
        ATSUCreateStyle(&st);
        { ATSUAttributeTag tg[2] = { kATSUFontTag, kATSUSizeTag }; ByteCount bs[2] = { sizeof fid, sizeof sz }; ATSUAttributeValuePtr v[2] = { &fid, &sz };
          ATSUSetAttributes(st, 2, tg, bs, v); }
        ATSUCreateTextLayoutWithTextPtr(txt, 0, 7, 7, 1, (UniCharCount[]){ 7 }, &st, &tl);
        { ATSUTextMeasurement a, b, asc, desc; OSStatus e = ATSUGetUnjustifiedBounds(tl, 0, 7, &a, &b, &asc, &desc);
          printf("%-22s %d found %d width %.2f asc %.2f desc %.2f\n", "ATSU bounds", (int)e, fid != 0, Fix2X(b - a), Fix2X(asc), Fix2X(desc)); }
    }
    return 0;
}
