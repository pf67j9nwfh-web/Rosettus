#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
int main(void){
  time_t t=time(0); struct tm *l=localtime(&t); char b[100];
  strftime(b,sizeof b,"%Y-%m-%d %H:%M:%S %Z %z",l); printf("localtime: %s TZ=%s tzname=%s/%s\n",b,getenv("TZ")?getenv("TZ"):"(unset)",tzname[0],tzname[1]);
  CFTimeZoneRef z=CFTimeZoneCopySystem(); char n[100]=""; CFStringGetCString(CFTimeZoneGetName(z),n,100,kCFStringEncodingUTF8);
  printf("CF system tz: %s offset %g\n",n,CFTimeZoneGetSecondsFromGMT(z,CFAbsoluteTimeGetCurrent()));
  return 0;
}
