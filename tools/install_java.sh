#!/bin/bash
# install_java.sh ARCHIVE - add PowerPC Java runtimes to Rosettus.
#
# ARCHIVE is made from Apple's installers by import/java/import_java_from_media.sh (Tiger DVD and Apple's
# Java updates), or on a PowerPC Mac by import/tiger/export_java_from_tiger.sh. The Java versions in
# it (1.3.1, 1.4.2, 5.0) go into the PowerPC system tree of this Rosettus install, next to the Snow
# Leopard files; Mojave's own /System and /Library/Java are not touched. Afterwards:
#     /Library/Rosettus/rjava -version            Java 5 (the default), or -j 1.4 / -j 1.3 for the others
#     /Library/Rosettus/rjava javac Hello.java    the other Java tools work the same way
# Running it again with a newer archive replaces the versions it contains.
set -e
ARCH="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
JV="$ROOT/System/Library/Frameworks/JavaVM.framework/Versions"
if [ -z "$ARCH" ] || [ ! -f "$ARCH" ]; then
  echo "usage: $0 Rosettus-Java-PPC.tar.gz   (made by import/java/import_java_from_media.sh, or by"
  echo "       import/tiger/export_java_from_tiger.sh on a PowerPC Mac)"; exit 1
fi
case "$ARCH" in /*) ;; *) ARCH="$(pwd)/$ARCH";; esac
if [ ! -d "$ROOT/System/Library/Frameworks" ] || [ ! -x "$ROOT/rosettus" ]; then
  echo "Rosettus is not installed in $ROOT (install it first)."; exit 1
fi
if [ "$(id -u)" != 0 ] && { [ ! -w "$ROOT/System/Library/Frameworks" ] || [ "$(stat -f %u "$ROOT")" = 0 ]; }; then
  echo "$ROOT belongs to the system: asking for administrator rights."
  exec sudo "$0" "$ARCH"
fi
if ! tar -tzf "$ARCH" ./.manifest.txt >/dev/null 2>&1 && ! tar -tzf "$ARCH" .manifest.txt >/dev/null 2>&1; then
  echo "$ARCH is not a Rosettus Java export (no .manifest.txt in it)."; exit 1
fi
T="${TMPDIR:-/tmp}/rosettus-java-install.$$"
mkdir -p "$T"; trap 'rm -rf "$T"' EXIT
echo "Unpacking $(basename "$ARCH") ..."
tar -xzpf "$ARCH" -C "$T"
sed 's/^/  /' "$T/.manifest.txt"
SV="$T/System/Library/Frameworks/JavaVM.framework/Versions"
[ -d "$SV" ] || { echo "No Java versions in the archive."; exit 1; }
mkdir -p "$JV"

# the Java versions (folders and their 1.x links); "A" stays Snow Leopard's (it starts all of them)
n=0
for d in "$SV"/*; do
  v=$(basename "$d")
  case "$v" in A|Current|CurrentJDK) continue;; esac
  if [ -L "$d" ]; then
    case "$(readlink "$d")" in /*) continue;; esac   # e.g. Leopard's 1.6.0 -> an Intel-only JDK
    ln -sfh "$(readlink "$d")" "$JV/$v"; continue
  fi
  # a placeholder (Leopard's updates carry an empty 1.3.1 / 1.4.2) never replaces a working version
  if [ ! -x "$d/Commands/java" ] && [ -x "$JV/$v/Commands/java" ]; then echo "kept Java $v (the archive has no runtime for it)"; continue; fi
  rm -rf "$JV/$v.new"; ditto "$d" "$JV/$v.new"
  [ "$(id -u)" = 0 ] && chown -R root:admin "$JV/$v.new"
  rm -rf "$JV/$v"; mv "$JV/$v.new" "$JV/$v"
  echo "installed Java $v"; n=$((n + 1))
done
[ $n -gt 0 ] || { echo "No Java versions in the archive."; exit 1; }
# JavaVM's own sub-frameworks that Java 5 from the Leopard updates links (A/Frameworks: JavaNativeFoundation,
# JavaRuntimeSupport). Snow Leopard's are Intel-only; one without PowerPC code is replaced.
for f in "$SV"/A/Frameworks/*.framework; do
  [ -d "$f" ] || continue
  b=$(basename "$f" .framework); dst="$JV/A/Frameworks/$b.framework"
  if [ -f "$dst/Versions/A/$b" ] && lipo -info "$dst/Versions/A/$b" 2>/dev/null | grep -q ppc; then echo "kept A/Frameworks/$b.framework"; continue; fi
  mkdir -p "$JV/A/Frameworks"; rm -rf "$dst.new"; ditto "$f" "$dst.new"
  [ "$(id -u)" = 0 ] && chown -R root:admin "$dst.new"
  rm -rf "$dst"; mv "$dst.new" "$dst"; echo "added A/Frameworks/$b.framework"
done
# default version for programs that ask for "the current JDK": the newest one
for v in 1.5.0 1.4.2 1.3.1; do [ -d "$JV/$v" ] && { ln -sfh "$v" "$JV/CurrentJDK"; echo "default: $v"; break; }; done

# support files Snow Leopard does not have: the Cocoa-Java bridge and system extensions, launcher
# frameworks, the Java utilities. Files the Snow Leopard tree already has are kept.
copy_missing() {   # copy_missing relative/path
  [ -e "$T/$1" ] || return 0
  if [ -e "$ROOT/$1" ]; then echo "kept $1 (already there)"; return 0; fi
  mkdir -p "$(dirname "$ROOT/$1")"; ditto "$T/$1" "$ROOT/$1"; echo "added $1"
  [ "$(id -u)" = 0 ] && chown -R root:admin "$ROOT/$1"
  return 0
}
copy_missing System/Library/Java
for f in "$T"/System/Library/PrivateFrameworks/Java*.framework "$T"/System/Library/Frameworks/JavaEmbedding.framework; do
  [ -e "$f" ] && copy_missing "${f#$T/}"
done
copy_missing Applications/Utilities/Java
if [ -d "$T/Library/Java/Extensions" ]; then
  echo "note: the PowerPC Mac's /Library/Java/Extensions are not installed (PowerPC programs see this Mac's"
  echo "      /Library/Java/Extensions); copy jars from them by hand if a program needs them."
fi
# functions Snow Leopard renamed (Java 5's AWT): Rosettus' libjavashim provides them, and the Java
# libraries that need them are pointed at it (tools/javafix.py)
SHIM=/System/Library/Frameworks/JavaVM.framework/Versions/A/Libraries/libjavashim.dylib
if [ -f "$ROOT/native/libjavashim.dylib" ] && [ -f "$ROOT/tools/javafix.py" ]; then
  mkdir -p "$JV/A/Libraries"
  install -m 755 "$ROOT/native/libjavashim.dylib" "$ROOT$SHIM"
  /usr/bin/python "$ROOT/tools/javafix.py" "$JV" "$SHIM"
fi
chmod -R go-w "$JV" 2>/dev/null || true
cp "$T/.manifest.txt" "$ROOT/java-manifest.txt"
echo "Done. Try:  $ROOT/rjava -version"
