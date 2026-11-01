/* oah_kc.h - the keychain for PowerPC apps, served by macOS' own Security framework.

   10.6's PowerPC Security framework talks to securityd in 10.6's protocol; Mojave's securityd does
   not understand it: SecKeychainCopyDefault answered errSecNoDefaultKeychain (-25307), lookups
   failed and SecKeychainCreate returned CSSMERR_CSSM_INTERNAL_ERROR, so apps could neither keep
   nor find passwords (mail and server accounts, licence keys). The keychain calls apps use are
   answered with the native ones (gtrace fix rows on the guest's Security exports): default /
   open / create / delete / lock / unlock / status / path, generic and internet passwords (add,
   find, change, delete), item content and attributes, searches. Keychain and item references
   handed to the guest are small guest CFData objects mapped to the native objects (the guest can
   retain and release them). Data handed out is in memory the guest reads in place, freed by
   SecKeychainItemFreeContent / FreeAttributeInfo as usual.
   OAH_NO_KEYCHAIN=1 leaves the keychain to the guest; OAH_KCLOG=1 logs the calls. */
#include <Security/Security.h>

static int oah_kc_log(void) { static int v = -1; if (v < 0) v = getenv("OAH_KCLOG") ? 1 : 0; return v; }
static int oah_want_keychain(void) { return !getenv("OAH_NO_KEYCHAIN"); }

/* ---- guest proxies for native CF objects ----
   Two dictionaries (native object -> guest proxy; guest proxy -> native object): the
   HID bridge hands out hundreds of element refs per device, too many for a list. A proxy is kept
   for the life of the process (we hold a retain on it), so a released one is never reused. */
static CFMutableDictionaryRef g_kcp_n2g, g_kcp_g2n;
static unsigned int g_kcp_serial = 0x4b430000u;
static pthread_mutex_t g_kcp_mu = PTHREAD_MUTEX_INITIALIZER;
static void oah_kcp_init(void) {
    if (!g_kcp_n2g) {
        /* by identity: IOHIDElement's CFEqual compares cookies, which made the elements of
           different devices one proxy (Security hands out one object per record anyway) */
        CFDictionaryKeyCallBacks kcb = kCFTypeDictionaryKeyCallBacks;
        kcb.equal = NULL; kcb.hash = NULL;
        g_kcp_n2g = CFDictionaryCreateMutable(NULL, 0, &kcb, NULL);
        g_kcp_g2n = CFDictionaryCreateMutable(NULL, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    }
}
static unsigned int oah_kc_wrap(CFTypeRef n) {
    static unsigned int fdc, frt;
    unsigned int a[3], b[2], g; const void *v;
    if (!n) return 0;
    pthread_mutex_lock(&g_kcp_mu);
    oah_kcp_init();
    if (CFDictionaryGetValueIfPresent(g_kcp_n2g, n, &v)) { pthread_mutex_unlock(&g_kcp_mu); return (unsigned int)(uintptr_t)v; }
    pthread_mutex_unlock(&g_kcp_mu);
    if (!fdc) { fdc = oah_guest_sym("CoreFoundation", "_CFDataCreate"); frt = oah_guest_sym("CoreFoundation", "_CFRetain"); }
    if (!fdc) return 0;
    b[0] = OSSwapHostToBigInt32(++g_kcp_serial); b[1] = OSSwapHostToBigInt32(0x6b637278);
    a[0] = 0; a[1] = (unsigned int)(uintptr_t)b; a[2] = 8;
    if (!(g = oah_callppc_n("kc_wrap", fdc, 3, a))) return 0;
    if (frt) { a[0] = g; oah_callppc_n("kc_keep", frt, 1, a); }   /* ours: a released wrapper is never reused */
    pthread_mutex_lock(&g_kcp_mu);
    if (CFDictionaryGetValueIfPresent(g_kcp_n2g, n, &v)) g = (unsigned int)(uintptr_t)v;   /* another thread was quicker (ours leaks) */
    else {
        CFDictionarySetValue(g_kcp_n2g, n, (const void *)(uintptr_t)g);
        CFDictionarySetValue(g_kcp_g2n, (const void *)(uintptr_t)g, n);
    }
    pthread_mutex_unlock(&g_kcp_mu);
    return g;
}
/* for a Create / Copy result: the proxy with a retain of the caller's own */
static unsigned int oah_kc_wrap_own(CFTypeRef n) {
    static unsigned int frt;
    unsigned int g = oah_kc_wrap(n), a[1];
    if (!frt) frt = oah_guest_sym("CoreFoundation", "_CFRetain");
    if (g && frt) { a[0] = g; oah_callppc_n("kc_own", frt, 1, a); }
    return g;
}
static CFTypeRef oah_kc_unwrap(unsigned int g) {
    CFTypeRef n = NULL;
    if (!g || !g_kcp_g2n) return NULL;
    pthread_mutex_lock(&g_kcp_mu);
    n = (CFTypeRef)CFDictionaryGetValue(g_kcp_g2n, (const void *)(uintptr_t)g);
    pthread_mutex_unlock(&g_kcp_mu);
    return n;
}
/* a keychain-or-array argument: NULL (the search list), one of our keychains, or a guest CFArray of them */
static CFTypeRef oah_kc_kcarg(unsigned int g, int *bad) {
    static unsigned int fcnt, fget, ftid, fatid;
    CFTypeRef n; unsigned int a[2], cnt, i;
    CFMutableArrayRef arr;
    *bad = 0;
    if (!g) return NULL;
    if ((n = oah_kc_unwrap(g))) return CFRetain(n);
    if (!fcnt) { fcnt = oah_guest_sym("CoreFoundation", "_CFArrayGetCount"); fget = oah_guest_sym("CoreFoundation", "_CFArrayGetValueAtIndex");
                 ftid = oah_guest_sym("CoreFoundation", "_CFGetTypeID"); fatid = oah_guest_sym("CoreFoundation", "_CFArrayGetTypeID"); }
    a[0] = g;
    if (!ftid || !fatid || oah_callppc_n("kc_tid", ftid, 1, a) != oah_callppc_n("kc_atid", fatid, 0, a)) { *bad = 1; return NULL; }
    cnt = oah_callppc_n("kc_cnt", fcnt, 1, a);
    arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (i = 0; i < cnt && i < 64; i++) {
        a[0] = g; a[1] = i;
        if ((n = oah_kc_unwrap(oah_callppc_n("kc_get", fget, 2, a)))) CFArrayAppendValue(arr, n);
    }
    return arr;
}
static unsigned int oah_kc_be(unsigned int a) { return (a && oah_readable(a, 4)) ? OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)a) : 0; }
static void oah_kc_put(unsigned int a, unsigned int v) { if (a && oah_readable(a, 4)) *(unsigned int *)(uintptr_t)a = OSSwapHostToBigInt32(v); }
/* PowerPC argument k (0-based): r3..r10, then the caller's parameter area */
static unsigned int oah_kc_arg(unsigned int *R, int k) { return k < 8 ? R[3 + k] : oah_kc_be(R[1] + 24 + 4 * k); }
/* a copy of native bytes in memory the guest reads (freed by oah_kc_free) */
static void *oah_kc_copy(const void *d, size_t n) { void *p = malloc(n ? n : 1); if (p && n) memcpy(p, d, n); return p; }

