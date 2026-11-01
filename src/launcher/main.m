// Rosettus Launcher - run, patch and manage PowerPC applications on macOS Mojave.
//
// One window lists the PowerPC programs found in /Applications, ~/Applications and ~/Downloads
// (plus anything dropped on the window or the Dock icon). For the selected item:
//   Open      run it under Rosetta (no Terminal; output goes to ~/Library/Logs/Rosettus/<name>.log)
//   Patch     make the app itself launch through Rosetta on double-click (rosettus_patch.py:
//             the original PowerPC executable is kept and a small Intel stub is added)
//   Unpatch   restore the original executable
//   Launcher  create a launcher app in ~/Applications/Rosettus (for apps that cannot be modified,
//             e.g. read-only volumes or Classic CFM applications)
//   Show Log  show the last run's log in Console
// Top row: Search This Mac, Patch All, and Preference Panes (opens Rosettus Preference Pane Viewer,
// which runs PowerPC .prefPane bundles such as Version Cue's).
// Universal apps (Intel + PowerPC) run natively and are not listed, unless File > Show Universal Apps
// is on or one is added by hand: then Open runs its PowerPC version, and Launcher makes a separate
// "<name> (PowerPC)" launcher; the app itself is never patched.
// Installer packages, screen savers and Dashboard widgets get "Install" (see README).
//
// build: clang -fobjc-arc -O2 -arch x86_64 -mmacosx-version-min=10.13 -framework Cocoa \
//               -o "Rosettus Launcher" main.m
#import <Cocoa/Cocoa.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

typedef NS_ENUM(NSInteger, RLKind) { RLKindMachO, RLKindCFM, RLKindPackage, RLKindSaver, RLKindWidget, RLKindTool };

@interface RLItem : NSObject
@property (copy) NSString *path, *name, *status;
@property RLKind kind;
@property BOOL patched, running, flat;   // flat: single-file Classic Carbon app (no bundle)
@property BOOL universal;                // Intel + PowerPC: runs natively; Open runs its PowerPC slice
@property (strong) NSImage *icon;
@end
@implementation RLItem
@end

static NSString *RLRoot(void) {
    NSString *r = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"RosettusRoot"];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *cands = @[ r ?: @"", [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Rosettus"],
                        @"/Library/Rosettus" ];
    for (NSString *c in cands)
        if (c.length && [fm isExecutableFileAtPath:[c stringByAppendingPathComponent:@"translate_mojave"]]) return c;
    return nil;
}

// the setup kit the installer package leaves in /Library/Rosettus/Setup (install.sh and this project's files)
static NSString *RLSetupScript(void) { return @"/Library/Rosettus/Setup/install.sh"; }
static NSString *RLShellQuote(NSString *s) { return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]]; }
// ---- what kind of program is this? ----
static uint32_t rd32(const uint8_t *p, BOOL be) {
    return be ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}
