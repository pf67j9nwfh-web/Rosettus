// Standalone PowerPC preference-pane host. Build on Snow Leopard.
// Legacy panes may use JavaVM.framework/Commands/java (removed on Mojave).
// Redirect this NSTask launch inside the pane process to the user's exported PPC JDK.
// No system paths or third-party bundles are modified.
#import <Cocoa/Cocoa.h>
#import <PreferencePanes/PreferencePanes.h>
#import <objc/runtime.h>
@interface NSTask (PaneTaskCompatibility)
- (void)rpp_launch;
@end
@implementation NSTask (PaneTaskCompatibility)
- (void)rpp_launch {
    fprintf(stderr, "prefpanehost: helper %s\n", [[self launchPath] UTF8String]);
    NSString *old = [self launchPath];
    if ([old isEqualToString:@"/System/Library/Frameworks/JavaVM.framework/Commands/java"]) {
        NSString *root = [[[NSProcessInfo processInfo] environment] objectForKey:@"ROSETTUS_PREFPANE_ROOT"];
        if (!root) root = @"/Library/Rosettus";
        [self setLaunchPath:[root stringByAppendingPathComponent:@"rjava"]];
        NSMutableDictionary *environment = [NSMutableDictionary dictionaryWithDictionary:
            [self environment] ? [self environment] : [[NSProcessInfo processInfo] environment]];
        [environment removeObjectForKey:@"OAH_GUEST_EXE"];
        [environment removeObjectForKey:@"CFProcessPath"];
        [environment removeObjectForKey:@"OAH_TRANSLATE_LINK"];
        [self setEnvironment:environment];
    }
    [self rpp_launch];
    fprintf(stderr, "prefpanehost: helper launched pid=%d\n", [self processIdentifier]);
}
@end

@interface PaneHost : NSObject <NSWindowDelegate> {
    NSPreferencePane *pane;
    NSWindow *window;
    NSString *path;
    BOOL pending;
    BOOL unselected;
}
- (id)initWithPath:(NSString *)value;
@end
@implementation PaneHost
- (id)initWithPath:(NSString *)value {
    if ((self = [super init])) path = [value copy];
    return self;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    @try {
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        if (!bundle || ![bundle load]) [NSException raise:@"PaneLoad" format:@"Cannot load preference pane: %@", path];
        Class cls = [bundle principalClass];
        if (!cls || ![cls isSubclassOfClass:[NSPreferencePane class]])
            [NSException raise:@"PaneLoad" format:@"The bundle has no NSPreferencePane principal class."];
        fprintf(stderr, "prefpanehost: initializing pane\n");
        pane = [[cls alloc] initWithBundle:bundle];
        fprintf(stderr, "prefpanehost: loading view\n");
        NSView *view = [pane loadMainView];
        if (!view) [NSException raise:@"PaneLoad" format:@"The preference pane did not create a view."];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, [view frame].size.width, [view frame].size.height)
            styleMask:NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask
            backing:NSBackingStoreBuffered defer:NO];
        [window setReleasedWhenClosed:NO];
        [window setDelegate:self];
        NSString *title = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
        [window setTitle:title ? title : [[path lastPathComponent] stringByDeletingPathExtension]];
        [window setContentView:view];
        [window center];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(unselectReply:)
            name:NSPreferencePaneDoUnselectNotification object:pane];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(unselectReply:)
            name:NSPreferencePaneCancelUnselectNotification object:pane];
        [window makeKeyAndOrderFront:nil];
        [pane willSelect];
        [window makeFirstResponder:[pane initialKeyView]];
        fprintf(stderr, "prefpanehost: didSelect\n");
        [pane didSelect];
        [NSApp activateIgnoringOtherApps:YES];
        fprintf(stderr, "prefpanehost: selected %s\n", [path fileSystemRepresentation]);
    } @catch (NSException *exception) {
        fprintf(stderr, "prefpanehost: %s\n%s\n", [[exception reason] UTF8String], [[[exception callStackSymbols] description] UTF8String]);
        NSRunAlertPanel(@"Cannot Open Preference Pane", @"%@", @"Quit", nil, nil, [exception reason]);
        exit(1);
    }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    if (!pane || unselected) return NSTerminateNow;
    if (pending) return NSTerminateCancel;
    if ([pane autoSaveTextFields]) [window makeFirstResponder:nil];
    NSPreferencePaneUnselectReply reply = [pane shouldUnselect];
    if (reply == NSUnselectCancel) return NSTerminateCancel;
    if (reply == NSUnselectLater) { pending = YES; return NSTerminateCancel; }
    [pane willUnselect];
    [pane didUnselect];
    unselected = YES;
    return NSTerminateNow;
}
- (void)unselectReply:(NSNotification *)notification {
    if (!pending) return;
    pending = NO;
    BOOL allow = [[notification name] isEqualToString:NSPreferencePaneDoUnselectNotification];
    if (allow) { [pane willUnselect]; [pane didUnselect]; unselected = YES; }
    if (allow) [NSApp terminate:nil];
}
- (BOOL)windowShouldClose:(id)sender { [NSApp terminate:nil]; return NO; }
@end

int main(int argc, char **argv) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    if (argc != 2) { fprintf(stderr, "usage: %s /path/to/Pane.prefPane\n", argv[0]); return 2; }
    Class concrete = NSClassFromString(@"NSConcreteTask");
    class_addMethod(concrete, @selector(rpp_launch), method_getImplementation(class_getInstanceMethod([NSTask class], @selector(rpp_launch))), "v@:");
    method_exchangeImplementations(class_getInstanceMethod(concrete, @selector(launch)), class_getInstanceMethod(concrete, @selector(rpp_launch)));
    [NSApplication sharedApplication];
    NSMenu *menu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    [menu addItem:item];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@"Rosettus Preference Pane Viewer"] autorelease];
    [appMenu addItemWithTitle:@"Quit Preference Pane Viewer" action:@selector(terminate:) keyEquivalent:@"q"];
    [item setSubmenu:appMenu];
    NSMenuItem *editItem = [[[NSMenuItem alloc] initWithTitle:@"Edit" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *editMenu = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editItem setSubmenu:editMenu]; [menu addItem:editItem];
    [NSApp setMainMenu:menu];
    PaneHost *host = [[PaneHost alloc] initWithPath:[NSString stringWithUTF8String:argv[1]]];
    [NSApp setDelegate:host];
    [NSApp run];
    [host release];
    [pool drain];
    return 0;
}
