/* ---- Files dragged onto a PowerPC app's window ----
   On Mojave a drag (Finder, any native app) finds its destination through CoreDrag: the window
   server names the connection that owns the window under the pointer, and CoreDrag on that side
   calls the tracking and receive handlers installed there. The guest's own 10.6 CoreDrag never
   reaches Mojave's drag server, so nothing answered for our windows: a file dropped on a
   TextWrangler, Word or Photoshop window bounced back.
   Here a native thread registers the process's connection as a CoreDrag client, accepts drags
   that carry files (copy cursor), and on a drop hands the files to the app as an 'odoc' Apple
   Event, as if they had been dropped on its Dock icon. Other drags (text, pictures from other
   apps) are not accepted. OAH_NO_DND=1 turns this off; OAH_DNDLOG=1 logs. */
typedef struct oah_OpaquePasteboard *oah_PasteboardRef;   /* HIServices' Pasteboard API, by dlsym */
static struct {
    int (*sync)(oah_PasteboardRef);
    int (*count)(oah_PasteboardRef, unsigned long *);
    int (*ident)(oah_PasteboardRef, unsigned long, void **);
    int (*copy)(oah_PasteboardRef, void *, CFStringRef, CFDataRef *);
} g_pbf;
typedef int (*oah_cd_track_t)(unsigned int msg, unsigned int wid, void *refcon, void *drag);
typedef int (*oah_cd_recv_t)(unsigned int wid, void *refcon, void *drag);
static struct {
    int (*reg)(unsigned int cid, CFArrayRef modes);
    int (*track)(unsigned int cid, unsigned int wid, oah_cd_track_t, void *refcon, void **ref);
    int (*recv)(unsigned int cid, unsigned int wid, oah_cd_recv_t, void *refcon, void **ref);
    int (*pb)(void *drag, oah_PasteboardRef *out);
    int (*setact)(void *drag, unsigned int actions);
} g_cd;
static int g_dndlog = -1;
#define DNDLOG(...) do { if (g_dndlog < 0) g_dndlog = getenv("OAH_DNDLOG") ? 1 : 0; if (g_dndlog) fprintf(stderr, __VA_ARGS__); } while (0)

