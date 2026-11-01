/* oah_exc.h - stuck Mach exception detector / rescuer.
   The Big City freeze: spindump showed the main thread and a guest pthread both parked in
   the kernel in exception_raise_state_identity, waiting for a reply from an exception port
   that nobody is servicing.  The hang watchdog thread calls oah_exc_poll() once a second.
   It looks at every task- and thread-level exception port whose receive right lives in
   this process; if one keeps holding queued messages for 3 polls, it takes them, logs the
   faulting thread (native + guest state) and replies KERN_FAILURE so the kernel moves on
   to the BSD signal (our SIGSEGV handler then prints a normal crash report).
   OAH_EXC_RESCUE=0 : only log, never reply.  */
#include <mach/mach.h>
#include <mach/exception_types.h>


/* ---- thread_suspend / thread_resume tracking ----
   The translator stops other threads with thread_suspend.  Spindump plus the heartbeat show
   the whole task going quiet at the freeze, so log who suspends whom, and never suspend the
   watchdog thread (so it can still report and rescue).  OAH_NO_SUSPGUARD=1 disables the
   exemption (logging stays). */
static volatile thread_t g_watch_thread;
static volatile int g_susp_out, g_susp_calls, g_susp_skipped;
static volatile unsigned int g_susp_last_ra, g_susp_last_target, g_susp_last_by;
#define OAH_SUSP_RING 32
static struct { unsigned int t, by, target, ra, op; } g_susp_ring[OAH_SUSP_RING];
static volatile unsigned int g_susp_ri;

static void oah_susp_note(int op, thread_t target, unsigned int ra) {
    unsigned int k = __sync_fetch_and_add(&g_susp_ri, 1) % OAH_SUSP_RING;
    thread_t me = mach_thread_self();
    g_susp_ring[k].t = (unsigned int)time(0); g_susp_ring[k].by = me;
    g_susp_ring[k].target = target; g_susp_ring[k].ra = ra; g_susp_ring[k].op = op;
    mach_port_deallocate(mach_task_self(), me);
}

kern_return_t my_thread_suspend(thread_t t) {
    unsigned int ra = (unsigned int)(uintptr_t)__builtin_return_address(0);
    __sync_fetch_and_add(&g_susp_calls, 1);
    g_susp_last_ra = ra; g_susp_last_target = t;
    oah_susp_note(1, t, ra);
    if (g_watch_thread && t == g_watch_thread && !getenv("OAH_NO_SUSPGUARD")) {
        __sync_fetch_and_add(&g_susp_skipped, 1);
        return KERN_SUCCESS;
    }
    __sync_fetch_and_add(&g_susp_out, 1);
    return thread_suspend(t);
}
kern_return_t my_thread_resume(thread_t t) {
    unsigned int ra = (unsigned int)(uintptr_t)__builtin_return_address(0);
    oah_susp_note(2, t, ra);
    if (g_watch_thread && t == g_watch_thread && !getenv("OAH_NO_SUSPGUARD")) {
        if (g_susp_skipped > 0) __sync_fetch_and_sub(&g_susp_skipped, 1);
        return KERN_SUCCESS;
    }
    __sync_fetch_and_sub(&g_susp_out, 1);
    return thread_resume(t);
}

static void oah_susp_dump(void) {
    unsigned int i, n = g_susp_ri, from = n > OAH_SUSP_RING ? n - OAH_SUSP_RING : 0;
    fprintf(stderr, "[susp] calls=%d outstanding=%d watchdog-exempt=%d; last %u events:\n",
            g_susp_calls, g_susp_out, g_susp_skipped, n - from);
    for (i = from; i < n; i++) {
        unsigned int k = i % OAH_SUSP_RING;
        fprintf(stderr, "[susp]   t=%u %s target=%x by=%x ra=%08x\n", g_susp_ring[k].t,
                g_susp_ring[k].op == 1 ? "SUSPEND" : "resume ", g_susp_ring[k].target, g_susp_ring[k].by, g_susp_ring[k].ra);
    }
}


/* ---- freeze black box ----
   The frozen task can't run any of its own threads (sample shows the main thread inside the
   vm_deallocate trap and everyone else parked), so record recent memory-unmap calls into a
   MAP_SHARED file page. The page cache keeps it readable from outside even when the task is
   wedged (/tmp/oah_blackbox.bin). */
