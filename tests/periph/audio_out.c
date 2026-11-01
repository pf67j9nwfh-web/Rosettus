/* audio_out: PowerPC peripheral test - sound output.
   1. CoreAudio HAL: default output device, its name and nominal sample rate.
   2. AudioUnit default output: a quiet 660 Hz tone for 0.6 s; counts render callbacks and frames.
   3. Sound Manager: SndPlay of a synthesized 0.3 s 8-bit sampled sound; checks the channel finishes.
   Build: gcc -arch ppc -o audio_out audio_out.c -framework CoreAudio -framework AudioUnit -framework Carbon */
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#ifdef __ppc__
#include <Carbon/Carbon.h>      /* the Sound Manager: PowerPC only (10.14 has no CarbonSound headers) */
#else
#include <CoreServices/CoreServices.h>
#endif
#include <AudioToolbox/AudioToolbox.h>

static volatile long g_calls, g_frames;
static double g_phase;
static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    UInt32 i, b;
    for (b = 0; b < io->mNumberBuffers; b++) {
        float *p = (float *)io->mBuffers[b].mData;
        UInt32 ch = io->mBuffers[b].mNumberChannels, k;
        double ph = g_phase;
        for (i = 0; i < n; i++) { float v = 0.03f * (float)sin(ph); ph += 2 * M_PI * 660.0 / 44100.0; for (k = 0; k < ch; k++) p[i * ch + k] = v; }
        if (b == io->mNumberBuffers - 1) g_phase = ph;
    }
    g_calls++; g_frames += n;
    return 0;
}
static volatile long g_aqbufs;
static void aqout(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    SInt16 *p = (SInt16 *)b->mAudioData; UInt32 i, n = b->mAudioDataBytesCapacity / 4;
    for (i = 0; i < n; i++) { SInt16 v = (SInt16)(900 * sin(g_phase)); g_phase += 2 * M_PI * 550.0 / 44100.0; p[2 * i] = p[2 * i + 1] = v; }
    b->mAudioDataByteSize = n * 4;
    g_aqbufs++;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
int main(void) {
    AudioDeviceID dev = 0; UInt32 sz = sizeof dev; OSStatus e;
    char name[256] = ""; Float64 rate = 0;
    int ok = 1;
    e = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &sz, &dev);
    printf("default output device %s (err %d)\n", dev ? "found" : "none", (int)e);
    if (dev) {
        sz = sizeof name; AudioDeviceGetProperty(dev, 0, false, kAudioDevicePropertyDeviceName, &sz, name);
        sz = sizeof rate; AudioDeviceGetProperty(dev, 0, false, kAudioDevicePropertyNominalSampleRate, &sz, &rate);
        printf("output device name \"%s\" rate %.0f\n", name, rate);
    } else ok = 0;
    printf("RESULT outdevice %s\n", dev && name[0] && rate > 0 ? "PASS" : "FAIL");
    {   /* AudioUnit */
        ComponentDescription cd = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        Component c = FindNextComponent(NULL, &cd);
        AudioUnit au = 0;
        AURenderCallbackStruct cb = { render, NULL };
        AudioStreamBasicDescription f = { 44100, kAudioFormatLinearPCM,
            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian, 8, 1, 8, 2, 32, 0 };
        if (!c || OpenAComponent(c, (ComponentInstance *)&au)) { printf("RESULT audiounit FAIL (no default output unit)\n"); ok = 0; }
        else {
            AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
            AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
            e = AudioUnitInitialize(au);
            if (!e) e = AudioOutputUnitStart(au);
            usleep(600000);
            AudioOutputUnitStop(au);
            AudioUnitUninitialize(au);
            CloseComponent(au);
            printf("audiounit start err %d, %ld callbacks, %ld frames (expect about 26460)\n", (int)e, g_calls, g_frames);
            printf("RESULT audiounit %s\n", (!e && g_calls > 5 && g_frames > 13000) ? "PASS" : "FAIL");
            if (e || g_calls <= 5) ok = 0;
        }
    }
    {   /* AudioQueue output: 0.5 s, buffers of 4410 frames */
        AudioStreamBasicDescription f = { 44100, kAudioFormatLinearPCM,
            kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked | kAudioFormatFlagsNativeEndian, 4, 1, 4, 2, 16, 0 };
        AudioQueueRef q = NULL; int i;
        e = AudioQueueNewOutput(&f, aqout, NULL, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &q);
        for (i = 0; i < 3 && !e; i++) { AudioQueueBufferRef b; if (!AudioQueueAllocateBuffer(q, 17640, &b)) aqout(NULL, q, b); }
        g_aqbufs = 0;
        if (!e) e = AudioQueueStart(q, NULL);
        if (!e) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        if (q) { AudioQueueStop(q, true); AudioQueueDispose(q, true); }
        printf("audioqueue output err %d, %ld buffers refilled in 0.5 s (expect about 5)\n", (int)e, g_aqbufs);
        printf("RESULT audioqueue_out %s\n", (!e && g_aqbufs >= 2) ? "PASS" : "FAIL");
        if (e || g_aqbufs < 2) ok = 0;
    }
#ifdef __ppc__   /* the Sound Manager (the native comparison build has no Sound Manager headers) */
    {   /* Sound Manager: a format 1 'snd ' with one bufferCmd and a standard sound header */
        long n = 6600, i;
        Handle h = NewHandleClear(20 + sizeof(SoundHeader) + n + 64);
        SndChannelPtr chan = NULL;
        SCStatus st; OSErr se; int done = 0;
        unsigned char *p = (unsigned char *)*h;
        SoundHeaderPtr sh;
        /* format 1: 1 synth (sampledSynth, initMono), 1 command (bufferCmd with the data-offset flag) */
        p[0] = 0; p[1] = 1; p[2] = 0; p[3] = 1; p[4] = 0; p[5] = 5; p[6] = 0; p[7] = 0; p[8] = 0; p[9] = 0x80;
        p[10] = 0; p[11] = 1;
        p[12] = 0x80; p[13] = 0x51; p[14] = 0; p[15] = 0; p[16] = 0; p[17] = 0; p[18] = 0; p[19] = 20;
        sh = (SoundHeaderPtr)(p + 20);
        sh->samplePtr = NULL; sh->length = n; sh->sampleRate = rate22050hz; sh->loopStart = 0; sh->loopEnd = 0;
        sh->encode = stdSH; sh->baseFrequency = kMiddleC;
        for (i = 0; i < n; i++) sh->sampleArea[i] = (unsigned char)(128 + 6 * sin(2 * M_PI * 440.0 * i / 22050.0));
        {   OSErr se0 = SndPlay(NULL, (SndListHandle)h, false);   /* synchronous, on a channel of its own */
            printf("sound manager SndPlay(NULL) err %d\n", se0);
            printf("RESULT soundmanager_sync %s\n", se0 ? "FAIL" : "PASS");
            if (se0) ok = 0;
        }
        se = SndNewChannel(&chan, sampledSynth, initMono, NULL);
        printf("SndNewChannel err %d\n", se);
        if (!se) se = SndPlay(chan, (SndListHandle)h, true);
        for (i = 0; i < 40 && !se; i++) {
            usleep(50000);
            if (SndChannelStatus(chan, sizeof st, &st)) break;
            if (!st.scChannelBusy) { done = 1; break; }
        }
        printf("sound manager play err %d, finished %s after %ld ms\n", (int)se, done ? "yes" : "no", i * 50);
        printf("RESULT soundmanager %s\n", (!se && done) ? "PASS" : "FAIL");
        if (chan) SndDisposeChannel(chan, true);
        if (se || !done) ok = 0;
    }
#endif
    return ok ? 0 : 1;
}
