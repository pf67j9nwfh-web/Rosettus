/* oah_ls.h - process/application information from Mojave's LaunchServices.

   On 10.6 every Carbon app checks in with coreservicesd (HIServices _RegisterApplication ->
   _LSApplicationCheckIn) and later reads its own process attributes back with
   _LSCopyApplicationInformation(Item). Under the translator the guest check-in returns NULL
   ("FAILED TO GET ASN FROM CORESERVICES", abort stubbed) and coreservicesd has no record of
   the process, so AE's aeGetProcessAttributes got NULL and trapped in CFEqual(NULL, ...)
   when AppleWorks sent itself an Apple Event (AEInteractWithUser).

   The process is a real, registered application on Mojave (oah_make_foreground) and the
   guest PSNs are Mojave's (GetCurrentProcess/GetNextProcess are bridged natively), so the
   answers come from Mojave's own LaunchServices and are rebuilt as guest CF objects:
     _LSApplicationCheckIn              -> a guest ASN for our native PSN
     _LSCopyApplicationInformationItem  -> native item (missing Is* booleans -> kCFBooleanFalse)
     _LSCopyApplicationInformation      -> native dictionary
   OAH_NO_LSBRIDGE=1 makes these fall back to the trace stubs. OAH_LSTRACE=1 logs. */
static void *g_ncs;
static unsigned int g_ls_self_info, g_ls_self_lo;   /* guest check-in dictionary of this process */
static void *oah_nls(const char *n) {
    if (!g_ncs) g_ncs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_LAZY | RTLD_GLOBAL);
    return g_ncs ? dlsym(g_ncs, n) : 0;
}
static int oah_ls_is_self(const unsigned int *psn) {
    static unsigned int me[2]; static int got;
    if (!got) { int (*gcp)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "GetCurrentProcess"); got = 1; if (gcp) gcp(me); }
    return psn[0] == me[0] && psn[1] == me[1];
}
static int oah_lstr(void) {
    static int t = -1;
    if (t < 0) t = getenv("OAH_LSTRACE") ? 1 : 0;
    return t;
}
/* guest CFString -> native CFString (NULL if not convertible) */
static CFStringRef oah_guest_str_native(unsigned int gs) {
    static unsigned int fget;
    static char buf[512];
    unsigned int a[4];
    if (!gs) return NULL;
    if (!fget) fget = oah_guest_sym("CoreFoundation", "_CFStringGetCString");
    if (!fget) return NULL;
    buf[0] = 0;
    a[0] = gs; a[1] = (unsigned int)buf; a[2] = sizeof buf; a[3] = 0x08000100u;   /* UTF-8 */
    if (!(oah_callppc_n("oah_ls_str", fget, 4, a) & 0xff)) return NULL;
    return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}
/* guest ASN -> native ASN (via the PSN it holds) */
static CFTypeRef oah_guest_asn_native(unsigned int gasn, unsigned int *hi_out, unsigned int *lo_out) {
    static unsigned int fext;
    static unsigned int hl[2];
    static CFTypeRef (*mk)(CFAllocatorRef, pid_t);
    static OSStatus (*gpid)(const void *, pid_t *);
    unsigned int a[3], psn[2];
    pid_t pid = 0;
    if (!fext) fext = oah_guest_sym("LaunchServices", "__LSASNExtractHighAndLowParts");
    if (!mk) mk = (CFTypeRef (*)(CFAllocatorRef, pid_t))oah_nls("_LSASNCreateWithPid");
    if (!gpid) {
        void *as = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_GLOBAL);
        gpid = as ? (OSStatus (*)(const void *, pid_t *))dlsym(as, "GetProcessPID") : 0;
    }
    if (!gasn || !fext || !mk) return NULL;
    hl[0] = hl[1] = 0;
    a[0] = gasn; a[1] = (unsigned int)&hl[0]; a[2] = (unsigned int)&hl[1];
    oah_callppc_n("oah_ls_asn", fext, 3, a);
    psn[0] = OSSwapBigToHostInt32(hl[0]); psn[1] = OSSwapBigToHostInt32(hl[1]);
    if (hi_out) *hi_out = psn[0];
    if (lo_out) *lo_out = psn[1];
    if (!gpid || gpid(psn, &pid) || pid <= 0) return NULL;
    return mk(NULL, pid);
}
/* Guest copy of Mojave's information dictionary for a process, with the guest's own ASN
   object under "LSASN" (native LSASN values cannot be rebuilt as guest CF and are dropped). */
/* native LSASN -> guest LSASN (same PSN) */
static unsigned int oah_ls_asn_to_guest(CFTypeRef v) {
    static CFTypeID (*tid)(void);
    static void (*ext)(CFTypeRef, unsigned int *, unsigned int *);
    static unsigned int fmk;
    unsigned int hi = 0, lo = 0, a[3];
    if (!tid) tid = (CFTypeID (*)(void))oah_nls("_LSASNGetTypeID");
    if (!ext) ext = (void (*)(CFTypeRef, unsigned int *, unsigned int *))oah_nls("_LSASNExtractHighAndLowParts");
    if (!fmk) fmk = oah_guest_sym("LaunchServices", "__LSASNCreate");
    if (!v || !tid || !ext || !fmk || CFGetTypeID(v) != tid()) return 0;
    ext(v, &hi, &lo);
    a[0] = 0; a[1] = hi; a[2] = lo;
    return oah_callppc_n("oah_ls_mkasn", fmk, 3, a);
}
/* Mojave describes the translator process ("translate_mojave", its path and architecture);
   only the run-time state keys are taken from it. Everything about the application itself
   comes from the guest's own dictionary (the one it passed to _LSApplicationCheckIn), which
   the caller merges on top (oah_ls_merge_guest). */
static CFDictionaryRef oah_ls_state_only(CFDictionaryRef d) {
    static const char *keep[] = { "pid", "ApplicationType", "LSApplicationHasRegistered", "LSApplicationReadyToBeFrontableKey",
                                  "CanBecomeFrontmost", "Flavor", "ChangeCount", "LSUIElement", "LSBackgroundOnly",
                                  "IsHiddenAttr", "IsStopped", "LSApplicationWasTerminatedByTALKey", 0 };
    CFMutableDictionaryRef m;
    int i;
    if (!d) return NULL;
    m = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (i = 0; keep[i]; i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, keep[i], kCFStringEncodingUTF8);
        const void *v = CFDictionaryGetValue(d, k);
        if (v) CFDictionarySetValue(m, k, v);
        CFRelease(k);
    }
    return m;
}
/* Copy every entry of guest dictionary gsrc into guest dictionary gdst. */
static void oah_ls_merge_guest(unsigned int gdst, unsigned int gsrc) {
    static unsigned int fcnt, fkv;
    static unsigned int kv[2 * 128];
    unsigned int a[3], n, i;
    if (!gdst || !gsrc) return;
    if (!fcnt) fcnt = oah_guest_sym("CoreFoundation", "_CFDictionaryGetCount");
    if (!fkv) fkv = oah_guest_sym("CoreFoundation", "_CFDictionaryGetKeysAndValues");
    if (!fcnt || !fkv) return;
    a[0] = gsrc;
    n = oah_callppc_n("oah_ls_cnt", fcnt, 1, a);
    if (n > 128) n = 128;
    a[0] = gsrc; a[1] = (unsigned int)kv; a[2] = (unsigned int)(kv + 128);
    oah_callppc_n("oah_ls_kv", fkv, 3, a);
    for (i = 0; i < n; i++)
        gcall(G_DICT_SET, 3, gdst, OSSwapBigToHostInt32(kv[i]), OSSwapBigToHostInt32(kv[128 + i]), 0);
}
static unsigned int oah_ls_info_guest(CFTypeRef nasn, unsigned int hi, unsigned int lo, unsigned int gextra) {
    static CFDictionaryRef (*cp)(int, CFTypeRef, CFArrayRef);
    static unsigned int fmk;
    CFDictionaryRef d = NULL, st = NULL;
    unsigned int gd, gk, ga, a[3];
    if (!cp) cp = (CFDictionaryRef (*)(int, CFTypeRef, CFArrayRef))oah_nls("_LSCopyApplicationInformation");
    if (!fmk) fmk = oah_guest_sym("LaunchServices", "__LSASNCreate");
    if (cp && nasn) d = cp(-2, nasn, NULL);
    st = gextra ? oah_ls_state_only(d) : (d ? CFRetain(d) : NULL);
    gd = st ? oah_cf_to_guest(st) : 0;
    if (st) CFRelease(st);
    if (!gd && oah_guest_cf_load()) gd = gcall(G_DICT_NEW, 4, 0, 0, g_cf[G_DKEYCB], g_cf[G_DVALCB]);
    oah_ls_merge_guest(gd, gextra);
    if (!gd && oah_guest_cf_load()) gd = gcall(G_DICT_NEW, 4, 0, 0, g_cf[G_DKEYCB], g_cf[G_DVALCB]);
    if (gd && fmk) {
        a[0] = 0; a[1] = hi; a[2] = lo;
        ga = oah_callppc_n("oah_ls_mkasn", fmk, 3, a);
        gk = gcall(G_STR_NEW, 3, 0, (unsigned int)"LSASN", 0x08000100u, 0);
        if (ga && gk) gcall(G_DICT_SET, 3, gd, gk, ga, 0);
        if (gk) gcall(G_RELEASE, 1, gk, 0, 0, 0);
        if (ga) gcall(G_RELEASE, 1, ga, 0, 0, 0);
    }
    if (oah_lstr()) fprintf(stderr, "[ls] info dict for psn %u.%u: %ld native keys -> guest 0x%08x\n",
                            hi, lo, d ? (long)CFDictionaryGetCount(d) : -1L, gd);
    if (d) CFRelease(d);
    return gd;
}
/* ---- opening URLs and documents through Mojave's LaunchServices ----
   The guest LaunchServices database (translated coreservicesd) knows no Mojave applications:
   Numbers' "Video Tutorials" said "A default web browser hasn't been set", and
   LSOpenFromRefSpec was a no-op stub. Handlers, URLs and launches go to Mojave instead. */
