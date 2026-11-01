// Native Mojave browser; loads no third-party code into the browser process.
#import <Cocoa/Cocoa.h>
#import <mach/machine.h>
@interface Viewer : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate> {
    NSWindow *window;
    NSTableView *table;
    NSTextField *status;
    NSMutableArray *paths;
    NSButton *openButton;
}
@end
@implementation Viewer
- (BOOL)isPowerPC:(NSString *)path {
    NSArray *architectures = [[NSBundle bundleWithPath:path] executableArchitectures];
    return [architectures containsObject:[NSNumber numberWithInt:CPU_TYPE_POWERPC]];
}
- (void)refresh:(id)sender {
    [paths removeAllObjects];
    NSArray *directories = [NSArray arrayWithObjects:[NSHomeDirectory() stringByAppendingPathComponent:@"Library/PreferencePanes"], @"/Library/PreferencePanes", @"/Network/Library/PreferencePanes", nil];
    for (NSString *directory in directories) {
        for (NSString *entry in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:NULL]) {
            NSString *path = [directory stringByAppendingPathComponent:entry];
            if ([[entry pathExtension] caseInsensitiveCompare:@"prefPane"] == NSOrderedSame && [self isPowerPC:path]) [paths addObject:path];
        }
    }
    [paths sortUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    [table reloadData];
    if ([paths count]) [table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [openButton setEnabled:[paths count] > 0];
    [status setStringValue:[NSString stringWithFormat:@"%lu PowerPC preference pane%@ found. Use Other… to choose a file.", (unsigned long)[paths count], [paths count] == 1 ? @"" : @"s"]];
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    paths = [[NSMutableArray alloc] init];
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,720,370)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"Rosettus Preference Pane Viewer"];
    [window setDelegate:self];
    [window setReleasedWhenClosed:NO];
    NSView *content = [window contentView];
    NSTextField *heading = [NSTextField labelWithString:@"Installed PowerPC Preference Panes"];
    [heading setFont:[NSFont boldSystemFontOfSize:18]];
    [heading setFrame:NSMakeRect(22,320,670,28)]; [content addSubview:heading];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(22,94,676,212)];
    [scroll setHasVerticalScroller:YES]; [scroll setBorderType:NSBezelBorder];
    table = [[NSTableView alloc] initWithFrame:[scroll bounds]];
    NSTableColumn *name = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    [name setTitle:@"Preference Pane"]; [name setWidth:230]; [table addTableColumn:name];
    NSTableColumn *location = [[NSTableColumn alloc] initWithIdentifier:@"path"];
    [location setTitle:@"Location"]; [location setWidth:420]; [table addTableColumn:location];
    [table setDataSource:self]; [table setDelegate:self]; [table setTarget:self]; [table setDoubleAction:@selector(openSelected:)];
    [scroll setDocumentView:table]; [content addSubview:scroll];
    status = [NSTextField labelWithString:@""]; [status setFrame:NSMakeRect(22,65,676,20)]; [content addSubview:status];
    NSArray *titles = [NSArray arrayWithObjects:@"Refresh", @"Other…", @"Open Pane", nil];
    SEL actions[] = {@selector(refresh:), @selector(browse:), @selector(openSelected:)};
    CGFloat positions[] = {22, 462, 580};
    for (int i=0; i<3; i++) {
        NSButton *button = [NSButton buttonWithTitle:[titles objectAtIndex:i] target:self action:actions[i]];
        [button setFrame:NSMakeRect(positions[i],20,118,32)]; [content addSubview:button];
        if (i==2) { openButton = button; [button setKeyEquivalent:@"\r"]; }
    }
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *root = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""]; [menu addItem:root];
    NSMenu *app = [[NSMenu alloc] initWithTitle:@"Rosettus Preference Pane Viewer"];
    [app addItemWithTitle:@"Quit Preference Pane Viewer" action:@selector(terminate:) keyEquivalent:@"q"]; [root setSubmenu:app];
    [NSApp setMainMenu:menu];
    [self refresh:nil]; [window center]; [window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    if ([args count] > 1 && ![[args objectAtIndex:1] hasPrefix:@"-"]) [self launchPane:[args objectAtIndex:1]];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)view { return [paths count]; }
