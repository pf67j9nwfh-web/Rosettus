/* hidpitest: can a non-AppKit client show 2x pixels through a CGS surface + CGL? (Mojave) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
typedef unsigned int U;
typedef struct { float x, y, w, h; } R4;
#define S(t, n, a) t (*n) a = (t (*) a)dlsym(RTLD_DEFAULT, #n)
int main(int argc, char **argv) {
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW | RTLD_GLOBAL);
    S(U, CGSMainConnectionID, (void));
    S(int, CGSNewRegionWithRect, (const R4 *, void **));
    S(int, CGSNewWindow, (U, int, float, float, void *, U *));
    S(int, CGSSetWindowLevel, (U, U, int));
    S(int, CGSOrderWindow, (U, U, int, U));
    S(int, CGSAddSurface, (U, U, U *));
    S(int, CGSSetSurfaceBounds, (U, U, U, R4));
    S(int, CGSOrderSurface, (U, U, U, int, U));
    S(int, CGSSetSurfaceResolution, (U, U, U, double));
    S(int, CGLSetSurface, (CGLContextObj, U, U, U));
    U cid, wid = 0, sid = 0; void *rgn = 0; int err, W = 400, H = 300, k;
    double res = argc > 1 ? atof(argv[1]) : 2.0;
    R4 r = { 0, 0, 400, 300 };
    (void)cg;
    if (!CGSMainConnectionID || !CGLSetSurface || !CGSSetSurfaceResolution) { printf("missing syms\n"); return 1; }
    cid = CGSMainConnectionID();
    CGSNewRegionWithRect(&r, &rgn);
    err = CGSNewWindow(cid, 2, 300, 200, rgn, &wid); printf("newwin %d wid %u\n", err, wid);
    CGSSetWindowLevel(cid, wid, 20);
    CGSOrderWindow(cid, wid, 1, 0);
    err = CGSAddSurface(cid, wid, &sid); printf("addsurf %d sid %u\n", err, sid);
    CGSSetSurfaceBounds(cid, wid, sid, r);
    if (res > 0) { err = CGSSetSurfaceResolution(cid, wid, sid, res); printf("setres %g -> %d\n", res, err); }
    CGSOrderSurface(cid, wid, sid, 1, 0);
    {
        CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFADoubleBuffer, kCGLPFAColorSize, 24, 0 };
        CGLPixelFormatObj pf; GLint np; CGLContextObj ctx; GLint dims[4]; unsigned int *px; GLuint tex;
        CGLChoosePixelFormat(at, &pf, &np);
        CGLCreateContext(pf, 0, &ctx);
        err = CGLSetSurface(ctx, cid, wid, sid); printf("setsurface %d\n", err);
        CGLSetCurrentContext(ctx);
        if (getenv("BS")) { GLint bs[2] = { 800, 600 }; int e1 = CGLSetParameter(ctx, 304, bs), e2 = CGLEnable(ctx, 305); printf("backing size %d %d\n", e1, e2); }
        CGLUpdateContext(ctx);
        glGetIntegerv(GL_VIEWPORT, dims); printf("viewport %d %d %d %d\n", dims[0], dims[1], dims[2], dims[3]);
        /* 800x600 texture: 1-pixel vertical black/white stripes */
        px = malloc(800 * 600 * 4);
        for (k = 0; k < 800 * 600; k++) px[k] = (k % 800) & 1 ? 0xffffffff : 0xff000000;
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 800, 600, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        if (getenv("BS")) glViewport(0, 0, 800, 600); else glViewport(0, 0, dims[2], dims[3]);
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 1); glVertex2f(-1, -1); glTexCoord2f(1, 1); glVertex2f(1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, 1); glTexCoord2f(0, 0); glVertex2f(-1, 1);
        glEnd();
        CGLFlushDrawable(ctx);
        printf("drawn; sleeping\n"); fflush(stdout);
        sleep(argc > 2 ? atoi(argv[2]) : 6);
    }
    (void)W; (void)H;
    return 0;
}
