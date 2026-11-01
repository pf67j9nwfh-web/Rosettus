#!/bin/bash
# fetch_java_update.sh [FOLDER] - download Apple's last PowerPC Java updates from Apple's Software Update
# servers, for import_java_from_media.sh:
#   Java 1.3.1 and 1.4.2 Release 2     (Tiger catalog; Java 1.3.1_16, the last 1.3.1)      about 48 MB
#   Java for Mac OS X 10.4 Release 9   (Tiger catalog; Java 1.4.2_21 and 1.5.0_19)       about 85 MB
#   Java for Mac OS X 10.5 Update 10   (Leopard catalog; Java 1.5.0_30)  only with --update10, about 126 MB
#     (experimental: its Java 5 starts and runs small programs, but crashes under load in its garbage
#      collector under Rosettus; 1.5.0_19 from Release 9 does not)
# Apple still serves them. Each download is checked against its known size and SHA-1. Nothing is installed.
#   usage: ./fetch_java_update.sh [--update10] [FOLDER]     (default: ~/Downloads)
U10=0
if [ "${1:-}" = "--update10" ]; then U10=1; shift; fi
DIR="${1:-$HOME/Downloads}"
mkdir -p "$DIR" || exit 1
check() { [ "$(stat -f %z "$1" 2>/dev/null)" = "$2" ] && [ "$(shasum -a 1 "$1" | awk '{print $1}')" = "$3" ]; }
fetch() {   # fetch NAME SIZE SHA1 CATALOG
  local name="$1" size="$2" sha="$3" cat="$4" out="$DIR/$1" url
  if [ -f "$out" ] && check "$out" "$size" "$sha"; then echo "Already downloaded: $out"; return 0; fi
  echo "Looking up $name in Apple's update catalog ..."
  url=$(curl -fsSL "$cat" | grep -o "https\{0,1\}://[^<]*/$name" | head -1)
  if [ -z "$url" ]; then echo "  Apple's catalog no longer lists $name (or the server could not be reached)."; return 1; fi
  echo "Downloading $url ..."
  curl -fL --progress-bar -o "$out.part" "$url" || { echo "  download failed"; rm -f "$out.part"; return 1; }
  if ! check "$out.part" "$size" "$sha"; then echo "  the download does not match the expected size and checksum"; rm -f "$out.part"; return 1; fi
  mv -f "$out.part" "$out"; echo "  saved $out"
}
st=0
fetch Java131and142Release2.tar 47513600 147281aa86a185374360d4f421300fb4098fcc84 \
  https://swscan.apple.com/content/catalogs/index-1.sucatalog || st=1
fetch JavaForMacOSX10.4Release9.tar 85032960 f83aedd6facdea9c924806141945b58f2ed1ba92 \
  https://swscan.apple.com/content/catalogs/index-1.sucatalog || st=1
if [ $U10 = 1 ]; then
  fetch JavaForMacOSX10.5Update10.pkg 126389229 faab832c4685f94bd7e9cc973ea14a6c11901963 \
    https://swscan.apple.com/content/catalogs/others/index-leopard-snowleopard.merged-1.sucatalog || st=1
fi
[ $st = 0 ] && echo "Next:  import_java_from_media.sh \"$DIR/Java131and142Release2.tar\" \"$DIR/JavaForMacOSX10.4Release9.tar\""
exit $st
