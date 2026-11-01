/* oah_midi.h - CoreMIDI for PowerPC apps, served by macOS' own CoreMIDI.

   10.6's PowerPC CoreMIDI talks to the MIDI server in Mach messages of 10.6's protocol; Mojave's
   server rejects them (MIDIClientCreate -> -304, MIG_BAD_ARGUMENTS), so a PowerPC sequencer,
   notation program or synth editor saw no MIDI at all. The calls that reach the server are answered
   here with the native CoreMIDI (gtrace fix rows on the guest's CoreMIDI exports, which skip the
   guest code): clients, ports, sources and destinations of the app's own, the device / entity /
   endpoint tree, properties (strings and numbers; dictionaries and data as property lists),
   MIDISend, MIDIReceived, MIDISendSysex. Object references are the native ones (opaque 32-bit
   numbers to the guest). MIDIPacketList and MIDIPacket have the same layout on both sides (packed:
   numPackets, then timeStamp 8 bytes, length 2, data), byte-swapped on the way.

   Incoming MIDI (read procedures of input ports and of the app's own destinations) and SysEx
   completions are delivered on a guest thread of ours: native CoreMIDI queues them, and the
   thread (started on the first port or destination; its body is a guest stub trapping into
   oah_fix_midi_pump) calls the guest procedure. Client notifications (device added / removed) are
   not delivered. MIDIPacketListInit / MIDIPacketListAdd stay the guest's own (no server).
   OAH_NO_MIDI=1 leaves CoreMIDI to the guest; OAH_MIDILOG=1 logs the calls. */
#include <CoreMIDI/MIDIServices.h>

static int g_midi_idx = -1;                                 /* gtrace hook entry: the delivery thread's body */
static unsigned int g_midi_stub;
static int oah_midi_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_MIDILOG") ? 1 : 0; return v; }
static int oah_want_midi(void) { return !getenv("OAH_NO_MIDI"); }

/* the native CoreMIDI, loaded on first use */
#define OAH_MIDI_FNS(X) \
    X(OSStatus, MIDIClientCreate, (CFStringRef, MIDINotifyProc, void *, UInt32 *)) \
    X(OSStatus, MIDIClientDispose, (UInt32)) \
    X(OSStatus, MIDIInputPortCreate, (UInt32, CFStringRef, MIDIReadProc, void *, UInt32 *)) \
    X(OSStatus, MIDIOutputPortCreate, (UInt32, CFStringRef, UInt32 *)) \
    X(OSStatus, MIDIPortDispose, (UInt32)) \
    X(OSStatus, MIDIPortConnectSource, (UInt32, UInt32, void *)) \
    X(OSStatus, MIDIPortDisconnectSource, (UInt32, UInt32)) \
    X(ItemCount, MIDIGetNumberOfDevices, (void)) \
    X(UInt32, MIDIGetDevice, (ItemCount)) \
    X(ItemCount, MIDIDeviceGetNumberOfEntities, (UInt32)) \
    X(UInt32, MIDIDeviceGetEntity, (UInt32, ItemCount)) \
    X(ItemCount, MIDIEntityGetNumberOfSources, (UInt32)) \
    X(UInt32, MIDIEntityGetSource, (UInt32, ItemCount)) \
    X(ItemCount, MIDIEntityGetNumberOfDestinations, (UInt32)) \
    X(UInt32, MIDIEntityGetDestination, (UInt32, ItemCount)) \
    X(OSStatus, MIDIEntityGetDevice, (UInt32, UInt32 *)) \
    X(ItemCount, MIDIGetNumberOfSources, (void)) \
    X(UInt32, MIDIGetSource, (ItemCount)) \
    X(ItemCount, MIDIGetNumberOfDestinations, (void)) \
    X(UInt32, MIDIGetDestination, (ItemCount)) \
    X(OSStatus, MIDIEndpointGetEntity, (UInt32, UInt32 *)) \
    X(OSStatus, MIDIDestinationCreate, (UInt32, CFStringRef, MIDIReadProc, void *, UInt32 *)) \
    X(OSStatus, MIDISourceCreate, (UInt32, CFStringRef, UInt32 *)) \
    X(OSStatus, MIDIEndpointDispose, (UInt32)) \
    X(ItemCount, MIDIGetNumberOfExternalDevices, (void)) \
    X(UInt32, MIDIGetExternalDevice, (ItemCount)) \
    X(OSStatus, MIDIObjectGetIntegerProperty, (UInt32, CFStringRef, SInt32 *)) \
    X(OSStatus, MIDIObjectSetIntegerProperty, (UInt32, CFStringRef, SInt32)) \
    X(OSStatus, MIDIObjectGetStringProperty, (UInt32, CFStringRef, CFStringRef *)) \
    X(OSStatus, MIDIObjectSetStringProperty, (UInt32, CFStringRef, CFStringRef)) \
    X(OSStatus, MIDIObjectGetDataProperty, (UInt32, CFStringRef, CFDataRef *)) \
    X(OSStatus, MIDIObjectGetDictionaryProperty, (UInt32, CFStringRef, CFDictionaryRef *)) \
    X(OSStatus, MIDIObjectGetProperties, (UInt32, CFPropertyListRef *, Boolean)) \
    X(OSStatus, MIDIObjectFindByUniqueID, (MIDIUniqueID, UInt32 *, MIDIObjectType *)) \
    X(OSStatus, MIDISend, (UInt32, UInt32, const MIDIPacketList *)) \
    X(OSStatus, MIDISendSysex, (MIDISysexSendRequest *)) \
    X(OSStatus, MIDIReceived, (UInt32, const MIDIPacketList *)) \
    X(OSStatus, MIDIFlushOutput, (UInt32)) \
    X(OSStatus, MIDIRestart, (void)) \
    X(OSStatus, MIDIEndpointGetRefCons, (UInt32, void **, void **)) \
    X(OSStatus, MIDIEndpointSetRefCons, (UInt32, void *, void *))
