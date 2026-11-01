#!/bin/bash
# telemetry.sh [-a app] [-n N] [--compact]: merge ~/Library/Logs/Rosettus/Telemetry/*.tsv (one per process)
# and rank the gaps by how many apps hit them, then by call count.
# --compact folds the per-pid files into one <app>.merged.tsv per app.
D=~/Library/Logs/Rosettus/Telemetry
exec /usr/bin/python - "$D" "$@" <<'EOF'
import sys, os, glob, collections
d = sys.argv[1]; args = sys.argv[2:]
app_f = None; top = 60; compact = False
i = 0
while i < len(args):
    if args[i] == '-a': app_f = args[i+1]; i += 1
    elif args[i] == '-n': top = int(args[i+1]); i += 1
    elif args[i] == '--compact': compact = True
    i += 1
calls = collections.Counter(); apps = collections.defaultdict(set); per_app = collections.defaultdict(collections.Counter)
files = glob.glob(os.path.join(d, '*.tsv'))
for f in files:
    base = os.path.basename(f)
    app = base.rsplit('.', 2)[0]
    if app_f and app_f.lower() not in app.lower(): continue
    for ln in open(f):
        p = ln.rstrip('\n').split('\t')
        if len(p) < 3: continue
        k = (p[0], p[1])
        try: n = int(p[2])
        except ValueError: continue
        calls[k] += n; apps[k].add(app); per_app[app][k] += n
if compact:
    for app, c in per_app.items():
        tmp = os.path.join(d, app + '.merged.tsv.tmp')
        with open(tmp, 'w') as o:
            for (cat, name), n in sorted(c.items()): o.write('%s\t%s\t%d\t0\n' % (cat, name, n))
        for f in glob.glob(os.path.join(d, app + '.*.tsv')): os.unlink(f)
        os.rename(tmp, os.path.join(d, app + '.merged.tsv'))
    print('compacted %d files into %d' % (len(files), len(per_app)))
print('%d processes, %d apps, %d distinct gaps' % (len(files), len(per_app), len(calls)))
print('%-5s %-10s %-16s %-44s %s' % ('apps', 'calls', 'category', 'name', 'seen in'))
for k in sorted(calls, key=lambda k: (-len(apps[k]), -calls[k]))[:top]:
    a = sorted(apps[k])
    print('%-5d %-10d %-16s %-44s %s' % (len(a), calls[k], k[0], k[1][:44], ', '.join(a)[:80]))
EOF