typedef struct { CFURLRef appURL; CFArrayRef itemURLs; const void *passThruParams; unsigned int launchFlags; void *asyncRefCon; } oah_LSLaunchURLSpec;
static CFURLRef oah_guest_url_native(unsigned int gurl) {
    static unsigned int fabs, fgs, frel;
    unsigned int a[1], ab, gs;
    CFStringRef s;
    CFURLRef u;
    if (!gurl) return NULL;
    if (!fabs) { fabs = oah_guest_sym("CoreFoundation", "_CFURLCopyAbsoluteURL"); fgs = oah_guest_sym("CoreFoundation", "_CFURLGetString");
                 frel = oah_guest_sym("CoreFoundation", "_CFRelease"); }
    if (!fgs) return NULL;
    a[0] = gurl; ab = fabs ? oah_callppc_n("oah_ls_absurl", fabs, 1, a) : 0;
    a[0] = ab ? ab : gurl; gs = oah_callppc_n("oah_ls_urlstr", fgs, 1, a);
    s = oah_guest_str_native(gs);
    if (ab && frel) { a[0] = ab; oah_callppc_n("CFRelease", frel, 1, a); }
    if (!s) return NULL;
    u = CFURLCreateWithString(NULL, s, NULL);
    CFRelease(s);
    return u;
}
static CFURLRef oah_guest_fsref_native(unsigned int fsref) {
    static unsigned int fmp;
    unsigned char path[1024];
    unsigned int a[3];
    if (!fsref) return NULL;
    if (!fmp) fmp = oah_guest_sym("CarbonCore", "_FSRefMakePath");
    if (!fmp) return NULL;
    path[0] = 0;
    a[0] = fsref; a[1] = (unsigned int)path; a[2] = sizeof path;
    if (oah_callppc_n("FSRefMakePath", fmp, 3, a) != 0 || !path[0]) return NULL;
    return CFURLCreateFromFileSystemRepresentation(NULL, path, strlen((char *)path), false);
}
/* Is the application at this bundle (or executable) path PowerPC-only? Then Mojave cannot
   launch it, and it is started under this translator instead (SimCity 4's APlayer.app,
   its background-music player, used to be dropped by a no-op LSOpenFromRefSpec). */
static int oah_app_arch(const char *app, char *exe, size_t exesz);
static int oah_app_is_ppc_only(const char *app, char *exe, size_t exesz) {
    return oah_app_arch(app, exe, exesz) == 1;
}
/* bit 0: has PowerPC code, bit 1: has Intel code */
static int oah_app_arch(const char *app, char *exe, size_t exesz) {
    char plist[1100];
    unsigned char h[4096];
    int fd, n, ppc = 0, x86 = 0;
    struct stat sb;
    exe[0] = 0;
    if (stat(app, &sb)) return 0;
    if (S_ISDIR(sb.st_mode)) {
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)app, strlen(app), true);
        CFBundleRef b = u ? CFBundleCreate(NULL, u) : NULL;
        CFURLRef eu = b ? CFBundleCopyExecutableURL(b) : NULL;
        /* a CFM app patched by rosettus_patch.py: Info(-macos).plist names the native stub
           (rosetta-launch); RosettaOriginalExecutable keeps the PowerPC file (AppleWorks 6) */
        CFTypeRef orig = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("RosettaOriginalExecutable")) : NULL;
        if (orig && CFGetTypeID(orig) == CFStringGetTypeID()) {
            char on[256];
            if (CFStringGetCString((CFStringRef)orig, on, sizeof on, kCFStringEncodingUTF8) && !strchr(on, '/')) {
                snprintf(exe, exesz, "%s/Contents/MacOS/%s", app, on);
                if (eu) CFRelease(eu);
                eu = NULL;
            }
        }
        if (eu) CFURLGetFileSystemRepresentation(eu, true, (UInt8 *)exe, exesz);
        if (eu) CFRelease(eu);
        if (b) CFRelease(b);
        if (u) CFRelease(u);
        (void)plist;
    } else snprintf(exe, exesz, "%s", app);
    if (!exe[0] || (fd = open(exe, O_RDONLY)) < 0) return 0;
    n = (int)read(fd, h, sizeof h);
    close(fd);
    if (n < 8) return 0;
    if (!memcmp(h, "Joy!", 4)) return 1;                                     /* CFM/PEF */
    if (h[0] == 0xfe && h[1] == 0xed && h[2] == 0xfa && h[3] == 0xce) return 1;   /* thin ppc */
    if (h[0] == 0xca && h[1] == 0xfe && h[2] == 0xba && h[3] == 0xbe) {          /* fat */
        unsigned int na = OSSwapBigToHostInt32(*(unsigned int *)(h + 4)), i;
        for (i = 0; i < na && 8 + 20 * i + 4 <= (unsigned)n; i++) {
            unsigned int ct = OSSwapBigToHostInt32(*(unsigned int *)(h + 8 + 20 * i));
            if (ct == 18) ppc = 1; else if (ct == 7 || ct == 0x01000007) x86 = 1;
        }
        return ppc | (x86 << 1);
    }
    if ((h[0] == 0xce || h[0] == 0xcf) && h[1] == 0xfa && h[2] == 0xed && h[3] == 0xfe) return 2;  /* thin Intel */
    return 0;
}
/* A helper application inside the running application's own bundle (ScreenFlow's
   Contents/MacOS/ScreenFlowHelper.app, its recording "executive") that has PowerPC code runs
   under the translator too, even when it is universal: it talks to its parent over Distributed
   Objects and shared files that assume one architecture and one runtime, and Mojave would
   start it as a separate, quarantined 32-bit Intel app. OAH_NO_NESTED_HELPER turns this off. */
static int oah_app_is_own_helper(const char *app) {
    const char *ge = getenv("OAH_GUEST_EXE"), *p;
    char exe[1100];
    size_t l;
    if (!ge || getenv("OAH_NO_NESTED_HELPER")) return 0;
    if (!(p = strstr(ge, ".app/"))) return 0;
    l = (size_t)(p - ge) + 5;
    if (strncmp(app, ge, l) || !strstr(app + l, ".app")) return 0;
    return (oah_app_arch(app, exe, sizeof exe) & 1) != 0;
}
static int oah_launch_translated_pid(const char *app, CFArrayRef docs, pid_t *outpid);
static int oah_launch_translated(const char *app, CFArrayRef docs) { return oah_launch_translated_pid(app, docs, NULL); }
/* An application that is already running is not started again: LaunchServices (unless
   kLSLaunchNewInstance) and the Process Manager's LaunchApplication bring the running copy to the
   front (unless asked not to switch) and send it the documents, or 'rapp' without any. Every
   request used to start another translated copy: MSN Messenger asks for its Daemon again when it
   quits, and a second Daemon stayed behind with a "signed out" alert. g_ls_launch_flags carries the
   request's LS flags (0x200 kLSLaunchDontSwitch, 0x80000 kLSLaunchNewInstance) to
   oah_launch_translated_pid. OAH_NO_LSREUSE=1 always starts a new copy. */