struct oah_bb_ent { unsigned int seq, thread, op, addr, size, ra, guest_lr, done; };
struct oah_bb { unsigned int magic, seq, main_bridge_id, main_bridge_t; char main_bridge[32];
                struct oah_bb_ent e[120]; };
static struct oah_bb *g_bb;
static void oah_bb_init(void) {
    int fd;
    if (getenv("OAH_NO_BLACKBOX")) return;
    fd = open("/tmp/oah_blackbox.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (ftruncate(fd, 8192) == 0) {
        void *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) { memset(p, 0, 8192); mlock(p, 8192); g_bb = (struct oah_bb *)p; g_bb->magic = 0x0ab1ac6b; }
    }
    close(fd);
}
static struct oah_bb_ent *oah_bb_begin(unsigned int op, unsigned int addr, unsigned int size, unsigned int ra, unsigned int ebp) {
    struct oah_bb_ent *e; unsigned int s, lr = 0;
    if (!g_bb) return 0;
    s = __sync_add_and_fetch(&g_bb->seq, 1);
    e = &g_bb->e[s % 120];
    e->done = 0; e->seq = s; e->op = op; e->addr = addr; e->size = size; e->ra = ra;
    if (ebp > 0x1000 && probe_read(ebp + 0x90, &lr)) e->guest_lr = lr; else e->guest_lr = 0;
    { thread_t me = mach_thread_self(); e->thread = me; mach_port_deallocate(mach_task_self(), me); }
    g_bb->main_bridge_id = g_main_bridge_id; g_bb->main_bridge_t = (unsigned int)g_main_bridge_t;
    if (g_main_bridge_id) strncpy(g_bb->main_bridge, oah_bridge_name(g_main_bridge_id), 31);
    return e;
}
/* A 32-bit range that wraps past 4 GB (or is absurdly large) is never a real request.  On
   Mojave the kernel wedges the whole task trying to remove it (the Big City freeze:
   vm_deallocate(0x7a323000, 0x876de000) from the translator's guest-trap path).  Refuse it. */
static int oah_bad_range(unsigned int a, unsigned int sz) {
    return (unsigned long long)a + sz > 0x100000000ULL;
}
static void oah_bad_range_log(const char *fn, unsigned int a, unsigned int sz, unsigned int ra, unsigned int cebp) {
    unsigned int R3 = 0, R4 = 0, R5 = 0, lr = 0, r1 = 0;
    static int n;
    if (n++ > 20) return;
    fprintf(stderr, "[unmap-guard] REFUSED %s(%08x, %08x) end=%llx caller=%08x ebp=%08x\n", fn, a, sz,
            (unsigned long long)a + sz, ra, cebp);
    if (cebp > 0x1000 && probe_read(cebp + 0x90, &lr) && probe_read(cebp + 4, &r1)) {
        probe_read(cebp + 12, &R3); probe_read(cebp + 16, &R4); probe_read(cebp + 20, &R5);
        fprintf(stderr, "[unmap-guard]   guest r1=%08x r3=%08x r4=%08x r5=%08x lr=%08x\n", r1, R3, R4, R5, lr);
        if (oah_hang_isR(cebp)) { char b[1024]; oah_hang_dump_R(cebp, b, sizeof b); fputs(b, stderr); }
    }
}
kern_return_t my_vm_deallocate(vm_map_t t, vm_address_t a, vm_size_t sz) {
    kern_return_t r; struct oah_bb_ent *e;
    unsigned int ra = (unsigned int)(uintptr_t)__builtin_return_address(0);
    unsigned int cebp = *(unsigned int *)__builtin_frame_address(0);
    e = oah_bb_begin(1, a, sz, ra, 0);
    if (t == mach_task_self() && oah_bad_range(a, sz) && !getenv("OAH_NO_UNMAPGUARD")) {
        oah_bad_range_log("vm_deallocate", a, sz, ra, cebp);
        if (e) e->done = 1 | (KERN_INVALID_ADDRESS << 8);
        return KERN_INVALID_ADDRESS;
    }
    r = vm_deallocate(t, a, sz);
    if (e) e->done = 1 | (r << 8);
    return r;
}
int my_munmap(void *a, size_t sz) {
    int r; struct oah_bb_ent *e;
    e = oah_bb_begin(2, (unsigned int)(uintptr_t)a, sz, (unsigned int)(uintptr_t)__builtin_return_address(0), 0);
    if (oah_bad_range((unsigned int)(uintptr_t)a, sz) && !getenv("OAH_NO_UNMAPGUARD")) {
        oah_bad_range_log("munmap", (unsigned int)(uintptr_t)a, sz, (unsigned int)(uintptr_t)__builtin_return_address(0),
                          *(unsigned int *)__builtin_frame_address(0));
        if (e) e->done = 1 | (0xfe << 8);
        errno = EINVAL; return -1;
    }
    r = munmap(a, sz);
    if (e) e->done = 1 | ((r ? 0xff : 0) << 8);
    return r;
}

