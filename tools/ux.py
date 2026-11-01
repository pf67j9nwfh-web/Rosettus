# ux.py: person-like interaction steps with checks, for testing PPC apps on Mojave.
# Usage: python ux.py STEP [/ STEP ...]
#   click X Y [HOLD]      glide to X,Y and click (HOLD seconds, default 0.08)
#   dclick X Y            double-click
#   drag X1 Y1 X2 Y2      press, move in steps, release
#   hover X Y             glide to X,Y and rest
#   key CODE [cmd,shift,alt,ctrl]   key press with modifiers
#   type TEXT             type text with real US key codes (letters, digits, space, .,)
#   wait SEC
#   cur LABEL             record the system cursor (name guess, size, hotspot) + PNG crop
#   shot LABEL            full screenshot scaled to 900 wide (with cursor)
#   crop LABEL X Y W H    screenshot region in points (2x pixels saved)
#   front                 print the frontmost app and the window under the mouse
# Results go to /tmp/ux/<LABEL>.png; "cur" steps are also put side by side in /tmp/ux/cursors.png.
import sys, time, os, subprocess
import Quartz as Q
from AppKit import NSCursor, NSBitmapImageRep, NSWorkspace, NSPNGFileType
from Foundation import NSURL

OUT = "/tmp/ux"
if not os.path.isdir(OUT): os.makedirs(OUT)
SRC = Q.CGEventSourceCreate(Q.kCGEventSourceStateHIDSystemState)
pos = [None]

def post(t, x, y, clicks=1):
    e = Q.CGEventCreateMouseEvent(SRC, t, (x, y), Q.kCGMouseButtonLeft)
    if clicks > 1: Q.CGEventSetIntegerValueField(e, Q.kCGMouseEventClickState, clicks)
    Q.CGEventPost(Q.kCGHIDEventTap, e)

def cur_xy():
    return Q.CGEventGetLocation(Q.CGEventCreate(None))

def glide(x, y, rest=0.25):
    sx, sy = cur_xy()
    n = 12
    for k in range(1, n + 1):
        post(Q.kCGEventMouseMoved, sx + (x - sx) * k / float(n), sy + (y - sy) * k / float(n))
        time.sleep(0.012)
    time.sleep(rest)

KEYS = {'a':0,'s':1,'d':2,'f':3,'h':4,'g':5,'z':6,'x':7,'c':8,'v':9,'b':11,'q':12,'w':13,'e':14,'r':15,
        'y':16,'t':17,'1':18,'2':19,'3':20,'4':21,'6':22,'5':23,'9':25,'7':26,'8':28,'0':29,'o':31,'u':32,
        'i':34,'p':35,'l':37,'j':38,'k':40,'n':45,'m':46,' ':49,'.':47,',':43,'\n':36,
        '=':24,'-':27,'/':44,';':41,"'":39,'[':33,']':30,'`':50,'\\':42,'\t':48}
SHIFTED = {'+':'=','*':'8','(':'9',')':'0',':':';','!':'1','@':'2','$':'4','%':'5','?':'/','"':"'",'_':'-',
           '~':'`','#':'3','^':'6','&':'7','<':',','>':'.','{':'[','}':']','|':'\\'}
MODS = {'cmd': Q.kCGEventFlagMaskCommand, 'shift': Q.kCGEventFlagMaskShift,
        'alt': Q.kCGEventFlagMaskAlternate, 'ctrl': Q.kCGEventFlagMaskControl}

MODKEYS = [(Q.kCGEventFlagMaskCommand, 55), (Q.kCGEventFlagMaskShift, 56), (Q.kCGEventFlagMaskAlternate, 58), (Q.kCGEventFlagMaskControl, 59)]

def modkeys(mods, down):
    # real modifier keys: flagsChanged down before the key, up after (else the system keeps them held)
    cur = mods if not down else 0
    order = MODKEYS if down else list(reversed(MODKEYS))
    for m, kc in order:
        if mods & m:
            cur = (cur | m) if down else (cur & ~m)
            e = Q.CGEventCreateKeyboardEvent(SRC, kc, down); Q.CGEventSetFlags(e, cur); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.03)

def key(code, mods=0):
    if mods: modkeys(mods, True)
    for down in (True, False):
        e = Q.CGEventCreateKeyboardEvent(SRC, code, down)
        Q.CGEventSetFlags(e, mods)
        Q.CGEventPost(Q.kCGHIDEventTap, e)
        time.sleep(0.03)
    if mods: modkeys(mods, False)