#define OAH_MIDI_DECL(r, n, a) static r (*nm_##n) a;
OAH_MIDI_FNS(OAH_MIDI_DECL)
static long oah_midi_missing(void) { return -10844; /* kMIDINotPermitted */ }
static int oah_midi_load(void) {
    static int ok = -1;
    void *h;
    if (ok >= 0) return ok;
    ok = 0;
    if (!(h = dlopen("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI", RTLD_LAZY | RTLD_LOCAL))) return 0;
    /* a function this macOS lacks answers kMIDINotPermitted (or 0) instead of disabling everything */
#define OAH_MIDI_SYM(r, n, a) if (!(nm_##n = (r (*) a)dlsym(h, #n))) { fprintf(stderr, "[midi] native %s missing\n", #n); nm_##n = (r (*) a)(void *)oah_midi_missing; }
    OAH_MIDI_FNS(OAH_MIDI_SYM)
    ok = nm_MIDIClientCreate != (void *)oah_midi_missing;
    return 1;
}

/* ---- guest CoreFoundation ---- */
enum { GM_GETCSTR, GM_MKCSTR, GM_DATACREATE, GM_PLFROMXML, GM_RELEASE, GM_N };
static unsigned int g_gm[GM_N];
static int oah_midi_gcf(void) {
    static const char *n[GM_N] = { "_CFStringGetCString", "_CFStringCreateWithCString", "_CFDataCreate", "_CFPropertyListCreateFromXMLData", "_CFRelease" };
    int i;
    for (i = 0; i < GM_N; i++) if (!g_gm[i] && !(g_gm[i] = oah_guest_sym("CoreFoundation", n[i]))) return 0;
    return 1;
}
static unsigned int oah_gm(int f, int n, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3) {
    unsigned int a[4]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3;
    return oah_callppc_n("midi_cf", g_gm[f], n, a);
}
/* a guest CFString as a new native one (NULL for NULL) */
static CFStringRef oah_midi_nstr(unsigned int gs) {
    char buf[1024];
    if (!gs || !oah_midi_gcf()) return NULL;
    buf[0] = 0;
    if (!oah_gm(GM_GETCSTR, 4, gs, (unsigned int)(uintptr_t)buf, 1024, kCFStringEncodingUTF8)) return NULL;
    return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}
