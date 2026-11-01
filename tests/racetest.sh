#!/bin/bash
# racetest.sh N app...: open each wrapper N times; report whether a translated process came up
# and stayed up for 12 s, and any startup fatal in its log. Kills only processes it started.
N=$1; shift
L=~/Library/Logs/Rosettus
tpids() { pgrep -f "(translate_mojave|MacOS/translate) " | sort; }
for a in "$@"; do
  ok=0; bad=0
  for i in $(seq 1 $N); do
    before=$(tpids)
    open "${DIR:-$HOME/Applications/Rosettus}/$a.app"; sleep 12
    new=$(comm -13 <(echo "$before") <(tpids) | tr '\n' ' ')
    f=$(grep -a -c -E "lazy pointer not found|translate fatal|^CRASH" "$L/$a.log" 2>/dev/null)
    if [ -n "$new" ] && [ "$f" = 0 ]; then ok=$((ok+1)); else bad=$((bad+1)); echo "  $a run $i: pids=[$new] fatal=$f"; fi
    for p in $new; do kill -9 $p 2>/dev/null; done
    sleep 2
  done
  echo "$a: ok=$ok bad=$bad"
done
