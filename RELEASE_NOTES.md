# Rosettus 1.0 release notes

**October 31, 2026 - the first release.**

Rosettus runs PowerPC Mac applications on macOS 10.14 Mojave, using the Rosetta translator from your
own Mac OS X 10.6 Snow Leopard installation. No Apple software is included: Rosettus copies the files
Rosetta needs from a Snow Leopard Mac or a Snow Leopard install DVD.

## What's included

* **Rosettus Launcher** (in `/Applications`): imports the Snow Leopard files, finds PowerPC apps,
  makes launchers for them, and opens them with a double-click, from the Dock or from the Finder.
* **Installer package** `Rosettus 1.0.pkg`: installs for every account on the Mac. A setup kit goes
  to `/Library/Rosettus/Setup`, and three folders to `/Applications/Rosettus`:
  * *Snow Leopard Export Kit*: copy it to the Snow Leopard Mac to make the export.
  * *Snow Leopard Install DVD Import*: makes the same export on Mojave from a Snow Leopard install
    DVD or disc image and Apple's 10.6.8 updates, without a Snow Leopard Mac.
  * *PowerPC Java Import*: builds PowerPC Java 1.3.1, 1.4.2 and 5.0 from Apple's own Java updates,
    downloaded from Apple's Software Update servers (optional).
* **The shim library** (`liboah_mojave.dylib`), which connects the PowerPC system frameworks to
  Mojave's window server, events, audio, OpenGL, clipboard, Launch Services and Apple Events.

## What works

Apps tested include:

* Microsoft Office 2004: Word, Excel and PowerPoint, with VBA macros and embedded objects.
* iWork '09, AppleWorks 6, Internet Explorer 5.2 and TextWrangler.
* Adobe Illustrator, InDesign and GoLive CS2.
* SimCity 4, Bejeweled 2 and Blender 2.63.
* Hex Fiend, Transmission and Kid Pix Deluxe 4.

Other things that work:

* Hardware and system services: printing, sound output and input, QuickTime movies, OpenGL, MIDI,
  the camera, HID game devices, the keychain and FSEvents.
* Documents opened from the Finder.
* Copy and paste with Mac apps.
* Old installer packages, screen savers, Dashboard widgets and preference panes.

[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) lists every app and feature tested, and what is known
not to work.

## Requirements

* macOS 10.14 Mojave. 32-bit code does not run on 10.15 or later.
* About 4 GB free.
* Mac OS X 10.6.8 with Rosetta, or a Snow Leopard install DVD or disc image.

## Known issues

* PowerPC Java:
  * A Java program that dereferences `null` (or otherwise relies on the JVM's own fault handling)
    crashes the JVM instead of getting a `NullPointerException`.
  * Java 1.3.1's garbage collector crashes under heavy allocation with several threads.
  * Java 5 built for Leopard (1.5.0_30 from Java for Mac OS X 10.5 Update 10, 1.5.0_13 from a Leopard
    DVD) crashes in its garbage collector under load. The Java import therefore installs Tiger's
    1.5.0_19 by default.
* Edit > Special Characters does nothing: there is no Character Viewer for PowerPC apps.
* Fonts that Office 2004 keeps in `~/Library/Fonts` are not available to it.
* Change notifications are not delivered for some services: network configuration, power sources
  and MIDI devices.
* AppleWorks 6 cannot open RTF files saved by current apps (a limit of its RTF translator).
* Illustrator and InDesign CS2 must be opened through their launchers (Mojave refuses them as apps that
  need Java 6).

For help, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). Logs are in
`~/Library/Logs/Rosettus/`.
