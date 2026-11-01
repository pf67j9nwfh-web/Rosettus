/* ===== Clipboard sync: guest (translated pboard) <-> Mojave's native pasteboard =====
   The PPC apps share a guest clipboard served by the translated pboard agent. Each GUI app runs
   one guest helper thread (entered through the bridged LSSharedFileListItemGetTypeID with a magic
   argument, the way oah_cvl.h starts link threads) that watches whether this app is front:
     - becoming front: if Mojave's clipboard holds something no translated app put there,
       copy it into the guest clipboard (import);
     - resigning front: if the guest clipboard holds something Mojave's doesn't, copy it out (export).
   Only common flavors are carried (text, RTF, RTFD, HTML, images, PDF, URLs). A shared file
   /tmp/oah_clip.<uid> keeps the hash of the last synced content so apps don't bounce it back and
   forth; /tmp/oah_clip_busy.<uid> marks an export in progress so a newly front app waits for it.
   OAH_NO_CLIPSYNC=1 turns it off; OAH_CLIPTRACE=1 logs. */
#define OAH_CLIP_MAGIC 0x0a5c11b0u
static const char *g_clip_flavors[] = {
    "public.utf8-plain-text", "public.utf16-plain-text", "public.utf16-external-plain-text",
    "public.rtf", "com.apple.flat-rtfd", "public.html", "public.tiff", "public.png", "public.jpeg",
    "com.adobe.pdf", "public.file-url", "public.url", "public.url-name", 0 };