/* attribute lists: the guest's (big-endian {count, attr*}, attr = {tag, length, data*}) as a native
   one and back. Guest data pointers are read in place (same address space). */
/* attributes whose 4-byte value is an integer (or a four-character code kept as one): byte-swapped */
static int oah_kc_numattr(UInt32 tag) {
    return tag == 'port' || tag == 'ptcl' || tag == 'atyp' || tag == 'crtr' || tag == 'type' || tag == 'invi' ||
           tag == 'nega' || tag == 'cusi' || tag == 'scrp';
}
static SecKeychainAttributeList *oah_kc_attrs_in(unsigned int g) {
    unsigned int cnt, ap, i;
    SecKeychainAttributeList *l;
    if (!g || !oah_readable(g, 8)) return NULL;
    cnt = oah_kc_be(g); ap = oah_kc_be(g + 4);
    if (cnt > 64 || (cnt && !oah_readable(ap, cnt * 12))) return NULL;
    l = (SecKeychainAttributeList *)calloc(1, sizeof *l + cnt * (sizeof(SecKeychainAttribute) + 4));
    l->count = cnt; l->attr = (SecKeychainAttribute *)(l + 1);
    for (i = 0; i < cnt; i++) {
        UInt32 *num = (UInt32 *)(l->attr + cnt) + i;   /* room for a swapped integer, inside the same block */
        l->attr[i].tag = oah_kc_be(ap + 12 * i); l->attr[i].length = oah_kc_be(ap + 12 * i + 4);
        l->attr[i].data = (void *)(uintptr_t)oah_kc_be(ap + 12 * i + 8);
        if (l->attr[i].length == 4 && oah_kc_numattr(l->attr[i].tag) && l->attr[i].data && oah_readable((unsigned int)(uintptr_t)l->attr[i].data, 4)) {
            *num = OSSwapBigToHostInt32(*(UInt32 *)l->attr[i].data); l->attr[i].data = num;
        }
    }
    return l;
}
static void oah_kc_attrs_free_in(SecKeychainAttributeList *l) { free(l); }
/* native attribute values as a guest list {count, attr*} in guest-readable memory */
static unsigned int oah_kc_attrs_out(SecKeychainAttributeList *l) {
    unsigned int *gl, *ga, i;
    if (!l) return 0;
    gl = (unsigned int *)malloc(8); ga = (unsigned int *)calloc(l->count ? l->count : 1, 12);
    gl[0] = OSSwapHostToBigInt32(l->count); gl[1] = OSSwapHostToBigInt32((unsigned int)(uintptr_t)ga);
    for (i = 0; i < l->count; i++) {
        SecKeychainAttribute *a = &l->attr[i];
        void *d = oah_kc_copy(a->data, a->length);
        if (a->length == 4 && d && oah_kc_numattr(a->tag)) *(UInt32 *)d = OSSwapHostToBigInt32(*(UInt32 *)d);   /* integers to big-endian */
        ga[3 * i] = OSSwapHostToBigInt32(a->tag); ga[3 * i + 1] = OSSwapHostToBigInt32(a->length);
        ga[3 * i + 2] = OSSwapHostToBigInt32(a->data ? (unsigned int)(uintptr_t)d : 0);
    }
    return (unsigned int)(uintptr_t)gl;
}
static void oah_kc_attrs_free_out(unsigned int g) {
    unsigned int cnt, ap, i;
    if (!g || !oah_readable(g, 8)) return;
    cnt = oah_kc_be(g); ap = oah_kc_be(g + 4);
    for (i = 0; i < cnt && i < 256; i++) free((void *)(uintptr_t)oah_kc_be(ap + 12 * i + 8));
    free((void *)(uintptr_t)ap); free((void *)(uintptr_t)g);
}
/* a guest SecKeychainAttributeInfo {count, UInt32 *tag, UInt32 *format} as native */
static SecKeychainAttributeInfo *oah_kc_info_in(unsigned int g) {
    unsigned int cnt, tp, fp, i; SecKeychainAttributeInfo *n;
    if (!g || !oah_readable(g, 12)) return NULL;
    cnt = oah_kc_be(g); tp = oah_kc_be(g + 4); fp = oah_kc_be(g + 8);
    if (cnt > 64) return NULL;
    n = (SecKeychainAttributeInfo *)calloc(1, sizeof *n + 2 * cnt * sizeof(UInt32));
    n->count = cnt; n->tag = (UInt32 *)(n + 1); n->format = n->tag + cnt;
    for (i = 0; i < cnt; i++) { n->tag[i] = oah_kc_be(tp + 4 * i); n->format[i] = fp ? oah_kc_be(fp + 4 * i) : 0; }
    return n;
}

