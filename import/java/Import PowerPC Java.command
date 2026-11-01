#!/bin/bash
# Double-click: download Apple's PowerPC Java updates, build the Rosettus Java archive and install it.
cd "$(dirname "$0")" || exit 1
echo "PowerPC Java for Rosettus"
echo "Downloads Apple's PowerPC Java updates (about 130 MB, from Apple's servers) into ~/Downloads,"
echo "builds ~/Desktop/Rosettus-Java-PPC.tar.gz and installs it into Rosettus."
echo
if [ ! -x /Library/Rosettus/rosettus ]; then echo "Rosettus is not installed (/Library/Rosettus)."; read -r -p "Press Return to close."; exit 1; fi
D="$HOME/Downloads"
bash ./fetch_java_update.sh "$D" || { echo; read -r -p "Download failed. Press Return to close."; exit 1; }
echo
OUT="$HOME/Desktop/Rosettus-Java-PPC.tar.gz"
bash ./import_java_from_media.sh -o "$OUT" "$D/Java131and142Release2.tar" "$D/JavaForMacOSX10.4Release9.tar" || { echo; read -r -p "Building the archive failed. Press Return to close."; exit 1; }
echo
/Library/Rosettus/tools/install_java.sh "$OUT" || { echo; read -r -p "Installing failed. Press Return to close."; exit 1; }
echo
/Library/Rosettus/rjava -version 2>&1 | grep -v '^\['
echo
read -r -p "Done. Press Return to close."
