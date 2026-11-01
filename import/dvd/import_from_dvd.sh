#!/bin/bash
# import_from_dvd.sh - make the Snow Leopard export ON MOJAVE, from a Mac OS X 10.6 install DVD,
# for people who have no Snow Leopard Mac to run import/sl/export_from_sl.sh on.
#
#   ./import_from_dvd.sh --dvd "Mac OS X Install DVD.iso" --download [OUTPUT.tar.gz]
#
# What it does:
#   1. opens the DVD (an .iso/.dmg image, or a mounted /Volumes/... disc) and unpacks its system packages
#   2. unpacks Apple's 10.6.8 updates over them: the 10.6.8 Combo update v1.1, the 10.6.8 Supplemental
#      update, Security Update 2013-004, Rosetta (041-85123), Safari 5.1.10, the last Java for Mac OS X 10.6
#      update and the Mac App Store update.
#      --download fetches them from Apple's servers (about 1.5 GB); --updates DIR uses ones you have
#   3. decodes Rosetta's translator (dvdtool.py decode: Mojave still runs the 32-bit Intel translate,
#      and a small library saves the text the kernel decrypts; nothing is downloaded for this)
#   4. collects what Rosettus needs (export_from_sl.py --root) into OUTPUT
#      (default ~/Desktop/Rosettus-SL-export.tar.gz), which you then give to install.sh or the Launcher.
# Needs about 12 GB free in the work folder (default ~/Library/Caches/Rosettus-DVD); it is removed at
# the end (with the downloaded updates) unless --keep.
# Apple's files stay on your Mac: nothing is uploaded and the repository holds none of them.
#
# options: --dvd PATH  --updates DIR  --download  --work DIR  --keep (keep the work folder)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DVD=; UPD=; DL=0; WORK=; KEEP=0; OUT=
while [ $# -gt 0 ]; do
  case "$1" in
    --dvd) DVD="$2"; shift 2;;
    --updates) UPD="$2"; shift 2;;
    --download) DL=1; shift;;
    --work) WORK="$2"; shift 2;;
    --keep) KEEP=1; shift;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*) echo "unknown option $1"; exit 1;;
    *) OUT="$1"; shift;;
  esac
done
OUT="${OUT:-$HOME/Desktop/Rosettus-SL-export.tar.gz}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT";; esac
[ -n "$DVD" ] || { echo "give the Snow Leopard install DVD: --dvd FILE.iso (or the mounted /Volumes/... disc)"; exit 1; }
for f in dvdtool.py dump_text_i386.dylib; do
  [ -f "$HERE/$f" ] || { echo "$f is missing next to this script"; exit 1; }
done
EXPORTPY="$HERE/export_from_sl.py"; [ -f "$EXPORTPY" ] || EXPORTPY="$HERE/../sl/export_from_sl.py"   # kit / repository
[ -f "$EXPORTPY" ] || { echo "export_from_sl.py not found"; exit 1; }
MANIFEST="$HERE/../manifest.tsv.gz"; [ -f "$MANIFEST" ] || MANIFEST="$HERE/manifest.tsv.gz"
[ -f "$MANIFEST" ] || { echo "manifest.tsv.gz not found"; exit 1; }
PY=/usr/bin/python
mkdir -p "$(dirname "$OUT")"
DEFWORK=0; [ -n "$WORK" ] || { WORK="$HOME/Library/Caches/Rosettus-DVD"; DEFWORK=1; }
mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"
avail=$(df -k "$WORK" | awk 'NR==2 {print $4}')
[ "${avail:-0}" -ge 12000000 ] || { echo "need about 12 GB free in $WORK"; exit 1; }
UPD="${UPD:-$WORK/updates}"
MNT=; ATTACHED=
cleanup() {
  [ -n "$ATTACHED" ] && hdiutil detach -quiet "$ATTACHED" 2>/dev/null || true
  [ $KEEP = 1 ] || rm -rf "$WORK/root" "$WORK/export" "$WORK/x" "$WORK/translate.decoded" "$WORK/attach.plist"
  [ $KEEP = 1 ] || [ $DEFWORK = 0 ] || [ $DONE = 0 ] || rm -rf "$WORK"
}
DONE=0
trap cleanup EXIT

