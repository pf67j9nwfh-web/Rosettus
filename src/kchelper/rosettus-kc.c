/* rosettus-kc: reads and writes keychain item data for PowerPC programs (liboah's keychain bridge,
   oah_kc.h). securityd refuses keychain item data to translate_mojave: the runtime patches the
   translator's code pages, so the kernel no longer counts it as validly signed, and securityd then
   denies access and suppresses the "allow" prompt ("suppressing keychain prompt for invalidly signed
   client"). This small helper is started for the calls that read or write item data. Items it adds
   trust it, so they read back without a prompt; items made by other programs show the usual
   keychain prompt for it.
   Protocol (native-endian 32-bit words on stdin): op, class, nkc, nkc x {len, path bytes},
   nattr, nattr x {tag, len, bytes}, datalen, data bytes (datalen 0xffffffff: none).
   op 'A' adds an item, 'R' returns the first match's data, 'W' replaces the first match's data.
   Reply on stdout: status, datalen, data bytes.
   Build (Mojave): src/kchelper/build.sh */
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rd(void *p, size_t n) {
    char *c = p;
    while (n) { ssize_t k = read(0, c, n); if (k <= 0) return 0; c += k; n -= (size_t)k; }
    return 1;
}
static int u32(UInt32 *v) { return rd(v, 4); }
static void *blob(UInt32 n) { void *p; if (n > (16u << 20)) return NULL; p = malloc(n ? n : 1); if (p && n && !rd(p, n)) { free(p); return NULL; } return p; }
static void reply(OSStatus e, const void *d, UInt32 n) {
    UInt32 h[2]; h[0] = (UInt32)e; h[1] = d ? n : 0;
    fwrite(h, 4, 2, stdout); if (d && n) fwrite(d, 1, n, stdout); fflush(stdout);
}
int main(void) {
    UInt32 op, cls, nkc, nattr, dlen, i;
    CFMutableArrayRef kcs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    SecKeychainAttributeList al; SecKeychainAttribute *at;
    void *data = NULL; OSStatus e;
    if (!u32(&op) || !u32(&cls) || !u32(&nkc) || nkc > 64) { reply(errSecParam, 0, 0); return 1; }
    for (i = 0; i < nkc; i++) {
        UInt32 n; char *p; SecKeychainRef k = NULL;
        if (!u32(&n) || n > 4096 || !(p = malloc(n + 1)) || (n && !rd(p, n))) { reply(errSecParam, 0, 0); return 1; }
        p[n] = 0;
        if (!SecKeychainOpen(p, &k) && k) { CFArrayAppendValue(kcs, k); CFRelease(k); }
        free(p);
    }
    if (!u32(&nattr) || nattr > 64) { reply(errSecParam, 0, 0); return 1; }
    at = calloc(nattr ? nattr : 1, sizeof *at);
    for (i = 0; i < nattr; i++) {
        if (!u32(&at[i].tag) || !u32(&at[i].length) || !(at[i].data = blob(at[i].length))) { reply(errSecParam, 0, 0); return 1; }
    }
    al.count = nattr; al.attr = at;
    if (!u32(&dlen)) { reply(errSecParam, 0, 0); return 1; }
    if (dlen != 0xffffffffu && !(data = blob(dlen))) { reply(errSecParam, 0, 0); return 1; }
    if (op == 'A') {
        SecKeychainRef k = CFArrayGetCount(kcs) ? (SecKeychainRef)CFArrayGetValueAtIndex(kcs, 0) : NULL;
        e = SecKeychainItemCreateFromContent((SecItemClass)cls, &al, data ? dlen : 0, data, k, NULL, NULL);
        reply(e, 0, 0);
    } else {
        SecKeychainSearchRef s = NULL; SecKeychainItemRef it = NULL;
        e = SecKeychainSearchCreateFromAttributes(CFArrayGetCount(kcs) ? kcs : NULL, (SecItemClass)cls, &al, &s);
        if (!e) e = SecKeychainSearchCopyNext(s, &it);
        if (!e && op == 'R') {
            UInt32 n = 0; void *d = NULL;
            e = SecKeychainItemCopyContent(it, NULL, NULL, &n, &d);
            reply(e, e ? 0 : d, n);
            if (d) SecKeychainItemFreeContent(NULL, d);
        } else if (!e && op == 'W') {
            e = SecKeychainItemModifyAttributesAndData(it, NULL, data ? dlen : 0, data);
            reply(e, 0, 0);
        } else reply(e ? e : errSecParam, 0, 0);
    }
    return 0;
}
