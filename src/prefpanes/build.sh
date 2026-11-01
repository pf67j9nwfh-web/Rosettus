#!/bin/bash
# Build host on Snow Leopard; browser on Mojave. Run on each machine.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../bin"
case "${1:-}" in
  host)
    H="$OUT/prefpanes/Rosettus Preference Pane Host.app"
    mkdir -p "$H/Contents/MacOS"
    gcc -arch ppc -O2 -Wall -framework Cocoa -framework PreferencePanes "$HERE/main.m" -o "$H/Contents/MacOS/Rosettus Preference Pane Host"
    cat > "$H/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Rosettus Preference Pane Host</string>
<key>CFBundleIdentifier</key><string>io.github.rosettus.prefpanehost</string>
<key>CFBundleName</key><string>Rosettus Preference Pane Viewer</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
</dict></plist>
PLIST
    ;;
  viewer)
    H="$OUT/Rosettus Preference Pane Viewer.app"
    mkdir -p "$H/Contents/MacOS"
    clang -arch x86_64 -mmacosx-version-min=10.14 -O2 -Wall -framework Cocoa "$HERE/viewer.m" -o "$H/Contents/MacOS/Rosettus Preference Pane Viewer"
    cat > "$H/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Rosettus Preference Pane Viewer</string>
<key>CFBundleIdentifier</key><string>io.github.rosettus.prefpaneviewer</string>
<key>CFBundleName</key><string>Rosettus Preference Pane Viewer</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSMinimumSystemVersion</key><string>10.14</string>
<key>CFBundleDocumentTypes</key><array><dict>
<key>CFBundleTypeExtensions</key><array><string>prefPane</string></array>
<key>CFBundleTypeName</key><string>PowerPC Preference Pane</string>
<key>CFBundleTypeRole</key><string>Viewer</string>
<key>LSHandlerRank</key><string>Alternate</string>
<key>LSTypeIsPackage</key><true/>
</dict></array>
</dict></plist>
PLIST
    codesign -f -s - "$H"
    ;;
  *) echo "usage: $0 host|viewer" >&2; exit 2;;
esac
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update "Rosettus Preference Pane Viewer.app" prefpanes
