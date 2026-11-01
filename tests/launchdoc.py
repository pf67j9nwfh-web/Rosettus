# launchdoc.py WRAPPER_NAME PGREP_NAME FILE [WAIT]: the app not running; open -a its launcher with FILE.
# Reports whether a window whose title contains the file's base name appears.
import subprocess, sys, time, os
import Quartz as Q
W = '/Applications/Rosettus/%s.app' % sys.argv[1]
if not os.path.exists(W): W = os.path.expanduser('~/Applications/Rosettus/%s.app' % sys.argv[1])
name = os.path.splitext(os.path.basename(sys.argv[3]))[0]
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 60
def gpid():
    o = subprocess.Popen(['pgrep', '-f', 'translate.*' + sys.argv[2]], stdout=subprocess.PIPE).communicate()[0].split()
    return int(o[0]) if o else 0
def wins(p):
    return [(w.get('kCGWindowName', '') or '', w.get('kCGWindowLayer'), int(w['kCGWindowBounds']['Width'])) for w in
            Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) if int(w.get('kCGWindowOwnerPID', 0)) == p and w.get('kCGWindowLayer') in (0, 8)]
if gpid(): sys.exit('%s is running' % sys.argv[2])
subprocess.call(['open', '-a', W, sys.argv[3]]); t = time.time(); got = []
while time.time() - t < wait:
    p = gpid()
    got = [w for w in wins(p) if name in w[0]] if p else []
    if got: break
    time.sleep(1)
p = gpid()
print 'RESULT launchdoc %s: %s after %.0f s; windows %s' % (sys.argv[1], 'OPENED' if got else 'NOT OPENED', time.time() - t, wins(p)[:4])
if p: os.system('kill %d; sleep 3; kill -9 %d 2>/dev/null' % (p, p))