- (id)tableView:(NSTableView *)view objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSString *path = [paths objectAtIndex:row];
    if ([[column identifier] isEqualToString:@"path"]) return path;
    NSString *name = [[NSBundle bundleWithPath:path] objectForInfoDictionaryKey:@"CFBundleName"];
    return name ? name : [[path lastPathComponent] stringByDeletingPathExtension];
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification { [openButton setEnabled:[table selectedRow] >= 0]; }
- (void)openSelected:(id)sender {
    NSInteger row = [table selectedRow]; if (row >= 0 && row < (NSInteger)[paths count]) [self launchPane:[paths objectAtIndex:row]];
}
- (void)browse:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Choose a PowerPC Preference Pane"]; [panel setAllowedFileTypes:[NSArray arrayWithObject:@"prefPane"]];
    [panel setAllowsMultipleSelection:NO]; [panel setCanChooseDirectories:NO]; [panel setTreatsFilePackagesAsDirectories:NO];
    if ([panel runModal] == NSModalResponseOK) [self launchPane:[[panel URL] path]];
}
- (void)showError:(NSString *)message {
    NSAlert *alert = [[NSAlert alloc] init]; [alert setMessageText:@"Cannot Open Preference Pane"]; [alert setInformativeText:message]; [alert runModal]; [alert release];
}
- (void)launchPane:(NSString *)path {
    path = [[path stringByStandardizingPath] stringByResolvingSymlinksInPath];
    if ([[path pathExtension] caseInsensitiveCompare:@"prefPane"] != NSOrderedSame || ![self isPowerPC:path]) {
        [self showError:@"Choose a .prefPane bundle containing PowerPC code. Intel-only panes can be opened with System Preferences."]; return;
    }
    NSString *runtime = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"RosettusRoot"];
    if (!runtime) runtime = @"/Library/Rosettus";
    NSString *launcher = [runtime stringByAppendingPathComponent:@"rosettus"];
    NSString *host = [runtime stringByAppendingPathComponent:@"prefpanes/Rosettus Preference Pane Host.app"];
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:launcher] || ![[NSFileManager defaultManager] fileExistsAtPath:host]) {
        [self showError:@"The Rosettus runtime or preference pane host is missing. Install or update Rosettus first."]; return;
    }
    NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Rosettus/PreferencePanes"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *log = [logs stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.log", [[path lastPathComponent] stringByDeletingPathExtension], [[NSProcessInfo processInfo] globallyUniqueString]]];
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle *output = [NSFileHandle fileHandleForWritingAtPath:log];
    if (!output) { [self showError:@"Cannot create the preference pane log in ~/Library/Logs/Rosettus."]; return; }
    NSTask *task = [[NSTask alloc] init]; [task setLaunchPath:launcher]; [task setArguments:[NSArray arrayWithObjects:host, path, nil]];
    NSMutableDictionary *environment = [NSMutableDictionary dictionaryWithDictionary:[[NSProcessInfo processInfo] environment]];
    [environment setObject:runtime forKey:@"ROSETTUS_PREFPANE_ROOT"];
    // Avoid concurrent JIT commpage-load rewriting in panes with background workers.
    if (![environment objectForKey:@"OAH_NO_CPPATCH"]) [environment setObject:@"1" forKey:@"OAH_NO_CPPATCH"];
    [task setEnvironment:environment];
    [task setStandardOutput:output]; [task setStandardError:output]; [task setStandardInput:[NSFileHandle fileHandleWithNullDevice]];
    @try { [task launch]; [status setStringValue:[NSString stringWithFormat:@"Opening %@…", [path lastPathComponent]]]; }
    @catch(NSException *exception) { [self showError:[exception reason]]; }
    [output closeFile]; [task release];
}
- (BOOL)application:(NSApplication *)sender openFile:(NSString *)filename { [self launchPane:filename]; return YES; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }
@end
int main(int argc, const char **argv) {
    @autoreleasepool { [NSApplication sharedApplication]; Viewer *viewer = [[Viewer alloc] init]; [NSApp setDelegate:viewer]; [NSApp run]; [viewer release]; } return 0;
}