/* the file paths a drag carries, newline-separated (NULL if none) */
static char *oah_dnd_paths(void *drag) {
    oah_PasteboardRef pb = 0;
    unsigned long n = 0, i;
    char *out = 0;
    size_t len = 0;
    if (!g_cd.pb || !g_pbf.copy || g_cd.pb(drag, &pb) || !pb) return 0;
    g_pbf.sync(pb);
    if (g_pbf.count(pb, &n)) return 0;
    for (i = 1; i <= n; i++) {
        void *id;
        CFDataRef d = 0;
        CFURLRef u;
        char path[PATH_MAX];
        if (g_pbf.ident(pb, i, &id)) continue;
        if (g_pbf.copy(pb, id, CFSTR("public.file-url"), &d) || !d) continue;
        u = CFURLCreateWithBytes(0, CFDataGetBytePtr(d), CFDataGetLength(d), kCFStringEncodingUTF8, 0);
        CFRelease(d);
        if (!u) continue;
        if (CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path)) {
            size_t l = strlen(path);
            char *o = (char *)realloc(out, len + l + 2);
            if (o) { out = o; memcpy(out + len, path, l); len += l; out[len++] = '\n'; out[len] = 0; }
        }
        CFRelease(u);
    }
    if (out && len) out[len - 1] = 0;   /* no trailing newline */
    return out;
}
static int oah_dnd_track(unsigned int msg, unsigned int wid, void *refcon, void *drag) {
    static void *last;
    static int files;
    (void)refcon;
    if (msg <= 1 || drag != last) {                 /* entering: look at the pasteboard once per drag */
        char *p = oah_dnd_paths(drag);
        DNDLOG("[dnd] tracking message %u window %u: %s\n", msg, wid, p ? p : "(no files)");
        files = p != 0;
        last = drag;
        free(p);
    }
    if (files && g_cd.setact) g_cd.setact(drag, 1);   /* copy */
    return 0;
}
static int oah_dnd_receive(unsigned int wid, void *refcon, void *drag) {
    char *p = oah_dnd_paths(drag), *buf;
    pthread_t t;
    size_t l;
    (void)refcon;
    DNDLOG("[dnd] drop on window %u: %s\n", wid, p ? p : "(no files)");
    if (!p) return -1;                              /* not ours to take: the drag slides back */
    l = strlen(p);
    buf = (char *)calloc(1, l + 4);                 /* "\001" paths "\0" (no print documents) "\0" */
    if (!buf) { free(p); return -1; }
    buf[0] = 1; memcpy(buf + 1, p, l);
    free(p);
    pthread_create(&t, 0, oah_aen_docs_thread, buf);   /* odoc, one per file, as launch documents are */
    pthread_detach(t);
    return 0;
}
/* install the handlers on windows that do not have them yet (new windows; a reused record) */
static void oah_dnd_scan(CFRunLoopTimerRef t, void *info) {
    int i;
    (void)t; (void)info;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        unsigned int wid = w->wid;
        void *r1 = 0, *r2 = 0;
        int e1, e2;
        if (!wid || w->dnd_wid == wid || wid == g_menubar_wid) continue;
        w->dnd_wid = wid;
        e1 = g_cd.track(g_ncid, wid, oah_dnd_track, 0, &r1);
        e2 = g_cd.recv(g_ncid, wid, oah_dnd_receive, 0, &r2);
        DNDLOG("[dnd] handlers on window %u: tracking %d, receive %d\n", wid, e1, e2);
    }
}
static void *oah_dnd_thread(void *arg) {
    void *h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
    void *r1 = 0, *r2 = 0;
    int e0 = -1, e1 = -1, e2 = -1;
    CFArrayRef modes;
    (void)arg;
    if (!h) return 0;
    *(void **)&g_cd.reg = dlsym(h, "CoreDragRegisterClientInModes");
    *(void **)&g_cd.track = dlsym(h, "CoreDragInstallTrackingHandlerOnConnection");
    *(void **)&g_cd.recv = dlsym(h, "CoreDragInstallReceiveHandlerOnConnection");
    *(void **)&g_cd.pb = dlsym(h, "CoreDragGetPasteboardRef");
    *(void **)&g_cd.setact = dlsym(h, "CoreDragSetDropActions");
    *(void **)&g_pbf.sync = dlsym(h, "PasteboardSynchronize");
    *(void **)&g_pbf.count = dlsym(h, "PasteboardGetItemCount");
    *(void **)&g_pbf.ident = dlsym(h, "PasteboardGetItemIdentifier");
    *(void **)&g_pbf.copy = dlsym(h, "PasteboardCopyItemFlavorData");
    if (!g_cd.reg || !g_cd.track || !g_cd.recv || !g_cd.pb || !g_pbf.sync || !g_pbf.count || !g_pbf.ident || !g_pbf.copy) { DNDLOG("[dnd] CoreDrag not found\n"); return 0; }
    while (!g_ncid) usleep(100000);                  /* the connection that owns our windows */
    modes = CFArrayCreate(0, (const void **)&kCFRunLoopDefaultMode, 1, &kCFTypeArrayCallBacks);
    e0 = g_cd.reg(g_ncid, modes);
    DNDLOG("[dnd] CoreDrag client on connection %u: register %d\n", g_ncid, e0);
    (void)r1; (void)r2; (void)e1; (void)e2;
    {   /* handlers go on each window (as AppKit installs them), checked a few times a second */
        CFRunLoopTimerContext tc = { 0, 0, 0, 0, 0 };
        CFRunLoopTimerRef tm = CFRunLoopTimerCreate(0, CFAbsoluteTimeGetCurrent(), 0.25, 0, 0, oah_dnd_scan, &tc);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), tm, kCFRunLoopDefaultMode);
    }
    CFRunLoopRun();                                  /* CoreDrag's messages arrive on this run loop */
    return 0;
}
static void oah_dnd_start(void) {
    static int done;
    pthread_t t;
    if (!__sync_bool_compare_and_swap(&done, 0, 1) || getenv("OAH_NO_DND")) return;
    pthread_create(&t, 0, oah_dnd_thread, 0);
    pthread_detach(t);
}
