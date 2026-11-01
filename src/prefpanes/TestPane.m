#import <PreferencePanes/PreferencePanes.h>
@interface RosettusTestPane : NSPreferencePane { NSTextField *label; int clicks; }
@end
@implementation RosettusTestPane
- (NSView *)loadMainView {
    NSView *view = [[[NSView alloc] initWithFrame:NSMakeRect(0,0,460,180)] autorelease];
    label = [[[NSTextField alloc] initWithFrame:NSMakeRect(24,110,410,30)] autorelease];
    [label setEditable:NO]; [label setBezeled:NO]; [label setDrawsBackground:NO];
    [label setStringValue:@"PowerPC preference pane — ready"];
    [view addSubview:label];
    NSButton *button = [[[NSButton alloc] initWithFrame:NSMakeRect(24,42,200,32)] autorelease];
    [button setTitle:@"Test PowerPC Action"]; [button setBezelStyle:NSRoundedBezelStyle];
    [button setTarget:self]; [button setAction:@selector(test:)]; [view addSubview:button];
    [self setMainView:view]; [self setInitialKeyView:button]; return view;
}
- (void)test:(id)sender { clicks++; [label setStringValue:[NSString stringWithFormat:@"PowerPC action received: %d", clicks]]; fprintf(stderr, "fixture: action %d\n", clicks); }
- (void)willSelect { [super willSelect]; fprintf(stderr,"fixture: willSelect\n"); }
- (void)didSelect { [super didSelect]; fprintf(stderr,"fixture: didSelect\n"); }
- (NSPreferencePaneUnselectReply)shouldUnselect {
    NSString *mode = [[[NSProcessInfo processInfo] environment] objectForKey:@"RPP_TEST_UNSELECT"];
    if ([mode isEqualToString:@"cancel"]) { fprintf(stderr,"fixture: cancel\n"); return NSUnselectCancel; }
    if ([mode isEqualToString:@"later"]) { [self performSelector:@selector(allow:) withObject:nil afterDelay:0.2]; fprintf(stderr,"fixture: later\n"); return NSUnselectLater; }
    return NSUnselectNow;
}
- (void)allow:(id)sender { [self replyToShouldUnselect:YES]; }
- (void)willUnselect { fprintf(stderr,"fixture: willUnselect\n"); [super willUnselect]; }
- (void)didUnselect { fprintf(stderr,"fixture: didUnselect\n"); [super didUnselect]; }
@end
