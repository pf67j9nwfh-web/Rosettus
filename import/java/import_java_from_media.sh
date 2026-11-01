#!/bin/bash
# import_java_from_media.sh - make the PowerPC Java archive for Rosettus from Apple's own installers,
# without a PowerPC Mac. Run it on the Mojave Mac; the result is the same kind of archive that
# import/tiger/export_java_from_tiger.sh makes on a PowerPC Mac, for
#     /Library/Rosettus/tools/install_java.sh Rosettus-Java-PPC.tar.gz
#
# usage: ./import_java_from_media.sh [-o OUT.tar.gz] [-w WORKDIR] [--thin] SOURCE...
#
# A SOURCE is any of
#   - a Mac OS X 10.4 Tiger or 10.5 Leopard install DVD: the .iso/.dmg/.cdr image or the mounted disc
#     (Java.pkg and the Cocoa-Java files from Essentials.pkg are used)
#   - an Apple Java update: the .pkg, the .dmg it came in, or the .tar that Software Update downloads
#     (Java for Mac OS X 10.4 Release 9, Java for Mac OS X 10.5 Update 10, ...)
#   - a folder holding any of these
# Sources are read in the order given; a later source replaces the Java versions it contains (1.3.1,
# 1.4.2, 1.5.0), so put install discs first and updates after them, older updates before newer ones.
# Recommended - no disc needed, all from Apple's Software Update servers (tools/fetch_java_update.sh):
#   Java131and142Release2.tar  JavaForMacOSX10.4Release9.tar   ->  1.3.1_16, 1.4.2_21, 1.5.0_19
# Java 5 builds for Leopard (Java for Mac OS X 10.5 Update 10: 1.5.0_30; Leopard DVD: 1.5.0_13) start and
# run small programs, but crash under load in their garbage collector under Rosettus: experimental.
# A Leopard DVD gives 1.4.2_16 (works) and 1.5.0_13; a Tiger DVD 1.3.1_15 (does not start) and 1.4.2_07.
# Java 6 has no PowerPC code and is left out.
# --thin keeps only the PowerPC code of universal files (smaller; Rosettus uses only that part).
# Nothing is installed or changed on this Mac; disk images are attached read-only and detached again.
set -u
OUT="$HOME/Desktop/Rosettus-Java-PPC.tar.gz"
WBASE="${TMPDIR:-/tmp}"
THIN=0
SOURCES=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2;;
    -w) WBASE="$2"; shift 2;;
    --thin) THIN=1; shift;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) case "$1" in /*) SOURCES+=("$1");; *) SOURCES+=("$(pwd)/$1");; esac; shift;;
  esac
done
if [ ${#SOURCES[@]} -eq 0 ]; then
  echo "usage: $0 [-o OUT.tar.gz] [-w WORKDIR] [--thin] SOURCE...   (see $0 --help)"; exit 1
fi
case "$OUT" in /*) ;; *) OUT="$(pwd)/$OUT";; esac
JVR=System/Library/Frameworks/JavaVM.framework/Versions
mkdir -p "$WBASE" || exit 1
WORK=$(mktemp -d "$WBASE/rosettus-java-import.XXXXXX") || exit 1
STAGE="$WORK/stage"; mkdir -p "$STAGE/$JVR"
MOUNTS=()
cleanup() {
  for m in "${MOUNTS[@]+"${MOUNTS[@]}"}"; do hdiutil detach -quiet "$m" 2>/dev/null || hdiutil detach -quiet -force "$m" 2>/dev/null; done
  chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
USED=""
n_src=0
PAT=('./System/Library/Frameworks/Java*' './System/Library/Java*' './System/Library/PrivateFrameworks/Java*' './Applications/Utilities/Java*')

# --- payload extraction -------------------------------------------------------------------------
# extract_pkg PKG DEST [PATTERN...]: unpack a package's files into DEST (only PATTERNs if given,
# e.g. './System/Library/Java*'). Handles bundle packages (Tiger, Archive.pax.gz) and flat packages
# (Leopard and later, a xar archive with a gzip cpio Payload, possibly holding several packages).
extract_pkg() {
  local pkg="$1" dest="$2"; shift 2
  mkdir -p "$dest"
  if [ -d "$pkg" ]; then
    local pax="$pkg/Contents/Archive.pax.gz"
    if [ -f "$pax" ]; then
      ( cd "$dest" && gzip -dc "$pax" | pax -r "$@" ) 2>>"$WORK/extract.err"; return
    fi
    local sub
    for sub in "$pkg"/Contents/Packages/*.pkg "$pkg"/Contents/Installers/*.pkg; do   # metapackage
      [ -e "$sub" ] && extract_pkg "$sub" "$dest" "$@"
    done
    return
  fi
  local p
  for p in $(tar -tf "$pkg" 2>/dev/null | grep -E '(^|/)Payload$'); do
    ( cd "$dest" && tar -xOf "$pkg" "$p" | gzip -dc | cpio -idm --quiet "$@" ) 2>>"$WORK/extract.err"
  done
}

# real_versions ROOT: Java versions in ROOT that can run (a Commands/java), not the stubs and links
real_versions() {
  local d
  for d in "$1/$JVR"/*; do
    [ -L "$d" ] && continue
    case "$(basename "$d")" in A|Current|CurrentJDK) continue;; esac
    [ -x "$d/Commands/java" ] && [ -f "$d/Commands/java" ] && basename "$d"
  done
}

# merge ROOT LABEL: take the Java versions and the support files from one unpacked source
merge() {
  local root="$1" label="$2" v l t f got=""
  for v in $(real_versions "$root"); do
    rm -rf "$STAGE/$JVR/$v"; ditto "$root/$JVR/$v" "$STAGE/$JVR/$v"; got="$got $v"
  done
  # Java 5 from the Leopard updates links JavaVM's own sub-frameworks (A/Frameworks: JavaNativeFoundation,
  # JavaRuntimeSupport); Snow Leopard's are Intel-only, so they come along with the versions that need them
  if [ -n "$got" ] && [ -d "$root/$JVR/A/Frameworks" ]; then
    rm -rf "$STAGE/$JVR/A/Frameworks"; mkdir -p "$STAGE/$JVR/A"; ditto "$root/$JVR/A/Frameworks" "$STAGE/$JVR/A/Frameworks"
  fi
  for l in "$root/$JVR"/*; do   # version links such as 1.5 -> 1.5.0 (not ones pointing outside)
    [ -L "$l" ] || continue
    case "$(basename "$l")" in Current|CurrentJDK|A) continue;; esac
    t=$(readlink "$l"); case "$t" in /*|*/*) continue;; esac
    [ -e "$STAGE/$JVR/$(basename "$l")" ] || [ -L "$STAGE/$JVR/$(basename "$l")" ] || ln -s "$t" "$STAGE/$JVR/$(basename "$l")"
  done
  # support files: the first source that has them wins (install_java.sh adds only missing ones)
  for f in System/Library/Java System/Library/Frameworks/JavaEmbedding.framework Applications/Utilities/Java \
           "$root"/System/Library/PrivateFrameworks/Java*.framework; do
    f="${f#$root/}"
    case "$f" in *JavaScript*|*'*'*) continue;; esac
    [ -e "$root/$f" ] || continue
    if [ "$f" = System/Library/Java ]; then   # merged: the DVD's Java.pkg and Essentials.pkg both add to it
      mkdir -p "$STAGE/$f"; rsync -a --ignore-existing "$root/$f/" "$STAGE/$f/"
    elif [ ! -e "$STAGE/$f" ]; then
      mkdir -p "$(dirname "$STAGE/$f")"; ditto "$root/$f" "$STAGE/$f"
    fi
  done
  if [ -n "$got" ]; then echo "  Java from $label:$got"; else echo "  (no PowerPC Java runtime in $label)"; fi
  USED="$USED${USED:+; }$label"
}