/* a native CFString as a new guest one */
static unsigned int oah_midi_gstr(CFStringRef s) {
    char *b; unsigned int g;
    CFIndex n;
    if (!s || !oah_midi_gcf()) return 0;
    n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    if (!(b = (char *)malloc(n))) return 0;
    if (!CFStringGetCString(s, b, n, kCFStringEncodingUTF8)) b[0] = 0;
    g = oah_gm(GM_MKCSTR, 3, 0, (unsigned int)(uintptr_t)b, kCFStringEncodingUTF8, 0);
    free(b);
    return g;
}
/* a native property list (dictionary, data, ...) as a new guest one, through XML */
static unsigned int oah_midi_gplist(CFPropertyListRef p) {
    CFDataRef x; unsigned int gd, g = 0;
    if (!p || !oah_midi_gcf() || !(x = CFPropertyListCreateXMLData(NULL, p))) return 0;
    gd = oah_gm(GM_DATACREATE, 3, 0, (unsigned int)(uintptr_t)CFDataGetBytePtr(x), (unsigned int)CFDataGetLength(x), 0);
    if (gd) { g = oah_gm(GM_PLFROMXML, 4, 0, gd, 0 /* immutable */, 0); oah_gm(GM_RELEASE, 1, gd, 0, 0, 0); }
    CFRelease(x);
    return g;
}
static void oah_midi_put(unsigned int a, unsigned int v) { if (a && oah_readable(a, 4)) *(unsigned int *)(uintptr_t)a = OSSwapHostToBigInt32(v); }
static unsigned int oah_midi_get(unsigned int a) { return (a && oah_readable(a, 4)) ? OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)a) : 0; }

/* ---- packet lists ---- */
/* a guest (big-endian) MIDIPacketList as a native one; NULL when malformed */
static MIDIPacketList *oah_midi_npl(unsigned int g) {
    const unsigned char *s = (const unsigned char *)(uintptr_t)g;
    unsigned int n, i, off = 4, len, total = 4;
    unsigned char *d;
    if (!g || !oah_readable(g, 4)) return NULL;
    n = OSSwapBigToHostInt32(*(const unsigned int *)s);
    if (n > 65536) return NULL;
    for (i = 0; i < n; i++) {                                   /* size first */
        if (!oah_readable(g + off, 10)) return NULL;
        len = (s[off + 8] << 8) | s[off + 9];
        if (!oah_readable(g + off, 10 + len)) return NULL;
        off += 10 + len; total += 10 + len;
    }
    if (!(d = (unsigned char *)malloc(total < 1024 ? 1024 : total))) return NULL;
    *(unsigned int *)d = n;
    for (i = 0, off = 4; i < n; i++) {
        unsigned long long ts; unsigned short l;
        memcpy(&ts, s + off, 8); ts = OSSwapBigToHostInt64(ts);
        l = (unsigned short)((s[off + 8] << 8) | s[off + 9]);
        memcpy(d + off, &ts, 8); memcpy(d + off + 8, &l, 2); memcpy(d + off + 10, s + off + 10, l);
        off += 10 + l;
    }
    return (MIDIPacketList *)d;
}
/* a native MIDIPacketList as a guest one (malloc'd, the guest reads it in place) */
static unsigned char *oah_midi_gpl(const MIDIPacketList *pl) {
    const MIDIPacket *p = &pl->packet[0];
    unsigned int i, n = pl->numPackets, total = 4, off = 4;
    unsigned char *d;
    for (i = 0; i < n; i++) { total += 10 + p->length; p = MIDIPacketNext(p); }
    if (!(d = (unsigned char *)malloc(total + 16))) return NULL;
    *(unsigned int *)d = OSSwapHostToBigInt32(n);
    for (i = 0, p = &pl->packet[0]; i < n; i++) {
        unsigned long long ts = OSSwapHostToBigInt64(p->timeStamp);
        memcpy(d + off, &ts, 8); d[off + 8] = (unsigned char)(p->length >> 8); d[off + 9] = (unsigned char)p->length;
        memcpy(d + off + 10, p->data, p->length);
        off += 10 + p->length;
        p = MIDIPacketNext(p);
    }
    return d;
}