typedef struct { unsigned int item; const char *flavor; unsigned char *data; unsigned int len; int conv; } oah_clip_ent_t;
#define OAH_CLIP_MAXE 64
static volatile int g_clip_state;   /* 0 idle, 1 thread requested, 2 running */
static int oah_cliptr(void) { static int t = -1; if (t < 0) t = getenv("OAH_CLIPTRACE") ? atoi(getenv("OAH_CLIPTRACE")) : 0; if (t < 0) t = 1; return t; }
static const char *oah_clip_known(const char *f) {
    int i;
    for (i = 0; g_clip_flavors[i]; i++) if (!strcmp(g_clip_flavors[i], f)) return g_clip_flavors[i];
    return 0;
}
static unsigned long long oah_clip_hash(oah_clip_ent_t *e, int n) {
    unsigned long long h = 1469598103934665603ULL; int i; unsigned int k;
    for (i = 0; i < n; i++) {
        const char *s;
        for (s = e[i].flavor; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
        for (k = 0; k < e[i].len; k++) { h ^= e[i].data[k]; h *= 1099511628211ULL; }
        h ^= e[i].item; h *= 1099511628211ULL;
    }
    return h;
}
static void oah_clip_free(oah_clip_ent_t *e, int n) { int i; for (i = 0; i < n; i++) free(e[i].data); }
/* public.utf16-plain-text is UTF-16 in the host's byte order with no BOM: big-endian in the guest,
   little-endian on Mojave. Entries are kept in Mojave's order (so both sides hash alike) and swapped
   on the way into or out of the guest clipboard; pasted Mojave text came out as CJK characters in
   Word 2004. (utf16-external-plain-text carries a BOM and passes unchanged.) */
static int oah_clip_is_u16(const char *f) { return !strcmp(f, "public.utf16-plain-text"); }
static void oah_clip_swap16(unsigned char *d, unsigned int len) {
    unsigned int k; unsigned char t;
    for (k = 0; k + 1 < len; k += 2) { t = d[k]; d[k] = d[k + 1]; d[k + 1] = t; }
}
static void oah_clip_path(char *p, size_t sz, const char *what) { snprintf(p, sz, "/tmp/oah_clip%s.%d", what, (int)getuid()); }
static unsigned long long oah_clip_shared(void) {
    char p[64]; unsigned long long h = 0; FILE *f;
    oah_clip_path(p, sizeof p, "");
    if ((f = fopen(p, "r"))) { if (fscanf(f, "%llx", &h) != 1) h = 0; fclose(f); }
    return h;
}
static void oah_clip_set_shared(unsigned long long h) {
    char p[64], t[80]; FILE *f;
    oah_clip_path(p, sizeof p, ""); snprintf(t, sizeof t, "%s.%d", p, (int)getpid());
    if ((f = fopen(t, "w"))) { fprintf(f, "%016llx\n", h); fclose(f); rename(t, p); }
}
static void oah_clip_busy(int on) {
    char p[64]; FILE *f;
    oah_clip_path(p, sizeof p, "_busy");
    if (!on) { unlink(p); return; }
    if ((f = fopen(p, "w"))) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}
static void oah_clip_wait_busy(void) {
    char p[64]; struct stat st; int i;
    oah_clip_path(p, sizeof p, "_busy");
    for (i = 0; i < 40 && stat(p, &st) == 0 && time(0) - st.st_mtime < 5; i++) usleep(100000);
}

/* ---- guest side (PPC Pasteboard Manager, called on our guest thread) ---- */
enum { GP_CREATE, GP_SYNC, GP_COUNT, GP_ITEMID, GP_FLAVORS, GP_FFLAGS, GP_DATA, GP_CLEAR, GP_PUT,
       GP_ACOUNT, GP_AGET, GP_DLEN, GP_DPTR, GP_DNEW, GP_SNEW, GP_SGET, GP_REL, GP_N };
static unsigned int g_gp[GP_N];
static int oah_clip_gload(void) {
    static const char *nm[GP_N] = { "_PasteboardCreate", "_PasteboardSynchronize", "_PasteboardGetItemCount",
        "_PasteboardGetItemIdentifier", "_PasteboardCopyItemFlavors", "_PasteboardGetItemFlavorFlags",
        "_PasteboardCopyItemFlavorData", "_PasteboardClear", "_PasteboardPutItemFlavor",
        "_CFArrayGetCount", "_CFArrayGetValueAtIndex", "_CFDataGetLength", "_CFDataGetBytePtr", "_CFDataCreate",
        "_CFStringCreateWithCString", "_CFStringGetCString", "_CFRelease" };
    int i;
    for (i = 0; i < GP_N; i++)
        if (!g_gp[i] && !(g_gp[i] = oah_guest_sym(i < GP_ACOUNT ? "HIServices" : "CoreFoundation", nm[i]))) {
            if (oah_cliptr()) fprintf(stderr, "[clip] guest %s not found\n", nm[i]);
            return 0;
        }
    return 1;
}
static unsigned int gp(int f, int n, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3, unsigned int a4) {
    unsigned int a[5]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4;
    return oah_callppc_n("oah_clip", g_gp[f], n, a);
}
static unsigned int g_clip_gref;
static volatile int g_clip_onmain;   /* a sync running on the app's main thread (oah_clip_service) */
static volatile int g_clip_skipped;  /* promised flavors were left out of the last read */
/* A Carbon app's promised flavors are made by its promise keeper on the app's main thread, in its
   event loop (PasteboardResolvePromises from oah_clip_service), not on the sync thread inside our
   read: Illustrator CS2's keeper renders the copy as PDF and crashed there (another thread, while
   the main thread drew). OAH_CLIP_KEEPER_SYNCTHREAD=1: the keeper runs inside the read, as before. */
static volatile int g_clip_resolve;
static int oah_clip_mainresolve(void) { static int v = -1; if (v < 0) v = getenv("OAH_CLIP_KEEPER_SYNCTHREAD") ? 0 : 1; return v; }
/* a Cocoa app: its bundle names an NSPrincipalClass (AppKit alone is no sign: Carbon apps load
   it for their Open and Save dialogs) */
static int oah_clip_cocoa_app(void) {
    static int cached = -1;
    char path[PATH_MAX], f[PATH_MAX + 32], *m;
    const char *exe = getenv("OAH_GUEST_EXE");
    int r = 0;
    CFURLRef u; CFDataRef d = 0; CFPropertyListRef p = 0;
    if (cached >= 0) return cached;
    cached = 0;
    if (!exe || !realpath(exe, path) || !(m = strstr(path, ".app/Contents/"))) return 0;
    m[14] = 0;
    snprintf(f, sizeof f, "%sInfo.plist", path);
    u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)f, strlen(f), false);
    if (u && CFURLCreateDataAndPropertiesFromResource(NULL, u, &d, NULL, NULL, NULL) && d)
        p = CFPropertyListCreateFromXMLData(NULL, d, kCFPropertyListImmutable, NULL);
    if (p && CFGetTypeID(p) == CFDictionaryGetTypeID() && CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("NSPrincipalClass"))) r = 1;
    if (p) CFRelease(p);
    if (d) CFRelease(d);
    if (u) CFRelease(u);
    if (oah_cliptr()) fprintf(stderr, "[clip] %s app\n", r ? "Cocoa" : "Carbon");
    return cached = r;
}
static int oah_clip_gread(oah_clip_ent_t *e) {
    unsigned int be, cnt, i, n = 0, own;
    /* A flavor the app promised (0x200 without 0x100, system translation) is made now by the
       copying app's promise keeper, inside our call (the app exporting is the one that copied).
       Carbon apps promise everything (Word, Excel) and their keepers just write the data. A Cocoa
       app promises only through a lazy NSPasteboard provider, and Hex Fiend's runs a modal
       "Copying n bytes to the clipboard" session that never ended inside the sync; its window
       also kept Cmd-H from hiding the app. So a Cocoa app's promised data is left out
       (OAH_CLIP_PROMISED=1 reads it anyway). */
    static int cocoa = -1;
    if (cocoa < 0) cocoa = oah_clip_cocoa_app();
    gp(GP_SYNC, 1, g_clip_gref, 0, 0, 0, 0);
    /* on the main thread (inside a bridged call) a Carbon keeper returned nothing either: there
       the promised flavors wait for the export after the switch, made from the sync thread */
    own = ((cocoa || g_clip_onmain) && !getenv("OAH_CLIP_PROMISED")) ? 1 : 0;
    if (gp(GP_COUNT, 2, g_clip_gref, (unsigned int)&be, 0, 0, 0)) return 0;
    cnt = OSSwapBigToHostInt32(be);
    if (oah_cliptr()) fprintf(stderr, "[clip]   guest clipboard: %u item(s)\n", cnt);
    if (cnt > 16) cnt = 16;
    for (i = 1; i <= cnt; i++) {
        unsigned int id, arr, fc, k; int pend;
        if (gp(GP_ITEMID, 3, g_clip_gref, i, (unsigned int)&be, 0, 0)) continue;
        id = OSSwapBigToHostInt32(be);
        if (gp(GP_FLAVORS, 3, g_clip_gref, id, (unsigned int)&be, 0, 0)) continue;
        arr = OSSwapBigToHostInt32(be);
        if (!arr) continue;
        fc = gp(GP_ACOUNT, 1, arr, 0, 0, 0, 0);
        /* Flavors the Translation Manager makes (0x300: promised + system translation; Word's JPEG,
           TIFF, PNG and external text come from its promised PICT and text) are made from the app's
           own promised flavors: reading one runs the app's promise keeper here, on the sync thread,
           beside its main thread. Word 2004 crashed after 10-20 copies that way (a corrupted heap,
           seen in a CFRunLoop timer). While the item still has promises left for the main thread,
           its translations wait too; the read after the main thread resolved them takes them. */
        pend = 0;
        if (!own && !cocoa && oah_clip_mainresolve() && !getenv("OAH_CLIP_PROMISED") && !getenv("OAH_CLIP_XLATE_EARLY"))
            for (k = 0; k < fc && k < 64; k++) {
                unsigned int f2 = gp(GP_AGET, 2, arr, k, 0, 0, 0);
                if (f2 && !gp(GP_FFLAGS, 4, g_clip_gref, id, f2, (unsigned int)&be, 0) &&
                    (OSSwapBigToHostInt32(be) & 0x300) == 0x200) { pend = 1; break; }
            }
        for (k = 0; k < fc && k < 64 && n < OAH_CLIP_MAXE; k++) {
            unsigned int fl = gp(GP_AGET, 2, arr, k, 0, 0, 0), flags = 0, d, len;
            char buf[256]; const char *known; int trad;
            if (!fl || !gp(GP_SGET, 4, fl, (unsigned int)buf, sizeof buf, 0x08000100u, 0)) continue;
            if (!gp(GP_FFLAGS, 4, g_clip_gref, id, fl, (unsigned int)&be, 0)) flags = OSSwapBigToHostInt32(be);
            if (oah_cliptr()) fprintf(stderr, "[clip]   guest item %u flavor %s flags %x\n", i, buf, flags);
            /* Classic text ('TEXT', MacRoman with CR line ends): Snow Leopard's pasteboard server
               offered it to native readers as Unicode text; here we convert it (AppleWorks' copies
               reached Mojave empty) */
            trad = !strcmp(buf, "com.apple.traditional-mac-plain-text");
            if (!(known = oah_clip_known(buf)) && !trad) continue;
            /* sender-only and request-only flavors stay; system-translated ones are carried: a Classic
               app's scrap ('TEXT', 'styl') reaches the clipboard only as translations to public types */
            if (flags & (1 | 8)) continue;
            if (own && (flags & 0x300) == 0x200) { g_clip_skipped = 1; continue; }
            if (!own && !cocoa && (flags & 0x300) == 0x200 && oah_clip_mainresolve() && !getenv("OAH_CLIP_PROMISED")) {
                g_clip_skipped = 1; g_clip_resolve = 1;
                if (oah_cliptr()) fprintf(stderr, "[clip]   guest item %u %s: promised, left for the main thread\n", i, buf);
                continue;
            }
            if (pend && (flags & 0x300) == 0x300) {
                g_clip_skipped = 1; g_clip_resolve = 1;
                if (oah_cliptr()) fprintf(stderr, "[clip]   guest item %u %s: a translation of promised data, after the main thread\n", i, buf);
                continue;
            }
            {   unsigned int derr = gp(GP_DATA, 4, g_clip_gref, id, fl, (unsigned int)&be, 0);
                if (derr) { if (oah_cliptr()) fprintf(stderr, "[clip]   guest item %u %s: no data (%d)\n", i, buf, (int)derr); continue; }
            }
            d = OSSwapBigToHostInt32(be);
            if (!d) continue;
            len = gp(GP_DLEN, 1, d, 0, 0, 0, 0);
            if (len < 64u << 20) {
                const unsigned char *src = (const unsigned char *)gp(GP_DPTR, 1, d, 0, 0, 0, 0);
                e[n].data = (unsigned char *)malloc(len + 1);
                if (e[n].data && (src || !len)) {
                    if (len) memcpy(e[n].data, src, len);
                    e[n].len = len; e[n].item = i; e[n].flavor = known; e[n].conv = 0;
                    if (trad) {
                        CFStringRef s = CFStringCreateWithBytes(0, e[n].data, len, kCFStringEncodingMacRoman, false);
                        CFIndex ul = 0, L = s ? CFStringGetLength(s) : 0;
                        unsigned char *u;
                        free(e[n].data); e[n].data = 0;
                        if (s && (u = (unsigned char *)malloc(L * 4 + 1))) {
                            unsigned int q;
                            CFStringGetBytes(s, CFRangeMake(0, L), kCFStringEncodingUTF8, '?', false, u, L * 4, &ul);
                            for (q = 0; q < (unsigned int)ul; q++) if (u[q] == '\r') u[q] = '\n';
                            e[n].data = u; e[n].len = (unsigned int)ul;
                            e[n].flavor = g_clip_flavors[0]; e[n].conv = 1;
                        }
                        if (s) CFRelease(s);
                    }
                    if (e[n].data && !trad && oah_clip_is_u16(e[n].flavor)) oah_clip_swap16(e[n].data, e[n].len);
                    if (e[n].data) n++;
                } else free(e[n].data);
            }
            gp(GP_REL, 1, d, 0, 0, 0, 0);
        }
        gp(GP_REL, 1, arr, 0, 0, 0, 0);
    }
    {   /* a converted Classic text stays only where the item has no Unicode text of its own */
        unsigned int a, b, m = 0;
        for (a = 0; a < n; a++) {
            int drop = 0;
            if (e[a].conv) for (b = 0; b < n; b++)
                if (b != a && !e[b].conv && e[b].item == e[a].item && strstr(e[b].flavor, "plain-text")) drop = 1;
            if (drop) { free(e[a].data); continue; }
            e[m++] = e[a];
        }
        n = m;
    }
    return (int)n;
}
static void oah_clip_gwrite(oah_clip_ent_t *e, int n) {
    int i;
    gp(GP_CLEAR, 1, g_clip_gref, 0, 0, 0, 0);
    gp(GP_SYNC, 1, g_clip_gref, 0, 0, 0, 0);
    for (i = 0; i < n; i++) {
        unsigned char *b = e[i].data, *sw = 0;
        unsigned int fl, d;
        if (oah_clip_is_u16(e[i].flavor) && e[i].len && (sw = (unsigned char *)malloc(e[i].len))) {
            memcpy(sw, e[i].data, e[i].len); oah_clip_swap16(sw, e[i].len); b = sw;
        }
        fl = gp(GP_SNEW, 3, 0, (unsigned int)e[i].flavor, 0x08000100u, 0, 0);
        d = gp(GP_DNEW, 3, 0, (unsigned int)b, e[i].len, 0, 0);
        free(sw);
        unsigned int err = (fl && d) ? gp(GP_PUT, 5, g_clip_gref, e[i].item, fl, d, 0) : 1;
        if (oah_cliptr()) fprintf(stderr, "[clip]   guest put item %u %s (%u bytes) -> %d\n", e[i].item, e[i].flavor, e[i].len, (int)err);
        if (fl) gp(GP_REL, 1, fl, 0, 0, 0, 0);
        if (d) gp(GP_REL, 1, d, 0, 0, 0, 0);
    }
}

