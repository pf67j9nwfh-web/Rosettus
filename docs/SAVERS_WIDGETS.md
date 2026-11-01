# Screen savers and Dashboard widgets

## Screen savers

Mojave's screen saver engine is a 64-bit Intel process, so it cannot load a PowerPC `.saver`.
`tools/install_saver.py` (or Rosettus Launcher → *Install*) creates
`~/Library/Screen Savers/<Name> (PowerPC).saver`. This is a small native saver (source:
`src/savers/shim`) with the original saver inside it. When the screen saver starts, the shim
runs **Rosettus Saver Host** (`src/savers/host`, a PowerPC program) through Rosetta. The host
loads the original saver into the translated 10.6 frameworks and shows it full screen, one
window level above the screen saver engine's own window. Its mouse events pass through, so
moving the mouse still ends the screen saver as usual. The host is stopped when the screen saver
stops, and it also exits if its parent goes away.

Limits: the small preview in System Preferences shows the saver's name instead of the animation,
and "Screen Saver Options" sheets of PowerPC savers are not shown. `bin/savers/PPCTestSaver.saver`
is a small PowerPC saver for trying it out.

```
python ~/Library/Rosettus/tools/install_saver.py "/path/to/Some.saver"
python ~/Library/Rosettus/tools/install_saver.py "/path/to/Some.saver" --uninstall
```

## Dashboard widgets

Widgets are HTML, CSS and JavaScript, and Mojave's Dashboard still runs them. The only PowerPC
part a widget can have is a widget plug-in: a native bundle loaded *into* Dashboard, which
Rosetta cannot translate in that 64-bit process. `tools/install_widget.py` copies the widget to
`~/Library/Widgets`. If its plug-in is PowerPC-only, the tool detaches it (the Info.plist
`Plugin` key, renamed `RosettaDetachedPlugin`), so the rest of the widget works. Dashboard is
off by default on Mojave: System Preferences > Mission Control > Dashboard.
