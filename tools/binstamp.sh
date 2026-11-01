#!/bin/bash
# tools/binstamp.sh - keep track of which sources each prebuilt file in bin/ was built from.
#
# bin/ holds prebuilt binaries (liboah has to be built on Snow Leopard, the rest on Mojave), so a
# changed source with a forgotten rebuild would otherwise go into a package unnoticed.
# bin/SOURCES.sha1 records, for every artifact, a SHA-1 over its source files (names and contents).
#
#   tools/binstamp.sh --check            list artifacts whose sources changed since they were built (exit 1 if any)
#   tools/binstamp.sh --update NAME...   record the current sources for NAME (after rebuilding it)
#   tools/binstamp.sh --update-all       record all of them
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
ST="$R/bin/SOURCES.sha1"
ARTS="liboah_mojave.dylib rlaunch libppcexec.dylib libjavashim.dylib Rosettus_Launcher.app Rosettus_Preference_Pane_Viewer.app prefpanes savers versioncue trigger rosettus-kc"
srcdir() {
  case "$1" in
    liboah_mojave.dylib) echo src/liboah ;;
    rlaunch) echo src/rlaunch ;;
    libppcexec.dylib) echo src/ppcexec ;;
    libjavashim.dylib) echo src/javashim ;;
    rosettus-kc) echo src/kchelper ;;
    Rosettus_Launcher.app) echo src/launcher ;;
    Rosettus_Preference_Pane_Viewer.app|prefpanes) echo src/prefpanes ;;
    savers) echo src/savers ;;
    versioncue) echo src/versioncue ;;
    trigger) echo src/trigger import/sl/dump_text_ppc.c ;;   # import/sl/Install Rosetta.app, dump_text_ppc
    *) echo "unknown artifact $1" >&2; exit 2 ;;
  esac
}
hashof() {
  local d; d="$(srcdir "$1")"
  (cd "$R" && find $d -type f ! -name .DS_Store | LC_ALL=C sort | while read -r f; do
     printf '%s %s\n' "$(shasum "$f" | cut -c1-40)" "$f"; done) | shasum | cut -c1-40
}
recorded() { [ -f "$ST" ] && awk -v a="$1" '$2==a{print $1}' "$ST"; }
put() {
  local a="$1" h; h="$(hashof "$a")"
  touch "$ST"; grep -v " $a\$" "$ST" > "$ST.tmp" || true
  echo "$h $a" >> "$ST.tmp"; LC_ALL=C sort -k2 "$ST.tmp" > "$ST"; rm -f "$ST.tmp"
  echo "recorded $a ($(srcdir "$a"))"
}
case "$1" in
  --check)
    bad=0
    for a in $ARTS; do
      if [ "$(recorded "$a")" != "$(hashof "$a")" ]; then
        echo "STALE: ${a//_/ } - $(srcdir "$a") changed since it was built"; bad=1
      fi
    done
    [ $bad = 0 ] && echo "bin/ is up to date with src/"
    exit $bad ;;
  --update) shift; for a in "$@"; do put "${a// /_}"; done ;;
  --update-all) for a in $ARTS; do put "$a"; done ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
