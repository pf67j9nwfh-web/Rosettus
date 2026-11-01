/* qtflash.c (PowerPC test): is a Flash media handler / importer registered, and does a .swf
   open and draw? usage: qtflash file.swf [frames] */
#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h>
#include <stdio.h>
static void comp(OSType t, OSType s) {
    ComponentDescription d = { t, s, 0, 0, 0 };
    Component c = FindNextComponent(0, &d);
    printf("component '%.4s' '%.4s': %s (count %ld)\n", (char *)&t, (char *)&s, c ? "found" : "missing", CountComponents(&d));
    while (c) {
        ComponentDescription cd; Handle nm = NewHandle(0);
        GetComponentInfo(c, &cd, nm, 0, 0);
        printf("   name \"%.*s\" version %08lx flags %08lx\n", nm && *nm && GetHandleSize(nm) ? (int)(unsigned char)(*nm)[0] : 0,
               nm && *nm && GetHandleSize(nm) ? *nm + 1 : "", GetComponentVersion((ComponentInstance)c), cd.componentFlags);
        DisposeHandle(nm);
        c = FindNextComponent(c, &d);
    }
}
int main(int argc, char **argv) {
    FSRef ref; FSSpec spec; short rf = 0; Movie m = 0; OSErr e;
    int frames = argc > 2 ? atoi(argv[2]) : 10, i;
    e = EnterMovies(); printf("EnterMovies %d\n", e);
    if (getenv("QTF_REG")) {   /* register a component bundle explicitly */
        FSRef br; FSSpec bs;
        if (!FSPathMakeRef((UInt8 *)getenv("QTF_REG"), &br, 0)) {
            CFURLRef u = CFURLCreateFromFSRef(0, &br);
            CFBundleRef b = u ? CFBundleCreate(0, u) : 0;
            short refn;
            printf("bundle %p\n", b);
            if (getenv("QTF_FILEREF")) printf("RegisterComponentFileRef %d\n", RegisterComponentFileRef(&br, 0));
            else if (b) {
                refn = CFBundleOpenBundleResourceMap(b);
                printf("resource map %d\n", refn);
                if (refn > 0) {
                    UseResFile(refn);
                    printf("RegisterComponentResourceFile %ld\n", (long)RegisterComponentResourceFile(refn, 0));
                    printf("thng count %d\n", Count1Resources('thng'));
                }
            }
            (void)bs;
        }
    }
    comp('mhlr', 'flsh'); comp('eat ', 'SWFL');
    {   /* every 'eat ' whose subtype looks like Flash, including missing/hidden ones */
        ComponentDescription d = { 'eat ', 0, 0, 0, 0 }, cd;
        Component c = 0;
        while ((c = FindNextComponent(c, &d))) {
            Handle nm = NewHandle(0);
            GetComponentInfo(c, &cd, nm, 0, 0);
            if (cd.componentSubType == 'SWFL' || cd.componentSubType == 'swf ' || cd.componentSubType == 'SWF ') {
                ComponentInstance ci = 0;
                OSErr oe = OpenAComponent(c, &ci);
                printf("eat '%.4s' '%.4s' flags %08lx: OpenAComponent %d inst %p version %08lx\n", (char *)&cd.componentSubType,
                       (char *)&cd.componentManufacturer, cd.componentFlags, oe, ci, ci ? GetComponentVersion(ci) : 0);
                if (ci) CloseComponent(ci);
            }
            DisposeHandle(nm);
        }
    }
    if (argc < 2) return 0;
    if (FSPathMakeRef((UInt8 *)argv[1], &ref, 0) || FSGetCatalogInfo(&ref, 0, 0, 0, &spec, 0)) { printf("no file\n"); return 1; }
    e = OpenMovieFile(&spec, &rf, fsRdPerm); printf("OpenMovieFile %d\n", e);
    if (!e) { short id = 0; e = NewMovieFromFile(&m, rf, &id, 0, newMovieActive, 0); printf("NewMovieFromFile %d movie %p\n", e, m); }
    if (m) {
        Rect r; GWorldPtr gw; long n, k, x, y;
        Track tr = GetMovieIndTrack(m, 1);
        Media md = tr ? GetTrackMedia(tr) : 0;
        OSType mt = 0;
        if (md) GetMediaHandlerDescription(md, &mt, 0, 0);
        GetMovieBox(m, &r);
        printf("tracks %ld, first media '%.4s', box %d,%d,%d,%d, duration %ld/%ld\n", GetMovieTrackCount(m), (char *)&mt,
               r.left, r.top, r.right, r.bottom, (long)GetMovieDuration(m), (long)GetMovieTimeScale(m));
        if (r.right <= r.left) SetRect(&r, 0, 0, 200, 200);
        NewGWorld(&gw, 32, &r, 0, 0, 0);
        SetMovieGWorld(m, gw, 0);
        for (i = 0; i < frames; i++) {
            PixMapHandle pm = GetGWorldPixMap(gw);
            unsigned char *p; long rb;
            LockPixels(pm);
            p = (unsigned char *)GetPixBaseAddr(pm); rb = GetPixRowBytes(pm) & 0x3fff;
            memset(p, 0xff, rb * (r.bottom - r.top));
            SetMovieTimeValue(m, GetMovieDuration(m) * i / (frames > 1 ? frames - 1 : 1));
            UpdateMovie(m); MoviesTask(m, 0);
            n = 0;
            for (y = 0; y < r.bottom - r.top; y++) for (x = 0; x < r.right - r.left; x++) {
                k = y * rb + 4 * x;
                if (p[k + 1] != 0xff || p[k + 2] != 0xff || p[k + 3] != 0xff) n++;
            }
            {   /* /tmp/qtf_N.bmp (24-bit, bottom-up) */
                char fn[64]; FILE *f; long w = r.right - r.left, h = r.bottom - r.top, rowb = (w * 3 + 3) & ~3;
                unsigned char hd[54] = { 'B', 'M' };
                long sz = 54 + rowb * h;
                #define P4(o, v) do { hd[o] = (v) & 255; hd[o + 1] = ((v) >> 8) & 255; hd[o + 2] = ((v) >> 16) & 255; hd[o + 3] = ((v) >> 24) & 255; } while (0)
                P4(2, sz); P4(10, 54); P4(14, 40); P4(18, w); P4(22, h); hd[26] = 1; hd[28] = 24;
                snprintf(fn, sizeof fn, "/tmp/qtf_%d.bmp", i);
                if ((f = fopen(fn, "wb"))) {
                    unsigned char row[4096 * 3];
                    fwrite(hd, 1, 54, f);
                    for (y = h - 1; y >= 0; y--) {
                        memset(row, 0, rowb);
                        for (x = 0; x < w && x < 4096; x++) { k = y * rb + 4 * x; row[3 * x] = p[k + 3]; row[3 * x + 1] = p[k + 2]; row[3 * x + 2] = p[k + 1]; }
                        fwrite(row, 1, rowb, f);
                    }
                    fclose(f);
                }
            }
            UnlockPixels(pm);
            printf("frame %d: %ld non-white pixels\n", i, n);
        }
    }
    return 0;
}
