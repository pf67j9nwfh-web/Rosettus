#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <dirent.h>
static void pr(const char *t, CFTypeRef u){ char b[1024]=""; if(u){CFStringRef s=CFCopyDescription(u); CFStringGetCString(s,b,sizeof b,kCFStringEncodingUTF8);} printf("%s: %s\n",t,b); }
int main(int argc,char**argv){
  const char *p = argc>1?argv[1]:"/System/Library/PrivateFrameworks/PrintingPrivate.framework/Versions/A/Plugins/PrintCocoaUI.bundle";
  const char *r = argc>2?argv[2]:"CocoaPrintWindow";
  CFURLRef u = CFURLCreateFromFileSystemRepresentation(0,(const UInt8*)p,strlen(p),true);
  CFBundleRef b = CFBundleCreate(0,u);
  pr("bundle", b);
  if(!b) return 1;
  CFStringRef rs = CFStringCreateWithCString(0,r,kCFStringEncodingUTF8);
  pr("nib", CFBundleCopyResourceURL(b, rs, CFSTR("nib"), 0));
  pr("nil-type", CFBundleCopyResourceURL(b, rs, 0, 0));
  pr("strings", CFBundleCopyResourceURL(b, CFSTR("Localizable"), CFSTR("strings"), 0));
  pr("resdir", CFBundleCopyResourcesDirectoryURL(b));
  pr("locs", CFBundleCopyBundleLocalizations(b));
  pr("preflocs", CFBundleCopyLocalizationsForPreferences(CFBundleCopyBundleLocalizations(b),0));
  pr("dev", CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleDevelopmentRegion")));
  pr("tiff", CFBundleCopyResourceURL(b, CFSTR("Add"), CFSTR("tiff"), 0));
  char d[1200]; snprintf(d,sizeof d,"%s/Contents/Resources/English.lproj",p);
  DIR *dd=opendir(d); struct dirent *e; printf("opendir %s -> %p\n",d,dd);
  while(dd && (e=readdir(dd))) printf("  %s\n",e->d_name);
  return 0;
}
