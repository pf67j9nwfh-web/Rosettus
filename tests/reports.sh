#!/bin/bash
# reports.sh [N]: the newest N (default 20) crash/hang/dump reports from ~/Library/Logs/Rosettus/Reports/index.txt
I=~/Library/Logs/Rosettus/Reports/index.txt
[ -f "$I" ] || { echo "no reports"; exit 0; }
tail -${1:-20} "$I" | awk -F'\t' '{ printf "%s  %-22s %-8s %s\n      %s\n", $1, $2, $3, $4, substr($5,1,150) }'
