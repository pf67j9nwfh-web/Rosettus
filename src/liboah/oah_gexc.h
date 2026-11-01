/* ===== Guest Mach exception ports =====
   A PowerPC program that sets its own exception ports (thread_/task_set_exception_ports with
   PPC_THREAD_STATE) got KERN_INVALID_ARGUMENT from Mojave (10.6's kernel accepted the PowerPC
   flavors for translated tasks). Apple's PowerPC JVM (Java 5) treats that as fatal. The guest's
   registrations are kept here instead (the translator keeps its own task port for its faults) and
   reported back by the get/swap calls. Faults are delivered to them by oah_gexc_fault (when it can
   recover the guest state). OAH_NO_GEXC=1 passes the calls to the kernel as before. */
#define GEXC_TYPES 14
typedef struct { unsigned int port, behavior, flavor; } oah_gexc_t;
static oah_gexc_t g_gexc_task[GEXC_TYPES];
static struct { unsigned int thread; oah_gexc_t e[GEXC_TYPES]; } g_gexc_thr[256];
static pthread_mutex_t g_gexc_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_gexc_log = -1;
static int oah_want_gexc(void) { return !getenv("OAH_NO_GEXC"); }
static oah_gexc_t *oah_gexc_slot(unsigned int target, int is_task, int create) {
    int i, fr = -1;
    if (is_task) return g_gexc_task;
    for (i = 0; i < 256; i++) {
        if (g_gexc_thr[i].thread == target) return g_gexc_thr[i].e;
        if (!g_gexc_thr[i].thread && fr < 0) fr = i;
    }
    if (!create || fr < 0) return 0;
    memset(&g_gexc_thr[fr], 0, sizeof g_gexc_thr[fr]);
    g_gexc_thr[fr].thread = target;
    return g_gexc_thr[fr].e;
}
static void oah_gexc_logf(const char *what, unsigned int t, unsigned int mask, unsigned int port, unsigned int beh, unsigned int fl) {
    if (g_gexc_log < 0) g_gexc_log = getenv("OAH_GEXC_LOG") ? 1 : 0;
    if (g_gexc_log) fprintf(stderr, "[gexc] %s target %x mask %05x port %x behavior %x flavor %u\n", what, t, mask, port, beh, fl);
}
/* fill the guest's (big-endian) out arrays from the table, merging equal entries as the kernel does */
static unsigned int oah_gexc_report(oah_gexc_t *e, unsigned int mask, unsigned int *masks, unsigned int cap,
                                    unsigned int *ports, unsigned int *behs, unsigned int *fls) {
    unsigned int n = 0, x, j;
    for (x = 1; x < GEXC_TYPES; x++) {
        if (!(mask & (1u << x))) continue;
        for (j = 0; j < n; j++)
            if (OSSwapBigToHostInt32(ports[j]) == e[x].port && OSSwapBigToHostInt32(behs[j]) == e[x].behavior &&
                OSSwapBigToHostInt32(fls[j]) == e[x].flavor) {
                masks[j] = OSSwapHostToBigInt32(OSSwapBigToHostInt32(masks[j]) | (1u << x));
                break;
            }
        if (j < n) continue;
        if (n >= cap) break;
        masks[n] = OSSwapHostToBigInt32(1u << x); ports[n] = OSSwapHostToBigInt32(e[x].port);
        behs[n] = OSSwapHostToBigInt32(e[x].behavior); fls[n] = OSSwapHostToBigInt32(e[x].flavor);
        n++;
    }
    return n;
}
static void oah_gexc_set(unsigned int *R, int is_task) {
    oah_gexc_t *e; unsigned int x;
    pthread_mutex_lock(&g_gexc_mu);
    e = oah_gexc_slot(R[3], is_task, 1);
    if (e) for (x = 1; x < GEXC_TYPES; x++) if (R[4] & (1u << x)) { e[x].port = R[5]; e[x].behavior = R[6]; e[x].flavor = R[7]; }
    pthread_mutex_unlock(&g_gexc_mu);
    oah_gexc_logf(is_task ? "task_set_exception_ports" : "thread_set_exception_ports", R[3], R[4], R[5], R[6], R[7]);
    g_gt_skip = 1; g_gt_skip_ret = e ? 0 : 6 /* KERN_RESOURCE_SHORTAGE */;
}
static void oah_fix_thr_setexc(unsigned int *R) { oah_gexc_set(R, 0); }
static void oah_fix_task_setexc(unsigned int *R) { oah_gexc_set(R, 1); }
/* get_exception_ports(target r3, mask r4, masks r5, masksCnt r6, handlers r7, behaviors r8, flavors r9) */
static void oah_gexc_get(unsigned int *R, int is_task) {
    oah_gexc_t *e, none[GEXC_TYPES]; unsigned int n, cap;
    if (!R[5] || !R[6] || !R[7] || !R[8] || !R[9]) { g_gt_skip = 1; g_gt_skip_ret = 4; return; }
    memset(none, 0, sizeof none);
    pthread_mutex_lock(&g_gexc_mu);
    e = oah_gexc_slot(R[3], is_task, 0);
    cap = OSSwapBigToHostInt32(*(unsigned int *)R[6]);
    n = oah_gexc_report(e ? e : none, R[4], (unsigned int *)R[5], cap, (unsigned int *)R[7], (unsigned int *)R[8], (unsigned int *)R[9]);
    pthread_mutex_unlock(&g_gexc_mu);
    *(unsigned int *)R[6] = OSSwapHostToBigInt32(n);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static void oah_fix_thr_getexc(unsigned int *R) { oah_gexc_get(R, 0); }
static void oah_fix_task_getexc(unsigned int *R) { oah_gexc_get(R, 1); }
/* swap_exception_ports(target r3, mask r4, port r5, behavior r6, flavor r7, masks r8, masksCnt r9,
   old_handlers r10, old_behaviors sp+56, old_flavors sp+60) */
static void oah_gexc_swap(unsigned int *R, int is_task) {
    oah_gexc_t *e, none[GEXC_TYPES]; unsigned int n = 0, cap, x, *sp = (unsigned int *)(R[1] + 56);
    unsigned int ob = OSSwapBigToHostInt32(sp[0]), of = OSSwapBigToHostInt32(sp[1]);
    memset(none, 0, sizeof none);
    pthread_mutex_lock(&g_gexc_mu);
    e = oah_gexc_slot(R[3], is_task, 1);
    if (R[8] && R[9] && R[10] && ob && of) {
        cap = OSSwapBigToHostInt32(*(unsigned int *)R[9]);
        n = oah_gexc_report(e ? e : none, R[4], (unsigned int *)R[8], cap, (unsigned int *)R[10], (unsigned int *)ob, (unsigned int *)of);
        *(unsigned int *)R[9] = OSSwapHostToBigInt32(n);
    }
    if (e) for (x = 1; x < GEXC_TYPES; x++) if (R[4] & (1u << x)) { e[x].port = R[5]; e[x].behavior = R[6]; e[x].flavor = R[7]; }
    pthread_mutex_unlock(&g_gexc_mu);
    oah_gexc_logf(is_task ? "task_swap_exception_ports" : "thread_swap_exception_ports", R[3], R[4], R[5], R[6], R[7]);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static void oah_fix_thr_swapexc(unsigned int *R) { oah_gexc_swap(R, 0); }
static void oah_fix_task_swapexc(unsigned int *R) { oah_gexc_swap(R, 1); }
/* the guest port registered for exception type exc on this thread (thread entry first, then task) */
static int oah_gexc_lookup(unsigned int thread, int exc, oah_gexc_t *out) {
    oah_gexc_t *e; int ok = 0;
    if (exc <= 0 || exc >= GEXC_TYPES) return 0;
    pthread_mutex_lock(&g_gexc_mu);
    if ((e = oah_gexc_slot(thread, 0, 0)) && e[exc].port) { *out = e[exc]; ok = 1; }
    else if (g_gexc_task[exc].port) { *out = g_gexc_task[exc]; ok = 1; }
    pthread_mutex_unlock(&g_gexc_mu);
    return ok;
}
