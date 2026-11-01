import struct, sys
# pefbase.py PEF LR1 LR2 ...: find the load address of the PEF code section from return addresses
# (each LR must point at "lwz r2,20(r1)" = 0x80410014 after a cross-TOC call)
d = open(sys.argv[1], 'rb').read()
lrs = [int(a, 16) for a in sys.argv[2:]]
assert d[:8] == b'Joy!peff'
nsec = struct.unpack('>H', d[32:34])[0]
for i in range(nsec):
    h = d[40 + 28 * i: 40 + 28 * (i + 1)]
    name, addr, execsz, initsz, rawsz, off, kind, align, share = struct.unpack('>iIIIIIBBB', h[:27])
    print 'section', i, 'kind', kind, 'off', hex(off), 'raw', hex(rawsz), 'exec', hex(execsz)
    if kind != 0: continue
    code = d[off:off + rawsz]
    lo = min(lrs)
    for o in range(0, len(code) - 4, 4):
        base = lo - o
        ok = True
        for L in lrs:
            p = L - base
            if p < 0 or p + 4 > len(code) or code[p:p + 4] != b'\x80\x41\x00\x14':
                ok = False; break
        if ok:
            print 'candidate base %08x (section %d, file off %x)' % (base, i, off)
