#!/usr/bin/python
"""export_from_sl.py - collect the Snow Leopard files Rosettus needs.

usage: export_from_sl.py MANIFEST.tsv.gz DUMP_TEXT_PPC DEST                      (ON Snow Leopard)
       export_from_sl.py MANIFEST.tsv.gz - DEST --root DIR --decoded FILE        (from an extracted
                                         Snow Leopard tree, e.g. the install DVD: import/dvd)

Reads manifest.tsv.gz (relpath, method, size, sha1) and builds DEST/root/<relpath>:
    copy        the file as is
    deref       the file a symlink points to, as a plain file
    thin:ARCH   only the ARCH (PowerPC) slice of a universal binary (lipo -thin)
    fat         the whole universal binary
    link:T      a symlink to T
    optional    copied when present (machine-wide /Library content), never verified
Every copied file is checked against the manifest sha1; mismatches are listed in DEST/mismatch.txt
(a different 10.6 build or a modified system) and the export continues.

Then translate: dump_text_ppc (a PowerPC program, so it runs inside your Rosetta) saves the
kernel-decoded __TEXT, which is spliced into a copy of /usr/libexec/oah/translate (the
SG_PROTECTED_VERSION_1 flag cleared) as DEST/root/translate.decoded. With --decoded that file is
already made (import/dvd/dvdtool.py decode) and is copied.
"""
import gzip, hashlib, os, shutil, subprocess, sys, struct, unicodedata

TRANSLATE = '/usr/libexec/oah/translate'
TRANSLATE_SHA1 = '860fa160189146f5a19de904b84e5204bd0bc85b'          # 10.6.8 (Rosetta 1.0.x)
DECODED_SHA1 = 'f267d31af007bcce60b06de566b3087352edbd17'
TEXT_SIZE = 0x1df000
ROOT = ''          # '' = this system; else an extracted Snow Leopard tree


def copy_mac_meta(src, out):
    """Carry the Finder info (type/creator) and the resource fork over to the exported copy.
    CFM finds shared libraries ('shlb') by file type and reads their 'cfrg' resource, and
    suitcase fonts and QuickTime's .rsrc files live entirely in the resource fork, so a plain
    data-fork copy is not enough. tar on 10.6 stores both as ._ entries; Mojave's tar restores them."""
    try:
        import xattr
    except ImportError:
        xattr = None
    if xattr is not None:
        try:
            fi = xattr.getxattr(src, 'com.apple.FinderInfo')
            xattr.setxattr(out, 'com.apple.FinderInfo', fi)
        except (IOError, OSError, KeyError):
            pass
    try:
        rs = src + '/..namedfork/rsrc'
        if os.path.getsize(rs) > 0:
            shutil.copyfile(rs, out + '/..namedfork/rsrc')
    except (IOError, OSError):
        pass


def sha1(path):
    h = hashlib.sha1()
    f = open(path, 'rb')
    while 1:
        b = f.read(1 << 20)
        if not b:
            break
        h.update(b)
    f.close()
    return h.hexdigest()


def resolve(p):
    """realpath, but inside ROOT: an absolute symlink in the tree means ROOT/<target>."""
    if not ROOT:
        return os.path.realpath(p)
    done = ''
    parts = [x for x in p[len(ROOT):].split('/') if x]
    hops = 0
    while parts:
        c = parts.pop(0)
        if c == '.':
            continue
        if c == '..':
            done = done.rsplit('/', 1)[0]
            continue
        cand = done + '/' + c
        full = ROOT + cand
        if os.path.islink(full) and hops < 40:
            hops += 1
            t = os.readlink(full)
            if t.startswith('/'):
                done = ''
            parts = [x for x in t.split('/') if x] + parts
        else:
            done = cand
    return ROOT + done


def find_src(rel):
    """/rel, or the same name in the other Unicode normal form (HFS+ vs. the manifest)."""
    cands = [rel]
    try:
        u = rel.decode('utf-8')
        for form in ('NFD', 'NFC'):
            q = unicodedata.normalize(form, u).encode('utf-8')
            if q not in cands:
                cands.append(q)
    except Exception:
        pass
    for r in cands:
        d, b = os.path.split(r)
        p = (resolve(ROOT + '/' + d) if ROOT else '/' + d) + '/' + b
        if os.path.lexists(p):
            return p
    return None


CPU = {'ppc': (18, None), 'ppc7400': (18, 10), 'ppc970': (18, 100), 'ppc64': (0x01000012, None),
       'i386': (7, None), 'x86_64': (0x01000007, None)}