/* ---- native side (Mojave's Pasteboard Manager, 32-bit HIServices) ---- */
typedef int (*np_create_t)(CFStringRef, void **);
static struct {
    int (*create)(CFStringRef, void **); unsigned int (*sync)(void *); int (*count)(void *, unsigned long *);
    int (*itemid)(void *, unsigned long, void **); int (*flavors)(void *, void *, CFArrayRef *);
    int (*fflags)(void *, void *, CFStringRef, unsigned int *); int (*data)(void *, void *, CFStringRef, CFDataRef *);
    int (*clear)(void *); int (*put)(void *, void *, CFStringRef, CFDataRef, unsigned int);
} g_np;
static void *g_clip_nref;
static int oah_clip_nload(void) {
    if (g_np.create) return 1;
    dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY);
    *(void **)&g_np.count = dlsym(RTLD_DEFAULT, "PasteboardGetItemCount");
    *(void **)&g_np.sync = dlsym(RTLD_DEFAULT, "PasteboardSynchronize");
    *(void **)&g_np.itemid = dlsym(RTLD_DEFAULT, "PasteboardGetItemIdentifier");
    *(void **)&g_np.flavors = dlsym(RTLD_DEFAULT, "PasteboardCopyItemFlavors");
    *(void **)&g_np.fflags = dlsym(RTLD_DEFAULT, "PasteboardGetItemFlavorFlags");
    *(void **)&g_np.data = dlsym(RTLD_DEFAULT, "PasteboardCopyItemFlavorData");
    *(void **)&g_np.clear = dlsym(RTLD_DEFAULT, "PasteboardClear");
    *(void **)&g_np.put = dlsym(RTLD_DEFAULT, "PasteboardPutItemFlavor");
    if (!g_np.count || !g_np.sync || !g_np.itemid || !g_np.flavors || !g_np.data || !g_np.clear || !g_np.put) return 0;
    *(void **)&g_np.create = dlsym(RTLD_DEFAULT, "PasteboardCreate");
    return g_np.create != 0;
}
static int oah_clip_nread(oah_clip_ent_t *e) {
    unsigned long cnt = 0, i; int n = 0;
    g_np.sync(g_clip_nref);
    if (g_np.count(g_clip_nref, &cnt)) return 0;
    if (cnt > 16) cnt = 16;
    for (i = 1; i <= cnt; i++) {
        void *id; CFArrayRef arr; CFIndex k, fc;
        if (g_np.itemid(g_clip_nref, i, &id) || g_np.flavors(g_clip_nref, id, &arr) || !arr) continue;
        fc = CFArrayGetCount(arr);
        for (k = 0; k < fc && n < OAH_CLIP_MAXE; k++) {
            CFStringRef fl = (CFStringRef)CFArrayGetValueAtIndex(arr, k);
            char buf[256]; const char *known; unsigned int flags = 0; CFDataRef d = 0;
            if (!CFStringGetCString(fl, buf, sizeof buf, kCFStringEncodingUTF8) || !(known = oah_clip_known(buf))) continue;
            if (g_np.fflags && !g_np.fflags(g_clip_nref, id, fl, &flags) && (flags & (1 | 8 | 0x100))) continue;
            if (g_np.data(g_clip_nref, id, fl, &d) || !d) continue;
            if (CFDataGetLength(d) < (64 << 20) && (e[n].data = (unsigned char *)malloc(CFDataGetLength(d) + 1))) {
                e[n].len = (unsigned int)CFDataGetLength(d);
                memcpy(e[n].data, CFDataGetBytePtr(d), e[n].len);
                e[n].item = (unsigned int)i; e[n].flavor = known; n++;
            }
            CFRelease(d);
        }
        CFRelease(arr);
    }
    return n;
}
static void oah_clip_nwrite(oah_clip_ent_t *e, int n) {
    int i;
    g_np.clear(g_clip_nref);
    g_np.sync(g_clip_nref);
    for (i = 0; i < n; i++) {
        CFStringRef fl = CFStringCreateWithCString(0, e[i].flavor, kCFStringEncodingUTF8);
        CFDataRef d = CFDataCreate(0, e[i].data, e[i].len);
        int err = g_np.put(g_clip_nref, (void *)(unsigned long)e[i].item, fl, d, 0);
        if (oah_cliptr()) fprintf(stderr, "[clip]   native put item %u %s (%u bytes) -> %d\n", e[i].item, e[i].flavor, e[i].len, err);
        CFRelease(fl); CFRelease(d);
    }
}

