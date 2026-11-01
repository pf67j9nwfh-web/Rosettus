#!/usr/bin/python
# javafix.py VERSIONS_DIR SHIM_INSTALL_NAME: let PowerPC Java libraries find functions that Snow Leopard's
# PowerPC frameworks no longer have (Java 5's libawt calls CTFontGetGraphicsFont, which 10.6 renamed).
# For each PowerPC library that imports one of them, a load command for Rosettus' libjavashim is added
# (in the free space after the existing load commands) and those imports are bound to it (the library
# ordinal in the symbol's n_desc). Everything else stays as it was. Idempotent; prints what it patched.
import os, struct, sys
NEED = ['_CTFontGetGraphicsFont', '_CTFontGetAdvancesForGlyphs', '_CTLineGetTypographicBounds',   # Java 1.4.2, 5.0
        '_CGFontCopy', '_CGFontGetMonikers', '_CGFontGetUnicodeToGlyphMapSize', '_CGFontGetUseIntegerAdvances',   # 1.3.1
        '_CGFontSetUseIntegerAdvances', '_CGFontSetDisplayBitmaps', '_CGFontSetShouldAntialias', '_EnableForegroundWithDockInfo',
        '_CGFontGetPlatformFont']
DYLIB_CMDS = (0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023)   # LOAD, LOAD_WEAK, REEXPORT, LAZY_LOAD, LOAD_UPWARD
LC_SYMTAB, LC_SEGMENT, LC_DYLD_INFO, LC_DYLD_INFO_ONLY = 0x2, 0x1, 0x22, 0x80000022

def ppc_slice(d):
    if struct.unpack('>I', d[:4])[0] == 0xcafebabe:
        for i in range(struct.unpack('>I', d[4:8])[0]):
            ct, cs, off, size, al = struct.unpack('>5I', d[8 + 20 * i:28 + 20 * i])
            if ct == 18: return off
        return None
    return 0 if struct.unpack('>I', d[:4])[0] == 0xfeedface else None

def patch(path, shim):
    d = bytearray(open(path, 'rb').read())
    if not any(s.encode() in d for s in NEED): return None
    b = ppc_slice(bytes(d))
    if b is None: return None
    ncmds, sizeofcmds = struct.unpack('>II', bytes(d[b + 16:b + 24]))
    off = b + 28; ords = 0; symtab = None; first_sect = None; have = 0
    for i in range(ncmds):
        cmd, size = struct.unpack('>II', bytes(d[off:off + 8]))
        if cmd in DYLIB_CMDS:
            ords += 1
            no = struct.unpack('>I', bytes(d[off + 8:off + 12]))[0]
            if bytes(d[off + no:off + size]).split(b'\0')[0] == shim.encode(): have = ords
        elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
            return 'skipped (compressed dyld info)'
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack('>4I', bytes(d[off + 8:off + 24]))
        elif cmd == LC_SEGMENT:
            nsects = struct.unpack('>I', bytes(d[off + 48:off + 52]))[0]
            for k in range(nsects):
                so = struct.unpack('>I', bytes(d[off + 56 + 68 * k + 40:off + 56 + 68 * k + 44]))[0]
                if so and (first_sect is None or so < first_sect): first_sect = so
        off += size
    if symtab is None: return 'skipped (no symbol table)'
    if not have:
        name = shim.encode() + b'\0'
        csize = (24 + len(name) + 3) & ~3
        end = b + 28 + sizeofcmds
        if first_sect is None or end + csize > b + first_sect or any(d[end:end + csize]):
            return 'skipped (no room for a load command)'
        d[end:end + csize] = struct.pack('>6I', 0xc, csize, 24, 2, 0x10000, 0x10000) + name + b'\0' * (csize - 24 - len(name))
        d[b + 16:b + 24] = struct.pack('>II', ncmds + 1, sizeofcmds + csize)
        have = ords + 1
    symoff, nsyms, stroff, strsize = symtab
    n = 0
    for i in range(nsyms):
        e = b + symoff + 12 * i
        strx, typ, sect, desc, val = struct.unpack('>IBBhI', bytes(d[e:e + 12]))
        s = bytes(d[b + stroff + strx:b + stroff + strx + 64]).split(b'\0')[0]
        if (typ & 0x0e) in (0, 0x0c) and s.decode() in NEED:   # N_UNDF, or N_PBUD (prebound, Tiger)
            newdesc = (desc & 0x00ff) | (have << 8)
            if newdesc != (desc & 0xffff):
                d[e + 6:e + 8] = struct.pack('>H', newdesc & 0xffff); n += 1
    if not n and have == ords: return None
    tmp = path + '.rosettus-tmp'
    open(tmp, 'wb').write(d); os.chmod(tmp, os.stat(path).st_mode); os.rename(tmp, path)
    return 'patched (%d import(s) -> %s)' % (n, os.path.basename(shim))

root, shim = sys.argv[1], sys.argv[2]
for dp, dn, fn in os.walk(root):
    for f in fn:
        if (f.endswith('.jnilib') or f.endswith('.dylib')) and f != os.path.basename(shim):
            p = os.path.join(dp, f)
            if os.path.islink(p): continue
            try:
                r = patch(p, shim)
            except Exception as x:
                r = 'skipped (%s)' % x
            if r: print('%s: %s' % (p[len(root):].lstrip('/'), r))
