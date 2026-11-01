# How Rosettus works

## The pieces

```
 PowerPC app (.app, CFM or Mach-O)
        │  launched by: Rosettus Launcher · a patched app's Intel stub (rlaunch) · a launcher app · ROOT/rosettus
        ▼
 ROOT/rosettus (shell)  ── sets the guest search paths, OAH_GTB, DYLD_INSERT_LIBRARIES=liboah_mojave.dylib
        ▼
 translate_mojave  = Snow Leopard's /usr/libexec/oah/translate (Rosetta), 38-byte patch, ad-hoc signed
   │   translates PowerPC code to x86 and runs it in this 32-bit Intel process
   │
   ├── guest tree (ROOT/System, ROOT/usr, ROOT/Library): Snow Leopard's PowerPC frameworks,
   │   thinned to their PowerPC slices; 56 of them patched (bridge marks and small fixes)
   ├── gtb/ overlay: unpatched HIToolbox, QD, CoreGraphics, Security, AE, AGL copies whose
   │   window-server calls are bridged instead ("guest toolbox" mode, the default)
   └── liboah_mojave.dylib (this project, i386) loaded into the translator:
         • bridges: ~2,000 guest functions start with `twi 31,r0,ID; blr`; the trap lands in
           liboah, which does the work natively on Mojave (CGS window server, OpenGL/CGL,
           CoreAudio, Launch Services, Apple Events, Authorization, pasteboard...) and returns
           into guest code
         • syscall and Mach-message fixes: paths into the guest tree (pathmap), sysctl
           (KERN_CLASSIC for the Rosetta shims), Snow Leopard MIG messages Mojave changed
         • diagnostics: crash/hang reports, unbridged-call telemetry, gtrace
```

Services that Mojave no longer offers in a form 10.6 code understands run as translated
Snow Leopard daemons under per-user launchd agents (`com.rosettus.ppc.pboard`, `.coreservicesd`,
`.fontd`, `.fontworker`). They start when a PowerPC app looks them up and exit 30 seconds after
the last PowerPC app quits.

## Launch paths

| How the user starts it | What runs |
|---|---|
| Rosettus Launcher → Open | `ROOT/rosettus App.app` (posix_spawn, log to ~/Library/Logs/Rosettus) |
| Patched app, double-click | the app's main executable is now fat: original PowerPC slice + an x86_64 stub (`rlaunch`). macOS runs the stub, which execs `ROOT/rosettus` on the same bundle; translate runs the PowerPC slice. A hard link to translate inside the bundle (`.rosetta-translate`) makes the process belong to the app (Dock icon, permission prompts, Force Quit). |
| Launcher app in ~/Applications/Rosettus | a small native bundle (`mkwrapper.sh`) whose `launcher.sh` runs `ROOT/rosettus` with the app |
| Terminal | `ROOT/rosettus /path/App.app` or a PowerPC tool |

## The import

`import/sl/export_from_sl.sh` (on Snow Leopard) copies each file listed in
`import/manifest.tsv.gz` (114,167 entries: path, how to copy it, size, SHA-1), thins universal
binaries to their PowerPC slice, and checks every checksum. Rosetta's `translate` has a
protected `__TEXT` segment that the kernel decodes when it loads the program on Apple hardware;
`dump_text_ppc`, a small PowerPC program, runs under the user's own Rosetta and writes that
decoded segment, which is spliced into a copy of `translate`.

`install.sh` (on Mojave) unpacks the export and applies `import/patches/**/*.rpatch`. These are
sparse patches holding only the bytes this project changed, each checked by SHA-1 before and
after. It patches translate, signs it ad hoc, builds the gtb overlay and bridge marks, then
verifies the result against `import/expected.sha1`.

## Bridges

`bridges/bridges.spec` lists `LIBRARY Function KIND:` lines. `genbridges.py` turns it into
`bridges_gen.h` (the dispatch table compiled into liboah) and `bridges_patch.sh`, which runs
`bridgepatch.py` to mark the functions in the guest libraries. IDs are given in file order, so
**new lines go at the end**. `bridgepatch.py` always starts from `LIB.prebridge`, so re-running
it is safe. To call a bridged function's original guest code, liboah reads its first two
instructions from `.prebridge` and builds a trampoline in guest-mapped memory
(`oah_guest_code_alloc`: native pages created after startup are invisible to translate).

## Where to look in the source

| File | Area |
|---|---|
| `liboah_mojave.c` | start-up, syscalls, Mach messages, pathmap, bridge dispatch, crash/hang reports |
| `oah_cgs.h` | window server: windows, events, activation, menus, Dock, cursor |
| `oah_gl.h`, `oah_glx.h` | OpenGL / CGL / AGL |
| `oah_au.h`, `oah_snd.h` | CoreAudio HAL, output units, Sound Manager |
| `oah_ls.h`, `oah_aen.h`, `oah_sfl.h` | Launch Services, Apple Events, shared file lists |
| `oah_auth.h` | Authorization Services (admin password prompts) |
| `oah_gtrace.h` | guest function tracing and built-in guest fixes |
| `oah_hang.h` | hang detection, telemetry flush |
