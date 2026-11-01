/* threads: the Mach thread calls media code makes when it sets up worker threads (QuickTime's H.264
   decoder: thread_info THREAD_BASIC_INFO / THREAD_SCHED_TIMESHARE_INFO, thread_policy_set
   THREAD_PRECEDENCE_POLICY), and the processor count it sizes its pool by. Under Rosettus the
   answers come from liboah's thread_info fix, armed once the guest frameworks load: link Carbon. */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#ifndef __LP64__
#include <Carbon/Carbon.h>
#endif
static void *body(void *a) { (void)a; sleep(2); return 0; }
static void res(const char *what, int ok, const char *fmt, long a, long b) {
    printf("RESULT %s %s ", what, ok ? "PASS" : "FAIL"); printf(fmt, a, b); printf("\n");
}
int main(void) {
    pthread_t t; pthread_attr_t at; mach_port_t th; kern_return_t kr; int ncpu = 0; size_t l = sizeof ncpu;
    struct thread_basic_info bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
    struct policy_timeshare_info ti; mach_msg_type_number_t c2 = POLICY_TIMESHARE_INFO_COUNT;
    thread_precedence_policy_data_t p;
#ifndef __LP64__
    { long v = 0; Gestalt(gestaltSystemVersion, &v); }   /* load the Carbon frameworks first */
#endif
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, body, 0)) { res("pthread_create", 0, "%ld%ld", 0, 0); return 1; }
    th = pthread_mach_thread_np(t);
    kr = thread_info(mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t)&bi, &c);
    res("thread_info_self", kr == 0 && bi.policy == POLICY_TIMESHARE, "kr %ld policy %ld", kr, bi.policy);
    c = THREAD_BASIC_INFO_COUNT;
    kr = thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &c);
    res("thread_info_basic", kr == 0 && c == THREAD_BASIC_INFO_COUNT && bi.policy == POLICY_TIMESHARE, "kr %ld policy %ld", kr, bi.policy);
    kr = thread_info(th, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t)&ti, &c2);
    res("thread_info_timeshare", kr == 0 && ti.base_priority > 0 && ti.base_priority < 64, "kr %ld base %ld", kr, ti.base_priority);
    p.importance = 31 - (kr ? 31 : ti.base_priority);
    kr = thread_policy_set(th, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&p, THREAD_PRECEDENCE_POLICY_COUNT);
    res("thread_policy_set", kr == 0, "kr %ld%.0ld", kr, 0);
    { thread_extended_policy_data_t e; mach_msg_type_number_t n = THREAD_EXTENDED_POLICY_COUNT; boolean_t d = 0;
      kr = thread_policy_get(th, THREAD_EXTENDED_POLICY, (thread_policy_t)&e, &n, &d);
      res("thread_policy_get", kr == 0 && n == THREAD_EXTENDED_POLICY_COUNT, "kr %ld timeshare %ld", kr, e.timeshare); }
    sysctlbyname("hw.ncpu", &ncpu, &l, 0, 0);
#ifndef __LP64__
    res("ncpu", ncpu > 0 && (long)MPProcessors() >= 1, "hw.ncpu %ld MPProcessors %ld", ncpu, (long)MPProcessors());
#else
    res("ncpu", ncpu > 0, "hw.ncpu %ld%.0ld", ncpu, 0);
#endif
    return 0;
}
