/* webcam: PowerPC peripheral test - video capture through the QuickTime Sequence Grabber.
   Lists the video devices and inputs, grabs one frame (SGGrabPict) and checks that it looks like a
   camera picture (varied, and smooth between neighbouring pixels: not black, not noise), then
   records 2 seconds to a movie (SGStartRecord, as capture apps do) and counts its video frames.
   On macOS 10.14 the camera asks for permission, for the app that runs the test: run it from an
   app bundle whose Info.plist has NSCameraUsageDescription (periph.sh says how).
   Build: gcc -arch ppc -o webcam webcam.c -framework QuickTime -framework Carbon */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h>

int main(void) {
    SeqGrabComponent sg; SGChannel vc = 0; OSErr e; ComponentResult r;
    SGDeviceList dl = NULL; int ndev = 0, ok = 1;
    EnterMovies();
    sg = OpenDefaultComponent(SeqGrabComponentType, 0);
    if (!sg) { printf("RESULT seqgrab FAIL (no sequence grabber)\n"); return 1; }
    r = SGInitialize(sg);
    printf("SGInitialize -> %ld\n", (long)r);
    {   Rect b = { 0, 0, 240, 320 }; GWorldPtr gw = NULL;
        NewGWorld(&gw, 32, &b, NULL, NULL, 0);
        SGSetGWorld(sg, gw, NULL);
    }
    r = SGNewChannel(sg, VideoMediaType, &vc);
    printf("SGNewChannel(video) -> %ld\n", (long)r);
    if (r || !vc) { printf("RESULT webcam_channel FAIL\n"); return 1; }
    if (!SGGetChannelDeviceList(vc, sgDeviceListIncludeInputs, &dl) && dl) {
        int i;
        for (i = 0; i < (*dl)->count; i++) {
            SGDeviceName *d = &(*dl)->entry[i];
            printf("video device %d \"%.*s\"%s\n", i, d->name[0], (char *)d->name + 1, (d->flags & sgDeviceNameFlagDeviceUnavailable) ? " (unavailable)" : "");
            if (d->inputs) {
                int k;
                for (k = 0; k < (*d->inputs)->count; k++)
                    printf("   input \"%.*s\"\n", (*d->inputs)->entry[k].name[0], (char *)(*d->inputs)->entry[k].name + 1);
            }
            if (!(d->flags & sgDeviceNameFlagDeviceUnavailable)) ndev++;
        }
        SGDisposeDeviceList(sg, dl);
    }
    printf("RESULT webcam_devices %s (%d available)\n", ndev ? "PASS" : "FAIL", ndev);
    if (!ndev) ok = 0;
    {   Rect b = { 0, 0, 240, 320 }; PicHandle pic = NULL;
        SGSetChannelBounds(vc, &b);
        SGSetChannelUsage(vc, seqGrabRecord);
        e = SGGrabPict(sg, &pic, &b, 0, grabPictOffScreen);
        if (!e && pic) {
            Rect pf = (*pic)->picFrame; GWorldPtr g = NULL; long distinct = 0; double smooth = 1.0;
            if (!NewGWorld(&g, 32, &b, NULL, NULL, 0)) {
                CGrafPtr op; GDHandle od; PixMapHandle pm; int y, x; UInt32 first = 0;
                GetGWorld(&op, &od); SetGWorld(g, NULL);
                pm = GetGWorldPixMap(g); LockPixels(pm);
                DrawPicture(pic, &b);
                for (y = 0; y < 240; y += 8) for (x = 0; x < 320; x += 8) {
                    UInt32 v = ((UInt32 *)((char *)GetPixBaseAddr(pm) + y * (GetPixRowBytes(pm) & 0x3fff)))[x] & 0xffffff;
                    if (!y && !x) first = v; else if (v != first) distinct++;
                }
                {   /* green channel: mean difference of horizontal neighbours against pixels 97 apart */
                    double dn = 0, dr = 0; long n = 0;
                    for (y = 0; y < 240; y += 4) for (x = 0; x + 98 < 320; x += 3) {
                        UInt8 *row = (UInt8 *)GetPixBaseAddr(pm) + y * (GetPixRowBytes(pm) & 0x3fff);
                        int a = row[x * 4 + 2], b = row[(x + 1) * 4 + 2], c = row[(x + 97) * 4 + 2];
                        dn += abs(a - b); dr += abs(a - c); n++;
                    }
                    smooth = dr > 0 ? dn / dr : 1.0;
                }
                UnlockPixels(pm); SetGWorld(op, od); DisposeGWorld(g);
            }
            printf("grabbed frame %dx%d, %ld sampled pixels differ from the first, neighbour/random difference %.2f\n", pf.right - pf.left, pf.bottom - pf.top, distinct, smooth);
            printf("RESULT webcam_frame %s\n", distinct > 10 && smooth < 0.8 ? "PASS" : (distinct <= 10 ? "FAIL (uniform frame)" : "FAIL (noise)"));
            if (distinct <= 10 || smooth >= 0.8) ok = 0;
            KillPicture(pic);
        } else { printf("SGGrabPict -> %d\nRESULT webcam_frame FAIL\n", e); ok = 0; }
    }
    {   /* record 2 seconds to a movie file and count its frames */
        Handle dr = NULL; OSType drt = 0; Movie m = NULL; short rid = 0; long frames = 0; TimeValue dur = 0; TimeScale ts = 1;
        const char *path = "/tmp/webcam_test.mov";
        unlink(path);
        e = QTNewDataReferenceFromFullPathCFString(CFSTR("/tmp/webcam_test.mov"), kQTPOSIXPathStyle, 0, &dr, &drt);
        if (!e) e = SGSetDataRef(sg, dr, drt, seqGrabToDisk);
        if (!e) e = SGSetChannelUsage(vc, seqGrabRecord);
        if (!e) e = SGStartRecord(sg);
        if (!e) {
            UInt32 t0 = TickCount();
            while (TickCount() - t0 < 120 && !e) e = SGIdle(sg);
            SGStop(sg);
        }
        if (!e && !NewMovieFromDataRef(&m, newMovieActive, &rid, dr, drt) && m) {
            Track t = GetMovieIndTrackType(m, 1, VideoMediaType, movieTrackMediaType);
            if (t) { Media md = GetTrackMedia(t); frames = GetMediaSampleCount(md); dur = GetMediaDuration(md); ts = GetMediaTimeScale(md); }
            DisposeMovie(m);
        }
        printf("recorded: error %d, %ld video frames in %.2f s\n", e, frames, ts ? (double)dur / ts : 0.0);
        printf("RESULT webcam_record %s (%ld frames)\n", !e && frames >= 10 ? "PASS" : "FAIL", frames);
        if (e || frames < 10) ok = 0;
        if (dr) DisposeHandle(dr);
        unlink(path);
    }
    SGDisposeChannel(sg, vc);
    CloseComponent(sg);
    return ok ? 0 : 1;
}
