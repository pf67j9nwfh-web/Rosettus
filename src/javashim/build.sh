#!/bin/bash
# Build bin/libjavashim.dylib (PowerPC; Snow Leopard with Xcode 3.2). See javashim.c.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../bin/libjavashim.dylib"
gcc -arch ppc -O2 -dynamiclib -o "$OUT" "$HERE/javashim.c" \
    -install_name /System/Library/Frameworks/JavaVM.framework/Versions/A/Libraries/libjavashim.dylib \
    -compatibility_version 1.0 -current_version 2.0 -framework CoreFoundation
lipo -info "$OUT"
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update libjavashim.dylib
