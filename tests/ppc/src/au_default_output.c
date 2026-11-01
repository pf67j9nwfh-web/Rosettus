/* au_default_output: the BASSMOD / FMOD-style default output AudioUnit sequence.
   Build (on 10.6): gcc -arch ppc -o au_default_output au_default_output.c \
       -framework AudioUnit -framework CoreServices -framework CoreAudio
   Prints one line per step; run under real Rosetta and under the Mojave port and diff.
   Plays a 440 Hz tone for 1.5 s; the render callback count shows whether audio was pulled. */
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>

static volatile int g_calls;
static double g_phase;
static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 frames, AudioBufferList *io) {
    UInt32 i, b;
    (void)ref; (void)flags; (void)ts; (void)bus;
    for (b = 0; b < io->mNumberBuffers; b++) {
        float *p = (float *)io->mBuffers[b].mData;
        UInt32 ch = io->mBuffers[b].mNumberChannels, n = io->mBuffers[b].mDataByteSize / 4;
        double ph = g_phase;
        for (i = 0; i + ch <= n; i += ch) {
            float s = (float)(0.2 * sin(ph)); UInt32 c;
            for (c = 0; c < ch; c++) p[i + c] = s;
            ph += 2 * M_PI * 440.0 / 44100.0;
        }
        if (b == io->mNumberBuffers - 1) g_phase = ph;
    }
    (void)frames;
    g_calls++;
    return noErr;
}

int main(void) {
    OSStatus e; UInt32 sz; AudioDeviceID dev = 0; Float64 rate = 0; UInt32 bufsz = 0;
    ComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    Component comp; AudioUnit au = 0; AudioStreamBasicDescription f, g;
    AURenderCallbackStruct cb;

    setvbuf(stdout, NULL, _IONBF, 0);
    sz = sizeof dev;
    e = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &sz, &dev);
    printf("AudioHardwareGetProperty(DefaultOutputDevice) -> %d, device %s\n", (int)e, dev ? "nonzero" : "0");
    sz = sizeof rate;
    e = AudioDeviceGetProperty(dev, 0, false, kAudioDevicePropertyNominalSampleRate, &sz, &rate);
    printf("AudioDeviceGetProperty(NominalSampleRate) -> %d, %s\n", (int)e, rate > 1000 ? "plausible rate" : "bad rate");
    sz = sizeof bufsz;
    e = AudioDeviceGetProperty(dev, 0, false, kAudioDevicePropertyBufferFrameSize, &sz, &bufsz);
    printf("AudioDeviceGetProperty(BufferFrameSize) -> %d, %s\n", (int)e, bufsz >= 14 && bufsz <= 8192 ? "plausible size" : "bad size");

    comp = FindNextComponent(NULL, &d);
    printf("FindNextComponent(auou/def) -> %s\n", comp ? "found" : "NULL");
    if (!comp) return 1;
    e = OpenAComponent(comp, &au);
    printf("OpenAComponent -> %d\n", (int)e);
    if (e) return 1;

    sz = sizeof g;
    e = AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &g, &sz);
    printf("AudioUnitGetProperty(StreamFormat, output) -> %d\n", (int)e);

    f.mSampleRate = 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; f.mBytesPerPacket = 8; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 8; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mReserved = 0;
    e = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    printf("AudioUnitSetProperty(StreamFormat, input) -> %d\n", (int)e);
    cb.inputProc = render; cb.inputProcRefCon = 0;
    e = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    printf("AudioUnitSetProperty(SetRenderCallback) -> %d\n", (int)e);
    e = AudioUnitInitialize(au);
    printf("AudioUnitInitialize -> %d\n", (int)e);
    e = AudioUnitSetParameter(au, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, 0.8f, 0);
    printf("AudioUnitSetParameter(Volume) -> %d\n", (int)e);
    e = AudioOutputUnitStart(au);
    printf("AudioOutputUnitStart -> %d\n", (int)e);
    usleep(1500000);
    printf("render callbacks in 1.5 s: %s\n", g_calls > 20 ? "many (audio running)" : g_calls ? "few" : "none");
    e = AudioOutputUnitStop(au);
    printf("AudioOutputUnitStop -> %d\n", (int)e);
    AudioUnitUninitialize(au);
    e = CloseComponent(au);
    printf("CloseComponent -> %d\n", (int)e);
    return 0;
}
