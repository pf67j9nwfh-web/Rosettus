#!/usr/bin/python
# rsrc_icns.py <file> <out.icns>: copy the first 'icns' resource (lowest ID) of a file's resource fork
# (classic CFM apps keep their icon there), or build one from 'ic08'/'it32'-style parts if present.
import sys, struct
src, out = sys.argv[1], sys.argv[2]
try:
    d = open(src + '/..namedfork/rsrc', 'rb').read()
except IOError:
    sys.exit(1)
if len(d) < 16: sys.exit(1)
doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
m = d[moff:moff + mlen]
tl_off, nl_off = struct.unpack('>HH', m[24:28])
tl = m[tl_off:]
n = struct.unpack('>H', tl[:2])[0] + 1
best = None
for i in range(n):
    typ, cnt, ref = struct.unpack('>4sHH', tl[2 + 8 * i:10 + 8 * i])
    if typ != b'icns': continue
    for j in range(cnt + 1):
        rid, nmo, attr_off = struct.unpack('>hHI', tl[ref + 12 * j:ref + 12 * j + 8])
        off = attr_off & 0xffffff
        ln = struct.unpack('>I', d[doff + off:doff + off + 4])[0]
        if best is None or rid < best[0]: best = (rid, d[doff + off + 4:doff + off + 4 + ln])
if not best: sys.exit(1)
open(out, 'wb').write(best[1])
