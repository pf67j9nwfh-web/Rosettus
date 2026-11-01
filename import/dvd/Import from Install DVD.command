#!/bin/bash
# Double-click: make the Snow Leopard files for Rosettus from a Mac OS X 10.6 install DVD (see README.txt)
cd "$(dirname "$0")" || exit 1
clear
cat <<'EOF'
Rosettus - Snow Leopard files from an install DVD
=================================================
This makes Rosettus-SL-export.tar.gz on your Desktop from a Mac OS X 10.6 Snow Leopard
install DVD (a disc image .iso/.dmg, or the disc itself) plus Apple's free 10.6.8 updates.
It takes 20-40 minutes and about 12 GB of free disk space while it runs.

EOF
DVD=
for v in /Volumes/*; do [ -f "$v/System/Installation/Packages/BaseSystem.pkg" ] && [ -f "$v/System/Installation/Packages/Essentials.pkg" ] && DVD="$v"; done
if [ -n "$DVD" ]; then
  echo "Found the install disc: $DVD"
  read -r -p "Use it? [Y/n] " a; case "$a" in n*|N*) DVD=;; esac
fi
if [ -z "$DVD" ]; then
  echo "Drag the Snow Leopard install DVD image (.iso or .dmg) into this window, then press Return:"
  read -r DVD
  DVD="${DVD%"${DVD##*[![:space:]]}"}"            # trailing spaces
  case "$DVD" in \'*\') DVD="${DVD#\'}"; DVD="${DVD%\'}";; \"*\") DVD="${DVD#\"}"; DVD="${DVD%\"}";; esac
  DVD="$(printf '%s' "$DVD" | sed 's/\\\(.\)/\1/g')"   # Terminal escapes spaces when a file is dropped
fi
[ -e "$DVD" ] || { echo "Not found: $DVD"; read -r -p "Press Return to close." a; exit 1; }
echo
echo "Apple's Snow Leopard updates are needed too: the 10.6.8 Combo update, the 10.6.8 Supplemental"
echo "update, Security Update 2013-004, Rosetta, Safari 5.1.10, Java for Mac OS X 10.6 and the Mac App"
echo "Store update (about 1.5 GB, from Apple's own servers, swcdn.apple.com)."
read -r -p "Download them now? [y/N] " a
case "$a" in y*|Y*) DL=--download;; *)
  echo "Then put them in one folder (the file names are listed next) and run:"
  echo "   \"$PWD/import_from_dvd.sh\" --dvd \"$DVD\" --updates FOLDER"
  /usr/bin/python dvdtool.py updates "$HOME/Library/Caches/Rosettus-DVD/updates" 2>/dev/null | sed -n '2,$p'
  read -r -p "Press Return to close." a; exit 1;;
esac
echo
if ./import_from_dvd.sh --dvd "$DVD" $DL; then
  echo
  echo "Now open Rosettus Launcher: it offers \"Install Snow Leopard Files...\" (File menu)."
fi
read -r -p "Press Return to close." a
