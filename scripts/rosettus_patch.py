#!/usr/bin/python
# rosettus_patch.py [--unpatch|--check] App.app ...
# Makes a PowerPC application open with a double-click on Mojave: its main executable becomes a fat
# file holding the original PowerPC code plus the native "rstub" launcher (x86_64). Name, icon, bundle
# id, document types and the app's own files stay as they are. The original executable is kept as
# Contents/Resources/rosetta-original-executable; --unpatch puts it back.
# Classic Carbon (CFM/PEF) apps keep their PEF file; a native stub is added next to it (patch_cfm).
# Exit status: 0 patched / already patched, 2 not a PowerPC-only app, 1 error.
import sys, os, struct, plistlib, subprocess, shutil

HERE = os.path.dirname(os.path.realpath(__file__))
STUB = os.environ.get('ROSETTA_STUB') or os.path.join(HERE, 'rlaunch')
CPU_PPC, CPU_PPC64, CPU_X86, CPU_X64 = 18, 0x01000012, 7, 0x01000007

def slices(data):
    m = struct.unpack('>I', data[:4])[0]
    if m == 0xcafebabe:
        n = struct.unpack('>I', data[4:8])[0]
        out = []
        for i in range(n):
            ct, cs, off, sz, al = struct.unpack('>5I', data[8 + 20 * i:28 + 20 * i])
            out.append((ct, cs, data[off:off + sz], al))
        return out
    if m == 0xfeedface: ct, cs = struct.unpack('>II', data[4:12]); return [(ct, cs, data, 12)]
    if m == 0xfeedfacf: ct, cs = struct.unpack('>II', data[4:12]); return [(ct, cs, data, 12)]
    if m in (0xcefaedfe, 0xcffaedfe): ct, cs = struct.unpack('<II', data[4:12]); return [(ct, cs, data, 12)]
    return None

def fat(sl):
    hdr = struct.pack('>II', 0xcafebabe, len(sl))
    off = 4096; body = []; arch = b''
    for ct, cs, d, al in sl:
        a = 1 << al
        off = (off + a - 1) & ~(a - 1)
        arch += struct.pack('>5I', ct, cs, off, len(d), al)
        body.append((off, d)); off += len(d)
    out = bytearray(off)
    out[0:len(hdr) + len(arch)] = hdr + arch
    for o, d in body: out[o:o + len(d)] = d
    return bytes(out)

def plist_path(app):
    """Info.plist, or Info-macos.plist (old Carbon bundles such as AppleWorks 6)"""
    for n in ('Info.plist', 'Info-macos.plist'):
        pl = os.path.join(app, 'Contents', n)
        if os.path.exists(pl): return pl
    return os.path.join(app, 'Contents', 'Info.plist')

def info(app):
    pl = plist_path(app)
    try: p = plistlib.readPlist(pl)
    except Exception:
        out = subprocess.Popen(['plutil', '-convert', 'xml1', '-o', '-', pl], stdout=subprocess.PIPE).communicate()[0]
        p = plistlib.readPlistFromString(out)
    exe = p.get('CFBundleExecutable') or os.path.splitext(os.path.basename(app.rstrip('/')))[0]
    return os.path.join(app, 'Contents', 'MacOS', exe)

def backup_path(app): return os.path.join(app, 'Contents', 'Resources', 'rosetta-original-executable')

def refresh(app):
    subprocess.call(['xattr', '-dr', 'com.apple.quarantine', app], stderr=open(os.devnull, 'w'))
    os.utime(app, None)
    subprocess.call(['/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister', '-f', app])

PLIST_BACKUP = ('Contents', 'Resources', 'rosetta-original-Info.plist')
CFM_STUB = 'rosetta-launch'

def read_plist(app):
    pl = plist_path(app)
    try: return plistlib.readPlist(pl)
    except Exception:
        out = subprocess.Popen(['plutil', '-convert', 'xml1', '-o', '-', pl], stdout=subprocess.PIPE).communicate()[0]
        return plistlib.readPlistFromString(out)

def patch_cfm(app, exe):
    """Classic Carbon (CFM/PEF) app: a PEF file cannot become fat, so the native stub is added as
    Contents/MacOS/rosetta-launch and Info.plist points at it; RosettaOriginalExecutable keeps the
    PEF name for the rosetta script. Info.plist is backed up first (--unpatch restores it)."""
    stub = slices(open(STUB, 'rb').read())
    stub = [s for s in stub if s[0] == CPU_X64]
    if not stub: print 'no x86_64 stub in %s' % STUB; return 1
    pl_path = plist_path(app)
    bak = os.path.join(app, *PLIST_BACKUP)
    if not os.path.isdir(os.path.dirname(bak)): os.makedirs(os.path.dirname(bak))
    if not os.path.exists(bak): shutil.copy2(pl_path, bak)
    p = read_plist(app)
    p['RosettaOriginalExecutable'] = os.path.basename(exe)
    p['CFBundleExecutable'] = CFM_STUB
    # macOS 10.14 asks for the camera / microphone only for an app that says why (and ends one that
    # opens the camera without saying it)
    nm = p.get('CFBundleName') or os.path.basename(app)[:-4]
    p.setdefault('NSCameraUsageDescription', '%s (a PowerPC app run by Rosettus) uses the camera for video capture.' % nm)
    p.setdefault('NSMicrophoneUsageDescription', '%s (a PowerPC app run by Rosettus) uses the microphone for sound input.' % nm)
    s = os.path.join(app, 'Contents', 'MacOS', CFM_STUB)
    open(s, 'wb').write(stub[0][2]); os.chmod(s, 0o755)
    plistlib.writePlist(p, pl_path)
    # CFBundle finds a CFM app's resources (its 'cfrg' fragment list above all) as
    # Resources/<CFBundleExecutable>.rsrc: give the stub's name the original file
    rs = os.path.join(app, 'Contents', 'Resources', os.path.basename(exe) + '.rsrc')
    ln = os.path.join(app, 'Contents', 'Resources', CFM_STUB + '.rsrc')
    if os.path.exists(rs) and not os.path.lexists(ln): os.symlink(os.path.basename(rs), ln)
    env = os.path.join(app, 'Contents', 'Resources', 'rosetta.env')
    if not os.path.exists(env): open(env, 'w').write('export OAH_GTB=1\n')
    refresh(app)
    print '%s: patched (Carbon CFM app + native launcher)' % app
    return 0

