#!/usr/bin/python
"""dvdtool.py - helpers for import_from_dvd.sh (runs on Mojave with the system /usr/bin/python 2.7).

    dvdtool.py updates DIR [--download]   check (or fetch from Apple) the Snow Leopard updates
    dvdtool.py order DVDPKGS UPDATES      print the packages to extract, in order
    dvdtool.py decode TRANSLATE DYLIB OUT decode Rosetta's translator (see below)

Decoding translate: /usr/libexec/oah/translate keeps its __TEXT encrypted on disk (SG_PROTECTED_VERSION_1)
and the kernel decrypts it when it maps the program. Mojave can still run this 32-bit Intel program, so we
start a copy of it with a small library (dump_text_i386.dylib) that writes the mapped, decrypted __TEXT to a
file and exits before translate's own code runs. The text is spliced into the original file with the
protection flag cleared: the same translate.decoded that export_from_sl.py makes on Snow Leopard.
The copy that runs has its Objective-C garbage-collection flags cleared (Mojave's libobjc refuses GC
images); it keeps Apple's signature, without which the kernel ends it.
"""
import hashlib, os, struct, subprocess, sys, tempfile, shutil

TRANSLATE_SHA1 = '860fa160189146f5a19de904b84e5204bd0bc85b'
DECODED_SHA1 = 'f267d31af007bcce60b06de566b3087352edbd17'

# Apple's Snow Leopard updates (Software Update catalog index-leopard-snowleopard.merged-1.sucatalog)
CDN = 'http://swcdn.apple.com/content/downloads/'
COMBO = CDN + '49/43/041-98179-A_LECWR24EMQ/2dml09vesnhzp7lgix8nrewu5moiymbfw8/'
PART_SIZES = [15181012, 43087965, 12227187, 84741548, 54631334, 85204709, 22449437, 24907192,
              951749, 710565, 58699896, 201182363, 124457468]
UPDATES = [('MacOSXUpdCombo10.6.8.Part%d.pkg' % i, COMBO + 'MacOSXUpdCombo10.6.8.Part%d.pkg' % i, s)
           for i, s in enumerate(PART_SIZES)] + [        # (file, url, size) in the order they are applied
    ('SUBaseSystemCombo10.6.8.pkg', COMBO + 'SUBaseSystemCombo10.6.8.pkg', 337054834),
    ('RosettaForCombo10.6.8.pkg', COMBO + 'RosettaForCombo10.6.8.pkg', 2015693),
    ('X11ForCombo10.6.8.pkg', COMBO + 'X11ForCombo10.6.8.pkg', 24623095),
    ('QuickTimePlayer7ForCombo10.6.8.pkg', COMBO + 'QuickTimePlayer7ForCombo10.6.8.pkg', 1493238),
    ('Rosetta.pkg', CDN + '51/25/041-85123-A_WG1OQ4FCWI/n0r7jt76h08bhgs0npexbdma2mwe5mhp7u/Rosetta.pkg', 2016551),
    ('MacOSXUpd10.6.8Supp.pkg', CDN + '41/37/041-87964-A_K41KSMBMV5/x4ivczmm3jifqsgo7lokl8ohejfavop537/MacOSXUpd10.6.8Supp.pkg', 10477018),
    ('SecUpd2013-004.pkg', CDN + '59/44/041-91751-A_6E8UPFATJA/pzuccll6v4vpgcdddnpjn9jq5mhkcvt6j6/SecUpd2013-004.pkg', 348940892),
    ('Safari5.1.10SnowLeopard.pkg', CDN + '51/12/041-92704-A_RWSKU2GQP1/trpie0tx0ehtp9sqkai8qus5pr8gg3fofj/Safari5.1.10SnowLeopard.pkg', 50376463),
    ('JavaForMacOSX10.6.pkg', CDN + '22/51/041-90789-A_MN0WY0B55D/neomuall16qnv716zkql9875ttsc7f0ejk/JavaForMacOSX10.6.pkg', 72179502),
    ('JavaSecurity.pkg', CDN + '22/51/041-90789-A_MN0WY0B55D/neomuall16qnv716zkql9875ttsc7f0ejk/JavaSecurity.pkg', 504938),
    ('MacAppStoreUpdateNB.pkg', CDN + '19/42/041-88414-A_NDYPBI6VJ6/zwx67h93ppwrfuxrshb7rsgrcbbafao61d/MacAppStoreUpdateNB.pkg', 3372458),
    ('MacAppStoreUpdate.pkg', CDN + '19/42/041-88414-A_NDYPBI6VJ6/zwx67h93ppwrfuxrshb7rsgrcbbafao61d/MacAppStoreUpdate.pkg', 79267),
]

# packages from the install DVD, in order (missing ones are skipped: DVDs differ a little)
DVD_PKGS = ('BaseSystem Essentials BSD AdditionalEssentials AdditionalFonts AdditionalSpeechVoices '
            'AsianLanguagesSupport Rosetta X11User Java MediaFiles QuickTimePlayer7 Safari Mail iCal iChat '
            'AddressBook Automator DVDPlayer iTunes iPodSupport OxfordDictionaries MigrationAssistant '
            'PodcastCapture BootCamp BrazilianPortuguese Danish Dutch Finnish French German Italian Japanese '
            'Korean Norwegian Polish Portuguese Russian SimplifiedChinese Spanish Swedish TraditionalChinese').split()


