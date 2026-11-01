#!/bin/bash
# Developer tool: write import/expected.sha1 - checksums install.sh verifies after patching:
# every patched file (and .prebridge), the translator, and the gtb overlay binaries.
# usage: tools/mkexpected.sh [WORKROOT]   (default /Library/Rosettus)
set -e
W="${1:-/Library/Rosettus}"
R="$(cd "$(dirname "$0")/.." && pwd)"
cd "$R/import/patches"
{
  find . -name '*.rpatch' ! -name translate.rpatch | sed 's#^\./##; s#\.rpatch$##' | sort | while IFS= read -r rel; do
    (cd "$W" && shasum "$rel")
  done
  (cd "$W" && shasum translate_mojave)
  (cd "$W" && find gtb -type f ! -name '*.prebridge' | sort | while IFS= read -r f; do shasum "$f"; done)
} > "$R/import/expected.sha1"
wc -l "$R/import/expected.sha1"
