#!/bin/bash
# Build the PowerPC parts of screen saver support (Snow Leopard, Xcode 3.2 with PowerPC compilers):
#   bin/savers/Rosettus Saver Host.app   runs a PowerPC .saver under Rosetta (see host/main.m)
#   bin/savers/PPCTestSaver.saver        a small PowerPC saver for testing
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../bin/savers"
mkdir -p "$OUT"
H="$OUT/Rosettus Saver Host.app"
rm -rf "$H"; mkdir -p "$H/Contents/MacOS"
gcc -arch ppc -O2 -framework Cocoa -framework ScreenSaver -o "$H/Contents/MacOS/Rosettus Saver Host" "$HERE/host/main.m"
cat > "$H/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleExecutable</key><string>Rosettus Saver Host</string>
	<key>CFBundleIdentifier</key><string>io.github.rosettus.saverhost</string>
	<key>CFBundleName</key><string>Rosettus Saver Host</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>LSUIElement</key><true/>
</dict></plist>
EOF
T="$OUT/PPCTestSaver.saver"
rm -rf "$T"; mkdir -p "$T/Contents/MacOS"
gcc -arch ppc -O2 -bundle -framework Cocoa -framework ScreenSaver -o "$T/Contents/MacOS/PPCTestSaver" "$HERE/test/PPCTestSaver.m"
cat > "$T/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleExecutable</key><string>PPCTestSaver</string>
	<key>CFBundleIdentifier</key><string>io.github.rosettus.ppctestsaver</string>
	<key>CFBundleName</key><string>PPC Test Saver</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>NSPrincipalClass</key><string>PPCTestSaver</string>
</dict></plist>
EOF
lipo -info "$H/Contents/MacOS/Rosettus Saver Host" "$T/Contents/MacOS/PPCTestSaver"