// 1 = PowerPC only, 2 = CFM/PEF, 3 = Universal (PowerPC and Intel), 0 = Intel only, -1 = not a program / unreadable
static int exeKind(NSString *exe) {
    BOOL dir = NO;
    if (!exe || ![[NSFileManager defaultManager] fileExistsAtPath:exe isDirectory:&dir] || dir) return -1;
    NSFileHandle *h = [NSFileHandle fileHandleForReadingAtPath:exe];
    NSData *d = [h readDataOfLength:4096];
    [h closeFile];
    if (d.length < 8) return -1;
    const uint8_t *b = d.bytes;
    uint32_t m = rd32(b, YES);
    if (m == FAT_MAGIC) {
        uint32_t n = rd32(b + 4, YES), i; BOOL ppc = NO, intel = NO;
        for (i = 0; i < n && 8 + 20 * (i + 1) <= d.length; i++) {
            uint32_t cpu = rd32(b + 8 + 20 * i, YES);
            if (cpu == CPU_TYPE_POWERPC || cpu == CPU_TYPE_POWERPC64) ppc = YES;
            if (cpu == CPU_TYPE_X86 || cpu == CPU_TYPE_X86_64) intel = YES;
        }
        return ppc ? (intel ? 3 : 1) : (intel ? 0 : -1);
    }
    if (m == MH_MAGIC || m == MH_MAGIC_64) {         // big-endian header: PowerPC
        uint32_t cpu = rd32(b + 4, YES);
        return (cpu == CPU_TYPE_POWERPC || cpu == CPU_TYPE_POWERPC64) ? 1 : -1;
    }
    if (rd32(b, NO) == MH_MAGIC || rd32(b, NO) == MH_MAGIC_64) return 0;
    if (!memcmp(b, "Joy!peff", 8)) return 2;
    return -1;
}
static BOOL bundleIsPatched(NSString *app) {
    NSFileManager *fm = [NSFileManager defaultManager];
    return [fm fileExistsAtPath:[app stringByAppendingPathComponent:@"Contents/Resources/rosetta-original-executable"]] ||
           [fm fileExistsAtPath:[app stringByAppendingPathComponent:@"Contents/Resources/rosetta-original-Info.plist"]];   // Carbon CFM app
}
// launcher made by mkwrapper.sh: shared (/Applications/Rosettus) or this user's; a Universal app's is "<name> (PowerPC).app"
static NSString *wrapperForU(NSString *p, BOOL universal) {
    NSString *n = p.lastPathComponent;
    if ([n.pathExtension.lowercaseString isEqualToString:@"app"]) n = n.stringByDeletingPathExtension;
    if (universal) n = [n stringByAppendingString:@" (PowerPC)"];
    n = [n stringByAppendingPathExtension:@"app"];
    NSString *shared = [@"/Applications/Rosettus" stringByAppendingPathComponent:n];
    if ([[NSFileManager defaultManager] fileExistsAtPath:shared]) return shared;
    return [[NSHomeDirectory() stringByAppendingPathComponent:@"Applications/Rosettus"] stringByAppendingPathComponent:n];
}
static NSString *wrapperFor(NSString *p) { return wrapperForU(p, NO); }
static NSString *wrapperOf(RLItem *it) { return wrapperForU(it.path, it.universal); }
static BOOL rlShowUniversal(void) { return [[NSUserDefaults standardUserDefaults] boolForKey:@"RLShowUniversal"]; }
static NSString *universalStatus(NSString *p) {
    return [[NSFileManager defaultManager] fileExistsAtPath:wrapperForU(p, YES)] ? @"Universal - PowerPC launcher in Applications/Rosettus"
                                                                                : @"Universal - Open runs its PowerPC version";
}
static BOOL isFlatCarbonApp(NSString *p) {      // Office 2004 style: one PEF file of type APPL
    NSDictionary *a = [[NSFileManager defaultManager] attributesOfItemAtPath:p error:nil];
    if (![a.fileType isEqualToString:NSFileTypeRegular]) return NO;
    return a.fileHFSTypeCode == 'APPL' || a.fileHFSTypeCode == 'APPC' || a.fileHFSTypeCode == 0;
}
static NSComparisonResult (^rlOrder)(RLItem *, RLItem *) = ^NSComparisonResult(RLItem *a, RLItem *b) {
    BOOL va = [a.path hasPrefix:@"/Volumes/"], vb = [b.path hasPrefix:@"/Volumes/"];   // installer discs last
    if (va != vb) return va ? NSOrderedDescending : NSOrderedAscending;
    return [a.name localizedCaseInsensitiveCompare:b.name];
};
// allowUniversal: list a Universal app too (File > Show Universal Apps, or added by hand)
static RLItem *itemForPathU(NSString *p, BOOL allowUniversal) {
    NSString *ext = p.pathExtension.lowercaseString;
    RLItem *it = [RLItem new];
    it.path = p; it.name = [[NSFileManager defaultManager] displayNameAtPath:p];
    it.icon = [[NSWorkspace sharedWorkspace] iconForFile:p];
    it.icon.size = NSMakeSize(32, 32);
    if ([ext isEqualToString:@"pkg"] || [ext isEqualToString:@"mpkg"]) { it.kind = RLKindPackage; it.status = @"Installer package"; return it; }
    if ([ext isEqualToString:@"saver"]) {
        NSString *exe = [NSBundle bundleWithPath:p].executablePath;
        if (exe && exeKind(exe) != 1) return nil;       // native saver: nothing to do
        it.kind = RLKindSaver; it.status = @"PowerPC screen saver"; return it;
    }
    if ([ext isEqualToString:@"wdgt"]) {   // HTML-only widgets run natively; only a PowerPC plug-in needs Rosetta
        NSDictionary *wi = [NSDictionary dictionaryWithContentsOfFile:[p stringByAppendingPathComponent:@"Info.plist"]];
        NSString *pl = [wi[@"Plugin"] isKindOfClass:[NSString class]] ? wi[@"Plugin"] : nil;
        NSString *exe = pl ? [NSBundle bundleWithPath:[p stringByAppendingPathComponent:pl]].executablePath : nil;
        if (!exe || exeKind(exe) != 1) return nil;
        it.kind = RLKindWidget; it.status = @"Dashboard widget (PowerPC plug-in)"; return it;
    }
    if ([ext isEqualToString:@"app"]) {
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[p stringByAppendingPathComponent:@"Contents/Info.plist"]] ?:
                             [NSDictionary dictionaryWithContentsOfFile:[p stringByAppendingPathComponent:@"Contents/Info-macos.plist"]];
        for (NSString *k in @[ @"LSBackgroundOnly", @"LSUIElement", @"NSUIElement", @"NSBGOnly" ])
            if ([info[k] respondsToSelector:@selector(boolValue)] && [info[k] boolValue]) return nil;   // helpers, daemons
        // the app's own icon: Finder's would carry the "not supported" badge PowerPC apps get on Mojave
        it.icon = [[NSWorkspace sharedWorkspace] iconForFileType:@"com.apple.application-bundle"];
        it.icon.size = NSMakeSize(32, 32);
        NSString *ic = info[@"CFBundleIconFile"];
        if ([ic isKindOfClass:[NSString class]] && ic.length) {
            NSString *f = [[p stringByAppendingPathComponent:@"Contents/Resources"] stringByAppendingPathComponent:ic];
            if (!f.pathExtension.length) f = [f stringByAppendingPathExtension:@"icns"];
            NSImage *im = [[NSImage alloc] initWithContentsOfFile:f];
            if (im) { im.size = NSMakeSize(32, 32); it.icon = im; }
        }
        if (bundleIsPatched(p)) { it.kind = RLKindMachO; it.patched = YES; it.status = @"Patched - opens with a double-click"; return it; }
        NSString *exe = [NSBundle bundleWithPath:p].executablePath;
        if (!exe)   // CFM apps often lack CFBundleExecutable
            exe = [[p stringByAppendingPathComponent:@"Contents/MacOS"] stringByAppendingPathComponent:[[p lastPathComponent] stringByDeletingPathExtension]];
        int r = exeKind(exe);
        if (r == 1) { it.kind = RLKindMachO; it.status = @"PowerPC"; return it; }
        if (r == 2) { it.kind = RLKindCFM; it.status = @"PowerPC (Carbon CFM)"; return it; }
        if (r == 3 && allowUniversal) {
            it.kind = RLKindMachO; it.universal = YES;
            it.patched = [[NSFileManager defaultManager] fileExistsAtPath:wrapperForU(p, YES)];   // has a PowerPC launcher
            it.status = universalStatus(p);
            return it;
        }
        return nil;
    }
    int r = exeKind(p);
    if (r == 2 && isFlatCarbonApp(p)) {
        it.kind = RLKindCFM; it.flat = YES;
        if ([[NSFileManager defaultManager] fileExistsAtPath:wrapperFor(p)]) {
            it.patched = YES; it.status = @"Launcher in ~/Applications/Rosettus";
            NSImage *im = [[NSWorkspace sharedWorkspace] iconForFile:wrapperFor(p)];   // carries the app's own icon
            if (im) { im.size = NSMakeSize(32, 32); it.icon = im; }
        }
        else it.status = @"PowerPC (Classic Carbon, single file)";
        return it;
    }
    if (r == 1 || r == 2) { it.kind = RLKindTool; it.status = @"PowerPC command-line tool"; return it; }
    return nil;
}
static RLItem *itemForPath(NSString *p) { return itemForPathU(p, rlShowUniversal()); }

// ---- running things without Terminal ----
static pid_t spawnLogged(NSArray<NSString *> *argv, NSString *log, NSDictionary *extraEnv) {
    posix_spawn_file_actions_t fa; posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa); posix_spawnattr_init(&at);
    [[NSFileManager defaultManager] createDirectoryAtPath:[log stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment] mutableCopy];
    [env addEntriesFromDictionary:extraEnv ?: @{}];
    NSUInteger n = argv.count, i;
    char **av = calloc(n + 1, sizeof *av), **ev = calloc(env.count + 1, sizeof *ev);
    for (i = 0; i < n; i++) av[i] = strdup(argv[i].fileSystemRepresentation);
    i = 0;
    for (NSString *k in env) ev[i++] = strdup([[NSString stringWithFormat:@"%@=%@", k, env[k]] UTF8String]);
    pid_t pid = 0;
    int e = posix_spawn(&pid, av[0], &fa, &at, av, ev);
    for (i = 0; av[i]; i++) free(av[i]);
    for (i = 0; ev[i]; i++) free(ev[i]);
    free(av); free(ev);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    return e ? -1 : pid;
}
static NSString *runCaptureEnv(NSArray<NSString *> *argv, NSDictionary *extraEnv, int *status) {
    NSTask *t = [NSTask new];
    t.launchPath = argv[0]; t.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    if (extraEnv.count) {
        NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment] mutableCopy];
        [env addEntriesFromDictionary:extraEnv];
        t.environment = env;
    }
    NSPipe *p = [NSPipe pipe]; t.standardOutput = p; t.standardError = p;
    [t launch];
    NSData *d = [p.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    if (status) *status = t.terminationStatus;
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
}
static NSString *runCapture(NSArray<NSString *> *argv, int *status) { return runCaptureEnv(argv, nil, status); }

@interface RLController : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property (strong) NSWindow *win;
@property (strong) NSTableView *table;
@property (strong) NSMutableArray<RLItem *> *items;
@property (strong) NSTextField *info;
@property (strong) NSButton *openB, *patchB, *unpatchB, *launcherB, *logB, *searchB, *patchAllB, *panesB;
@property (strong) NSProgressIndicator *spinner;
@property (strong) NSTextField *searchInfo;
@property BOOL searching;
@property (strong) NSMutableDictionary<NSNumber *, NSString *> *children;   // pid -> path
@property (copy) NSString *root;
@property (strong) NSMutableArray<NSString *> *pendingOpen;
@end

