/* swftest.c - render SWF files with oah_swf.h, write a contact sheet PNG of up to 8 frames each */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ApplicationServices/ApplicationServices.h>
#include "../src/liboah/oah_swf.h"
int main(int argc, char **argv) {
    int i, rows = argc - 2, cw = 0, ch = 0;
    swf_movie mv[64]; size_t k;
    if (argc < 3) return 1;
    for (i = 0; i < rows && i < 64; i++) {
        FILE *f = fopen(argv[i + 2], "rb"); long len; unsigned char *d; int e;
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET); d = malloc(len); fread(d, 1, len, f); fclose(f);
        e = swf_render(d, len, 2.0, 1, &mv[i]);
        printf("%s: err %d, %dx%d, %d frames, %.1f fps, bg %02x%02x%02x\n", argv[i + 2], e, mv[i].w, mv[i].h, mv[i].frames, mv[i].fps, mv[i].bg[0], mv[i].bg[1], mv[i].bg[2]);
        if (mv[i].w * 8 > cw) cw = mv[i].w * 8;
        ch += mv[i].h;
    }
    {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(0, cw, ch, 8, cw * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
        int y = ch;
        CGContextSetRGBFillColor(cg, 0.5, 0.5, 0.5, 1); CGContextFillRect(cg, CGRectMake(0, 0, cw, ch));
        for (i = 0; i < rows && i < 64; i++) {
            int fi;
            y -= mv[i].h;
            for (fi = 0; fi < 8 && mv[i].frames; fi++) {
                int src = fi * mv[i].frames / 8;
                CGDataProviderRef dp = CGDataProviderCreateWithData(0, mv[i].pix + (size_t)src * mv[i].w * mv[i].h * 4, (size_t)mv[i].w * mv[i].h * 4, 0);
                CGImageRef im = CGImageCreate(mv[i].w, mv[i].h, 8, 32, mv[i].w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big, dp, 0, 0, 0);
                CGContextDrawImage(cg, CGRectMake(fi * mv[i].w, y, mv[i].w, mv[i].h), im);
                CGImageRelease(im); CGDataProviderRelease(dp);
            }
        }
        {
            CGImageRef out = CGBitmapContextCreateImage(cg);
            CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)argv[1], strlen(argv[1]), false);
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, 0);
            CGImageDestinationAddImage(dst, out, 0); CGImageDestinationFinalize(dst);
        }
    }
    (void)k;
    return 0;
}
