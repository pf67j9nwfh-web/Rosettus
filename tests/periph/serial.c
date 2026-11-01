/* serial: PowerPC peripheral test - serial ports (USB-serial adapters) through IOKit and termios.
   Lists IOSerialBSDClient devices, opens each /dev/cu.* that is not Bluetooth with O_NONBLOCK,
   sets 9600 8N1 and reads the settings back. Nothing is written to the port.
   Build: gcc -arch ppc -o serial serial.c -framework IOKit -framework CoreFoundation */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <CoreFoundation/CoreFoundation.h>

int main(void) {
    io_iterator_t it = 0; io_service_t s; int n = 0, opened = 0, ok = 1;
    CFMutableDictionaryRef m = IOServiceMatching(kIOSerialBSDServiceValue);
    CFDictionarySetValue(m, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
    printf("IOServiceGetMatchingServices(IOSerialBSDClient) -> 0x%x\n", IOServiceGetMatchingServices(kIOMasterPortDefault, m, &it));
    while (it && (s = IOIteratorNext(it))) {
        char path[256] = "";
        CFTypeRef t = IORegistryEntryCreateCFProperty(s, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (t) { CFStringGetCString((CFStringRef)t, path, sizeof path, kCFStringEncodingUTF8); CFRelease(t); }
        printf("serial port %s\n", path);
        n++;
        if (path[0] && !strstr(path, "Bluetooth")) {
            int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd < 0) { printf("   open failed\n"); ok = 0; }
            else {
                struct termios o, c;
                tcgetattr(fd, &o); c = o;
                cfmakeraw(&c); cfsetspeed(&c, B9600); c.c_cflag |= CS8 | CLOCAL | CREAD;
                if (tcsetattr(fd, TCSANOW, &c)) printf("   tcsetattr failed\n");
                tcgetattr(fd, &c);
                printf("   opened, speed %lu, CS8 %s\n", (unsigned long)cfgetospeed(&c), (c.c_cflag & CSIZE) == CS8 ? "yes" : "no");
                if (cfgetospeed(&c) == 9600) opened++; else ok = 0;
                tcsetattr(fd, TCSANOW, &o);
                close(fd);
            }
        }
        IOObjectRelease(s);
    }
    printf("RESULT serial %s (%d ports, %d opened)\n", n && ok ? "PASS" : "FAIL", n, opened);
    return n && ok ? 0 : 1;
}
