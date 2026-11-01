#!/bin/bash
# Build rosettus-kc, the keychain data helper of liboah (oah_kc.h), on macOS 10.14 + Command Line Tools.
# usage: src/kchelper/build.sh [OUT]   (default: bin/rosettus-kc); installed as native/rosettus-kc
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../bin/rosettus-kc}"
clang -O2 -arch x86_64 -mmacosx-version-min=10.9 -Wno-deprecated-declarations -o "$OUT" "$HERE/rosettus-kc.c" -framework Security -framework CoreFoundation
echo "built $OUT"
# then record the sources it was built from: tools/binstamp.sh --update rosettus-kc