#define OAH_EXC_MAXP 64
static mach_port_t g_exc_port[OAH_EXC_MAXP];
static int g_exc_stuck[OAH_EXC_MAXP];
static int g_exc_np;

static void oah_exc_add(mach_port_t p) {
    int i;
    if (!MACH_PORT_VALID(p)) return;
    for (i = 0; i < g_exc_np; i++) if (g_exc_port[i] == p) return;
    if (g_exc_np < OAH_EXC_MAXP) { g_exc_stuck[g_exc_np] = 0; g_exc_port[g_exc_np++] = p; }
}

static void oah_exc_collect(int log) {
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t beh[EXC_TYPES_COUNT];
    thread_state_flavor_t fl[EXC_TYPES_COUNT];
    mach_msg_type_number_t n = EXC_TYPES_COUNT, i, nt = 0, t;
    thread_act_array_t th;
    if (task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, masks, &n, ports, beh, fl) == KERN_SUCCESS) {
        for (i = 0; i < n; i++) {
            mach_port_type_t ty = 0;
            if (MACH_PORT_VALID(ports[i])) mach_port_type(mach_task_self(), ports[i], &ty);
            if (log) fprintf(stderr, "[exc] task mask=%05x port=%x beh=%x flavor=%d %s\n", masks[i], ports[i],
                             beh[i], fl[i], (ty & MACH_PORT_TYPE_RECEIVE) ? "RECEIVE-IN-PROCESS" : "external/none");
            if (ty & MACH_PORT_TYPE_RECEIVE) oah_exc_add(ports[i]);
        }
    }
    if (task_threads(mach_task_self(), &th, &nt) != KERN_SUCCESS) return;
    for (t = 0; t < nt; t++) {
        n = EXC_TYPES_COUNT;
        if (thread_get_exception_ports(th[t], EXC_MASK_ALL, masks, &n, ports, beh, fl) == KERN_SUCCESS) {
            for (i = 0; i < n; i++) {
                mach_port_type_t ty = 0;
                if (!MACH_PORT_VALID(ports[i])) continue;
                mach_port_type(mach_task_self(), ports[i], &ty);
                if (log) fprintf(stderr, "[exc] thread %x mask=%05x port=%x beh=%x %s\n", th[t], masks[i], ports[i],
                                 beh[i], (ty & MACH_PORT_TYPE_RECEIVE) ? "RECEIVE-IN-PROCESS" : "external");
                if (ty & MACH_PORT_TYPE_RECEIVE) oah_exc_add(ports[i]);
            }
        }
        mach_port_deallocate(mach_task_self(), th[t]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, nt * sizeof(thread_t));
}

