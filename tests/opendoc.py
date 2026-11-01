# opendoc.py WRAPPER_NAME PGREP_NAME FILE [WAIT]: start the app, then open FILE in the running app (odoc).
import subprocess, sys, time, os
import Quartz as Q
W = '/Applications/Rosettus/%s.app' % sys.argv[1]
if not os.path.exists(W): W = os.path.expanduser('~/Applications/Rosettus/%s.app' % sys.argv[1])
name = os.path.splitext(os.path.basename(sys.argv[3]))[0]
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 40
def gpid():
    o = subprocess.Popen(['pgrep', '-f', 'translate.*' + sys.argv[2]], stdout=subprocess.PIPE).communicate()[0].split()
    return int(o[0]) if o else 0
def wins(p):
    return [(w.get('kCGWindowName', '') or '', w.get('kCGWindowLayer'), int(w['kCGWindowBounds']['Width'])) for w in
            Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) if int(w.get('kCGWindowOwnerPID', 0)) == p and w.get('kCGWindowLayer') in (0, 8)]
subprocess.call(['open', W]); t = time.time()
while time.time() - t < 60 and not (gpid() and wins(gpid())): time.sleep(1)
time.sleep(5); p = gpid()
subprocess.call(['open', '-a', W, sys.argv[3]]); t = time.time(); got = []
while time.time() - t < wait:
    got = [w for w in wins(p) if name in w[0]]
    if got or not gpid(): break
    time.sleep(1)
alive = gpid() == p
print 'RESULT opendoc %s: %s after %.0f s (%s); windows %s' % (sys.argv[1], 'OPENED' if got else 'NOT OPENED', time.time() - t, 'running' if alive else 'GONE', wins(p)[:4])
if alive and p: os.system('kill %d; sleep 3; kill -9 %d 2>/dev/null' % (p, p))
