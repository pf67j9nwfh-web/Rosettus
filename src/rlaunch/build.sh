#!/bin/bash
# Build rlaunch, the x86_64 stub used by patched apps and launcher apps (macOS 10.14 + Command Line Tools).
# usage: src/rlaunch/build.sh [OUT]   (default: bin/rlaunch)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../bin/rlaunch}"
clang -O2 -arch x86_64 -mmacosx-version-min=10.9 -o "$OUT" "$HERE/rlaunch.c" -framework Carbon -framework CoreFoundation
echo "built $OUT"
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update rlaunch