say() { echo; echo "== $*"; }

say "Opening the install DVD"
if [ -d "$DVD" ]; then MNT="$DVD"
else
  [ -f "$DVD" ] || { echo "$DVD not found"; exit 1; }
  plist="$WORK/attach.plist"
  hdiutil attach -nobrowse -readonly -noverify -mountrandom /tmp -plist "$DVD" > "$plist" ||
    { echo "could not open $DVD"; exit 1; }
  MNT=$($PY - "$plist" <<'EOF'
import plistlib, sys, os
for e in plistlib.readPlist(sys.argv[1]).get('system-entities', []):
    m = e.get('mount-point')
    if m and os.path.isdir(os.path.join(m, 'System/Installation/Packages')):
        print m; break
EOF
)
  ATTACHED=$($PY -c 'import plistlib,sys; print [e["dev-entry"] for e in plistlib.readPlist(sys.argv[1])["system-entities"] if "dev-entry" in e][0]' "$plist" 2>/dev/null || true)
  ATTACHED="${ATTACHED%s[0-9]*}"
fi
PK="$MNT/System/Installation/Packages"
[ -d "$PK" ] || { echo "$DVD is not a Mac OS X install disc (no System/Installation/Packages)"; exit 1; }
[ -f "$PK/BaseSystem.pkg" ] && [ -f "$PK/Essentials.pkg" ] || { echo "$DVD does not look like a 10.6 install DVD"; exit 1; }
echo "   $MNT"

say "Snow Leopard updates ($UPD)"
if [ $DL = 1 ]; then $PY "$HERE/dvdtool.py" updates "$UPD" --download
else $PY "$HERE/dvdtool.py" updates "$UPD" || {
  echo; echo "Put them in $UPD (or pass --updates DIR), or add --download to fetch them from Apple."; exit 1; }
fi

say "Unpacking the system packages (10-20 minutes)"
R="$WORK/root"; rm -rf "$R"; mkdir -p "$R"
$PY "$HERE/dvdtool.py" order "$PK" "$UPD" | while IFS= read -r p; do
  rm -rf "$WORK/x"; mkdir -p "$WORK/x"
  ( cd "$WORK/x" && xar -xf "$p" Payload 2>/dev/null ) || { echo "   could not open $(basename "$p")"; exit 1; }
  if [ -f "$WORK/x/Payload" ]; then
    ( cd "$R" && gunzip -c "$WORK/x/Payload" | cpio -idmu --quiet 2>>"$WORK/cpio.log" ) || true
    echo "   $(basename "$p")"
  fi
done
rm -rf "$WORK/x"
[ -x "$R/usr/libexec/oah/translate" ] || { echo "Rosetta's translate is not in the unpacked files"; exit 1; }
echo "   merging resource forks..."
dot_clean -m "$R" 2>/dev/null || true

say "Decoding Rosetta's translator"
$PY "$HERE/dvdtool.py" decode "$R/usr/libexec/oah/translate" "$HERE/dump_text_i386.dylib" "$WORK/translate.decoded"

say "Collecting the files Rosettus needs"
rm -rf "$WORK/export"
$PY "$EXPORTPY" "$MANIFEST" - "$WORK/export" --root "$R" --decoded "$WORK/translate.decoded"
rm -rf "$R"

say "Writing $OUT"
( cd "$WORK/export" && cp mismatch.txt missing.txt root/ 2>/dev/null
  printf 'ProductName:\tMac OS X (from install DVD + 10.6.8 updates)\nProductVersion:\t10.6.8\nSource:\t%s\n' "$(basename "$DVD")" > root/sl_version.txt
  tar czf "$OUT" -C root . )
ls -lh "$OUT"
[ $KEEP = 1 ] || rm -rf "$WORK/export"
DONE=1
echo
echo "Done. Install it with:   ./install.sh \"$OUT\""
echo "or the Rosettus Launcher: Install Snow Leopard Files..."
if [ $KEEP = 1 ] || [ $DEFWORK = 0 ]; then echo "(the work folder $WORK stays; delete it when you no longer need it)"; fi
