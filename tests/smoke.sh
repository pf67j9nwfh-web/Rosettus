#!/bin/bash
# smoke.sh [apps...]: open each wrapper, wait, report alive/crash from its own Rosetta log, then quit
# ONLY the processes this run started (an earlier version matched by name and killed the user's apps).
# A wrapper whose guest app is already running is skipped. Default: every wrapper.
L=~/Library/Logs/Rosettus; out=/tmp/smoke.out; : > $out
apps=("$@")
if [ ${#apps[@]} -eq 0 ]; then for w in ~/Applications/Rosettus/*.app; do apps+=("$(basename "$w" .app)"); done; fi
# translated processes, without the shared agents (fontd, its font workers, coreservicesd, pboard, Version Cue):
# they start on demand during an app's launch, and killing them made later apps wait on fonts
tpids() { pgrep -f "(translate_mojave|MacOS/translate) " | while read p; do
    ps -o args= -p $p | grep -qE "ATS.framework|coreservicesd|/usr/sbin/pboard|versioncue|VersionCue|mysqld" || echo $p; done | sort; }
for a in "${apps[@]}"; do
  w=~/Applications/Rosettus/$a.app
  g=$(/usr/libexec/PlistBuddy -c "Print :RosettaGuestApp" "$w/Contents/Info.plist" 2>/dev/null)
  if [ -n "$g" ] && pgrep -f "translate.* .*$(basename "$g" | sed 's/[][\.*^$]/\\&/g')" > /dev/null; then echo "$a: skipped (already running)" >> $out; continue; fi
  before=$(tpids)
  open "$w"; sleep ${WAIT:-40}
  new=$(comm -13 <(echo "$before") <(tpids) | tr '\n' ' ')
  log="$L/$a.log"
  c=$(grep -c "^CRASH\|^\[exit\]\|exit=" "$log" 2>/dev/null)
  wins=0
  for p in $new; do wins=$((wins + $(/usr/bin/python -c "
import Quartz as Q, sys
print len([w for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, 0) if w.get('kCGWindowOwnerPID') == int(sys.argv[1]) and w.get('kCGWindowBounds', {}).get('Width', 0) > 40])" $p))); done
  echo "$a: pids=${new:-none} windows=$wins crashes=$c" >> $out
  screencapture -x "/tmp/smoke_$(echo "$a" | tr -cd 'A-Za-z0-9').png"
  for p in $new; do kill -9 $p 2>/dev/null; done
  sleep 2
done
echo DONE >> $out
