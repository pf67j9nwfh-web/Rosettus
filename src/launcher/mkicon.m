// mkicon.m - original Rosettus Launcher gear/play icon. usage: mkicon OUT.iconset
#import <Cocoa/Cocoa.h>

// Original metallic gear with a blue launch medallion, drawn at every resolution.
static void draw(CGFloat s) {
    NSRect tile = NSMakeRect(s*.065,s*.065,s*.87,s*.87);
    NSBezierPath *base = [NSBezierPath bezierPathWithRoundedRect:tile xRadius:s*.16 yRadius:s*.16];
    [NSGraphicsContext saveGraphicsState];
    NSShadow *shadow = [NSShadow new];
    shadow.shadowColor = [NSColor colorWithCalibratedWhite:0 alpha:.4];
    shadow.shadowOffset = NSMakeSize(0,-s*.014); shadow.shadowBlurRadius=s*.025; [shadow set];
    [[NSColor colorWithCalibratedWhite:.5 alpha:1] setFill]; [base fill];
    [NSGraphicsContext restoreGraphicsState];
    NSGradient *metal = [[NSGradient alloc] initWithColorsAndLocations:
        [NSColor colorWithCalibratedWhite:.62 alpha:1],0.0,
        [NSColor colorWithCalibratedWhite:.91 alpha:1],.48,
        [NSColor colorWithCalibratedWhite:.97 alpha:1],1.0,nil];
    [metal drawInBezierPath:base angle:90];
    [[NSColor colorWithCalibratedWhite:1 alpha:.8] setStroke]; base.lineWidth=s*.004; [base stroke];
    // Fine horizontal machining lines stay subtle at large sizes.
    [NSGraphicsContext saveGraphicsState]; [base addClip];
    [[NSColor colorWithCalibratedWhite:0 alpha:.035] setStroke];
    for (CGFloat y=s*.08;y<s*.93;y+=MAX(2,s*.004)) {
        NSBezierPath *line=[NSBezierPath bezierPath];
        [line moveToPoint:NSMakePoint(s*.07,y)]; [line lineToPoint:NSMakePoint(s*.93,y)];
        line.lineWidth=MAX(.35,s*.0007); [line stroke];
    }
    [NSGraphicsContext restoreGraphicsState];
    NSPoint c=NSMakePoint(s*.5,s*.51);
    NSBezierPath *gear=[NSBezierPath bezierPath];
    for(int i=0;i<96;i++) {
        CGFloat a=(i+.5)*2*M_PI/96;
        CGFloat r=s*((i%8>=2 && i%8<=5) ? .355 : .303);
        NSPoint p=NSMakePoint(c.x+cos(a)*r,c.y+sin(a)*r);
        if(i==0) [gear moveToPoint:p]; else [gear lineToPoint:p];
    }
    [gear closePath]; gear.lineJoinStyle=NSRoundLineJoinStyle;
    [NSGraphicsContext saveGraphicsState];
    shadow.shadowOffset=NSMakeSize(0,-s*.008); shadow.shadowBlurRadius=s*.012; [shadow set];
    [[NSColor colorWithCalibratedWhite:.23 alpha:1] setFill]; [gear fill];
    [NSGraphicsContext restoreGraphicsState];
    NSGradient *steel=[[NSGradient alloc] initWithStartingColor:[NSColor colorWithCalibratedWhite:.18 alpha:1]
        endingColor:[NSColor colorWithCalibratedWhite:.48 alpha:1]];
    [steel drawInBezierPath:gear angle:90];
    [[NSColor colorWithCalibratedWhite:.12 alpha:1] setStroke];gear.lineWidth=s*.006;[gear stroke];
    NSBezierPath *rim=[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-s*.257,c.y-s*.257,s*.514,s*.514)];
    [metal drawInBezierPath:rim angle:90];
    [[NSColor colorWithCalibratedWhite:.1 alpha:.7] setStroke];rim.lineWidth=s*.008;[rim stroke];
    NSBezierPath *hub=[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-s*.209,c.y-s*.209,s*.418,s*.418)];
    NSGradient *blue=[[NSGradient alloc] initWithStartingColor:[NSColor colorWithCalibratedRed:.04 green:.29 blue:.62 alpha:1]
        endingColor:[NSColor colorWithCalibratedRed:.19 green:.71 blue:.96 alpha:1]];
    [blue drawInBezierPath:hub angle:90];
    [[NSColor colorWithCalibratedWhite:1 alpha:.7] setStroke];hub.lineWidth=s*.005;[hub stroke];
    NSBezierPath *play=[NSBezierPath bezierPath];
    [play moveToPoint:NSMakePoint(c.x-s*.061,c.y+s*.108)];
    [play lineToPoint:NSMakePoint(c.x+s*.111,c.y)];
    [play lineToPoint:NSMakePoint(c.x-s*.061,c.y-s*.108)];[play closePath];
    [[NSColor colorWithCalibratedWhite:1 alpha:.98] setFill];[play fill];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) { fprintf(stderr, "usage: mkicon OUT.iconset\n"); return 2; }
        NSString *dir = [NSString stringWithUTF8String:argv[1]];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        int sizes[] = { 16, 32, 128, 256, 512 }, k, sc;
        for (k = 0; k < 5; k++) for (sc = 1; sc <= 2; sc++) {
            int px = sizes[k] * sc;
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:px pixelsHigh:px
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
            draw(px);
            [NSGraphicsContext restoreGraphicsState];
            NSString *name = sc == 1 ? [NSString stringWithFormat:@"icon_%dx%d.png", sizes[k], sizes[k]]
                                     : [NSString stringWithFormat:@"icon_%dx%d@2x.png", sizes[k], sizes[k]];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES];
        }
    }
    return 0;
}
