#!/bin/bash
# tests/blender/bge_run.sh [GLSL|MULTITEXTURE] : the Blender Game Engine (blenderplayer 2.63) under Rosettus.
# Blender builds a small physics scene (bge_play.py make), blenderplayer runs it in a 640x480
# window for 240 logic tics, then Blender checks the log and the game's own screenshot
# (bge_play.py check). Output in /tmp/bge-test-MODE/. Run from a logged-in session.
cd "$(dirname "$0")" || exit 1
R="${ROSETTUS:-/Library/Rosettus/rosettus}"
A="${APPS:-${ROSETTUS_TEST_APPS:-/Applications}}"
M="${1:-GLSL}"
O=/tmp/bge-test-$M; rm -rf "$O"; mkdir -p "$O"
B="$A/blender.app/Contents/MacOS/blender"
perl -e 'alarm 200; exec @ARGV' "$R" "$B" -b --factory-startup -noaudio -P "$PWD/bge_play.py" -- make "$O" "$M" > "$O/make.log" 2>&1
perl -e 'alarm 200; exec @ARGV' "$R" "$A/blenderplayer.app/Contents/MacOS/blenderplayer" -w 640 480 "$O/game.blend" > "$O/play.log" 2>&1
perl -e 'alarm 200; exec @ARGV' "$R" "$B" -b --factory-startup -noaudio -P "$PWD/bge_play.py" -- check "$O" > "$O/check.log" 2>&1
grep -a '^RESULT' "$O/check.log"
n=$(grep -a '^RESULT' "$O/check.log" | grep -c ' FAIL')
echo "bge $M: $(grep -a -c '^RESULT' "$O/check.log") results, $n failed"