# add_source PATH: one SOURCE from the command line
add_source() {
  local s="$1" root
  n_src=$((n_src + 1)); root="$WORK/src$n_src"
  case "$s" in
    *.iso|*.dmg|*.cdr|*.img|*.toast)
      echo "Attaching $(basename "$s") (read-only) ..."
      local mp
      mp=$(hdiutil attach -readonly -nobrowse -noverify -noautoopen -mountrandom "$WORK" "$s" 2>/dev/null \
           | awk -F'\t' '$3 ~ /^\// {print $3}' | tail -1)
      if [ -z "$mp" ]; then echo "  could not attach $s"; return 1; fi
      MOUNTS+=("$mp")
      add_dir "$mp" "$(basename "$s")"
      return;;
    *.tar)
      echo "Unpacking $(basename "$s") ..."
      mkdir -p "$root.tar"; tar -xf "$s" -C "$root.tar" || return 1
      local t; for t in "$root.tar"/*.pkg.tar; do [ -e "$t" ] && tar -xf "$t" -C "$root.tar"; done
      add_dir "$root.tar" "$(basename "$s")"
      return;;
    *.pkg|*.mpkg)
      echo "Reading $(basename "$s") ..."
      extract_pkg "$s" "$root" "${PAT[@]}"; merge "$root" "$(basename "$s")"; return;;
  esac
  if [ -d "$s" ]; then add_dir "$s" "$(basename "$s")"; return; fi
  echo "Not a disc image, package, Software Update .tar or folder: $s"; return 1
}

# add_dir DIR LABEL: an install disc (System/Installation/Packages) or a folder with Java packages
add_dir() {
  local d="$1" label="$2" root
  n_src=$((n_src + 1)); root="$WORK/src$n_src"
  if [ -d "$d/System/Installation/Packages" ]; then
    local P="$d/System/Installation/Packages" ver
    ver=$(defaults read "$d/System/Library/CoreServices/SystemVersion" ProductVersion 2>/dev/null)
    label="Mac OS X ${ver:-?} install disc ($label)"
    echo "  $label: Java.pkg, Essentials.pkg (Cocoa-Java files) ..."
    [ -e "$P/Java.pkg" ] || { echo "  no Java.pkg on this disc"; return 1; }
    extract_pkg "$P/Java.pkg" "$root" "${PAT[@]}"
    extract_pkg "$P/Essentials.pkg" "$root" './System/Library/Java*'
    merge "$root" "$label"
    return
  fi
  local found=0 p
  while IFS= read -r p; do
    case "$(basename "$p")" in *[Jj]ava*) ;; *) continue;; esac
    found=1; add_source "$p"
  done < <(find "$d" -maxdepth 3 \( -name '*.pkg' -o -name '*.mpkg' \) -prune 2>/dev/null | sort)
  [ $found = 1 ] || { echo "  no Java installer found in $label"; return 1; }
}

for s in "${SOURCES[@]}"; do add_source "$s" || echo "  skipped $s"; done

VERSIONS=$(real_versions "$STAGE" | tr '\n' ' ' | sed 's/ $//')
if [ -z "$VERSIONS" ]; then echo "No PowerPC Java runtime found in the sources."; exit 1; fi
# drop version links whose target did not come along (e.g. 1.3 -> 1.3.1 without a 1.3.1)
for l in "$STAGE/$JVR"/*; do [ -L "$l" ] && [ ! -e "$l" ] && rm -f "$l"; done
for v in 1.5.0 1.4.2 1.3.1; do [ -d "$STAGE/$JVR/$v" ] && { DEF=$v; break; }; done

# only runtimes with PowerPC code are useful: check each java command
for v in $VERSIONS; do
  if ! lipo -info "$STAGE/$JVR/$v/Commands/java" 2>/dev/null | grep -q ppc; then
    echo "Java $v has no PowerPC code: left out."; rm -rf "$STAGE/$JVR/$v"
  fi
done
VERSIONS=$(real_versions "$STAGE" | tr '\n' ' ' | sed 's/ $//')
[ -n "$VERSIONS" ] || { echo "No PowerPC Java runtime found in the sources."; exit 1; }

if [ $THIN = 1 ]; then
  echo "Keeping only the PowerPC code of universal files ..."
  find "$STAGE" -type f -perm -0100 -o -type f \( -name '*.dylib' -o -name '*.jnilib' -o -name '*.bundle' \) | while read -r f; do
    a=$(lipo -info "$f" 2>/dev/null) || continue
    case "$a" in Architectures*) ;; *) continue;; esac
    pa=$(echo "${a##*: }" | tr ' ' '\n' | grep '^ppc' | head -1)
    [ -n "$pa" ] && lipo "$f" -thin "$pa" -output "$f.thin" 2>/dev/null && mv "$f.thin" "$f"
  done
fi

# version strings, read from the runtime libraries (nothing is run)
jver() {
  local d="$STAGE/$JVR/$1" f
  for f in "$d"/Libraries/libjvm.dylib "$d"/Libraries/libclient.dylib "$d"/Libraries/libhotspot.dylib "$d"/Libraries/libjava.jnilib; do
    [ -f "$f" ] && strings -a "$f"
  done | grep -oE "\b$1_[0-9]+" | sort -u -t_ -k2,2n | tail -1
}
( cd "$STAGE" && for f in "$JVR"/* System/Library/Java System/Library/Frameworks/JavaEmbedding.framework \
                    System/Library/PrivateFrameworks/Java*.framework Applications/Utilities/Java; do
    [ -e "$f" ] || [ -L "$f" ] && echo "$f"; done ) > "$STAGE/.files.txt"
{
  echo "Rosettus Java export"
  echo "date: $(date)"
  echo "system: Apple installers (made on $(sw_vers -productName) $(sw_vers -productVersion) by import_java_from_media.sh)"
  echo "sources: $USED"
  echo "versions: $VERSIONS"
  echo "default: $DEF"
  for v in $VERSIONS; do echo "java $v: java version \"$(jver $v)\""; done
} > "$STAGE/.manifest.txt"
cat "$STAGE/.manifest.txt"

if [ -s "$WORK/extract.err" ] && grep -qv -e 'blocks$' -e '^$' "$WORK/extract.err"; then
  echo "unpacking reported:"; grep -v -e 'blocks$' "$WORK/extract.err" | sort -u | head -10 | sed 's/^/  /'
fi
echo "Writing $OUT ..."
mkdir -p "$(dirname "$OUT")"
( cd "$STAGE" && COPYFILE_DISABLE=1 tar -czf "$OUT.part" . ) || { echo "Writing the archive failed."; rm -f "$OUT.part"; exit 1; }
mv -f "$OUT.part" "$OUT"
echo "Done: $OUT ($(du -h "$OUT" | awk '{print $1}'))"
echo "Install it with:  /Library/Rosettus/tools/install_java.sh \"$OUT\""