static void oah_clip_import(void) {
    oah_clip_ent_t ne[OAH_CLIP_MAXE], ge[OAH_CLIP_MAXE];
    int nn, gn; unsigned long long nh, gh;
    oah_clip_wait_busy();
    nn = oah_clip_nread(ne);
    if (!nn) { if (oah_cliptr()) fprintf(stderr, "[clip] import: native clipboard has nothing we carry\n"); return; }
    nh = oah_clip_hash(ne, nn);
    if (nh == oah_clip_shared()) { if (oah_cliptr()) fprintf(stderr, "[clip] import: native content came from us\n"); oah_clip_free(ne, nn); return; }
    gn = oah_clip_gread(ge); gh = oah_clip_hash(ge, gn); oah_clip_free(ge, gn);
    if (gh != nh) {
        if (oah_cliptr()) fprintf(stderr, "[clip] import: %d flavors from Mojave\n", nn);
        oah_clip_gwrite(ne, nn);
    }
    oah_clip_set_shared(nh);
    oah_clip_free(ne, nn);
}
static void oah_clip_export(void) {
    oah_clip_ent_t ge[OAH_CLIP_MAXE];
    int gn; unsigned long long gh;
    oah_clip_busy(1);
    gn = oah_clip_gread(ge);
    gh = oah_clip_hash(ge, gn);
    if (gn && gh != oah_clip_shared()) {
        if (oah_cliptr()) fprintf(stderr, "[clip] export: %d flavors to Mojave\n", gn);
        oah_clip_nwrite(ge, gn);
        oah_clip_set_shared(gh);
    } else if (oah_cliptr()) fprintf(stderr, "[clip] export: nothing new (%d flavors)\n", gn);
    oah_clip_free(ge, gn);
    oah_clip_busy(0);
}
/* The guest side of a sync runs on the app's main thread (oah_clip_service, from oah_mb_tick): a
   copying app can answer a read with its own code (a promised flavor is made when asked for), and
   Cocoa code run on our thread had no autorelease pool or run loop. Hex Fiend's copy put up a
   progress window that was never drawn or closed, and it kept Cmd-H from hiding the app.
   Requests: 1 export, 2 import, 4 look for a change on either side. OAH_CLIP_OFFMAIN=1 runs them
   on the sync thread as before. */
