#!/bin/bash
# periph.sh [test ...]: run the PowerPC peripheral tests under Rosettus and, for comparison, the same
# sources built natively (x86_64, clang) on this Mac. Output in /tmp/periph-out/. Each test prints
# "RESULT <check> PASS|FAIL ..." lines. audio_out makes sound (short tones).
# Run from a logged-in session. The camera and microphone tests (webcam, audio_in) run inside a small
# app bundle, /tmp/PeriphTest.app, whose Info.plist says why it uses them: macOS 10.14 asks for
# permission once for that app (answer OK), and ends a process that opens the camera without one.
# hid lists the HID devices and their elements and reads the keyboard ("hid led" also switches the caps
# lock LED on and off; "hid watch 10" prints the input values for 10 s); hidold does the same through
# the old IOHIDLib plug-in interface. services checks the keychain (a keychain of its own in /tmp),
# Bonjour, FSEvents, HTTP, System Configuration, preferences, power sources and Disk Arbitration.
# qt_movie decodes (muted, off screen) and qt_play plays (with sound, off screen, 8 s each) the movies in $PERIPH_MOVIES (default: /tmp/mov/*.mov, *.m4v, *.mp4).
cd "$(dirname "$0")" || exit 1
R="${ROSETTUS:-/Library/Rosettus/rosettus}"
O=/tmp/periph-out; mkdir -p "$O/native"
tests=("$@"); [ ${#tests[@]} -eq 0 ] && tests=(usb_list hid hidold serial threads midi services audio_out audio_in webcam qt_movie qt_play)
fw() { case "$1" in usb_list|serial) echo "-framework IOKit -framework CoreFoundation";;
  audio_out) echo "-framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreServices";;
  audio_in) echo "-framework CoreAudio -framework AudioToolbox -framework Carbon";;
  webcam|qt_movie|qt_play) echo "-framework QuickTime -framework Carbon";;
  threads) echo "-framework Carbon";;
  midi) echo "-framework CoreMIDI -framework CoreFoundation";;
  hid|hidold) echo "-framework IOKit -framework CoreFoundation";;
  services) echo "-framework Security -framework CoreServices -framework CoreFoundation -framework SystemConfiguration -framework IOKit -framework DiskArbitration";; esac; }
movies() { ls ${PERIPH_MOVIES:-/tmp/mov/*.mov /tmp/mov/*.m4v /tmp/mov/*.mp4} 2>/dev/null; }
args() { case "$1" in qt_movie|qt_play) movies;; esac; }
# the app bundle for the camera and microphone: runs the command line in /tmp/periphtest.cmd
app() {
  A=/tmp/PeriphTest.app
  if [ ! -x "$A/Contents/MacOS/run" ]; then
    mkdir -p "$A/Contents/MacOS"
    cat > "$A/Contents/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>run</string>
<key>CFBundleIdentifier</key><string>local.rosettus.periphtest</string>
<key>CFBundleName</key><string>PeriphTest</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>NSCameraUsageDescription</key><string>PeriphTest (Rosettus peripheral tests) uses the camera.</string>
<key>NSMicrophoneUsageDescription</key><string>PeriphTest (Rosettus peripheral tests) uses the microphone.</string>
</dict></plist>
P
    cat > "$A/Contents/MacOS/run" <<'S'
#!/bin/bash
# one command line from /tmp/periphtest.cmd; its output in /tmp/periphtest.out
C=$(cat /tmp/periphtest.cmd 2>/dev/null); rm -f /tmp/periphtest.cmd; [ -n "$C" ] || exit 0
cd /tmp; eval "$C" > /tmp/periphtest.out 2>/tmp/periphtest.err; echo "exit $?" >> /tmp/periphtest.err
S
    chmod +x "$A/Contents/MacOS/run"
  fi
  printf '%s' "$*" > /tmp/periphtest.cmd; rm -f /tmp/periphtest.out /tmp/periphtest.err
  open -W "$A" 2>/dev/null
  # open -W does not always manage to wait (when the app is quick): wait for the run script's last line
  for i in $(seq 180); do grep -q '^exit' /tmp/periphtest.err 2>/dev/null && break; sleep 0.5; done
}
for t in "${tests[@]}"; do
  inapp=; case "$t" in webcam|audio_in) inapp=1;; esac
  if clang -w -arch i386 -o "$O/native/$t" "$t.c" $(fw "$t") 2>/dev/null || clang -w -o "$O/native/$t" "$t.c" $(fw "$t") 2>/dev/null; then
    if [ -n "$inapp" ]; then app "perl -e 'alarm 60; exec @ARGV' '$O/native/$t'"; cp /tmp/periphtest.out "$O/$t.native.txt" 2>/dev/null
    else perl -e 'alarm 60; exec @ARGV' "$O/native/$t" $(args "$t") > "$O/$t.native.txt" 2>&1; fi
  else echo "(no native build)" > "$O/$t.native.txt"; fi
  if [ -n "$inapp" ]; then
    app "OAH_CAMERA_ASK=1 OAH_GTB=1 OAH_QUIET_EXIT=1 OAH_NO_REPORTS=1 perl -e 'alarm 60; exec @ARGV' '$R' '$PWD/$t'"
    grep -v '^\[' /tmp/periphtest.out > "$O/$t.ppc.txt" 2>/dev/null; cp /tmp/periphtest.err "$O/$t.err" 2>/dev/null
  else
    OAH_GTB=1 OAH_QUIET_EXIT=1 OAH_NO_REPORTS=1 perl -e 'alarm 60; exec @ARGV' "$R" "$PWD/$t" $(args "$t") 2>"$O/$t.err" | grep -v '^\[' > "$O/$t.ppc.txt"
  fi
  echo "== $t"
  paste -d'|' <(grep '^RESULT' "$O/$t.ppc.txt" | sed 's/^RESULT //') <(grep '^RESULT' "$O/$t.native.txt" | sed 's/^RESULT /native: /') | sed 's/|/   /'
done
