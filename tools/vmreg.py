#!/usr/bin/python
"""vmreg.py PID [OUT.json]: the address space of a (32-bit) process by region: start, size, user tag,
protection, resident and dirtied pages (mach_vm_region, VM_REGION_EXTENDED_INFO). Needs root
(task_for_pid). vmmap32 crashes on a translated process; footprint folds all guest memory into
translate's __MYZEROPAGE. Prints a summary by tag and by 16 MB address band; OUT.json keeps the list
for vmdiff.py."""
import ctypes, ctypes.util, json, sys
libc = ctypes.CDLL(ctypes.util.find_library('c'))
self_task = ctypes.c_uint.in_dll(libc, 'mach_task_self_')
pid = int(sys.argv[1])
task = ctypes.c_uint()
if libc.task_for_pid(self_task, pid, ctypes.byref(task)) != 0:
    sys.exit('task_for_pid failed (run as root)')
VM_REGION_EXTENDED_INFO = 13
class Ext(ctypes.Structure):
    _fields_ = [('protection', ctypes.c_int), ('user_tag', ctypes.c_uint), ('pages_resident', ctypes.c_uint),
                ('pages_shared_now_private', ctypes.c_uint), ('pages_swapped_out', ctypes.c_uint),
                ('pages_dirtied', ctypes.c_uint), ('ref_count', ctypes.c_uint), ('shadow_depth', ctypes.c_ushort),
                ('external_pager', ctypes.c_ubyte), ('share_mode', ctypes.c_ubyte), ('pages_reusable', ctypes.c_uint)]
addr = ctypes.c_uint64(0); size = ctypes.c_uint64(0)
regs = []
while True:
    info = Ext(); cnt = ctypes.c_uint(ctypes.sizeof(Ext) // 4); obj = ctypes.c_uint()
    kr = libc.mach_vm_region(task, ctypes.byref(addr), ctypes.byref(size), VM_REGION_EXTENDED_INFO,
                             ctypes.byref(info), ctypes.byref(cnt), ctypes.byref(obj))
    if kr != 0: break
    regs.append([addr.value, size.value, info.user_tag, info.protection, info.pages_resident, info.pages_dirtied,
                 info.share_mode, info.pages_swapped_out])
    addr.value += size.value
    if addr.value >= 1 << 32: break
if len(sys.argv) > 2:
    json.dump(regs, open(sys.argv[2], 'w'))
bytag = {}; byband = {}
for a, s, t, p, r, d, sm, sw in regs:
    bt = bytag.setdefault(t, [0, 0, 0]); bt[0] += 1; bt[1] += r; bt[2] += d
    bb = byband.setdefault(a >> 24, [0, 0, 0]); bb[0] += 1; bb[1] += r; bb[2] += d
print '%d regions, resident %d MB, dirtied %d MB' % (len(regs), sum(x[4] for x in regs) * 4 // 1024, sum(x[5] for x in regs) * 4 // 1024)
print 'by tag (regions, resident KB, dirtied KB):'
for t, v in sorted(bytag.items(), key=lambda x: -x[1][2])[:15]:
    print '  tag %3d: %5d %8d %8d' % (t, v[0], v[1] * 4, v[2] * 4)
print 'by 16 MB band:'
for b, v in sorted(byband.items(), key=lambda x: -x[1][2])[:20]:
    print '  %08x: %5d %8d %8d' % (b << 24, v[0], v[1] * 4, v[2] * 4)
