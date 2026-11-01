/* oah_hang.h - hang diagnostics.
   OAH_HANGDUMP=N : a watchdog thread dumps every thread's guest (PPC) state when the
   main thread has not entered a bridge for N seconds (again every 30 s while stuck).
   SIGUSR2 also requests a dump.  Translated code keeps the guest register file R in
   %ebp (rN = R[N], LR = R[0x90/4]); a thread blocked in a trap has R either in ebp or
   somewhere on its native frame chain / stack, so candidates are validated by walking
   the guest back chain. */
#include <mach/thread_act.h>
#include <time.h>

static volatile time_t g_main_bridge_t;
static volatile unsigned int g_main_bridge_id;
static volatile int g_hang_req;

static int oah_hang_rd(unsigned int a, unsigned int *v) { return a > 0x1000 && !(a & 3) && probe_read(a, v); }

/* Is X a plausible guest register file?  r1 must be a readable, 16-ish aligned guest
   stack whose back chain goes upward at least twice with readable LR save words. */
int oah_hang_isR_ext(unsigned int X);
static int oah_hang_isR(unsigned int X) {
    unsigned int sp, back, lr, n = 0, w;
    if (!oah_hang_rd(X + 4, &sp) || !oah_hang_rd(X + 0x90, &w)) return 0;
    if (sp < 0x1000 || (sp & 3) || sp >= 0xfffe0000U) return 0;
    while (n < 3) {
        if (!oah_hang_rd(sp, &back)) return 0;
        back = OSSwapBigToHostInt32(back);
        if (!back) return n >= 1;
        if (back <= sp || back - sp > 0x100000 || (back & 3) || !oah_hang_rd(back + 8, &lr)) return 0;
        sp = back; n++;
    }
    return 1;
}

static int oah_hang_dump_R(unsigned int X, char *b, int cap) {
    unsigned int *R = (unsigned int *)X, sp = R[1], k;
    int o = snprintf(b, cap, "[hang]   R=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x lr=%08x\n[hang]   guest bt:",
            X, R[1], R[3], R[4], R[5], R[6], R[0x90 / 4]);
    for (k = 0; k < 28 && sp > 0x1000 && !(sp & 3) && o < cap - 12; k++) {
        unsigned int back, lr;
        if (!oah_hang_rd(sp, &back)) break;
        back = OSSwapBigToHostInt32(back);
        if (!back || back <= sp || !oah_hang_rd(back + 8, &lr)) break;
        o += snprintf(b + o, cap - o, " %08x", OSSwapBigToHostInt32(lr));
        if (getenv("OAH_HANGNAMES") && o < cap - 64) {   /* image+offset of each frame */
            unsigned int base = 0; const char *nm = oah_guest_image_name_fast(OSSwapBigToHostInt32(lr), &base), *sl = strrchr(nm, '/');
            o += snprintf(b + o, cap - o, "(%s+%x)", sl ? sl + 1 : nm, OSSwapBigToHostInt32(lr) - base);
        }
        sp = back;
    }
    if (o < cap - 2) { b[o++] = '\n'; b[o] = 0; }
    return o;
}

