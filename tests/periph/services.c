/* services: PowerPC system-services test - the OS services apps use besides the hardware.
   1. Keychain: a keychain of our own in /tmp (made, unlocked with a test passphrase, deleted
      afterwards; the user's keychains are not touched): add, find and read back a generic password.
   2. Bonjour: register a service (_rosettustest._tcp) and browse for it.
   3. FSEvents: watch a folder in /tmp and see a file created there.
   4. CFNetwork: an HTTP GET (http://example.com/), status code and body.
   5. SystemConfiguration: the computer name and the network reachability of example.com.
   6. Preferences: write, sync, read back and remove a value in a domain of our own.
   7. Power sources (IOKit): the list of batteries / power adapters.
   8. Disk Arbitration: the mounted volumes it reports.
   Build: gcc -arch ppc -o services services.c -framework Security -framework CoreServices
          -framework CoreFoundation -framework SystemConfiguration -framework IOKit -framework DiskArbitration */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <Security/Security.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/ps/IOPowerSources.h>
#include <DiskArbitration/DiskArbitration.h>
#include <dns_sd.h>
#include <stdarg.h>

static int ok_all = 1;
static void res(const char *n, int ok, const char *fmt, ...) {
    va_list ap; printf("RESULT %s %s ", n, ok ? "PASS" : "FAIL");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); fflush(stdout);
    if (!ok) ok_all = 0;
}
static void spin(double s) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, s, false); }

