#!/usr/bin/python
"""threads.py PID [N]: the threads of a 32-bit process (root): eip/esp/ebp of each, the file each eip is
mapped from (proc_regionfilename) and a few frames of the native ebp chain. sample(1) and vmmap32 crash
on a translated process; this does not. N samples, 0.2 s apart, to see where a spinning thread runs."""
import ctypes, ctypes.util, struct, sys, time
libc = ctypes.CDLL(ctypes.util.find_library('c'))
libproc = ctypes.CDLL('/usr/lib/libproc.dylib')
self_task = ctypes.c_uint.in_dll(libc, 'mach_task_self_')
pid = int(sys.argv[1]); N = int(sys.argv[2]) if len(sys.argv) > 2 else 1
task = ctypes.c_uint()
if libc.task_for_pid(self_task, pid, ctypes.byref(task)) != 0: sys.exit('task_for_pid failed (root?)')
def rd(addr, n=4):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_uint64()
    if libc.mach_vm_read_overwrite(task, ctypes.c_uint64(addr), ctypes.c_uint64(n), ctypes.cast(buf, ctypes.c_void_p), ctypes.byref(got)) != 0: return None
    return buf.raw
def fname(a):
    b = ctypes.create_string_buffer(1024)
    n = libproc.proc_regionfilename(pid, ctypes.c_uint64(a), b, 1024)
    return b.value.split('/')[-1] if n > 0 else '-'
x86_THREAD_STATE32 = 1
for s in range(N):
    th = ctypes.POINTER(ctypes.c_uint)(); cnt = ctypes.c_uint()
    if libc.task_threads(task, ctypes.byref(th), ctypes.byref(cnt)) != 0: sys.exit('task_threads failed')
    print '--- sample %d: %d threads' % (s, cnt.value)
    for i in range(cnt.value):
        st = (ctypes.c_uint * 16)(); sc = ctypes.c_uint(16)
        if libc.thread_get_state(th[i], x86_THREAD_STATE32, st, ctypes.byref(sc)) != 0: print '  thread %d: no state' % i; continue
        eax, ebx, ecx, edx, edi, esi, ebp, esp, ss, efl, eip = st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[10]
        fr = []
        bp = ebp
        for k in range(6):
            w = rd(bp, 8)
            if not w: break
            nbp, ra = struct.unpack('<II', w)
            fr.append('%08x(%s)' % (ra, fname(ra)))
            if nbp <= bp: break
            bp = nbp
        print '  thread %2d eip %08x (%s) esp %08x eax %08x | %s' % (i, eip, fname(eip), esp, eax, ' '.join(fr))
    time.sleep(0.2)