def reset_mods():
    for m, kc in MODKEYS:
        e = Q.CGEventCreateKeyboardEvent(SRC, kc, False); Q.CGEventSetFlags(e, 0); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.02)

def capture(path):
    subprocess.call(["screencapture", "-C", "-x", path])

def load(path):
    s = Q.CGImageSourceCreateWithURL(NSURL.fileURLWithPath_(path), None)
    return Q.CGImageSourceCreateImageAtIndex(s, 0, None)

def save(im, path):
    d = Q.CGImageDestinationCreateWithURL(NSURL.fileURLWithPath_(path), "public.png", 1, None)
    Q.CGImageDestinationAddImage(d, im, None); Q.CGImageDestinationFinalize(d)

def crop(path, out, x, y, w, h):
    im = load(path)
    save(Q.CGImageCreateWithImageInRect(im, ((x, y), (w, h))), out)

def tiff(c):
    return c.image().TIFFRepresentation() if c else None

KNOWN = None
def known():
    global KNOWN
    if KNOWN is None:
        KNOWN = []
        for nm in ("arrowCursor", "IBeamCursor", "crosshairCursor", "pointingHandCursor", "openHandCursor",
                   "closedHandCursor", "resizeLeftRightCursor", "resizeUpDownCursor", "operationNotAllowedCursor",
                   "dragCopyCursor", "dragLinkCursor", "contextualMenuCursor"):
            try: KNOWN.append((nm.replace("Cursor", ""), getattr(NSCursor, nm)()))
            except Exception: pass
    return KNOWN

