/* sigtest: PowerPC test - guest faults delivered to the guest's own signal handlers, as Java's
   HotSpot needs them (implicit null checks, the safepoint polling page).
   1. NULL read: SA_SIGINFO handler checks si_addr and skips the instruction (srr0 += 4).
   2. NULL write, recovered with siglongjmp.
   3. read of a PROT_NONE page: the handler makes it readable and returns; the read is retried.
   4. the same on a second thread.
   Build: gcc -arch ppc -o sigtest sigtest.c
   Result: it hangs at test 1 under Snow Leopard's own Rosetta too - guest faults never reach
   a guest handler there either - so it has no .sl.txt reference and difftest does not run it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

static volatile int mode, hits;
static volatile void *last_addr;
static sigjmp_buf jb;
static char *page;
static size_t pgsz;

static void handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    hits++;
    last_addr = si->si_addr;
    if (mode == 1) {
#if defined(__ppc__)
        uc->uc_mcontext->ss.srr0 += 4;
#endif
        return;
    }
    if (mode == 2) siglongjmp(jb, sig);
    if (mode == 3) { mprotect(page, pgsz, PROT_READ | PROT_WRITE); return; }
    _exit(99);
}
static int t_null_read(void) {
    volatile int *p = (volatile int *)0; int v = 7;
    mode = 1; hits = 0;
    v = *p;
    return hits == 1 && last_addr == 0 && v == 7 ? 0 : 1;   /* the load was skipped: v unchanged */
}
static int t_longjmp(void) {
    volatile int *p = (volatile int *)0x10;
    int s;
    mode = 2; hits = 0;
    if ((s = sigsetjmp(jb, 1)) == 0) { *p = 1; return 1; }
    return hits == 1 && (s == SIGSEGV || s == SIGBUS) && last_addr == (void *)0x10 ? 0 : 1;
}
static int t_poll(void) {
    volatile int v;
    mode = 3; hits = 0;
    page[0] = 42;
    mprotect(page, pgsz, PROT_NONE);
    v = *(volatile char *)page;
    return hits == 1 && v == 42 && last_addr == page ? 0 : 1;
}
static void *thr(void *a) { (void)a; return (void *)(long)t_poll(); }
int main(void) {
    struct sigaction sa; int r, fails = 0; void *tr;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    pgsz = getpagesize();
    page = mmap(0, pgsz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    r = t_null_read(); printf("RESULT null_read_skip %s (hits %d)\n", r ? "FAIL" : "PASS", hits); fails += r; fflush(stdout);
    r = t_longjmp();   printf("RESULT null_write_longjmp %s (hits %d, addr %p)\n", r ? "FAIL" : "PASS", hits, last_addr); fails += r; fflush(stdout);
    r = t_poll();      printf("RESULT protected_page %s (hits %d)\n", r ? "FAIL" : "PASS", hits); fails += r; fflush(stdout);
    pthread_t t; pthread_create(&t, 0, thr, 0); pthread_join(t, &tr);
    r = (int)(long)tr; printf("RESULT protected_page_thread %s\n", r ? "FAIL" : "PASS"); fails += r;
    printf("RESULT sigtest %s\n", fails ? "FAIL" : "PASS");
    return fails;
}
