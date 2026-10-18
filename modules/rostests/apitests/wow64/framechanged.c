/* A frame change in place (DrawMenuBar, SWP_FRAMECHANGED) must not leave the
 * client area erased without a WM_PAINT. win32k's non-client repaint erased
 * the whole visible window region and then dropped the update region it had
 * made for that, so a client painted once stayed blank: the 32-bit Total
 * Commander Lister lost its text this way after updating its menu bar.
 * Not WoW64-specific; the PE32 fixture also runs natively.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>

static unsigned Checks, Failures, Paints;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_PAINT)
    {
        PAINTSTRUCT Paint;
        RECT Client;
        HDC Dc = BeginPaint(Window, &Paint);
        GetClientRect(Window, &Client);
        FillRect(Dc, &Client, GetStockObject(BLACK_BRUSH));
        EndPaint(Window, &Paint);
        ++Paints;
        return 0;
    }
    return DefWindowProcA(Window, Message, Wparam, Lparam);
}

static void Pump(void)
{
    MSG Msg;
    DWORD Start = GetTickCount();
    while (GetTickCount() - Start < 500)
    {
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&Msg);
        Sleep(10);
    }
}

/* Share of black pixels in the client area, read back from the screen */
static unsigned BlackPercent(HWND Window)
{
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), 100, -60, 1, 32, BI_RGB}};
    DWORD *Bits = NULL;
    unsigned i, Black = 0;
    HDC Dc = GetDC(Window), Memory = CreateCompatibleDC(Dc);
    HBITMAP Bitmap = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, (void **)&Bits, NULL, 0);
    HGDIOBJ Old = Bitmap ? SelectObject(Memory, Bitmap) : NULL;
    BOOL Copied;
    /* A non-black sentinel: a failed readback must not count as black */
    for (i = 0; Bits && i < 100 * 60; ++i) Bits[i] = 0x123456;
    Copied = Dc && Memory && Old && BitBlt(Memory, 0, 0, 100, 60, Dc, 10, 10, SRCCOPY);
    GdiFlush();
    CHECK(Copied);
    for (i = 0; Copied && Bits && i < 100 * 60; ++i)
        if (!(Bits[i] & 0xffffff)) ++Black;
    if (Old) SelectObject(Memory, Old);
    if (Bitmap) DeleteObject(Bitmap);
    DeleteDC(Memory);
    ReleaseDC(Window, Dc);
    return Black * 100 / (100 * 60);
}

int main(void)
{
    WNDCLASSA Class = {0};
    HMENU Menu;
    HWND Window;
    unsigned Before, After;
    BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    CHECK(IsWow64Process(GetCurrentProcess(), &Wow64));
    printf("START framechanged wow64=%u\n", Wow64);
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.hbrBackground = GetStockObject(WHITE_BRUSH);
    Class.lpszClassName = "Wow64FrameChanged";
    CHECK(RegisterClassA(&Class) != 0);
    Menu = CreateMenu();
    CHECK(AppendMenuA(Menu, MF_STRING, 100, "File"));
    Window = CreateWindowA(Class.lpszClassName, "framechanged", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           60, 60, 320, 220, NULL, Menu, Class.hInstance, NULL);
    CHECK(Window != NULL);
    if (!Window) goto Done;
    SetForegroundWindow(Window);
    UpdateWindow(Window);
    Pump();
    Before = BlackPercent(Window);

    /* Change the menu bar, as Total Commander does, then redraw the frame */
    CHECK(AppendMenuA(Menu, MF_STRING, 101, "Edit"));
    CHECK(DrawMenuBar(Window));
    Pump();
    After = BlackPercent(Window);
    printf("CLIENT black before=%u%% after=%u%% paints=%u\n", Before, After, Paints);
    CHECK(Before >= 95);
    /* Kept, or erased and painted again: never left erased */
    CHECK(After >= 95);

    CHECK(SetWindowPos(Window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                       SWP_NOZORDER | SWP_NOACTIVATE));
    Pump();
    After = BlackPercent(Window);
    printf("CLIENT black after SWP_FRAMECHANGED=%u%% paints=%u\n", After, Paints);
    CHECK(After >= 95);
    DestroyWindow(Window);
Done:
    printf("FRAMECHANGED: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
