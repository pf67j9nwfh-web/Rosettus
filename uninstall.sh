#!/bin/bash
# uninstall.sh - remove Rosettus.
# usage: uninstall.sh [--root DIR] [--yes]
#        sudo uninstall.sh --system [--yes]      (the install for every user: /Library/Rosettus)
# Apps you patched with rosettus_patch.py keep only a small launcher stub; restore them first with
#   python ROOT/rosettus_patch.py --unpatch /path/App.app   (or Unpatch in Rosettus Launcher)
ROOT="$HOME/Library/Rosettus"; YES=0; SYSTEM=0
while [ $# -gt 0 ]; do case "$1" in --root) ROOT="$2"; shift;; --yes) YES=1;; --system) SYSTEM=1; ROOT=/Library/Rosettus;; esac; shift; done
if [ $SYSTEM = 1 ]; then
  [ "$(id -u)" = 0 ] || { echo "--system needs root: sudo $0 --system"; exit 1; }
  AGENTDIR=/Library/LaunchAgents; APPDIR=/Applications
else
  AGENTDIR="$HOME/Library/LaunchAgents"; APPDIR="$HOME/Applications"
fi
[ -f "$ROOT/translate_mojave" ] || [ -d "$ROOT/Setup" ] || { echo "no Rosettus install at $ROOT"; exit 1; }
if [ $YES = 0 ]; then
  read -r -p "Remove $ROOT, its launch agents and Rosettus Launcher.app? [y/N] " a
  case "$a" in y|Y|yes) ;; *) echo "cancelled"; exit 0;; esac
fi
# Drain Version Cue and its database before removing the translator or support files.
if [ -f "$ROOT/tools/install_versioncue.py" ]; then
  if [ "$SYSTEM" = 1 ]; then vcargs=(--all-users); else vcargs=(--user "$(id -un)"); fi
  if ! /usr/bin/python "$ROOT/tools/install_versioncue.py" --root "$ROOT" --remove "${vcargs[@]}"; then
    echo "Version Cue has not finished stopping; Rosettus was not removed." >&2
    exit 1
  fi
fi
for a in "$AGENTDIR"/com.rosettus.ppc.*.plist; do
  [ -f "$a" ] || continue
  if [ $SYSTEM = 1 ]; then
    for uid in $(ps -axo uid=,comm= | awk '/loginwindow/{print $1}' | sort -u); do
      launchctl bootout "gui/$uid/$(basename "$a" .plist)" 2>/dev/null
    done
  else
    launchctl unload "$a" 2>/dev/null
  fi
  rm -f "$a"
done
pkill -f "$ROOT/translate_mojave" 2>/dev/null
rm -rf "$APPDIR/Rosettus Launcher.app"
rm -rf "$APPDIR/Rosettus Preference Pane Viewer.app"
rm -rf "$ROOT"
[ $SYSTEM = 1 ] && pkgutil --forget io.github.rosettus >/dev/null 2>&1
echo "Rosettus removed. (Launcher apps in $APPDIR/Rosettus, if any, can be deleted too.)"