/* ---- delivery to the guest ---- */
typedef struct oah_mq { struct oah_mq *next; unsigned int proc, a0, a1, a2; int n; unsigned char *mem; } oah_mq;
static pthread_mutex_t g_mq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mq_cv = PTHREAD_COND_INITIALIZER;
static oah_mq *g_mq_head, *g_mq_tail;
static int g_midi_thread;                                   /* 0 none, 1 starting/running, 2 failed */
static void oah_midi_queue(unsigned int proc, int n, unsigned int a0, unsigned int a1, unsigned int a2, unsigned char *mem) {
    oah_mq *q = (oah_mq *)calloc(1, sizeof *q);
    if (!q) { free(mem); return; }
    q->proc = proc; q->n = n; q->a0 = a0; q->a1 = a1; q->a2 = a2; q->mem = mem;
    pthread_mutex_lock(&g_mq_mu);
    if (g_mq_tail) g_mq_tail->next = q; else g_mq_head = q;
    g_mq_tail = q;
    pthread_cond_signal(&g_mq_cv);
    pthread_mutex_unlock(&g_mq_mu);
}
/* the guest thread's body: never returns */
static void oah_fix_midi_pump(unsigned int *R) {
    (void)R;
    if (oah_midi_log()) fprintf(stderr, "[midi] delivery thread running\n");
    for (;;) {
        oah_mq *q;
        unsigned int a[3];
        pthread_mutex_lock(&g_mq_mu);
        while (!g_mq_head) pthread_cond_wait(&g_mq_cv, &g_mq_mu);
        q = g_mq_head; g_mq_head = q->next; if (!g_mq_head) g_mq_tail = NULL;
        pthread_mutex_unlock(&g_mq_mu);
        a[0] = q->a0; a[1] = q->a1; a[2] = q->a2;
        if (q->proc) oah_callppc_n("midi_read", q->proc, q->n, a);
        free(q->mem); free(q);
    }
}
static void oah_midi_start_thread(void) {
    unsigned int pc, a[4], *t;
    static unsigned int tid;
    if (g_midi_thread || g_midi_idx < 0) return;
    g_midi_thread = 2;
    if (!(pc = oah_guest_sym("libSystem", "_pthread_create")) || !(t = (unsigned int *)oah_guest_code_alloc(8))) return;
    t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_midi_idx));
    t[1] = OSSwapHostToBigInt32(0x4e800020);
    g_midi_stub = (unsigned int)(uintptr_t)t;
    a[0] = (unsigned int)(uintptr_t)&tid; a[1] = 0; a[2] = g_midi_stub; a[3] = 0;
    if (!oah_callppc_n("midi_pthread_create", pc, 4, a)) g_midi_thread = 1;
    if (oah_midi_log()) fprintf(stderr, "[midi] delivery thread %s\n", g_midi_thread == 1 ? "started" : "could not start");
}
/* native read procedure of a port or destination of the guest's: refCon -> {guest proc, guest refCon} */
typedef struct { unsigned int proc, ref; } oah_mrd;
static void oah_midi_nread(const MIDIPacketList *pl, void *readRef, void *connRef) {
    oah_mrd *r = (oah_mrd *)readRef;
    unsigned char *g;
    if (!r || !pl || !(g = oah_midi_gpl(pl))) return;
    oah_midi_queue(r->proc, 3, (unsigned int)(uintptr_t)g, r->ref, (unsigned int)(uintptr_t)connRef, g);
}
/* SysEx: the native request, the guest's, the copied data */
typedef struct { MIDISysexSendRequest n; unsigned int g, proc; unsigned char data[1]; } oah_msx;
static void oah_midi_sxdone(MIDISysexSendRequest *n) {
    oah_msx *x = (oah_msx *)n;
    if (oah_readable(x->g, 24)) {
        oah_midi_put(x->g + 8, 0);                          /* bytesToSend */
        *(unsigned char *)(uintptr_t)(x->g + 12) = 1;       /* complete */
    }
    if (x->proc) oah_midi_queue(x->proc, 1, x->g, 0, 0, (unsigned char *)x);
    else free(x);
}

