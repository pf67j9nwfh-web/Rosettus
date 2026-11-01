import struct, sys
# pefsyms.py PEF FILEBASE CODEOFF CODELEN : function names and starts from XCOFF traceback tables
f, B = sys.argv[1], int(sys.argv[2], 16)
off, ln = int(sys.argv[3], 16), int(sys.argv[4], 16)
d = open(f, 'rb').read()
o = off
out = []
while o < off + ln - 16:
    if d[o:o + 4] == '\0\0\0\0' and d[o + 4] == '\0':
        f1 = ord(d[o + 6]); fp, flp = ord(d[o + 10]), ord(d[o + 11])
        if f1 & 0x20:                     # has_tboff
            p = o + 12
            if fp or (flp >> 1): p += 4
            tbo = struct.unpack('>I', d[p:p + 4])[0]; p += 4
            if 0 < tbo < 0x40000 and tbo % 4 == 0:
                q = p
                name = None
                for k in range(q, q + 24, 2):
                    n = struct.unpack('>H', d[k:k + 2])[0]
                    if 2 < n < 200 and all(32 <= ord(c) < 127 for c in d[k + 2:k + 2 + n]):
                        name = d[k + 2:k + 2 + n]; break
                if name:
                    out.append((B + o - tbo, B + o, name))
                    o += 8
                    continue
    o += 4
for s, e, n in out:
    print '%08x %08x %s' % (s, e, n)
