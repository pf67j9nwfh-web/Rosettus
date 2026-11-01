#!/bin/bash
# javatest.sh [ARCHIVE] - (optionally install ARCHIVE, then) run -version, javac, JTest and SwingTest
# with every installed PowerPC Java. Run on the Mojave Mac (work folder: JAVATEST_DIR, default ~/rosettus-javatest).
S=sudo
R=/Library/Rosettus/rjava
D="${JAVATEST_DIR:-$HOME/rosettus-javatest}"
mkdir -p "$D"; cp "$(dirname "$0")/JTest.java" "$(dirname "$0")/SwingTest.java" "$D/" 2>/dev/null
if [ -n "$1" ]; then $S /Library/Rosettus/tools/install_java.sh "$1" 2>&1 | grep -v "^  " ; fi
cat /Library/Rosettus/java-manifest.txt
for v in 1.3 1.4 1.5; do
  echo "===== $v"
  $R -j $v -version 2>&1 | grep -v "^\[" | head -4
  rm -rf "$D/$v"; mkdir -p "$D/$v"
  ( cd "$D" && $R -j $v javac -d "$v" JTest.java SwingTest.java 2>&1 | grep -v "^\[" | head -5 )
  ls "$D/$v" | tr '\n' ' '; echo
  ( cd "$D/$v" && $R -j $v -cp . JTest 2>&1 | grep -v "^\[" | tail -3 )
  ( cd "$D/$v" && $R -j $v -cp . SwingTest 2>&1 | grep -v "^\[" | tail -3 ) &
  sp=$!; sleep 4; screencapture -x "$D/swing-$v.png" 2>/dev/null; wait $sp
done
