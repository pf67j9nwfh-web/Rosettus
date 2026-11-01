#!/usr/bin/python
# Mark PowerPC functions for the liboah_mojave bridge.
# Usage: bridgepatch.py LIBRARY SYMBOL=ID [SYMBOL=ID ...]
# Writes "twi 31,r0,ID ; blr" over the first two instructions of each symbol.
# The shim's trap handler computes the result natively and sets r3; the blr
# then returns to the caller, which works for interpreted and compiled code.
# A backup LIBRARY.prebridge is made the first time; patching always starts
# from that backup, so rerunning with a new list is safe.
import struct, sys, os, shutil

def ppc_slice(data):
    magic = struct.unpack(">I", data[0:4])[0]
    if magic == 0xfeedface:
        return 0, len(data)
    if magic == 0xcafebabe:
        n = struct.unpack(">I", data[4:8])[0]
        for i in range(n):
            cpu, sub, off, size, al = struct.unpack(">IIIII", data[8 + i * 20:28 + i * 20])
            if cpu == 18:
                return off, size
    raise SystemExit("no 32-bit PowerPC slice")

def parse(data, base):
    ncmds = struct.unpack(">I", data[base + 16:base + 20])[0]
    p = base + 28
    segs = []
    symtab = None
    for i in range(ncmds):
        cmd, size = struct.unpack(">II", data[p:p + 8])
        if cmd == 1:  # LC_SEGMENT
            vmaddr, vmsize, fileoff, filesize = struct.unpack(">IIII", data[p + 24:p + 40])
            segs.append((vmaddr, vmsize, fileoff, filesize))
        elif cmd == 2:  # LC_SYMTAB
            symtab = struct.unpack(">IIII", data[p + 8:p + 24])
        p += size
    return segs, symtab

def find_symbols(data, base, segs, symtab, wanted):
    symoff, nsyms, stroff, strsize = symtab
    found = {}
    for i in range(nsyms):
        e = base + symoff + i * 12
        strx, ntype, nsect, ndesc, value = struct.unpack(">IBBhI", data[e:e + 12])
        if ntype & 0xe0 or (ntype & 0x0e) != 0x0e:  # skip stabs, keep N_SECT
            continue
        s = base + stroff + strx
        name = data[s:data.index("\0", s)]
        if name in wanted and name not in found:
            found[name] = value
    return found

def all_starts(data, base, symtab):
    """sorted addresses of all section symbols (to find one-instruction functions)"""
    symoff, nsyms, stroff, strsize = symtab
    st = set()
    for i in range(nsyms):
        e = base + symoff + i * 12
        strx, ntype, nsect, ndesc, value = struct.unpack(">IBBhI", data[e:e + 12])
        if ntype & 0xe0 or (ntype & 0x0e) != 0x0e:
            continue
        st.add(value)
    return st
def vm_to_file(segs, addr):
    for vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= addr < vmaddr + filesize:
            return fileoff + (addr - vmaddr)
    return None

def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__ or "usage: bridgepatch.py LIB SYM=ID ...")
    lib = sys.argv[1]
    backup = lib + ".prebridge"
    if not os.path.exists(backup):
        shutil.copy2(lib, backup)
    data = open(backup, "rb").read()
    base, size = ppc_slice(data)
    segs, symtab = parse(data, base)
    wanted = {}
    for a in sys.argv[2:]:
        name, ident = a.split("=")
        if not name.startswith("_"):
            name = "_" + name
        wanted[name] = int(ident, 0)
    found = find_symbols(data, base, segs, symtab, wanted)
    starts = all_starts(data, base, symtab)
    out = list(data)
    bad = 0
    for name in sorted(wanted.keys()):
        if name not in found:
            print "MISSING %s" % name
            bad = 1
            continue
        off = vm_to_file(segs, found[name])
        if off is None:
            print "NO FILE OFFSET %s" % name
            bad = 1
            continue
        off += base
        old = struct.unpack(">II", data[off:off + 8])
        # A one-instruction function (another symbol 4 bytes on) cannot take two words: the "blr"
        # would land on the next function's first instruction and replace its own mark
        # (CGSAddButtonRegionInWindow is "b CGSAddButtonRegion" right before CGSMoveWindowWithGroup:
        # every call ran the Move bridge with junk and moved the window to 0,0). Leave it alone:
        # a branch reaches the function it branches to, which carries its own mark if bridged.
        if (found[name] + 4) in starts:
            print "SKIP %-28s one instruction (%08x), next symbol at +4" % (name, old[0])
            continue
        new = struct.pack(">II", 0x0FE00000 | wanted[name], 0x4E800020)
        out[off:off + 8] = list(new)
        print "%-28s id=%-3d vm=0x%08x file=0x%08x old=%08x %08x" % (
            name, wanted[name], found[name], off, old[0], old[1])
    if bad:
        raise SystemExit(1)
    f = open(lib, "wb")
    f.write("".join(out))
    f.close()

main()
