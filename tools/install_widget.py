#!/usr/bin/python
"""install_widget.py - install a Dashboard widget made for PowerPC Macs on Mojave.

usage: install_widget.py PATH/TO/Some.wdgt [--uninstall]

Dashboard widgets are HTML, CSS and JavaScript, which Mojave's Dashboard runs as is. What can stop
an old widget is a native widget plug-in (the Info.plist "Plugin" key, a .widgetplugin bundle)
built only for PowerPC: Dashboard is a 64-bit Intel process and cannot load it, and plug-ins run
inside Dashboard, so Rosetta cannot translate them there. This tool copies the widget into
~/Library/Widgets and, when its plug-in is PowerPC-only (or 32-bit only), detaches it, so the
widget loads and every feature that does not need the plug-in works. It prints what it changed.

Dashboard is off by default on Mojave: System Preferences > Mission Control > Dashboard.
"""
import os, plistlib, shutil, subprocess, sys

DEST_DIR = os.path.expanduser('~/Library/Widgets')


def archs(path):
    try:
        out = subprocess.check_output(['lipo', '-info', path], stderr=open(os.devnull, 'w'))
    except (subprocess.CalledProcessError, OSError):
        return []
    return out.decode('utf-8', 'replace').rsplit(':', 1)[-1].split()


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    src = os.path.abspath(argv[1].rstrip('/'))
    dest = os.path.join(DEST_DIR, os.path.basename(src))
    if '--uninstall' in argv:
        shutil.rmtree(dest, ignore_errors=True)
        print('removed %s' % dest)
        return
    if not (os.path.isdir(src) and src.endswith('.wdgt')):
        sys.exit('not a .wdgt widget: %s' % src)
    if not os.path.isdir(DEST_DIR):
        os.makedirs(DEST_DIR)
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(src, dest, symlinks=True)
    pl_path = os.path.join(dest, 'Info.plist')
    pl = plistlib.readPlist(pl_path)
    plugin = pl.get('Plugin')
    if plugin:
        pdir = os.path.join(dest, plugin)
        exe = None
        try:
            ppl = plistlib.readPlist(os.path.join(pdir, 'Contents', 'Info.plist'))
            exe = os.path.join(pdir, 'Contents', 'MacOS', ppl.get('CFBundleExecutable', ''))
        except Exception:
            pass
        a = archs(exe) if exe and os.path.isfile(exe) else []
        if 'x86_64' in a:
            print('plug-in %s has a 64-bit Intel slice: kept' % plugin)
        else:
            del pl['Plugin']
            pl['RosettaDetachedPlugin'] = plugin
            plistlib.writePlist(pl, pl_path)
            print('plug-in %s (%s) cannot load in Mojave\'s Dashboard: detached;' % (plugin, ' '.join(a) or 'unknown'))
            print('  the widget works except for features that need the plug-in.')
    subprocess.call(['xattr', '-dr', 'com.apple.quarantine', dest])
    print('installed %s' % dest)
    print('Open Dashboard (System Preferences > Mission Control > Dashboard: As Overlay or As Space) and add it with "+".')


if __name__ == '__main__':
    main(sys.argv)
