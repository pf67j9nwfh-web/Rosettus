// RosettusSaverShim - a native (x86_64) screen saver that runs a PowerPC screen saver via Rosetta.
//
// tools/install_saver.py copies this bundle for each PowerPC saver: the original goes into
// Contents/Resources/Original.saver, Info.plist gets RosettusRoot, and the class name placeholder
// below is replaced by a unique one (several shims can then load into one process, as System
// Preferences does). When the screen saver runs, the shim starts
//     ROOT/rosettus "ROOT/savers/Rosettus Saver Host.app" Original.saver X Y W H LEVEL
// which shows the original saver over this view's screen area; it is stopped (its whole process
// group) when the screen saver stops. The small preview in System Preferences shows a label.
//
// build: src/savers/build_shim.sh (macOS 10.14, Command Line Tools)
#import <ScreenSaver/ScreenSaver.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#define SHIM_CLASS RosettusSaverShim_000000000000      /* 12-digit placeholder, replaced per install */

@interface SHIM_CLASS : ScreenSaverView
@property pid_t host;
@property (copy) NSString *saverName;
@end

@implementation SHIM_CLASS

- (NSBundle *)myBundle { return [NSBundle bundleForClass:[self class]]; }

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    if ((self = [super initWithFrame:frame isPreview:isPreview])) {
        self.animationTimeInterval = 1.0;
        NSString *orig = [[self myBundle] pathForResource:@"Original" ofType:@"saver"];
        NSBundle *ob = orig ? [NSBundle bundleWithPath:orig] : nil;
        self.saverName = [ob objectForInfoDictionaryKey:@"CFBundleName"] ?: [[[self myBundle] bundlePath].lastPathComponent stringByDeletingPathExtension];
    }
    return self;
}

- (void)startHost {
    if (self.host > 0 || self.isPreview) return;
    NSBundle *me = [self myBundle];
    NSString *root = [me objectForInfoDictionaryKey:@"RosettusRoot"];
    NSString *orig = [me pathForResource:@"Original" ofType:@"saver"];
    NSString *hostApp = [root stringByAppendingPathComponent:@"savers/Rosettus Saver Host.app"];
    if (!root || !orig) return;
    // this view's area in global (bottom-left origin) coordinates
    NSRect r = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
    NSArray *args = @[ [root stringByAppendingPathComponent:@"rosetta"], hostApp, orig,
                       [NSString stringWithFormat:@"%g", r.origin.x], [NSString stringWithFormat:@"%g", r.origin.y],
                       [NSString stringWithFormat:@"%g", r.size.width], [NSString stringWithFormat:@"%g", r.size.height],
                       [NSString stringWithFormat:@"%ld", (long)self.window.level] ];
    NSString *log = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Rosettus/ScreenSaver.log"];
    [[NSFileManager defaultManager] createDirectoryAtPath:[log stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    posix_spawn_file_actions_t fa; posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa); posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    char *av[9]; int i;
    for (i = 0; i < 8; i++) av[i] = (char *)[args[i] fileSystemRepresentation];
    av[8] = NULL;
    extern char **environ;
    pid_t pid = 0;
    if (posix_spawn(&pid, av[0], &fa, &at, av, environ) == 0) self.host = pid;
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
}

- (void)stopHost {
    if (self.host > 0) {
        killpg(self.host, SIGTERM);
        kill(self.host, SIGTERM);
        int st; waitpid(self.host, &st, WNOHANG);
        self.host = 0;
    }
}

- (void)startAnimation { [super startAnimation]; [self startHost]; }
- (void)stopAnimation { [self stopHost]; [super stopAnimation]; }
- (void)viewDidMoveToWindow { if (!self.window) [self stopHost]; }
- (void)dealloc { [self stopHost]; }

- (void)drawRect:(NSRect)rect {
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!self.isPreview) return;              // full screen: the PowerPC saver draws over us
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.alignment = NSTextAlignmentCenter;
    CGFloat fs = MAX(9, self.bounds.size.height / 12);
    NSDictionary *a1 = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:fs], NSForegroundColorAttributeName: [NSColor whiteColor], NSParagraphStyleAttributeName: ps };
    NSDictionary *a2 = @{ NSFontAttributeName: [NSFont systemFontOfSize:fs * 0.7], NSForegroundColorAttributeName: [NSColor colorWithWhite:0.75 alpha:1], NSParagraphStyleAttributeName: ps };
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc] initWithString:[self.saverName stringByAppendingString:@"\n"] attributes:a1];
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"PowerPC screen saver - runs full screen through Rosetta" attributes:a2]];
    NSRect b = NSInsetRect(self.bounds, 8, 0);
    NSSize sz = [s boundingRectWithSize:b.size options:NSStringDrawingUsesLineFragmentOrigin].size;
    [s drawWithRect:NSMakeRect(b.origin.x, (self.bounds.size.height - sz.height) / 2, b.size.width, sz.height) options:NSStringDrawingUsesLineFragmentOrigin];
}

- (void)animateOneFrame {
    if (!self.isPreview && self.host > 0) {    // restart the host if it ended early
        int st;
        if (waitpid(self.host, &st, WNOHANG) == self.host) { self.host = 0; [self startHost]; }
    }
}
- (BOOL)hasConfigureSheet { return NO; }
- (NSWindow *)configureSheet { return nil; }
@end
