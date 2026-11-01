#!/bin/bash
# behave_clean.sh [apps...]: like tests/behave_all.sh, but before each app it kills translated test
# processes left by earlier apps (only those started from launchers or the dev tree; never Version
# Cue, preference pane hosts, pboard, ATS or a SimCity the user runs).
W=${BEHAVE_WRAPDIR:-$HOME/Applications/Rosettus}
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 1
apps=("$@")
if [ ${#apps[@]} -eq 0 ]; then for w in "$W"/*.app; do apps+=("$(basename "$w" .app)"); done; fi
start=$(date +%Y%m%d-%H%M%S)
cleanup() {
  ps axo pid=,command= | grep -E "translate" | grep -v -E "versioncue|Preference Pane|/usr/sbin/pboard|ATS.framework|SimCity|grep" |
    grep -E "/Applications/Rosettus/|LaunchCFMApp" | awk '{print $1}' | xargs kill 2>/dev/null
  sleep 2
}
for a in "${apps[@]}"; do
  case "$a" in "SimCity 4 Rush Hour") echo "skip $a"; continue;; esac
  cleanup
  /usr/bin/python "$HERE/behave.py" "$a"
  sleep 3
done
cleanup
awk -F'\t' -v s="$start" '$1 >= s' ~/Library/Logs/Rosettus/Behave/summary.tsv
