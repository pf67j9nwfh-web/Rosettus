Rosettus - Snow Leopard Export Kit
==================================

Rosettus runs PowerPC apps on macOS Mojave with Snow Leopard's own Rosetta. Apple's files are not
part of Rosettus: they come from a Mac (or virtual machine) running Mac OS X 10.6.8 Snow Leopard.
This folder is what you run there.

1. Copy this whole folder ("Snow Leopard Export Kit") to the Snow Leopard Mac
   (a USB stick or a network share).

2. On Snow Leopard, open Terminal and run:

       cd "/path/to/Snow Leopard Export Kit"
       ./export_from_sl.sh

   If Rosetta is not installed yet, the script opens "Install Rosetta.app": Snow Leopard then
   offers to download and install Rosetta. Accept, and run the script again.
   The result is Rosettus-SL-export.tar.gz on the Desktop (5-15 minutes, about 1 GB).

3. Copy Rosettus-SL-export.tar.gz back to the Mojave Mac (Downloads or Desktop) and open
   Rosettus Launcher: it offers "Install Snow Leopard Files..." (File menu) and finds it there.
   From Terminal instead:  sudo /Library/Rosettus/Setup/install.sh --system ~/Downloads/Rosettus-SL-export.tar.gz

Snow Leopard needs to be 10.6.8 with its updates (Software Update until nothing is left);
files from another build are listed by the script, and install.sh says which ones it cannot use.

No Snow Leopard Mac? "Snow Leopard Install DVD Import" (next to this folder in Applications >
Rosettus; import/dvd in the repository) makes the same export on the Mojave Mac from a Snow
Leopard install DVD and Apple's 10.6.8 updates.

Java (optional): tiger/export_java_from_tiger.sh does the same for Java 5 on a PowerPC or
Tiger Mac - see the Java section of the Rosettus README.
