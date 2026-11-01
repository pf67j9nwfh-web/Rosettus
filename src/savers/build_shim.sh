#!/bin/bash
# Build bin/savers/RosettusSaverShim.saver (macOS 10.14 + Command Line Tools).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../bin/savers"
S="$OUT/RosettusSaverShim.saver"
mkdir -p "$OUT"; rm -rf "$S"; mkdir -p "$S/Contents/MacOS" "$S/Contents/Resources"
clang -fobjc-arc -O2 -arch x86_64 -mmacosx-version-min=10.13 -bundle -framework Cocoa -framework ScreenSaver \
      -o "$S/Contents/MacOS/RosettusSaverShim" "$HERE/shim/RosettusSaverShim.m"
cat > "$S/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleExecutable</key><string>RosettusSaverShim</string>
	<key>CFBundleIdentifier</key><string>io.github.rosettus.savershim.000000000000</string>
	<key>CFBundleName</key><string>Rosettus Saver</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>NSPrincipalClass</key><string>RosettusSaverShim_000000000000</string>
</dict></plist>
EOF
nm "$S/Contents/MacOS/RosettusSaverShim" | grep -c RosettusSaverShim_000000000000 | sed 's/^/placeholder symbols: /'
echo "built $S"