static volatile int g_clip_req, g_clip_done;
static pthread_mutex_t g_clip_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned int g_clip_mx_owner;   /* Mach port of the thread holding g_clip_mx (hang dumps) */
static void oah_clip_run(int r) {
    if (r & (4 | 8)) {   /* 8: in the background, only a guest change goes out (no import) */
        if (gp(GP_SYNC, 1, g_clip_gref, 0, 0, 0, 0) & 1) r |= 1;
        else if (g_clip_dirty > 0 && !(r & 16)) { g_clip_dirty--; r |= 1; }   /* a lazy owner's late answer */
        else if ((r & 4) && (g_np.sync(g_clip_nref) & 1)) r |= 2;
        /* a Carbon app's promised data left out on the main thread goes out now, from here */
        if ((r & 8) && g_clip_skipped && !g_clip_onmain && !oah_clip_cocoa_app()) r |= 1;
    }
    if ((r & 1) && !g_clip_onmain) g_clip_skipped = 0;
    if (r & 1) oah_clip_export();
    if ((r & 2) && !(r & 16)) oah_clip_import();
    else if (r & 2) g_clip_req |= 2;   /* the import is left for the main thread */
}
/* Called in the guest's event fetch on the main thread (next to the native Apple Events), where a
   real pasteboard request would reach the app's promise keeper too. */
