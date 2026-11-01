/* hidold.c - HID devices through the old IOHIDLib plug-in interface (IOHIDDeviceInterface122,
   IOHIDQueueInterface), as games and HID Utilities used it before Mac OS X 10.5.
   1. every IOHIDDevice service: plug-in, QueryInterface for the 1.2.2 interface, copyMatchingElements
   2. the keyboard: open, a key's value (getElementValue), a queue on the key elements with an event
      source added to the run loop, getNextEvent (nothing pressed: underrun), close
   3. with "led": the caps lock LED through setElementValue, read back, off; also through an output
      transaction
   Build: gcc -arch ppc -mmacosx-version-min=10.4 -o hidold hidold.c -framework IOKit -framework CoreFoundation */
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDUsageTables.h>
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
static long dnum(CFDictionaryRef d, const char *k) {
    long v = -1; CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8); CFTypeRef n = CFDictionaryGetValue(d, s);
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberLongType, &v);
    CFRelease(s); return v;
}
static long pnum(io_service_t s, const char *k) {
    long v = -1; CFStringRef ks = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFTypeRef n = IORegistryEntryCreateCFProperty(s, ks, NULL, 0);
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberLongType, &v);
    if (n) CFRelease(n); CFRelease(ks); return v;
}
int main(int argc, char **argv) {
    int led = argc > 1 && !strcmp(argv[1], "led");
    io_iterator_t it = 0; io_service_t s; int ndev = 0, nplug = 0, nel = 0;
    IOHIDDeviceInterface122 **kbd = NULL; CFArrayRef kels = NULL;
    IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(kIOHIDDeviceKey), &it);
    while ((s = IOIteratorNext(it))) {
        IOCFPlugInInterface **p = NULL; SInt32 score = 0; IOHIDDeviceInterface122 **d = NULL; CFArrayRef els = NULL;
        IOReturn e = IOCreatePlugInInterfaceForService(s, kIOHIDDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &p, &score);
        ndev++;
        if (!e && p) {
            HRESULT h = (*p)->QueryInterface(p, CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID122), (LPVOID *)&d);
            if (!h && d) {
                nplug++;
                if ((*d)->copyMatchingElements(d, NULL, &els) == kIOReturnSuccess && els) nel += (int)CFArrayGetCount(els);
                printf("  device %d: usage %ld:%ld, %ld elements\n", ndev, pnum(s, kIOHIDPrimaryUsagePageKey), pnum(s, kIOHIDPrimaryUsageKey), els ? (long)CFArrayGetCount(els) : -1L);
                if (!kbd && pnum(s, kIOHIDPrimaryUsagePageKey) == kHIDPage_GenericDesktop && pnum(s, kIOHIDPrimaryUsageKey) == kHIDUsage_GD_Keyboard) { kbd = d; kels = els; els = NULL; d = NULL; }
            } else printf("  device %d: QueryInterface 0x%lx\n", ndev, (long)h);
            if (els) CFRelease(els);
            if (d) (*d)->Release(d);
            IODestroyPlugInInterface(p);
        } else printf("  device %d: plug-in 0x%x\n", ndev, e);
        IOObjectRelease(s);
    }
    res("plugins", ndev > 0 && nplug == ndev && nel > 0, "%d device(s), %d interface(s), %d elements", ndev, nplug, nel);
    if (kbd) {
        IOReturn oe = (*kbd)->open(kbd, 0), ge = -1, qe = -1, se = -1, ne = -1; IOHIDElementCookie a = 0; IOHIDEventStruct ev; CFIndex i;
        IOHIDQueueInterface **q; CFRunLoopSourceRef src = NULL;
        memset(&ev, 0, sizeof ev);
        for (i = 0; kels && i < CFArrayGetCount(kels); i++) {
            CFDictionaryRef el = (CFDictionaryRef)CFArrayGetValueAtIndex(kels, i);
            if (dnum(el, kIOHIDElementUsagePageKey) == kHIDPage_KeyboardOrKeypad && dnum(el, kIOHIDElementUsageKey) == kHIDUsage_KeyboardA) a = (IOHIDElementCookie)dnum(el, kIOHIDElementCookieKey);
        }
        if (a) ge = (*kbd)->getElementValue(kbd, a, &ev);
        res("keyboard", !oe && a && !ge && ev.elementCookie == a && ev.value == 0, "open 0x%x, A cookie %u, get 0x%x, value %ld", oe, (unsigned)a, ge, (long)ev.value);
        q = (*kbd)->allocQueue(kbd);
        if (q) {
            AbsoluteTime z = { 0, 0 };
            qe = (*q)->create(q, 0, 16);
            if (a) (*q)->addElement(q, a, 0);
            se = (*q)->createAsyncEventSource(q, &src);
            if (src) CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
            (*q)->start(q);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
            ne = (*q)->getNextEvent(q, &ev, z, 0);
            res("queue", !qe && !se && src && (*q)->hasElement(q, a) && ne == kIOReturnUnderrun, "create 0x%x, source 0x%x, has A %d, next 0x%x (underrun expected)", qe, se, a ? (*q)->hasElement(q, a) : -1, ne);
            (*q)->stop(q);
            if (src) CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
            (*q)->dispose(q); (*q)->Release(q);
        } else res("queue", 0, "allocQueue failed");
        if (led) {
            IOHIDElementCookie l = 0; IOReturn s1 = -1, g1 = -1, s2 = -1, c1 = -1; long back = -1, back2 = -1;
            IOHIDOutputTransactionInterface **t;
            for (i = 0; kels && i < CFArrayGetCount(kels); i++) {
                CFDictionaryRef el = (CFDictionaryRef)CFArrayGetValueAtIndex(kels, i);
                if (dnum(el, kIOHIDElementUsagePageKey) == kHIDPage_LEDs && dnum(el, kIOHIDElementUsageKey) == kHIDUsage_LED_CapsLock) l = (IOHIDElementCookie)dnum(el, kIOHIDElementCookieKey);
            }
            if (l) {
                memset(&ev, 0, sizeof ev); ev.type = kIOHIDElementTypeOutput; ev.elementCookie = l; ev.value = 1;
                s1 = (*kbd)->setElementValue(kbd, l, &ev, 0, NULL, NULL, NULL);
                memset(&ev, 0, sizeof ev); g1 = (*kbd)->getElementValue(kbd, l, &ev); back = ev.value;
                ev.value = 0; s2 = (*kbd)->setElementValue(kbd, l, &ev, 0, NULL, NULL, NULL);
                if ((t = (*kbd)->allocOutputTransaction(kbd))) {   /* on and off again through a transaction */
                    (*t)->create(t); (*t)->addElement(t, l);
                    ev.value = 1; (*t)->setElementValue(t, l, &ev); c1 = (*t)->commit(t, 0, NULL, NULL, NULL);
                    memset(&ev, 0, sizeof ev); (*kbd)->getElementValue(kbd, l, &ev); back2 = ev.value;
                    ev.value = 0; (*t)->setElementValue(t, l, &ev); (*t)->commit(t, 0, NULL, NULL, NULL);
                    (*t)->dispose(t); (*t)->Release(t);
                }
            }
            res("led", l && !s1 && !g1 && back == 1 && !s2 && !c1 && back2 == 1, "cookie %u, set 0x%x, read back %ld, off 0x%x; transaction 0x%x, read back %ld", (unsigned)l, s1, back, s2, c1, back2);
        }
        (*kbd)->close(kbd);
        (*kbd)->Release(kbd);
    } else res("keyboard", 0, "no keyboard");
    if (kels) CFRelease(kels);
    return ok_all ? 0 : 1;
}
