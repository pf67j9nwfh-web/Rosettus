#!/bin/bash
# copy_forks.sh SOURCE_ROOT DEST_ROOT
#
# Copies the Finder info and resource forks from SOURCE_ROOT onto the same-named files in
# DEST_ROOT, for files whose data forks are identical. Use it to repair a Rosetta root that was
# unpacked from an export that lost its resource forks (copied without them, so
# CFM applications fail with cfragNoLibraryErr and suitcase fonts are empty).
#
# SOURCE_ROOT can be a mounted Snow Leopard volume ("/Volumes/Snow Leopard") or another
# complete Rosetta root. Needs write access to DEST_ROOT (run with sudo for /Library/Rosettus).
set -e
SRC="${1:?usage: copy_forks.sh SOURCE_ROOT DEST_ROOT}"; DST="${2:?usage: copy_forks.sh SOURCE_ROOT DEST_ROOT}"
n=0; skipped=0
cd "$SRC"
while IFS= read -r -d '' f; do
  f="${f#./}"
  [ -f "$DST/$f" ] && [ ! -L "$DST/$f" ] || continue
  case "$f" in */.DS_Store|.DS_Store) continue;; esac
  if ! cmp -s "$f" "$DST/$f"; then skipped=$((skipped+1)); continue; fi
  fi_hex=$(xattr -px com.apple.FinderInfo "$f" 2>/dev/null || true)
  [ -n "$fi_hex" ] && xattr -wx com.apple.FinderInfo "$fi_hex" "$DST/$f"
  if [ -s "$f/..namedfork/rsrc" ]; then cat "$f/..namedfork/rsrc" > "$DST/$f/..namedfork/rsrc"; fi
  n=$((n+1))
done < <(find ./System/Library ./Library ./usr -type f -xattr -print0 2>/dev/null)
echo "copied Finder info / resource forks for $n files ($skipped skipped: data forks differ)"
