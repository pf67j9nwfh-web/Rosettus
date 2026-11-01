/* fontinfo.c: QuickDraw font metrics (GetFontInfo, StringWidth) for a few fonts and sizes, in an
   offscreen GWorld. Run under Snow Leopard's Rosetta and under Rosettus and compare.
   gcc -arch ppc -o fontinfo fontinfo.c -framework Carbon */
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    static const char *names[] = { "Helvetica", "Times", "Geneva", "Lucida Grande", "Courier", "Arial", "Monaco", 0 };
    static const int sizes[] = { 9, 10, 12, 14, 18, 24, 0 };
    Rect r = { 0, 0, 100, 100 };
    GWorldPtr gw = 0;
    int i, j;
    if (NewGWorld(&gw, 32, &r, 0, 0, 0) || !gw) { printf("NewGWorld failed\n"); return 1; }
    SetGWorld(gw, 0);
    for (i = 0; names[i]; i++) {
        Str255 pn;
        FMFontFamily fam;
        pn[0] = (unsigned char)strlen(names[i]); memcpy(pn + 1, names[i], pn[0]);
        fam = FMGetFontFamilyFromName(pn);
        if (fam == kInvalidFontFamily) { printf("%-14s missing\n", names[i]); continue; }
        TextFont(fam);
        for (j = 0; sizes[j]; j++) {
            FontInfo fi;
            Str255 s = "\pThe quick brown fox";
            TextSize(sizes[j]);
            GetFontInfo(&fi);
            printf("%-14s %2d: ascent %2d descent %2d widMax %3d leading %2d  width %4d\n", names[i], sizes[j],
                   fi.ascent, fi.descent, fi.widMax, fi.leading, StringWidth(s));
        }
    }
    return 0;
}
