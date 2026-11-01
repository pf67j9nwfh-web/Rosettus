#!/bin/bash
# Build "Rosettus Launcher.app" (on macOS 10.14 with the Xcode command line tools).
# usage: src/launcher/build.sh [OUTDIR]     (default: bin/)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../bin}"
APP="$OUT/Rosettus Launcher.app"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clang -fobjc-arc -O2 -arch x86_64 -mmacosx-version-min=10.13 -framework Cocoa -Wall \
      -o "$APP/Contents/MacOS/Rosettus Launcher" "$HERE/main.m"
clang -fobjc-arc -O2 -framework Cocoa -o "$TMP/mkicon" "$HERE/mkicon.m"
"$TMP/mkicon" "$TMP/AppIcon.iconset"
iconutil -c icns -o "$APP/Contents/Resources/AppIcon.icns" "$TMP/AppIcon.iconset"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
codesign -f -s - "$APP" 2>/dev/null || true
echo "built $APP"
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update "Rosettus Launcher.app"