static void oah_clip_resolve_main(void) {
    if (!pthread_main_np()) return;
    {   static int once; if (!once && oah_cliptr() > 1) { once = 1; fprintf(stderr, "[clip] event fetch reached (promises)\n"); } }
    if (g_clip_resolve && g_clip_state == 2 && g_clip_gref) {
        static unsigned int frp;
        unsigned int a[1], e;
        g_clip_resolve = 0;
        if (!frp) frp = oah_guest_sym("HIServices", "_PasteboardResolvePromises");
        if (frp && !pthread_mutex_trylock(&g_clip_mx)) {
            g_clip_mx_owner = pthread_mach_thread_np(pthread_self());
            /* on the app's own PasteboardRefs (the promises are kept there, not on ours) */
            int k; e = 0;
            for (k = 0; k < g_nkeeper; k++) { a[0] = g_keeper_refs[k]; e |= oah_callppc_n("PasteboardResolvePromises", frp, 1, a); }
            g_clip_mx_owner = 0; pthread_mutex_unlock(&g_clip_mx);
            g_clip_dirty = 2;                  /* the next looks export what the keeper made */
            if (oah_cliptr()) fprintf(stderr, "[clip] promises resolved on the main thread -> %d\n", (int)e);
        } else {
            g_clip_resolve = 1;
            if (oah_cliptr() > 1) fprintf(stderr, "[clip] promises: %s\n", frp ? "sync busy, later" : "no PasteboardResolvePromises");
        }
    } else if (g_clip_resolve && oah_cliptr() > 1) fprintf(stderr, "[clip] promises: state %d ref %08x\n", g_clip_state, g_clip_gref);
}
static void oah_clip_service(void) {
    int r;
    if (!g_clip_req || g_clip_state != 2) return;
    if (pthread_mutex_trylock(&g_clip_mx)) return;
    g_clip_mx_owner = pthread_mach_thread_np(pthread_self());   /* also keeps a modal loop inside a read from nesting */
    r = g_clip_req; g_clip_req = 0;
    g_clip_onmain = 1;
    if (r) oah_clip_run(r);
    g_clip_onmain = 0;
    g_clip_done++;
    g_clip_mx_owner = 0; pthread_mutex_unlock(&g_clip_mx);
}
/* An app in the background may sit in its event wait for a long time without reaching
   oah_mb_tick: a request the main thread has not taken within 0.75 s runs here (the export after
   a switch, and a Classic app's late scrap conversion, worked this way before). */
