Rosettus - Snow Leopard files from an install DVD
=================================================

Rosettus runs PowerPC apps on macOS Mojave with Snow Leopard's own Rosetta. Apple's files are not
part of Rosettus. Usually they are exported from a Mac running Snow Leopard (the "Snow Leopard
Export Kit"). Without one, this folder makes the same export on the Mojave Mac itself, from:

  - a Mac OS X 10.6 Snow Leopard install DVD: the disc, or an image of it (.iso / .dmg).
    Rosetta is on every 10.6 disc. Tested with the 10.6.7 disc of the 2011 MacBook Pro; another
    disc can hold another build of a file Rosettus patches, and install.sh then stops and names
    it (please report it, with the disc's part number).
  - Apple's free updates for Snow Leopard, which the tool can download from Apple's servers
    (about 1.5 GB): Mac OS X 10.6.8 Update Combo v1.1, 10.6.8 Supplemental Update, Security
    Update 2013-004, Rosetta (041-85123), Safari 5.1.10, Java for Mac OS X 10.6 (its last
    update) and the Mac App Store update. They bring the DVD's
    files up to the 10.6.8 that Rosettus is made for.

Easiest: double-click "Import from Install DVD.command". It asks for the DVD image (drag it into
the window) and whether to download the updates, and leaves Rosettus-SL-export.tar.gz on your
Desktop. Then open Rosettus Launcher and choose "Install Snow Leopard Files..." (File menu).

From Terminal:

    ./import_from_dvd.sh --dvd ~/Downloads/SnowLeopard.iso --download
    ./import_from_dvd.sh --dvd /Volumes/"Mac OS X Install DVD" --updates ~/SLUpdates [OUT.tar.gz]

It takes 20-40 minutes and about 12 GB of free space in ~/Library/Caches/Rosettus-DVD, which is
removed at the end (--keep keeps it; --work DIR puts it elsewhere).

How it works:
  1. the system packages on the DVD and then the updates are unpacked into the work folder
     (xar and cpio; nothing is installed and nothing on your Mac is changed)
  2. Rosetta's translator keeps its code encrypted on disk and the kernel decrypts it when the
     program runs. Mojave can still run this 32-bit Intel program, so the tool starts a copy of it
     with dump_text_i386.dylib (source: dump_text_i386.c), which saves the decrypted code and
     exits before the translator itself runs. On Snow Leopard, export_from_sl.sh does the same
     with a small PowerPC program.
  3. export_from_sl.py --root collects the files Rosettus needs, as on Snow Leopard.

An install DVD's Snow Leopard is a different build of some files than a Snow Leopard Mac updated
through Software Update, so the installer reports a few thousand "different build" files: that is
expected. The handful of files Rosettus patches have patches for both known builds (import/patches/
*.alt1.rpatch, made with tools/mkaltpatch.py); install.sh picks the one that fits.
