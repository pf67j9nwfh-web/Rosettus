/* files_rsrc: File Manager, resource forks, Resource Manager, Alias Manager, POSIX calls.
   Build (10.6): gcc -arch ppc -o files_rsrc files_rsrc.c -framework CoreServices */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <CoreServices/CoreServices.h>

int main(void) {
    char dir[256], path[300];
    FSRef dref, fref; OSErr e; OSStatus s;
    HFSUniStr255 fork;
    snprintf(dir, sizeof dir, "/tmp/ppcdiff-%d", (int)getpid());
    mkdir(dir, 0755);
    s = FSPathMakeRef((const UInt8 *)dir, &dref, 0); printf("%-24s %d\n", "FSPathMakeRef(dir)", (int)s);
    {   UniChar name[] = { 'd', 'a', 't', 'a', '.', 't', 'x', 't' };
        e = FSCreateFileUnicode(&dref, 8, name, kFSCatInfoNone, 0, &fref, 0); printf("%-24s %d\n", "FSCreateFileUnicode", (int)e); }
    FSGetDataForkName(&fork);
    {   FSIORefNum ref; ByteCount n = 0; char buf[64];
        e = FSOpenFork(&fref, fork.length, fork.unicode, fsRdWrPerm, &ref); printf("%-24s %d\n", "FSOpenFork", (int)e);
        e = FSWriteFork(ref, fsFromStart, 0, 11, "hello world", &n); printf("%-24s %d %lu\n", "FSWriteFork", (int)e, (unsigned long)n);
        { SInt64 sz = 0; FSGetForkSize(ref, &sz); printf("%-24s %lld\n", "FSGetForkSize", sz); }
        e = FSReadFork(ref, fsFromStart, 6, 5, buf, &n); buf[n] = 0; printf("%-24s %d '%s'\n", "FSReadFork", (int)e, buf);
        e = FSReadFork(ref, fsFromStart, 6, 50, buf, &n); printf("%-24s %d %lu\n", "FSReadFork(eof)", (int)e, (unsigned long)n);
        FSCloseFork(ref); }
    {   FSCatalogInfo ci; HFSUniStr255 nm;
        e = FSGetCatalogInfo(&fref, kFSCatInfoDataSizes | kFSCatInfoNodeFlags | kFSCatInfoFinderInfo, &ci, &nm, 0, 0);
        printf("%-24s %d size %llu flags %x name-len %d\n", "FSGetCatalogInfo", (int)e, ci.dataLogicalSize, ci.nodeFlags, nm.length);
        ((FileInfo *)ci.finderInfo)->fileType = 'TEXT'; ((FileInfo *)ci.finderInfo)->fileCreator = 'ttxt';
        e = FSSetCatalogInfo(&fref, kFSCatInfoFinderInfo, &ci); printf("%-24s %d\n", "FSSetCatalogInfo", (int)e);
        e = FSGetCatalogInfo(&fref, kFSCatInfoFinderInfo, &ci, 0, 0, 0);
        printf("%-24s %.4s/%.4s\n", "type/creator", (char *)&((FileInfo *)ci.finderInfo)->fileType, (char *)&((FileInfo *)ci.finderInfo)->fileCreator); }
    {   /* resource fork */
        HFSUniStr255 rname; ResFileRefNum rf; Handle h;
        FSGetResourceForkName(&rname);
        FSCreateResourceFork(&fref, rname.length, rname.unicode, 0);
        rf = FSOpenResFile(&fref, fsRdWrPerm); printf("%-24s %d err %d\n", "FSOpenResFile", rf > 0, (int)ResError());
        h = NewHandle(6); memcpy(*h, "\x01\x02\x03\x04\xab\xcd", 6);
        AddResource(h, 'TEST', 128, "\pdiff"); printf("%-24s %d\n", "AddResource", (int)ResError());
        { Handle h2 = NewHandle(4); memcpy(*h2, "STR!", 4); AddResource(h2, 'STR ', 300, "\p"); }
        UpdateResFile(rf); CloseResFile(rf);
        rf = FSOpenResFile(&fref, fsRdPerm);
        printf("%-24s %d %d\n", "Count1Resources", (int)Count1Resources('TEST'), (int)Count1Resources('STR '));
        h = Get1Resource('TEST', 128);
        if (h) { unsigned char *p = (unsigned char *)*h; printf("%-24s %ld %02x%02x%02x%02x%02x%02x\n", "Get1Resource", (long)GetHandleSize(h), p[0], p[1], p[2], p[3], p[4], p[5]); }
        { ResID id; ResType t; Str255 nm; GetResInfo(h, &id, &t, nm); printf("%-24s %d %.4s %.*s\n", "GetResInfo", id, (char *)&t, nm[0], nm + 1); }
        CloseResFile(rf);
        snprintf(path, sizeof path, "%s/data.txt/..namedfork/rsrc", dir);
        { struct stat st; printf("%-24s %d %lld\n", "stat(rsrc)", stat(path, &st), st.st_size > 0 ? 1LL : 0LL); } }
    {   /* alias */
        AliasHandle al = 0; FSRef out; Boolean changed;
        e = FSNewAlias(0, &fref, &al); printf("%-24s %d\n", "FSNewAlias", (int)e);
        e = FSResolveAlias(0, al, &out, &changed); printf("%-24s %d same %d\n", "FSResolveAlias", (int)e, FSCompareFSRefs(&out, &fref) == noErr); }
    {   UniChar nn[] = { 'r', 'e', 'n', 'a', 'm', 'e', 'd' }; FSRef nref;
        e = FSRenameUnicode(&fref, 7, nn, kTextEncodingUnknown, &nref); printf("%-24s %d\n", "FSRenameUnicode", (int)e);
        { UInt8 p[512]; FSRefMakePath(&nref, p, sizeof p); printf("%-24s %s\n", "FSRefMakePath(tail)", strrchr((char *)p, '/')); }
        {   FSIterator it; ItemCount n = 0; FSRef refs[8]; HFSUniStr255 names[8];
            FSOpenIterator(&dref, kFSIterateFlat, &it);
            e = FSGetCatalogInfoBulk(it, 8, &n, 0, kFSCatInfoNone, 0, refs, 0, names);
            printf("%-24s %d n=%lu\n", "FSGetCatalogInfoBulk", (int)e, (unsigned long)n); FSCloseIterator(it); }
        e = FSDeleteObject(&nref); printf("%-24s %d\n", "FSDeleteObject", (int)e); }
    {   FSRef tmp; e = FSFindFolder(kUserDomain, kPreferencesFolderType, false, &tmp);
        UInt8 p[512] = ""; FSRefMakePath(&tmp, p, sizeof p); printf("%-24s %d %s\n", "FSFindFolder(prefs)", (int)e, strstr((char *)p, "/Library/") ? strstr((char *)p, "/Library/") : (char *)p);
        e = FSFindFolder(kOnSystemDisk, kTemporaryFolderType, true, &tmp); printf("%-24s %d\n", "FSFindFolder(temp)", (int)e); }
    {   FSVolumeInfo vi; HFSUniStr255 vn; FSRef root;
        e = FSGetVolumeInfo(kFSInvalidVolumeRefNum, 1, 0, kFSVolInfoBlocks | kFSVolInfoFlags, &vi, &vn, &root);
        printf("%-24s %d blocksize-nonzero %d\n", "FSGetVolumeInfo(1)", (int)e, vi.blockSize > 0); }
    rmdir(dir);
    { long r = 0; Gestalt(gestaltSystemVersion, &r); printf("%-24s %lx\n", "Gestalt(sysv)", r); }
    return 0;
}
