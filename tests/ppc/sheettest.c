/* sheettest: a non-compositing Carbon sheet drawn on kEventWindowDrawContent, as Opera 7 does */
#include <Carbon/Carbon.h>
#include <stdio.h>
static WindowRef gSheet, gDoc;
static int gDraws, gUpdates;
static pascal OSStatus sheetEvt(EventHandlerCallRef h, EventRef e, void *ud) {
    UInt32 k = GetEventKind(e);
    if (k == kEventWindowDrawContent) {
        Rect r; GrafPtr old; GetPort(&old); SetPortWindowPort(gSheet);
        GetWindowPortBounds(gSheet, &r); RGBColor c = { 0x8000, 0x8000, 0xffff }; RGBForeColor(&c);
        InsetRect(&r, 20, 20); PaintRect(&r);
        RGBColor b = { 0, 0, 0 }; RGBForeColor(&b);
        MoveTo(40, 60); DrawString("\pHello from the sheet");
        QDFlushPortBuffer(GetWindowPort(gSheet), NULL); SetPort(old);
        printf("draw content #%d\n", ++gDraws); fflush(stdout);
        return noErr;
    }
    if (k == kEventWindowUpdate) { printf("update #%d\n", ++gUpdates); fflush(stdout); }
    return eventNotHandledErr;
}
static pascal void quitTimer(EventLoopTimerRef t, void *ud) { QuitAppModalLoopForWindow(gSheet); }
static pascal void showTimer(EventLoopTimerRef t, void *ud) {
    Rect s = { 0, 0, 200, 400 };
    EventTypeSpec ev[] = { { kEventClassWindow, kEventWindowDrawContent }, { kEventClassWindow, kEventWindowUpdate } };
    CreateNewWindow(kSheetWindowClass, kWindowStandardHandlerAttribute, &s, &gSheet);
    InstallWindowEventHandler(gSheet, NewEventHandlerUPP(sheetEvt), 2, ev, 0, NULL);
    ShowSheetWindow(gSheet, gDoc);
    printf("sheet shown, visible %d\n", IsWindowVisible(gSheet)); fflush(stdout);
    InstallEventLoopTimer(GetMainEventLoop(), 25, 0, NewEventLoopTimerUPP(quitTimer), NULL, NULL);
    RunAppModalLoopForWindow(gSheet);
    printf("modal loop done: draws %d updates %d\n", gDraws, gUpdates); fflush(stdout);
    QuitApplicationEventLoop();
}
int main(void) {
    Rect r = { 100, 100, 500, 700 };
    CreateNewWindow(kDocumentWindowClass, kWindowStandardHandlerAttribute | kWindowStandardDocumentAttributes, &r, &gDoc);
    SetWTitle(gDoc, "\psheettest");
    ShowWindow(gDoc);
    InstallEventLoopTimer(GetMainEventLoop(), 3, 0, NewEventLoopTimerUPP(showTimer), NULL, NULL);
    RunApplicationEventLoop();
    return 0;
}
