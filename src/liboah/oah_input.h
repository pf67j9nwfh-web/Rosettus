/* oah_input.h - mouse/keyboard input for guests whose native event loop gets
   no input events from the WindowServer (included by liboah_mojave.c).

   SimCity 4 runs RunApplicationEventLoop with a Carbon handler on the
   application target for mouse and raw-key events.  Fallback only: in the
   19:00 run real mouse events (moves and a click) DID reach the dispatcher,
   so this stays idle whenever the dispatcher sees real input.  It exists for
   sessions where the full-screen context leaves the process without events.

   This module polls the real HID state on a main-run-loop timer (buttons,
   cursor, keys, modifiers), takes scroll-wheel events from a listen-only
   event tap, and sends standard Carbon events (kEventClassMouse /
   kEventClassKeyboard, same parameters the toolbox would supply) to the
   application target.  It stands down whenever real input events are seen
   at the dispatcher, and while another app is in front.  OAH_NO_SYNTH_INPUT=1
   disables it; OAH_EVTTRACE logs what it sends. */
typedef void *CGEventRef;
typedef struct { float x, y; } oah_cgpt;          /* CGPoint, CGFloat=float on i386 */
typedef CGEventRef (*oah_tapcb)(void *, unsigned int, CGEventRef, void *);
#define CGPoint oah_cgpt
#define CGEventSourceRef void *
#define CGEventType unsigned int
#define CGEventTapProxy void *
#define CGEventTapCallBack oah_tapcb
#define kCGEventFlagMaskAlphaShift 0x10000ULL
#define kCGEventFlagMaskShift 0x20000ULL
#define kCGEventFlagMaskControl 0x40000ULL
#define kCGEventFlagMaskAlternate 0x80000ULL
#define kCGEventFlagMaskCommand 0x100000ULL
#define kCGEventScrollWheel 22
#define kCGScrollWheelEventDeltaAxis1 11
#define kCGSessionEventTap 1
#define kCGHeadInsertEventTap 0
#define kCGEventTapOptionListenOnly 1
#define CGEventMaskBit(t) (1ULL << (t))

static double g_real_input_ts;                  /* last real mouse/key event seen at the dispatcher */
static int g_inp_tr = -1;
int inp_tr(void) { if (g_inp_tr < 0) g_inp_tr = getenv("OAH_EVTTRACE") ? 1 : 0; return g_inp_tr; }

static struct {
    void *(*CreateEvent)(void *, unsigned int, unsigned int, double, unsigned int, void **);
    int (*SetParam)(void *, unsigned int, unsigned int, unsigned int, const void *);
    int (*Send)(void *, void *);
    void (*Release)(void *);
    double (*Now)(void);
    void *(*AppTarget)(void);
    int (*GetFront)(void *);
    int (*GetCur)(void *);
    int (*Same)(const void *, const void *, unsigned char *);
    bool (*BtnState)(int, int);
    bool (*KeyState)(int, unsigned short);
    unsigned long long (*FlagsState)(int);
    CGEventRef (*EvCreate)(CGEventSourceRef);
    CGPoint (*EvLoc)(CGEventRef);
    long long (*EvInt)(CGEventRef, int);
    CFMachPortRef (*TapCreate)(int, int, int, unsigned long long, CGEventTapCallBack, void *);
    int ok;
} I;

static struct {
    int btn[3];
    short h, v;
    unsigned char keys[128];
    unsigned int mods;
    double last_click_t; short last_click_h, last_click_v; unsigned int clicks;
    unsigned long sent;
} S;

static int inp_load(void) {
    if (I.ok) return I.ok > 0;
    I.CreateEvent = (void *(*)(void *, unsigned int, unsigned int, double, unsigned int, void **))oah_csym("CreateEvent");
    I.SetParam = (int (*)(void *, unsigned int, unsigned int, unsigned int, const void *))oah_csym("SetEventParameter");
    I.Send = (int (*)(void *, void *))oah_csym("SendEventToEventTarget");
    I.Release = (void (*)(void *))oah_csym("ReleaseEvent");
    I.Now = (double (*)(void))oah_csym("GetCurrentEventTime");
    I.AppTarget = (void *(*)(void))oah_csym("GetApplicationEventTarget");
    I.GetFront = (int (*)(void *))oah_csym("GetFrontProcess");
    I.GetCur = (int (*)(void *))oah_csym("GetCurrentProcess");
    I.Same = (int (*)(const void *, const void *, unsigned char *))oah_csym("SameProcess");
    I.BtnState = (bool (*)(int, int))dlsym(RTLD_DEFAULT, "CGEventSourceButtonState");
    I.KeyState = (bool (*)(int, unsigned short))dlsym(RTLD_DEFAULT, "CGEventSourceKeyState");
    I.FlagsState = (unsigned long long (*)(int))dlsym(RTLD_DEFAULT, "CGEventSourceFlagsState");
    I.EvCreate = (CGEventRef (*)(CGEventSourceRef))dlsym(RTLD_DEFAULT, "CGEventCreate");
    I.EvLoc = (CGPoint (*)(CGEventRef))dlsym(RTLD_DEFAULT, "CGEventGetLocation");
    I.EvInt = (long long (*)(CGEventRef, int))dlsym(RTLD_DEFAULT, "CGEventGetIntegerValueField");
    I.TapCreate = (CFMachPortRef (*)(int, int, int, unsigned long long, CGEventTapCallBack, void *))dlsym(RTLD_DEFAULT, "CGEventTapCreate");
    I.ok = (I.CreateEvent && I.SetParam && I.Send && I.Release && I.Now && I.AppTarget &&
            I.BtnState && I.KeyState && I.FlagsState && I.EvCreate && I.EvLoc) ? 1 : -1;
    if (I.ok < 0) fprintf(stderr, "[input] missing native symbols, synthetic input disabled\n");
    return I.ok > 0;
}

