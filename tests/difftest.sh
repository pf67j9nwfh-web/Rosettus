#!/bin/bash
# difftest.sh [test ...]: run the PowerPC differential tests in t/ppc under this port and compare with
# the Snow Leopard Rosetta output (<test>.sl.txt, made with "arch -ppc ./<test> > <test>.sl.txt" on 10.6).
# Lines matching a regex in <test>.allow (expected platform differences) are ignored.
# Each test runs in guest-toolbox mode (as the apps do) with a 60 s limit; output in /tmp/ppcdiff/.
cd "$(dirname "$0")/ppc" || exit 1
mkdir -p /tmp/ppcdiff
tests=("$@")
[ ${#tests[@]} -eq 0 ] && for f in *.sl.txt; do tests+=("${f%.sl.txt}"); done
pass=0; fail=0
for t in "${tests[@]}"; do
  [ -x "$t" ] || { echo "?? $t: no binary"; continue; }
  OAH_GTB=1 OAH_QUIET_EXIT=1 OAH_NO_REPORTS=1 perl -e 'alarm 60; exec @ARGV' "${ROSETTUS:-/Library/Rosettus/rosettus}" "$PWD/$t" > /tmp/ppcdiff/$t.mj.txt 2> /tmp/ppcdiff/$t.err
  rc=$?
  if [ -f "$t.allow" ]; then
    grep -v -E -f "$t.allow" "$t.sl.txt" > /tmp/ppcdiff/$t.a; grep -v -E -f "$t.allow" /tmp/ppcdiff/$t.mj.txt > /tmp/ppcdiff/$t.b
  else cp "$t.sl.txt" /tmp/ppcdiff/$t.a; cp /tmp/ppcdiff/$t.mj.txt /tmp/ppcdiff/$t.b; fi
  if diff -u /tmp/ppcdiff/$t.a /tmp/ppcdiff/$t.b > /tmp/ppcdiff/$t.diff && [ $rc -eq 0 ]; then
    echo "PASS $t"; pass=$((pass+1))
  else
    echo "FAIL $t (rc $rc)"; fail=$((fail+1)); grep '^[-+][^-+]' /tmp/ppcdiff/$t.diff | head -20 | sed 's/^/     /'
  fi
done
echo "$pass passed, $fail failed"
