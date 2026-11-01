// PPCTestSaver - a small PowerPC screen saver used to test the Rosetta saver host
// (coloured discs drifting over a dark background, with "PowerPC" text).
// build (Snow Leopard): src/savers/build_ppc.sh
#import <ScreenSaver/ScreenSaver.h>

#define N 24
@interface PPCTestSaver : ScreenSaverView {
    NSPoint p[N], d[N];
    float r[N], hue[N];
}
@end

@implementation PPCTestSaver
- (id)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    if ((self = [super initWithFrame:frame isPreview:isPreview])) {
        int i;
        [self setAnimationTimeInterval:1 / 30.0];
        for (i = 0; i < N; i++) {
            p[i] = NSMakePoint(SSRandomFloatBetween(0, frame.size.width), SSRandomFloatBetween(0, frame.size.height));
            d[i] = NSMakePoint(SSRandomFloatBetween(-3, 3), SSRandomFloatBetween(-3, 3));
            r[i] = SSRandomFloatBetween(20, 90);
            hue[i] = SSRandomFloatBetween(0, 1);
        }
    }
    return self;
}
- (void)drawRect:(NSRect)rect {
    NSRect b = [self bounds];
    int i;
    [[NSColor colorWithCalibratedRed:0.05 green:0.05 blue:0.12 alpha:1] set];
    NSRectFill(b);
    for (i = 0; i < N; i++) {
        [[NSColor colorWithCalibratedHue:hue[i] saturation:0.7 brightness:0.95 alpha:0.55] set];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(p[i].x - r[i], p[i].y - r[i], 2 * r[i], 2 * r[i])] fill];
    }
    NSDictionary *at = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSFont boldSystemFontOfSize:b.size.height / 12], NSFontAttributeName,
        [NSColor colorWithCalibratedWhite:1 alpha:0.8], NSForegroundColorAttributeName, nil];
    NSString *s = @"PowerPC";
    NSSize ts = [s sizeWithAttributes:at];
    [s drawAtPoint:NSMakePoint((b.size.width - ts.width) / 2, (b.size.height - ts.height) / 2) withAttributes:at];
}
- (void)animateOneFrame {
    NSRect b = [self bounds];
    int i;
    for (i = 0; i < N; i++) {
        p[i].x += d[i].x; p[i].y += d[i].y;
        if (p[i].x < 0 || p[i].x > b.size.width) d[i].x = -d[i].x;
        if (p[i].y < 0 || p[i].y > b.size.height) d[i].y = -d[i].y;
        hue[i] += 0.002; if (hue[i] > 1) hue[i] -= 1;
    }
    [self setNeedsDisplay:YES];
}
- (BOOL)hasConfigureSheet { return NO; }
- (NSWindow *)configureSheet { return nil; }
@end