def updates(d, download):
    if not os.path.isdir(d):
        os.makedirs(d)
    missing = []
    total = sum(s for f, u, s in UPDATES if not (os.path.isfile(os.path.join(d, f)) and os.path.getsize(os.path.join(d, f)) == s))
    if download and total:
        print '  %.0f MB to download from Apple' % (total / 1e6)
    for f, url, size in UPDATES:
        p = os.path.join(d, f)
        if os.path.isfile(p) and os.path.getsize(p) == size:
            continue
        if not download:
            missing.append((f, url, size))
            continue
        print '  downloading %s (%.0f MB)' % (f, size / 1e6)
        sys.stdout.flush()
        if os.path.isfile(p) and os.path.getsize(p) > size:
            os.remove(p)
        rc = subprocess.call(['curl', '-fsS', '-L', '--retry', '3', '-C', '-', '-o', p, url])
        if rc != 0 or not os.path.isfile(p) or os.path.getsize(p) != size:
            missing.append((f, url, size))
    if missing:
        print 'These Snow Leopard updates are not in %s:' % d
        for f, url, size in missing:
            print '   %s  (%d bytes)\n      %s' % (f, size, url)
        sys.exit(2)


def order(dvd, upd):
    for p in DVD_PKGS:
        f = os.path.join(dvd, p + '.pkg')
        if os.path.isfile(f):
            print f
    for f, url, size in UPDATES:
        print os.path.join(upd, f)


def sha1(path):
    return hashlib.sha1(open(path, 'rb').read()).hexdigest()


def segments(data):
    """[(load command offset, segname, fileoff, filesize, [(sectname, offset, size)])] of a thin i386 Mach-O"""
    magic, cpu, sub, ftype, ncmds = struct.unpack('<IiiII', bytes(data[:20]))
    if magic != 0xfeedface:
        sys.exit('translate is not a 32-bit Intel program')
    p = 28
    out = []
    for i in range(ncmds):
        cmd, sz = struct.unpack('<II', bytes(data[p:p + 8]))
        if cmd == 1:
            seg = bytes(data[p + 8:p + 24]).rstrip('\0')
            fileoff, filesize = struct.unpack('<II', bytes(data[p + 32:p + 40]))
            nsects = struct.unpack('<I', bytes(data[p + 48:p + 52]))[0]
            secs = []
            for k in range(nsects):
                q = p + 56 + 68 * k
                sn = bytes(data[q:q + 16]).rstrip('\0')
                ssize, soff = struct.unpack('<II', bytes(data[q + 36:q + 44]))
                secs.append((sn, soff, ssize))
            out.append((p, seg, fileoff, filesize, secs))
        p += sz
    return out


def decode(translate, dylib, out):
    if sha1(translate) != TRANSLATE_SHA1:
        print 'note: %s is not the Rosetta build this release was made for' % translate
    work = tempfile.mkdtemp(prefix='rosettus-tr.')
    try:
        data = bytearray(open(translate, 'rb').read())
        # the copy that runs: Objective-C GC flags cleared, Apple's signature kept
        run = bytearray(data)
        n = 0
        for p, seg, fo, fs, secs in segments(run):
            for sn, soff, ssize in secs:
                if sn in ('__image_info', '__objc_imageinfo') and ssize >= 8:
                    run[soff + 4:soff + 8] = '\0\0\0\0'
                    n += 1
        if not n:
            print 'note: no Objective-C image info found in translate'
        exe = os.path.join(work, 'translate')
        open(exe, 'wb').write(bytes(run))
        os.chmod(exe, 0755)
        text = os.path.join(work, 'text.bin')
        env = dict(os.environ, ROSETTUS_DUMP_TEXT=text, DYLD_INSERT_LIBRARIES=os.path.abspath(dylib))
        rc = subprocess.call([exe], env=env, cwd=work)
        if rc != 0 or not os.path.isfile(text):
            sys.exit('could not run the Snow Leopard translator to decode it (exit %d)' % rc)
        t = open(text, 'rb').read()
        tseg = [s for s in segments(data) if s[1] == '__TEXT'][0]
        if len(t) != tseg[3]:
            sys.exit('decoded text has %d bytes, expected %d' % (len(t), tseg[3]))
        data[0:len(t)] = t
        p = tseg[0]
        fl = struct.unpack('<I', bytes(data[p + 52:p + 56]))[0]
        data[p + 52:p + 56] = struct.pack('<I', fl & ~0x8)       # SG_PROTECTED_VERSION_1
        open(out, 'wb').write(bytes(data))
        os.chmod(out, 0755)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    h = sha1(out)
    print 'translate decoded: %s' % ('OK' if h == DECODED_SHA1 else 'sha1 %s (expected %s)' % (h, DECODED_SHA1))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['updates'] and len(a) in (2, 3):
        updates(a[1], '--download' in a)
    elif a[:1] == ['order'] and len(a) == 3:
        order(a[1], a[2])
    elif a[:1] == ['decode'] and len(a) == 4:
        decode(a[1], a[2], a[3])
    else:
        sys.exit(__doc__)