/* The property-name constants (kMIDIPropertyName, ...) of 10.6's CoreMIDI are filled in by its own
   client set-up, which no longer runs: give each one that is still NULL a guest CFString with the
   native constant's text (the same names on both sides). */
static void oah_midi_consts(void) {
    static int done;
    static const char *const n[] = { "Name", "Manufacturer", "Model", "UniqueID", "DeviceID", "ReceiveChannels", "TransmitChannels", "MaxSysExSpeed", "AdvanceScheduleTimeMuSec", "IsEmbeddedEntity", "IsBroadcast", "SingleRealtimeEntity", "ConnectionUniqueID", "Offline", "Private", "DriverOwner", "FactoryPatchNameFile", "UserPatchNameFile", "NameConfiguration", "Image", "DriverVersion", "SupportsGeneralMIDI", "SupportsMMC", "CanRoute", "ReceivesClock", "ReceivesMTC", "ReceivesNotes", "ReceivesProgramChanges", "ReceivesBankSelectMSB", "ReceivesBankSelectLSB", "TransmitsBankSelectMSB", "TransmitsBankSelectLSB", "TransmitsClock", "TransmitsMTC", "TransmitsNotes", "TransmitsProgramChanges", "PanDisruptsStereo", "IsSampler", "IsDrumMachine", "IsMixer", "IsEffectUnit", "MaxReceiveChannels", "MaxTransmitChannels", "DriverDeviceEditorApp", "SupportsShowControl", "DisplayName", 0 };
    void *h; int i, set = 0;
    if (done) return;
    if (!(h = dlopen("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI", RTLD_LAZY | RTLD_LOCAL))) { done = 1; return; }
    nm_MIDIGetNumberOfDevices();                            /* the native constants are filled in by its set-up too */
    for (i = 0; n[i]; i++) {
        char sym[64]; CFStringRef *nv; unsigned int g;
        snprintf(sym, sizeof sym, "kMIDIProperty%s", n[i]);
        nv = (CFStringRef *)dlsym(h, sym);
        snprintf(sym, sizeof sym, "_kMIDIProperty%s", n[i]);
        g = oah_guest_sym("CoreMIDI", sym);
        if (!nv || !*nv || !g || !oah_readable(g, 4) || *(unsigned int *)(uintptr_t)g) continue;
        if (oah_midi_put(g, oah_midi_gstr(*nv)), oah_midi_get(g)) set++;
    }
    {   CFStringRef *k = (CFStringRef *)dlsym(h, "kMIDIPropertyName"); done = !k || *k; }   /* else again at the next call */
    if (oah_midi_log()) fprintf(stderr, "[midi] %d property names set\n", set);
}
/* ---- the guest's calls ---- */
enum { MC_CLIENTCREATE, MC_CLIENTDISPOSE, MC_INPORT, MC_OUTPORT, MC_PORTDISPOSE, MC_CONNECT, MC_DISCONNECT,
       MC_NDEV, MC_DEV, MC_DEV_NENT, MC_DEV_ENT, MC_ENT_NSRC, MC_ENT_SRC, MC_ENT_NDST, MC_ENT_DST, MC_ENT_DEV,
       MC_NSRC, MC_SRC, MC_NDST, MC_DST, MC_EP_ENT, MC_DSTCREATE, MC_SRCCREATE, MC_EPDISPOSE, MC_NEXT, MC_EXT,
       MC_GETINT, MC_SETINT, MC_GETSTR, MC_SETSTR, MC_GETDATA, MC_GETDICT, MC_GETPROPS, MC_FINDUID,
       MC_SEND, MC_SYSEX, MC_RECEIVED, MC_FLUSH, MC_RESTART, MC_GETREFCONS, MC_SETREFCONS, MC_N };