static unsigned int g_ls_launch_flags;
static char g_ls_launch_ae[256];   /* file with the next launch's flattened launchAppParameters event (oah_gtrace.h) */
static pid_t oah_translated_running(const char *app) {
    char exe[1100], path[PROC_PIDPATHINFO_MAXSIZE];
    static char buf[32768];
    pid_t pids[4096];
    int n, i;
    if (getenv("OAH_NO_LSREUSE") || (g_ls_launch_flags & 0x00080000)) return 0;
    if (!(oah_app_arch(app, exe, sizeof exe) & 1) || !exe[0]) return 0;
    {   /* the running copy may have been started through a symlink */
        char rp[PATH_MAX];
        if (realpath(exe, rp)) snprintf(exe, sizeof exe, "%s", rp);
    }
    n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof pids) / (int)sizeof(pid_t);
    for (i = 0; i < n; i++) {
        int mib[3] = { CTL_KERN, KERN_PROCARGS2, pids[i] }, argc, k;
        size_t bl = sizeof buf, pos;
        const char *b;
        if (pids[i] <= 0 || proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        b = strrchr(path, '/'); b = b ? b + 1 : path;
        if (strcmp(b, "translate_mojave") && strcmp(b, "translate") && strcmp(b, ".rosetta-translate")) continue;
        if (sysctl(mib, 3, buf, &bl, NULL, 0) || bl < 8) continue;
        memcpy(&argc, buf, 4);
        pos = 4;
        while (pos < bl && buf[pos]) pos++;          /* exec path */
        while (pos < bl && !buf[pos]) pos++;         /* padding */
        for (k = 0; k < argc && pos < bl; k++) {     /* argv: the guest program follows translate */
            if (k > 0) {
                const char *a1 = buf + pos, *s1 = strrchr(a1, '/'), *s2 = strrchr(exe, '/');
                char rp[PATH_MAX];
                if (!strcmp(a1, exe)) return pids[i];
                if (s1 && s2 && !strcmp(s1, s2) && realpath(a1, rp) && !strcmp(rp, exe)) return pids[i];
            }
            pos += strlen(buf + pos) + 1;
        }
    }
    return 0;
}
typedef struct { unsigned int type; void *h; } oah_aedesc_t;
static void oah_reuse_running(pid_t p, CFArrayRef docs) {
    static short (*cd)(unsigned int, const void *, long, oah_aedesc_t *), (*cae)(unsigned int, unsigned int, const oah_aedesc_t *, short, int, oah_aedesc_t *),
                 (*cl)(const void *, long, unsigned char, oah_aedesc_t *), (*pp)(oah_aedesc_t *, long, unsigned int, const void *, long),
                 (*ppd)(oah_aedesc_t *, unsigned int, const oah_aedesc_t *), (*dd)(oah_aedesc_t *);
    static int (*sm)(const oah_aedesc_t *, oah_aedesc_t *, int, long), (*gpfp)(pid_t, unsigned int *), (*sfp)(const unsigned int *);
    oah_aedesc_t tgt = { 0, 0 }, ev = { 0, 0 }, lst = { 0, 0 };
    int i, cnt = docs ? (int)CFArrayGetCount(docs) : 0, err = -1;
    unsigned int psn[2] = { 0, 0 };
    if (!cd) {
        cd = dlsym(RTLD_DEFAULT, "AECreateDesc"); cae = dlsym(RTLD_DEFAULT, "AECreateAppleEvent");
        cl = dlsym(RTLD_DEFAULT, "AECreateList"); pp = dlsym(RTLD_DEFAULT, "AEPutPtr");
        ppd = dlsym(RTLD_DEFAULT, "AEPutParamDesc"); dd = dlsym(RTLD_DEFAULT, "AEDisposeDesc");
        sm = dlsym(RTLD_DEFAULT, "AESendMessage"); gpfp = dlsym(RTLD_DEFAULT, "GetProcessForPID");
        sfp = dlsym(RTLD_DEFAULT, "SetFrontProcess");
    }
    if (!(g_ls_launch_flags & 0x200) && gpfp && sfp && !gpfp(p, psn)) sfp(psn);
    if (cd && cae && cl && pp && ppd && dd && sm && !cd('kpid', &p, sizeof p, &tgt)) {
        if (!cae('aevt', cnt ? 'odoc' : 'rapp', &tgt, -1, 0, &ev)) {
            if (cnt && !cl(NULL, 0, 0, &lst)) {
                for (i = 0; i < cnt; i++) {
                    char u[2048];
                    CFURLRef au = CFURLCopyAbsoluteURL((CFURLRef)CFArrayGetValueAtIndex(docs, i));
                    CFStringRef s = au ? CFURLGetString(au) : NULL;
                    if (s && CFStringGetCString(s, u, sizeof u, kCFStringEncodingUTF8)) pp(&lst, 0, 'furl', u, (long)strlen(u));
                    if (au) CFRelease(au);
                }
                ppd(&ev, '----', &lst);
                dd(&lst);
            }
            err = sm(&ev, NULL, 1 /* kAENoReply */, -1);
            dd(&ev);
        }
        dd(&tgt);
    }
    fprintf(stderr, "[ls] already running as pid %d: %s, %s %d document(s) -> %d\n", (int)p,
            (g_ls_launch_flags & 0x200) ? "not activated" : "activated", cnt ? "odoc" : "rapp", cnt, err);
}
/* LSOpenURLsWithRole(urls, roles, AEKeyDesc *, LSApplicationParameters *, PSN *out, maxPSN), the
   call 10.6's /usr/bin/open and NSWorkspace make, runs in the guest's own LaunchServices, which
   refuses a PowerPC application here: "The application ... cannot be opened because its executable
   is missing." (kLSNoExecutableErr). The export only checks its arguments and branches to
   _LSOpenURLsWithRole_Common (r9 = 0), so that is hooked (follow): when the parameters name a
   PowerPC application, it is started - or the running copy reused - under the translator with the
   URLs as its documents. Everything else goes on to the guest code. OAH_NO_LSURLSFIX=1 disables. */