/* 1 */
static void t_keychain(void) {
    const char *path = "/tmp/rosettus-test.keychain", *pw = "rosettus-test-only";
    SecKeychainRef kc = NULL; SecKeychainItemRef it = NULL; OSStatus e;
    UInt32 len = 0; void *data = NULL; int ok = 0;
    unlink(path);
    e = SecKeychainCreate(path, (UInt32)strlen(pw), pw, false, NULL, &kc);
    if (!e) e = SecKeychainAddGenericPassword(kc, 13, "RosettusTest1", 4, "user", 6, "secret", &it);
    if (!e) e = SecKeychainFindGenericPassword(kc, 13, "RosettusTest1", 4, "user", &len, &data, NULL);
    if (!e && len == 6 && !memcmp(data, "secret", 6)) ok = 1;
    if (data) SecKeychainItemFreeContent(NULL, data);
    data = NULL;
    /* change the password through the item, read it back through the item */
    if (ok && it) {
        e = SecKeychainItemModifyAttributesAndData(it, NULL, 7, "secret2");
        if (!e) e = SecKeychainItemCopyContent(it, NULL, NULL, &len, &data);
        ok = !e && len == 7 && !memcmp(data, "secret2", 7);
        if (data) SecKeychainItemFreeContent(NULL, data);
        data = NULL;
    }
    /* an internet password */
    if (ok) {
        SecKeychainItemRef i2 = NULL;
        e = SecKeychainAddInternetPassword(kc, 11, "example.com", 0, NULL, 4, "user", 1, "/", 443, kSecProtocolTypeHTTPS,
                                           kSecAuthenticationTypeDefault, 5, "inetp", &i2);
        if (!e) e = SecKeychainFindInternetPassword(kc, 11, "example.com", 0, NULL, 4, "user", 0, NULL, 443, kSecProtocolTypeHTTPS,
                                                    kSecAuthenticationTypeAny, &len, &data, NULL);
        ok = !e && len == 5 && !memcmp(data, "inetp", 5);
        if (data) SecKeychainItemFreeContent(NULL, data);
        if (i2) CFRelease(i2);
    }
    if (it) CFRelease(it);
    if (kc) { SecKeychainDelete(kc); CFRelease(kc); }
    unlink(path);
    res("keychain", ok, "err %d, %u bytes read back (generic, modified, internet)", (int)e, (unsigned)len);
}
/* 2 */
static volatile int g_bj_found;
static void bj_reg(DNSServiceRef s, DNSServiceFlags f, DNSServiceErrorType e, const char *n, const char *t, const char *d, void *c) { (void)s; (void)f; (void)n; (void)t; (void)d; *(int *)c = e ? -1 : 1; }
static void bj_browse(DNSServiceRef s, DNSServiceFlags f, uint32_t i, DNSServiceErrorType e, const char *n, const char *t, const char *d, void *c) {
    (void)s; (void)i; (void)t; (void)d; (void)c;
    if (!e && (f & kDNSServiceFlagsAdd) && !strncmp(n, "Rosettus test", 13)) g_bj_found = 1;
}
static void bj_pump(DNSServiceRef s, double secs) {
    int fd = DNSServiceRefSockFD(s); double t = 0;
    while (t < secs) {
        fd_set r; struct timeval tv = { 0, 100000 };
        FD_ZERO(&r); FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) > 0) DNSServiceProcessResult(s);
        t += 0.1;
        if (g_bj_found) break;
    }
}
static void t_bonjour(void) {
    DNSServiceRef reg = NULL, br = NULL; int regd = 0; DNSServiceErrorType e;
    char name[64]; snprintf(name, sizeof name, "Rosettus test %d", (int)getpid());
    e = DNSServiceRegister(&reg, 0, 0, name, "_rosettustest._tcp", NULL, NULL, htons(49999), 0, NULL, bj_reg, &regd);
    if (!e) { int fd = DNSServiceRefSockFD(reg); double t = 0; while (!regd && t < 5) { fd_set r; struct timeval tv = { 0, 100000 }; FD_ZERO(&r); FD_SET(fd, &r); if (select(fd + 1, &r, 0, 0, &tv) > 0) DNSServiceProcessResult(reg); t += 0.1; } }
    if (!e && regd == 1) e = DNSServiceBrowse(&br, 0, 0, "_rosettustest._tcp", NULL, bj_browse, NULL);
    if (!e && br) bj_pump(br, 6);
    if (br) DNSServiceRefDeallocate(br);
    if (reg) DNSServiceRefDeallocate(reg);
    res("bonjour", !e && regd == 1 && g_bj_found, "err %d, registered %d, found by browsing %d", (int)e, regd, g_bj_found);
}
/* 3 */
static volatile int g_fse;
static void fse_cb(ConstFSEventStreamRef s, void *c, size_t n, void *paths, const FSEventStreamEventFlags f[], const FSEventStreamEventId ids[]) {
    size_t i; char **p = (char **)paths; (void)s; (void)c; (void)f; (void)ids;
    for (i = 0; i < n; i++) if (strstr(p[i], "rosettus-fse")) g_fse++;
}
static void t_fsevents(void) {
    char dir[128]; FSEventStreamRef st; CFStringRef d; CFArrayRef a; FILE *f;
    snprintf(dir, sizeof dir, "/tmp/rosettus-fse-%d", (int)getpid());
    mkdir(dir, 0755);
    d = CFStringCreateWithCString(0, dir, kCFStringEncodingUTF8);
    a = CFArrayCreate(0, (const void **)&d, 1, &kCFTypeArrayCallBacks);
    st = FSEventStreamCreate(0, fse_cb, NULL, a, kFSEventStreamEventIdSinceNow, 0.2, kFSEventStreamCreateFlagNone);
    FSEventStreamScheduleWithRunLoop(st, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(st);
    spin(0.5);
    {   char fn[160]; snprintf(fn, sizeof fn, "%s/file", dir); if ((f = fopen(fn, "w"))) { fputs("x", f); fclose(f); } }
    { double t = 0; while (!g_fse && t < 5) { spin(0.1); t += 0.1; } }
    FSEventStreamStop(st); FSEventStreamInvalidate(st); FSEventStreamRelease(st);
    { char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); system(cmd); }
    CFRelease(a); CFRelease(d);
    res("fsevents", g_fse > 0, "%d event(s) for the new file", g_fse);
}
/* 4 */
static void t_http(void) {
    CFURLRef u = CFURLCreateWithString(0, CFSTR("http://example.com/"), 0);
    CFHTTPMessageRef rq = CFHTTPMessageCreateRequest(0, CFSTR("GET"), u, kCFHTTPVersion1_1);
    CFReadStreamRef rs = CFReadStreamCreateForHTTPRequest(0, rq);
    UInt8 buf[4096]; CFIndex n, total = 0; int status = 0, sawhtml = 0;
    CFReadStreamSetProperty(rs, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);
    if (CFReadStreamOpen(rs)) {
        while ((n = CFReadStreamRead(rs, buf, sizeof buf - 1)) > 0) { buf[n] = 0; if (strstr((char *)buf, "<html") || strstr((char *)buf, "<HTML")) sawhtml = 1; total += n; }
        {   CFHTTPMessageRef resp = (CFHTTPMessageRef)CFReadStreamCopyProperty(rs, kCFStreamPropertyHTTPResponseHeader);
            if (resp) { status = (int)CFHTTPMessageGetResponseStatusCode(resp); CFRelease(resp); } }
        CFReadStreamClose(rs);
    }
    CFRelease(rs); CFRelease(rq); CFRelease(u);
    res("http", status == 200 && total > 200 && sawhtml, "status %d, %ld bytes", status, (long)total);
}
/* 5 */
static void t_sysconfig(void) {
    CFStringRef cn = SCDynamicStoreCopyComputerName(NULL, NULL);
    char name[256] = "";
    SCNetworkReachabilityRef r = SCNetworkReachabilityCreateWithName(0, "example.com");
    SCNetworkConnectionFlags fl = 0; Boolean got = r ? SCNetworkReachabilityGetFlags(r, &fl) : 0;
    if (cn) { CFStringGetCString(cn, name, sizeof name, kCFStringEncodingUTF8); CFRelease(cn); }
    if (r) CFRelease(r);
    res("sysconfig", name[0] && got && (fl & kSCNetworkFlagsReachable), "computer name %s, example.com reachability flags 0x%x", name[0] ? "found" : "missing", (unsigned)fl);
}
/* 6 */
static void t_prefs(void) {
    CFStringRef dom = CFSTR("local.rosettus.servicestest"), key = CFSTR("value");
    int v = 4242, back = 0; CFNumberRef n = CFNumberCreate(0, kCFNumberIntType, &v), r;
    CFPreferencesSetAppValue(key, n, dom);
    CFPreferencesAppSynchronize(dom);
    r = (CFNumberRef)CFPreferencesCopyAppValue(key, dom);
    if (r) { CFNumberGetValue(r, kCFNumberIntType, &back); CFRelease(r); }
    CFPreferencesSetAppValue(key, NULL, dom); CFPreferencesAppSynchronize(dom);
    CFRelease(n);
    { char p[512]; snprintf(p, sizeof p, "%s/Library/Preferences/local.rosettus.servicestest.plist", getenv("HOME")); unlink(p); }
    res("preferences", back == 4242, "read back %d", back);
}
/* 7 */
static int g_need_battery;
static void t_power(void) {
    CFTypeRef info = IOPSCopyPowerSourcesInfo(); CFArrayRef l = info ? IOPSCopyPowerSourcesList(info) : NULL;
    long n = l ? (long)CFArrayGetCount(l) : -1; char kind[64] = ""; int cap = -1, mx = -1;
    if (n > 0) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(l, 0));
        CFStringRef t = d ? (CFStringRef)CFDictionaryGetValue(d, CFSTR("Type")) : NULL;
        CFNumberRef c = d ? (CFNumberRef)CFDictionaryGetValue(d, CFSTR("Current Capacity")) : NULL, m = d ? (CFNumberRef)CFDictionaryGetValue(d, CFSTR("Max Capacity")) : NULL;
        if (t) CFStringGetCString(t, kind, sizeof kind, kCFStringEncodingUTF8);
        if (c) CFNumberGetValue(c, kCFNumberIntType, &cap);
        if (m) CFNumberGetValue(m, kCFNumberIntType, &mx);
    }
    if (l) CFRelease(l);
    if (info) CFRelease(info);
    /* a Mac without a battery lists none: passing needs only an answer (pass "battery" to require one) */
    res("power_sources", n >= 0 && (!g_need_battery || (n > 0 && cap >= 0)), "%ld source(s) %s, charge %d of %d", n, kind, cap, mx);
}
/* 8 */
static int g_da_n;
static void da_app(DADiskRef d, void *c) { (void)d; (void)c; g_da_n++; }
static void t_diskarb(void) {
    DASessionRef s = DASessionCreate(0);
    if (s) {
        DARegisterDiskAppearedCallback(s, NULL, da_app, NULL);
        DASessionScheduleWithRunLoop(s, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        spin(1.5);
        DASessionUnscheduleFromRunLoop(s, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRelease(s);
    }
    res("diskarbitration", s != NULL, "session %s, %d disk(s) reported", s ? "made" : "not made", g_da_n);
}
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "keychain")) t_keychain(); else if (!strcmp(argv[i], "bonjour")) t_bonjour();
        else if (!strcmp(argv[i], "fsevents")) t_fsevents(); else if (!strcmp(argv[i], "http")) t_http();
        else if (!strcmp(argv[i], "sysconfig")) t_sysconfig(); else if (!strcmp(argv[i], "prefs")) t_prefs();
        else if (!strcmp(argv[i], "power")) t_power(); else if (!strcmp(argv[i], "battery")) { g_need_battery = 1; t_power(); } else if (!strcmp(argv[i], "diskarb")) t_diskarb();
    }
    if (argc < 2) { t_keychain(); t_bonjour(); t_fsevents(); t_http(); t_sysconfig(); t_prefs(); t_power(); t_diskarb(); }
    return ok_all ? 0 : 1;
}
