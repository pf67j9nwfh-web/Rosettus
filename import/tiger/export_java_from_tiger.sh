#!/bin/bash
# export_java_from_tiger.sh - optional: run on a PowerPC Mac with Mac OS X 10.4 Tiger (or 10.5
# Leopard) to collect its PowerPC Java runtimes for Rosettus.
#
# Snow Leopard has no PowerPC Java (its Java is Intel-only), so the main Snow Leopard export
# carries none. This copies every Java version the PowerPC Mac has - 1.3.1, 1.4.2, 5.0 (1.5.0) -
# with the Java support files around them into one archive. Copy it to your Mojave Mac and run
#     /Library/Rosettus/tools/install_java.sh Rosettus-Java-PPC.tar.gz
# Nothing on this Mac is changed. Apple's files are never part of the Rosettus repository: they
# come from your own PowerPC Mac.
#
# usage: ./export_java_from_tiger.sh [OUTPUT.tar.gz]   (default: ~/Desktop/Rosettus-Java-PPC.tar.gz)
OUT="${1:-$HOME/Desktop/Rosettus-Java-PPC.tar.gz}"
case "$OUT" in /*) ;; *) OUT="$(pwd)/$OUT";; esac
WORK="${TMPDIR:-/tmp}/rosettus-java-export.$$"
JV=/System/Library/Frameworks/JavaVM.framework/Versions

ver=$(sw_vers -productVersion 2>/dev/null)
arch=$(uname -p)
case "$ver" in
  10.4|10.4.*|10.5|10.5.*) ;;
  *) echo "This script is for Mac OS X 10.4 Tiger or 10.5 Leopard on a PowerPC Mac (this is ${ver:-unknown})."; exit 1;;
esac
if [ "$arch" != "powerpc" ]; then
  echo "This Mac is not a PowerPC Mac ($arch): its Java has no PowerPC code. Run this on a PowerPC Mac."
  exit 1
fi
if [ ! -d "$JV" ]; then echo "No Java found ($JV is missing)."; exit 1; fi

# the Java versions present (real folders; the 1.4 / 1.5 links are kept, A/Current/CurrentJDK separately)
VERSIONS=""
for d in "$JV"/*; do
  n=$(basename "$d")
  [ -L "$d" ] && continue
  case "$n" in A|Current|CurrentJDK) continue;; esac
  [ -d "$d" ] && VERSIONS="$VERSIONS $n"
done
if [ -z "$VERSIONS" ]; then echo "No Java versions found in $JV."; exit 1; fi
echo "Java versions on this Mac:$VERSIONS"

need=$(du -sk "$JV" /System/Library/Java 2>/dev/null | awk '{s += $1} END {print s}')
avail=$(df -k "$(dirname "$OUT")" | awk 'NR==2 {print $4}')
if [ "${avail:-0}" -lt $(( ${need:-0} * 3 / 2 + 50000 )) ]; then   # an uncompressed copy while packing
  echo "Not enough free space (need about $(( need * 3 / 2048 + 50 )) MB)."; exit 1
fi

mkdir -p "$WORK" || exit 1
trap 'rm -rf "$WORK"' EXIT
LIST="$WORK/.files.txt"
: > "$LIST"
add() {   # add a path relative to / if it exists (symbolic links are kept as links)
  if [ -e "/$1" ] || [ -L "/$1" ]; then echo "$1" >> "$LIST"; else echo "  (not present: /$1)"; fi
}
for v in $VERSIONS; do add "${JV#/}/$v"; done
for l in "$JV"/*; do   # version links (1.4 -> 1.4.2, 1.5 -> 1.5.0)
  n=$(basename "$l")
  [ -L "$l" ] || continue
  case "$n" in Current|CurrentJDK) continue;; esac
  add "${JV#/}/$n"
done
# shared Java support: the framework's version-independent part, the Cocoa-Java bridge and system
# extensions, the Java application launcher (JavaApplicationStub apps), the embedding framework,
# the Java utilities (Java Preferences, Web Start, plug-in settings) and installed extensions
add "${JV#/}/A"
add System/Library/Java
add System/Library/Frameworks/JavaEmbedding.framework
for f in /System/Library/PrivateFrameworks/Java*.framework; do
  case "$f" in *JavaScript*) continue;; esac
  [ -d "$f" ] && add "${f#/}"
done
add Applications/Utilities/Java
add Library/Java/Extensions

{
  echo "Rosettus Java export"
  echo "date: $(date)"
  echo "system: $(sw_vers -productName) $ver ($(sw_vers -buildVersion)) $arch"
  echo "versions:$VERSIONS"
  echo "default: $(readlink "$JV/CurrentJDK" 2>/dev/null)"
  for v in $VERSIONS; do
    j="$JV/$v/Commands/java"
    [ -x "$j" ] && echo "java $v: $("$j" -version 2>&1 | grep -i version | head -1)"
  done
} > "$WORK/.manifest.txt"
cat "$WORK/.manifest.txt"

echo "Writing $OUT ..."
# (Tiger's tar 1.14 applies a later -C before reading the -T list: write the list first, then append)
( cd / && tar -cf "$WORK/java.tar" -T "$LIST" ) 2>"$WORK/tar.err"
st=$?
tar -rf "$WORK/java.tar" -C "$WORK" .manifest.txt .files.txt 2>>"$WORK/tar.err"
if [ -s "$WORK/tar.err" ]; then
  echo "tar reported (unreadable files are skipped):"; sed 's/^/  /' "$WORK/tar.err" | head -20
fi
if [ ! -s "$WORK/java.tar" ]; then echo "Writing the archive failed."; exit 1; fi
gzip -c "$WORK/java.tar" > "$OUT" || { echo "Writing the archive failed."; rm -f "$OUT"; exit 1; }
echo "Done: $OUT ($(du -h "$OUT" | awk '{print $1}'))"
echo "MD5: $(md5 -q "$OUT" 2>/dev/null)"
echo "Copy it to your Mojave Mac and run:  /Library/Rosettus/tools/install_java.sh \"$(basename "$OUT")\""
