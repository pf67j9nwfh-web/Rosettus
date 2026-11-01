#!/bin/bash
# Build the PowerPC helpers on Snow Leopard (Xcode 3.2.x with the PowerPC compilers):
#   import/sl/Install Rosetta.app   PowerPC-only app that makes Snow Leopard offer to install Rosetta
#   import/sl/dump_text_ppc         saves translate's decoded __TEXT for export_from_sl.py
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SL="$HERE/../../import/sl"
APP="$SL/Install Rosetta.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
gcc -arch ppc -O2 -mmacosx-version-min=10.4 -framework Cocoa -o "$APP/Contents/MacOS/Install Rosetta" "$HERE/main.m"
cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>Install Rosetta</string>
	<key>CFBundleIdentifier</key><string>io.github.rosettus.install-rosetta</string>
	<key>CFBundleName</key><string>Install Rosetta</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>LSMinimumSystemVersion</key><string>10.4</string>
</dict>
</plist>
EOF
printf 'APPL????' > "$APP/Contents/PkgInfo"
[ -f "$HERE/AppIcon.icns" ] && cp "$HERE/AppIcon.icns" "$APP/Contents/Resources/"
gcc -arch ppc -O2 -o "$SL/dump_text_ppc" "$SL/dump_text_ppc.c"
lipo -info "$APP/Contents/MacOS/Install Rosetta" "$SL/dump_text_ppc"
