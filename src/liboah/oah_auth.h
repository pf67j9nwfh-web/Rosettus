#include <sys/socket.h>
/* ---- Authorization Services -> native Security.framework ----
   The 10.6 guest Security asks securityd for authorizations over MIG; on Mojave authorization moved to
   authd (XPC), so AuthorizationCreate failed (Adobe CS2 "Adobe Installer": "AuthorizationCreate failed
   (status=1)") and nothing could get admin rights.  The Authorization* calls go to the native framework:
   AuthorizationRefs are native pointers handed through; rights/environment item sets are converted
   (16-byte big-endian AuthorizationItem {name, valueLength, value, flags}); item sets coming back are
   big-endian copies freed by AuthorizationFreeItemSet.  AuthorizationExecuteWithPrivileges runs a
   PowerPC tool through the rosetta launcher (as root, in GTB mode), as Snow Leopard's Rosetta did,
   and turns the communications pipe into a guest FILE (guest fdopen).  OAH_AUTHTRACE=1 logs. */
static void *g_auth_lib;
static int oah_auth_tr(void) { static int t = -1; if (t < 0) t = getenv("OAH_AUTHTRACE") != 0; return t; }
static void *oah_auth_fn(const char *n) {
    if (!g_auth_lib) g_auth_lib = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    return g_auth_lib ? dlsym(g_auth_lib, n) : 0;
}
typedef struct { const char *name; size_t valueLength; void *value; unsigned int flags; } oah_aitem_t;
typedef struct { unsigned int count; oah_aitem_t *items; } oah_aset_t;
/* guest (big-endian) set -> native set in buf (items in itemsbuf, max 32) */
static oah_aset_t *oah_aset_in(unsigned int g, oah_aset_t *set, oah_aitem_t *items) {
    unsigned int n, i, gi;
    if (!g) return 0;
    n = OSSwapBigToHostInt32(((unsigned int *)g)[0]); gi = OSSwapBigToHostInt32(((unsigned int *)g)[1]);
    if (n > 32) n = 32;
    for (i = 0; i < n && gi; i++) {
        unsigned int *w = (unsigned int *)(gi + 16 * i);
        items[i].name = (const char *)OSSwapBigToHostInt32(w[0]);
        items[i].valueLength = OSSwapBigToHostInt32(w[1]);
        items[i].value = (void *)OSSwapBigToHostInt32(w[2]);
        items[i].flags = OSSwapBigToHostInt32(w[3]);
    }
    set->count = gi ? n : 0; set->items = items;
    return set;
}
/* native set -> big-endian copy; remembered so AuthorizationFreeItemSet frees the native one too */
static struct { unsigned int *g; oah_aset_t *n; } g_asets[64];
static unsigned int oah_aset_out(oah_aset_t *s) {
    unsigned int *g, i, k;
    if (!s) return 0;
    g = (unsigned int *)calloc(1, 8 + 16 * (s->count ? s->count : 1));
    g[0] = OSSwapHostToBigInt32(s->count);
    g[1] = OSSwapHostToBigInt32((unsigned int)(g + 2));
    for (i = 0; i < s->count; i++) {
        unsigned int *w = g + 2 + 4 * i;
        w[0] = OSSwapHostToBigInt32((unsigned int)s->items[i].name);
        w[1] = OSSwapHostToBigInt32((unsigned int)s->items[i].valueLength);
        w[2] = OSSwapHostToBigInt32((unsigned int)s->items[i].value);
        w[3] = OSSwapHostToBigInt32(s->items[i].flags);
    }
    for (k = 0; k < 64; k++) if (!g_asets[k].g) { g_asets[k].g = g; g_asets[k].n = s; break; }
    return (unsigned int)g;
}
static int oah_auth_is_ppc(const char *p) {
    unsigned char h[8]; int fd = open(p, O_RDONLY), r = 0;
    if (fd < 0) return 0;
    if (read(fd, h, 8) == 8) {
        unsigned int m = (h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3], ct = (h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
        if (m == 0xfeedface && ct == 18) r = 1;                    /* thin ppc */
        else if (!memcmp(h, "Joy!peff", 8)) r = 1;                 /* CFM */
        else if (m == 0xcafebabe) {                                /* fat: ppc only if no i386/x86_64 slice */
            unsigned int n = ct, i, has_ppc = 0, has_x86 = 0, fa[5];
            for (i = 0; i < n && i < 8; i++) {
                if (read(fd, fa, 20) != 20) break;
                unsigned int c = OSSwapBigToHostInt32(fa[0]);
                if (c == 18) has_ppc = 1; else if (c == 7 || c == 0x01000007) has_x86 = 1;
            }
            r = has_ppc && !has_x86;
        } else if (m != 0xfeedface && m != 0xfeedfacf && m != 0xcefaedfe && m != 0xcffaedfe) {
            /* a CFM fragment behind a header (VISE installers: PEF at 0x30) */
            static char b[65536];
            ssize_t got = pread(fd, b, sizeof b, 0), j;
            for (j = 0; j + 8 <= got; j++) if (b[j] == 'J' && !memcmp(b + j, "Joy!peff", 8)) { r = 1; break; }
        }
    }
    close(fd);
    return r;
}
/* Classic volume names are limited to 27 characters, so the File Manager hands out mangled
   names ("Logic Platinum 5.4.0 Mac #2" for "Logic Platinum 5.4.0 Mac OS X") and VISE builds
   the POSIX path of its relaunch from them. Resolve each missing "prefix#N" component to the
   one entry in its directory that starts with the prefix. */
static const char *oah_auth_unmangle(const char *p, char *out, size_t cap) {
    char buf[1024], *s, *save = 0;
    size_t o = 0;
    if (!p || p[0] != '/' || !access(p, F_OK) || !strchr(p, '#') || strlen(p) >= sizeof buf) return p;
    snprintf(buf, sizeof buf, "%s", p);
    out[0] = 0;
    for (s = strtok_r(buf + 1, "/", &save); s; s = strtok_r(0, "/", &save)) {
        char cand[1024];
        snprintf(cand, sizeof cand, "%s/%s", out, s);
        if (access(cand, F_OK) && strchr(s, '#')) {
            size_t pl = (size_t)(strrchr(s, '#') - s);
            DIR *d = opendir(o ? out : "/");
            struct dirent *e; int hits = 0; char name[256] = "";
            while (d && (e = readdir(d)))
                if (!strncmp(e->d_name, s, pl) && strlen(e->d_name) > 27) { hits++; snprintf(name, sizeof name, "%s", e->d_name); }
            if (d) closedir(d);
            if (hits == 1) snprintf(cand, sizeof cand, "%s/%s", out, name);
        }
        o = (size_t)snprintf(out, cap, "%s", cand);
        if (o >= cap) return p;
    }
    return access(out, F_OK) ? p : out;
}
static int oah_auth_call(const char *n, unsigned int *R) {
    if (strncmp(n, "Authorization", 13)) return 0;
    n += 13;
    if (!strcmp(n, "Create")) {   /* (rights, environment, flags, AuthorizationRef *) */
        oah_aset_t rs, es; oah_aitem_t ri[32], ei[32]; void *ref = 0;
        int (*f)(oah_aset_t *, oah_aset_t *, unsigned int, void **) = oah_auth_fn("AuthorizationCreate");
        int err = f ? f(oah_aset_in(R[3], &rs, ri), oah_aset_in(R[4], &es, ei), R[5], R[6] ? &ref : 0) : -60008;
        if (R[6]) oah_put32(R[6], (unsigned int)ref);
        if (oah_auth_tr()) fprintf(stderr, "[auth] AuthorizationCreate(flags %#x, %u rights) -> %d ref %p\n", R[5], R[3] ? rs.count : 0, err, ref);
        R[3] = (unsigned int)err; return 1;
    }
    if (!strcmp(n, "Free")) {
        int (*f)(void *, unsigned int) = oah_auth_fn("AuthorizationFree");
        R[3] = f ? (unsigned int)f((void *)R[3], R[4]) : 0; return 1;
    }
    if (!strcmp(n, "CopyRights")) {   /* (ref, rights, env, flags, AuthorizationRights **out) */
        oah_aset_t rs, es, *out = 0; oah_aitem_t ri[32], ei[32];
        int (*f)(void *, oah_aset_t *, oah_aset_t *, unsigned int, oah_aset_t **) = oah_auth_fn("AuthorizationCopyRights");
        int err = f ? f((void *)R[3], oah_aset_in(R[4], &rs, ri), oah_aset_in(R[5], &es, ei), R[6], R[7] ? &out : 0) : -60008;
        if (R[7]) oah_put32(R[7], err ? 0 : oah_aset_out(out));
        if (oah_auth_tr()) fprintf(stderr, "[auth] AuthorizationCopyRights(%08x, %u rights%s%s, flags %#x) -> %d\n", R[3],
                                   R[4] ? rs.count : 0, R[4] && rs.count ? " first " : "", R[4] && rs.count && rs.items[0].name ? rs.items[0].name : "", R[6], err);
        R[3] = (unsigned int)err; return 1;
    }
    if (!strcmp(n, "CopyInfo")) {     /* (ref, tag, AuthorizationItemSet **) */
        oah_aset_t *out = 0;
        int (*f)(void *, const char *, oah_aset_t **) = oah_auth_fn("AuthorizationCopyInfo");
        int err = f ? f((void *)R[3], (const char *)R[4], &out) : -60008;
        if (R[5]) oah_put32(R[5], err ? 0 : oah_aset_out(out));
        R[3] = (unsigned int)err; return 1;
    }
    if (!strcmp(n, "FreeItemSet")) {
        int (*f)(oah_aset_t *) = oah_auth_fn("AuthorizationFreeItemSet");
        unsigned int k;
        for (k = 0; k < 64; k++) if (g_asets[k].g && (unsigned int)g_asets[k].g == R[3]) {
            if (f) f(g_asets[k].n);
            free(g_asets[k].g); g_asets[k].g = 0; break;
        }
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "MakeExternalForm")) {   /* (ref, AuthorizationExternalForm *): 32 opaque bytes */
        int (*f)(void *, void *) = oah_auth_fn("AuthorizationMakeExternalForm");
        R[3] = f ? (unsigned int)f((void *)R[3], (void *)R[4]) : (unsigned int)-60008; return 1;
    }
    if (!strcmp(n, "CreateFromExternalForm")) {
        void *ref = 0; int (*f)(void *, void **) = oah_auth_fn("AuthorizationCreateFromExternalForm");
        int err = f ? f((void *)R[3], &ref) : -60008;
        if (R[4]) oah_put32(R[4], (unsigned int)ref);
        R[3] = (unsigned int)err; return 1;
    }
    if (!strcmp(n, "ExecuteWithPrivileges")) {   /* (ref, tool, options, char *const *args, FILE **pipe) */
        int (*f)(void *, const char *, unsigned int, char *const *, FILE **) = oah_auth_fn("AuthorizationExecuteWithPrivileges");
        const char *tool = (const char *)R[4];
        char *argv[64], rosetta[1100], logp[1200];
        unsigned int k = 0, ga = R[6], i;
        FILE *pipe = 0; int err;
        Dl_info di;
        char unm[1024];
        unsigned int first = 0;
        /* "LaunchCFMApp <app>" (VISE relaunching itself): the native Carbon has no LaunchCFMApp;
           run the CFM app itself, and rosettus starts the guest LaunchCFMApp for it. */
        if (tool && strstr(tool, "/Support/LaunchCFMApp") && ga && ((unsigned int *)ga)[0]) {
            tool = (const char *)OSSwapBigToHostInt32(((unsigned int *)ga)[0]);
            first = 1;
        }
        tool = oah_auth_unmangle(tool, unm, sizeof unm);
        if (tool && oah_auth_is_ppc(tool) && dladdr((void *)oah_auth_call, &di) && di.dli_fname) {
            snprintf(rosetta, sizeof rosetta, "%s", di.dli_fname);
            if (strrchr(rosetta, '/')) strcpy(strrchr(rosetta, '/') + 1, "rosettus");
            /* The privileged tool runs with effective uid 0 only, and bash (the rosettus script)
               drops an effective uid that differs from the real one: the PowerPC app came up as
               the user again, asked for the password again and relaunched itself (Adobe CS2's
               installer three times, then AuthorizationCreate failed). Make the real uid 0 too. */
            /* Its messages go to "<tool> (admin).log" next to the app's own log (stdout stays the
               pipe to the caller). */
            {   const char *h = getenv("HOME"), *bn = strrchr(tool, '/');
                snprintf(logp, sizeof logp, "%s/Library/Logs/Rosettus/%s (admin).log", h ? h : "/tmp", bn ? bn + 1 : tool); }
            argv[k++] = "-c";
            argv[k++] = "import os, sys\nos.setuid(0)\nos.environ['OAH_GTB'] = '1'\n"
                        /* the tool is another program: not our bundle identity (CS2's installer
                           relaunched a temp copy of itself, which then saw the disc path again) */
                        "for v in ('OAH_GUEST_EXE', 'CFProcessPath'): os.environ.pop(v, None)\n"
                        "try:\n fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644); os.dup2(fd, 2)\n"
                        "except Exception:\n pass\n"
                        "os.execv(sys.argv[2], sys.argv[2:])";
            argv[k++] = logp; argv[k++] = rosetta; argv[k++] = (char *)tool;
            tool = "/usr/bin/python";
        }
        for (i = first; ga && k < 62; i++) {
            unsigned int a = OSSwapBigToHostInt32(((unsigned int *)ga)[i]);
            if (!a) break;
            argv[k++] = (char *)a;
        }
        argv[k] = 0;
        if (!getuid() && !geteuid() && !getenv("OAH_NO_ROOTAEWP")) {
            /* Already root (run through sudo): authd would start the tool outside this login
               session, where its windows show but never take clicks or focus (VISE relaunches
               itself this way even as root). Start it here instead, with the same pipe. */
            int sv[2] = { -1, -1 }; pid_t pid;
            char *av[66]; unsigned int j;
            av[0] = (char *)tool;
            for (j = 0; j < k; j++) av[j + 1] = argv[j];
            av[k + 1] = 0;
            if (R[7] && socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) sv[0] = sv[1] = -1;
            pid = fork();
            if (pid == 0) {
                if (sv[1] >= 0) { dup2(sv[1], 0); dup2(sv[1], 1); close(sv[0]); close(sv[1]); }
                setsid();
                execv(tool, av);
                _exit(127);
            }
            if (sv[1] >= 0) close(sv[1]);
            err = pid > 0 ? 0 : -60031;
            if (!err && sv[0] >= 0) pipe = fdopen(sv[0], "r+");
            else if (sv[0] >= 0) close(sv[0]);
        } else
        err = f ? f((void *)R[3], tool, R[5], argv, R[7] ? &pipe : 0) : -60008;
        if (oah_auth_tr() || 1) {
            fprintf(stderr, "[auth] AuthorizationExecuteWithPrivileges(%s", tool);
            for (i = 0; i < k; i++) fprintf(stderr, " %s", argv[i]);
            fprintf(stderr, ") -> %d\n", err);
        }
        if (R[7]) {
            unsigned int gfp = 0;
            if (!err && pipe) {
                static unsigned int gfdopen;
                int fd = dup(fileno(pipe));
                fclose(pipe);
                if (!gfdopen) gfdopen = oah_guest_sym("libSystem", "_fdopen");
                if (gfdopen && fd >= 0) { unsigned int a[2]; a[0] = (unsigned int)fd; a[1] = (unsigned int)"r+"; gfp = oah_callppc_n("fdopen", gfdopen, 2, a); }
            }
            oah_put32(R[7], gfp);
        }
        R[3] = (unsigned int)err; return 1;
    }
    return 0;
}