static int inp_front(void) {
    unsigned int a[2], b[2]; unsigned char same = 0;
    if (!I.GetFront || !I.GetCur || !I.Same) return 1;
    if (I.GetFront(a) || I.GetCur(b) || I.Same(a, b, &same)) return 1;
    return same;
}

/* CGEventFlags -> Carbon modifier bits (Events.h) */
static unsigned int inp_mods(void) {
    unsigned long long f = I.FlagsState(1 /* HIDSystemState */);
    unsigned int m = 0;
    if (f & kCGEventFlagMaskCommand) m |= 0x0100;      /* cmdKey */
    if (f & kCGEventFlagMaskShift) m |= 0x0200;        /* shiftKey */
    if (f & kCGEventFlagMaskAlphaShift) m |= 0x0400;   /* alphaLock */
    if (f & kCGEventFlagMaskAlternate) m |= 0x0800;    /* optionKey */
    if (f & kCGEventFlagMaskControl) m |= 0x1000;      /* controlKey */
    return m;
}

static void inp_send_mouse(unsigned int kind, unsigned short button, unsigned int clicks, int wheel) {
    void *ev = 0;
    short pt[2];
    unsigned int mods = S.mods, chord = (S.btn[0] ? 1 : 0) | (S.btn[1] ? 2 : 0) | (S.btn[2] ? 4 : 0);
    if (I.CreateEvent(0, 'mous', kind, I.Now(), 1 /* kEventAttributeUserEvent */, &ev) || !ev) return;
    pt[0] = S.v; pt[1] = S.h;                           /* Point is {v, h} */
    I.SetParam(ev, 'mloc', 'QDpt', 4, pt);
    I.SetParam(ev, 'kmod', 'magn', 4, &mods);
    if (kind == 1 || kind == 2) {
        I.SetParam(ev, 'mbtn', 'mbtn', 2, &button);
        I.SetParam(ev, 'ccnt', 'magn', 4, &clicks);
    }
    if (kind != 10) I.SetParam(ev, 'mchd', 'magn', 4, &chord);
    if (kind == 10) {
        unsigned short axis = 1;                       /* kEventMouseWheelAxisY */
        I.SetParam(ev, 'mwax', 'mwax', 2, &axis);
        I.SetParam(ev, 'mwdl', 'long', 4, &wheel);
    }
    I.Send(ev, I.AppTarget());
    I.Release(ev);
    S.sent++;
    if (inp_tr() && (kind != 5 && kind != 6 ? 1 : (S.sent % 50) == 0))
        fprintf(stderr, "[input] mouse kind=%u btn=%u clicks=%u at (%d,%d) mods=%04x wheel=%d\n", kind, button, clicks, S.h, S.v, mods, wheel);
}