@implementation RLController

- (NSButton *)button:(NSString *)title action:(SEL)a x:(CGFloat)x {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:a];
    b.frame = NSMakeRect(x, 12, 104, 32);
    b.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [self.win.contentView addSubview:b];
    return b;
}

- (void)applicationWillFinishLaunching:(NSNotification *)n {
    self.items = [NSMutableArray array];
    self.children = [NSMutableDictionary dictionary];
    self.pendingOpen = [NSMutableArray array];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    self.root = RLRoot();
    [self buildMenu];
    self.win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 660, 480)
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered defer:NO];
    self.win.title = @"Rosettus Launcher";
    self.win.minSize = NSMakeSize(660, 340);
    self.win.delegate = self;
    NSView *cv = self.win.contentView;

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 84, 636, 344)];
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
    self.table = [[NSTableView alloc] initWithFrame:sv.bounds];
    self.table.rowHeight = 38;
    self.table.usesAlternatingRowBackgroundColors = YES;
    NSTableColumn *c1 = [[NSTableColumn alloc] initWithIdentifier:@"app"]; c1.title = @"Item"; c1.width = 340;
    NSTableColumn *c2 = [[NSTableColumn alloc] initWithIdentifier:@"status"]; c2.title = @"Status"; c2.width = 270;
    [self.table addTableColumn:c1]; [self.table addTableColumn:c2];
    self.table.dataSource = self; self.table.delegate = self;
    self.table.target = self; self.table.doubleAction = @selector(openItem:);
    [self.table registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    sv.documentView = self.table;
    [cv addSubview:sv];

    self.info = [NSTextField labelWithString:@""];
    self.info.frame = NSMakeRect(14, 52, 632, 24);
    self.info.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    self.info.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.info.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:self.info];

    // top row: find PowerPC programs anywhere on this Mac, patch everything found
    self.searchB = [NSButton buttonWithTitle:@"Search This Mac" target:self action:@selector(searchMac:)];
    self.searchB.frame = NSMakeRect(12, 436, 150, 32);
    self.searchB.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    self.searchB.toolTip = @"Find PowerPC applications, screen savers and widgets on all disks (Spotlight plus a folder scan).";
    [cv addSubview:self.searchB];
    self.patchAllB = [NSButton buttonWithTitle:@"Patch All…" target:self action:@selector(patchAll:)];
    self.patchAllB.frame = NSMakeRect(166, 436, 110, 32);
    self.patchAllB.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    self.patchAllB.toolTip = @"Patch every listed app so it opens with a normal double-click.";
    [cv addSubview:self.patchAllB];
    self.panesB = [NSButton buttonWithTitle:@"Preference Panes…" target:self action:@selector(openPaneViewer:)];
    self.panesB.frame = NSMakeRect(280, 436, 150, 32);
    self.panesB.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    self.panesB.toolTip = @"Open Rosettus Preference Pane Viewer to use PowerPC preference panes.";
    [cv addSubview:self.panesB];
    self.spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(438, 444, 16, 16)];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;
    self.spinner.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    [cv addSubview:self.spinner];
    self.searchInfo = [NSTextField labelWithString:@""];
    self.searchInfo.frame = NSMakeRect(460, 442, 188, 20);
    self.searchInfo.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    self.searchInfo.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.searchInfo.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:self.searchInfo];

    self.openB = [self button:@"Open" action:@selector(openItem:) x:12];
    self.openB.keyEquivalent = @"\r";
    self.patchB = [self button:@"Patch" action:@selector(patchItem:) x:118];
    self.unpatchB = [self button:@"Unpatch" action:@selector(unpatchItem:) x:224];
    self.launcherB = [self button:@"Launcher" action:@selector(makeLauncher:) x:330];
    self.logB = [self button:@"Show Log" action:@selector(showLog:) x:436];
    NSButton *add = [self button:@"Add…" action:@selector(addItem:) x:544];
    add.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    self.patchB.toolTip = @"Change the app so a normal double-click runs it through Rosettus (reversible with Unpatch).";
    self.launcherB.toolTip = @"Make a separate launcher app in ~/Applications/Rosettus and leave this app unchanged.";

    [self.win setContentSize:NSMakeSize(720, 600)];   // room for a long list; the user size is remembered
    [self.win center];
    [self.win setFrameAutosaveName:@"RosettaLauncherMain"];
    [self.win makeKeyAndOrderFront:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(tableSel:) name:NSTableViewSelectionDidChangeNotification object:self.table];
    [NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(reap:) userInfo:nil repeats:YES];
    [self rescan:nil];
    if (!self.root) {
        NSAlert *a = [NSAlert new];
        BOOL kit = [[NSFileManager defaultManager] isExecutableFileAtPath:RLSetupScript()];
        a.messageText = kit ? @"Finish installing Rosettus" : @"Rosettus is not installed";
        a.informativeText = kit
            ? @"Rosettus needs the files exported from a Snow Leopard Mac (Rosettus-SL-export.tar.gz, made by export_from_sl.sh). Choose that file to install them for every user on this Mac. You will be asked for an administrator password."
            : @"Run install.sh from the Rosettus folder first, or the Rosettus installer package. It needs the files exported from a Snow Leopard Mac (see README).";
        if (kit) { [a addButtonWithTitle:@"Choose Export…"]; [a addButtonWithTitle:@"Later"]; }
        [a beginSheetModalForWindow:self.win completionHandler:^(NSModalResponse r) {
            if (kit && r == NSAlertFirstButtonReturn) [self importExport:nil];
        }];
    }
    for (NSString *p in self.pendingOpen) [self openPath:p];
    [self.pendingOpen removeAllObjects];
}

