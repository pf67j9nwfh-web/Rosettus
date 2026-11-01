/* oah_speech.h - the Speech Manager (SpeechSynthesis) for the guest, on Mojave's own.

   The 10.6 PowerPC SpeechSynthesis drives its channels through Snow Leopard's speech server, which
   Mojave does not have in that form: a channel never finishes, and StopSpeech waits in
   StopSpeechChannel (10 ms sleeps) for an acknowledgement that never comes. Kid Pix Deluxe 4 froze
   there when an animated sticker was played. The public calls are answered by Mojave's 32-bit
   Speech Manager instead (gtrace fixes, guest code skipped):
     - a guest SpeechChannel is the native channel pointer (the guest never looks inside one);
     - VoiceSpec / VoiceDescription / SpeechStatusInfo / Fixed / OSType values are converted
       between big- and little-endian;
     - callbacks the guest installs (done, text done, sync, error, phoneme, word) are called on the
       guest from native ones, with the guest's own refCon;
     - TextToPhonemes / UseDictionary / output to a file or device are not supported (paramErr /
       ignored / siUnknownInfoType).
   OAH_NO_SPEECH=1 leaves the guest's SpeechSynthesis alone. OAH_SPEECHTRACE=1 logs. */
typedef struct { unsigned int creator, id; } oah_vspec;
#pragma pack(push, 2)
typedef struct { int length; oah_vspec voice; int version; unsigned char name[64], comment[256];
                 short gender, age, script, language, region; int reserved[4]; } oah_vdesc;