static void oah_hang_dump_all(const char *why) {
    thread_act_array_t th; mach_msg_type_number_t n = 0, i;
    thread_t self = mach_thread_self();
    oah_report_mark(!strcmp(why, "SIGUSR2") || strstr(why, "request") ? "dump" : "hang");
    fprintf(stderr, "[hang] ===== dump (%s): main last bridge %s %lds ago =====\n", why,
            g_main_bridge_id ? oah_bridge_name(g_main_bridge_id) : "-", (long)(time(0) - g_main_bridge_t));
    fprintf(stderr, "[hang] guest-call lock held by %x (depth %d), clipboard lock by %x\n", g_cp_owner ? pthread_mach_thread_np(g_cp_owner) : 0, g_cp_count, g_clip_mx_owner);
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    for (i = 0; i < n; i++) {
        x86_thread_state32_t ts; mach_msg_type_number_t c = x86_THREAD_STATE32_COUNT;
        unsigned int cand[160], nc = 0, k, fp, found = 0, gotst = 0, sa[8], tmo[2] = {0, 0}, hastmo = 0, nbtv[10], nnb = 0;
        /* While the thread is suspended only raw reads happen (no stdio, no
           snprintf, no malloc): a suspended thread may hold any libc lock. */
        unsigned int Rcopy[0x94 / 4], btv[28], nbt = 0, jitpc = 0, jitcode[8];
        char buf[2048]; int o = 0;
        thread_basic_info_data_t bi; mach_msg_type_number_t bic = THREAD_BASIC_INFO_COUNT; int gotbi;
        if (th[i] == self) continue;
        gotbi = thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bic) == KERN_SUCCESS;
        thread_suspend(th[i]);
        if (thread_get_state(th[i], x86_THREAD_STATE32, (thread_state_t)&ts, &c) == KERN_SUCCESS) {
            gotst = 1;
            cand[nc++] = ts.__ebp; cand[nc++] = ts.__ebx; cand[nc++] = ts.__esi; cand[nc++] = ts.__edi;
            for (fp = ts.__ebp, k = 0; k < 16 && nc < 32; k++) {   /* saved ebps on the native chain */
                unsigned int nfp;
                if (nnb < 10 && oah_hang_rd(fp + 4, &nbtv[nnb])) nnb++;
                if (!oah_hang_rd(fp, &nfp) || nfp <= fp) break;
                cand[nc++] = nfp; fp = nfp;
            }
            /* In Rosetta's indirect-branch dispatcher (translate 0xb8070a0a..0xb8070a35): eax is
               the lookup entry, *(eax+0x1c) the target block, *(block+0x20) its guest pc. */
            if (ts.__eip >= 0xb8070a0a && ts.__eip <= 0xb8070a19) {
                unsigned int blk = 0, gpc = 0;
                if (oah_hang_rd(ts.__eax + 0x1c, &blk) && blk && oah_hang_rd(blk + 0x20, &gpc)) jitpc = gpc;
            } else if (ts.__eip > 0xb8070a19 && ts.__eip <= 0xb8070a2d) jitpc = ts.__eax;   /* already loaded */
            if (jitpc) for (k = 0; k < 8; k++) if (!oah_hang_rd((jitpc & ~3u) + 4 * k, &jitcode[k])) jitcode[k] = 0;
            for (k = 0; k < 8; k++) if (!oah_hang_rd(ts.__esp + 4 + 4 * k, &sa[k])) sa[k] = 0;   /* syscall args */
            if (ts.__eax == 363 && sa[5]) { hastmo = oah_hang_rd(sa[5], &tmo[0]) && oah_hang_rd(sa[5] + 4, &tmo[1]); }
            for (k = 0; k < 4096 && nc < 160; k++) {              /* words on the native stack */
                unsigned int v;
                if (!oah_hang_rd(ts.__esp + 4 * k, &v)) break;
                if (v > 0x10000 && !(v & 3)) cand[nc++] = v;
            }
            for (k = 0; k < nc && !found; k++) if (oah_hang_isR(cand[k])) found = cand[k];
            if (found) {
                unsigned int sp, back, lr;
                for (k = 0; k < 0x94 / 4; k++) if (!oah_hang_rd(found + 4 * k, &Rcopy[k])) Rcopy[k] = 0;
                for (sp = Rcopy[1]; nbt < 28 && sp > 0x1000 && !(sp & 3); ) {
                    if (!oah_hang_rd(sp, &back)) break;
                    back = OSSwapBigToHostInt32(back);
                    if (!back || back <= sp || !oah_hang_rd(back + 8, &lr)) break;
                    btv[nbt++] = OSSwapBigToHostInt32(lr);
                    sp = back;
                }
            }
        }
        thread_resume(th[i]);
        /* formatting happens only after the thread runs again */
        if (gotst) {
            o = snprintf(buf, sizeof buf, "[hang] thread %u port=%x eip=%08x esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n[hang]   args %x %x %x %x %x %x %x %x\n", i, th[i], ts.__eip, ts.__esp, ts.__ebp, ts.__eax, ts.__ecx, ts.__edx, ts.__esi, ts.__edi,
                         sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], sa[6], sa[7]);
            if (gotbi) o += snprintf(buf + o, sizeof buf - o, "[hang]   info run=%d susp=%d flags=%#x sleep=%d pol=%d pri=%d cpu=%d.%06d\n",
                                     bi.run_state, bi.suspend_count, bi.flags, bi.sleep_time, bi.policy, bi.cpu_usage,
                                     bi.user_time.seconds, bi.user_time.microseconds);
            if (jitpc) {
                o += snprintf(buf + o, sizeof buf - o, "[hang]   dispatching to guest pc %08x:", jitpc);
                for (k = 0; k < 8; k++) o += snprintf(buf + o, sizeof buf - o, " %08x", OSSwapBigToHostInt32(jitcode[k]));
                o += snprintf(buf + o, sizeof buf - o, "\n");
            }
            if (gotbi) o += snprintf(buf + o, sizeof buf - o, "[hang]   system time %d.%06d\n", bi.system_time.seconds, bi.system_time.microseconds);
            o += snprintf(buf + o, sizeof buf - o, "[hang]   native bt:");
            for (k = 0; k < nnb; k++) {
                Dl_info di;
                o += snprintf(buf + o, sizeof buf - o, " %08x", nbtv[k]);
                if (nbtv[k] < 0xb0000000u && dladdr((void *)nbtv[k], &di) && di.dli_sname && o < (int)sizeof buf - 64)
                    o += snprintf(buf + o, sizeof buf - o, "(%s+%u)", di.dli_sname, nbtv[k] - (unsigned int)di.dli_saddr);
            }
            o += snprintf(buf + o, sizeof buf - o, "\n");
            if (hastmo) o += snprintf(buf + o, sizeof buf - o, "[hang]   kevent timeout {%u, %u}\n", tmo[0], tmo[1]);
            if (found) {
                o += snprintf(buf + o, sizeof buf - o, "[hang]   R=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x lr=%08x\n[hang]   guest bt:",
                              found, Rcopy[1], Rcopy[3], Rcopy[4], Rcopy[5], Rcopy[6], Rcopy[0x90 / 4]);
                for (k = 0; k < nbt && o < (int)sizeof buf - 12; k++) o += snprintf(buf + o, sizeof buf - o, " %08x", btv[k]);
                if (o < (int)sizeof buf - 2) { buf[o++] = '\n'; buf[o] = 0; }
                if (getenv("OAH_HANG_R3")) {   /* the object in r3 and its isa (big-endian words) */
                    unsigned int w0, isa = 0;
                    o += snprintf(buf + o, sizeof buf - o, "[hang]   *r3:");
                    for (k = 0; k < 8; k++) if (oah_hang_rd(Rcopy[3] + 4 * k, &w0)) { o += snprintf(buf + o, sizeof buf - o, " %08x", OSSwapBigToHostInt32(w0)); if (!k) isa = OSSwapBigToHostInt32(w0); }
                    {   /* objc1 class: name at +8 */
                        unsigned int np = 0; char nm[64]; int j;
                        if (oah_hang_rd(Rcopy[3] + 8, &np)) {
                            np = OSSwapBigToHostInt32(np);
                            for (j = 0; j < 63; j++) { unsigned int w; if (!oah_hang_rd((np + j) & ~3u, &w)) break; nm[j] = ((char *)&w)[(np + j) & 3]; if (!nm[j]) break; }
                            nm[j < 63 ? j : 63] = 0;
                            o += snprintf(buf + o, sizeof buf - o, " name@%08x=\"%s\"", np, nm);
                        }
                    }
                    {   /* metaclass method list (class methods): isa+0x1c */
                        unsigned int ml = 0, w1, j2;
                        if (isa && oah_hang_rd(isa + 0x1c, &ml)) {
                            ml = OSSwapBigToHostInt32(ml);
                            o += snprintf(buf + o, sizeof buf - o, "\n[hang]   meta methodLists %08x:", ml);
                            for (j2 = 0; j2 < 12; j2++) if (oah_hang_rd(ml + 4 * j2, &w1)) o += snprintf(buf + o, sizeof buf - o, " %08x", OSSwapBigToHostInt32(w1));
                        }
                    }
                    o += snprintf(buf + o, sizeof buf - o, "\n[hang]   *isa:");
                    for (k = 0; k < 10 && isa; k++) if (oah_hang_rd(isa + 4 * k, &w0)) o += snprintf(buf + o, sizeof buf - o, " %08x", OSSwapBigToHostInt32(w0));
                    o += snprintf(buf + o, sizeof buf - o, "\n");
                }
            } else if (i == 0 && g_main_R) {   /* main thread in translated code: use its register file from the last bridge */
                unsigned int *M = (unsigned int *)g_main_R, sp = M[1], k2, back, ra, bb;
                const char *nm2 = oah_guest_image_name(M[0x90 / 4], &bb), *sl2 = strrchr(nm2, '/');
                char rf[512]; const char *f2 = oah_regfile(M[0x90 / 4], rf, sizeof rf);
                o += snprintf(buf + o, sizeof buf - o, "[hang]   (main R %08x from the last bridge) r1=%08x r3=%08x lr=%08x %s+0x%x [%s]\n[hang]   guest bt:",
                              (unsigned int)M, M[1], M[3], M[0x90 / 4], sl2 ? sl2 + 1 : nm2, M[0x90 / 4] - bb, f2);
                for (k2 = 0; k2 < 20 && sp > 0x1000 && o < (int)sizeof buf - 40; k2++) {
                    if (!oah_hang_rd(sp, &back)) break; back = OSSwapBigToHostInt32(back);
                    if (!back || back <= sp || !oah_hang_rd(back + 8, &ra)) break; ra = OSSwapBigToHostInt32(ra);
                    f2 = oah_regfile(ra, rf, sizeof rf);
                    o += snprintf(buf + o, sizeof buf - o, " %08x[%s]", ra, f2);
                    sp = back;
                }
                o += snprintf(buf + o, sizeof buf - o, "\n");
            } else snprintf(buf + o, sizeof buf - o, "[hang]   (no guest register file found)\n");
        }
        if (o) fputs(buf, stderr);
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof th[0]);
    mach_port_deallocate(mach_task_self(), self);
    {   /* guest image list (header address + install name) so guest bt addresses can be
           symbolised offline with nm -arch ppc on the Snow Leopard originals */
        vm_address_t a = 0x1000;
        for (;;) {
            vm_size_t size = 0; vm_region_basic_info_data_t info;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t obj = MACH_PORT_NULL; vm_address_t p;
            if (vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
            if (a >= 0xb0000000u) break;
            if ((info.protection & VM_PROT_READ) && size <= 0x8000000u)
                if (oah_scan_region(a)) for (p = a; p < a + size; p += 4096) {
                    unsigned int h0, h1, h3, h4;
                    if (!oah_hang_rd(p, &h0) || h0 != OSSwapHostToBigInt32(0xfeedfaceu)) continue;
                    if (!oah_hang_rd(p + 4, &h1) || h1 != OSSwapHostToBigInt32(18u)) continue;
                    if (!oah_hang_rd(p + 12, &h3) || !oah_hang_rd(p + 16, &h4)) continue;
                    {
                        unsigned int ncmds = OSSwapBigToHostInt32(h4), off = 28, k, cmd, sz, no;
                        const char *nm = OSSwapBigToHostInt32(h3) == 2 ? "(main executable)" : "(no id)";
                        for (k = 0; k < ncmds && off < 16384; k++) {
                            if (!oah_hang_rd(p + off, &cmd) || !oah_hang_rd(p + off + 4, &sz)) break;
                            cmd = OSSwapBigToHostInt32(cmd); sz = OSSwapBigToHostInt32(sz);
                            if (sz < 8) break;
                            if (cmd == 0xd && oah_hang_rd(p + off + 8, &no)) { nm = (const char *)(p + off + OSSwapBigToHostInt32(no)); break; }
                            off += sz;
                        }
                        fprintf(stderr, "[hang] image %08x %s\n", (unsigned int)p, nm);
                    }
                }
            a += size;
        }
    }
    { extern void oah_dring_dump(void); oah_dring_dump(); }
    fprintf(stderr, "[hang] ===== end =====\n");
    oah_report_flush();
}

