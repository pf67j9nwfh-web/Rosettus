#!/usr/bin/python
# autopatch.py [--once] [FOLDER...]: patch PowerPC-only apps that appear in the Applications folders,
# so they open with a double-click like on Snow Leopard. Run by the com.rosettus.autopatch agent
# (WatchPaths on the folders) when "Patch new PowerPC apps automatically" is on in Rosettus Launcher
# (or: install.sh --autopatch). Apps already patched, Universal / Intel apps and apps this account
# cannot change are left alone. Log: ~/Library/Logs/Rosettus/autopatch.log
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.dirname(HERE)
PATCH = os.path.join(ROOT, 'rosettus_patch.py')
HOME = os.path.expanduser('~')
LOG = os.path.join(HOME, 'Library/Logs/Rosettus/autopatch.log')
STATE = os.path.join(HOME, 'Library/Caches/com.rosettus.autopatch.seen')
SKIP = ('/Applications/Rosettus', os.path.join(HOME, 'Applications/Rosettus'))

def log(msg):
    try:
        if not os.path.isdir(os.path.dirname(LOG)): os.makedirs(os.path.dirname(LOG))
        open(LOG, 'a').write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
    except Exception: pass

def apps(folders):
    for f in folders:
        if not os.path.isdir(f): continue
        for n in sorted(os.listdir(f)):
            p = os.path.join(f, n)
            if p.startswith(SKIP) or os.path.islink(p): continue
            if n.endswith('.app'): yield p
            elif os.path.isdir(p) and not n.startswith('.'):
                try:
                    for m in sorted(os.listdir(p)):
                        if m.endswith('.app'): yield os.path.join(p, m)
                except OSError: pass

def newest_mtime(app):
    t = 0
    for d, dirs, files in os.walk(os.path.join(app, 'Contents')):
        for x in files[:200]:
            try: t = max(t, os.lstat(os.path.join(d, x)).st_mtime)
            except OSError: pass
        if d.count(os.sep) - app.count(os.sep) > 3: del dirs[:]
    return t

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    folders = args or ['/Applications', os.path.join(HOME, 'Applications')]
    try: seen = set(open(STATE).read().splitlines())
    except IOError: seen = set()
    changed = False
    for app in apps(folders):
        if app in seen: continue
        t = newest_mtime(app)
        if t and time.time() - t < 10:     # still being copied: the next change event comes back to it
            continue
        if not os.access(os.path.join(app, 'Contents'), os.W_OK):
            seen.add(app); changed = True; continue
        r = subprocess.Popen(['/usr/bin/python', PATCH, app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.communicate()[0].strip()
        if r.returncode == 0 and 'already patched' not in out: log(out)
        elif r.returncode == 1: log('%s: %s' % (app, out))
        seen.add(app); changed = True
    if changed:
        try:
            if not os.path.isdir(os.path.dirname(STATE)): os.makedirs(os.path.dirname(STATE))
            open(STATE, 'w').write('\n'.join(sorted(seen)) + '\n')
        except IOError: pass

if __name__ == '__main__':
    main()
