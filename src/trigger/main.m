// Install Rosetta.app - a PowerPC-only application for Snow Leopard.
//
// Snow Leopard does not install Rosetta by default. Opening a PowerPC-only application makes the
// system offer to download and install it ("To open Install Rosetta, you need to install Rosetta").
// Once Rosetta is installed this app actually runs (translated) and says so, and offers to run the
// export script that collects the files for Rosettus.
//
// build (Snow Leopard, Xcode 3.2 with PowerPC support): see build.sh
#import <Cocoa/Cocoa.h>

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    [NSApp activateIgnoringOtherApps:YES];
    NSString *here = [[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent];
    NSString *script = [here stringByAppendingPathComponent:@"export_from_sl.sh"];
    BOOL haveScript = [[NSFileManager defaultManager] isExecutableFileAtPath:script];

    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:@"Rosetta is installed"];
    [a setInformativeText:haveScript
        ? @"This PowerPC application is running, so Rosetta works on this Mac.\n\n"
          @"Next, collect the files for Rosettus. \"Run Export\" opens Terminal and runs "
          @"export_from_sl.sh (5-15 minutes); the archive is saved on your Desktop."
        : @"This PowerPC application is running, so Rosetta works on this Mac.\n\n"
          @"Next, run import/sl/export_from_sl.sh from the Rosettus folder."];
    if (haveScript) {
        [a addButtonWithTitle:@"Run Export"];
        [a addButtonWithTitle:@"Quit"];
    } else [a addButtonWithTitle:@"OK"];
    NSInteger r = [a runModal];
    if (haveScript && r == NSAlertFirstButtonReturn) {
        // Terminal shows the progress; the script itself does all the work
        NSString *esc = [[here stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
                               stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
        NSString *src = [NSString stringWithFormat:
            @"tell application \"Terminal\"\nactivate\ndo script \"cd \" & quoted form of \"%@\" & \" && ./export_from_sl.sh\"\nend tell", esc];
        NSAppleScript *as = [[[NSAppleScript alloc] initWithSource:src] autorelease];
        [as executeAndReturnError:nil];
    }
    [pool release];
    return 0;
}
