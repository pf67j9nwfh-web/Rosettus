#!/bin/bash
# install_qtflash.sh SOURCE [ROOT]
# Adds Flash to the guest's QuickTime 7: QuickTime 7.3.1 and later disabled Flash (no importer,
# and a media handler that draws nothing), so .swf movies failed with -2048 in PowerPC apps
# (Kid Pix's animated stickers). This builds RosettusFlash.component from YOUR OWN QuickTime 6
# for Mac OS X - the Flash 5 media handler and Flash importer of its QuickTimeComponents - and
# puts it in ROOT/System/Library/QuickTime, where Rosettus registers it when an app starts
# QuickTime. Nothing from Apple is shipped with Rosettus.
#   SOURCE: a QuickTime 6.x installer package (e.g. "QuickTime651.pkg", on the Kid Pix Deluxe 4
#           disc), a disk image or folder containing one, or a QuickTimeComponents.component
#   ROOT:   the Rosettus install (default /Library/Rosettus)
# Needs Rez and DeRez (Xcode Command Line Tools). Remove the component to undo; OAH_NO_QTFLASH=1
# skips it for one app.
set -e
SRC="$1"; ROOT="${2:-/Library/Rosettus}"
[ -n "$SRC" ] || { sed -n '2,15p' "$0"; exit 1; }
command -v Rez >/dev/null && command -v DeRez >/dev/null || { echo "Rez/DeRez not found: install the Xcode Command Line Tools (xcode-select --install)"; exit 1; }
[ -d "$ROOT/System/Library/QuickTime" ] || { echo "$ROOT is not a Rosettus install"; exit 1; }
T=$(mktemp -d /tmp/qtflash.XXXX)
MNT=""
cleanup() { [ -n "$MNT" ] && hdiutil detach -quiet "$MNT" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
case "$SRC" in
    *.dmg|*.iso|*.cdr|*.toast)
        IMG=$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")
        # already attached? (hdiutil says "Resource busy") then use its volume
        AT=$(hdiutil info -plist | python -c '
import plistlib, sys, os
img = sys.argv[1]
for i in plistlib.readPlist(sys.stdin).get("images", []):
    if os.path.realpath(i.get("image-path", "")) != os.path.realpath(img): continue
    for e in i.get("system-entities", []):
        if e.get("mount-point"): print(e["mount-point"]); sys.exit(0)
    for e in i.get("system-entities", []):          # attached but not mounted: mount it read-only
        if e.get("content-hint", "").startswith("Apple_HFS") or e.get("content-hint") == "48465300-0000-11AA-AA11-00306543ECAC":
            os.system("diskutil quiet mount readOnly %s" % e["dev-entry"])
            for l in os.popen("mount"):
                if l.startswith(e["dev-entry"] + " on "):
                    print(l.split(" on ", 1)[1].rsplit(" (", 1)[0]); sys.exit(0)
' "$IMG")
        if [ -n "$AT" ] && [ -d "$AT" ]; then SRC="$AT"
        else
            MNT="$T/mnt"; mkdir "$MNT"
            hdiutil attach -readonly -nobrowse -noverify -mountpoint "$MNT" "$SRC" >/dev/null
            SRC="$MNT"
        fi ;;
esac
COMP=""
if [ -d "$SRC/Contents/MacOS" ] && [ -f "$SRC/Contents/MacOS/QuickTimeComponents" ]; then
    COMP="$SRC"
else
    PKG=""
    if [ -f "$SRC/Contents/Archive.pax.gz" ]; then PKG="$SRC"
    else PKG=$(find "$SRC" -maxdepth 4 -name "QuickTime*.pkg" -type d 2>/dev/null | while read -r p; do [ -f "$p/Contents/Archive.pax.gz" ] && { echo "$p"; break; }; done)
    fi
    [ -n "$PKG" ] || { echo "no QuickTime 6 installer package found in $1"; exit 1; }
    echo "unpacking $PKG"
    mkdir "$T/x"; (cd "$T/x" && gunzip -c "$PKG/Contents/Archive.pax.gz" | pax -r 2>/dev/null) || true
    COMP="$T/x/System/Library/QuickTime/QuickTimeComponents.component"
fi
R="$COMP/Contents/Resources/QuickTimeComponents.rsrc"
B="$COMP/Contents/MacOS/QuickTimeComponents"
[ -f "$R" ] && [ -f "$B" ] || { echo "no QuickTimeComponents.component in $1"; exit 1; }
OUT="$T/RosettusFlash.component"
DeRez -useDF "$R" > "$T/all.r"
python - "$T/all.r" "$T/flash.r" <<'EOF'
import sys, re
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
blocks = re.findall(r'(?ms)^data .*?^\};', text)
want = {('thng', -18519), ('thng', -18541), ('dlle', -18519), ('dlle', -18541), ('thga', -18541),
        ('thnr', -18541), ('strn', -18519), ('stri', -18519), ('strn', -19149), ('stri', -19148)}
out, have = [], set()
for b in blocks:
    m = re.match(r"data '(.{4})' \((-?\d+)", b)
    if not m: continue
    k = (m.group(1), int(m.group(2)))
    if k not in want: continue
    if k == ('thng', -18519):
        # QuickTime 7.3.1 and later keep a disabled Flash media handler (version 3.0.15) with
        # automatic versioning: register this one with a higher version so that it is used
        mm = re.search(r'"0003 00([0-9A-F]{2})"', b) or re.search(r'0003 00([0-9A-F]{2})', b)
        if not mm: sys.exit('Flash media handler version not found')
        b = b.replace(mm.group(0), mm.group(0).replace('0003 00' + mm.group(1), '0003 0100'), 1)
    out.append(b.rstrip() + '\n'); have.add(k)
if ('thng', -18519) not in have or ('thng', -18541) not in have:
    sys.exit('this QuickTimeComponents has no Flash 5 components (QuickTime 6 is needed)')
open(dst, 'w').write(''.join(out))
EOF
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
Rez -useDF -o "$OUT/Contents/Resources/RosettusFlash.rsrc" "$T/flash.r"
cp "$B" "$OUT/Contents/MacOS/RosettusFlash"
# Its install name is QuickTimeComponents' own: dyld then handed back the QuickTime 7 component
# binary that is already loaded, and the Flash entry points were not found (OpenComponent -50).
python - "$OUT/Contents/MacOS/RosettusFlash" <<'EOF'
import struct, sys
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
if d[:4] != b'\xfe\xed\xfa\xce': sys.exit('QuickTimeComponents is not a thin PowerPC Mach-O')
ncmds = struct.unpack('>I', bytes(d[16:20]))[0]
o = 28
new = b'/System/Library/QuickTime/RosettusFlash.component/Contents/MacOS/RosettusFlash'
for i in range(ncmds):
    cmd, size = struct.unpack('>II', bytes(d[o:o + 8]))
    if cmd == 0xd:                                   # LC_ID_DYLIB
        off = struct.unpack('>I', bytes(d[o + 8:o + 12]))[0]
        room = size - off
        if len(new) >= room: sys.exit('install name does not fit')
        d[o + off:o + size] = new + b'\0' * (room - len(new))
        open(p, 'wb').write(bytes(d))
        break
    o += size
else:
    sys.exit('no LC_ID_DYLIB')
EOF
cat > "$OUT/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>RosettusFlash</string>
	<key>CFBundleIdentifier</key><string>org.rosettus.flash-component</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>RosettusFlash</string>
	<key>CFBundlePackageType</key><string>thng</string>
	<key>CFBundleSignature</key><string>????</string>
	<key>CFBundleVersion</key><string>6</string>
	<key>CSResourcesFileMapped</key><true/>
</dict>
</plist>
EOF
DST="$ROOT/System/Library/QuickTime/RosettusFlash.component"
rm -rf "$DST.new"
cp -R "$OUT" "$DST.new" && rm -rf "$DST" && mv "$DST.new" "$DST"
echo "installed $DST"
