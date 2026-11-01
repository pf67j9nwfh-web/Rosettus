// Rosettus Saver Host - a PowerPC program that runs a PowerPC screen saver on Mojave.
//
// Mojave's screen saver engine cannot load PowerPC .saver bundles. The native shim saver
// (src/savers/shim) starts this host through Rosetta when the screen saver starts; the host loads
// the original saver here, inside the translated 10.6 frameworks, and shows its view in a
// borderless window over the screen saver (mouse events pass through, so moving the mouse still
// ends the screen saver). The shim kills the host when the screen saver stops; the host also exits
// when its parent goes away.
//
// usage: "Rosettus Saver Host" SAVER.saver X Y WIDTH HEIGHT [LEVEL]   (window level: one above LEVEL)
// build (Snow Leopard, Xcode 3.2): src/savers/build_ppc.sh
#import <Cocoa/Cocoa.h>
#import <ScreenSaver/ScreenSaver.h>
#include <signal.h>
#include <unistd.h>

static pid_t g_parent;

@interface RSHWatch : NSObject
@end
@implementation RSHWatch
- (void)check:(NSTimer *)t { if (getppid() != g_parent) exit(0); }
@end

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    if (argc != 6 && argc != 7) { fprintf(stderr, "usage: %s SAVER X Y WIDTH HEIGHT [LEVEL]\n", argv[0]); return 2; }
    g_parent = getppid();
    [NSApplication sharedApplication];
    if ([NSApp respondsToSelector:@selector(setActivationPolicy:)])
        [NSApp setActivationPolicy:1 /* NSApplicationActivationPolicyAccessory */];

    [NSApp finishLaunching];        /* screens are known after this; window frames depend on them */
    {
        NSArray *sc = [NSScreen screens];
        NSRect f0 = [sc count] ? [[sc objectAtIndex:0] frame] : NSZeroRect;
        fprintf(stderr, "saverhost: %u screen(s), main %gx%g\n", (unsigned)[sc count], f0.size.width, f0.size.height);
    }
    NSString *path = [NSString stringWithUTF8String:argv[1]];
    NSRect frame = NSMakeRect(atof(argv[2]), atof(argv[3]), atof(argv[4]), atof(argv[5]));
    NSBundle *b = [NSBundle bundleWithPath:path];
    if (![b load]) { fprintf(stderr, "saverhost: cannot load %s\n", argv[1]); return 1; }
    Class cls = [b principalClass];
    if (!cls || ![cls isSubclassOfClass:[ScreenSaverView class]]) {
        fprintf(stderr, "saverhost: %s has no ScreenSaverView principal class\n", argv[1]); return 1;
    }
    NSWindow *w = [[NSWindow alloc] initWithContentRect:frame styleMask:NSBorderlessWindowMask
                                                backing:NSBackingStoreBuffered defer:NO];
    [w setLevel:(argc == 7 ? atoi(argv[6]) : NSScreenSaverWindowLevel) + 1];   /* above the screen saver engine's window */
    [w setBackgroundColor:[NSColor blackColor]];
    [w setIgnoresMouseEvents:YES];
    [w setHasShadow:NO];
    ScreenSaverView *v = [[cls alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height) isPreview:NO];
    if (!v) { fprintf(stderr, "saverhost: the saver did not create its view\n"); return 1; }
    [w setContentView:v];
    [w orderFrontRegardless];
    [v startAnimation];
    [NSTimer scheduledTimerWithTimeInterval:1.0 target:[[RSHWatch alloc] init] selector:@selector(check:)
                                   userInfo:nil repeats:YES];
    signal(SIGTERM, SIG_DFL);
    [NSApp run];
    [pool release];
    return 0;
}
