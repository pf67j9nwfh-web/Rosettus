/* cf_basics: CoreFoundation behaviour that apps lean on (strings, numbers, dates, plists, URLs, prefs).
   Build (10.6): gcc -arch ppc -o cf_basics cf_basics.c -framework CoreFoundation */
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

static void show(const char *tag, CFTypeRef o) {
    char buf[1024] = "";
    CFStringRef d = o ? (CFGetTypeID(o) == CFStringGetTypeID() ? CFRetain(o) : CFCopyDescription(o)) : 0;
    if (d) { CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(d); }
    { char *p; for (p = buf; *p; p++) if (*p == '\n') *p = ' '; }
    /* descriptions contain pointers: drop "0x..." runs */
    { char out[1024], *s = buf, *t = out;
      while (*s && t < out + sizeof out - 1) { if (s[0] == '0' && s[1] == 'x') { *t++ = '@'; s += 2; while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')) s++; } else *t++ = *s++; }
      *t = 0; printf("%-22s %s\n", tag, out); }
}
int main(void) {
    CFStringRef s = CFSTR("Hello, Rosetta été — café");
    CFMutableStringRef m = CFStringCreateMutableCopy(0, 0, s);
    CFStringUppercase(m, 0); show("uppercase", m);
    CFStringFold(m, kCFCompareCaseInsensitive | kCFCompareDiacriticInsensitive, 0); show("fold", m);
    printf("%-22s %ld\n", "length", (long)CFStringGetLength(s));
    { CFRange r = CFStringFind(s, CFSTR("café"), 0); printf("%-22s %ld,%ld\n", "find", (long)r.location, (long)r.length); }
    printf("%-22s %d\n", "compare-ci", (int)CFStringCompare(CFSTR("abc"), CFSTR("ABD"), kCFCompareCaseInsensitive));
    printf("%-22s %d\n", "compare-num", (int)CFStringCompare(CFSTR("file10"), CFSTR("file9"), kCFCompareNumerically));
    { char mac[64]; CFIndex used = 0; UInt8 b[64];
      CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingMacRoman, '?', false, b, sizeof b, &used);
      { CFIndex i; char *p = mac; for (i = 0; i < used && i < 20; i++) p += sprintf(p, "%02x", b[i]); }
      printf("%-22s %ld %s\n", "macroman", (long)used, mac); }
    { CFArrayRef parts = CFStringCreateArrayBySeparatingStrings(0, CFSTR("a:b::c"), CFSTR(":")); show("split", parts); CFRelease(parts); }
    { double d = 3.141592653589793; CFNumberRef n = CFNumberCreate(0, kCFNumberDoubleType, &d); show("number", n);
      SInt64 big = 0x123456789abcdefLL; CFNumberRef n2 = CFNumberCreate(0, kCFNumberSInt64Type, &big); show("int64", n2);
      SInt32 back = 0; CFNumberGetValue(n2, kCFNumberSInt32Type, &back); printf("%-22s %ld\n", "int64->32", (long)back); }
    { CFTimeZoneRef tz = CFTimeZoneCreateWithTimeIntervalFromGMT(0, 0);
      CFGregorianDate g = CFAbsoluteTimeGetGregorianDate(300000000.5, tz);
      printf("%-22s %d-%02d-%02d %02d:%02d:%06.3f dow %d\n", "gregorian", (int)g.year, g.month, g.day, g.hour, g.minute, g.second,
             (int)CFAbsoluteTimeGetDayOfWeek(300000000.5, tz));
      CFTimeZoneRef ny = CFTimeZoneCreateWithName(0, CFSTR("America/New_York"), true);
      printf("%-22s %.0f %d\n", "ny-offset", ny ? CFTimeZoneGetSecondsFromGMT(ny, 300000000.0) : -1.0, ny ? (int)CFTimeZoneIsDaylightSavingTime(ny, 300000000.0) : -1); }
    {   /* plist round trip */
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int v = 42; CFNumberRef n = CFNumberCreate(0, kCFNumberIntType, &v);
        CFDictionarySetValue(d, CFSTR("answer"), n);
        CFDictionarySetValue(d, CFSTR("name"), CFSTR("rösetta"));
        CFDictionarySetValue(d, CFSTR("flag"), kCFBooleanTrue);
        { UInt8 raw[5] = {1, 2, 3, 0xfe, 0xff}; CFDataRef dt = CFDataCreate(0, raw, 5); CFDictionarySetValue(d, CFSTR("data"), dt); }
        CFDataRef x = CFPropertyListCreateXMLData(0, d);
        printf("%-22s %ld bytes\n", "plist-xml", (long)CFDataGetLength(x));
        CFPropertyListRef back = CFPropertyListCreateFromXMLData(0, x, kCFPropertyListImmutable, 0);
        printf("%-22s %d\n", "plist-equal", (int)CFEqual(back, d));
        CFWriteStreamRef ws = CFWriteStreamCreateWithAllocatedBuffers(0, 0); CFWriteStreamOpen(ws);
        CFIndex nb = CFPropertyListWriteToStream(d, ws, kCFPropertyListBinaryFormat_v1_0, 0);
        printf("%-22s %ld bytes\n", "plist-binary", (long)nb);
    }
    { CFURLRef u = CFURLCreateWithFileSystemPath(0, CFSTR("/Applications/Some App.app/Contents"), kCFURLPOSIXPathStyle, true);
      show("url", u); show("url-last", CFURLCopyLastPathComponent(u));
      CFURLRef h = CFURLCreateWithFileSystemPath(0, CFSTR("Macintosh HD:Users:jr:Doc"), kCFURLHFSPathStyle, false); show("url-hfs", h);
      CFURLRef w = CFURLCreateWithString(0, CFSTR("http://68k.news/article.php?a=1&b=%20x#frag"), 0);
      show("url-host", CFURLCopyHostName(w)); show("url-query", CFURLCopyQueryString(w, 0)); }
    {   /* preferences in a private domain */
        CFStringRef app = CFSTR("local.rosetta.difftest");
        int v = 7; CFNumberRef n = CFNumberCreate(0, kCFNumberIntType, &v);
        CFPreferencesSetAppValue(CFSTR("k"), n, app);
        printf("%-22s %d\n", "prefs-sync", (int)CFPreferencesAppSynchronize(app));
        CFPropertyListRef r = CFPreferencesCopyAppValue(CFSTR("k"), app); show("prefs-read", r);
        CFPreferencesSetAppValue(CFSTR("k"), 0, app); CFPreferencesAppSynchronize(app);
    }
    { CFUUIDRef u1 = CFUUIDCreateFromString(0, CFSTR("68753A44-4D6F-1226-9C60-0050E4C00067"));
      CFUUIDBytes b = CFUUIDGetUUIDBytes(u1); printf("%-22s %02x%02x..%02x\n", "uuid", b.byte0, b.byte1, b.byte15); }
    { CFCharacterSetRef ws = CFCharacterSetGetPredefined(kCFCharacterSetWhitespaceAndNewline);
      printf("%-22s %d %d\n", "charset", (int)CFCharacterSetIsCharacterMember(ws, ' '), (int)CFCharacterSetIsCharacterMember(ws, 'x')); }
    { CFLocaleRef loc = CFLocaleCreate(0, CFSTR("en_US")); CFNumberFormatterRef nf = CFNumberFormatterCreate(0, loc, kCFNumberFormatterDecimalStyle);
      double d = 1234567.891; CFStringRef s2 = CFNumberFormatterCreateStringWithValue(0, nf, kCFNumberDoubleType, &d); show("numfmt-en", s2); }
    return 0;
}
