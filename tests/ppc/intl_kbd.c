/* intl_kbd: Script Manager / keyboard layout state that old apps read at launch (Office 2004 reads the
   KCHR through smKCHRCache).  Build (10.6): gcc -arch ppc -o intl_kbd intl_kbd.c -framework Carbon */
#include <stdio.h>
#include <Carbon/Carbon.h>
int main(void) {
    static const short sel[] = { smVersion, smSysScript, smKeyScript, smKCHRCache, smRegionCode, smKeyCache, smDoubleByte };
    static const char *nm[] = { "smVersion", "smSysScript", "smKeyScript", "smKCHRCache", "smRegionCode", "smKeyCache", "smDoubleByte" };
    int i;
    for (i = 0; i < 7; i++) {
        long v = GetScriptManagerVariable(sel[i]);
        if (sel[i] == smKCHRCache || sel[i] == smKeyCache) printf("%-22s %s\n", nm[i], v ? "non-null" : "NULL");
        else printf("%-22s %ld\n", nm[i], v);
    }
    { long kchr = GetScriptManagerVariable(smKCHRCache);
      if (kchr) printf("%-22s version %d\n", "KCHR", *(short *)kchr); }
    printf("%-22s %ld\n", "smScriptKeys(roman)", GetScriptVariable(smRoman, smScriptKeys));
    { KeyboardLayoutRef kl = 0; OSStatus e = KLGetCurrentKeyboardLayout(&kl); const void *p = 0; CFStringRef n = 0; char b[128] = "";
      printf("%-22s %d %s\n", "KLGetCurrentLayout", (int)e, kl ? "ok" : "NULL");
      if (kl) {
          KLGetKeyboardLayoutProperty(kl, kKLName, &p); n = (CFStringRef)p; if (n) CFStringGetCString(n, b, sizeof b, kCFStringEncodingUTF8);
          printf("%-22s %s\n", "layout name", b);
          p = 0; e = KLGetKeyboardLayoutProperty(kl, kKLKCHRData, &p); printf("%-22s %d %s\n", "KCHRData", (int)e, p ? "non-null" : "NULL");
          p = 0; e = KLGetKeyboardLayoutProperty(kl, kKLuchrData, &p); printf("%-22s %d %s\n", "uchrData", (int)e, p ? "non-null" : "NULL");
          { long kind = 0; KLGetKeyboardLayoutProperty(kl, kKLKind, (const void **)&kind); printf("%-22s %ld\n", "kind", kind); }
      } }
    { TISInputSourceRef s = TISCopyCurrentKeyboardLayoutInputSource(); char b[256] = "";
      if (s) { CFStringRef id = TISGetInputSourceProperty(s, kTISPropertyInputSourceID); if (id) CFStringGetCString(id, b, sizeof b, kCFStringEncodingUTF8); }
      printf("%-22s %s\n", "TIS current", s ? b : "NULL"); }
    { Handle h = GetResource('itlk', 0); printf("%-22s %s\n", "itlk 0", h ? "present" : "absent"); }
    { Handle h = GetIntlResource(0); printf("%-22s %s\n", "itl0", h ? "present" : "absent"); }
    return 0;
}