def update_stub(app):
    """An app that is already patched: bring its native launcher up to date (the PowerPC code
    and the backups are left alone). Returns 0 either way."""
    stub = [x for x in slices(open(STUB, 'rb').read()) if x[0] == CPU_X64]
    if not stub: return 0
    cfm = os.path.join(app, 'Contents', 'MacOS', CFM_STUB)
    if os.path.exists(os.path.join(app, *PLIST_BACKUP)) and os.path.exists(cfm):
        if open(cfm, 'rb').read() == stub[0][2]: print '%s: already patched' % app; return 0
        tmp = cfm + '.rosettus-new'
        open(tmp, 'wb').write(stub[0][2]); os.chmod(tmp, 0o755); os.rename(tmp, cfm)
    else:
        exe = info(app)
        sl = slices(open(exe, 'rb').read())
        cur = [x for x in sl if x[0] == CPU_X64]
        if not sl or (cur and cur[0][2] == stub[0][2]): print '%s: already patched' % app; return 0
        new = fat([x for x in sl if x[0] in (CPU_PPC, CPU_PPC64)] + stub)
        tmp = exe + '.rosettus-new'
        open(tmp, 'wb').write(new); os.chmod(tmp, 0o755); os.rename(tmp, exe)
    refresh(app)
    print '%s: already patched; native launcher updated' % app
    return 0

def patch(app):
    if os.path.exists(os.path.join(app, *PLIST_BACKUP)): return update_stub(app)
    exe = info(app)
    data = open(exe, 'rb').read()
    if data[:8] == b'Joy!peff': return patch_cfm(app, exe)
    sl = slices(data)
    if not sl: print '%s: executable is not Mach-O' % app; return 2
    cts = [s[0] for s in sl]
    if CPU_X64 in cts or CPU_X86 in cts:
        if os.path.exists(backup_path(app)): return update_stub(app)
        print '%s: already has native code (%s); nothing to do' % (app, cts); return 2
    if CPU_PPC not in cts: print '%s: no 32-bit PowerPC code' % app; return 2
    stub = slices(open(STUB, 'rb').read())
    stub = [s for s in stub if s[0] == CPU_X64]
    if not stub: print 'no x86_64 stub in %s' % STUB; return 1
    if not os.path.isdir(os.path.dirname(backup_path(app))): os.makedirs(os.path.dirname(backup_path(app)))   # bundles without Resources
    if not os.path.exists(backup_path(app)): shutil.copy2(exe, backup_path(app))
    new = fat([s for s in sl if s[0] in (CPU_PPC, CPU_PPC64)] + stub)
    tmp = exe + '.rosettus-new'
    open(tmp, 'wb').write(new); os.chmod(tmp, 0o755); os.rename(tmp, exe)
    env = os.path.join(app, 'Contents', 'Resources', 'rosetta.env')
    if not os.path.exists(env): open(env, 'w').write('export OAH_GTB=1\n')
    refresh(app)
    print '%s: patched (PowerPC + native launcher)' % app
    return 0

def unpatch(app):
    pb = os.path.join(app, *PLIST_BACKUP)
    if os.path.exists(pb):                                   # CFM app patched by patch_cfm
        shutil.copy2(pb, plist_path(app)); os.remove(pb)
        for f in ('Contents/MacOS/' + CFM_STUB, 'Contents/Resources/' + CFM_STUB + '.rsrc', 'Contents/Resources/rosetta.env', 'Contents/MacOS/.rosetta-translate'):
            try: os.remove(os.path.join(app, f))
            except OSError: pass
        refresh(app)
        print '%s: restored' % app
        return 0
    exe = info(app); b = backup_path(app)
    if not os.path.exists(b): print '%s: not patched' % app; return 2
    shutil.copy2(b, exe); os.remove(b)
    for f in ('Contents/Resources/rosetta.env', 'Contents/MacOS/.rosetta-translate'):
        try: os.remove(os.path.join(app, f))
        except OSError: pass
    refresh(app)
    print '%s: restored' % app
    return 0

def check(app):
    if os.path.exists(os.path.join(app, *PLIST_BACKUP)): print 'patched'; return 0
    exe = info(app); data = open(exe, 'rb').read(4096 * 4)
    if data[:8] == b'Joy!peff': print 'cfm'; return 0
    full = open(exe, 'rb').read(); sl = slices(full) or []
    cts = [s[0] for s in sl]
    if os.path.exists(backup_path(app)): print 'patched'
    elif CPU_PPC in cts and CPU_X86 not in cts and CPU_X64 not in cts: print 'ppc'
    elif CPU_PPC in cts: print 'universal'
    else: print 'native'
    return 0

if __name__ == '__main__':
    args = sys.argv[1:]; mode = patch
    if args and args[0] == '--unpatch': mode = unpatch; args = args[1:]
    elif args and args[0] == '--check': mode = check; args = args[1:]
    rc = 0
    for a in args: rc = max(rc, mode(a.rstrip('/')))
    sys.exit(rc)