/* For a Cocoa app the look while front (4) and the import (2) are only run on the main thread: a
   look it did not take is dropped (the next comes half a second later) and an import is asked for
   again; only the export after a switch (8) falls back to this thread. */
static int oah_clip_req(int r) {
    static int offmain = -1;
    int d = g_clip_done, i, here;
    /* Carbon apps keep the sync on this thread, as before: their promise keepers (Word, Excel:
       every flavor is promised) answer a read made here and returned nothing to one made inside
       a bridged call on the main thread, and an import run there during start-up (inside the
       window creation of Office's notification helper) crashed Microsoft Component Plugin */
    if (offmain < 0) offmain = (getenv("OAH_CLIP_OFFMAIN") || !oah_clip_cocoa_app()) ? 1 : 0;
    here = offmain;
    if (here) {
        pthread_mutex_lock(&g_clip_mx); g_clip_mx_owner = pthread_mach_thread_np(pthread_self());
        r |= g_clip_req; g_clip_req = 0;
        if (!offmain) r &= ~4;
        if (r) oah_clip_run(r);
        g_clip_mx_owner = 0; pthread_mutex_unlock(&g_clip_mx);
        /* promised flavors wait for the main thread's event fetch: an idle app (TextWrangler) sits
           in its event wait until something arrives, so wake it */
        if (g_clip_resolve) oah_ev_wake();
        return 1;
    }
    g_clip_req |= r;
    for (i = 0; i < 15 && g_clip_done == d; i++) usleep(50000);
    if (g_clip_done != d) return 1;
    pthread_mutex_lock(&g_clip_mx); g_clip_mx_owner = pthread_mach_thread_np(pthread_self());
    r = g_clip_req; g_clip_req = 0;
    /* only exports run here: the export after a switch (8), and the look for a guest change while
       front (4) - an idle Cocoa app (Hex Fiend) may not reach the main-thread service for a long
       time, and a copy stayed in the app until it was switched away from. A Cocoa app's promised
       flavors are not read here (oah_clip_gread), so no app code runs on this thread. An import
       (a native change) waits for the main thread (16: no import). */
    r &= 8 | 4;
    if (r) oah_clip_run(r | 16);
    g_clip_mx_owner = 0; pthread_mutex_unlock(&g_clip_mx);
    return r != 0;
}
/* OAH_MEMSTATS=n: every n seconds the guest's malloc totals (malloc_zone_statistics over all zones,
   called on this guest thread) go to the log, beside the process's resident size, so that memory
   growth in a long session can be put on the app's own heap or on the translator / host side. */
