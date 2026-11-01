#include <Carbon/Carbon.h>
#include <stdio.h>
int main(void) {
    ProcessSerialNumber psn = { 0, kNoProcess }, me;
    GetCurrentProcess(&me);
    while (GetNextProcess(&psn) == noErr) {
        ProcessInfoRec info; Str255 name; FSSpec spec; Boolean same = 0; OSErr e;
        memset(&info, 0, sizeof info); memset(&spec, 0, sizeof spec);
        info.processInfoLength = sizeof info; info.processName = name; info.processAppSpec = &spec;
        name[0] = 0;
        if ((e = GetProcessInformation(&psn, &info)) == noErr) {
            pid_t pid = 0; GetProcessPID(&psn, &pid); SameProcess(&psn, &me, &same);
            printf("%u.%u pid %d spec '%.*s' (vol %d dir %ld) type %.4s sig %.4s name '%.*s'%s\n", (unsigned)psn.highLongOfPSN, (unsigned)psn.lowLongOfPSN, (int)pid,
                   spec.name[0], spec.name + 1, spec.vRefNum, (long)spec.parID,
                   (char *)&info.processType, (char *)&info.processSignature, name[0], name + 1, same ? "  <- me" : "");
        } else printf("%u.%u err %d\n", (unsigned)psn.highLongOfPSN, (unsigned)psn.lowLongOfPSN, e);
    }
    return 0;
}
