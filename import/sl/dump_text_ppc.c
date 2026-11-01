/* dump_text_ppc.c - a PowerPC program that saves Rosetta's translator code as the kernel mapped it.
 *
 * Snow Leopard runs PowerPC programs inside /usr/libexec/oah/translate, in the same address space.
 * translate's __TEXT segment is protected (SG_PROTECTED_VERSION_1) on disk and decoded by the
 * kernel when it is loaded on Apple hardware; the Mojave port needs the plain code. This program
 * runs under your own Rosetta on your own Snow Leopard Mac and writes the mapped segment to the
 * file named on its command line. Nothing is downloaded or sent anywhere.
 *
 * build (Snow Leopard, Xcode 3.2 with PowerPC support):
 *     gcc -arch ppc -O2 -o dump_text_ppc dump_text_ppc.c
 * run:  ./dump_text_ppc /path/to/translate_text.bin
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define TEXT_ADDR 0xb8000000u      /* translate's __TEXT vmaddr (10.6.x) */
#define TEXT_SIZE 0x001df000u      /* and vmsize */

int main(int argc, char **argv) {
    const char *base = (const char *)TEXT_ADDR;
    unsigned int done = 0;
    int fd;
    if (argc != 2) { fprintf(stderr, "usage: %s OUTPUT\n", argv[0]); return 2; }
    /* sanity check: an i386 Mach-O header (0xfeedface, little-endian on disk) */
    if (!(base[0] == (char)0xce && base[1] == (char)0xfa && base[2] == (char)0xed && base[3] == (char)0xfe)) {
        fprintf(stderr, "dump_text_ppc: no i386 Mach-O header at 0x%08x - is this running under Rosetta 10.6?\n", TEXT_ADDR);
        return 1;
    }
    fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(argv[1]); return 1; }
    while (done < TEXT_SIZE) {
        ssize_t n = write(fd, base + done, TEXT_SIZE - done);
        if (n <= 0) { perror("write"); close(fd); return 1; }
        done += (unsigned int)n;
    }
    close(fd);
    printf("wrote %u bytes of translate __TEXT to %s\n", done, argv[1]);
    return 0;
}