- (void)buildMenu {
    NSMenu *mb = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new]; [mb addItem:appItem];
    NSMenu *am = [NSMenu new];
    [am addItemWithTitle:@"About Rosettus Launcher" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [am addItem:[NSMenuItem separatorItem]];
    [am addItemWithTitle:@"Hide Rosettus Launcher" action:@selector(hide:) keyEquivalent:@"h"];
    [am addItemWithTitle:@"Quit Rosettus Launcher" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = am;
    NSMenuItem *fileItem = [NSMenuItem new]; [mb addItem:fileItem];
    NSMenu *fm = [[NSMenu alloc] initWithTitle:@"File"];
    [fm addItemWithTitle:@"Add…" action:@selector(addItem:) keyEquivalent:@"o"];
    [fm addItemWithTitle:@"Rescan" action:@selector(rescan:) keyEquivalent:@"r"];
    [fm addItemWithTitle:@"Search This Mac" action:@selector(searchMac:) keyEquivalent:@"f"];
    [fm addItemWithTitle:@"Patch All…" action:@selector(patchAll:) keyEquivalent:@""];
    [fm addItemWithTitle:@"Preference Panes…" action:@selector(openPaneViewer:) keyEquivalent:@""];
    NSMenuItem *ap = [fm addItemWithTitle:@"Patch New PowerPC Apps Automatically" action:@selector(toggleAutoPatch:) keyEquivalent:@""];
    ap.state = [self autoPatchOn] ? NSControlStateValueOn : NSControlStateValueOff;
    [fm addItemWithTitle:@"Install Snow Leopard Files…" action:@selector(importExport:) keyEquivalent:@""];
    NSMenuItem *su = [fm addItemWithTitle:@"Show Universal Apps (Run Their PowerPC Version)" action:@selector(toggleUniversal:) keyEquivalent:@""];
    su.state = rlShowUniversal() ? NSControlStateValueOn : NSControlStateValueOff;
    NSMenuItem *rt = [fm addItemWithTitle:@"Sharp Text on Retina Displays (Experimental)" action:@selector(toggleRetina:) keyEquivalent:@""];
    rt.state = [[self rosettusDefaults] boolForKey:@"Retina"] ? NSControlStateValueOn : NSControlStateValueOff;
    [fm addItem:[NSMenuItem separatorItem]];
    [fm addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    fileItem.submenu = fm;
    NSMenuItem *winItem = [NSMenuItem new]; [mb addItem:winItem];
    NSMenu *wm = [[NSMenu alloc] initWithTitle:@"Window"];
    [wm addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    winItem.submenu = wm;
    [NSApp setWindowsMenu:wm];
    [NSApp setMainMenu:mb];
}

// ---- list ----
- (RLItem *)addPath:(NSString *)p {
    for (RLItem *i in self.items) if ([i.path isEqualToString:p]) return i;
    RLItem *it = itemForPathU(p, YES);      // added by hand: a Universal app is listed too
    if (it) [self.items addObject:it];
    return it;
}
- (void)rescan:(id)sender {
    NSMutableArray *keep = [NSMutableArray array];
    for (RLItem *i in self.items) if ([[NSFileManager defaultManager] fileExistsAtPath:i.path]) [keep addObject:i.path];
    [self.items removeAllObjects];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *dirs = @[ @"/Applications", [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"],
                       [NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"],
                       [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Screen Savers"], @"/Library/Screen Savers",
                       [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Widgets"], @"/Library/Widgets" ];
    NSSet *exts = [NSSet setWithArray:@[ @"app", @"pkg", @"mpkg", @"saver", @"wdgt" ]];
    for (NSString *d in dirs) {
        NSDirectoryEnumerator *en = [fm enumeratorAtURL:[NSURL fileURLWithPath:d] includingPropertiesForKeys:nil
                                                options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil];
        for (NSURL *u in en) {
            NSString *ext = u.pathExtension.lowercaseString;
            if ([exts containsObject:ext]) {
                [en skipDescendants];
                if ([u.path containsString:@"/Applications/Rosettus/"]) continue;      // our own launcher apps
                BOOL inDownloads = [d hasSuffix:@"Downloads"];
                if (([ext isEqualToString:@"pkg"] || [ext isEqualToString:@"mpkg"]) && !inDownloads) continue;
                RLItem *it = itemForPath(u.path);
                if (it) [self.items addObject:it];
            } else if (!ext.length && [fm isExecutableFileAtPath:u.path] && exeKind(u.path) == 2) {
                RLItem *it = itemForPath(u.path);          // single-file Carbon app
                if (it && it.kind == RLKindCFM) [self.items addObject:it];
            } else if (en.level >= 3) [en skipDescendants];
        }
    }
    for (NSString *p in keep) [self addPath:p];
    // items found by an earlier Search This Mac (still present, disk mounted)
    for (NSString *p in [[NSUserDefaults standardUserDefaults] stringArrayForKey:@"RLFoundItems"]) {
        BOOL have = NO;
        for (RLItem *i in self.items) if ([i.path isEqualToString:p]) { have = YES; break; }
        if (!have && [fm fileExistsAtPath:p]) { RLItem *it = itemForPath(p); if (it) [self.items addObject:it]; }
    }
    [self.items sortUsingComparator:rlOrder];
    for (NSString *p in self.children.allValues) for (RLItem *i in self.items) if ([i.path isEqualToString:p]) i.running = YES;
    [self.table reloadData];
    [self updateButtons];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return self.items.count; }
- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    RLItem *it = self.items[row];
    NSTableCellView *v = [tv makeViewWithIdentifier:col.identifier owner:self];
    if (!v) {
        v = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, col.width, 38)];
        v.identifier = col.identifier;
        NSTextField *t = [NSTextField labelWithString:@""];
        t.lineBreakMode = NSLineBreakByTruncatingTail;
        if ([col.identifier isEqualToString:@"app"]) {
            NSImageView *iv = [[NSImageView alloc] initWithFrame:NSMakeRect(4, 3, 32, 32)];
            [v addSubview:iv]; v.imageView = iv;
            t.frame = NSMakeRect(42, 10, col.width - 46, 18);
        } else t.frame = NSMakeRect(4, 10, col.width - 8, 18);
        t.autoresizingMask = NSViewWidthSizable;
        [v addSubview:t]; v.textField = t;
    }
    if ([col.identifier isEqualToString:@"app"]) {
        NSString *shown = it.name;
        NSArray *pc = it.path.pathComponents;           // show which disk an item lives on, so copies on several disks are clear
        if (pc.count > 3 && [pc[1] isEqualToString:@"Volumes"]) shown = [NSString stringWithFormat:@"%@  \u2014 %@", it.name, pc[2]];
        v.textField.stringValue = shown; v.imageView.image = it.icon; v.toolTip = it.path;
    }
    else v.textField.stringValue = it.running ? [it.status stringByAppendingString:@" - running"] : it.status;
    return v;
}
- (NSDragOperation)tableView:(NSTableView *)tv validateDrop:(id<NSDraggingInfo>)info proposedRow:(NSInteger)row proposedDropOperation:(NSTableViewDropOperation)op {
    [tv setDropRow:-1 dropOperation:NSTableViewDropOn];
    return NSDragOperationCopy;
}
- (BOOL)tableView:(NSTableView *)tv acceptDrop:(id<NSDraggingInfo>)info row:(NSInteger)row dropOperation:(NSTableViewDropOperation)op {
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    RLItem *last = nil;
    for (NSURL *u in urls) { RLItem *i = [self addPath:u.path]; if (i) last = i; }
    [tv reloadData];
    if (last) [tv selectRowIndexes:[NSIndexSet indexSetWithIndex:[self.items indexOfObject:last]] byExtendingSelection:NO];
    else if (urls.count) self.info.stringValue = @"Not a PowerPC program (it may already run natively).";
    return YES;
}
- (void)tableSel:(NSNotification *)n { [self updateButtons]; }
- (RLItem *)sel { NSInteger r = self.table.selectedRow; return r >= 0 && r < (NSInteger)self.items.count ? self.items[r] : nil; }
- (void)updateButtons {
    RLItem *it = [self sel];
    BOOL ok = it && self.root;
    self.openB.enabled = ok;
    self.openB.title = (it.kind == RLKindPackage || it.kind == RLKindSaver || it.kind == RLKindWidget) ? @"Install" : @"Open";
    self.patchB.enabled = ok && (it.kind == RLKindMachO || it.kind == RLKindCFM) && !it.patched && !it.universal;
    self.patchAllB.enabled = self.root && !self.searching;
    self.searchB.enabled = !self.searching;
    self.unpatchB.enabled = ok && it.patched;
    self.unpatchB.title = it.universal ? @"Remove" : @"Unpatch";
    self.unpatchB.toolTip = it.universal ? @"Move this app's PowerPC launcher to the Trash (the app itself was never changed)." : nil;
    self.launcherB.toolTip = it.universal ? @"Make a \u201c(PowerPC)\u201d launcher in Applications/Rosettus that always runs this Universal app's PowerPC version. The app itself and what opens its documents stay as they are."
                                          : @"Make a separate launcher app in ~/Applications/Rosettus and leave this app unchanged.";
    self.launcherB.enabled = ok && (it.kind == RLKindMachO || it.kind == RLKindCFM) && !it.patched && !it.flat;
    self.logB.enabled = it != nil;
    if (it) self.info.stringValue = it.path;
    else self.info.stringValue = [NSString stringWithFormat:@"%lu item(s) - drop more on this window (Universal apps too).   Rosettus: %@",
                                  (unsigned long)self.items.count, self.root ?: @"not installed"];
}

// ---- actions ----
- (NSString *)logFor:(RLItem *)it {   // a Universal app's PowerPC runs log as "<name> (PowerPC).log", like its launcher
    NSString *n = it.name;            // only ".app" goes ("Build-a-lot 3 v1.1" keeps its ".1", as mkwrapper.sh names the log)
    if ([n.pathExtension.lowercaseString isEqualToString:@"app"]) n = n.stringByDeletingPathExtension;
    if (it.universal) n = [n stringByAppendingString:@" (PowerPC)"];
    return [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Rosettus"]
            stringByAppendingPathComponent:[n stringByAppendingPathExtension:@"log"]];
}
- (void)fail:(NSString *)msg detail:(NSString *)d {
    NSAlert *a = [NSAlert new]; a.messageText = msg; a.informativeText = d ?: @"";
    [a beginSheetModalForWindow:self.win completionHandler:nil];
}
- (void)openPath:(NSString *)p {
    RLItem *it = [self addPath:p];
    if (!it) { [[NSWorkspace sharedWorkspace] openFile:p]; return; }   // native only: let the system open it
    [self.table reloadData];
    [self launchItem:it];
}
- (void)launchItem:(RLItem *)it {
    if (!self.root) return;
    if (it.patched) { [[NSWorkspace sharedWorkspace] launchApplication:(it.flat || it.universal) ? wrapperOf(it) : it.path]; return; }
    NSArray *argv;
    NSString *tools = [self.root stringByAppendingPathComponent:@"tools"];
    switch (it.kind) {
        case RLKindPackage: argv = @[ @"/bin/bash", [tools stringByAppendingPathComponent:@"ppc_pkg_install.sh"], it.path ]; break;
        case RLKindSaver:   argv = @[ @"/usr/bin/python", [tools stringByAppendingPathComponent:@"install_saver.py"], it.path ]; break;
        case RLKindWidget:  argv = @[ @"/usr/bin/python", [tools stringByAppendingPathComponent:@"install_widget.py"], it.path ]; break;
        default:            argv = @[ [self.root stringByAppendingPathComponent:@"rosettus"], it.path ];
    }
    pid_t pid = spawnLogged(argv, [self logFor:it], @{ @"OAH_LAUNCHED_BY": @"RosettaLauncher" });
    if (pid <= 0) { [self fail:@"Could not start Rosettus" detail:argv[0]]; return; }
    self.children[@(pid)] = it.path;
    it.running = YES;
    [self.table reloadData];
}
- (void)openItem:(id)sender { RLItem *it = [self sel]; if (it) [self launchItem:it]; }
- (void)reap:(NSTimer *)t {
    BOOL changed = NO;
    for (NSNumber *k in self.children.allKeys) {
        int st; pid_t r = waitpid(k.intValue, &st, WNOHANG);
        if (r == k.intValue || (r < 0 && errno == ECHILD)) {
            NSString *p = self.children[k]; [self.children removeObjectForKey:k];
            killpg(k.intValue, SIGTERM);          // anything the app left running in its process group
            for (RLItem *i in self.items) if ([i.path isEqualToString:p]) {
                i.running = NO;
                if (i.kind == RLKindSaver || i.kind == RLKindWidget || i.kind == RLKindPackage) {
                    NSString *log = [NSString stringWithContentsOfFile:[self logFor:i] encoding:NSUTF8StringEncoding error:nil];
                    NSArray *lines = [log componentsSeparatedByString:@"\n"];
                    NSString *lastLine = @"";
                    for (NSString *l in lines.reverseObjectEnumerator) if (l.length) { lastLine = l; break; }
                    [self fail:(WIFEXITED(st) && WEXITSTATUS(st) == 0) ? @"Installed" : @"Install failed" detail:lastLine];
                }
            }
            changed = YES;
        }
    }
    if (changed) { [self.table reloadData]; if (!self.children.count && !self.win.isVisible) [NSApp terminate:nil]; }
}
- (NSString *)patchCommandOutput:(RLItem *)it status:(int *)st {
    if (it.flat)   // a single file can't carry a launcher inside it: make one in ~/Applications/Rosettus
        return runCapture(@[ @"/bin/bash", [self.root stringByAppendingPathComponent:@"mkwrapper.sh"], it.path ], st);
    return runCapture(@[ @"/usr/bin/python", [self.root stringByAppendingPathComponent:@"rosettus_patch.py"], it.path ], st);
}
- (NSString *)patchedStatus:(RLItem *)it { return it.flat ? @"Launcher in ~/Applications/Rosettus" : @"Patched - opens with a double-click"; }
- (void)patchItem:(id)sender {
    RLItem *it = [self sel]; if (!it || it.universal) return;   // a Universal app is never changed: use Launcher
    int st = 0;
    NSString *out = [self patchCommandOutput:it status:&st];
    if (st) {
        [self fail:@"The app could not be patched" detail:[out stringByAppendingString:@"\nUse Launcher instead: it makes a separate launcher app and leaves this one unchanged."]];
        return;
    }
    it.patched = YES; it.status = [self patchedStatus:it];
    [self.table reloadData]; [self updateButtons];
}
- (void)unpatchItem:(id)sender {
    RLItem *it = [self sel]; if (!it) return;
    if (it.universal) {
        [[NSWorkspace sharedWorkspace] recycleURLs:@[ [NSURL fileURLWithPath:wrapperOf(it)] ] completionHandler:^(NSDictionary *m, NSError *e) {
            dispatch_async(dispatch_get_main_queue(), ^{
                it.patched = [[NSFileManager defaultManager] fileExistsAtPath:wrapperOf(it)]; it.status = universalStatus(it.path);
                [self.table reloadData]; [self updateButtons];
            });
        }];
        return;
    }
    if (it.flat) {
        [[NSWorkspace sharedWorkspace] recycleURLs:@[ [NSURL fileURLWithPath:wrapperFor(it.path)] ] completionHandler:nil];
        it.patched = NO; it.status = @"PowerPC (Classic Carbon, single file)";
        [self.table reloadData]; [self updateButtons]; return;
    }
    int st = 0;
    NSString *out = runCapture(@[ @"/usr/bin/python", [self.root stringByAppendingPathComponent:@"rosettus_patch.py"], @"--unpatch", it.path ], &st);
    if (st) { [self fail:@"The app could not be restored" detail:out]; return; }
    it.patched = NO; it.status = it.kind == RLKindCFM ? @"PowerPC (Carbon CFM)" : @"PowerPC";
    [self.table reloadData]; [self updateButtons];
}
- (void)makeLauncher:(id)sender {
    RLItem *it = [self sel]; if (!it) return;
    int st = 0;
    NSString *out = runCaptureEnv(@[ @"/bin/bash", [self.root stringByAppendingPathComponent:@"mkwrapper.sh"], it.path ],
                                  it.universal ? @{ @"RL_PPC_COPY": @"1" } : nil, &st);
    if (st) { [self fail:@"Could not make a launcher" detail:out]; return; }
    NSString *w = wrapperOf(it);
    if (it.universal) { it.patched = YES; it.status = universalStatus(it.path); [self.table reloadData]; [self updateButtons]; }
    [[NSWorkspace sharedWorkspace] selectFile:w inFileViewerRootedAtPath:@""];
}
// ---- Preference Pane Viewer ----
- (NSString *)paneViewerPath {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *name = @"Rosettus Preference Pane Viewer.app";
    NSMutableArray *c = [NSMutableArray arrayWithObjects:[@"/Applications" stringByAppendingPathComponent:name],
                         [[NSHomeDirectory() stringByAppendingPathComponent:@"Applications"] stringByAppendingPathComponent:name], nil];
    NSString *bin = [[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] stringByAppendingPathComponent:name];
    [c addObject:bin];   /* next to the Launcher (the repository's bin/); installed copies first */
    for (NSString *p in c) if ([fm fileExistsAtPath:p]) return p;
    NSURL *u = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"io.github.rosettus.prefpaneviewer"];
    if (u && [fm fileExistsAtPath:u.path]) return u.path;   /* anywhere else LaunchServices knows */
    return nil;
}
- (void)openPaneViewer:(id)sender {
    NSString *p = [self paneViewerPath];
    if (!p) { [self fail:@"Rosettus Preference Pane Viewer is not installed"
                  detail:@"It is installed with Rosettus (install.sh or the installer package) in /Applications."]; return; }
    NSError *e = nil;
    if (![[NSWorkspace sharedWorkspace] launchApplicationAtURL:[NSURL fileURLWithPath:p] options:NSWorkspaceLaunchDefault
                                                 configuration:@{} error:&e])
        [self fail:@"Could not open Rosettus Preference Pane Viewer" detail:e.localizedDescription ?: p];
}
- (void)showLog:(id)sender {
    RLItem *it = [self sel]; if (!it) return;
    NSString *l = [self logFor:it];
    if (![[NSFileManager defaultManager] fileExistsAtPath:l]) { [self fail:@"No log yet" detail:@"Open the item once; its output is kept in ~/Library/Logs/Rosettus."]; return; }
    [[NSWorkspace sharedWorkspace] openFile:l withApplication:@"Console"];
}
// ---- Import the Snow Leopard export (shared install from the installer package) ----
- (void)importExport:(id)sender {
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:RLSetupScript()]) {
        [self fail:@"No setup kit" detail:@"Install the Rosettus package first, or run install.sh from the repository folder."]; return; }
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.message = @"Choose the Snow Leopard export (Rosettus-SL-export.tar.gz)";
    op.allowedFileTypes = @[ @"gz", @"tgz" ];
    op.directoryURL = [NSURL fileURLWithPath:[NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"]];
    [op beginSheetModalForWindow:self.win completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !op.URL) return;
        NSString *exp = op.URL.path;
        NSString *log = @"/tmp/RosettusImport.log";
        NSString *cmd = [NSString stringWithFormat:@"%@ %@ --system --no-apps > %@ 2>&1", RLShellQuote(RLSetupScript()), RLShellQuote(exp), log];
        NSString *as = [NSString stringWithFormat:@"do shell script \"%@\" with prompt \"Rosettus Launcher installs the Snow Leopard files for every user.\" with administrator privileges",
                        [[cmd stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"] stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""]];
        self.searching = YES; [self updateButtons];
        [self.spinner startAnimation:nil];
        self.searchInfo.stringValue = @"Installing the Snow Leopard files (a few minutes)…";
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int st = 0;
            NSString *out = runCapture(@[ @"/usr/bin/osascript", @"-e", as ], &st);
            NSString *tail = [[NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil] ?: @"" copy];
            dispatch_async(dispatch_get_main_queue(), ^{
                self.searching = NO; [self.spinner stopAnimation:nil];
                self.root = RLRoot();
                if (st || !self.root) {
                    self.searchInfo.stringValue = @"Installation did not finish.";
                    NSArray *ls = [tail componentsSeparatedByString:@"\n"];
                    NSString *last = ls.count > 8 ? [[ls subarrayWithRange:NSMakeRange(ls.count - 8, 8)] componentsJoinedByString:@"\n"] : tail;
                    [self fail:@"Rosettus could not be installed" detail:[NSString stringWithFormat:@"%@\n%@", out, last]];
                } else {
                    self.searchInfo.stringValue = [NSString stringWithFormat:@"Rosettus installed in %@ for every user.", self.root];
                    [self rescan:nil];
                }
                [self updateButtons];
            });
        });
    }];
}
// ---- Search This Mac ----
static BOOL rlSkipDir(NSString *path, NSString *root) {
    NSString *h = NSHomeDirectory();
    if (root && ([path isEqualToString:root] || [path hasPrefix:[root stringByAppendingString:@"/"]])) return YES;   // the guest tree
    NSString *last = path.lastPathComponent;
    if ([last hasPrefix:@"."]) return YES;
    // bundle internals and data stores never hold apps worth listing
    static NSSet *skipExt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ skipExt = [NSSet setWithArray:@[ @"framework", @"bundle", @"lproj", @"plugin", @"kext", @"xpc", @"component",
        @"appex", @"photoslibrary", @"photolibrary", @"musiclibrary", @"tvlibrary", @"noindex", @"xcarchive", @"sdk", @"platform",
        @"dSYM", @"nib", @"xib", @"pbproj", @"xcodeproj", @"docset", @"git", @"svn", @"rtfd", @"localized" ]]; });
    if ([skipExt containsObject:path.pathExtension]) return YES;
    if ([last isEqualToString:@"node_modules"] || [last isEqualToString:@"site-packages"]) return YES;
    // this project's own checkout (the importer app in import/sl must stay unmodified)
    if ([last isEqualToString:@"import"] && [[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"sl/export_from_sl.sh"]]) return YES;
    for (NSString *x in @[ [h stringByAppendingPathComponent:@"Library"], [h stringByAppendingPathComponent:@".Trash"],
                           [h stringByAppendingPathComponent:@"Applications/Rosettus"], @"/Applications/Rosettus", @"/Library/Rosettus", @"/Volumes/Recovery" ])
        if ([path isEqualToString:x] || [path hasPrefix:[x stringByAppendingString:@"/"]]) return YES;
    // system folders, on the boot disk and on every other mounted disk
    NSString *rel = path;
    if ([path hasPrefix:@"/Volumes/"]) {
        NSArray *c = path.pathComponents;               // "/", "Volumes", name, ...
        if (c.count <= 3) return NO;
        rel = [@"/" stringByAppendingString:[[c subarrayWithRange:NSMakeRange(3, c.count - 3)] componentsJoinedByString:@"/"]];
    }
    static NSSet *sysTop;
    static dispatch_once_t once2;
    dispatch_once(&once2, ^{ sysTop = [NSSet setWithArray:@[ @"System", @"private", @"usr", @"bin", @"sbin", @"dev", @"cores", @"opt",
        @"etc", @"var", @"tmp", @"Network", @".vol", @"Volumes", @"Library", @"sw", @"Developer" ]]; });
    NSArray *rc = rel.pathComponents;                   // "/", top, ...
    if (rc.count >= 2 && [sysTop containsObject:rc[1]]) {
        // /Library keeps the two places screen savers and widgets live
        if ([rc[1] isEqualToString:@"Library"]) {
            if (rc.count == 2) return NO;
            return !([rc[2] isEqualToString:@"Screen Savers"] || [rc[2] isEqualToString:@"Widgets"]);
        }
        return YES;
    }
    if (rc.count >= 4 && [rc[1] isEqualToString:@"Users"] && [rc[3] isEqualToString:@"Library"]) return YES;
    return NO;
}
- (void)searchMac:(id)sender {
    if (self.searching) return;
    self.searching = YES;
    [self.spinner startAnimation:nil];
    self.searchInfo.stringValue = @"Searching…";
    [self updateButtons];
    NSString *root = self.root;
    NSUInteger before = self.items.count;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSMutableOrderedSet *cands = [NSMutableOrderedSet orderedSet];
        __block NSUInteger checked = 0;
        // 1. Spotlight knows the architectures of indexed applications and bundles
        NSTask *t = [NSTask new];
        t.launchPath = @"/usr/bin/mdfind";
        t.arguments = @[ @"kMDItemExecutableArchitectures == \"ppc*\" || kMDItemContentType == \"com.apple.application-file\"" ];
        NSPipe *pipe = [NSPipe pipe]; t.standardOutput = pipe; t.standardError = [NSFileHandle fileHandleWithNullDevice];
        @try { [t launch]; } @catch (NSException *e) { t = nil; }
        if (t) {
            NSData *d = [pipe.fileHandleForReading readDataToEndOfFile];
            [t waitUntilExit];
            for (NSString *l in [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] componentsSeparatedByString:@"\n"]) {
                NSString *ext = l.pathExtension.lowercaseString;
                if (([@[ @"app", @"saver", @"wdgt" ] containsObject:ext] || (l.length && !ext.length)) && !rlSkipDir(l, root)) [cands addObject:l];
            }
        }
        // 2. folder scan: finds Classic Carbon (CFM) apps and unindexed disks too
        NSMutableArray *roots = [NSMutableArray arrayWithObjects:@"/Applications", NSHomeDirectory(), nil];
        for (NSString *v in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/Volumes" error:nil]) {
            NSString *vp = [@"/Volumes" stringByAppendingPathComponent:v];
            if ([[[NSFileManager defaultManager] destinationOfSymbolicLinkAtPath:vp error:nil] isEqualToString:@"/"]) continue;
            // network shares are slow to walk and are not where apps are installed; add them with Add… if needed
            NSNumber *local = nil;
            [[NSURL fileURLWithPath:vp] getResourceValue:&local forKey:NSURLVolumeIsLocalKey error:nil];
            if (local && !local.boolValue) continue;
            [roots addObject:vp];
        }
        NSSet *exts = [NSSet setWithArray:@[ @"app", @"saver", @"wdgt", @"pkg", @"mpkg" ]];
        for (NSString *r in roots) {
            NSDirectoryEnumerator *en = [[NSFileManager defaultManager] enumeratorAtURL:[NSURL fileURLWithPath:r]
                includingPropertiesForKeys:@[ NSURLIsDirectoryKey, NSURLIsRegularFileKey, NSURLIsExecutableKey, NSURLFileSizeKey ] options:0
                errorHandler:^BOOL(NSURL *u, NSError *e) { return YES; }];
            for (NSURL *u in en) {
                NSString *pth = u.path;
                if (rlSkipDir(pth, root) || en.level > 9) { [en skipDescendants]; continue; }
                NSString *ext = pth.pathExtension.lowercaseString;
                if (!ext.length) {   // single-file Carbon apps (Office 2004): executable, sizeable, PEF inside
                    NSNumber *reg = nil, *x = nil, *sz = nil;
                    [u getResourceValue:&reg forKey:NSURLIsRegularFileKey error:nil];
                    if (reg.boolValue) {
                        [u getResourceValue:&x forKey:NSURLIsExecutableKey error:nil];
                        [u getResourceValue:&sz forKey:NSURLFileSizeKey error:nil];
                        if (x.boolValue && sz.longLongValue > 65536 && exeKind(pth) == 2) [cands addObject:pth];
                    }
                }
                if ([exts containsObject:ext]) {
                    [en skipDescendants];
                    BOOL pkg = [ext isEqualToString:@"pkg"] || [ext isEqualToString:@"mpkg"];
                    if (!pkg) [cands addObject:pth];
                }
                if ((++checked % 2000) == 0)
                    dispatch_async(dispatch_get_main_queue(), ^{ self.searchInfo.stringValue = [NSString stringWithFormat:@"Searching… %@", pth.stringByDeletingLastPathComponent]; });
            }
        }
        // 3. keep the PowerPC ones
        NSMutableArray *found = [NSMutableArray array];
        for (NSString *c in cands) { RLItem *it = itemForPath(c); if (it && it.kind != RLKindTool) [found addObject:it]; }
        dispatch_async(dispatch_get_main_queue(), ^{
            NSMutableArray *remember = [NSMutableArray array];
            for (RLItem *it in found) [remember addObject:it.path];
            [[NSUserDefaults standardUserDefaults] setObject:remember forKey:@"RLFoundItems"];
            for (RLItem *it in found) {
                BOOL have = NO;
                for (RLItem *i in self.items) if ([i.path isEqualToString:it.path]) { have = YES; break; }
                if (!have) [self.items addObject:it];
            }
            [self.items sortUsingComparator:rlOrder];
            self.searching = NO;
            [self.spinner stopAnimation:nil];
            self.searchInfo.stringValue = [NSString stringWithFormat:rlShowUniversal() ? @"Found %lu PowerPC or Universal item(s) on this Mac (%lu new)." : @"Found %lu PowerPC item(s) on this Mac (%lu new).",
                                           (unsigned long)found.count, (unsigned long)(self.items.count - before)];
            [self.table reloadData];
            [self updateButtons];
        });
    });
}
// ---- Patch All ----
// ---- automatic patching: the com.rosettus.autopatch agent (tools/autopatch.py) for this account ----
- (NSString *)autoPatchPlist {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents/com.rosettus.autopatch.plist"];
}
- (BOOL)autoPatchOn { return [[NSFileManager defaultManager] fileExistsAtPath:[self autoPatchPlist]]; }
// ---- Retina: PowerPC windows at 2x (the rosettus script reads com.rosettus Retina) ----
- (NSUserDefaults *)rosettusDefaults { return [[NSUserDefaults alloc] initWithSuiteName:@"com.rosettus"]; }
- (void)toggleRetina:(NSMenuItem *)sender {
    NSUserDefaults *d = [self rosettusDefaults];
    BOOL on = ![d boolForKey:@"Retina"];
    [d setBool:on forKey:@"Retina"];
    [d synchronize];
    sender.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    NSAlert *a = [NSAlert new];
    a.messageText = on ? @"PowerPC apps will draw their windows at Retina resolution" : @"PowerPC apps will draw their windows at standard resolution";
    a.informativeText = on ? @"Text and graphics in PowerPC app windows become as sharp as in native apps. This takes effect the next time each app opens. It is experimental: if an app looks wrong or stops, turn it off again here." : @"This takes effect the next time each app opens.";
    [a runModal];
}
- (void)toggleUniversal:(NSMenuItem *)sender {
    BOOL on = !rlShowUniversal();
    [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"RLShowUniversal"];
    sender.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    if (!on) {   // hide them again (one added by hand comes back when added again)
        NSMutableArray *keep = [NSMutableArray array];
        for (RLItem *i in self.items) if (!i.universal || i.running) [keep addObject:i];
        [self.items setArray:keep];
    }
    [self rescan:nil];
    if (on) {
        NSAlert *a = [NSAlert new];
        a.messageText = @"Universal apps are listed too";
        a.informativeText = @"Apps with both Intel and PowerPC code normally run natively. For one listed here, Open runs its PowerPC version through Rosettus instead (useful when the Intel version misbehaves or a plug-in is PowerPC-only), and Launcher makes a separate \u201c(PowerPC)\u201d app for that in Applications/Rosettus. The app itself is never changed, and Finder keeps opening it and its documents natively.";
        [a beginSheetModalForWindow:self.win completionHandler:nil];
    }
}
- (void)toggleAutoPatch:(NSMenuItem *)sender {
    NSString *pl = [self autoPatchPlist];
    int st = 0;
    if ([self autoPatchOn]) {
        runCapture(@[ @"/bin/launchctl", @"unload", pl ], &st);
        [[NSFileManager defaultManager] removeItemAtPath:pl error:nil];
        sender.state = NSControlStateValueOff;
        return;
    }
    if (!self.root) { NSBeep(); return; }
    NSString *tool = [self.root stringByAppendingPathComponent:@"tools/autopatch.py"];
    NSDictionary *d = @{ @"Label": @"com.rosettus.autopatch",
                         @"ProgramArguments": @[ @"/usr/bin/python", tool ],
                         @"WatchPaths": @[ @"/Applications", [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"] ],
                         @"StartInterval": @300, @"RunAtLoad": @YES, @"ThrottleInterval": @15, @"LowPriorityIO": @YES, @"Nice": @10 };
    [[NSFileManager defaultManager] createDirectoryAtPath:[pl stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    [d writeToFile:pl atomically:YES];
    runCapture(@[ @"/bin/launchctl", @"load", pl ], &st);
    sender.state = NSControlStateValueOn;
    NSAlert *a = [NSAlert new];
    a.messageText = @"New PowerPC apps will be patched automatically";
    a.informativeText = @"When a PowerPC-only app appears in /Applications or ~/Applications (for example copied from a disk image), Rosettus patches it so it opens with a double-click. Apps you cannot change and Universal apps are left alone. Unpatch restores an app.";
    [a runModal];
}

- (void)patchAll:(id)sender {
    NSMutableArray *todo = [NSMutableArray array];
    NSUInteger readOnly = 0;
    for (RLItem *i in self.items) if ((i.kind == RLKindMachO || i.kind == RLKindCFM) && !i.patched && !i.universal) {
        NSNumber *ro = nil;                              // installer discs and disk images: nothing to change there
        [[NSURL fileURLWithPath:i.path] getResourceValue:&ro forKey:NSURLVolumeIsReadOnlyKey error:nil];
        if (!i.flat && (ro.boolValue || ![[NSFileManager defaultManager] isWritableFileAtPath:[i.path stringByAppendingPathComponent:@"Contents"]])) { readOnly++; continue; }
        [todo addObject:i];
    }
    if (!todo.count) { [self fail:@"Nothing to patch" detail:readOnly ? [NSString stringWithFormat:@"Every listed app is patched already, or is on a read-only disk (%lu). Use Open or Launcher for those.", (unsigned long)readOnly]
                                                                       : @"Every listed app is patched already. Use Search This Mac to find more."]; return; }
    NSAlert *a = [NSAlert new];
    a.messageText = readOnly ? [NSString stringWithFormat:@"Patch %lu app(s)? (%lu on read-only disks are left as they are)", (unsigned long)todo.count, (unsigned long)readOnly]
                             : [NSString stringWithFormat:@"Patch %lu app(s)?", (unsigned long)todo.count];
    a.informativeText = @"Each app is changed so a normal double-click (Finder, Dock, Spotlight, Open With) runs it through Rosettus. The original is kept inside the app; Unpatch restores it. Apps on read-only disks are skipped (use Launcher for those).";
    [a addButtonWithTitle:@"Patch"]; [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:self.win completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn) return;
        NSMutableArray *failed = [NSMutableArray array]; NSUInteger ok = 0;
        for (RLItem *it in todo) {
            int st = 0;
            NSString *o = [self patchCommandOutput:it status:&st];
            if (st) {
                NSString *why = [[o stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
                                 componentsSeparatedByString:@"\n"].lastObject ?: @"";
                if ([why hasPrefix:it.path]) why = [why substringFromIndex:it.path.length];
                why = [why stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@": "]];
                [failed addObject:[NSString stringWithFormat:@"%@ \u2014 %@", it.path.stringByAbbreviatingWithTildeInPath, why.length ? why : @"failed"]];
                NSLog(@"patch %@ failed (%d): %@", it.path, st, o);
            }
            else { it.patched = YES; it.status = [self patchedStatus:it]; ok++; }
        }
        [self.table reloadData]; [self updateButtons];
        self.searchInfo.stringValue = [NSString stringWithFormat:@"Patched %lu app(s)%@.", (unsigned long)ok,
                                       failed.count ? [NSString stringWithFormat:@", %lu could not be changed", (unsigned long)failed.count] : @""];
        if (failed.count) [self fail:@"Some apps could not be patched" detail:[[failed componentsJoinedByString:@"\n"]
            stringByAppendingString:@"\n\nThey are probably on a read-only disk or owned by another user. Select one and use Launcher instead."]];
    }];
}
- (void)addItem:(id)sender {
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.allowsMultipleSelection = YES; op.canChooseDirectories = NO;
    op.message = @"Choose PowerPC applications, installer packages, screen savers or widgets";
    [op beginSheetModalForWindow:self.win completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        RLItem *last = nil;
        for (NSURL *u in op.URLs) { RLItem *i = [self addPath:u.path]; if (i) last = i; }
        [self.table reloadData];
        if (last) [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:[self.items indexOfObject:last]] byExtendingSelection:NO];
        else self.info.stringValue = @"Nothing PowerPC-only in that selection.";
    }];
}
// Files dropped on the Dock icon / "Open With": run them right away.
- (void)application:(NSApplication *)app openFiles:(NSArray<NSString *> *)files {
    if (!self.win) [self.pendingOpen addObjectsFromArray:files];
    else for (NSString *f in files) [self openPath:f];
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}
// Closing the window keeps the launcher alive only while something it started still runs.
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return self.children.count == 0; }
- (BOOL)applicationShouldHandleReopen:(NSApplication *)a hasVisibleWindows:(BOOL)v { [self.win makeKeyAndOrderFront:nil]; return YES; }
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        RLController *c = [RLController new];
        app.delegate = c;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
