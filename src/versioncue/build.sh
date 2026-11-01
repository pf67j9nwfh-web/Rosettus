#!/bin/bash
# Build on Snow Leopard with Xcode 3.2 and Java 6; target Java 5 and PowerPC.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../bin/versioncue"
TEMP="$(mktemp -d /tmp/vcbuild.XXXXXX)"
trap 'rm -rf "$TEMP"' EXIT
mkdir -p "$OUT" "$TEMP/classes"
gcc -std=gnu99 -arch ppc -O2 -Wall -framework Foundation "$HERE/controller.m" -o "$OUT/controller"
gcc -arch ppc -O2 -Wall -dynamiclib -I/System/Library/Frameworks/JavaVM.framework/Headers \
 "$HERE/vc-jvm.c" -o "$OUT/libvcjvm.dylib" -install_name /Library/Rosettus/versioncue/libvcjvm.dylib \
 -compatibility_version 1.0.0 -current_version 1.0.0
javac -source 1.5 -target 1.5 -cp "$HERE/vendor/asm-all-3.3.1.jar" -d "$TEMP/classes" "$HERE/ExplicitNullChecks.java"
(cd "$TEMP/classes" && jar xf "$HERE/vendor/asm-all-3.3.1.jar")
mkdir -p "$TEMP/classes/META-INF"
cp "$HERE/vendor/ASM-LICENSE.txt" "$TEMP/classes/META-INF/ASM-LICENSE.txt"
printf 'Premain-Class: ExplicitNullChecks\n\n' > "$TEMP/manifest"
jar cfm "$OUT/nullchecks.jar" "$TEMP/manifest" -C "$TEMP/classes" .
cp "$HERE/vendor/ASM-LICENSE.txt" "$OUT/ASM-LICENSE.txt"
echo "Built PowerPC Version Cue support and Java 5 null-check agent."
