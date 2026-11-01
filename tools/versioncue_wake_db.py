#!/usr/bin/python
# Wake only the database directly owned by the Version Cue process which is stopping.
import os, signal, subprocess, sys
server = int(sys.argv[1])
base = os.environ.get('VC_APP_ROOT', '/Applications/Adobe Version Cue CS2')
needle = base.rstrip('/') + '/data/database/bin/mysqld '
if server <= 1:
    sys.exit(2)
for line in subprocess.check_output(['/bin/ps', '-axo', 'pid=,ppid=,command=']).splitlines():
    fields = line.strip().split(None, 2)
    if len(fields) == 3 and int(fields[1]) == server and needle in fields[2]:
        pid = int(fields[0])
        try:
            os.kill(pid, signal.SIGUSR2)
            sys.stderr.write('vc-controller: woke database pid %d\n' % pid)
        except OSError:
            pass
