#!/bin/bash
# tests/blender/run.sh [BLENDER_APP] : Blender used for what it is for, under Rosettus.
# 1. blender_func.py in the background (no window): modelling, modifiers, edit-mode operators,
#    animation, particles, a ray-traced render checked pixel by pixel, save and reopen.
# 2. blender_gui.py with the window open on the saved scene: the OpenGL 3-D viewport, checked
#    from a screenshot Blender takes of itself.
# No sound (-noaudio). Output and renders in /tmp/blender-test/. Run from a logged-in session.
cd "$(dirname "$0")" || exit 1
R="${ROSETTUS:-/Library/Rosettus/rosettus}"
APP="${1:-${ROSETTUS_TEST_APPS:-/Applications}/blender.app}"
B="$APP/Contents/MacOS/blender"
O=/tmp/blender-test; rm -rf "$O"; mkdir -p "$O"
perl -e 'alarm 300; exec @ARGV' "$R" "$B" -b --factory-startup -noaudio -P "$PWD/blender_func.py" -- "$O" > "$O/func.log" 2>&1
perl -e 'alarm 300; exec @ARGV' "$R" "$B" --factory-startup -noaudio "$O/scene.blend" -P "$PWD/blender_gui.py" -- "$O" > "$O/gui.log" 2>&1
# the same script under Mac OS X 10.6's Rosetta (2-core Mac) printed these; its render differed from
# Rosettus' in 2 of 307200 channel values, by 1/255
REF="0.90966 2.537429 1.27778 0.08431 0.5915"
V=$(grep -a '^VALUES' "$O/func.log" | sed 's/^VALUES //')
[ "$V" = "$REF" ] && echo "RESULT values_match_10.6 PASS $V" >> "$O/func.log" || echo "RESULT values_match_10.6 FAIL $V (10.6: $REF)" >> "$O/func.log"
grep -a -h '^RESULT\|^VALUES' "$O/func.log" "$O/gui.log"
n=$(grep -a -h '^RESULT' "$O/func.log" "$O/gui.log" | grep -c ' FAIL')
echo "blender: $(grep -a -h -c '^RESULT' "$O/func.log" "$O/gui.log" | paste -sd+ - | bc) results, $n failed"
