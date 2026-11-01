#!/usr/bin/python
"""vmdiff.py A.json B.json: where the dirtied memory grew between two vmreg.py snapshots
(by 1 MB band and by tag), and the biggest new or grown regions."""
import json, sys
A = json.load(open(sys.argv[1])); B = json.load(open(sys.argv[2]))
def bands(R, shift):
    d = {}
    for a, s, t, p, r, dd, sm, sw in R:
        k = (a >> shift, t)
        d[k] = d.get(k, 0) + dd
    return d
da, db = bands(A, 20), bands(B, 20)
keys = set(da) | set(db)
ch = sorted(((db.get(k, 0) - da.get(k, 0)) * 4, k) for k in keys)
print 'total dirtied: %d -> %d KB' % (sum(da.values()) * 4, sum(db.values()) * 4)
print 'grew most (KB, 1 MB band, tag):'
for kb, k in ch[::-1][:25]:
    if kb <= 0: break
    print '  %+7d  %08x tag %d' % (kb, k[0] << 20, k[1])
print 'shrank most:'
for kb, k in ch[:8]:
    if kb >= 0: break
    print '  %+7d  %08x tag %d' % (kb, k[0] << 20, k[1])
