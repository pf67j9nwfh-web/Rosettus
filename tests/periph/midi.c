/* midi: PowerPC peripheral test - CoreMIDI (music software: sequencers, notation, synth editors).
   1. Lists the MIDI devices, sources and destinations the MIDI server knows.
   2. A virtual destination of our own, reached by MIDISend through an output port: the note-on
      must arrive in its read procedure unchanged.
   3. A virtual source of our own (MIDIReceived), connected to an input port: the controller
      message must arrive in the port's read procedure.
   4. A 300-byte SysEx message through the same path (MIDISendSysex).
   Everything stays inside this process and the MIDI server: no instrument needed, no sound.
   Build: gcc -arch ppc -o midi midi.c -framework CoreMIDI -framework CoreFoundation */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

static volatile int g_dest_got, g_port_got, g_sysex_len;
static unsigned char g_dest_msg[3], g_port_msg[3];
static unsigned char g_sysex[400];

static void dest_read(const MIDIPacketList *pl, void *ref, void *src) {
    const MIDIPacket *p = &pl->packet[0]; UInt32 i;
    for (i = 0; i < pl->numPackets; i++) {
        if (p->length >= 1 && p->data[0] == 0xF0) {
            if (g_sysex_len + p->length <= (int)sizeof g_sysex) { memcpy(g_sysex + g_sysex_len, p->data, p->length); g_sysex_len += p->length; }
        } else if (p->length >= 3 && !g_dest_got) { memcpy(g_dest_msg, p->data, 3); g_dest_got = 1; }
        else if (g_sysex_len && p->length && g_sysex_len + p->length <= (int)sizeof g_sysex) { memcpy(g_sysex + g_sysex_len, p->data, p->length); g_sysex_len += p->length; }
        p = MIDIPacketNext(p);
    }
}
static void port_read(const MIDIPacketList *pl, void *ref, void *src) {
    const MIDIPacket *p = &pl->packet[0];
    if (pl->numPackets && p->length >= 3 && !g_port_got) { memcpy(g_port_msg, p->data, 3); g_port_got = 1; }
}
static void sysex_done(MIDISysexSendRequest *r) { (void)r; }
static void wait_for(volatile int *flag, double secs) {
    double t = 0;
    while (!*flag && t < secs) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false); t += 0.05; }
}
static void name_of(MIDIObjectRef o, char *buf, int n) {
    CFStringRef s = NULL; buf[0] = 0;
    if (!MIDIObjectGetStringProperty(o, kMIDIPropertyName, &s) && s) { CFStringGetCString(s, buf, n, kCFStringEncodingUTF8); CFRelease(s); }
}
int main(void) {
    MIDIClientRef cl = 0; MIDIEndpointRef vdst = 0, vsrc = 0; MIDIPortRef out = 0, in = 0;
    OSStatus e; int ok = 1; ItemCount i, nd, ns, nt; char nm[256];
    e = MIDIClientCreate(CFSTR("Rosettus MIDI test"), NULL, NULL, &cl);
    printf("MIDIClientCreate err %d\n", (int)e);
    printf("RESULT midi_client %s\n", e ? "FAIL" : "PASS");
    if (e) return 1;
    nd = MIDIGetNumberOfDevices(); ns = MIDIGetNumberOfSources(); nt = MIDIGetNumberOfDestinations();
    printf("%lu devices, %lu sources, %lu destinations\n", (unsigned long)nd, (unsigned long)ns, (unsigned long)nt);
    {   int named = 0;
        for (i = 0; i < nd; i++) {
            SInt32 uid = 0;
            name_of(MIDIGetDevice(i), nm, sizeof nm);
            MIDIObjectGetIntegerProperty(MIDIGetDevice(i), kMIDIPropertyUniqueID, &uid);
            printf("  device %lu \"%s\" unique ID %ld, %lu entities\n", (unsigned long)i, nm, (long)uid, (unsigned long)MIDIDeviceGetNumberOfEntities(MIDIGetDevice(i)));
            if (nm[0]) named++;
        }
        printf("RESULT midi_devices %s (%lu devices, %d named)\n", (ItemCount)named == nd ? "PASS" : "FAIL", (unsigned long)nd, named);
        if ((ItemCount)named != nd) ok = 0;
    }
    /* 2. our own destination, reached by MIDISend */
    e = MIDIDestinationCreate(cl, CFSTR("Rosettus test in"), dest_read, NULL, &vdst);
    if (!e) e = MIDIOutputPortCreate(cl, CFSTR("out"), &out);
    if (!e) {
        Byte buf[256]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p = MIDIPacketListInit(pl);
        Byte note[3] = { 0x90, 60, 100 };
        p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, note);
        e = MIDISend(out, vdst, pl);
        wait_for(&g_dest_got, 3.0);
    }
    printf("MIDISend to a virtual destination: err %d, received %s %02x %02x %02x\n", (int)e, g_dest_got ? "yes" : "no", g_dest_msg[0], g_dest_msg[1], g_dest_msg[2]);
    printf("RESULT midi_send %s\n", !e && g_dest_got && g_dest_msg[0] == 0x90 && g_dest_msg[1] == 60 && g_dest_msg[2] == 100 ? "PASS" : "FAIL");
    if (e || !g_dest_got || g_dest_msg[0] != 0x90) ok = 0;
    /* 3. our own source, connected to an input port */
    e = MIDISourceCreate(cl, CFSTR("Rosettus test out"), &vsrc);
    if (!e) e = MIDIInputPortCreate(cl, CFSTR("in"), port_read, NULL, &in);
    if (!e) e = MIDIPortConnectSource(in, vsrc, NULL);
    if (!e) {
        Byte buf[256]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p = MIDIPacketListInit(pl);
        Byte cc[3] = { 0xB3, 7, 99 };
        p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, cc);
        usleep(100000);
        e = MIDIReceived(vsrc, pl);
        wait_for(&g_port_got, 3.0);
    }
    printf("MIDIReceived on a virtual source: err %d, received %s %02x %02x %02x\n", (int)e, g_port_got ? "yes" : "no", g_port_msg[0], g_port_msg[1], g_port_msg[2]);
    printf("RESULT midi_receive %s\n", !e && g_port_got && g_port_msg[0] == 0xB3 && g_port_msg[1] == 7 && g_port_msg[2] == 99 ? "PASS" : "FAIL");
    if (e || !g_port_got || g_port_msg[0] != 0xB3) ok = 0;
    /* 4. SysEx */
    {   static Byte sx[300]; static MIDISysexSendRequest rq; int k, good;
        sx[0] = 0xF0; for (k = 1; k < 299; k++) sx[k] = (Byte)(k & 0x7f); sx[299] = 0xF7;
        memset(&rq, 0, sizeof rq);
        rq.destination = vdst; rq.data = sx; rq.bytesToSend = sizeof sx; rq.complete = false; rq.completionProc = sysex_done;
        e = MIDISendSysex(&rq);
        {   double t = 0; while (g_sysex_len < 300 && t < 3.0) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false); t += 0.05; } }
        good = g_sysex_len == 300 && !memcmp(g_sysex, sx, 300);
        printf("MIDISendSysex: err %d, %d of 300 bytes received%s\n", (int)e, g_sysex_len, good ? ", identical" : "");
        printf("RESULT midi_sysex %s\n", !e && good ? "PASS" : "FAIL");
        if (e || !good) ok = 0;
    }
    if (in) MIDIPortDispose(in);
    if (out) MIDIPortDispose(out);
    if (vsrc) MIDIEndpointDispose(vsrc);
    if (vdst) MIDIEndpointDispose(vdst);
    MIDIClientDispose(cl);
    return ok ? 0 : 1;
}