#include "oah_exc.h"

/* Translator lock deadlock breaker.
   translate's locks hold the owner's thread port (lock word: 0 = free). A thread that finds the
   lock held, with its "owner may be suspended" flag set, writes its own port into the word,
   resumes the old owner and spins until the word is 0 (translate 0xb8144c13-0xb8144c30). But the
   old owner releases with compare-and-swap(its port -> 0), which now fails: the word stays at the
   waiter's port for ever. Every other thread then waits on that waiter, all at 100% CPU
   (CS2's package helpers HFSTest, pltool and Help Center hung the install at random).
   Seen twice a second apart in that loop with the word still its own port: the old owner has long
   left its critical section, so free the word; the waiter takes the lock and the rest follow.
   OAH_NO_LOCKFIX=1 disables. */
static void oah_lockfix_scan(void) {
    static thread_t last_t; static unsigned int last_lock; static int off = -1;
    thread_act_array_t th; mach_msg_type_number_t n = 0, i;
    thread_t self = mach_thread_self(), hit_t = 0; unsigned int hit_lock = 0;
    if (off < 0) off = getenv("OAH_NO_LOCKFIX") ? 1 : 0;
    if (off) { mach_port_deallocate(mach_task_self(), self); return; }
    if (task_threads(mach_task_self(), &th, &n) == KERN_SUCCESS) {
        for (i = 0; i < n; i++) {
            x86_thread_state32_t ts; mach_msg_type_number_t c = x86_THREAD_STATE32_COUNT; unsigned int w;
            if (th[i] != self && !hit_t &&
                thread_get_state(th[i], x86_THREAD_STATE32, (thread_state_t)&ts, &c) == KERN_SUCCESS &&
                ts.__eip >= 0xb8144c20 && ts.__eip <= 0xb8144c30 && ts.__edi > 0x1000 &&
                oah_hang_rd(ts.__edi, &w) && w == ts.__esi && w == th[i]) {
                hit_t = th[i]; hit_lock = ts.__edi;
            }
            if (th[i] != hit_t) mach_port_deallocate(mach_task_self(), th[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
    }
    if (hit_t && hit_t == last_t && hit_lock == last_lock) {
        int ok = __sync_bool_compare_and_swap((volatile unsigned int *)hit_lock, (unsigned int)hit_t, 0u);
        fprintf(stderr, "[lockfix] thread %x stuck waiting for lock %08x that it holds itself: %s\n", hit_t, hit_lock, ok ? "freed" : "changed meanwhile");
        last_t = 0; last_lock = 0;
    } else { last_t = hit_t; last_lock = hit_lock; }
    if (hit_t) mach_port_deallocate(mach_task_self(), hit_t);
    mach_port_deallocate(mach_task_self(), self);
}
static void *oah_hang_watch(void *arg) {
    int lim = (int)(long)arg;
    g_watch_thread = mach_thread_self();
    time_t lastdump = 0;
    for (;;) {
        time_t now;
        sleep(1);
        now = time(0);
        if (!getenv("OAH_NO_EXCWATCH")) oah_exc_poll();
        oah_lockfix_scan();
        if (!access("/tmp/oah_dump_now", F_OK)) { unlink("/tmp/oah_dump_now"); g_hang_req = 1; }   /* touch it to request a dump */
        {   /* /tmp/oah_peek.<pid>: lines "hexaddr nwords" or "s hexaddr" -> guest memory (big-endian words / C string) */
            char pf[64]; FILE *f;
            snprintf(pf, sizeof pf, "/tmp/oah_peek.%d", getpid());
            if ((f = fopen(pf, "r"))) {
                char ln[128];
                while (fgets(ln, sizeof ln, f)) {
                    unsigned int a = 0, n = 0, w, k;
                    if (ln[0] == 'r' && sscanf(ln + 1, "%x", &a) == 1) {
                        vm_address_t ra = a; vm_size_t rs = 0; natural_t depth = 99;
                        vm_region_submap_info_data_t si; mach_msg_type_number_t sc = VM_REGION_SUBMAP_INFO_COUNT;
                        if (vm_region_recurse(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&si, &sc) == KERN_SUCCESS)
                            fprintf(stderr, "[peek] region %08x: %08lx-%08lx prot=%d/%d tag=%u share=%d\n", a, (unsigned long)ra,
                                    (unsigned long)(ra + rs), si.protection, si.max_protection, si.user_tag, si.share_mode);
                        else fprintf(stderr, "[peek] region %08x: none\n", a);
                    } else if (ln[0] == 's' && sscanf(ln + 1, "%x", &a) == 1) {
                        char s[256]; int j;
                        for (j = 0; j < 255; j++) { if (!oah_hang_rd((a + j) & ~3u, &w)) break; s[j] = ((char *)&w)[(a + j) & 3]; if (!s[j]) break; }
                        s[j] = 0;
                        fprintf(stderr, "[peek] %08x \"%s\"\n", a, s);
                    } else if (sscanf(ln, "%x %u", &a, &n) == 2) {
                        fprintf(stderr, "[peek] %08x:", a);
                        for (k = 0; k < n && k < 256; k++) { if (!oah_hang_rd(a + 4 * k, &w)) { fprintf(stderr, " ????????"); break; } fprintf(stderr, " %08x", OSSwapBigToHostInt32(w)); }
                        fprintf(stderr, "\n");
                    }
                }
                fclose(f); unlink(pf);
            }
        }
        {   static time_t lastsave; if (now - lastsave >= 20) { lastsave = now; oah_tel_save(); } }
        if (g_hang_req) { g_hang_req = 0; oah_hang_dump_all("SIGUSR2"); continue; }
        if (lim > 0 && g_main_bridge_t && now - g_main_bridge_t >= lim && now - lastdump >= 30) {
            lastdump = now;
            oah_hang_dump_all("main thread quiet");
        }
    }
    return 0;
}
static void oah_hang_usr2(int s) { (void)s; g_hang_req = 1; }

static void oah_hang_init(void) {
    const char *e = getenv("OAH_HANGDUMP");
    pthread_t t;
    signal(SIGUSR2, oah_hang_usr2);
    oah_bb_init();
    pthread_create(&t, 0, oah_hang_watch, (void *)(long)(e ? atoi(e) : 0));
    pthread_detach(t);
}
static inline void oah_hang_note(unsigned int id) {
    if (pthread_main_np()) { g_main_bridge_t = time(0); g_main_bridge_id = id; }
}

int oah_hang_isR_ext(unsigned int X) { return oah_hang_isR(X); }
