#!/bin/bash
# Build liboah_mojave.dylib - on Mac OS X 10.6 with Xcode 3.2.x (gcc 4.2, 10.6 SDK).
# Mojave's toolchain cannot link 32-bit Intel code any more, so the shim is built on Snow Leopard
# and copied to bin/ (and to the install root: ~/Library/Rosettus/liboah_mojave.dylib).
# usage: src/liboah/build.sh [OUT]   (default: bin/liboah_mojave.dylib)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../bin/liboah_mojave.dylib}"
# bridges_gen.h comes from bridges/bridges.spec (python bridges/genbridges.py bridges/bridges.spec)
gcc -std=gnu99 -O2 -arch i386 -dynamiclib \
    -framework IOKit -framework CoreFoundation -framework AudioUnit -framework CoreAudio -lobjc -framework Security -framework SystemConfiguration -framework CoreServices \
    -o "$OUT" "$HERE/liboah_mojave.c"
echo "built $OUT"
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update liboah_mojave.dylib