static const char *const g_mc_name[MC_N] = { "MIDIClientCreate", "MIDIClientDispose", "MIDIInputPortCreate", "MIDIOutputPortCreate",
    "MIDIPortDispose", "MIDIPortConnectSource", "MIDIPortDisconnectSource", "MIDIGetNumberOfDevices", "MIDIGetDevice",
    "MIDIDeviceGetNumberOfEntities", "MIDIDeviceGetEntity", "MIDIEntityGetNumberOfSources", "MIDIEntityGetSource",
    "MIDIEntityGetNumberOfDestinations", "MIDIEntityGetDestination", "MIDIEntityGetDevice", "MIDIGetNumberOfSources",
    "MIDIGetSource", "MIDIGetNumberOfDestinations", "MIDIGetDestination", "MIDIEndpointGetEntity", "MIDIDestinationCreate",
    "MIDISourceCreate", "MIDIEndpointDispose", "MIDIGetNumberOfExternalDevices", "MIDIGetExternalDevice",
    "MIDIObjectGetIntegerProperty", "MIDIObjectSetIntegerProperty", "MIDIObjectGetStringProperty", "MIDIObjectSetStringProperty",
    "MIDIObjectGetDataProperty", "MIDIObjectGetDictionaryProperty", "MIDIObjectGetProperties", "MIDIObjectFindByUniqueID",
    "MIDISend", "MIDISendSysex", "MIDIReceived", "MIDIFlushOutput", "MIDIRestart", "MIDIEndpointGetRefCons", "MIDIEndpointSetRefCons" };
