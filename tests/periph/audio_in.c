/* audio_in: PowerPC peripheral test - microphone input.
   1. CoreAudio HAL: default input device, name, rate, input channels.
   2. AudioQueue input: records 1 s of 16-bit mono, counts buffers and the peak level.
   3. Sound Input Manager: SPBGetDeviceInfo of the default input (siDeviceName) - the Classic API.
   A peak of 0 can be real silence or a refused microphone permission; it is reported, not failed.
   Build: gcc -arch ppc -o audio_in audio_in.c -framework CoreAudio -framework AudioToolbox -framework Carbon */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <Carbon/Carbon.h>
#include <AudioUnit/AudioUnit.h>

static volatile long g_bufs, g_frames; static volatile int g_peak;
static void inproc(void *ud, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 np, const AudioStreamPacketDescription *pd) {
    SInt16 *s = (SInt16 *)b->mAudioData; UInt32 n = b->mAudioDataByteSize / 2, i;
    for (i = 0; i < n; i++) { int v = abs(s[i]); if (v > g_peak) g_peak = v; }
    g_bufs++; g_frames += n;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static AudioUnit g_hal; static volatile long g_hcalls, g_hframes; static volatile float g_hpeak;
static OSStatus halin(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *unused) {
    static float buf[2 * 8192]; AudioBufferList bl; UInt32 i;
    if (n > 8192) n = 8192;
    bl.mNumberBuffers = 1; bl.mBuffers[0].mNumberChannels = 2; bl.mBuffers[0].mDataByteSize = n * 8; bl.mBuffers[0].mData = buf;
    if (!AudioUnitRender(g_hal, fl, ts, 1, n, &bl)) {
        for (i = 0; i < n * 2; i++) { float v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > g_hpeak) g_hpeak = v; }
        g_hframes += n;
    }
    g_hcalls++;
    return 0;
}
int main(void) {
    AudioDeviceID dev = 0; UInt32 sz = sizeof dev; OSStatus e; int ok = 1;
    char name[256] = ""; Float64 rate = 0; UInt32 chans = 0;
    e = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &sz, &dev);
    if (dev) {
        AudioBufferList *bl; UInt32 i;
        sz = sizeof name; AudioDeviceGetProperty(dev, 0, true, kAudioDevicePropertyDeviceName, &sz, name);
        sz = sizeof rate; AudioDeviceGetProperty(dev, 0, true, kAudioDevicePropertyNominalSampleRate, &sz, &rate);
        if (!AudioDeviceGetPropertyInfo(dev, 0, true, kAudioDevicePropertyStreamConfiguration, &sz, NULL) && (bl = malloc(sz))) {
            if (!AudioDeviceGetProperty(dev, 0, true, kAudioDevicePropertyStreamConfiguration, &sz, bl))
                for (i = 0; i < bl->mNumberBuffers; i++) chans += bl->mBuffers[i].mNumberChannels;
            free(bl);
        }
    }
    printf("default input device \"%s\" rate %.0f channels %u (err %d)\n", name, rate, (unsigned)chans, (int)e);
    printf("RESULT indevice %s\n", dev && chans > 0 ? "PASS" : "FAIL");
    if (!dev) ok = 0;
    {
        AudioStreamBasicDescription f = { 44100, kAudioFormatLinearPCM,
            kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked | kAudioFormatFlagsNativeEndian, 2, 1, 2, 1, 16, 0 };
        AudioQueueRef q = NULL; int i;
        e = AudioQueueNewInput(&f, inproc, NULL, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &q);
        for (i = 0; i < 3 && !e; i++) { AudioQueueBufferRef b; if (!AudioQueueAllocateBuffer(q, 8820, &b)) AudioQueueEnqueueBuffer(q, b, 0, NULL); }
        if (!e) e = AudioQueueStart(q, NULL);
        if (!e) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        if (q) { AudioQueueStop(q, true); AudioQueueDispose(q, true); }
        printf("audioqueue input err %d, %ld buffers, %ld frames (expect about 44100), peak %d of 32767\n", (int)e, g_bufs, g_frames, g_peak);
        printf("RESULT audioqueue_in %s%s\n", (!e && g_frames > 22000) ? "PASS" : "FAIL", g_peak == 0 ? " (silent: no sound or no permission)" : "");
        if (e || g_frames <= 22000) ok = 0;
    }
    {   /* AUHAL input (the HAL output unit with input enabled, output disabled) */
        ComponentDescription cd = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        Component c = FindNextComponent(NULL, &cd);
        UInt32 one = 1, zero = 0;
        AURenderCallbackStruct cb = { halin, NULL };
        AudioStreamBasicDescription f = { 44100, kAudioFormatLinearPCM,
            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian, 8, 1, 8, 2, 32, 0 };
        e = (!c || OpenAComponent(c, (ComponentInstance *)&g_hal)) ? -1 : 0;
        if (!e) e = AudioUnitSetProperty(g_hal, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
        if (!e) e = AudioUnitSetProperty(g_hal, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof zero);
        if (!e) e = AudioUnitSetProperty(g_hal, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
        if (!e) e = AudioUnitSetProperty(g_hal, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
        if (!e) e = AudioUnitSetProperty(g_hal, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
        if (!e) e = AudioUnitInitialize(g_hal);
        if (!e) e = AudioOutputUnitStart(g_hal);
        if (!e) { usleep(800000); AudioOutputUnitStop(g_hal); }
        if (g_hal) { AudioUnitUninitialize(g_hal); CloseComponent(g_hal); }
        printf("AUHAL input err %d, %ld callbacks, %ld frames (expect about 35000), peak %.4f\n", (int)e, g_hcalls, g_hframes, g_hpeak);
        printf("RESULT auhal_in %s%s\n", (!e && g_hframes > 17000) ? "PASS" : "FAIL", g_hpeak == 0 ? " (silent: no sound or no permission)" : "");
        if (e || g_hframes <= 17000) ok = 0;
    }
#ifndef __LP64__   /* the Sound Input Manager is 32-bit only (the native comparison build is 64-bit) */
    {
        Str255 dn; OSErr se = SPBGetIndexedDevice(1, dn, NULL);
        printf("sound input manager device 1: err %d name \"%.*s\"\n", se, se ? 0 : dn[0], (char *)dn + 1);
        printf("RESULT spb_device %s\n", se ? "FAIL" : "PASS");
        if (se) ok = 0;
    }
#endif
    return ok ? 0 : 1;
}