static void oah_memstats_tick(void) {
    static int every = -1; static time_t last; static unsigned int fms;
    unsigned int st[4], a[2];
    struct task_basic_info ti; mach_msg_type_number_t c = TASK_BASIC_INFO_COUNT;
    if (every < 0) { const char *e = getenv("OAH_MEMSTATS"); every = e ? atoi(e) : 0; if (every < 0) every = 0; }
    if (!every || time(0) - last < every) return;
    last = time(0);
    if (!fms && !(fms = oah_guest_sym("libSystem", "_malloc_zone_statistics"))) { every = 0; return; }
    memset(st, 0, sizeof st);
    a[0] = 0; a[1] = (unsigned int)st;
    oah_callppc_n("malloc_zone_statistics", fms, 2, a);
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&ti, &c)) ti.resident_size = 0;
    fprintf(stderr, "[mem] t=%ld guest malloc: %u blocks, in use %u KB, allocated %u KB, peak %u KB; resident %lu KB\n",
            (long)last, OSSwapBigToHostInt32(st[0]), OSSwapBigToHostInt32(st[1]) >> 10, OSSwapBigToHostInt32(st[3]) >> 10,
            OSSwapBigToHostInt32(st[2]) >> 10, (unsigned long)(ti.resident_size >> 10));
}
static void oah_clip_thread(void) {
    int (*getfront)(unsigned int *) = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetFrontProcess");
    int (*getcur)(unsigned int *) = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetCurrentProcess");
    unsigned int self[2] = { 0, 0 }, f[2], be = 0, name;
    int was = 0, late = 0, tick = 0, want_import = 0, cocoa = oah_clip_cocoa_app();
    g_clip_state = 2;
    if (!getfront || !getcur || getcur(self) || !oah_clip_gload() || !oah_clip_nload()) goto out;
    if (g_np.create(CFSTR("com.apple.pasteboard.clipboard"), &g_clip_nref) || !g_clip_nref) goto out;
    name = gp(GP_SNEW, 3, 0, (unsigned int)"com.apple.pasteboard.clipboard", 0x08000100u, 0, 0);
    if (!name || gp(GP_CREATE, 2, name, (unsigned int)&be, 0, 0, 0) || !(g_clip_gref = OSSwapBigToHostInt32(be))) goto out;
    if (oah_cliptr()) fprintf(stderr, "[clip] sync thread running (guest ref %08x)\n", g_clip_gref);
    for (;;) {
        int now;
        f[0] = f[1] = 0;
        if (getfront(f)) { usleep(500000); continue; }
        now = f[0] == self[0] && f[1] == self[1];
        if (now && !was) { late = 0; usleep(300000); oah_clip_wait_busy(); want_import = !oah_clip_req(2); tick = 0; }
        else if (now && want_import) want_import = !oah_clip_req(2);   /* the main thread was busy */
        else if (now && !(++tick % 2)) {
            /* while front, follow copies as they happen (Snow Leopard's Rosetta shared one pasteboard
               server, so a copy was visible to native apps at once): kPasteboardModified (bit 0) from
               a guest sync means the app changed the clipboard; a native change means another process
               (a clipboard manager, pbcopy) did */
            oah_clip_req(4);
        }
        /* every read syncs, so "modified" means changed since we last looked: an unchanged clipboard
           (already sent out while front) is not read again, which kept a big copy (Keynote's PDF
           and TIFF flavors) from tying up the app for seconds after each switch */
        /* Carbon apps are read in full each time, as before: Word owns the clipboard with promised
           flavors and a new copy changes them without a new PasteboardClear, so "modified" never
           showed it */
        else if (!now && was) { oah_clip_req(cocoa ? 8 : 1); late = 12; want_import = 0; }
        else if (!now && late > 0) {
            /* Classic apps convert their private scrap to the public clipboard only when they get the
               suspend event, which arrives after the switch: look again for 3 s (AppleWorks copied
               nothing to Mojave before) */
            if (!(--late % 2)) oah_clip_req(cocoa ? 8 : 1);
        }
        was = now;
        oah_memstats_tick();
        usleep(250000);
    }
out:
    if (oah_cliptr()) fprintf(stderr, "[clip] sync thread could not start\n");
    g_clip_state = 3;
}
/* Called from oah_mb_tick (main thread, inside a bridge) once the app has a menu bar. */
static void oah_clip_start(void) {
    static unsigned int fpc, fgt;
    unsigned int a[4], tid = 0;
    if (g_clip_state) return;
    g_clip_state = 3;
    if (getenv("OAH_NO_CLIPSYNC")) return;
    if (!fpc) fpc = oah_guest_sym("libSystem", "_pthread_create");
    if (!fgt) fgt = oah_guest_sym("LaunchServices", "_LSSharedFileListItemGetTypeID");
    if (!fpc || !fgt) { if (oah_cliptr()) fprintf(stderr, "[clip] no guest pthread_create / entry\n"); return; }
    g_clip_state = 1;
    a[0] = (unsigned int)&tid; a[1] = 0; a[2] = fgt; a[3] = OAH_CLIP_MAGIC;
    if (oah_callppc_n("clip_pthread_create", fpc, 4, a)) g_clip_state = 3;
}