static void oah_midi_call(int which, unsigned int *R) {
    unsigned int r = 0;
    CFStringRef s = NULL;
    if (!oah_midi_load()) return;                           /* no native CoreMIDI: the guest's own code */
    g_gt_skip = 1;
    oah_midi_consts();
    switch (which) {
    case MC_CLIENTCREATE: { UInt32 c = 0;
        s = oah_midi_nstr(R[3]);
        r = (unsigned int)nm_MIDIClientCreate(s ? s : CFSTR("PowerPC app"), NULL, NULL, &c);
        oah_midi_put(R[6], c); break; }
    case MC_CLIENTDISPOSE: r = (unsigned int)nm_MIDIClientDispose(R[3]); break;
    case MC_INPORT: case MC_DSTCREATE: { UInt32 o = 0; oah_mrd *rd = (oah_mrd *)calloc(1, sizeof *rd);
        s = oah_midi_nstr(R[4]);
        if (rd) { rd->proc = R[5]; rd->ref = R[6]; }
        if (which == MC_INPORT) r = (unsigned int)nm_MIDIInputPortCreate(R[3], s ? s : CFSTR("in"), oah_midi_nread, rd, &o);
        else r = (unsigned int)nm_MIDIDestinationCreate(R[3], s ? s : CFSTR("in"), oah_midi_nread, rd, &o);
        oah_midi_put(R[7], o);
        if (!r) oah_midi_start_thread();
        break; }
    case MC_OUTPORT: { UInt32 o = 0; s = oah_midi_nstr(R[4]);
        r = (unsigned int)nm_MIDIOutputPortCreate(R[3], s ? s : CFSTR("out"), &o); oah_midi_put(R[5], o); break; }
    case MC_SRCCREATE: { UInt32 o = 0; s = oah_midi_nstr(R[4]);
        r = (unsigned int)nm_MIDISourceCreate(R[3], s ? s : CFSTR("out"), &o); oah_midi_put(R[5], o); break; }
    case MC_PORTDISPOSE: r = (unsigned int)nm_MIDIPortDispose(R[3]); break;
    case MC_EPDISPOSE: r = (unsigned int)nm_MIDIEndpointDispose(R[3]); break;
    case MC_CONNECT: r = (unsigned int)nm_MIDIPortConnectSource(R[3], R[4], (void *)(uintptr_t)R[5]); break;
    case MC_DISCONNECT: r = (unsigned int)nm_MIDIPortDisconnectSource(R[3], R[4]); break;
    case MC_NDEV: r = (unsigned int)nm_MIDIGetNumberOfDevices(); break;
    case MC_DEV: r = nm_MIDIGetDevice(R[3]); break;
    case MC_DEV_NENT: r = (unsigned int)nm_MIDIDeviceGetNumberOfEntities(R[3]); break;
    case MC_DEV_ENT: r = nm_MIDIDeviceGetEntity(R[3], R[4]); break;
    case MC_ENT_NSRC: r = (unsigned int)nm_MIDIEntityGetNumberOfSources(R[3]); break;
    case MC_ENT_SRC: r = nm_MIDIEntityGetSource(R[3], R[4]); break;
    case MC_ENT_NDST: r = (unsigned int)nm_MIDIEntityGetNumberOfDestinations(R[3]); break;
    case MC_ENT_DST: r = nm_MIDIEntityGetDestination(R[3], R[4]); break;
    case MC_ENT_DEV: { UInt32 d = 0; r = (unsigned int)nm_MIDIEntityGetDevice(R[3], &d); oah_midi_put(R[4], d); break; }
    case MC_NSRC: r = (unsigned int)nm_MIDIGetNumberOfSources(); break;
    case MC_SRC: r = nm_MIDIGetSource(R[3]); break;
    case MC_NDST: r = (unsigned int)nm_MIDIGetNumberOfDestinations(); break;
    case MC_DST: r = nm_MIDIGetDestination(R[3]); break;
    case MC_EP_ENT: { UInt32 e = 0; r = (unsigned int)nm_MIDIEndpointGetEntity(R[3], &e); oah_midi_put(R[4], e); break; }
    case MC_NEXT: r = (unsigned int)nm_MIDIGetNumberOfExternalDevices(); break;
    case MC_EXT: r = nm_MIDIGetExternalDevice(R[3]); break;
    case MC_GETINT: { SInt32 v = 0; s = oah_midi_nstr(R[4]);
        r = (unsigned int)nm_MIDIObjectGetIntegerProperty(R[3], s, &v); if (!r) oah_midi_put(R[5], (unsigned int)v); break; }
    case MC_SETINT: s = oah_midi_nstr(R[4]); r = (unsigned int)nm_MIDIObjectSetIntegerProperty(R[3], s, (SInt32)R[5]); break;
    case MC_GETSTR: { CFStringRef v = NULL; s = oah_midi_nstr(R[4]);
        r = (unsigned int)nm_MIDIObjectGetStringProperty(R[3], s, &v);
        oah_midi_put(R[5], !r && v ? oah_midi_gstr(v) : 0);
        if (v) CFRelease(v);
        break; }
    case MC_SETSTR: { CFStringRef v = oah_midi_nstr(R[5]); s = oah_midi_nstr(R[4]);
        r = (unsigned int)nm_MIDIObjectSetStringProperty(R[3], s, v); if (v) CFRelease(v); break; }
    case MC_GETDATA: case MC_GETDICT: { CFTypeRef v = NULL; s = oah_midi_nstr(R[4]);
        r = (unsigned int)(which == MC_GETDATA ? nm_MIDIObjectGetDataProperty(R[3], s, (CFDataRef *)&v)
                                               : nm_MIDIObjectGetDictionaryProperty(R[3], s, (CFDictionaryRef *)&v));
        oah_midi_put(R[5], !r && v ? oah_midi_gplist(v) : 0);
        if (v) CFRelease(v);
        break; }
    case MC_GETPROPS: { CFPropertyListRef v = NULL;
        r = (unsigned int)nm_MIDIObjectGetProperties(R[3], &v, (Boolean)(R[5] & 0xff));
        oah_midi_put(R[4], !r && v ? oah_midi_gplist(v) : 0);
        if (v) CFRelease(v);
        break; }
    case MC_FINDUID: { UInt32 o = 0; MIDIObjectType t = 0;
        r = (unsigned int)nm_MIDIObjectFindByUniqueID((MIDIUniqueID)R[3], &o, &t); oah_midi_put(R[4], o); oah_midi_put(R[5], (unsigned int)t); break; }
    case MC_SEND: case MC_RECEIVED: { MIDIPacketList *pl = oah_midi_npl(which == MC_SEND ? R[5] : R[4]);
        if (!pl) { r = (unsigned int)kMIDIMessageSendErr; break; }
        r = (unsigned int)(which == MC_SEND ? nm_MIDISend(R[3], R[4], pl) : nm_MIDIReceived(R[3], pl));
        free(pl); break; }
    case MC_SYSEX: { unsigned int g = R[3], data, len; oah_msx *x;
        if (!oah_readable(g, 24)) { r = (unsigned int)-50 /* paramErr */; break; }
        data = oah_midi_get(g + 4); len = oah_midi_get(g + 8);
        if (len > (16u << 20) || (len && !oah_readable(data, len)) || !(x = (oah_msx *)calloc(1, sizeof *x + len))) { r = (unsigned int)-50 /* paramErr */; break; }
        memcpy(x->data, (const void *)(uintptr_t)data, len);
        x->g = g; x->proc = oah_midi_get(g + 16);
        x->n.destination = (MIDIEndpointRef)(uintptr_t)oah_midi_get(g); x->n.data = x->data; x->n.bytesToSend = len; x->n.complete = false;
        x->n.completionProc = oah_midi_sxdone; x->n.completionRefCon = NULL;
        if (x->proc) oah_midi_start_thread();
        r = (unsigned int)nm_MIDISendSysex(&x->n);
        if (r) free(x);
        break; }
    case MC_FLUSH: r = (unsigned int)nm_MIDIFlushOutput(R[3]); break;
    case MC_RESTART: r = (unsigned int)nm_MIDIRestart(); break;
    case MC_GETREFCONS: { void *a = 0, *b = 0; r = (unsigned int)nm_MIDIEndpointGetRefCons(R[3], &a, &b);
        oah_midi_put(R[4], (unsigned int)(uintptr_t)a); oah_midi_put(R[5], (unsigned int)(uintptr_t)b); break; }
    case MC_SETREFCONS: r = (unsigned int)nm_MIDIEndpointSetRefCons(R[3], (void *)(uintptr_t)R[4], (void *)(uintptr_t)R[5]); break;
    }
    if (s) CFRelease(s);
    if (oah_midi_log()) fprintf(stderr, "[midi] %s(%x, %x, %x) -> %d\n", g_mc_name[which], R[3], R[4], R[5], (int)r);
    g_gt_skip_ret = r;
}
#define OAH_MIDI_FIX(n) static void oah_fix_midi_##n(unsigned int *R) { oah_midi_call(MC_##n, R); }
OAH_MIDI_FIX(CLIENTCREATE) OAH_MIDI_FIX(CLIENTDISPOSE) OAH_MIDI_FIX(INPORT) OAH_MIDI_FIX(OUTPORT) OAH_MIDI_FIX(PORTDISPOSE)
OAH_MIDI_FIX(CONNECT) OAH_MIDI_FIX(DISCONNECT) OAH_MIDI_FIX(NDEV) OAH_MIDI_FIX(DEV) OAH_MIDI_FIX(DEV_NENT) OAH_MIDI_FIX(DEV_ENT)
OAH_MIDI_FIX(ENT_NSRC) OAH_MIDI_FIX(ENT_SRC) OAH_MIDI_FIX(ENT_NDST) OAH_MIDI_FIX(ENT_DST) OAH_MIDI_FIX(ENT_DEV) OAH_MIDI_FIX(NSRC)
OAH_MIDI_FIX(SRC) OAH_MIDI_FIX(NDST) OAH_MIDI_FIX(DST) OAH_MIDI_FIX(EP_ENT) OAH_MIDI_FIX(DSTCREATE) OAH_MIDI_FIX(SRCCREATE)
OAH_MIDI_FIX(EPDISPOSE) OAH_MIDI_FIX(NEXT) OAH_MIDI_FIX(EXT) OAH_MIDI_FIX(GETINT) OAH_MIDI_FIX(SETINT) OAH_MIDI_FIX(GETSTR)
OAH_MIDI_FIX(SETSTR) OAH_MIDI_FIX(GETDATA) OAH_MIDI_FIX(GETDICT) OAH_MIDI_FIX(GETPROPS) OAH_MIDI_FIX(FINDUID) OAH_MIDI_FIX(SEND)
OAH_MIDI_FIX(SYSEX) OAH_MIDI_FIX(RECEIVED) OAH_MIDI_FIX(FLUSH) OAH_MIDI_FIX(RESTART) OAH_MIDI_FIX(GETREFCONS) OAH_MIDI_FIX(SETREFCONS)