def cursor_desc():
    c = NSCursor.currentSystemCursor()
    if c is None: return "none"
    sz = c.image().size(); hs = c.hotSpot()
    name = "custom"
    d = tiff(c)
    for nm, k in known():
        if tiff(k) == d: name = nm; break
    # blue-square check: Mojave's placeholder for a cursor it does not have
    rep = NSBitmapImageRep.imageRepWithData_(d)
    if rep is not None and rep.pixelsWide() > 2:
        cols = set()
        for yy in range(0, rep.pixelsHigh(), max(1, rep.pixelsHigh() // 6)):
            for xx in range(0, rep.pixelsWide(), max(1, rep.pixelsWide() // 6)):
                col = rep.colorAtX_y_(xx, yy)
                if col is not None:
                    c2 = col.colorUsingColorSpaceName_("NSCalibratedRGBColorSpace")
                    if c2 is not None: cols.add((round(c2.redComponent(), 1), round(c2.greenComponent(), 1), round(c2.blueComponent(), 1), round(c2.alphaComponent(), 1)))
        if cols == set([(0.0, 0.0, 1.0, 1.0)]): name = "BLUE-SQUARE"
    sig = {(17, 23, 4, 4): "arrow", (9, 18, 4, 9): "ibeam", (16, 16, 8, 8): "crosshair?"}
    if name == "custom": name = sig.get((int(sz.width), int(sz.height), int(hs.x), int(hs.y)), "custom")
    return "%s %.0fx%.0f hot %.0f,%.0f" % (name, sz.width, sz.height, hs.x, hs.y)

def front():
    app = NSWorkspace.sharedWorkspace().frontmostApplication()
    x, y = cur_xy()
    wl = Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, 0)
    under = None
    for w in wl:
        b = w.get("kCGWindowBounds")
        if w.get("kCGWindowLayer", 0) >= 1000 or not b or w.get("kCGWindowOwnerName") in ("Dock", "Window Server"): continue
        if b["X"] <= x < b["X"] + b["Width"] and b["Y"] <= y < b["Y"] + b["Height"] and w.get("kCGWindowAlpha", 1) > 0:
            under = "%s wid %d layer %d '%s'" % (w.get("kCGWindowOwnerName"), w.get("kCGWindowNumber"), w.get("kCGWindowLayer", 0), w.get("kCGWindowName") or "")
            break
    return "front %s; under mouse %s" % (app.localizedName() if app else None, under)

curs = []
steps, cur = [], []
args = sys.argv[1:]
if args[:1] == ["-b64"]:
    import base64
    args = base64.b64decode(args[1]).decode("utf-8").split()
for a in args:
    if a == "/":
        if cur: steps.append(cur); cur = []
    else: cur.append(a)
if cur: steps.append(cur)
for st in steps:
    op = st[0]
    if op == "click":
        x, y = float(st[1]), float(st[2]); hold = float(st[3]) if len(st) > 3 else 0.08
        glide(x, y); post(Q.kCGEventLeftMouseDown, x, y); time.sleep(hold); post(Q.kCGEventLeftMouseUp, x, y); time.sleep(0.2)
    elif op == "dclick":
        x, y = float(st[1]), float(st[2]); glide(x, y)
        for n in (1, 2):
            post(Q.kCGEventLeftMouseDown, x, y, n); time.sleep(0.05); post(Q.kCGEventLeftMouseUp, x, y, n); time.sleep(0.08)
    elif op == "drag":
        x1, y1, x2, y2 = [float(v) for v in st[1:5]]
        n = int(st[5]) if len(st) > 5 else 20; dt = float(st[6]) if len(st) > 6 else 0.02
        glide(x1, y1); post(Q.kCGEventLeftMouseDown, x1, y1); time.sleep(0.15)
        for k in range(1, n + 1):
            post(Q.kCGEventLeftMouseDragged, x1 + (x2 - x1) * k / float(n), y1 + (y2 - y1) * k / float(n)); time.sleep(dt)
        time.sleep(0.1); post(Q.kCGEventLeftMouseUp, x2, y2); time.sleep(0.2)
    elif op == "hover":
        glide(float(st[1]), float(st[2]), 0.6)
    elif op == "key":
        m = 0
        if len(st) > 2:
            for nm in st[2].split(","): m |= MODS[nm]
        key(int(st[1]), m); time.sleep(0.15)
    elif op == "type":
        for ch in " ".join(st[1:]).replace("\\n", "\n"):
            sh = ch.isupper() or ch in SHIFTED
            if ch in SHIFTED: ch = SHIFTED[ch]
            key(KEYS[ch.lower()], MODS['shift'] if sh else 0)
        time.sleep(0.2)
    elif op == "scroll":   # scroll N LINES [times]: wheel lines (negative = down), repeated
        n = int(st[1]); times = int(st[2]) if len(st) > 2 else 1
        for k in range(times):
            Q.CGEventPost(Q.kCGHIDEventTap, Q.CGEventCreateScrollWheelEvent(SRC, Q.kCGScrollEventUnitLine, 1, n)); time.sleep(0.05)
        time.sleep(0.3)
    elif op == "resetmods":
        reset_mods()
    elif op == "wait":
        time.sleep(float(st[1]))
    elif op == "cur":
        time.sleep(0.4)
        d = cursor_desc()
        x, y = cur_xy()
        capture(OUT + "/_full.png")
        crop(OUT + "/_full.png", OUT + "/%s.png" % st[1], int(x * 2) - 40, int(y * 2) - 40, 80, 80)
        curs.append(OUT + "/%s.png" % st[1])
        print("cur %s: %s" % (st[1], d))
    elif op == "shot":
        time.sleep(0.3)
        capture(OUT + "/%s.png" % st[1]); subprocess.call(["sips", "-Z", "900", OUT + "/%s.png" % st[1]], stdout=open(os.devnull, "w"))
        print("shot %s" % st[1])
    elif op == "crop":
        time.sleep(0.3)
        capture(OUT + "/_full.png")
        x, y, w, h = [int(float(v) * 2) for v in st[2:6]]
        crop(OUT + "/_full.png", OUT + "/%s.png" % st[1], x, y, w, h)
        print("crop %s" % st[1])
    elif op == "front":
        print(front())
    else:
        print("unknown step %s" % op)
if curs:
    W = 84 * len(curs)
    ctx = Q.CGBitmapContextCreate(None, W, 84, 8, 0, Q.CGColorSpaceCreateDeviceRGB(), Q.kCGImageAlphaPremultipliedLast)
    Q.CGContextSetRGBFillColor(ctx, 0.75, 0.75, 0.75, 1); Q.CGContextFillRect(ctx, ((0, 0), (W, 84)))
    for i, f in enumerate(curs):
        Q.CGContextDrawImage(ctx, ((i * 84 + 2, 2), (80, 80)), load(f))
    save(Q.CGBitmapContextCreateImage(ctx), OUT + "/cursors.png")
    print("cursors -> %s" % OUT + "/cursors.png")
fl = Q.CGEventSourceFlagsState(Q.kCGEventSourceStateHIDSystemState) & 0x00ff0000
if fl:
    reset_mods()
    print("WARNING: modifier flags %x were left held; released" % fl)
