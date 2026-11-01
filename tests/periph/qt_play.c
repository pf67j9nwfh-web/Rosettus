/* qt_play: PowerPC test - QuickTime movie playback with sound, one movie after another (as a
   game's intro does; SimCity 4 plays several). Each movie plays into an offscreen GWorld for up to QT_PLAY_SECS seconds
   (default 8) with MoviesTask; the test samples the frame every 100 ms and counts distinct
   frames, then stops and disposes the movie and opens the next. Sound plays at full movie volume
   (the system output volume is whatever the user set). Prints RESULT lines. QT_PLAY_MARK=1 marks each step on stderr (to line up with OAH_AUTRACE).
   Build: gcc -arch ppc -o qt_play qt_play.c -framework QuickTime -framework Carbon */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h>
#define MARK(...) do { if (getenv("QT_PLAY_MARK")) { fprintf(stderr, "[qt_play] " __VA_ARGS__); fputc(10, stderr); } } while (0)

static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }
static unsigned long frame_sum(GWorldPtr gw, int w, int h, int *uniform) {
    PixMapHandle pm = GetGWorldPixMap(gw);
    unsigned char *base; long rb; int x, y; unsigned long s = 5381; unsigned int first = 0, diff = 0;
    LockPixels(pm);
    base = (unsigned char *)GetPixBaseAddr(pm); rb = GetPixRowBytes(pm);
    for (y = 0; y < h; y += 4) for (x = 0; x < w; x += 4) {
        unsigned int v = *(unsigned int *)(base + y * rb + x * 4) & 0x00ffffff;
        if (!x && !y) first = v; else if (v != first) diff++;
        s = s * 33 + v;
    }
    UnlockPixels(pm);
    *uniform = diff == 0;
    return s;
}
int main(int argc, char **argv) {
    int i, ok = 1, secs = getenv("QT_PLAY_SECS") ? atoi(getenv("QT_PLAY_SECS")) : 8;
    EnterMovies();
    for (i = 1; i < argc; i++) {
        FSRef ref; FSSpec spec; short rf = -1, rid = 0; Movie m = NULL; OSErr e;
        const char *nm = strrchr(argv[i], '/'); nm = nm ? nm + 1 : argv[i];
        Rect box = { 0, 0, 240, 320 }; GWorldPtr gw = NULL;
        unsigned long sums[64]; int nsums = 0, k, frames = 0, nonuni = 0, hassound = 0, t;
        double t0, dur, played;
        MARK("open %s", nm);
        e = FSPathMakeRef((const UInt8 *)argv[i], &ref, NULL);
        if (!e) e = FSGetCatalogInfo(&ref, kFSCatInfoNone, NULL, NULL, &spec, NULL);
        if (!e) e = OpenMovieFile(&spec, &rf, fsRdPerm);
        if (!e) e = NewMovieFromFile(&m, rf, &rid, NULL, newMovieActive, NULL);
        if (e || !m) { printf("RESULT play %s FAIL (open err %d)\n", nm, e); ok = 0; continue; }
        for (t = 1; t <= GetMovieTrackCount(m); t++) {
            OSType mt; GetMediaHandlerDescription(GetTrackMedia(GetMovieIndTrack(m, t)), &mt, NULL, NULL);
            if (mt == SoundMediaType) hassound = 1;
        }
        NewGWorld(&gw, 32, &box, NULL, NULL, 0);
        SetMovieGWorld(m, (CGrafPtr)gw, NULL);
        SetMovieBox(m, &box);
        SetMovieVolume(m, kFullVolume);
        dur = (double)GetMovieDuration(m) / GetMovieTimeScale(m);
        GoToBeginningOfMovie(m);
        MARK("preroll"); PrerollMovie(m, 0, fixed1);
        MARK("start"); StartMovie(m);
        t0 = now();
        while (!IsMovieDone(m) && now() - t0 < secs) {
            int uni; unsigned long s;
            MoviesTask(m, 0);
            usleep(10000);
            if ((int)((now() - t0) * 10) > frames) {
                frames++;
                s = frame_sum(gw, 320, 240, &uni);
                if (!uni) nonuni++;
                for (k = 0; k < nsums && sums[k] != s; k++) ;
                if (k == nsums && nsums < 64) sums[nsums++] = s;
            }
        }
        played = (double)GetMovieTime(m, NULL) / GetMovieTimeScale(m);
        MARK("loop ended (done %d)", (int)IsMovieDone(m)); StopMovie(m); MARK("stopped");
        printf("%s: duration %.1f s, played %.1f s in %.1f s wall, sound %s, %d samples, %d with a picture, %d distinct\n",
               nm, dur, played, now() - t0, hassound ? "yes" : "no", frames, nonuni, nsums);
        {   int good = nonuni > 0 && nsums > 1 && played > 0.5 * (dur < secs ? dur : secs) - 0.5;
            printf("RESULT play %s %s\n", nm, good ? "PASS" : "FAIL"); if (!good) ok = 0; }
        fflush(stdout);
        DisposeMovie(m); MARK("disposed"); CloseMovieFile(rf); DisposeGWorld(gw); MARK("closed");
    }
    ExitMovies();
    printf("RESULT qt_play %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