/* Item data through rosettus-kc (native/rosettus-kc, src/kchelper): securityd hands keychain item data
   only to validly signed clients, and translate_mojave stops being one when the runtime patches its
   code pages ("suppressing keychain prompt for invalidly signed client"): every read of a password
   failed with errSecAuthFailed. Attributes, searches and item refs still work here; the data is read
   and written by the helper, which is also what items added from here trust. OAH_KC_NOHELPER=1 does
   everything in this process. */
#include <spawn.h>
#include <sys/wait.h>
static int oah_kc_nohelper(void) { static int v = -1; if (v < 0) v = getenv("OAH_KC_NOHELPER") ? 1 : 0; return v; }
typedef struct { unsigned char *p; size_t n, cap; } oah_kcbuf;
static void oah_kcb_put(oah_kcbuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->p = (unsigned char *)realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n); b->n += n;
}
static void oah_kcb_u32(oah_kcbuf *b, UInt32 v) { oah_kcb_put(b, &v, 4); }
static void oah_kcb_kc(oah_kcbuf *b, CFTypeRef k) {   /* a keychain's path */
    char buf[1024]; UInt32 n = sizeof buf - 1;
    if (SecKeychainGetPath((SecKeychainRef)k, &n, buf)) n = 0;
    oah_kcb_u32(b, n); oah_kcb_put(b, buf, n);
}
/* runs the helper: 1 when it ran (status in *st), 0 when it could not be started */
static int oah_kc_helper(UInt32 op, SecItemClass cls, CFTypeRef kcs, const SecKeychainAttributeList *l,
                         const void *d, UInt32 dl, int hasdata, void **out, UInt32 *outlen, OSStatus *st) {
    static char path[1100]; static int ok = -1;
    oah_kcbuf b = { 0, 0, 0 };
    int in[2], outp[2], status; pid_t pid; UInt32 i, h[2];
    posix_spawn_file_actions_t fa;
    char *argv[2], *envp[3], home[1100];
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (ok < 0) { snprintf(path, sizeof path, "%s/native/rosettus-kc", g_rosetta_root); ok = access(path, X_OK) == 0 && !oah_kc_nohelper(); }
    if (!ok) return 0;
    oah_kcb_u32(&b, op); oah_kcb_u32(&b, (UInt32)cls);
    if (!kcs) oah_kcb_u32(&b, 0);
    else if (CFGetTypeID(kcs) == CFArrayGetTypeID()) {
        CFIndex c = CFArrayGetCount((CFArrayRef)kcs), k;
        oah_kcb_u32(&b, (UInt32)c);
        for (k = 0; k < c; k++) oah_kcb_kc(&b, CFArrayGetValueAtIndex((CFArrayRef)kcs, k));
    } else { oah_kcb_u32(&b, 1); oah_kcb_kc(&b, kcs); }
    oah_kcb_u32(&b, l ? l->count : 0);
    for (i = 0; l && i < l->count; i++) {
        oah_kcb_u32(&b, l->attr[i].tag); oah_kcb_u32(&b, l->attr[i].data ? l->attr[i].length : 0);
        if (l->attr[i].data) oah_kcb_put(&b, l->attr[i].data, l->attr[i].length);
    }
    oah_kcb_u32(&b, hasdata ? dl : 0xffffffffu);
    if (hasdata && dl) oah_kcb_put(&b, d, dl);
    if (pipe(in)) { free(b.p); return 0; }
    if (pipe(outp)) { close(in[0]); close(in[1]); free(b.p); return 0; }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0); posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]); posix_spawn_file_actions_addclose(&fa, outp[0]);
    argv[0] = path; argv[1] = NULL;
    /* a clean environment: ours carries the translator's DYLD_ variables */
    snprintf(home, sizeof home, "HOME=%s", getenv("HOME") ? getenv("HOME") : "/");
    envp[0] = home; envp[1] = (char *)"PATH=/usr/bin:/bin"; envp[2] = NULL;
    status = posix_spawn(&pid, path, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]); close(outp[1]);
    if (status) { close(in[1]); close(outp[0]); free(b.p); return 0; }
    {   size_t o = 0; while (o < b.n) { ssize_t k = write(in[1], b.p + o, b.n - o); if (k <= 0) break; o += (size_t)k; } }
    close(in[1]); free(b.p);
    {   size_t o = 0; char *c = (char *)h; while (o < 8) { ssize_t k = read(outp[0], c + o, 8 - o); if (k <= 0) break; o += (size_t)k; }
        if (o < 8) { h[0] = (UInt32)errSecInteractionNotAllowed; h[1] = 0; } }
    if (h[1] && h[1] < (64u << 20)) {
        void *p = malloc(h[1]); size_t o = 0;
        while (p && o < h[1]) { ssize_t k = read(outp[0], (char *)p + o, h[1] - o); if (k <= 0) break; o += (size_t)k; }
        if (p && o == h[1] && out) { *out = p; if (outlen) *outlen = h[1]; } else free(p);
    }
    close(outp[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *st = (OSStatus)h[0];
    if (oah_kc_log()) fprintf(stderr, "[keychain] helper %c -> %d (%u bytes)\n", (int)op, (int)*st, (unsigned)h[1]);
    return 1;
}
/* the attributes that find an item again: its class, keychain and key attributes */
static SecKeychainAttributeList *oah_kc_itemkey(SecKeychainItemRef it, SecItemClass *cls, SecKeychainRef *kc) {
    static UInt32 gtags[] = { kSecServiceItemAttr, kSecAccountItemAttr };
    static UInt32 itags[] = { kSecServerItemAttr, kSecSecurityDomainItemAttr, kSecAccountItemAttr, kSecPathItemAttr,
                              kSecPortItemAttr, kSecProtocolItemAttr, kSecAuthenticationTypeItemAttr };
    static UInt32 fmt[8];
    SecKeychainAttributeInfo info; SecKeychainAttributeList *l = NULL;
    *cls = 0; *kc = NULL;
    if (!it || SecKeychainItemCopyContent(it, cls, NULL, NULL, NULL)) return NULL;
    if (*cls == kSecGenericPasswordItemClass) { info.count = 2; info.tag = gtags; }
    else if (*cls == kSecInternetPasswordItemClass) { info.count = 7; info.tag = itags; }
    else return NULL;
    info.format = fmt;
    if (SecKeychainItemCopyAttributesAndData(it, &info, NULL, &l, NULL, NULL)) return NULL;
    {   UInt32 i, j = 0;   /* unset strings would match only empty ones: left out */
        for (i = 0; i < l->count; i++) if (l->attr[i].length && l->attr[i].data) l->attr[j++] = l->attr[i];
        l->count = j; }
    SecKeychainItemCopyKeychain(it, kc);
    return l;
}
/* an item's data through the helper; 0 = the helper is not there (do it here) */
static int oah_kc_item_data(SecKeychainItemRef it, void **d, UInt32 *len, OSStatus *e) {
    SecItemClass cls; SecKeychainRef kc; SecKeychainAttributeList *l = oah_kc_itemkey(it, &cls, &kc); int r;
    if (!l) { if (kc) CFRelease(kc); return 0; }
    r = oah_kc_helper('R', cls, kc, l, NULL, 0, 0, d, len, e);
    SecKeychainItemFreeAttributesAndData(l, NULL); if (kc) CFRelease(kc);
    return r;
}
/* An item another process changed is stale here (-25300), and so is every search result for it
   while that ref lives (Security caches one object per record): the guest's ref is let go, the item
   found again, and the ref pointed at the new object. */
static void oah_kc_unwrap_drop(unsigned int g) {
    CFTypeRef n;
    pthread_mutex_lock(&g_kcp_mu);
    if (g_kcp_g2n && (n = (CFTypeRef)CFDictionaryGetValue(g_kcp_g2n, (const void *)(uintptr_t)g))) {
        CFRetain(n);
        CFDictionaryRemoveValue(g_kcp_n2g, n); CFDictionaryRemoveValue(g_kcp_g2n, (const void *)(uintptr_t)g);
        pthread_mutex_unlock(&g_kcp_mu);
        CFRelease(n);   /* the last native ref to the stale object: a new search makes a fresh one */
        return;
    }
    pthread_mutex_unlock(&g_kcp_mu);
}
static void oah_kc_rewrap(unsigned int g, CFTypeRef n) {
    pthread_mutex_lock(&g_kcp_mu);
    oah_kcp_init();
    CFDictionarySetValue(g_kcp_n2g, n, (const void *)(uintptr_t)g);
    CFDictionarySetValue(g_kcp_g2n, (const void *)(uintptr_t)g, n);
    pthread_mutex_unlock(&g_kcp_mu);
}
static int oah_kc_item_setdata(unsigned int g, SecKeychainItemRef it, const void *d, UInt32 len, OSStatus *e) {
    SecItemClass cls; SecKeychainRef kc; SecKeychainAttributeList *l = oah_kc_itemkey(it, &cls, &kc); int r;
    if (!l) { if (kc) CFRelease(kc); return 0; }
    r = oah_kc_helper('W', cls, kc, l, d, len, 1, NULL, NULL, e);
    if (r && !*e) {
        SecKeychainSearchRef s = NULL; SecKeychainItemRef ni = NULL;
        oah_kc_unwrap_drop(g);
        if (!SecKeychainSearchCreateFromAttributes(kc, cls, l, &s) && !SecKeychainSearchCopyNext(s, &ni) && ni) { oah_kc_rewrap(g, ni); CFRelease(ni); }
        if (s) CFRelease(s);
    }
    SecKeychainItemFreeAttributesAndData(l, NULL); if (kc) CFRelease(kc);
    return r;
}
/* adds an item through the helper and finds it here (for the item ref); 0 = no helper */
static int oah_kc_add(SecItemClass cls, SecKeychainRef kc, SecKeychainAttributeList *l, const void *d, UInt32 len,
                      SecKeychainItemRef *it, OSStatus *e) {
    if (!oah_kc_helper('A', cls, kc, l, d, len, 1, NULL, NULL, e)) return 0;
    if (!*e && it) {
        SecKeychainSearchRef s = NULL; OSStatus e2;
        CFTypeRef ks = kc ? (CFTypeRef)kc : NULL;
        if (!kc) { SecKeychainRef dk = NULL; if (!SecKeychainCopyDefault(&dk)) ks = dk; }
        e2 = SecKeychainSearchCreateFromAttributes(ks, cls, l, &s);
        if (!e2) e2 = SecKeychainSearchCopyNext(s, it);
        if (e2) *it = NULL;
        if (s) CFRelease(s);
        if (!kc && ks) CFRelease(ks);
    }
    return 1;
}
static void oah_kc_attr(SecKeychainAttribute *a, UInt32 tag, UInt32 len, const void *d) { a->tag = tag; a->length = len; a->data = (void *)d; }

enum { KC_COPYDEFAULT, KC_OPEN, KC_CREATE, KC_DELETE, KC_UNLOCK, KC_LOCK, KC_STATUS, KC_PATH, KC_ADDGEN, KC_FINDGEN,
       KC_ADDINET, KC_FINDINET, KC_FREECONTENT, KC_MODIFY, KC_ITEMDELETE, KC_COPYCONTENT, KC_COPYATTRDATA, KC_FREEATTRDATA,
       KC_SEARCHCREATE, KC_SEARCHNEXT, KC_SETUI, KC_GETUI, KC_ITEMKEYCHAIN, KC_ITEMCREATE, KC_SEARCHLIST, KC_N };
static const char *const g_kc_name[KC_N] = { "SecKeychainCopyDefault", "SecKeychainOpen", "SecKeychainCreate", "SecKeychainDelete",
    "SecKeychainUnlock", "SecKeychainLock", "SecKeychainGetStatus", "SecKeychainGetPath", "SecKeychainAddGenericPassword",
    "SecKeychainFindGenericPassword", "SecKeychainAddInternetPassword", "SecKeychainFindInternetPassword", "SecKeychainItemFreeContent",
    "SecKeychainItemModifyAttributesAndData", "SecKeychainItemDelete", "SecKeychainItemCopyContent", "SecKeychainItemCopyAttributesAndData",
    "SecKeychainItemFreeAttributesAndData", "SecKeychainSearchCreateFromAttributes", "SecKeychainSearchCopyNext",
    "SecKeychainSetUserInteractionAllowed", "SecKeychainGetUserInteractionAllowed", "SecKeychainItemCopyKeychain", "SecKeychainItemCreateFromContent",
    "SecKeychainCopySearchList" };
static void oah_kc_call(int which, unsigned int *R) {
    OSStatus e = 0;
    g_gt_skip = 1;
    switch (which) {
    case KC_COPYDEFAULT: { SecKeychainRef k = NULL; e = SecKeychainCopyDefault(&k); oah_kc_put(R[3], e ? 0 : oah_kc_wrap_own(k)); if (k) CFRelease(k); break; }
    case KC_OPEN: { SecKeychainRef k = NULL; const char *p = (const char *)(uintptr_t)R[3];
        e = (p && oah_readable(R[3], 1)) ? SecKeychainOpen(p, &k) : errSecParam; oah_kc_put(R[4], e ? 0 : oah_kc_wrap_own(k)); if (k) CFRelease(k); break; }
    case KC_CREATE: { SecKeychainRef k = NULL;   /* (path, pwLen, pw, prompt, initialAccess, out) */
        e = SecKeychainCreate((const char *)(uintptr_t)R[3], R[4], (const void *)(uintptr_t)R[5], (Boolean)(R[6] & 0xff), NULL, &k);
        oah_kc_put(R[8], e ? 0 : oah_kc_wrap_own(k)); if (k) CFRelease(k); break; }
    case KC_DELETE: case KC_LOCK: { CFTypeRef k = oah_kc_unwrap(R[3]);
        if (!k && R[3]) { e = errSecInvalidKeychain; break; }
        e = which == KC_DELETE ? SecKeychainDelete((SecKeychainRef)k) : SecKeychainLock((SecKeychainRef)k); break; }
    case KC_UNLOCK: { CFTypeRef k = oah_kc_unwrap(R[3]);
        e = SecKeychainUnlock((SecKeychainRef)k, R[4], (const void *)(uintptr_t)R[5], (Boolean)(R[6] & 0xff)); break; }
    case KC_STATUS: { SecKeychainStatus s = 0; e = SecKeychainGetStatus((SecKeychainRef)oah_kc_unwrap(R[3]), &s); oah_kc_put(R[4], s); break; }
    case KC_PATH: { char buf[1024]; UInt32 n = sizeof buf, cap = oah_kc_be(R[4]);
        e = SecKeychainGetPath((SecKeychainRef)oah_kc_unwrap(R[3]), &n, buf);
        if (!e) { if (n + 1 > cap) e = errSecBufferTooSmall; else if (oah_readable(R[5], n + 1)) { memcpy((void *)(uintptr_t)R[5], buf, n); ((char *)(uintptr_t)R[5])[n] = 0; } oah_kc_put(R[4], n); }
        break; }
    case KC_ADDGEN: { SecKeychainItemRef it = NULL;   /* (kc, svcLen, svc, accLen, acc, pwLen, pw, out) */
        SecKeychainAttribute at[2]; SecKeychainAttributeList l = { 0, at };
        if (R[4]) oah_kc_attr(&at[l.count++], kSecServiceItemAttr, R[4], (const void *)(uintptr_t)R[5]);
        if (R[6]) oah_kc_attr(&at[l.count++], kSecAccountItemAttr, R[6], (const void *)(uintptr_t)R[7]);
        if (!oah_kc_add(kSecGenericPasswordItemClass, (SecKeychainRef)oah_kc_unwrap(R[3]), &l, (const void *)(uintptr_t)R[9], R[8], R[10] ? &it : NULL, &e))
        e = SecKeychainAddGenericPassword((SecKeychainRef)oah_kc_unwrap(R[3]), R[4], (const char *)(uintptr_t)R[5], R[6], (const char *)(uintptr_t)R[7],
                                          R[8], (const void *)(uintptr_t)R[9], R[10] ? &it : NULL);
        if (R[10]) oah_kc_put(R[10], e ? 0 : oah_kc_wrap_own(it));
        if (it) CFRelease(it);
        break; }
    case KC_FINDGEN: { SecKeychainItemRef it = NULL; UInt32 len = 0; void *d = NULL; int bad; CFTypeRef ks = oah_kc_kcarg(R[3], &bad);
        unsigned int plen = R[8], pdata = R[9], pit = R[10];   /* (kcs, svcLen, svc, accLen, acc, *pwLen, **pw, *item) */
        if (bad) { e = errSecParam; break; }
        e = SecKeychainFindGenericPassword(ks, R[4], (const char *)(uintptr_t)R[5], R[6], (const char *)(uintptr_t)R[7],
                                           NULL, NULL, &it);
        if (!e && (plen || pdata) && !oah_kc_item_data(it, &d, &len, &e)) {
            if (it) CFRelease(it);
            it = NULL;
            e = SecKeychainFindGenericPassword(ks, R[4], (const char *)(uintptr_t)R[5], R[6], (const char *)(uintptr_t)R[7],
                                               plen ? &len : NULL, pdata ? &d : NULL, pit ? &it : NULL);
            if (d) { void *c = oah_kc_copy(d, len); SecKeychainItemFreeContent(NULL, d); d = c; }
        }
        if (!e) {
            if (plen) oah_kc_put(plen, len);
            if (pdata) { oah_kc_put(pdata, (unsigned int)(uintptr_t)(d ? d : oah_kc_copy("", 0))); d = NULL; }
            if (pit) oah_kc_put(pit, oah_kc_wrap_own(it));
        }
        free(d);
        if (it) CFRelease(it);
        if (ks) CFRelease(ks);
        break; }
    case KC_ADDINET: case KC_FINDINET: {
        /* Add: (kc, serverLen, server, domLen, dom, accLen, acc, pathLen, path, port, protocol, auth, pwLen, pw, *item)
           Find: (kcs, serverLen, server, domLen, dom, accLen, acc, pathLen, path, port, protocol, auth, *pwLen, **pw, *item) */
        unsigned int A[15]; int k, bad = 0; SecKeychainItemRef it = NULL; CFTypeRef ks;
        for (k = 0; k < 15; k++) A[k] = oah_kc_arg(R, k);
        if (which == KC_ADDINET) {
            SecKeychainAttribute at[7]; SecKeychainAttributeList l = { 0, at }; UInt32 port = (UInt16)A[9], ptcl = A[10], auth = A[11];
            if (A[1]) oah_kc_attr(&at[l.count++], kSecServerItemAttr, A[1], (const void *)(uintptr_t)A[2]);
            if (A[3]) oah_kc_attr(&at[l.count++], kSecSecurityDomainItemAttr, A[3], (const void *)(uintptr_t)A[4]);
            if (A[5]) oah_kc_attr(&at[l.count++], kSecAccountItemAttr, A[5], (const void *)(uintptr_t)A[6]);
            if (A[7]) oah_kc_attr(&at[l.count++], kSecPathItemAttr, A[7], (const void *)(uintptr_t)A[8]);
            oah_kc_attr(&at[l.count++], kSecPortItemAttr, 4, &port);
            oah_kc_attr(&at[l.count++], kSecProtocolItemAttr, 4, &ptcl);
            oah_kc_attr(&at[l.count++], kSecAuthenticationTypeItemAttr, 4, &auth);
            if (!oah_kc_add(kSecInternetPasswordItemClass, (SecKeychainRef)oah_kc_unwrap(A[0]), &l, (const void *)(uintptr_t)A[13], A[12], A[14] ? &it : NULL, &e))
            e = SecKeychainAddInternetPassword((SecKeychainRef)oah_kc_unwrap(A[0]), A[1], (const char *)(uintptr_t)A[2], A[3], (const char *)(uintptr_t)A[4],
                    A[5], (const char *)(uintptr_t)A[6], A[7], (const char *)(uintptr_t)A[8], (UInt16)A[9], A[10], A[11], A[12], (const void *)(uintptr_t)A[13], A[14] ? &it : NULL);
            if (A[14]) oah_kc_put(A[14], e ? 0 : oah_kc_wrap_own(it));
        } else {
            UInt32 len = 0; void *d = NULL;
            ks = oah_kc_kcarg(A[0], &bad);
            if (bad) { e = errSecParam; break; }
            e = SecKeychainFindInternetPassword(ks, A[1], (const char *)(uintptr_t)A[2], A[3], (const char *)(uintptr_t)A[4], A[5], (const char *)(uintptr_t)A[6],
                    A[7], (const char *)(uintptr_t)A[8], (UInt16)A[9], A[10], A[11], NULL, NULL, &it);
            if (!e && (A[12] || A[13]) && !oah_kc_item_data(it, &d, &len, &e)) {
                if (it) CFRelease(it);
                it = NULL;
                e = SecKeychainFindInternetPassword(ks, A[1], (const char *)(uintptr_t)A[2], A[3], (const char *)(uintptr_t)A[4], A[5], (const char *)(uintptr_t)A[6],
                        A[7], (const char *)(uintptr_t)A[8], (UInt16)A[9], A[10], A[11], A[12] ? &len : NULL, A[13] ? &d : NULL, A[14] ? &it : NULL);
                if (d) { void *c = oah_kc_copy(d, len); SecKeychainItemFreeContent(NULL, d); d = c; }
            }
            if (!e) {
                if (A[12]) oah_kc_put(A[12], len);
                if (A[13]) { oah_kc_put(A[13], (unsigned int)(uintptr_t)(d ? d : oah_kc_copy("", 0))); d = NULL; }
                if (A[14]) oah_kc_put(A[14], oah_kc_wrap_own(it));
            }
            free(d);
            if (ks) CFRelease(ks);
        }
        if (it) CFRelease(it);
        break; }
    case KC_FREECONTENT:   /* (attrList, data): both ours */
        if (R[3]) oah_kc_attrs_free_out(R[3]);
        if (R[4]) free((void *)(uintptr_t)R[4]);
        break;
    case KC_MODIFY: { SecKeychainAttributeList *l = oah_kc_attrs_in(R[4]);   /* (item, attrList, len, data) */
        SecKeychainItemRef it = (SecKeychainItemRef)oah_kc_unwrap(R[3]);
        if (R[6] && it) {   /* attributes here, the data through the helper */
            e = l ? SecKeychainItemModifyAttributesAndData(it, l, 0, NULL) : 0;
            if (!e && !oah_kc_item_setdata(R[3], it, (const void *)(uintptr_t)R[6], R[5], &e)) e = SecKeychainItemModifyAttributesAndData(it, NULL, R[5], (const void *)(uintptr_t)R[6]);
        } else
        e = SecKeychainItemModifyAttributesAndData(it, l, R[5], R[6] ? (const void *)(uintptr_t)R[6] : NULL);
        oah_kc_attrs_free_in(l); break; }
    case KC_ITEMDELETE: { CFTypeRef it = oah_kc_unwrap(R[3]); e = it ? SecKeychainItemDelete((SecKeychainItemRef)it) : errSecInvalidItemRef; break; }
    case KC_COPYCONTENT: {   /* (item, *itemClass, attrList in/out, *len, **data) */
        SecItemClass cls = 0; UInt32 len = 0; void *d = NULL; SecKeychainAttributeList *l = oah_kc_attrs_in(R[5]);
        if (l) { UInt32 i; for (i = 0; i < l->count; i++) { l->attr[i].length = 0; l->attr[i].data = NULL; } }
        void *hd = NULL; int viah = 0; SecKeychainItemRef it = (SecKeychainItemRef)oah_kc_unwrap(R[3]);
        if ((R[6] || R[7]) && it && oah_kc_item_data(it, &hd, &len, &e)) { viah = 1; if (e) { free(l); break; } }
        e = SecKeychainItemCopyContent(it, R[4] ? &cls : NULL, l, (R[6] && !viah) ? &len : NULL, (R[7] && !viah) ? &d : NULL);
        if (!e) {
            if (R[4]) oah_kc_put(R[4], cls);
            if (R[6]) oah_kc_put(R[6], len);
            if (R[7]) { oah_kc_put(R[7], (unsigned int)(uintptr_t)(viah ? (hd ? hd : oah_kc_copy("", 0)) : oah_kc_copy(d, len))); hd = NULL; }
            if (l) {   /* the values into the guest's own attribute array (data in memory of ours, freed with FreeContent) */
                unsigned int ap = oah_kc_be(R[5] + 4); UInt32 i;
                for (i = 0; i < l->count; i++) {
                    void *v = oah_kc_copy(l->attr[i].data, l->attr[i].length);
                    if (l->attr[i].length == 4 && v && oah_kc_numattr(l->attr[i].tag)) *(UInt32 *)v = OSSwapHostToBigInt32(*(UInt32 *)v);
                    oah_kc_put(ap + 12 * i + 4, l->attr[i].length); oah_kc_put(ap + 12 * i + 8, l->attr[i].data ? (unsigned int)(uintptr_t)v : 0);
                }
            }
        }
        if (d || l) SecKeychainItemFreeContent(l, d);
        free(l); free(hd);
        break; }
    case KC_COPYATTRDATA: {   /* (item, info, *itemClass, **attrList, *len, **data) */
        SecItemClass cls = 0; UInt32 len = 0; void *d = NULL; SecKeychainAttributeList *l = NULL; SecKeychainAttributeInfo *info = oah_kc_info_in(R[4]);
        void *hd = NULL; int viah = 0; SecKeychainItemRef it = (SecKeychainItemRef)oah_kc_unwrap(R[3]);
        if ((R[7] || R[8]) && it && oah_kc_item_data(it, &hd, &len, &e)) { viah = 1; if (e) { free(info); break; } }
        e = SecKeychainItemCopyAttributesAndData(it, info, R[5] ? &cls : NULL, R[6] ? &l : NULL, (R[7] && !viah) ? &len : NULL, (R[8] && !viah) ? &d : NULL);
        if (!e) {
            if (R[5]) oah_kc_put(R[5], cls);
            if (R[6]) oah_kc_put(R[6], oah_kc_attrs_out(l));
            if (R[7]) oah_kc_put(R[7], len);
            if (R[8]) { oah_kc_put(R[8], (unsigned int)(uintptr_t)(viah ? (hd ? hd : oah_kc_copy("", 0)) : oah_kc_copy(d, len))); hd = NULL; }
        }
        if (l || d) SecKeychainItemFreeAttributesAndData(l, d);
        free(info); free(hd);
        break; }
    case KC_FREEATTRDATA: if (R[3]) oah_kc_attrs_free_out(R[3]); if (R[4]) free((void *)(uintptr_t)R[4]); break;
    case KC_SEARCHCREATE: {   /* (kcs, itemClass, attrList, *search) */
        int bad; CFTypeRef ks = oah_kc_kcarg(R[3], &bad); SecKeychainAttributeList *l = oah_kc_attrs_in(R[5]); SecKeychainSearchRef s = NULL;
        if (bad) { e = errSecParam; free(l); break; }
        e = SecKeychainSearchCreateFromAttributes(ks, R[4], l, &s);
        oah_kc_put(R[6], e ? 0 : oah_kc_wrap_own(s));
        if (s) CFRelease(s);
        if (ks) CFRelease(ks);
        free(l);
        break; }
    case KC_SEARCHNEXT: { SecKeychainItemRef it = NULL;
        e = SecKeychainSearchCopyNext((SecKeychainSearchRef)oah_kc_unwrap(R[3]), &it);
        oah_kc_put(R[4], e ? 0 : oah_kc_wrap_own(it)); if (it) CFRelease(it); break; }
    case KC_SEARCHLIST: {   /* (CFArrayRef *): a guest array of keychain proxies */
        static unsigned int fca, fcb;
        CFArrayRef l = NULL; unsigned int *v, a[4], g = 0; CFIndex n, k;
        if (!fca) { fca = oah_guest_sym("CoreFoundation", "_CFArrayCreate"); fcb = oah_guest_sym("CoreFoundation", "_kCFTypeArrayCallBacks"); }
        e = SecKeychainCopySearchList(&l);
        if (!e && l && fca && fcb) {
            n = CFArrayGetCount(l); v = (unsigned int *)calloc(n ? n : 1, 4);
            for (k = 0; k < n; k++) v[k] = OSSwapHostToBigInt32(oah_kc_wrap(CFArrayGetValueAtIndex(l, k)));
            a[0] = 0; a[1] = (unsigned int)(uintptr_t)v; a[2] = (unsigned int)n; a[3] = fcb;
            g = oah_callppc_n("kc_list", fca, 4, a);
            free(v);
        }
        if (l) CFRelease(l);
        oah_kc_put(R[3], g);
        if (!e && !g) e = errSecAllocate;
        break; }
    case KC_SETUI: e = SecKeychainSetUserInteractionAllowed((Boolean)(R[3] & 0xff)); break;
    case KC_GETUI: { Boolean b = 0; e = SecKeychainGetUserInteractionAllowed(&b); if (R[3] && oah_readable(R[3], 1)) *(unsigned char *)(uintptr_t)R[3] = b; break; }
    case KC_ITEMKEYCHAIN: { SecKeychainRef k = NULL; e = SecKeychainItemCopyKeychain((SecKeychainItemRef)oah_kc_unwrap(R[3]), &k);
        oah_kc_put(R[4], e ? 0 : oah_kc_wrap_own(k)); if (k) CFRelease(k); break; }
    case KC_ITEMCREATE: {   /* (itemClass, attrList, len, data, kc, initialAccess, *item) */
        SecKeychainAttributeList *l = oah_kc_attrs_in(R[4]); SecKeychainItemRef it = NULL;
        if (R[8] || !oah_kc_add(R[3], (SecKeychainRef)oah_kc_unwrap(R[7]), l, (const void *)(uintptr_t)R[6], R[5], R[9] ? &it : NULL, &e))   /* an initial access of its own: here */
        e = SecKeychainItemCreateFromContent(R[3], l, R[5], (const void *)(uintptr_t)R[6], (SecKeychainRef)oah_kc_unwrap(R[7]), NULL, R[9] ? &it : NULL);
        if (R[9]) oah_kc_put(R[9], e ? 0 : oah_kc_wrap_own(it));
        if (it) CFRelease(it);
        free(l);
        break; }
    }
    if (oah_kc_log()) fprintf(stderr, "[keychain] %s -> %d\n", g_kc_name[which], (int)e);
    g_gt_skip_ret = (unsigned int)e;
}
#define OAH_KC_FIX(n) static void oah_fix_kc_##n(unsigned int *R) { oah_kc_call(KC_##n, R); }
OAH_KC_FIX(COPYDEFAULT) OAH_KC_FIX(OPEN) OAH_KC_FIX(CREATE) OAH_KC_FIX(DELETE) OAH_KC_FIX(UNLOCK) OAH_KC_FIX(LOCK) OAH_KC_FIX(STATUS)
OAH_KC_FIX(PATH) OAH_KC_FIX(ADDGEN) OAH_KC_FIX(FINDGEN) OAH_KC_FIX(ADDINET) OAH_KC_FIX(FINDINET) OAH_KC_FIX(FREECONTENT) OAH_KC_FIX(MODIFY)
OAH_KC_FIX(ITEMDELETE) OAH_KC_FIX(COPYCONTENT) OAH_KC_FIX(COPYATTRDATA) OAH_KC_FIX(FREEATTRDATA) OAH_KC_FIX(SEARCHCREATE)
OAH_KC_FIX(SEARCHNEXT) OAH_KC_FIX(SETUI) OAH_KC_FIX(GETUI) OAH_KC_FIX(ITEMKEYCHAIN) OAH_KC_FIX(ITEMCREATE) OAH_KC_FIX(SEARCHLIST)
