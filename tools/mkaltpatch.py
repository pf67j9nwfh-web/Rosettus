#!/usr/bin/python
"""mkaltpatch.py EXPORT_ROOT --ref REFDIR [--write] - alternate patches for another 10.6.8 build of a patched file.

For every import/patches/X.rpatch whose base does not match EXPORT_ROOT/X (the export made from an
install DVD, import/dvd), the same records are applied to that build's file and X.altN.rpatch is
made from the result, with its sha1 added to import/expected.alt.sha1. Only for files whose build
differences stay clear of the patched bytes (checked here: no differing byte within 16 bytes of a
record, same size, against REFDIR's copy of the patch's base); others are reported and must be looked at by hand (import/optional_patches.txt
for files no PowerPC app needs).
"""
import hashlib, os, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'import'))
import rpatch

root = sys.argv[1]
write = '--write' in sys.argv
refdir = sys.argv[sys.argv.index('--ref') + 1] if '--ref' in sys.argv else None
PD = os.path.join(REPO, 'import', 'patches')
exp = {}
for l in open(os.path.join(REPO, 'import', 'expected.sha1')):
    h, f = l.rstrip('\n').split('  ', 1)
    exp[f] = h
altexp_p = os.path.join(REPO, 'import', 'expected.alt.sha1')
altexp = [l.rstrip('\n') for l in open(altexp_p)] if os.path.exists(altexp_p) else []
optional = set(l.strip() for l in open(os.path.join(REPO, 'import', 'optional_patches.txt'))) \
    if os.path.exists(os.path.join(REPO, 'import', 'optional_patches.txt')) else set()
for d, dirs, files in os.walk(PD):
    for fn in sorted(files):
        if not fn.endswith('.rpatch') or fn == 'translate.rpatch' or '.alt' in fn:
            continue
        pf = os.path.join(d, fn)
        rel = os.path.relpath(pf, PD)[:-len('.rpatch')]
        src = rel[:-len('.prebridge')] if rel.endswith('.prebridge') else rel
        bp = os.path.join(root, src)
        if not os.path.exists(bp):
            print 'missing in export: %s' % src
            continue
        base = open(bp, 'rb').read()
        bsha, tsha, tsize, recs = rpatch.parse(open(pf, 'rb').read())
        if hashlib.sha1(base).hexdigest() == bsha:
            continue
        stem = pf[:-len('.rpatch')]
        alts = [x for x in os.listdir(d) if x.startswith(os.path.basename(stem) + '.alt') and x.endswith('.rpatch')]
        if any(rpatch.parse(open(os.path.join(d, a), 'rb').read())[0] == hashlib.sha1(base).hexdigest() for a in alts):
            print 'have alternate: %s' % rel
            continue
        if rel in optional:
            print 'optional, skipped: %s' % rel
            continue
        if len(base) != tsize:
            print 'DIFFERENT SIZE (%d vs %d), by hand: %s' % (len(base), tsize, rel)
            continue
        # the bytes around every record must be the same in this build as in the build the patch was
        # made from (REFDIR: that build's pristine files, e.g. thinned on the Snow Leopard Mac the
        # release was made with); then the same offsets hold the same code and the records fit
        cand = bytearray(base)
        for off, b in recs:
            cand[off:off + len(b)] = b
        refp = os.path.join(refdir, src) if refdir else None
        if not refp or not os.path.exists(refp):
            print 'no reference base for %s (give --ref DIR)' % rel
            continue
        r = open(refp, 'rb').read()
        if hashlib.sha1(r).hexdigest() != bsha:
            print 'reference base for %s is not the patch base' % rel
            continue
        near = 0
        for off, b in recs:
            lo, hi = max(0, off - 16), off + len(b) + 16
            near += sum(1 for i in range(lo, min(hi, len(r), len(base))) if r[i] != base[i])
        if near:
            print 'BUILD DIFFERS NEAR THE PATCH (%d bytes), by hand: %s' % (near, rel)
            continue
        data, n, nb = rpatch.make(base, bytes(cand))
        out = '%s.alt%d.rpatch' % (stem, len(alts) + 1)
        h = hashlib.sha1(bytes(cand)).hexdigest()
        print '%s %s (%d records)' % ('wrote' if write else 'would write', os.path.relpath(out, REPO), n)
        if write:
            open(out, 'wb').write(data)
            if rel in exp:
                altexp.append('%s  %s' % (h, rel))
if write:
    open(altexp_p, 'w').write(''.join(l + '\n' for l in sorted(set(altexp), key=lambda x: x[42:])))