static void oah_exc_handle_one(mach_port_t p, int rescue) {
    union { mach_msg_header_t h; char b[4096]; } m;
    struct { mach_msg_header_t Head; NDR_record_t NDR; kern_return_t RetCode; } rep;
    mach_msg_return_t r;
    memset(&m, 0, sizeof m);
    r = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, p, 0, MACH_PORT_NULL);
    if (r != MACH_MSG_SUCCESS) { fprintf(stderr, "[exc] receive on %x failed: %x\n", p, r); return; }
    fprintf(stderr, "[exc] STUCK exception message on port %x id=%d size=%u\n", p, m.h.msgh_id, m.h.msgh_size);
    if ((m.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) && m.h.msgh_id >= 2401 && m.h.msgh_id <= 2407) {
        mach_msg_body_t *body = (mach_msg_body_t *)(&m.h + 1);
        mach_msg_port_descriptor_t *d = (mach_msg_port_descriptor_t *)(body + 1);
        mach_port_t thr = d[0].name, tsk = d[1].name;
        unsigned int *w = (unsigned int *)(d + 2);   /* NDR(8 bytes), exception, codeCnt, codes */
        int exc = (int)w[2], ncode = (int)w[3], is64 = m.h.msgh_id >= 2405;
        unsigned int c0 = w[4], c1 = is64 ? w[6] : w[5];
        x86_thread_state32_t ts; mach_msg_type_number_t c = x86_THREAD_STATE32_COUNT;
        fprintf(stderr, "[exc]   thread=%x exception=%d codes=%d [%x, %x]%s\n", thr, exc, ncode, c0, c1,
                exc == EXC_BAD_ACCESS ? " (EXC_BAD_ACCESS, code[1] = fault address)" : "");
        if (thread_get_state(thr, x86_THREAD_STATE32, (thread_state_t)&ts, &c) == KERN_SUCCESS) {
            char buf[1024];
            fprintf(stderr, "[exc]   eip=%08x esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
                    ts.__eip, ts.__esp, ts.__ebp, ts.__eax, ts.__ebx, ts.__ecx, ts.__edx, ts.__esi, ts.__edi);
            if (oah_hang_isR(ts.__ebp)) { oah_hang_dump_R(ts.__ebp, buf, sizeof buf); fputs(buf, stderr); }
            else fprintf(stderr, "[exc]   (ebp is not a guest register file)\n");
        }
        if (MACH_PORT_VALID(thr)) mach_port_deallocate(mach_task_self(), thr);
        if (MACH_PORT_VALID(tsk)) mach_port_deallocate(mach_task_self(), tsk);
    }
    if (!rescue) { fprintf(stderr, "[exc]   OAH_EXC_RESCUE=0: message dropped without reply\n"); return; }
    memset(&rep, 0, sizeof rep);
    rep.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(m.h.msgh_bits), 0);
    rep.Head.msgh_remote_port = m.h.msgh_remote_port;
    rep.Head.msgh_size = sizeof rep;
    rep.Head.msgh_id = m.h.msgh_id + 100;
    rep.NDR = NDR_record;
    rep.RetCode = KERN_FAILURE;
    r = mach_msg(&rep.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof rep, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
    fprintf(stderr, "[exc]   replied KERN_FAILURE (send=%x); kernel will now raise the signal\n", r);
}

/* heartbeat: one line per second in /tmp/oah_hb.<pid>.txt (overwritten). The file is removed at
   exit, and files of processes that are gone (crashed, killed) are swept when the next process
   starts: every translated process left one behind, and a test Mac had 2,300 of them in /tmp,
   which TextWrangler then spent seconds enumerating at launch. */