def py_thin(src, arch, dst):
    """lipo -thin without lipo (Mojave without the developer tools): the slice is copied as is."""
    want = CPU.get(arch)
    if want is None:
        return False
    f = open(src, 'rb')
    h = f.read(8)
    if len(h) < 8 or h[:4] != '\xca\xfe\xba\xbe':
        return False
    n = struct.unpack('>I', h[4:])[0]
    arches = [struct.unpack('>iiIII', f.read(20)) for i in range(n)]
    pick = [a for a in arches if a[0] == want[0] and (want[1] is None or a[1] & 0xffffff == want[1])]
    if not pick:
        return False
    f.seek(pick[0][2])
    open(dst, 'wb').write(f.read(pick[0][3]))
    return True


def lipo_thin(src, arch, dst):
    try:
        if subprocess.call(['lipo', src, '-thin', arch, '-output', dst],
                           stderr=open('/dev/null', 'w')) == 0:
            return True
    except OSError:
        pass
    return py_thin(src, arch, dst)


def splice_translate(dumper, dest, mism):
    print 'decoding translate (runs a small PowerPC program under Rosetta)...'
    if sha1(TRANSLATE) != TRANSLATE_SHA1:
        print 'WARNING: %s is not the 10.6.8 build this release was made for' % TRANSLATE
    text = os.path.join(dest, 'translate_text.bin')
    rc = subprocess.call([dumper, text])
    if rc != 0 or not os.path.isfile(text) or os.path.getsize(text) != TEXT_SIZE:
        sys.exit('could not dump translate (is Rosetta installed? run "Install Rosetta.app" first)')
    data = bytearray(open(TRANSLATE, 'rb').read())
    t = open(text, 'rb').read()
    data[0:TEXT_SIZE] = t
    # clear SG_PROTECTED_VERSION_1 (0x8) in the __TEXT segment flags
    ncmds = struct.unpack('<I', bytes(data[16:20]))[0]
    p = 28
    for i in range(ncmds):
        cmd, sz = struct.unpack('<II', bytes(data[p:p + 8]))
        if cmd == 1 and bytes(data[p + 8:p + 24]).rstrip('\0') == '__TEXT':
            fl = struct.unpack('<I', bytes(data[p + 52:p + 56]))[0]
            data[p + 52:p + 56] = struct.pack('<I', fl & ~0x8)
        p += sz
    out = os.path.join(dest, 'root', 'translate.decoded')
    open(out, 'wb').write(bytes(data))
    os.remove(text)
    return out


def main():
    global ROOT
    args = sys.argv[1:]
    decoded = None
    while '--root' in args or '--decoded' in args:
        for k in ('--root', '--decoded'):
            if k in args:
                i = args.index(k)
                v = args[i + 1]
                del args[i:i + 2]
                if k == '--root':
                    ROOT = os.path.realpath(v).rstrip('/')
                else:
                    decoded = v
    if len(args) != 3 or (ROOT and not decoded):
        sys.exit(__doc__)
    manifest, dumper, dest = args
    root = os.path.join(dest, 'root')
    if not os.path.isdir(root):
        os.makedirs(root)
    entries = [l.rstrip('\n').split('\t') for l in gzip.open(manifest) if l.strip()]
    mism = []
    missing = []
    n = 0
    for rel, method, size, want in entries:
        n += 1
        if n % 5000 == 0:
            print '  %d / %d' % (n, len(entries))
            sys.stdout.flush()
        out = os.path.join(root, rel)
        d = os.path.dirname(out)
        if not os.path.isdir(d):
            os.makedirs(d)
        if os.path.lexists(out):
            os.remove(out)
        if method.startswith('link:'):
            os.symlink(method[5:], out)
            continue
        src = find_src(rel)
        if src is None:
            if method != 'optional':
                missing.append(rel)
            continue
        real = resolve(src)
        if not os.path.isfile(real):
            if method != 'optional':
                missing.append(rel)
            continue
        if method.startswith('thin:'):
            if not lipo_thin(real, method[5:], out):
                shutil.copyfile(real, out)          # already thin
        else:
            shutil.copyfile(real, out)
        copy_mac_meta(real, out)
        try:
            os.chmod(out, os.stat(real).st_mode & 0777 | 0200)
        except OSError:
            pass
        if method != 'optional' and want != '-' and sha1(out) != want:
            mism.append(rel)

    if decoded:
        out = os.path.join(root, 'translate.decoded')
        shutil.copyfile(decoded, out)
    else:
        out = splice_translate(dumper, dest, mism)
    os.chmod(out, 0755)
    if sha1(out) != DECODED_SHA1:
        mism.append('translate.decoded')

    open(os.path.join(dest, 'mismatch.txt'), 'w').write(''.join(m + '\n' for m in mism))
    open(os.path.join(dest, 'missing.txt'), 'w').write(''.join(m + '\n' for m in missing))
    print '%d entries, %d missing, %d not matching the manifest' % (len(entries), len(missing), len(mism))
    if missing:
        print 'missing (first 10):'
        for m in missing[:10]:
            print '   ', m


if __name__ == '__main__':
    main()
