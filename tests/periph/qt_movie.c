/* qt_movie: PowerPC test - QuickTime movie decoding. For each movie file given: lists its tracks
   (media type and codec), draws the frame at the middle of the movie into an offscreen GWorld and
   checks the frame is not uniform. The movie's sound is muted (nothing is played).
   Build: gcc -arch ppc -o qt_movie qt_movie.c -framework QuickTime -framework Carbon */
#include <stdio.h>
#include <string.h>
#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h>

static void fourcc(OSType t, char *b) { b[0] = t >> 24; b[1] = t >> 16; b[2] = t >> 8; b[3] = t; b[4] = 0; }
int main(int argc, char **argv) {
    int i, ok = 1;
    EnterMovies();
    for (i = 1; i < argc; i++) {
        FSRef ref; FSSpec spec; short rf = -1, rid = 0; Movie m = NULL; OSErr e;
        const char *nm = strrchr(argv[i], '/'); nm = nm ? nm + 1 : argv[i];
        char vcodec[5] = "-"; long t, nt; int distinct = 0;
        e = FSPathMakeRef((const UInt8 *)argv[i], &ref, NULL);
        if (!e) e = FSGetCatalogInfo(&ref, kFSCatInfoNone, NULL, NULL, &spec, NULL);
        if (!e) e = OpenMovieFile(&spec, &rf, fsRdPerm);
        if (!e) e = NewMovieFromFile(&m, rf, &rid, NULL, newMovieActive, NULL);
        if (e || !m) { printf("%s: open err %d\nRESULT movie %s FAIL (open)\n", nm, e, nm); ok = 0; continue; }
        nt = GetMovieTrackCount(m);
        for (t = 1; t <= nt; t++) {
            Track tr = GetMovieIndTrack(m, t); Media md = GetTrackMedia(tr);
            OSType mt; char a[5], c[5] = "-";
            SampleDescriptionHandle sd = (SampleDescriptionHandle)NewHandle(0);
            GetMediaHandlerDescription(md, &mt, NULL, NULL);
            GetMediaSampleDescription(md, 1, sd);
            if (GetHandleSize((Handle)sd) >= 8) fourcc((*sd)->dataFormat, c);
            fourcc(mt, a);
            printf("%s: track %ld %s %s\n", nm, t, a, c);
            if (mt == VideoMediaType) strcpy(vcodec, c);
            DisposeHandle((Handle)sd);
        }
        SetMovieVolume(m, kNoVolume);
        {   Rect b; GWorldPtr g = NULL;
            GetMovieBox(m, &b); OffsetRect(&b, -b.left, -b.top);
            if (b.right <= 0 || b.bottom <= 0) SetRect(&b, 0, 0, 160, 120);
            if (!NewGWorld(&g, 32, &b, NULL, NULL, 0)) {
                PixMapHandle pm = GetGWorldPixMap(g); int x, y; UInt32 first = 0;
                SetMovieGWorld(m, g, NULL); SetMovieBox(m, &b);
                SetMovieTimeValue(m, GetMovieDuration(m) / 2);
                UpdateMovie(m); MoviesTask(m, 0); MoviesTask(m, 0);
                LockPixels(pm);
                for (y = 0; y < b.bottom; y += 4) for (x = 0; x < b.right; x += 4) {
                    UInt32 v = ((UInt32 *)((char *)GetPixBaseAddr(pm) + y * (GetPixRowBytes(pm) & 0x3fff)))[x] & 0xffffff;
                    if (!x && !y) first = v; else if (v != first) distinct++;
                }
                UnlockPixels(pm);
                SetMovieGWorld(m, NULL, NULL);
                DisposeGWorld(g);
                printf("%s: %dx%d, duration %ld, %d sampled pixels differ, MoviesTask err %ld\n", nm, b.right, b.bottom,
                       (long)GetMovieDuration(m), distinct, (long)GetMoviesError());
            }
        }
        printf("RESULT movie %s %s (video %s)\n", nm, distinct > 20 ? "PASS" : "FAIL", vcodec);
        if (distinct <= 20) ok = 0;
        DisposeMovie(m); CloseMovieFile(rf);
    }
    return ok ? 0 : 1;
}
