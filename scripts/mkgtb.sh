#!/bin/bash
# Build the guest-toolbox overlay: copies of guest framework binaries that the
# "rosetta" script puts first in the guest framework path when OAH_GTB=1.
# Each starts from the unpatched binary (.prebridge if the main tree has one);
# the bridge marks for this mode come from the *G lines in bridges.spec.
# Everything else in the framework (Resources, PlugIns...) is a symlink to the
# real guest tree, so bundles still find their files.
# QuickTime has no guest-toolbox bridge marks: in this mode movies are played by the guest's own
# QuickTime 7, drawing into guest QuickDraw ports (the native-QuickTime bridges need native ports:
# NSMovie returned nil, CS2's installer slideshow stayed blank).
# usage: mkgtb.sh [name ...]   (default: all)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd -P)"; cd "$ROOT"
R=System/Library/Frameworks
mk() {   # name  real-framework-dir
  local n=$1 real=$2 d=gtb/Frameworks/$1.framework src f
  rm -rf "$d"
  mkdir -p "$d/Versions/A"
  src="$real/Versions/A/$n.prebridge"; [ -f "$src" ] || src="$real/Versions/A/$n"
  cp "$src" "$d/Versions/A/$n"
  cp "$src" "$d/Versions/A/$n.prebridge"
  for f in "$real"/Versions/A/*; do
    b=$(basename "$f")
    case "$b" in "$n"|"$n".*|_CodeSignature) continue;; esac
    ln -s "$ROOT/$f" "$d/Versions/A/$b"
    [ -e "$d/$b" ] || ln -s "Versions/Current/$b" "$d/$b"
  done
  ln -s A "$d/Versions/Current"
  ln -s "Versions/Current/$n" "$d/$n"
  echo "made $d ($(basename $src))"
}
want() { [ $# -eq 0 ] || [[ " $ALL " == *" $1 "* ]]; }
ALL="$*"
[ -z "$ALL" ] || true
for spec in \
  "HIToolbox $R/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework" \
  "QD $R/ApplicationServices.framework/Versions/A/Frameworks/QD.framework" \
  "CoreGraphics $R/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework" \
  "Security $R/Security.framework" \
  "AE $R/CoreServices.framework/Versions/A/Frameworks/AE.framework" \
  "AGL $R/AGL.framework" \
  "QuickTime $R/QuickTime.framework"; do
  set -- $spec
  if [ -z "$ALL" ] || [[ " $ALL " == *" $1 "* ]]; then mk $1 $2; fi
done
