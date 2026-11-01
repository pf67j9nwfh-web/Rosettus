/* hid.c - HID devices through IOHIDManager (Mac OS X 10.5 API), for the Rosettus peripheral tests.
   1. the manager: open, device list, a device-matching callback per device on the run loop
   2. each device: product, usage, elements (buttons / axes / keys) and their properties
   3. the keyboard: opened, a key's value read, a queue on the key elements started and stopped
   4. with "led": the caps lock LED switched on through IOHIDDeviceSetValue, read back, switched off
   5. with "watch N": every input value for N seconds (press a key / move a game pad)
   Build: gcc -arch ppc -mmacosx-version-min=10.5 -o hid hid.c -framework IOKit -framework CoreFoundation */
#include <IOKit/hid/IOHIDLib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
static int ok_all = 1;
static void res(const char *n, int ok, const char *fmt, ...) {
    va_list ap; char b[512];
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("RESULT %s %s %s\n", n, ok ? "PASS" : "FAIL", b); fflush(stdout);
    if (!ok) ok_all = 0;
}
static int g_matched, g_values;
static void matched(void *c, IOReturn r, void *s, IOHIDDeviceRef d) { (void)c; (void)r; (void)s; (void)d; g_matched++; }
static void value(void *c, IOReturn r, void *s, IOHIDValueRef v) {
    IOHIDElementRef e = IOHIDValueGetElement(v);
    (void)c; (void)r; (void)s;
    if (g_values++ < 40) printf("  value: page 0x%x usage 0x%x = %ld (scaled %.2f)\n", (unsigned)IOHIDElementGetUsagePage(e), (unsigned)IOHIDElementGetUsage(e),
                                (long)IOHIDValueGetIntegerValue(v), IOHIDValueGetScaledValue(v, kIOHIDValueScaleTypeCalibrated));
}
static int g_cbvals; static long g_cbval = -1;
static void getcb(void *c, IOReturn r, void *s, IOHIDValueRef v) { (void)s; if (c == (void *)0x1234 && !r && v) { g_cbvals++; g_cbval = IOHIDValueGetIntegerValue(v); } }
static int g_setcbs; static long g_setval = -1;
static void setcb(void *c, IOReturn r, void *s, IOHIDValueRef v) { (void)s; (void)r; if (c == (void *)0x5678) { g_setcbs++; g_setval = v ? IOHIDValueGetIntegerValue(v) : -2; } }
static long num(IOHIDDeviceRef d, CFStringRef k) { long v = -1; CFTypeRef n = IOHIDDeviceGetProperty(d, k); if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberLongType, &v); return v; }
static void str(IOHIDDeviceRef d, CFStringRef k, char *b, int n) { CFTypeRef s = IOHIDDeviceGetProperty(d, k); b[0] = 0; if (s && CFGetTypeID(s) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)s, b, n, kCFStringEncodingUTF8); }
int main(int argc, char **argv) {
    int led = 0, watch = 0, i;
    IOHIDManagerRef m; CFSetRef set; CFIndex n = 0; IOHIDDeviceRef *devs = NULL, kbd = NULL;
    IOReturn e;
    for (i = 1; i < argc; i++) { if (!strcmp(argv[i], "led")) led = 1; else if (!strcmp(argv[i], "watch") && i + 1 < argc) watch = atoi(argv[++i]); }
    m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(m, NULL);
    IOHIDManagerRegisterDeviceMatchingCallback(m, matched, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    e = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    set = IOHIDManagerCopyDevices(m);
    if (set) { n = CFSetGetCount(set); devs = (IOHIDDeviceRef *)calloc(n ? n : 1, sizeof *devs); CFSetGetValues(set, (const void **)devs); }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
    res("manager", m && set && n > 0 && g_matched == n, "open 0x%x, %ld device(s), %d matching callback(s)", e, (long)n, g_matched);
    {   int total = 0, named = 0, withel = 0;
        for (i = 0; i < n; i++) {
            char prod[128], tr[64]; long pg = num(devs[i], CFSTR(kIOHIDPrimaryUsagePageKey)), us = num(devs[i], CFSTR(kIOHIDPrimaryUsageKey));
            CFArrayRef els = IOHIDDeviceCopyMatchingElements(devs[i], NULL, kIOHIDOptionsTypeNone);
            int ne = els ? (int)CFArrayGetCount(els) : 0, nb = 0, na = 0, k;
            str(devs[i], CFSTR(kIOHIDProductKey), prod, sizeof prod); str(devs[i], CFSTR(kIOHIDTransportKey), tr, sizeof tr);
            for (k = 0; k < ne; k++) {
                IOHIDElementRef el = (IOHIDElementRef)CFArrayGetValueAtIndex(els, k);
                IOHIDElementType t = IOHIDElementGetType(el);
                if (t == kIOHIDElementTypeInput_Button) nb++;
                else if (t == kIOHIDElementTypeInput_Misc || t == kIOHIDElementTypeInput_Axis) na++;
                if (IOHIDElementGetDevice(el) != devs[i]) { printf("  element %d: wrong device\n", k); ok_all = 0; }
            }
            printf("  device %d: \"%s\" (%s) vendor 0x%lx product 0x%lx usage %ld:%ld, %d elements (%d buttons, %d values)\n", i, prod, tr,
                   num(devs[i], CFSTR(kIOHIDVendorIDKey)), num(devs[i], CFSTR(kIOHIDProductIDKey)), pg, us, ne, nb, na);
            total += ne; if (prod[0]) named++; if (ne) withel++;
            if (pg == kHIDPage_GenericDesktop && us == kHIDUsage_GD_Keyboard && !kbd) kbd = devs[i];
            if (els) CFRelease(els);
        }
        res("devices", named > 0 && withel > 0, "%d named, %d with elements, %d elements in all", named, withel, total);
    }
    if (kbd) {
        CFMutableDictionaryRef mt = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int pg = kHIDPage_KeyboardOrKeypad; CFNumberRef pn = CFNumberCreate(NULL, kCFNumberIntType, &pg);
        CFArrayRef keys; IOHIDElementRef a = NULL; IOHIDValueRef v = NULL; IOHIDQueueRef q; IOReturn oe, ge = -1; long lmin = -9, lmax = -9;
        CFDictionarySetValue(mt, CFSTR(kIOHIDElementUsagePageKey), pn);
        oe = IOHIDDeviceOpen(kbd, kIOHIDOptionsTypeNone);
        keys = IOHIDDeviceCopyMatchingElements(kbd, mt, kIOHIDOptionsTypeNone);
        for (i = 0; keys && i < CFArrayGetCount(keys); i++) {
            IOHIDElementRef el = (IOHIDElementRef)CFArrayGetValueAtIndex(keys, i);
            if (IOHIDElementGetUsage(el) == kHIDUsage_KeyboardA) a = el;
        }
        if (a) { ge = IOHIDDeviceGetValue(kbd, a, &v); lmin = IOHIDElementGetLogicalMin(a); lmax = IOHIDElementGetLogicalMax(a); }
        res("keyboard", !oe && keys && CFArrayGetCount(keys) > 0 && a && !ge && v && IOHIDValueGetElement(v) == a,
            "open 0x%x, %ld key elements, A: cookie %u, range %ld..%ld, value %ld (get 0x%x)", oe, keys ? (long)CFArrayGetCount(keys) : -1L,
            a ? (unsigned)IOHIDElementGetCookie(a) : 0, lmin, lmax, v ? (long)IOHIDValueGetIntegerValue(v) : -1L, ge);
        if (a) {   /* a value delivered through a callback on this run loop */
            IOHIDValueRef cv = NULL; IOReturn ce;
            IOHIDDeviceScheduleWithRunLoop(kbd, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            ce = IOHIDDeviceGetValueWithCallback(kbd, a, &cv, 1.0, getcb, (void *)0x1234);
            for (i = 0; i < 20 && !g_cbvals; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            IOHIDDeviceUnscheduleFromRunLoop(kbd, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            /* answered at once (macOS and 10.6 alike): the value comes back, the callback is not called */
            res("get_with_callback", !ce && cv && IOHIDValueGetIntegerValue(cv) == 0 && !g_cbvals, "result 0x%x, value %ld, %d callback(s)", ce, cv ? (long)IOHIDValueGetIntegerValue(cv) : -1L, g_cbvals);
        }
        q = IOHIDQueueCreate(NULL, kbd, 32, kIOHIDOptionsTypeNone);
        if (q) {
            IOHIDValueRef qv;
            for (i = 0; keys && i < CFArrayGetCount(keys); i++) IOHIDQueueAddElement(q, (IOHIDElementRef)CFArrayGetValueAtIndex(keys, i));
            IOHIDQueueStart(q);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
            qv = IOHIDQueueCopyNextValueWithTimeout(q, 0.1);
            if (qv) CFRelease(qv);
            IOHIDQueueStop(q);
            res("queue", a && IOHIDQueueContainsElement(q, a) && IOHIDQueueGetDepth(q) == 32 && IOHIDQueueGetDevice(q) == kbd, "depth %ld, contains A %d", (long)IOHIDQueueGetDepth(q), a ? IOHIDQueueContainsElement(q, a) : -1);
            CFRelease(q);
        } else res("queue", 0, "not created");
        if (led) {
            CFMutableDictionaryRef lm = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int lp = kHIDPage_LEDs, lu = kHIDUsage_LED_CapsLock; CFNumberRef a1 = CFNumberCreate(NULL, kCFNumberIntType, &lp), a2 = CFNumberCreate(NULL, kCFNumberIntType, &lu);
            CFArrayRef ls; IOHIDElementRef l = NULL; IOReturn se = -1, se2 = -1; long back = -1;
            CFDictionarySetValue(lm, CFSTR(kIOHIDElementUsagePageKey), a1); CFDictionarySetValue(lm, CFSTR(kIOHIDElementUsageKey), a2);
            ls = IOHIDDeviceCopyMatchingElements(kbd, lm, kIOHIDOptionsTypeNone);
            if (ls && CFArrayGetCount(ls)) l = (IOHIDElementRef)CFArrayGetValueAtIndex(ls, 0);
            if (l) {
                IOHIDValueRef on = IOHIDValueCreateWithIntegerValue(NULL, l, 0, 1), off = IOHIDValueCreateWithIntegerValue(NULL, l, 0, 0), b = NULL;
                IOHIDDeviceScheduleWithRunLoop(kbd, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
                g_setcbs = 0;
                se = IOHIDDeviceSetValueWithCallback(kbd, l, on, 1.0, setcb, (void *)0x5678);
                for (i = 0; i < 20 && !g_setcbs; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
                printf("  set with callback: 0x%x, %d callback(s), value in callback %ld\n", se, g_setcbs, g_setval);
                if (!IOHIDDeviceGetValue(kbd, l, &b) && b) back = IOHIDValueGetIntegerValue(b);
                se2 = IOHIDDeviceSetValue(kbd, l, off);
                CFRelease(on); CFRelease(off);
            }
            res("led", l && !se && back == 1 && !se2, "caps lock LED element %s, set 0x%x, read back %ld, off 0x%x", l ? "found" : "missing", se, back, se2);
        }
        if (watch) {
            IOHIDDeviceRegisterInputValueCallback(kbd, value, NULL);
            IOHIDDeviceScheduleWithRunLoop(kbd, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        }
        IOHIDDeviceClose(kbd, kIOHIDOptionsTypeNone);
    } else res("keyboard", 0, "no keyboard found");
    if (watch) {
        printf("watching input values for %d s\n", watch); fflush(stdout);
        IOHIDManagerRegisterInputValueCallback(m, value, NULL);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, watch, false);
        res("watch", g_values > 0, "%d input value(s)", g_values);
    }
    if (set) CFRelease(set);
    IOHIDManagerClose(m, kIOHIDOptionsTypeNone);
    CFRelease(m);
    return ok_all ? 0 : 1;
}
