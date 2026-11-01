#!/bin/bash
# Build bin/libppcexec.dylib (macOS 10.14 + Command Line Tools; x86_64 and i386: a 32-bit tool started from such a script inherits DYLD_INSERT_LIBRARIES and dyld aborted it when the library had no i386 slice): loaded by ROOT/native/bash so that
# shell scripts started by PowerPC programs can run PowerPC tools (see ppcexec.c).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../bin/libppcexec.dylib}"
clang -O2 -arch x86_64 -mmacosx-version-min=10.9 -dynamiclib -install_name libppcexec.dylib -o "$OUT.x86_64" "$HERE/ppcexec.c"
# the i386 slice: Mojave's SDK has no i386 libSystem, so it is built on Snow Leopard and kept here:
#   gcc -O2 -arch i386 -mmacosx-version-min=10.5 -dynamiclib -o ppcexec_i386.dylib ppcexec.c
lipo -create "$OUT.x86_64" "$HERE/ppcexec_i386.dylib" -output "$OUT"; rm -f "$OUT.x86_64"
echo "built $OUT"
# then record the sources it was built from (on Mojave): tools/binstamp.sh --update libppcexec.dylib