typedef struct { unsigned char busy, paused; int left; short phon; } oah_sstat;
typedef struct { short count; short oldest; int oldPos; short newest; int newPos; } oah_serr;
#pragma pack(pop)
#define OAH_SPCH_MAX 32
typedef struct { void *n; unsigned int refcon, done, tdone, sync, err, phon, word; } oah_spch_t;
static oah_spch_t g_spch[OAH_SPCH_MAX];
static pthread_mutex_t g_spch_mu = PTHREAD_MUTEX_INITIALIZER;
static int oah_sptr(void) { static int t = -1; if (t < 0) t = getenv("OAH_SPEECHTRACE") ? 1 : 0; return t; }
static int oah_want_speech(void) { return !getenv("OAH_NO_SPEECH"); }
static void *oah_spf(const char *n) {
    static void *h;
    if (!h) h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY);
    return h ? dlsym(h, n) : 0;
}
#define SPF(ret, name, args) static ret (*p_##name) args; if (!p_##name) p_##name = (ret (*) args)oah_spf(#name)
static unsigned int sp_be32(unsigned int v) { return OSSwapInt32(v); }
static unsigned int sp_rd32(unsigned int a) { return a ? OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)a) : 0; }
static void sp_wr32(unsigned int a, unsigned int v) { if (a) *(unsigned int *)(uintptr_t)a = OSSwapHostToBigInt32(v); }
static void sp_wr16(unsigned int a, unsigned short v) { if (a) *(unsigned short *)(uintptr_t)a = OSSwapHostToBigInt16(v); }
static oah_spch_t *oah_spch_find(void *n) {
    int i;
    for (i = 0; i < OAH_SPCH_MAX; i++) if (n && g_spch[i].n == n) return &g_spch[i];
    return 0;
}
static void oah_sp_ret(int v) { g_gt_skip = 1; g_gt_skip_ret = (unsigned int)v; }
/* native callbacks: refCon is our table index + 1 */
static oah_spch_t *oah_spch_ref(long ref) { return ref > 0 && ref <= OAH_SPCH_MAX && g_spch[ref - 1].n ? &g_spch[ref - 1] : 0; }
static void oah_sp_done(void *chan, long ref) {
    oah_spch_t *c = oah_spch_ref(ref); unsigned int a[2];
    if (oah_sptr()) fprintf(stderr, "[speech] done on %p\n", chan);
    if (!c || !c->done) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon;
    oah_callppc_n("speech_done", c->done, 2, a);
}
static void oah_sp_sync(void *chan, long ref, unsigned int msg) {
    oah_spch_t *c = oah_spch_ref(ref); unsigned int a[3];
    if (!c || !c->sync) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon; a[2] = msg;
    oah_callppc_n("speech_sync", c->sync, 3, a);
}
static void oah_sp_err(void *chan, long ref, short e, long pos) {
    oah_spch_t *c = oah_spch_ref(ref); unsigned int a[4];
    if (!c || !c->err) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon; a[2] = (unsigned int)(int)e; a[3] = (unsigned int)pos;
    oah_callppc_n("speech_err", c->err, 4, a);
}
static void oah_sp_phon(void *chan, long ref, short op) {
    oah_spch_t *c = oah_spch_ref(ref); unsigned int a[3];
    if (!c || !c->phon) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon; a[2] = (unsigned int)(int)op;
    oah_callppc_n("speech_phon", c->phon, 3, a);
}
static void oah_sp_word(void *chan, long ref, unsigned long pos, unsigned short len) {
    oah_spch_t *c = oah_spch_ref(ref); unsigned int a[4];
    if (!c || !c->word) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon; a[2] = (unsigned int)pos; a[3] = len;
    oah_callppc_n("speech_word", c->word, 4, a);
}
static void oah_sp_tdone(void *chan, long ref, const void **next, unsigned long *len, int *flags) {
    oah_spch_t *c = oah_spch_ref(ref);
    unsigned int a[5], gnext = 0, glen = 0, gfl = 0;   /* the guest writes these big-endian */
    if (!c || !c->tdone) return;
    a[0] = (unsigned int)(uintptr_t)chan; a[1] = c->refcon;
    a[2] = (unsigned int)(uintptr_t)&gnext; a[3] = (unsigned int)(uintptr_t)&glen; a[4] = (unsigned int)(uintptr_t)&gfl;
    oah_callppc_n("speech_tdone", c->tdone, 5, a);
    if (next) *next = (const void *)(uintptr_t)OSSwapBigToHostInt32(gnext);
    if (len) *len = OSSwapBigToHostInt32(glen);
    if (flags) *flags = (int)OSSwapBigToHostInt32(gfl);
}
static void oah_fix_sp_count(unsigned int *R) {          /* CountVoices(SInt16 *) */
    SPF(short, CountVoices, (short *)); short n = 0; short e;
    if (!p_CountVoices) return;
    e = p_CountVoices(&n); sp_wr16(R[3], (unsigned short)n); oah_sp_ret(e);
}
static void oah_fix_sp_indvoice(unsigned int *R) {       /* GetIndVoice(SInt16 index, VoiceSpec *) */
    SPF(short, GetIndVoice, (short, oah_vspec *)); oah_vspec v = { 0, 0 }; short e;
    if (!p_GetIndVoice) return;
    e = p_GetIndVoice((short)R[3], &v);
    if (R[4]) { sp_wr32(R[4], v.creator); sp_wr32(R[4] + 4, v.id); }
    oah_sp_ret(e);
}
static void oah_vdesc_out(unsigned int g, const oah_vdesc *d, int len) {
    oah_vdesc b = *d; int i;
    if (!g || len <= 0) return;
    b.length = (int)sp_be32((unsigned int)d->length); b.voice.creator = sp_be32(d->voice.creator); b.voice.id = sp_be32(d->voice.id);
    b.version = (int)sp_be32((unsigned int)d->version);
    b.gender = (short)OSSwapInt16(d->gender); b.age = (short)OSSwapInt16(d->age); b.script = (short)OSSwapInt16(d->script);
    b.language = (short)OSSwapInt16(d->language); b.region = (short)OSSwapInt16(d->region);
    for (i = 0; i < 4; i++) b.reserved[i] = (int)sp_be32((unsigned int)d->reserved[i]);
    memcpy((void *)(uintptr_t)g, &b, len < (int)sizeof b ? len : (int)sizeof b);
}
static void oah_fix_sp_vdesc(unsigned int *R) {          /* GetVoiceDescription(const VoiceSpec *, VoiceDescription *, long) */
    SPF(short, GetVoiceDescription, (const oah_vspec *, oah_vdesc *, long)); oah_vspec v, *vp = 0; oah_vdesc d; short e;
    if (!p_GetVoiceDescription) return;
    if (R[3]) { v.creator = sp_rd32(R[3]); v.id = sp_rd32(R[3] + 4); vp = &v; }
    memset(&d, 0, sizeof d);
    e = p_GetVoiceDescription(vp, &d, sizeof d);
    oah_vdesc_out(R[4], &d, (int)R[5]);
    oah_sp_ret(e);
}
static void oah_fix_sp_vinfo(unsigned int *R) {          /* GetVoiceInfo(const VoiceSpec *, OSType, void *) */
    SPF(short, GetVoiceInfo, (const oah_vspec *, unsigned int, void *)); oah_vspec v, *vp = 0; oah_vdesc d; short e;
    if (!p_GetVoiceInfo) return;
    if (R[3]) { v.creator = sp_rd32(R[3]); v.id = sp_rd32(R[3] + 4); vp = &v; }
    if (R[4] != 'info') { oah_sp_ret(-231); return; }   /* siUnknownInfoType ('fref' needs an FSSpec) */
    memset(&d, 0, sizeof d); d.length = sizeof d;
    e = p_GetVoiceInfo(vp, R[4], &d);
    oah_vdesc_out(R[5], &d, (int)sizeof d);
    oah_sp_ret(e);
}
static void oah_fix_sp_new(unsigned int *R) {            /* NewSpeechChannel(VoiceSpec *, SpeechChannel *) */
    SPF(short, NewSpeechChannel, (oah_vspec *, void **)); SPF(short, SetSpeechInfo, (void *, unsigned int, const void *));
    oah_vspec v, *vp = 0; void *ch = 0; short e; int i;
    if (!p_NewSpeechChannel) return;
    if (R[3]) { v.creator = sp_rd32(R[3]); v.id = sp_rd32(R[3] + 4); vp = &v; }
    e = p_NewSpeechChannel(vp, &ch);
    if (!e && ch) {
        pthread_mutex_lock(&g_spch_mu);
        for (i = 0; i < OAH_SPCH_MAX && g_spch[i].n; i++) ;
        if (i < OAH_SPCH_MAX) { memset(&g_spch[i], 0, sizeof g_spch[i]); g_spch[i].n = ch; }
        pthread_mutex_unlock(&g_spch_mu);
        if (i < OAH_SPCH_MAX && p_SetSpeechInfo) p_SetSpeechInfo(ch, 'refc', (const void *)(long)(i + 1));
    }
    sp_wr32(R[4], (unsigned int)(uintptr_t)ch);
    if (oah_sptr()) fprintf(stderr, "[speech] NewSpeechChannel(%08x %08x) -> %d, channel %p\n", vp ? v.creator : 0, vp ? v.id : 0, e, ch);
    oah_sp_ret(e);
}
static void oah_fix_sp_dispose(unsigned int *R) {
    SPF(short, DisposeSpeechChannel, (void *)); oah_spch_t *c; short e;
    if (!p_DisposeSpeechChannel) return;
    pthread_mutex_lock(&g_spch_mu); c = oah_spch_find((void *)(uintptr_t)R[3]); pthread_mutex_unlock(&g_spch_mu);
    e = p_DisposeSpeechChannel((void *)(uintptr_t)R[3]);
    if (c) c->n = 0;
    oah_sp_ret(e);
}
static void oah_fix_sp_string(unsigned int *R) {         /* SpeakString(ConstStr255Param) */
    SPF(short, SpeakString, (const unsigned char *));
    if (!p_SpeakString) return;
    if (oah_sptr() && R[3]) fprintf(stderr, "[speech] SpeakString \"%.*s\"\n", *(unsigned char *)(uintptr_t)R[3], (char *)(uintptr_t)R[3] + 1);
    oah_sp_ret(p_SpeakString((const unsigned char *)(uintptr_t)R[3]));
}
static void oah_fix_sp_text(unsigned int *R) {           /* SpeakText(chan, buf, len) */
    SPF(short, SpeakText, (void *, const void *, unsigned long));
    if (!p_SpeakText) return;
    if (oah_sptr()) fprintf(stderr, "[speech] SpeakText(%08x) \"%.*s\"\n", R[3], (int)(R[5] < 200 ? R[5] : 200), (char *)(uintptr_t)R[4]);
    oah_sp_ret(p_SpeakText((void *)(uintptr_t)R[3], (const void *)(uintptr_t)R[4], R[5]));
}
static void oah_fix_sp_buffer(unsigned int *R) {         /* SpeakBuffer(chan, buf, len, controlFlags) */
    SPF(short, SpeakBuffer, (void *, const void *, unsigned long, int));
    if (!p_SpeakBuffer) return;
    if (oah_sptr()) fprintf(stderr, "[speech] SpeakBuffer(%08x, flags %x) \"%.*s\"\n", R[3], R[6], (int)(R[5] < 200 ? R[5] : 200), (char *)(uintptr_t)R[4]);
    oah_sp_ret(p_SpeakBuffer((void *)(uintptr_t)R[3], (const void *)(uintptr_t)R[4], R[5], (int)R[6]));
}
static void oah_fix_sp_cfstring(unsigned int *R) {       /* SpeakCFString(chan, CFStringRef, CFDictionaryRef options) */
    SPF(short, SpeakCFString, (void *, CFStringRef, CFDictionaryRef));
    static unsigned int gcs; unsigned int a[4]; char buf[4096]; CFStringRef s; short e;
    if (!p_SpeakCFString) return;
    if (!gcs) gcs = oah_guest_sym("CoreFoundation", "_CFStringGetCString");
    buf[0] = 0;
    if (gcs && R[4]) { a[0] = R[4]; a[1] = (unsigned int)(uintptr_t)buf; a[2] = sizeof buf; a[3] = 0x08000100u; oah_callppc_n("CFStringGetCString", gcs, 4, a); }
    s = CFStringCreateWithCString(0, buf, kCFStringEncodingUTF8);
    if (!s) { oah_sp_ret(-50); return; }
    if (oah_sptr()) fprintf(stderr, "[speech] SpeakCFString(%08x) \"%.200s\"\n", R[3], buf);
    e = p_SpeakCFString((void *)(uintptr_t)R[3], s, 0);
    CFRelease(s);
    oah_sp_ret(e);
}
static void oah_fix_sp_stop(unsigned int *R) {
    SPF(short, StopSpeech, (void *));
    if (!p_StopSpeech) return;
    oah_sp_ret(p_StopSpeech((void *)(uintptr_t)R[3]));
}
static void oah_fix_sp_stopat(unsigned int *R) {
    SPF(short, StopSpeechAt, (void *, int));
    if (!p_StopSpeechAt) return;
    oah_sp_ret(p_StopSpeechAt((void *)(uintptr_t)R[3], (int)R[4]));
}
static void oah_fix_sp_pauseat(unsigned int *R) {
    SPF(short, PauseSpeechAt, (void *, int));
    if (!p_PauseSpeechAt) return;
    oah_sp_ret(p_PauseSpeechAt((void *)(uintptr_t)R[3], (int)R[4]));
}
static void oah_fix_sp_continue(unsigned int *R) {
    SPF(short, ContinueSpeech, (void *));
    if (!p_ContinueSpeech) return;
    oah_sp_ret(p_ContinueSpeech((void *)(uintptr_t)R[3]));
}
static void oah_fix_sp_busy(unsigned int *R) {
    SPF(short, SpeechBusy, (void));
    (void)R;
    if (!p_SpeechBusy) return;
    oah_sp_ret(p_SpeechBusy());
}
static void oah_fix_sp_busysys(unsigned int *R) {
    SPF(short, SpeechBusySystemWide, (void));
    (void)R;
    if (!p_SpeechBusySystemWide) return;
    oah_sp_ret(p_SpeechBusySystemWide());
}
static void oah_fix_sp_setrate(unsigned int *R) {        /* SetSpeechRate / SetSpeechPitch (chan, Fixed) */
    SPF(short, SetSpeechInfo, (void *, unsigned int, const void *)); int f = (int)R[4];
    if (!p_SetSpeechInfo) return;
    oah_sp_ret(p_SetSpeechInfo((void *)(uintptr_t)R[3], 'rate', &f));
}
static void oah_fix_sp_setpitch(unsigned int *R) {
    SPF(short, SetSpeechInfo, (void *, unsigned int, const void *)); int f = (int)R[4];
    if (!p_SetSpeechInfo) return;
    oah_sp_ret(p_SetSpeechInfo((void *)(uintptr_t)R[3], 'pbas', &f));
}
static void oah_fix_sp_getrate(unsigned int *R) {        /* GetSpeechRate / GetSpeechPitch (chan, Fixed *) */
    SPF(short, GetSpeechInfo, (void *, unsigned int, void *)); int f = 0; short e;
    if (!p_GetSpeechInfo) return;
    e = p_GetSpeechInfo((void *)(uintptr_t)R[3], 'rate', &f); sp_wr32(R[4], (unsigned int)f); oah_sp_ret(e);
}
static void oah_fix_sp_getpitch(unsigned int *R) {
    SPF(short, GetSpeechInfo, (void *, unsigned int, void *)); int f = 0; short e;
    if (!p_GetSpeechInfo) return;
    e = p_GetSpeechInfo((void *)(uintptr_t)R[3], 'pbas', &f); sp_wr32(R[4], (unsigned int)f); oah_sp_ret(e);
}
static void oah_fix_sp_setinfo(unsigned int *R) {        /* SetSpeechInfo(chan, OSType selector, const void *info) */
    SPF(short, SetSpeechInfo, (void *, unsigned int, const void *));
    void *ch = (void *)(uintptr_t)R[3]; unsigned int sel = R[4], info = R[5], v; oah_vspec vs; oah_spch_t *c; short e = 0;
    void *cb = 0; unsigned int *slot = 0;
    if (!p_SetSpeechInfo) return;
    pthread_mutex_lock(&g_spch_mu); c = oah_spch_find(ch); pthread_mutex_unlock(&g_spch_mu);
    if (oah_sptr()) fprintf(stderr, "[speech] SetSpeechInfo(%p, '%c%c%c%c', %08x)\n", ch, sel >> 24, sel >> 16, sel >> 8, sel, info);
    switch (sel) {
    case 'rate': case 'pbas': case 'pmod': case 'volm': case 'inpt': case 'char': case 'nmbr':
        v = sp_rd32(info); e = p_SetSpeechInfo(ch, sel, &v); break;
    case 'cvox': vs.creator = sp_rd32(info); vs.id = info ? sp_rd32(info + 4) : 0; e = p_SetSpeechInfo(ch, sel, info ? &vs : 0); break;
    case 'rset': case 'dlim': e = p_SetSpeechInfo(ch, sel, (const void *)(uintptr_t)info); break;
    case 'refc': if (c) c->refcon = info; break;     /* ours stays the native refCon */
    case 'sdcb': cb = (void *)oah_sp_done; if (c) slot = &c->done; break;
    case 'tdcb': cb = (void *)oah_sp_tdone; if (c) slot = &c->tdone; break;
    case 'sycb': cb = (void *)oah_sp_sync; if (c) slot = &c->sync; break;
    case 'ercb': cb = (void *)oah_sp_err; if (c) slot = &c->err; break;
    case 'phcb': cb = (void *)oah_sp_phon; if (c) slot = &c->phon; break;
    case 'wdcb': cb = (void *)oah_sp_word; if (c) slot = &c->word; break;
    default: e = -231;                                 /* siUnknownInfoType */
    }
    if (cb) {
        if (!slot) e = -50;
        else { *slot = info; e = p_SetSpeechInfo(ch, sel, info ? cb : 0); }
    }
    oah_sp_ret(e);
}
static void oah_fix_sp_getinfo(unsigned int *R) {        /* GetSpeechInfo(chan, OSType selector, void *info) */
    SPF(short, GetSpeechInfo, (void *, unsigned int, void *));
    void *ch = (void *)(uintptr_t)R[3]; unsigned int sel = R[4], info = R[5], v = 0; oah_spch_t *c; short e = 0;
    if (!p_GetSpeechInfo) return;
    switch (sel) {
    case 'stat': {
        oah_sstat s; memset(&s, 0, sizeof s);
        e = p_GetSpeechInfo(ch, sel, &s);
        if (info) { unsigned char *o = (unsigned char *)(uintptr_t)info; o[0] = s.busy; o[1] = s.paused; sp_wr32(info + 2, (unsigned int)s.left); sp_wr16(info + 6, (unsigned short)s.phon); }
        break; }
    case 'erro': {
        oah_serr s; memset(&s, 0, sizeof s);
        e = p_GetSpeechInfo(ch, sel, &s);
        if (info) { sp_wr16(info, (unsigned short)s.count); sp_wr16(info + 2, (unsigned short)s.oldest); sp_wr32(info + 4, (unsigned int)s.oldPos);
                    sp_wr16(info + 8, (unsigned short)s.newest); sp_wr32(info + 10, (unsigned int)s.newPos); }
        break; }
    case 'rate': case 'pbas': case 'pmod': case 'volm': case 'inpt': case 'char': case 'nmbr': case 'sync':
        e = p_GetSpeechInfo(ch, sel, &v); sp_wr32(info, v); break;
    case 'refc':
        pthread_mutex_lock(&g_spch_mu); c = oah_spch_find(ch); pthread_mutex_unlock(&g_spch_mu);
        sp_wr32(info, c ? c->refcon : 0); break;
    case 'cvox': {
        oah_vspec vs = { 0, 0 };
        e = p_GetSpeechInfo(ch, sel, &vs); sp_wr32(info, vs.creator); sp_wr32(info + 4, vs.id); break; }
    default: e = -231;
    }
    oah_sp_ret(e);
}
static void oah_fix_sp_phonemes(unsigned int *R) { (void)R; oah_sp_ret(-50); }   /* TextToPhonemes: guest Handle out */
static void oah_fix_sp_dict(unsigned int *R) { (void)R; oah_sp_ret(0); }          /* UseDictionary: ignored */