static int oah_want_lsurlsfix(void) { return !getenv("OAH_NO_LSURLSFIX"); }
static void oah_fix_lsopenurls(unsigned int *R) {
    static unsigned int fcnt, fget;
    static int (*gpfp)(pid_t, unsigned int *);
    unsigned int garr = R[3], par = R[6], fsr, flags, i, cnt = 0, a[2];
    char ap[1100] = "", ex[1100];
    CFURLRef app;
    CFMutableArrayRef items;
    pid_t pid = 0;
    int err;
    if (!par || !(fsr = oah_get32(par + 8)) || !(app = oah_guest_fsref_native(fsr))) return;
    if (!CFURLGetFileSystemRepresentation(app, true, (UInt8 *)ap, sizeof ap) ||
        !(oah_app_is_ppc_only(ap, ex, sizeof ex) || oah_app_is_own_helper(ap))) { CFRelease(app); return; }
    flags = oah_get32(par + 4);
    items = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!fcnt) { fcnt = oah_guest_sym("CoreFoundation", "_CFArrayGetCount"); fget = oah_guest_sym("CoreFoundation", "_CFArrayGetValueAtIndex"); }
    if (garr && fcnt && fget) {
        a[0] = garr; cnt = oah_callppc_n("CFArrayGetCount", fcnt, 1, a);
        for (i = 0; i < cnt && i < 256; i++) {
            CFURLRef d;
            a[0] = garr; a[1] = i;
            d = oah_guest_url_native(oah_callppc_n("CFArrayGetValueAtIndex", fget, 2, a));
            if (d) { CFArrayAppendValue(items, d); CFRelease(d); }
        }
    }
    g_ls_launch_flags = flags;
    err = getenv("OAH_LS_DRYRUN") ? 0 : oah_launch_translated_pid(ap, CFArrayGetCount(items) ? items : NULL, &pid);
    if (R[7] && (int)R[8] > 0) {
        unsigned int psn[2] = { 0, 0 };
        int k, r = -1;
        if (!gpfp) gpfp = (int (*)(pid_t, unsigned int *))dlsym(RTLD_DEFAULT, "GetProcessForPID");
        for (k = 0; !err && pid > 0 && gpfp && k < 100 && (r = gpfp(pid, psn)) != 0; k++) usleep(50000);
        if (r) psn[0] = psn[1] = 0;
        OSWriteBigInt32((void *)R[7], 0, psn[0]);
        OSWriteBigInt32((void *)R[7], 4, psn[1]);
    }
    fprintf(stderr, "[ls] LSOpenURLsWithRole(\"%s\", %d item(s), flags %#x) -> %d (pid %d)\n", ap, (int)CFArrayGetCount(items), flags, err, (int)pid);
    CFRelease(items);
    CFRelease(app);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)err;
}
static int oah_launch_translated_pid(const char *app, CFArrayRef docs, pid_t *outpid) {
    char cmd[4096], q[1100];
    int o, i, cnt = docs ? (int)CFArrayGetCount(docs) : 0, pfd[2] = { -1, -1 };
    const char *base = strrchr(app, '/');
    pid_t pid;
    {   pid_t run = oah_translated_running(app);
        if (run > 0) oah_reuse_running(run, docs);
        g_ls_launch_flags = 0;                       /* one request only */
        if (run > 0) {
            if (outpid) *outpid = run;
            return 0;
        }
    }
    /* the guest-toolbox mode every app needs (SimCity itself runs without it) */
    /* the new app is not this one: drop our identity (OAH_GUEST_EXE, CFProcessPath) and our
       wrapper's launch variables, or the child took them over (SimCity 4's game process checked in
       as its launcher, with the launcher's executable and bundle, and never started) */
    {   /* who launched it: the child reports us as its processLauncher (Office's helpers - Clip Gallery,
           Graph, Organization Chart - quit "can only be launched from Office" when it was 0.0) */
        unsigned int me[2] = { 0, 0 };
        int (*gcp)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
        size_t al = strlen(app);
        char od[2048] = "";
        if (gcp) gcp(me);
        /* Documents for an application bundle go in OAH_OPEN_DOCS (newline-separated), which the new
           process turns into its launch odoc (oah_aen.h), as LaunchServices did; on its command line
           a Carbon application ignored them (Photoshop CS2 started without the picture). */
        if (cnt && al > 4 && (!strcmp(app + al - 4, ".app") || !strcmp(app + al - 5, ".app/")) && !getenv("OAH_LS_ARGVDOCS")) {
            size_t ol = 0;
            for (i = 0; i < cnt; i++)
                if (CFURLGetFileSystemRepresentation((CFURLRef)CFArrayGetValueAtIndex(docs, i), true, (UInt8 *)q, sizeof q) &&
                    !strchr(q, '\'') && ol + strlen(q) + 2 < sizeof od)
                    ol += (size_t)snprintf(od + ol, sizeof od - ol, "%s%s", ol ? "\n" : "", q);
            cnt = 0;                                  /* not on the command line too */
        }
        o = snprintf(cmd, sizeof cmd, "cd '%s' && unset OAH_GUEST_EXE CFProcessPath OAH_RLAUNCH OAH_RLAUNCH_CHILD "
                     "OAH_TRANSLATE_LINK OAH_OPEN_DOCS OAH_PRINT_DOCS OAH_RLAUNCH_FWDPID OAH_LAUNCH_AE; %s%s%s%s%s%sOAH_LAUNCHER_PSN=%u.%u OAH_GTB=1 exec ./rosettus '%s'",
                     g_rosetta_root, od[0] ? "OAH_OPEN_DOCS='" : "", od, od[0] ? "' " : "",
                     g_ls_launch_ae[0] ? "OAH_LAUNCH_AE='" : "", g_ls_launch_ae, g_ls_launch_ae[0] ? "' " : "", me[0], me[1], app);
        g_ls_launch_ae[0] = 0;                       /* one launch only */
    }
    for (i = 0; i < cnt && o < (int)sizeof cmd - 1100; i++) {
        if (CFURLGetFileSystemRepresentation((CFURLRef)CFArrayGetValueAtIndex(docs, i), true, (UInt8 *)q, sizeof q))
            o += snprintf(cmd + o, sizeof cmd - o, " '%s'", q);
    }
    snprintf(cmd + o, sizeof cmd - o, " > '/tmp/oah_child_%s.log' 2>&1", base ? base + 1 : "app");
    if (outpid) { *outpid = 0; if (pipe(pfd)) pfd[0] = pfd[1] = -1; }
    pid = fork();   /* double fork: the launched app is not our child (no zombie, survives us) */
    if (pid == 0) {
        pid_t g = fork();
        if (g == 0) { if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); } setsid(); execl("/bin/sh", "sh", "-c", cmd, (char *)0); _exit(127); }
        if (pfd[1] >= 0) { int w = (int)write(pfd[1], &g, sizeof g); (void)w; }
        _exit(g > 0 ? 0 : 1);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
    if (pfd[0] >= 0) {   /* the app's own pid ("exec ./rosettus" keeps the shell's pid) */
        pid_t g = 0;
        close(pfd[1]);
        if (read(pfd[0], &g, sizeof g) == (ssize_t)sizeof g && g > 0) *outpid = g;
        close(pfd[0]);
    }
    fprintf(stderr, "[ls] PowerPC application started under the translator (pid %d): %s\n", outpid && *outpid ? (int)*outpid : (int)pid, app);
    return pid > 0 ? 0 : -10810;   /* kLSUnknownErr */
}
/* The running guest application's bundle, when its Info.plist claims ext (CFBundleTypeExtensions),
   type (CFBundleTypeOSTypes) or creator (CFBundleSignature): path of the .app in out. Mojave's
   LaunchServices has not registered every PowerPC application (InDesign CS2), while on 10.6 the
   running application was always in the database. */
static int oah_own_app_claims(CFStringRef ext, OSType type, OSType creator, char *out, size_t n) {
    const char *exe = getenv("OAH_GUEST_EXE");
    char path[1100], *m;
    CFURLRef u; CFBundleRef b; CFArrayRef docs; CFTypeRef sig;
    int hit = 0;
    CFIndex i, j;
    if (!exe || !realpath(exe, path) || !(m = strstr(path, ".app/Contents/"))) return 0;
    m[4] = 0;
    u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), true);
    b = u ? CFBundleCreate(NULL, u) : NULL;
    if (u) CFRelease(u);
    if (!b) return 0;
    sig = CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleSignature"));
    if (creator && creator != '????' && sig && CFGetTypeID(sig) == CFStringGetTypeID()) {
        char c[8] = "";
        CFStringGetCString((CFStringRef)sig, c, sizeof c, kCFStringEncodingMacRoman);
        if (strlen(c) == 4 && (OSType)(((unsigned)(unsigned char)c[0] << 24) | ((unsigned char)c[1] << 16) | ((unsigned char)c[2] << 8) | (unsigned char)c[3]) == creator) hit = 1;
    }
    docs = (CFArrayRef)CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleDocumentTypes"));
    if (!hit && docs && CFGetTypeID(docs) == CFArrayGetTypeID())
        for (i = 0; i < CFArrayGetCount(docs) && !hit; i++) {
            CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(docs, i);
            CFArrayRef a;
            if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
            a = (CFArrayRef)CFDictionaryGetValue(d, CFSTR("CFBundleTypeExtensions"));
            if (ext && a && CFGetTypeID(a) == CFArrayGetTypeID())
                for (j = 0; j < CFArrayGetCount(a) && !hit; j++) {
                    CFStringRef e = (CFStringRef)CFArrayGetValueAtIndex(a, j);
                    if (e && CFGetTypeID(e) == CFStringGetTypeID() && CFStringCompare(e, ext, kCFCompareCaseInsensitive) == kCFCompareEqualTo) hit = 1;
                }
            a = (CFArrayRef)CFDictionaryGetValue(d, CFSTR("CFBundleTypeOSTypes"));
            if (type && type != '????' && a && CFGetTypeID(a) == CFArrayGetTypeID())
                for (j = 0; j < CFArrayGetCount(a) && !hit; j++) {
                    CFStringRef t = (CFStringRef)CFArrayGetValueAtIndex(a, j);
                    char c[8] = "";
                    if (t && CFGetTypeID(t) == CFStringGetTypeID() && CFStringGetCString(t, c, sizeof c, kCFStringEncodingMacRoman) && strlen(c) == 4 &&
                        (OSType)(((unsigned)(unsigned char)c[0] << 24) | ((unsigned char)c[1] << 16) | ((unsigned char)c[2] << 8) | (unsigned char)c[3]) == type) hit = 1;
                }
        }
    CFRelease(b);
    if (hit) strlcpy(out, path, n);
    return hit;
}
/* kEventAppLaunchNotification ('appl'/4) for an asynchronous launch (kLSLaunchDefaults or
   kLSLaunchAsync): LaunchServices sends it to the launching application once the new process is
   running, with kEventParamProcessID, kEventParamLaunchRefCon (the spec's asyncRefCon) and
   kEventParamLaunchErr. Word 2004 starts Microsoft Database Daemon that way and waits up to 30 s
   for it in ReceiveNextEvent; we never sent it, so the first menu or document after a cold start
   took 30 s. Built and posted with the guest's Carbon Event Manager from the calling guest
   thread. OAH_NO_LAUNCHNOTIF=1 disables. */
