#!/usr/bin/python
# manifest.py ROOT [LIST] > out.tsv
# Without LIST: every regular file / symlink under ROOT (System, usr, Library) -> "relpath\tsize\tsha1"
# (symlinks: "relpath\tlink\ttarget").  With LIST (a manifest from another machine): the same paths
# under ROOT only (missing ones reported as "relpath\tmissing\t-").
import os, sys, hashlib
root = sys.argv[1].rstrip('/')
def ent(rel):
    p = root + '/' + rel
    if os.path.islink(p): return '%s\tlink\t%s' % (rel, os.readlink(p))
    if not os.path.isfile(p): return '%s\tmissing\t-' % rel
    h = hashlib.sha1()
    try:
        f = open(p, 'rb')
        while True:
            b = f.read(1 << 20)
            if not b: break
            h.update(b)
        f.close()
    except IOError:
        return '%s\tunreadable\t-' % rel
    return '%s\t%d\t%s' % (rel, os.path.getsize(p), h.hexdigest())
if len(sys.argv) > 2:
    for l in open(sys.argv[2]):
        rel = l.split('\t', 1)[0].strip()
        if rel: print ent(rel)
else:
    for top in ('System', 'usr', 'Library'):
        for d, dirs, files in os.walk(root + '/' + top):
            for n in dirs + files:
                p = os.path.join(d, n)
                if n in dirs and not os.path.islink(p): continue
                print ent(p[len(root) + 1:])
