/* usb_list: PowerPC peripheral test - USB and HID devices through IOKit.
   Prints one line per USB device (vendor:product name) and per HID device (usage page/usage),
   then "RESULT usb N hid M". Build on 10.6: gcc -arch ppc -o usb_list usb_list.c -framework IOKit -framework CoreFoundation */
#include <stdio.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>

static long num(io_registry_entry_t e, CFStringRef key) {
    long v = -1;
    CFTypeRef t = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    if (t && CFGetTypeID(t) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)t, kCFNumberLongType, &v);
    if (t) CFRelease(t);
    return v;
}
static long hnum(IOHIDDeviceRef d, CFStringRef key) {
    long v = -1;
    CFTypeRef t = IOHIDDeviceGetProperty(d, key);
    if (t && CFGetTypeID(t) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)t, kCFNumberLongType, &v);
    return v;
}
static void hstr(IOHIDDeviceRef d, CFStringRef key, char *b, int n) {
    CFTypeRef t = IOHIDDeviceGetProperty(d, key);
    b[0] = 0;
    if (t && CFGetTypeID(t) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)t, b, n, kCFStringEncodingUTF8);
}
int main(void) {
    io_iterator_t it = 0;
    io_service_t s;
    int nusb = 0, nhid = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it);
    printf("IOServiceGetMatchingServices(IOUSBDevice) -> 0x%x\n", kr);
    while (it && (s = IOIteratorNext(it))) {
        io_name_t name;
        if (IORegistryEntryGetName(s, name)) name[0] = 0;
        printf("USB %04lx:%04lx %s\n", num(s, CFSTR(kUSBVendorID)), num(s, CFSTR(kUSBProductID)), name);
        IOObjectRelease(s);
        nusb++;
    }
    if (it) IOObjectRelease(it);
    {   /* the HID services themselves, and IOHIDDeviceCreate on each (what IOHIDManager does) */
        io_iterator_t hi = 0; int ns = 0, nc = 0;
        IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(kIOHIDDeviceKey), &hi);
        while (hi && (s = IOIteratorNext(hi))) {
            IOHIDDeviceRef d = IOHIDDeviceCreate(kCFAllocatorDefault, s);
            ns++; if (d) { nc++; CFRelease(d); }
            IOObjectRelease(s);
        }
        printf("IOHIDDevice services %d, IOHIDDeviceCreate succeeded for %d\n", ns, nc);
    }
    {
        IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        CFSetRef set;
        IOHIDManagerSetDeviceMatching(m, NULL);
        printf("IOHIDManagerOpen -> 0x%x\n", IOHIDManagerOpen(m, kIOHIDOptionsTypeNone));
        set = IOHIDManagerCopyDevices(m);
        if (set) {
            CFIndex c = CFSetGetCount(set), i;
            const void **v = malloc(sizeof(void *) * (c ? c : 1));
            CFSetGetValues(set, v);
            for (i = 0; i < c; i++) {
                char prod[128];
                hstr((IOHIDDeviceRef)v[i], CFSTR(kIOHIDProductKey), prod, sizeof prod);
                printf("HID page 0x%lx usage 0x%lx %04lx:%04lx %s\n", hnum((IOHIDDeviceRef)v[i], CFSTR(kIOHIDPrimaryUsagePageKey)),
                       hnum((IOHIDDeviceRef)v[i], CFSTR(kIOHIDPrimaryUsageKey)), hnum((IOHIDDeviceRef)v[i], CFSTR(kIOHIDVendorIDKey)),
                       hnum((IOHIDDeviceRef)v[i], CFSTR(kIOHIDProductIDKey)), prod);
                nhid++;
            }
            CFRelease(set);
        }
    }
    printf("RESULT usb %d hid %d\n", nusb, nhid);
    return nusb > 0 ? 0 : 1;
}