static void oah_post_launch_notification(unsigned int *R, const unsigned int psn[2], unsigned int refcon, int lerr) {
    static unsigned int fc, fs, fp, fq, fr;
    unsigned int a[9], ev = 0, q, be[2];
    int e;
    if (getenv("OAH_NO_LAUNCHNOTIF")) return;
    if (!fc) { fc = oah_guest_sym("HIToolbox", "_CreateEvent"); fs = oah_guest_sym("HIToolbox", "_SetEventParameter");
               fp = oah_guest_sym("HIToolbox", "_PostEventToQueue"); fq = oah_guest_sym("HIToolbox", "_GetMainEventQueue");
               fr = oah_guest_sym("HIToolbox", "_ReleaseEvent"); }
    if (!fc || !fs || !fp || !fq || !fr) { fprintf(stderr, "[ls] launch notification: Carbon Event Manager not found\n"); return; }
    {   double *F = (double *)((unsigned char *)R + 0x1e8);
        F[1] = 0.0;   /* inWhen (f1): now */
    }
    /* CreateEvent(allocator, class, kind, EventTime when [f1; r6/r7 skipped], attributes, EventRef *out) */
    a[0] = 0; a[1] = 'appl'; a[2] = 4; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = (unsigned int)be;
    be[0] = 0;
    e = (short)oah_callppc_n("CreateEvent", fc, 7, a);
    ev = OSSwapBigToHostInt32(be[0]);
    if (e || !ev) { fprintf(stderr, "[ls] launch notification: CreateEvent -> %d\n", e); return; }
    {   unsigned int pbe[2] = { OSSwapHostToBigInt32(psn[0]), OSSwapHostToBigInt32(psn[1]) }, rbe = OSSwapHostToBigInt32(refcon), ebe = OSSwapHostToBigInt32((unsigned int)lerr);
        a[0] = ev; a[1] = 'psn '; a[2] = 'psn '; a[3] = 8; a[4] = (unsigned int)pbe; oah_callppc_n("SetEventParameter", fs, 5, a);
        a[0] = ev; a[1] = 'lref'; a[2] = 'magn'; a[3] = 4; a[4] = (unsigned int)&rbe; oah_callppc_n("SetEventParameter", fs, 5, a);
        a[0] = ev; a[1] = 'err '; a[2] = 'osst'; a[3] = 4; a[4] = (unsigned int)&ebe; oah_callppc_n("SetEventParameter", fs, 5, a);
    }
    q = oah_callppc_n("GetMainEventQueue", fq, 0, a);
    a[0] = q; a[1] = ev; a[2] = 1;   /* kEventPriorityStandard */
    e = (short)oah_callppc_n("PostEventToQueue", fp, 3, a);
    a[0] = ev; oah_callppc_n("ReleaseEvent", fr, 1, a);
    fprintf(stderr, "[ls] kEventAppLaunchNotification for psn %u.%u (refcon %#x) posted -> %d\n", psn[0], psn[1], refcon, e);
}
static int oah_ls_open_call(const char *n, unsigned int *R) {
    /* Uniform Type Identifiers: the guest LaunchServices database never saw the applications
       installed on Mojave, so their declared types were unknown (iWeb: "Assertion failure:
       Method: domainFilenameExtension", then an exception and no window). Mojave's database
       knows them. */
    if (!strncmp(n, "UTType", 6)) {
        static void *f[8];
        static const char *const nm[8] = { "UTTypeCopyPreferredTagWithClass", "UTTypeCreatePreferredIdentifierForTag",
                                           "UTTypeCreateAllIdentifiersForTag", "UTTypeConformsTo", "UTTypeCopyDescription",
                                           "UTTypeCopyDeclaration", "UTTypeCopyAllTagsWithClass", 0 };
        int i;
        for (i = 0; nm[i] && strcmp(nm[i], n); i++) ;
        if (!nm[i]) return 0;
        if (!f[i]) f[i] = oah_nls(nm[i]);
        if (!f[i]) return 0;
        {
            CFStringRef a0 = oah_guest_str_native(R[3]), a1 = (i <= 3 || i == 6) ? oah_guest_str_native(R[4]) : NULL,
                        a2 = (i == 1 || i == 2) ? oah_guest_str_native(R[5]) : NULL;
            CFTypeRef r = NULL;
            unsigned int out = 0;
            switch (i) {
            case 0: r = ((CFStringRef (*)(CFStringRef, CFStringRef))f[i])(a0, a1); break;
            case 1: r = ((CFStringRef (*)(CFStringRef, CFStringRef, CFStringRef))f[i])(a0, a1, a2); break;
            case 2: r = ((CFArrayRef (*)(CFStringRef, CFStringRef, CFStringRef))f[i])(a0, a1, a2); break;
            case 3: out = (a0 && a1) ? (unsigned int)(((Boolean (*)(CFStringRef, CFStringRef))f[i])(a0, a1) != 0) : 0; break;
            case 4: r = ((CFStringRef (*)(CFStringRef))f[i])(a0); break;
            case 5: r = ((CFDictionaryRef (*)(CFStringRef))f[i])(a0); break;
            case 6: r = ((CFArrayRef (*)(CFStringRef, CFStringRef))f[i])(a0, a1); break;
            }
            if (i != 3) out = r ? oah_cf_to_guest(r) : 0;
            if (oah_lstr()) {
                char s0[160] = "", s1[160] = "", so[160] = "";
                if (a0) CFStringGetCString(a0, s0, sizeof s0, kCFStringEncodingUTF8);
                if (a1) CFStringGetCString(a1, s1, sizeof s1, kCFStringEncodingUTF8);
                if (r && CFGetTypeID(r) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)r, so, sizeof so, kCFStringEncodingUTF8);
                fprintf(stderr, "[ls] %s(%s, %s) -> %s %08x\n", n, s0, s1, so, out);
            }
            if (a0) CFRelease(a0);
            if (a1) CFRelease(a1);
            if (a2) CFRelease(a2);
            if (r) CFRelease(r);
            R[3] = out;
            return 1;
        }
    }
    if (!strcmp(n, "LSCopyDefaultHandlerForURLScheme")) {   /* (CFStringRef scheme) -> CFStringRef bundle id */
        CFStringRef (*fn)(CFStringRef) = (CFStringRef (*)(CFStringRef))oah_nls("LSCopyDefaultHandlerForURLScheme");
        CFStringRef s = oah_guest_str_native(R[3]), r = (fn && s) ? fn(s) : NULL;
        R[3] = r ? oah_cf_to_guest(r) : 0;
        if (oah_lstr()) fprintf(stderr, "[ls] default handler -> guest %08x\n", R[3]);
        if (s) CFRelease(s);
        if (r) CFRelease(r);
        return 1;
    }
    if (!strcmp(n, "LSGetApplicationForURL")) {   /* (CFURLRef, roles, FSRef *outApp, CFURLRef *outAppURL) */
        OSStatus (*fn)(CFURLRef, unsigned int, void *, CFURLRef *) =
            (OSStatus (*)(CFURLRef, unsigned int, void *, CFURLRef *))oah_nls("LSGetApplicationForURL");
        CFURLRef u = oah_guest_url_native(R[3]), app = NULL;
        OSStatus err = (fn && u) ? fn(u, R[4], NULL, &app) : -10814;   /* kLSApplicationNotFoundErr */
        char path[1024];
        if (!err && app && CFURLGetFileSystemRepresentation(app, true, (UInt8 *)path, sizeof path)) {
            unsigned int a[4];
            if (R[5]) {
                static unsigned int fpm;
                if (!fpm) fpm = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
                a[0] = (unsigned int)path; a[1] = R[5]; a[2] = 0;
                if (fpm) oah_callppc_n("FSPathMakeRef", fpm, 3, a);
            }
            if (R[6]) {
                static unsigned int fmk;
                if (!fmk) fmk = oah_guest_sym("CoreFoundation", "_CFURLCreateFromFileSystemRepresentation");
                a[0] = 0; a[1] = (unsigned int)path; a[2] = strlen(path); a[3] = 1;
                *(unsigned int *)R[6] = OSSwapHostToBigInt32(fmk ? oah_callppc_n("CFURLCreateFromFSR", fmk, 4, a) : 0);
            }
        } else if (!err) err = -10814;
        if (oah_lstr()) fprintf(stderr, "[ls] LSGetApplicationForURL -> %d %s\n", (int)err, err ? "" : path);
        if (u) CFRelease(u);
        if (app) CFRelease(app);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "LSGetApplicationForInfo") && !getenv("OAH_NO_LSAPPINFO")) {
        /* (OSType type, OSType creator, CFStringRef ext, roles, FSRef *outApp, CFURLRef *outAppURL).
           The guest LaunchServices database has none of the applications installed on Mojave, so it
           knew no application for "cwk": the 10.6 Save panel then took AppleWorks' ".cwk" for an
           unknown extension and appended its own each time (Save As "untitled.cwk.cwk.cwk").
           Mojave's database knows them (and the PowerPC applications it has seen). */
        OSStatus (*fn)(OSType, OSType, CFStringRef, unsigned int, void *, CFURLRef *) =
            (OSStatus (*)(OSType, OSType, CFStringRef, unsigned int, void *, CFURLRef *))oah_nls("LSGetApplicationForInfo");
        CFStringRef ext = R[5] ? oah_guest_str_native(R[5]) : NULL;
        CFURLRef app = NULL;
        OSStatus err = fn ? fn((OSType)R[3], (OSType)R[4], ext, R[6], NULL, &app) : -10814;
        char path[1024] = "";
        if (!err && app) { if (!CFURLGetFileSystemRepresentation(app, true, (UInt8 *)path, sizeof path)) path[0] = 0; }
        else if (oah_own_app_claims(ext, (OSType)R[3], (OSType)R[4], path, sizeof path)) err = 0;   /* the running app claims it */
        if (!err && path[0]) {
            unsigned int a[4];
            if (R[7]) {
                static unsigned int fpm;
                if (!fpm) fpm = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
                a[0] = (unsigned int)path; a[1] = R[7]; a[2] = 0;
                if (fpm) oah_callppc_n("FSPathMakeRef", fpm, 3, a);
            }
            if (R[8]) {
                static unsigned int fmk;
                if (!fmk) fmk = oah_guest_sym("CoreFoundation", "_CFURLCreateFromFileSystemRepresentation");
                a[0] = 0; a[1] = (unsigned int)path; a[2] = strlen(path); a[3] = 1;
                *(unsigned int *)R[8] = OSSwapHostToBigInt32(fmk ? oah_callppc_n("CFURLCreateFromFSR", fmk, 4, a) : 0);
            }
        } else if (!err) err = -10814;
        if (oah_lstr()) {
            char e[64] = "";
            if (ext) CFStringGetCString(ext, e, sizeof e, kCFStringEncodingUTF8);
            fprintf(stderr, "[ls] LSGetApplicationForInfo(%08x, %08x, %s) -> %d %s\n", R[3], R[4], e, (int)err, path);
        }
        if (ext) CFRelease(ext);
        if (app) CFRelease(app);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "LSOpenCFURLRef")) {   /* (CFURLRef, CFURLRef *outLaunchedURL) */
        OSStatus (*fn)(CFURLRef, CFURLRef *) = (OSStatus (*)(CFURLRef, CFURLRef *))oah_nls("LSOpenCFURLRef");
        CFURLRef u = oah_guest_url_native(R[3]);
        char up[1100], ue[1100];
        OSStatus err;
        if (u && CFURLGetFileSystemRepresentation(u, true, (UInt8 *)up, sizeof up) &&
            oah_app_is_ppc_only(up, ue, sizeof ue) && !getenv("OAH_LS_DRYRUN"))
            err = oah_launch_translated(up, NULL);       /* a PowerPC application itself */
        else err = (fn && u) ? fn(u, NULL) : -50;
        if (R[4]) *(unsigned int *)R[4] = 0;
        if (oah_lstr()) fprintf(stderr, "[ls] LSOpenCFURLRef -> %d\n", (int)err);
        if (u) CFRelease(u);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "LSOpenApplication")) {
        /* (const LSApplicationParameters *, ProcessSerialNumber *outPSN). Parameters: version,
           flags, const FSRef *application, asyncRefCon, environment, argv, initialEvent.
           The guest LaunchServices could not launch anything on Mojave: ScreenFlow's "New
           Recording" started its ScreenFlowHelper.app this way, and nothing happened. */
        unsigned int p = R[3];
        CFURLRef app = p ? oah_guest_fsref_native(oah_get32(p + 8)) : NULL;
        char ap[1100] = "", ex[1100];
        OSStatus err = -50;
        if (app && CFURLGetFileSystemRepresentation(app, true, (UInt8 *)ap, sizeof ap)) {
            if (getenv("OAH_LS_DRYRUN")) err = 0;
            else if (oah_app_is_ppc_only(ap, ex, sizeof ex) || oah_app_is_own_helper(ap)) {
                g_ls_launch_flags = oah_get32(p + 4);
                err = oah_launch_translated(ap, NULL);
            }
            else {
                OSStatus (*fn)(const oah_LSLaunchURLSpec *, CFURLRef *) =
                    (OSStatus (*)(const oah_LSLaunchURLSpec *, CFURLRef *))oah_nls("LSOpenFromURLSpec");
                oah_LSLaunchURLSpec ns;
                memset(&ns, 0, sizeof ns);
                ns.appURL = app;
                ns.launchFlags = oah_get32(p + 4) | 0x00010000;   /* kLSLaunchAsync */
                err = fn ? fn(&ns, NULL) : -50;
            }
        }
        if (R[4]) { ((unsigned int *)R[4])[0] = 0; ((unsigned int *)R[4])[1] = 0; }   /* kNoProcess */
        fprintf(stderr, "[ls] LSOpenApplication(\"%s\", flags %#x) -> %d\n", ap, p ? oah_get32(p + 4) : 0, (int)err);
        if (app) CFRelease(app);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "LSOpenFromURLSpec") || !strcmp(n, "LSOpenFromRefSpec")) {
        /* URLSpec: appURL, itemURLs (CFArray), passThru, launchFlags, asyncRefCon.
           RefSpec: appRef, numDocs, itemRefs (FSRef[]), passThru, launchFlags, asyncRefCon. */
        OSStatus (*fn)(const oah_LSLaunchURLSpec *, CFURLRef *) =
            (OSStatus (*)(const oah_LSLaunchURLSpec *, CFURLRef *))oah_nls("LSOpenFromURLSpec");
        int ref = n[10] == 'R';           /* LSOpenFrom[R]efSpec / LSOpenFrom[U]RLSpec */
        unsigned int s = R[3], i, cnt = 0;
        CFMutableArrayRef items = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        oah_LSLaunchURLSpec ns;
        OSStatus err;
        memset(&ns, 0, sizeof ns);
        if (!s) { CFRelease(items); R[3] = (unsigned int)-50; return 1; }
        if (ref) {
            unsigned int app = oah_get32(s), nd = oah_get32(s + 4), docs = oah_get32(s + 8);
            ns.appURL = oah_guest_fsref_native(app);
            for (i = 0; i < nd && i < 256 && docs; i++) {
                CFURLRef d = oah_guest_fsref_native(docs + 80 * i);   /* sizeof(FSRef) */
                if (d) { CFArrayAppendValue(items, d); CFRelease(d); }
            }
            ns.launchFlags = oah_get32(s + 16);
        } else {
            static unsigned int fcnt, fget;
            unsigned int garr = oah_get32(s + 4), a[2];
            ns.appURL = oah_guest_url_native(oah_get32(s));
            if (!fcnt) { fcnt = oah_guest_sym("CoreFoundation", "_CFArrayGetCount"); fget = oah_guest_sym("CoreFoundation", "_CFArrayGetValueAtIndex"); }
            if (garr && fcnt && fget) {
                a[0] = garr; cnt = oah_callppc_n("CFArrayGetCount", fcnt, 1, a);
                for (i = 0; i < cnt && i < 256; i++) {
                    CFURLRef d;
                    a[0] = garr; a[1] = i;
                    d = oah_guest_url_native(oah_callppc_n("CFArrayGetValueAtIndex", fget, 2, a));
                    if (d) { CFArrayAppendValue(items, d); CFRelease(d); }
                }
            }
            ns.launchFlags = oah_get32(s + 12);
        }
        /* No application, and items that are themselves PowerPC-only applications (Photoshop
           CS2's Help > Updates opens "Adobe Updater.app" that way): Mojave refused them
           (-10665, "Adobe Updater could not be started"). Start each under the translator and
           hand only the remaining items to Mojave. */
        if (!ns.appURL && CFArrayGetCount(items) && !getenv("OAH_LS_DRYRUN")) {
            CFIndex i2;
            int started = 0;
            for (i2 = CFArrayGetCount(items) - 1; i2 >= 0; i2--) {
                char ip2[1100], ex2[1100];
                if (CFURLGetFileSystemRepresentation((CFURLRef)CFArrayGetValueAtIndex(items, i2), true, (UInt8 *)ip2, sizeof ip2) &&
                    oah_app_is_ppc_only(ip2, ex2, sizeof ex2) && !oah_launch_translated(ip2, NULL)) {
                    CFArrayRemoveValueAtIndex(items, i2);
                    started++;
                }
            }
            if (started && !CFArrayGetCount(items)) {
                fprintf(stderr, "[ls] %s: %d PowerPC application(s) started under the translator\n", n, started);
                if (ns.appURL) CFRelease(ns.appURL);
                CFRelease(items);
                if (R[4]) *(unsigned int *)R[4] = 0;
                R[3] = 0;
                return 1;
            }
        }
        if (CFArrayGetCount(items)) ns.itemURLs = items;
        {   char ap[1100], ex[1100];
            if (ns.appURL && CFURLGetFileSystemRepresentation(ns.appURL, true, (UInt8 *)ap, sizeof ap) &&
                (oah_app_is_ppc_only(ap, ex, sizeof ex) || oah_app_is_own_helper(ap)) && !getenv("OAH_LS_DRYRUN")) {
                pid_t lpid = 0;
                g_ls_launch_flags = ns.launchFlags;
                err = oah_launch_translated_pid(ap, ns.itemURLs, &lpid);
                /* Wait up to 5 s for the new application to check in (its PSN is needed for the
                   launch notification below). OAH_NO_LSSYNCWAIT=1 returns at once. */
                if (!err && lpid > 0 && !getenv("OAH_NO_LSSYNCWAIT")) {
                    static int (*gpfp)(pid_t, unsigned int *);
                    unsigned int psn[2] = { 0, 0 };
                    struct timeval t0, t1;
                    int k, r = -1;
                    if (!gpfp) gpfp = (int (*)(pid_t, unsigned int *))dlsym(RTLD_DEFAULT, "GetProcessForPID");
                    gettimeofday(&t0, NULL);
                    for (k = 0; gpfp && k < 100 && (r = gpfp(lpid, psn)) != 0 && !(kill(lpid, 0) && errno == ESRCH); k++) usleep(50000);
                    gettimeofday(&t1, NULL);
                    fprintf(stderr, "[ls] %s: pid %d %s after %ld ms\n", n, (int)lpid, r ? "did not check in" : "checked in",
                            (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
                    if (!r && (ns.launchFlags & 0x00010001))   /* kLSLaunchAsync or kLSLaunchDefaults */
                        oah_post_launch_notification(R, psn, ref ? oah_get32(s + 20) : oah_get32(s + 16), 0);
                }
            }
            else err = getenv("OAH_LS_DRYRUN") ? 0 : (fn && (ns.appURL || ns.itemURLs)) ? fn(&ns, NULL) : -50;
        }
        if (R[4]) *(unsigned int *)R[4] = 0;
        {   char ap[512] = "", ip[512] = "";
            if (ns.appURL) CFURLGetFileSystemRepresentation(ns.appURL, true, (UInt8 *)ap, sizeof ap);
            if (CFArrayGetCount(items)) {
                CFURLRef f = (CFURLRef)CFArrayGetValueAtIndex(items, 0);
                if (!CFURLGetFileSystemRepresentation(f, true, (UInt8 *)ip, sizeof ip)) {
                    CFStringRef us = CFURLGetString(f);
                    if (us) CFStringGetCString(us, ip, sizeof ip, kCFStringEncodingUTF8);
                }
            }
            fprintf(stderr, "[ls] %s: app \"%s\", %ld item(s) first \"%s\", flags %#x -> %d\n", n, ap,
                    (long)CFArrayGetCount(items), ip, ns.launchFlags, (int)err);
        }
        if (ns.appURL) CFRelease(ns.appURL);
        CFRelease(items);
        R[3] = (unsigned int)err;
        return 1;
    }
    return 0;
}
static int oah_ls_call(const char *n, unsigned int *R) {
    static int off = -1;
    if (off < 0) off = getenv("OAH_NO_LSBRIDGE") ? 1 : 0;
    if (n[0] == '_' && n[1] == '_') n++;            /* spec names carry both underscores */
    if (!off && oah_ls_open_call(n, R)) return 1;
    if (!strcmp(n, "GetProcessInformation")) {
        /* (const ProcessSerialNumber *psn, ProcessInfoRec *info).  Guest PSNs are native, so ask Mojave and
           convert.  Was an unimplemented stub returning noErr with the record untouched: Internet Explorer
           then used uninitialized fields (0xededed..) as pointers and crashed in a background thread.
           ProcessInfoRec (pack 2, 60 bytes): 0 length, 4 StringPtr name, 8 PSN, 16 type, 20 signature,
           24 mode, 28 location, 32 size, 36 freeMem, 40 launcher PSN, 48 launchDate, 52 activeTime,
           56 FSSpecPtr appSpec.  FSSpec: short vRefNum, long parID, Str63 name (70 bytes). */
        static int (*gpi)(const void *, void *);
        unsigned char nrec[60], nname[64], nspec[70], *gi = (unsigned char *)R[4];
        unsigned int psn[2], gname, gspec, i, v;
        int err;
        if (!g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        if (!gpi && g_carbon_handle) gpi = (int (*)(const void *, void *))dlsym(g_carbon_handle, "GetProcessInformation");
        if (!gpi || !R[3] || !R[4]) { R[3] = (unsigned int)-50; return 1; }
        psn[0] = OSSwapBigToHostInt32(((unsigned int *)R[3])[0]);
        psn[1] = OSSwapBigToHostInt32(((unsigned int *)R[3])[1]);
        gname = OSSwapBigToHostInt32(*(unsigned int *)(gi + 4));
        gspec = OSSwapBigToHostInt32(*(unsigned int *)(gi + 56));
        /* Vue 5 Esprit's installer allocates the record with operator new and sets only
           processAppSpec: processInfoLength and processName are whatever the heap held (zero on
           a fresh 10.4 heap, 0xf here), and copying the name to 0xf crashed. Take a pointer that
           is not writable guest memory as NULL, as the zeroed field would have been. */
        if (gname && (gname < 0x1000 || !oah_readable(gname, 64))) {
            if (oah_lstr()) fprintf(stderr, "[ls] GetProcessInformation: processName %#x ignored\n", gname);
            gname = 0;
        }
        if (gspec && (gspec < 0x1000 || !oah_readable(gspec, 70))) gspec = 0;
        memset(nrec, 0, sizeof nrec);
        *(unsigned int *)nrec = 60;
        *(void **)(nrec + 4) = gname ? nname : 0;
        *(void **)(nrec + 56) = gspec ? nspec : 0;
        err = gpi(psn, nrec);
        if (!err && !*(unsigned int *)(nrec + 40) && !*(unsigned int *)(nrec + 44) && getenv("OAH_LAUNCHER_PSN") &&
            ((psn[0] == 0 && psn[1] == 2) || oah_ls_is_self(psn))) {
            /* started by another translated app (LaunchApplication, oah_launch_translated_pid): report it
               as our launcher, as the 10.6 Process Manager did */
            unsigned int lh = 0, ll = 0;
            if (sscanf(getenv("OAH_LAUNCHER_PSN"), "%u.%u", &lh, &ll) == 2 && (lh || ll)) {
                *(unsigned int *)(nrec + 40) = lh; *(unsigned int *)(nrec + 44) = ll;
            }
        }
        if (!err) {
            for (i = 8; i < 56; i += 4) {                 /* all 32-bit fields from PSN to activeTime */
                if (i == 28) v = 0; else v = *(unsigned int *)(nrec + i);   /* processLocation: no guest meaning */
                *(unsigned int *)(gi + i) = OSSwapHostToBigInt32(v);
            }
            if (gname) memcpy((void *)gname, nname, nname[0] < 63 ? nname[0] + 1 : 64);
            if (gspec) {
                /* processAppSpec: Mojave's FSSpec carries Mojave's own volume refnum (and, for our own
                   process, a bogus parID - 748 for AppleWorks, a random .lproj). The guest File Manager
                   numbers volumes itself, so FSpMakeFSRef on it failed with nsvErr about half the time
                   (Adobe's MoverTool named its message port from it: CFStringCreateCopy(NULL), crash,
                   CS2 Version Cue's install failed). Rebuild it from the path: the guest executable
                   for ourselves, else Mojave's FSSpec -> path; guest vref of the folder, folder inode
                   as parID, file name. */
                static OSErr (*mkref)(const void *, void *); static OSStatus (*mkpath)(const void *, unsigned char *, unsigned int);
                char full[PATH_MAX] = "", rp[PATH_MAX], *sl; struct stat st; short gv;
                if (!mkref && g_carbon_handle) {
                    mkref = (OSErr (*)(const void *, void *))dlsym(g_carbon_handle, "FSpMakeFSRef");
                    mkpath = (OSStatus (*)(const void *, unsigned char *, unsigned int))dlsym(g_carbon_handle, "FSRefMakePath");
                }
                if ((psn[0] == 0 && psn[1] == 2) || oah_ls_is_self(psn)) {
                    const char *exe = getenv("CFProcessPath");
                    if (!exe) exe = getenv("OAH_GUEST_EXE");
                    if (exe && realpath(exe, rp)) strlcpy(full, rp, sizeof full);
                }
                if (!full[0] && mkref && mkpath) {
                    unsigned char ref[80];
                    if (!mkref(nspec, ref) && !mkpath(ref, (unsigned char *)rp, sizeof rp)) strlcpy(full, rp, sizeof full);
                }
                if (full[0] && (sl = strrchr(full, '/'))) {
                    size_t L = strlen(sl + 1); if (L > 63) L = 63;
                    *sl = 0;
                    if (!stat(full[0] ? full : "/", &st)) {
                        *(unsigned int *)(nspec + 2) = (unsigned int)st.st_ino;
                        nspec[6] = (unsigned char)L; memcpy(nspec + 7, sl + 1, L);
                        if (!oah_guest_vref_for(full[0] ? full : "/", 0, &gv)) *(unsigned short *)nspec = (unsigned short)gv;
                    }
                }
            }
            if (gspec) {
                unsigned char *gs = (unsigned char *)gspec;
                *(unsigned short *)gs = OSSwapHostToBigInt16(*(unsigned short *)nspec);
                *(unsigned int *)(gs + 2) = OSSwapHostToBigInt32(*(unsigned int *)(nspec + 2));
                memcpy(gs + 6, nspec + 6, 64);
            }
        }
        if (oah_lstr() && !err) fprintf(stderr, "[ls]   type %.4s sig %.4s mode %08x size %u free %u launcher %u.%u\n",
                                         (char *)nrec + 16, (char *)nrec + 20, *(unsigned int *)(nrec + 24), *(unsigned int *)(nrec + 32),
                                         *(unsigned int *)(nrec + 36), *(unsigned int *)(nrec + 40), *(unsigned int *)(nrec + 44));
        if (oah_lstr()) fprintf(stderr, "[ls] GetProcessInformation psn %u.%u -> %d spec vref %d parID %u name %.*s\n", psn[0], psn[1], err,
                                gspec ? (short)*(unsigned short *)nspec : 0, gspec ? *(unsigned int *)(nspec + 2) : 0, gspec ? nspec[6] : 0, gspec ? (char *)nspec + 7 : "");
        R[3] = (unsigned int)err;
        return 1;
    }
    if (off || n[0] != '_' || n[1] != 'L' || n[2] != 'S') return 0;
    if (!strcmp(n, "_LSApplicationCheckIn")) {   /* (LSSessionID, CFDictionaryRef info) -> CFDictionaryRef app info */
        static CFTypeRef (*mk)(CFAllocatorRef, pid_t);
        struct { unsigned int hi, lo; } psn = { 0, 0 };
        int (*gcp)(void *);
        CFTypeRef nasn;
        if (!g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        gcp = g_carbon_handle ? (int (*)(void *))dlsym(g_carbon_handle, "GetCurrentProcess") : 0;
        if (!mk) mk = (CFTypeRef (*)(CFAllocatorRef, pid_t))oah_nls("_LSASNCreateWithPid");
        if (gcp) gcp(&psn);
        nasn = mk ? mk(NULL, getpid()) : NULL;
        R[3] = psn.lo ? oah_ls_info_guest(nasn, psn.hi, psn.lo, R[4]) : 0;
        if (R[4] && R[3]) {             /* remember the app's own info for later CopyApplicationInformation calls */
            static unsigned int fret;
            unsigned int a1[1];
            if (!fret) fret = oah_guest_sym("CoreFoundation", "_CFRetain");
            if (g_ls_self_info) gcall(G_RELEASE, 1, g_ls_self_info, 0, 0, 0);
            a1[0] = R[4];
            g_ls_self_info = fret ? oah_callppc_n("oah_ls_ret", fret, 1, a1) : 0;
            g_ls_self_lo = psn.lo;
        }
        if (nasn) CFRelease(nasn);
        if (oah_lstr()) fprintf(stderr, "[ls] ApplicationCheckIn -> guest dict 0x%08x for psn %u.%u\n", R[3], psn.hi, psn.lo);
        return 1;
    }
    if (!strcmp(n, "_LSCopyApplicationInformationItem")) {   /* (session, ASN, CFStringRef key) */
        static CFTypeRef (*cp)(int, CFTypeRef, CFStringRef);
        CFTypeRef asn, v = NULL;
        CFStringRef key;
        char kb[128] = "";
        unsigned int hi = 0, lo = 0;
        if (!cp) cp = (CFTypeRef (*)(int, CFTypeRef, CFStringRef))oah_nls("_LSCopyApplicationInformationItem");
        asn = oah_guest_asn_native(R[4], &hi, &lo);
        if (g_ls_self_info && (lo == g_ls_self_lo || (hi == 0 && lo == 2)) && R[5]) {      /* the app's own check-in info first */
            static unsigned int fget, fret;
            unsigned int a2[2], gv;
            if (!fget) fget = oah_guest_sym("CoreFoundation", "_CFDictionaryGetValue");
            if (!fret) fret = oah_guest_sym("CoreFoundation", "_CFRetain");
            a2[0] = g_ls_self_info; a2[1] = R[5];
            gv = (fget && fret) ? oah_callppc_n("oah_ls_get", fget, 2, a2) : 0;
            if (gv) {
                a2[0] = gv;
                R[3] = oah_callppc_n("oah_ls_ret", fret, 1, a2);
                if (asn) CFRelease(asn);
                if (oah_lstr()) fprintf(stderr, "[ls] CopyApplicationInformationItem(self) -> guest 0x%08x (check-in info)\n", R[3]);
                return 1;
            }
        }
        key = oah_guest_str_native(R[5]);
        if (key) CFStringGetCString(key, kb, sizeof kb, kCFStringEncodingUTF8);
        if (cp && asn && key) v = cp(-2, asn, key);
        R[3] = v ? oah_cf_to_guest(v) : 0;
        if (!R[3] && v) R[3] = oah_ls_asn_to_guest(v);
        if (!R[3] && oah_guest_cf_load() && (!strncmp(kb, "LSBackgroundOnly", 16) || !strncmp(kb, "LSUIElement", 11) ||
                                            !strncmp(kb, "Is", 2) || strstr(kb, "Hidden") || strstr(kb, "Stopped")))
            R[3] = OSSwapBigToHostInt32(*(unsigned int *)g_cf[G_FALSE]);
        if (oah_lstr()) fprintf(stderr, "[ls] CopyApplicationInformationItem(psn %u.%u, \"%s\") -> native %p guest 0x%08x\n",
                                hi, lo, kb, v, R[3]);
        if (v) CFRelease(v);
        if (key) CFRelease(key);
        if (asn) CFRelease(asn);
        return 1;
    }
    if (!strcmp(n, "_LSCopyMetaApplicationInformationItem")) {   /* (session, CFStringRef key) */
        /* HIToolbox enables the app menu's Hide <app> / Hide Others while more than one app is
           unhidden and Show All while any is hidden (_kLSUnhiddenApplicationCountKey /
           _kLSHiddenApplicationCountKey). The guest's own LaunchServices has no coreservicesd to
           ask, so all three stayed disabled. */
        static CFTypeRef (*cp)(int, CFStringRef);
        CFStringRef key = oah_guest_str_native(R[4]);
        CFTypeRef v = NULL;
        char kb[128] = "";
        if (!cp) cp = (CFTypeRef (*)(int, CFStringRef))oah_nls("_LSCopyMetaApplicationInformationItem");
        if (key) CFStringGetCString(key, kb, sizeof kb, kCFStringEncodingUTF8);
        if (cp && key) v = cp(-2, key);
        R[3] = v ? oah_cf_to_guest(v) : 0;
        if (oah_lstr()) fprintf(stderr, "[ls] CopyMetaApplicationInformationItem(\"%s\") -> native %p guest 0x%08x\n", kb, v, R[3]);
        if (v) CFRelease(v);
        if (key) CFRelease(key);
        return 1;
    }
    if (!strcmp(n, "_LSCopyApplicationInformation")) {       /* (session, ASN, CFArrayRef keys) */
        static CFDictionaryRef (*cp)(int, CFTypeRef, CFArrayRef);
        CFTypeRef asn;
        CFDictionaryRef d = NULL;
        unsigned int hi = 0, lo = 0;
        if (!cp) cp = (CFDictionaryRef (*)(int, CFTypeRef, CFArrayRef))oah_nls("_LSCopyApplicationInformation");
        asn = oah_guest_asn_native(R[4], &hi, &lo);
        R[3] = asn ? oah_ls_info_guest(asn, hi, lo, (lo == g_ls_self_lo || (hi == 0 && lo == 2)) ? g_ls_self_info : 0) : 0;
        if (oah_lstr()) fprintf(stderr, "[ls] CopyApplicationInformation(psn %u.%u) -> %ld keys, guest 0x%08x\n",
                                hi, lo, d ? (long)CFDictionaryGetCount(d) : -1L, R[3]);
        if (d) CFRelease(d);
        if (asn) CFRelease(asn);
        return 1;
    }
    return 0;
}
