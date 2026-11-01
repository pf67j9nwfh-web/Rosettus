/* oah_net.h - DNS resolver configuration for the guest.
   The 10.6 libSystem fetches the resolver list from configd with the old
   shared_dns_infoGet MIG call, which Mojave's configd rejects ("(ipc/mig) bad
   request message ID").  With no dns_config the guest libinfo resolves nothing:
   gethostbyname/getaddrinfo fail, Open Transport's OTInetStringToAddress returns
   kOTBadNameErr, and Internet Explorer's stream library then uses a stream it has
   already disposed (crash in a cooperative thread).
   dns_configuration_copy/free are bridged: Mojave's own configuration is copied
   into one block in the 10.6 PowerPC layout (big-endian words, 4-byte pointers),
   which dns_configuration_free releases again.

   Both 10.6 and Mojave use configd's DNS_PTR layout (each pointer in an 8-byte
   union, pack(4)), so the field offsets are the same; the guest's words are
   big-endian with the pointer in the union's first word.
   dns_config_t   0 n_resolver 4 resolver 12 n_scoped 16 scoped
   dns_resolver_t 0 domain 8 n_ns 12 ns 20 port 24 n_search 28 search
                  36 n_sort 40 sort 48 options 56 timeout 60 search_order
                  64 if_index 68 flags 72 reach_flags  */
#define OAH_DNS_MAXBLK 32
static void *g_dns_blk[OAH_DNS_MAXBLK];
static pthread_mutex_t g_dns_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { char *b, *p, *end; } oah_dnsbuf;
static unsigned int oah_dns_alloc(oah_dnsbuf *d, size_t n) {
    char *r = (char *)(((uintptr_t)d->p + 3) & ~(uintptr_t)3);
    if (r + n > d->end) return 0;
    memset(r, 0, n); d->p = r + n;
    return (unsigned int)(uintptr_t)r;
}
static unsigned int oah_dns_str(oah_dnsbuf *d, const char *s) {
    unsigned int r;
    if (!s) return 0;
    r = oah_dns_alloc(d, strlen(s) + 1);
    if (r) strcpy((char *)(uintptr_t)r, s);
    return r;
}
#define OAH_W(p, off) (*(unsigned int *)((char *)(p) + (off)))
static void oah_dns_put(unsigned int g, int off, unsigned int v) { OAH_W(g, off) = OSSwapHostToBigInt32(v); }