static char g_hb_path[64];
static void oah_hb_unlink(void) { if (g_hb_path[0]) unlink(g_hb_path); }
static void oah_hb_sweep(void) {
    DIR *d = opendir("/tmp");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        int pid; char tail[8], p[96]; struct stat st;
        if (strncmp(e->d_name, "oah_hb.", 7) || sscanf(e->d_name + 7, "%d%7s", &pid, tail) != 2 || strcmp(tail, ".txt")) continue;
        if (pid <= 0 || pid == getpid() || kill(pid, 0) == 0 || errno != ESRCH) continue;
        snprintf(p, sizeof p, "/tmp/%s", e->d_name);
        if (lstat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid()) unlink(p);
    }
    closedir(d);
}
static void oah_exc_heartbeat(void) {
    static int fd = -2; char b[512]; int n, i, o;
    struct task_basic_info ti; mach_msg_type_number_t c = TASK_BASIC_INFO_COUNT;
    if (fd == -2) {
        snprintf(g_hb_path, sizeof g_hb_path, "/tmp/oah_hb.%d.txt", getpid());
        fd = open(g_hb_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) atexit(oah_hb_unlink); else g_hb_path[0] = 0;
        oah_hb_sweep();
    }
    if (fd < 0) return;
    memset(&ti, 0, sizeof ti);
    task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&ti, &c);
    o = snprintf(b, sizeof b, "t=%ld suspend=%d tsusp=%d/%d ra=%08x mainbridge=%s age=%lds", (long)time(0), ti.suspend_count,
                 g_susp_out, g_susp_calls, g_susp_last_ra,
                 g_main_bridge_id ? oah_bridge_name(g_main_bridge_id) : "-", (long)(time(0) - g_main_bridge_t));
    if (g_main_R) {   /* where the main thread is in guest code (r1 back chain from its register file) */
        unsigned int *R = (unsigned int *)g_main_R, sp = R[1], k, back, lr;
        o += snprintf(b + o, sizeof b - o, " main lr=%08x r1=%08x bt:", R[0x90 / 4], sp);
        for (k = 0; k < 6 && sp > 0x1000 && !(sp & 3) && o < (int)sizeof b - 16; k++) {
            if (!probe_read(sp, &back)) break;
            back = OSSwapBigToHostInt32(back);
            if (!back || back <= sp || !probe_read(back + 8, &lr)) break;
            {   unsigned int base = 0, a = OSSwapBigToHostInt32(lr);
                const char *nm = oah_guest_image_name_fast(a, &base), *sl = strrchr(nm, '/');
                o += snprintf(b + o, sizeof b - o, " %08x(%.20s+%x)", a, sl ? sl + 1 : nm, a - base); }
            sp = back;
        }
        {   unsigned int base = 0, a = R[0x90 / 4];
            const char *nm = oah_guest_image_name_fast(a, &base), *sl = strrchr(nm, '/');
            o += snprintf(b + o, sizeof b - o, " lr=(%.20s+%x)", sl ? sl + 1 : nm, a - base); }
    }
    for (i = 0; i < g_exc_np && o < (int)sizeof b - 32; i++) {
        mach_port_status_t st; mach_msg_type_number_t cn = MACH_PORT_RECEIVE_STATUS_COUNT;
        if (mach_port_get_attributes(mach_task_self(), g_exc_port[i], MACH_PORT_RECEIVE_STATUS,
                                     (mach_port_info_t)&st, &cn) == KERN_SUCCESS)
            o += snprintf(b + o, sizeof b - o, " port%x:msgs=%u", g_exc_port[i], st.mps_msgcount);
    }
    {   /* busiest bridges in the last second */
        static unsigned int prev[4096];
        unsigned int top[3] = { 0, 0, 0 }, topn[3] = { 0, 0, 0 }, k2, d, j, tot = 0;
        for (k2 = 0; k2 < 4096; k2++) {
            d = g_bcount[k2] - prev[k2]; prev[k2] = g_bcount[k2]; tot += d;
            for (j = 0; j < 3; j++) if (d > topn[j]) { memmove(top + j + 1, top + j, (2 - j) * 4); memmove(topn + j + 1, topn + j, (2 - j) * 4); top[j] = k2; topn[j] = d; break; }
        }
        if (o < (int)sizeof b - 120) {
            o += snprintf(b + o, sizeof b - o, " bridges/s=%u", tot);
            for (j = 0; j < 3 && topn[j]; j++) o += snprintf(b + o, sizeof b - o, " %.24s:%u", oah_bridge_name(top[j]), topn[j]);
        }
    }
    b[o++] = '\n';
    { static const char sp[512] = { [0 ... 510] = 32 }; n = pwrite(fd, sp, 511, 0); }
    n = pwrite(fd, b, o, 0); (void)n;
}

/* called once a second from the watchdog thread */
static void oah_exc_poll(void) {
    static int ticks, logged;
    int i, rescue;
    const char *e = getenv("OAH_EXC_RESCUE");
    rescue = !(e && *e == '0');
    oah_exc_heartbeat();
    if (!logged || (ticks++ % 10) == 0) {          /* ports can appear late; rescan every 10 s */
        oah_exc_collect(!logged);
        if (!logged) fprintf(stderr, "[exc] watching %d in-process exception port(s)\n", g_exc_np);
        logged = 1;
    }
    {
        static int quiet, dumped; static int lastcalls;
        if (g_susp_out > 0 && g_susp_calls == lastcalls) quiet++; else quiet = 0;
        lastcalls = g_susp_calls;
        if (quiet >= 3 && !dumped) { dumped = 1; fprintf(stderr, "[susp] threads suspended for %d s with no further suspend calls\n", quiet);
            oah_susp_dump(); oah_hang_dump_all("world stopped"); }
        if (quiet == 0) dumped = 0;
    }
    for (i = 0; i < g_exc_np; i++) {
        mach_port_status_t st; mach_msg_type_number_t cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
        if (mach_port_get_attributes(mach_task_self(), g_exc_port[i], MACH_PORT_RECEIVE_STATUS,
                                     (mach_port_info_t)&st, &cnt) != KERN_SUCCESS) continue;
        if (st.mps_msgcount == 0) { g_exc_stuck[i] = 0; continue; }
        if (++g_exc_stuck[i] < 3) continue;
        fprintf(stderr, "[exc] port %x has held %u message(s) for %d s with nobody receiving\n",
                g_exc_port[i], st.mps_msgcount, g_exc_stuck[i]);
        while (st.mps_msgcount-- > 0) oah_exc_handle_one(g_exc_port[i], rescue);
        g_exc_stuck[i] = 0;
    }
}
