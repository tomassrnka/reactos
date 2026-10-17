/* First-window WoW64 integration fixture; also run unchanged on native i386.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>

static unsigned Failures, Created, Painted, Clicked, NcCreated;
static HWND Button;
static HINSTANCE Instance;
static const WCHAR ClassName[] = L"Wow64FirstWindow";
static const WCHAR TextLine[] = L"Hello from a 32-bit WoW64 program";
static const WCHAR Title[] = L"WoW64: 32-bit window on amd64";
#define CHECK(c) do { if (!(c)) { ++Failures; printf("FAIL line %u: %s error=%lu\n",__LINE__,#c,GetLastError()); fflush(stdout); } } while (0)

static LRESULT CALLBACK WindowProc(HWND Window, UINT Msg, WPARAM Wparam, LPARAM Lparam)
{
    switch (Msg)
    {
    case WM_NCCREATE:
    case WM_CREATE:
    {
        const CREATESTRUCTW *Create = (const CREATESTRUCTW *)Lparam;
        CHECK(Create != NULL);
        CHECK(Create->lpCreateParams == (void *)0x12345678);
        CHECK(Create->hInstance == Instance);
        CHECK(Create->lpszName && lstrcmpW(Create->lpszName,Title) == 0);
        if (Msg == WM_NCCREATE) { ++NcCreated; return DefWindowProcW(Window,Msg,Wparam,Lparam); }
        ++Created;
        Button = CreateWindowExW(0,L"BUTTON",L"Click to finish",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                 145,105,180,36,Window,(HMENU)100,Instance,NULL);
        CHECK(Button != NULL);
        CHECK(SetTimer(Window,1,90000,NULL) != 0);
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT Paint;
        RECT Client;
        HDC Dc = BeginPaint(Window,&Paint);
        CHECK(Dc != NULL);
        CHECK(GetClientRect(Window,&Client));
        CHECK(FillRect(Dc,&Client,(HBRUSH)(COLOR_WINDOW+1)) != 0);
        SetBkMode(Dc,TRANSPARENT);
        SetTextColor(Dc,RGB(20,40,90));
        CHECK(TextOutW(Dc,40,40,TextLine,ARRAYSIZE(TextLine)-1));
        CHECK(EndPaint(Window,&Paint));
        ++Painted;
        printf("PAINT created=%u nc=%u paints=%u button=%p\n",Created,NcCreated,Painted,Button); fflush(stdout);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(Wparam) == 100 && HIWORD(Wparam) == BN_CLICKED && (HWND)Lparam == Button)
        {
            ++Clicked;
            printf("CLICK received from button\n"); fflush(stdout);
            CHECK(DestroyWindow(Window));
        }
        return 0;
    case WM_TIMER:
        ++Failures;
        printf("FAIL: timed out waiting for external mouse click\n"); fflush(stdout);
        DestroyWindow(Window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(Window,Msg,Wparam,Lparam);
}

int main(void)
{
    WNDCLASSEXW Class = {sizeof(Class)};
    HWND Window;
    MSG Msg;
    BOOL Ret, Wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(),&Wow64);
    printf("START first-window pointer-size=%u wow64=%u\n",(unsigned)sizeof(void *),Wow64); fflush(stdout);
    Instance=GetModuleHandleW(NULL);
    Class.lpfnWndProc=WindowProc; Class.hInstance=Instance; Class.lpszClassName=ClassName;
    Class.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); Class.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    CHECK(RegisterClassExW(&Class) != 0);
    if (Failures) return 1;
    Window=CreateWindowExW(0,ClassName,Title,WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                           80,80,500,260,NULL,NULL,Instance,(void *)0x12345678);
    CHECK(Window != NULL);
    if (!Window) return 1;
    CHECK(UpdateWindow(Window));
    printf("READY window=%p button=%p\n",Window,Button); fflush(stdout);
    while ((Ret=GetMessageW(&Msg,NULL,0,0)) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    CHECK(Ret == 0);
    CHECK(Created == 1 && NcCreated == 1 && Painted > 0 && Clicked == 1);
    CHECK(UnregisterClassW(ClassName,Instance));
    printf("RESULT failures=%u created=%u nc=%u paints=%u clicks=%u\n",Failures,Created,NcCreated,Painted,Clicked);
    return Failures ? 1 : 0;
}
