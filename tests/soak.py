#!/usr/bin/python
"""soak.py WRAPPER_NAME MINUTES [--doc-close cmdd|cmddel]: a long session with one PowerPC app.

Starts /Applications/Rosettus/<WRAPPER_NAME>.app and repeats, while it stays in front:
New (Cmd-N), type a line, select all, copy, paste, close (Cmd-W) and Don't Save.
Every cycle: the translate process's RSS, CPU time, window count and the heartbeat's age; a cycle
whose New does not add a window within 20 s counts as a stall. Output: one line per minute and a
summary (RSS growth per 100 cycles). Stops early if the app dies or something else takes the front.
"""
import os, re, subprocess, sys, time
import Quartz as Q
from AppKit import NSPasteboard, NSStringPboardType

APP = sys.argv[1]
MIN = float(sys.argv[2])
CLOSE = 'cmddel' if '--doc-close' in sys.argv and sys.argv[sys.argv.index('--doc-close') + 1] == 'cmddel' else 'cmdd'
W = '/Applications/Rosettus/%s.app' % APP
SRC = Q.CGEventSourceCreate(Q.kCGEventSourceStateHIDSystemState)


def front_pid():
    for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) or []:
        if w.get('kCGWindowLayer', 0) == 0 and w.get('kCGWindowOwnerName') not in ('Window Server', 'Dock'):
            return int(w['kCGWindowOwnerPID'])
    return 0


def windows(pid):
    return [w for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) or []
            if int(w.get('kCGWindowOwnerPID', 0)) == pid and w.get('kCGWindowLayer', 0) in (0, 3, 8)]


def titles(pid):
    return tuple(sorted(w.get('kCGWindowName', '') or '' for w in windows(pid) if w.get('kCGWindowLayer', 0) == 0))


class Lost(Exception): pass


def key(code, cmd=False, pid=0):
    if pid and front_pid() != pid:
        raise Lost('front pid %d' % front_pid())
    for down in (True, False):
        e = Q.CGEventCreateKeyboardEvent(SRC, code, down)
        Q.CGEventSetFlags(e, Q.kCGEventFlagMaskCommand if cmd else 0)
        Q.CGEventPost(Q.kCGHIDEventTap, e)
        time.sleep(0.03)


KC = dict(a=0, s=1, d=2, f=3, h=4, g=5, z=6, x=7, c=8, v=9, b=11, q=12, w=13, e=14, r=15, y=16, t=17,
          o=31, u=32, i=34, p=35, l=37, j=38, k=40, n=45, m=46)
KC.update({'1': 18, '2': 19, '3': 20, '4': 21, '6': 22, '5': 23, '9': 25, '7': 26, '8': 28, '0': 29, ' ': 49})


def type_text(s, pid):   # US key codes (as behave.py)
    for ch in s.lower():
        if ch in KC: key(KC[ch], False, pid)


def stats(pid):
    o = subprocess.Popen(['ps', '-o', 'rss=,time=', '-p', str(pid)], stdout=subprocess.PIPE).communicate()[0].split()
    if len(o) < 2: return None
    t = o[1].split(':'); secs = float(t[-1]) + 60 * float(t[-2]) + (3600 * float(t[-3]) if len(t) > 2 else 0)
    return int(o[0]), secs


def hb_age(pid):
    try:
        m = re.search(r'age=(\d+)s', open('/tmp/oah_hb.%d.txt' % pid).read())
        return int(m.group(1)) if m else -1
    except IOError:
        return -1


subprocess.call(['open', W])
t0 = time.time(); pid = 0
while time.time() - t0 < 60:
    o = subprocess.Popen(['pgrep', '-f', 'translate.*' + APP], stdout=subprocess.PIPE).communicate()[0].split()
    pid = int(o[0]) if o else 0
    if pid and windows(pid) and front_pid() == pid: break
    time.sleep(1)
if not pid:
    sys.exit('%s did not start' % APP)
time.sleep(5)
if '--start-esc' in sys.argv:   # a start-up dialog first (Word 2004: its Project Gallery)
    key(53, False, pid); time.sleep(3)
pb = NSPasteboard.generalPasteboard()
start = stats(pid); base = None; lat = []; cycles = 0; stalls = 0; copies = 0; last_min = time.time(); why = 'time'
print '%s pid %d: start rss %d KB' % (APP, pid, start[0])
try:
    while time.time() - t0 < MIN * 60:
        n0 = len(windows(pid)); t0n = titles(pid)
        key(45, True, pid)                     # Cmd-N (a new window, or a new document in the same window: TextWrangler)
        tn = time.time()
        while time.time() - tn < 20 and len(windows(pid)) <= n0 and titles(pid) == t0n: time.sleep(0.2)
        if len(windows(pid)) <= n0 and titles(pid) == t0n:
            stalls += 1; print '  stall: New gave no window (cycle %d)' % cycles
            continue
        time.sleep(0.5)
        txt = 'soak %d %d' % (cycles, int(time.time()) % 100000)
        type_text(txt, pid); time.sleep(0.3)
        if '--no-copy' in sys.argv: pass
        else:
          key(0, True, pid); key(8, True, pid); tc = time.time()   # select all, copy
          while time.time() - tc < float(os.environ.get("SOAK_COPYWAIT", "5")) and txt not in (pb.stringForType_(NSStringPboardType) or '').lower(): time.sleep(0.05)
          if txt in (pb.stringForType_(NSStringPboardType) or '').lower(): copies += 1; lat.append(time.time() - tc)
        if '--no-paste' not in sys.argv: key(9, True, pid); time.sleep(0.5)   # paste
        key(13, True, pid); time.sleep(1.0)                   # close
        if len(windows(pid)) > n0 or len([w for w in windows(pid) if w.get('kCGWindowLayer', 0) == 8]):
            key(2, True, pid) if CLOSE == 'cmdd' else key(51, True, pid)   # Don't Save
            time.sleep(1.0)
        cycles += 1
        if time.time() - last_min >= 60:
            last_min = time.time(); s = stats(pid)
            if base is None: base = (s[0], cycles)
            print '  %4.0f s: %d cycles, rss %d KB, cpu %.0f s, windows %d, hb age %d s' % (time.time() - t0, cycles, s[0], s[1], len(windows(pid)), hb_age(pid))
            sys.stdout.flush()
except Lost as e:
    why = 'lost the front (%s)' % e
end = stats(pid)
if end is None:
    print 'RESULT soak %s: the app DIED after %d cycles' % (APP, cycles)
else:
    b = base or (start[0], 0)
    growth = (end[0] - b[0]) * 100.0 / max(cycles - b[1], 1)
    lat.sort()
    print '  copy -> Mojave pasteboard: median %.2f s, 90%% %.2f s, max %.2f s' % (lat[len(lat) // 2], lat[len(lat) * 9 // 10], lat[-1]) if lat else '  no copy seen'
    print 'RESULT soak %s: %d cycles in %.0f s (%s), %d stalls, %d/%d copies seen, rss %d -> %d KB (%+.0f KB / 100 cycles after the first minute), cpu %.0f s' % (
        APP, cycles, time.time() - t0, why, stalls, copies, cycles, start[0], end[0], growth, end[1])
    if '--keep' not in sys.argv:
        os.system('kill %d; sleep 3; kill -9 %d 2>/dev/null' % (pid, pid))
