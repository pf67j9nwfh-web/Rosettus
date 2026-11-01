/* dump_text_i386.dylib - inserted (DYLD_INSERT_LIBRARIES) into Snow Leopard's own
 * /usr/libexec/oah/translate when it is started on Mojave from an extracted install DVD.
 * translate's __TEXT is an Apple-protected segment: the kernel decrypts it page by page as the
 * process touches it. The constructor runs inside translate before its main(), copies the
 * decrypted __TEXT to the file named by ROSETTUS_DUMP_TEXT and exits. On Snow Leopard itself
 * import/sl/dump_text_ppc does the same job from a PowerPC process.
 * build (Snow Leopard): gcc -arch i386 -dynamiclib -o dump_text_i386.dylib dump_text_i386.c */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void dump_text(void) {
    const char *out = getenv("ROSETTUS_DUMP_TEXT");
    const struct mach_header *mh;
    const struct load_command *lc;
    unsigned int i;
    FILE *f;
    if (!out || !*out) return;
    mh = _dyld_get_image_header(0);
    if (!mh || mh->magic != MH_MAGIC) _exit(3);
    lc = (const struct load_command *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        const struct segment_command *sg = (const struct segment_command *)lc;
        if (lc->cmd != LC_SEGMENT || strcmp(sg->segname, "__TEXT")) continue;
        /* __TEXT starts at the Mach-O header (file offset 0) */
        f = fopen(out, "wb");
        if (!f) _exit(4);
        if (fwrite((const void *)mh, 1, sg->filesize, f) != sg->filesize) _exit(5);
        fclose(f);
        _exit(0);
    }
    _exit(6);
}
