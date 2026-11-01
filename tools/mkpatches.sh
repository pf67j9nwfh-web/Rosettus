#!/bin/bash
# Developer tool (run on the Mojave machine that has a working install): regenerate import/patches
# from the thinned Snow Leopard originals in $ORIG and the working install root $ROOT.
ORIG=${ORIG:-/tmp/slorig}; ROOT=${ROOT:-/Library/Rosettus}; REPO=${REPO:-$HOME/rosettus}
P="$REPO/import/rpatch.py"; OUT="$REPO/import/patches"
rm -rf "$OUT"; mkdir -p "$OUT"
n=0
while IFS=$'\t' read -r rel rest; do
  [ -n "$rel" ] || continue
  mkdir -p "$OUT/$(dirname "$rel")"
  python "$P" make "$ORIG/$rel" "$ROOT/$rel" "$OUT/$rel.rpatch" >/dev/null || echo "FAILED $rel"
  if [ -f "$ROOT/$rel.prebridge" ]; then
    python "$P" make "$ORIG/$rel" "$ROOT/$rel.prebridge" "$OUT/$rel.prebridge.rpatch" >/dev/null || echo "FAILED $rel.prebridge"
  fi
  n=$((n+1))
done < /tmp/man_mod.txt
python "$P" make /tmp/tr_dec_u /tmp/tr_unsigned "$OUT/translate.rpatch"
echo "$n files"; du -sh "$OUT"; find "$OUT" -name "*.rpatch" | wc -l
