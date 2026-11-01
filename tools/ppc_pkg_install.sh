#!/bin/bash
# ppc_pkg_install.sh - install a PowerPC-era installer package on Mojave.
#
# Most old packages install with Mojave's own Installer. What breaks is PowerPC-only code inside
# them: preflight/postflight/InstallationCheck tools and Installer plug-ins. This script:
#   1. scans the package (flat .pkg, bundle .pkg/.mpkg, nested packages);
#   2. if nothing is PowerPC-only, opens it in Installer unchanged;
#   3. otherwise makes a copy in which each PowerPC-only script/tool runs through Rosetta (a small
#      shell wrapper calls ROOT/rosettus with the original) and PowerPC-only Installer plug-ins are
#      left out (they would have to load into Mojave's Installer), then opens the copy in Installer.
# Installer asks for the administrator password itself when the package needs it.
#
# usage: ppc_pkg_install.sh PACKAGE [--dry-run]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
PKG="${1%/}"; DRY="$2"
[ -e "$PKG" ] || { echo "no such package: $PKG"; exit 1; }

is_ppc_only() {   # Mach-O with a PowerPC slice and no Intel slice, or a CFM/PEF binary
  local f="$1" info
  [ -f "$f" ] || return 1
  [ "$(head -c 8 "$f" 2>/dev/null)" = "Joy!peff" ] && return 0
  info=$(lipo -info "$f" 2>/dev/null) || return 1
  case "$info" in *ppc*) ;; *) return 1;; esac
  case "$info" in *i386*|*x86_64*) return 1;; esac
  return 0
}

WORK="$(mktemp -d /tmp/ppcpkg.XXXXXX)"
NAME="$(basename "$PKG")"
if [ -d "$PKG" ]; then
  cp -R "$PKG" "$WORK/$NAME"; FLAT=0
else
  pkgutil --expand "$PKG" "$WORK/$NAME.x"; FLAT=1
fi
TREE="$WORK/$NAME"; [ $FLAT = 1 ] && TREE="$WORK/$NAME.x"

wrapped=0; dropped=0
# scripts and tools (any executable file that is PowerPC-only), outside of payload archives
while IFS= read -r f; do
  case "$f" in */Plugins/*) continue;; esac
  is_ppc_only "$f" || continue
  mv "$f" "$f.ppc"
  cat > "$f" <<EOF
#!/bin/sh
# Rosettus: run the PowerPC original of this installer script
exec "$ROOT/rosettus" "\$0.ppc" "\$@"
EOF
  chmod 755 "$f"
  echo "  runs through Rosetta: ${f#$TREE/}"
  wrapped=$((wrapped + 1))
done < <(find "$TREE" -type f -perm -100 ! -name "*.ppc")

# Installer plug-ins (in-process bundles): drop PowerPC-only ones and their InstallerSections entries
while IFS= read -r b; do
  exe="$b/Contents/MacOS/$(defaults read "$b/Contents/Info" CFBundleExecutable 2>/dev/null || basename "$b" .bundle)"
  is_ppc_only "$exe" || continue
  plist="$(dirname "$b")/InstallerSections.plist"
  if [ -f "$plist" ]; then
    /usr/bin/python - "$plist" "$(basename "$b")" <<'PY'
import plistlib, sys
p, name = sys.argv[1], sys.argv[2]
d = plistlib.readPlist(p)
d['SectionOrder'] = [s for s in d.get('SectionOrder', []) if s != name]
plistlib.writePlist(d, p)
PY
  fi
  rm -rf "$b"
  echo "  left out PowerPC-only Installer plug-in: $(basename "$b")"
  dropped=$((dropped + 1))
done < <(find "$TREE" -type d -name "*.bundle" -path "*/Plugins/*")

if [ $wrapped = 0 ] && [ $dropped = 0 ]; then
  echo "No PowerPC-only parts: opening $NAME in Installer unchanged."
  rm -rf "$WORK"
  [ -n "$DRY" ] || open -a Installer "$PKG"
  exit 0
fi

OUT="$WORK/out"; mkdir -p "$OUT"
if [ $FLAT = 1 ]; then pkgutil --flatten "$TREE" "$OUT/$NAME"; else mv "$TREE" "$OUT/$NAME"; fi
xattr -dr com.apple.quarantine "$OUT/$NAME" 2>/dev/null || true
echo "Prepared $OUT/$NAME ($wrapped wrapped, $dropped plug-in(s) left out)."
[ -n "$DRY" ] && exit 0
open -W -a Installer "$OUT/$NAME"
rm -rf "$WORK"
