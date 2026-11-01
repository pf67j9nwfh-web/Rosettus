/* ===== QuickDraw print jobs (application/pictwps) =====
   A Carbon app that prints with QuickDraw (AppleWorks 6) has the 10.6 printing system spool
   "pictwps" data: QuickDraw PICTs behind an 'rbDG' 00 01 header. 10.6's cupsd recognised it
   (apple.types) and made PostScript of it with its pictwpstops filter - a PowerPC-only filter that
   Intel Macs ran under Rosetta. Mojave's CUPS knows neither, so the job was sent to the printer as
   raw application/octet-stream (nothing sensible prints). CUPS can't be given the filter either:
   mime.convs only runs filters from its SIP-protected filter directory.
   So the conversion happens here, before the job leaves the app: the guest's cupsPrintFiles2
   (cupsPrintFile2 calls it) is hooked; each file that starts with the pictwps header is turned into
   PostScript by Snow Leopard's own pictwpstops (ROOT/usr/libexec/cups/filter/pictwpstops, from the
   import manifest), run under Rosettus with the queue's PPD from Mojave's CUPS, and the
   PostScript is printed instead. The temporary files go when the call returns.
   OAH_NO_PICTWPS=1 disables. */
static char g_prt_files[16][1100];          /* converted files handed to the guest call */
static unsigned int g_prt_ptrs[16];
static int g_prt_n;
static char g_prt_ppd[1100];
static int oah_want_pictwps(void) { return !getenv("OAH_NO_PICTWPS"); }
static int oah_prt_is_pictwps(const char *f) {
    unsigned char h[6];
    int fd = open(f, O_RDONLY), n;
    if (fd < 0) return 0;
    n = (int)read(fd, h, sizeof h);
    close(fd);
    return n == 6 && !memcmp(h, "rbDG\0\1", 6);
}
static const char *oah_prt_ppd(const char *printer) {
    static void *lib;
    static const char *(*getppd)(const char *);
    if (!lib) lib = dlopen("/usr/lib/libcups.2.dylib", RTLD_LAZY);
    if (lib && !getppd) *(void **)&getppd = dlsym(lib, "cupsGetPPD");
    return getppd && printer ? getppd(printer) : NULL;
}
/* pictwps file -> PostScript file (out); 0 on success */
static int oah_prt_convert(const char *in, const char *out, const char *ppd, const char *title) {
    char cmd[4096], filt[1100], t[256];
    int st = -1, i;
    pid_t p;
    snprintf(filt, sizeof filt, "%s/usr/libexec/cups/filter/pictwpstops", g_rosetta_root);
    if (access(filt, X_OK)) { fprintf(stderr, "[print] %s missing: QuickDraw print job left as is\n", filt); return -1; }
    for (i = 0; title && title[i] && i < (int)sizeof t - 1; i++) t[i] = title[i] == '\'' ? ' ' : title[i];
    t[title ? i : 0] = 0;
    snprintf(cmd, sizeof cmd, "cd '%s' && unset OAH_GUEST_EXE CFProcessPath OAH_RLAUNCH OAH_RLAUNCH_CHILD OAH_TRANSLATE_LINK "
             "OAH_OPEN_DOCS OAH_PRINT_DOCS OAH_RLAUNCH_FWDPID; PPD='%s' exec ./rosettus '%s' 1 \"$USER\" '%s' 1 '' '%s' > '%s' 2>/dev/null",
             g_rosetta_root, ppd ? ppd : "", filt, t, in, out);
    p = fork();
    if (p == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)0); _exit(127); }
    if (p < 0) return -1;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}
/* cupsPrintFiles2(http, name, num_files, const char **files, title, num_options, options) */
static void oah_fix_cupsprintfiles(unsigned int *R) {
    const char *name = (const char *)R[4], *title = (const char *)R[7];
    unsigned int nf = R[5], files = R[6], i, conv = 0;
    const char *ppd = NULL;
    g_prt_n = 0; g_prt_ppd[0] = 0;
    if (!files || !nf || nf > 16) return;
    for (i = 0; i < nf; i++) {
        const char *f = (const char *)oah_get32(files + 4 * i);
        char out[1100];
        g_prt_ptrs[i] = oah_get32(files + 4 * i);
        if (!f || !oah_prt_is_pictwps(f)) continue;
        if (!ppd && !(ppd = oah_prt_ppd(name))) fprintf(stderr, "[print] no PPD for \"%s\"; converting without one\n", name ? name : "");
        if (ppd && !g_prt_ppd[0]) snprintf(g_prt_ppd, sizeof g_prt_ppd, "%s", ppd);
        snprintf(out, sizeof out, "%s/rosettus-pictwps-%d-%u.ps", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid(), i);
        if (oah_prt_convert(f, out, g_prt_ppd[0] ? g_prt_ppd : NULL, title)) {
            fprintf(stderr, "[print] pictwpstops failed for %s: sent as is\n", f);
            unlink(out);
            continue;
        }
        snprintf(g_prt_files[i], sizeof g_prt_files[i], "%s", out);
        g_prt_ptrs[i] = OSSwapHostToBigInt32((unsigned int)g_prt_files[i]);
        conv++;
        fprintf(stderr, "[print] QuickDraw job (pictwps) for \"%s\" converted to PostScript by Snow Leopard's pictwpstops\n", name ? name : "");
    }
    if (conv) {   /* the guest reads the array through big-endian words */
        g_prt_n = (int)nf;
        R[6] = (unsigned int)g_prt_ptrs;
        for (i = 0; i < nf; i++) if (!g_prt_files[i][0]) g_prt_ptrs[i] = OSSwapHostToBigInt32(oah_get32(files + 4 * i));
    }
}
static void oah_fixret_cupsprintfiles(unsigned int *R, unsigned int a0) {
    int i;
    (void)R; (void)a0;
    for (i = 0; i < g_prt_n && i < 16; i++) if (g_prt_files[i][0]) { unlink(g_prt_files[i]); g_prt_files[i][0] = 0; }
    if (g_prt_ppd[0]) { unlink(g_prt_ppd); g_prt_ppd[0] = 0; }
    g_prt_n = 0;
}
