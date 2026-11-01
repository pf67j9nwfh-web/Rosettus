#!/usr/bin/python
"""install_saver.py - make a PowerPC screen saver usable on Mojave.

usage: install_saver.py PATH/TO/Some.saver [--uninstall]

Creates ~/Library/Screen Savers/<Name> (PowerPC).saver: a copy of the native shim saver
(ROOT/savers/RosettusSaverShim.saver) with the original saver inside it, a unique class name, and the
Rosetta install folder recorded. Pick it in System Preferences > Desktop & Screen Saver; when it
runs, the original runs full screen through Rosetta (the small preview only shows its name).
"""
import hashlib, os, plistlib, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SHIM = os.path.join(ROOT, 'savers', 'RosettusSaverShim.saver')
PLACEHOLDER = b'000000000000'
DEST_DIR = os.path.expanduser('~/Library/Screen Savers')


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    src = os.path.abspath(argv[1].rstrip('/'))
    name = os.path.splitext(os.path.basename(src))[0]
    dest = os.path.join(DEST_DIR, '%s (PowerPC).saver' % name)
    if '--uninstall' in argv:
        shutil.rmtree(dest, ignore_errors=True)
        print('removed %s' % dest)
        return
    if not os.path.isdir(src) or not src.endswith('.saver'):
        sys.exit('not a .saver bundle: %s' % src)
    if not os.path.isdir(SHIM):
        sys.exit('missing %s - reinstall Rosettus' % SHIM)
    uid = hashlib.sha1(src.encode('utf-8')).hexdigest()[:12].encode('ascii')
    if not os.path.isdir(DEST_DIR):
        os.makedirs(DEST_DIR)
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(SHIM, dest, symlinks=True)
    shutil.copytree(src, os.path.join(dest, 'Contents', 'Resources', 'Original.saver'), symlinks=True)
    exe = os.path.join(dest, 'Contents', 'MacOS', 'RosettusSaverShim')
    data = open(exe, 'rb').read()
    open(exe, 'wb').write(data.replace(b'RosettusSaverShim_' + PLACEHOLDER, b'RosettusSaverShim_' + uid))
    pl_path = os.path.join(dest, 'Contents', 'Info.plist')
    pl = plistlib.readPlist(pl_path)
    pl['NSPrincipalClass'] = 'RosettusSaverShim_' + uid.decode('ascii')
    pl['CFBundleIdentifier'] = 'io.github.rosettus.savershim.' + uid.decode('ascii')
    pl['CFBundleName'] = '%s (PowerPC)' % name
    pl['RosettusRoot'] = ROOT
    plistlib.writePlist(pl, pl_path)
    subprocess.call(['xattr', '-dr', 'com.apple.quarantine', dest])
    subprocess.call(['codesign', '-f', '-s', '-', dest], stderr=open(os.devnull, 'w'))
    print('installed %s' % dest)
    print('Choose "%s (PowerPC)" in System Preferences > Desktop & Screen Saver.' % name)


if __name__ == '__main__':
    main(sys.argv)