static unsigned int oah_dns_resolver(oah_dnsbuf *d, const char *nr) {
    /* same offsets in both layouts; guest words are big-endian, pointers in the
       first word of each 8-byte DNS_PTR union */
    unsigned int g = oah_dns_alloc(d, 96), a;
    int i, n;
    if (!g || !nr) return 0;
    oah_dns_put(g, 0, oah_dns_str(d, *(char **)(nr + 0)));
    n = *(int *)(nr + 8);
    if (n < 0 || n > 64) n = 0;
    a = n ? oah_dns_alloc(d, 4 * n) : 0;
    for (i = 0; i < n && a; i++) {
        struct sockaddr *sa = (*(struct sockaddr ***)(nr + 12))[i];
        unsigned int c = 0;
        if (sa && sa->sa_len >= 2 && (c = oah_dns_alloc(d, sa->sa_len))) memcpy((void *)(uintptr_t)c, sa, sa->sa_len);
        oah_dns_put(a, 4 * i, c);
    }
    oah_dns_put(g, 8, a ? n : 0); oah_dns_put(g, 12, a);
    *(unsigned short *)(uintptr_t)(g + 20) = OSSwapHostToBigInt16(*(unsigned short *)(nr + 20));
    n = *(int *)(nr + 24);
    if (n < 0 || n > 64) n = 0;
    a = n ? oah_dns_alloc(d, 4 * n) : 0;
    for (i = 0; i < n && a; i++) oah_dns_put(a, 4 * i, oah_dns_str(d, (*(char ***)(nr + 28))[i]));
    oah_dns_put(g, 24, a ? n : 0); oah_dns_put(g, 28, a);
    n = *(int *)(nr + 36);
    if (n < 0 || n > 64) n = 0;
    a = n ? oah_dns_alloc(d, 4 * n) : 0;
    for (i = 0; i < n && a; i++) {
        unsigned int c = oah_dns_alloc(d, 8);
        if (c) memcpy((void *)(uintptr_t)c, (*(void ***)(nr + 40))[i], 8);   /* in_addr pair: network order already */
        oah_dns_put(a, 4 * i, c);
    }
    oah_dns_put(g, 36, a ? n : 0); oah_dns_put(g, 40, a);
    oah_dns_put(g, 48, oah_dns_str(d, *(char **)(nr + 48)));
    for (i = 56; i <= 72; i += 4) oah_dns_put(g, i, *(unsigned int *)(nr + i));   /* timeout, search_order, if_index, flags, reach_flags */
    return g;
}
static unsigned int oah_dns_list(oah_dnsbuf *d, int n, char **lst) {
    unsigned int a;
    int i;
    if (n <= 0 || n > 256 || !lst) return 0;
    a = oah_dns_alloc(d, 4 * n);
    for (i = 0; i < n && a; i++) oah_dns_put(a, 4 * i, oah_dns_resolver(d, lst[i]));
    return a;
}
static unsigned int oah_dns_copy(void) {
    static void *(*cp)(void); static void (*fr)(void *);
    char *nc;
    oah_dnsbuf d;
    unsigned int g, a;
    int n, i;
    if (!cp) { cp = (void *(*)(void))dlsym(RTLD_DEFAULT, "dns_configuration_copy");
               fr = (void (*)(void *))dlsym(RTLD_DEFAULT, "dns_configuration_free"); }
    if (!cp || !(nc = (char *)cp())) return 0;
    d.b = d.p = (char *)malloc(65536); d.end = d.b + 65536;
    if (!d.b) { fr(nc); return 0; }
    g = oah_dns_alloc(&d, 64);
    n = *(int *)(nc + 0);
    a = oah_dns_list(&d, n, *(char ***)(nc + 4));
    oah_dns_put(g, 0, a ? n : 0); oah_dns_put(g, 4, a);
    n = *(int *)(nc + 12);
    a = oah_dns_list(&d, n, *(char ***)(nc + 16));
    oah_dns_put(g, 12, a ? n : 0); oah_dns_put(g, 16, a);
    fr(nc);
    pthread_mutex_lock(&g_dns_lock);
    for (i = 0; i < OAH_DNS_MAXBLK && g_dns_blk[i]; i++) ;
    if (i < OAH_DNS_MAXBLK) g_dns_blk[i] = d.b;
    pthread_mutex_unlock(&g_dns_lock);
    /* (more than OAH_DNS_MAXBLK live copies: the extra one is simply never freed) */
    if (getenv("OAH_VERBOSE")) {
        unsigned int r0 = OSSwapBigToHostInt32(OAH_W(g, 4));
        fprintf(stderr, "[dns] dns_configuration_copy -> %#x: %d resolvers, %d scoped, %d bytes",
                g, OSSwapBigToHostInt32(OAH_W(g, 0)), OSSwapBigToHostInt32(OAH_W(g, 12)), (int)(d.p - d.b));
        if (r0 && OAH_W(OSSwapBigToHostInt32(OAH_W(r0, 0)), 8)) {
            unsigned int r = OSSwapBigToHostInt32(OAH_W(r0, 0)), ns = OSSwapBigToHostInt32(OAH_W(r, 12));
            if (ns) { struct sockaddr_in *s = (struct sockaddr_in *)(uintptr_t)OSSwapBigToHostInt32(OAH_W(ns, 0));
                      if (s && s->sin_family == AF_INET) fprintf(stderr, ", first nameserver %s", inet_ntoa(s->sin_addr)); }
        }
        fprintf(stderr, "\n");
    }
    return g;
}
static int oah_net_call(const char *n, unsigned int *R) {
    if (!strcmp(n, "dns_configuration_copy")) { R[3] = oah_dns_copy(); return 1; }
    if (!strcmp(n, "ptrace")) {
        /* The translator terminates the process (SIGILL) on a guest ptrace syscall; iWeb calls
           ptrace(PT_DENY_ATTACH) at launch. Pretend it worked: nothing can attach to a guest anyway. */
        static int nlog;
        if (nlog++ < 3) fprintf(stderr, "[sys] guest ptrace(%d, %d) -> 0\n", (int)R[3], (int)R[4]);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "IORegisterForSystemPower")) {
        /* (refcon, IONotificationPortRef *port, callback, io_object_t *notifier) -> io_connect_t.
           The 10.6 IOKit cannot open Mojave's IOPMrootDomain user client, so it returned 0 and a
           NULL port, and DesktopServices (every open/save panel) passed that port to
           IONotificationPortGetRunLoopSource. Give it a working (silent) notification port:
           no sleep/wake messages are delivered. */
        static unsigned int fcreate;
        unsigned int a[1], np;
        if (!fcreate) fcreate = oah_guest_sym("IOKit.framework", "_IONotificationPortCreate");
        a[0] = 0;
        np = fcreate ? oah_callppc_n("IONotificationPortCreate", fcreate, 1, a) : 0;
        if (R[4]) *(unsigned int *)R[4] = OSSwapHostToBigInt32(np);
        if (R[6]) *(unsigned int *)R[6] = 0;
        R[3] = np ? 0xffffffffu : 0;       /* MACH_PORT_DEAD: non-zero, and closing it touches nothing */
        fprintf(stderr, "[iokit] IORegisterForSystemPower -> notification port %08x (no power events)\n", np);
        return 1;
    }
    if (!strcmp(n, "IODisplayCreateInfoDictionary")) {
        /* (io_service_t framebuffer, IOOptionBits) -> CFDictionaryRef. The 10.6 code walks the
           registry under Mojave's framebuffer, finds no dictionary where it expects one and
           crashed in CFDictionaryGetValue(NULL) (ScreenFlowHelper at launch). The service is a
           native port (CGDisplayIOServicePort is bridged), so ask Mojave's IOKit. */
        static CFDictionaryRef (*fn)(unsigned int, unsigned int);
        CFDictionaryRef d;
        if (!fn) fn = (CFDictionaryRef (*)(unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "IODisplayCreateInfoDictionary");
        d = fn ? fn(R[3], R[4]) : NULL;
        { static int nlog; if (nlog++ < 3) fprintf(stderr, "[iokit] IODisplayCreateInfoDictionary(%#x, %#x) natively -> %s\n",
                                                    R[3], R[4], d ? "dictionary" : "NULL"); }
        R[3] = d ? oah_cf_to_guest(d) : 0;
        if (d) CFRelease(d);
        return 1;
    }
    if (!strcmp(n, "dns_configuration_free")) {
        int i;
        pthread_mutex_lock(&g_dns_lock);
        for (i = 0; i < OAH_DNS_MAXBLK; i++)
            if (g_dns_blk[i] && (uintptr_t)g_dns_blk[i] <= R[3] && R[3] < (uintptr_t)g_dns_blk[i] + 64) { free(g_dns_blk[i]); g_dns_blk[i] = 0; break; }
        pthread_mutex_unlock(&g_dns_lock);
        return 1;
    }
    return 0;
}