/* US layout char codes for raw key events (kEventParamKeyMacCharCodes) */
static unsigned char inp_char(unsigned int kc, unsigned int mods) {
    static const char lo[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\rlj'k;\\,/nm.\t `";
    static const char hi[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\rLJ\"K:|<?NM>\t ~";
    int shift = (mods & 0x0200) != 0;
    if (kc < sizeof lo - 1) { char c = shift ? hi[kc] : lo[kc]; if ((mods & 0x0400) && c >= 'a' && c <= 'z') c -= 32; return (unsigned char)c; }
    switch (kc) {
    case 0x33: return 8;  case 0x35: return 27; case 0x75: return 127; case 0x4C: return 3;
    case 0x7B: return 28; case 0x7C: return 29; case 0x7E: return 30; case 0x7D: return 31;
    case 0x73: return 1;  case 0x77: return 4;  case 0x74: return 11; case 0x79: return 12;
    case 0x24: return 13; case 0x30: return 9;  case 0x31: return 32;
    case 0x7A: case 0x78: case 0x63: case 0x76: case 0x60: case 0x61: case 0x62: case 0x64:
    case 0x65: case 0x6D: case 0x67: case 0x6F: return 16;   /* F1..F12: kFunctionKeyCharCode */
    case 0x52: return '0'; case 0x53: return '1'; case 0x54: return '2'; case 0x55: return '3';
    case 0x56: return '4'; case 0x57: return '5'; case 0x58: return '6'; case 0x59: return '7';
    case 0x5B: return '8'; case 0x5C: return '9'; case 0x41: return '.'; case 0x43: return '*';
    case 0x45: return '+'; case 0x4E: return '-'; case 0x4B: return '/'; case 0x51: return '=';
    }
    return 0;
}

static void inp_send_key(unsigned int kind, unsigned int kc) {
    void *ev = 0;
    unsigned int mods = S.mods, kbt = 40;
    unsigned char ch = inp_char(kc, mods);
    if (I.CreateEvent(0, 'keyb', kind, I.Now(), 1, &ev) || !ev) return;
    I.SetParam(ev, 'kmod', 'magn', 4, &mods);
    if (kind != 4) {
        I.SetParam(ev, 'kcod', 'magn', 4, &kc);
        I.SetParam(ev, 'kchr', 'TEXT', 1, &ch);
        I.SetParam(ev, 'kbdt', 'magn', 4, &kbt);
    }
    I.Send(ev, I.AppTarget());
    I.Release(ev);
    if (inp_tr()) fprintf(stderr, "[input] key kind=%u code=0x%02x char=%u mods=%04x\n", kind, kc, ch, mods);
}

static int inp_active(void) {
    if (I.Now() - g_real_input_ts < 2.0) return 0;      /* real events are flowing: stand down */
    return inp_front();
}

static CGEventRef inp_tap(CGEventTapProxy p, CGEventType type, CGEventRef e, void *ud) {
    (void)p; (void)ud;
    if (type == kCGEventScrollWheel && I.EvInt && inp_active()) {
        int d = (int)I.EvInt(e, kCGScrollWheelEventDeltaAxis1);
        if (d) inp_send_mouse(10, 0, 0, d);
    }
    return e;
}

static void inp_tick(CFRunLoopTimerRef t, void *ud) {
    static const int mapb[3] = { 0, 1, 2 };            /* CG button -> Carbon button-1 */
    CGEventRef e;
    CGPoint p;
    unsigned int i, mods;
    short h, v;
    (void)t; (void)ud;
    if (!inp_active()) return;
    mods = inp_mods();
    if (mods != S.mods) { S.mods = mods; inp_send_key(4, 0); }
    if ((e = I.EvCreate(0))) { p = I.EvLoc(e); CFRelease(e); } else return;
    h = (short)p.x; v = (short)p.y;
    if (h != S.h || v != S.v) {
        S.h = h; S.v = v;
        inp_send_mouse(S.btn[0] || S.btn[1] || S.btn[2] ? 6 : 5, 0, 0, 0);
    }
    for (i = 0; i < 3; i++) {
        int down = I.BtnState(1, mapb[i]) ? 1 : 0;
        if (down == S.btn[i]) continue;
        S.btn[i] = down;
        if (down) {
            double now = I.Now();
            if (i == 0 && now - S.last_click_t < 0.45 && abs(h - S.last_click_h) < 5 && abs(v - S.last_click_v) < 5) S.clicks++;
            else S.clicks = 1;
            if (i == 0) { S.last_click_t = now; S.last_click_h = h; S.last_click_v = v; }
        }
        inp_send_mouse(down ? 1 : 2, (unsigned short)(i + 1), i == 0 ? S.clicks : 1, 0);
    }
    for (i = 0; i < 128; i++) {
        int down;
        if (i >= 0x36 && i <= 0x3F) continue;            /* modifier keys: reported via kmod */
        down = I.KeyState(1, (unsigned short)i) ? 1 : 0;
        if (down == S.keys[i]) continue;
        S.keys[i] = (unsigned char)down;
        inp_send_key(down ? 1 : 3, i);
    }
}

static void oah_input_start(void) {
    static int started;
    CFRunLoopTimerRef t;
    if (started || getenv("OAH_NO_SYNTH_INPUT") || !inp_load()) return;
    started = 1;
    memset(&S, 0, sizeof S);
    for (unsigned int i = 0; i < 128; i++) S.keys[i] = I.KeyState(1, (unsigned short)i) ? 1 : 0;
    S.mods = inp_mods();
    t = CFRunLoopTimerCreate(0, CFAbsoluteTimeGetCurrent() + 0.5, 0.008, 0, 0, inp_tick, 0);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
    if (I.TapCreate) {
        CFMachPortRef tap = I.TapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
                                        CGEventMaskBit(kCGEventScrollWheel), inp_tap, 0);
        if (tap) {
            CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(0, tap, 0);
            CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
        }
        fprintf(stderr, "[input] synthetic input on (scroll tap %s)\n", tap ? "on" : "unavailable");
    } else fprintf(stderr, "[input] synthetic input on (no scroll tap)\n");
}
