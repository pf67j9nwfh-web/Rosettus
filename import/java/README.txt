PowerPC Java for Rosettus, from Apple's own installers
======================================================

Snow Leopard has no PowerPC Java. Rosettus can run Apple's PowerPC Java 1.3.1, 1.4.2 and 5.0; this
folder makes them from Apple's installers, so no PowerPC Mac is needed.

Easiest: double-click "Import PowerPC Java.command". It
  1. downloads Apple's last Tiger Java updates from Apple's Software Update servers
     (fetch_java_update.sh, about 130 MB into ~/Downloads, checked against known checksums):
       Java 1.3.1 and 1.4.2 Release 2          -> Java 1.3.1_16
       Java for Mac OS X 10.4 Release 9        -> Java 1.4.2_21 and 1.5.0_19
  2. builds Rosettus-Java-PPC.tar.gz on the Desktop from them (import_java_from_media.sh)
  3. installs it with /Library/Rosettus/tools/install_java.sh (asks for an administrator password)
Then:  /Library/Rosettus/rjava -version     (Java 5; rjava -j 1.4 / -j 1.3 for the others)

Without Internet access, an install DVD image works as well:
  ./import_java_from_media.sh "Mac OS X Leopard Install DVD.iso"     -> Java 1.4.2_16, 1.5.0_13
Disc images are attached read-only. A Tiger DVD carries Java 1.3.1_15 (stops at start under Rosettus;
1.3.1_16 from Release 2 works) and 1.4.2_07, so Leopard's disc is the better one of the two. Discs
and updates can be combined; later sources replace the versions they contain.

Experimental: Java 5 built for Leopard (Java for Mac OS X 10.5 Update 10: 1.5.0_30, downloaded with
"fetch_java_update.sh --update10"; the Leopard DVD's 1.5.0_13) starts and runs small programs, but
crashes under load in its garbage collector under Rosettus. 1.5.0_19 from Release 9 does not.

On a PowerPC Mac, ../tiger/export_java_from_tiger.sh (in the Snow Leopard Export Kit) collects the
Java it has instead. Apple's files are never part of Rosettus: they come from Apple's servers or
your own discs.
