/* spelltest.m (PowerPC): does NSSpellChecker work? usage: spelltest ["text"] */
#import <Cocoa/Cocoa.h>
int main(int argc, char **argv) {
    NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    NSString *s = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : @"This sentense has a speling mistake.";
    NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
    if (getenv("ST_LANG")) printf("setLanguage %d\n", [sc setLanguage:[NSString stringWithUTF8String:getenv("ST_LANG")]]);
    {   NSInteger wc = 0;
        NSRange r = [sc checkSpellingOfString:s startingAt:0 language:@"en" wrap:NO inSpellDocumentWithTag:0 wordCount:&wc];
        printf("en: first misspelled at %ld len %lu, %ld words\n", (long)(r.location == NSNotFound ? -1 : (long)r.location), (unsigned long)r.length, (long)wc);
    }
    printf("language %s, available %lu\n", [[sc language] UTF8String], (unsigned long)[[sc availableLanguages] count]);
    NSUInteger at = 0;
    for (;;) {
        NSRange r = [sc checkSpellingOfString:s startingAt:at];
        if (r.location == NSNotFound || r.length == 0 || r.location < at) break;   /* it wraps */
        NSString *w = [s substringWithRange:r];
        NSArray *g = [sc guessesForWordRange:r inString:s language:nil inSpellDocumentWithTag:0];
        printf("misspelled \"%s\" at %lu, guesses: %s\n", [w UTF8String], (unsigned long)r.location,
               [[g componentsJoinedByString:@", "] UTF8String]);
        at = r.location + r.length;
        if (at >= [s length]) break;
    }
    printf("done\n");
    [p release];
    return 0;
}
