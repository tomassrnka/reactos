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

/* Share (percent) of dark pixels in a screen rectangle, read back from the screen */
static unsigned DarkPercent(const RECT *Screen)
{
    LONG Width = Screen->right - Screen->left, Height = Screen->bottom - Screen->top;
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), Width, -Height, 1, 32, BI_RGB}};
    DWORD *Bits = NULL;
    LONG i, Dark = 0;
    HDC Dc = GetDC(NULL), Memory = CreateCompatibleDC(Dc);
    HBITMAP Bitmap = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, (void **)&Bits, NULL, 0);
    HGDIOBJ Old = Bitmap ? SelectObject(Memory, Bitmap) : NULL;
    BOOL Copied;
    /* A non-dark sentinel: a failed readback must not count as dark */
    for (i = 0; Bits && i < Width * Height; ++i) Bits[i] = 0xc0c0c0;
    Copied = Width > 0 && Height > 0 && Dc && Memory && Old &&
             BitBlt(Memory, 0, 0, Width, Height, Dc, Screen->left, Screen->top, SRCCOPY);
    GdiFlush();
    CHECK(Copied);
    for (i = 0; Copied && Bits && i < Width * Height; ++i)
    {
        DWORD c = Bits[i];
        if (((c & 0xff) + ((c >> 8) & 0xff) + ((c >> 16) & 0xff)) < 3 * 0x40) ++Dark;
    }
    if (Old) SelectObject(Memory, Old);
    if (Bitmap) DeleteObject(Bitmap);
    DeleteDC(Memory);
    ReleaseDC(NULL, Dc);
    return Copied && Width > 0 && Height > 0 ? (unsigned)(Dark * 100 / (Width * Height)) : 0;
}

static unsigned ClientBlack(HWND Window)
{
    RECT Client;
    POINT Origin = {10, 10};
    ClientToScreen(Window, &Origin);
    SetRect(&Client, Origin.x, Origin.y, Origin.x + 100, Origin.y + 60);
    return DarkPercent(&Client);
}

/* The menu bar is part of the frame: the new item's text must be painted */
static unsigned ItemDark(HWND Window, HMENU Menu, UINT Item)
{
    RECT Rect;
    if (!GetMenuItemRect(Window, Menu, Item, &Rect)) return 0;
    return DarkPercent(&Rect);
}

static void Position(WNDCLASSA *Class, int x, int y)
{
    HMENU Menu = CreateMenu();
    HWND Window;
    unsigned Before, After, Edit;
    Paints = 0;
    CHECK(AppendMenuA(Menu, MF_STRING, 100, "File"));
    Window = CreateWindowA(Class->lpszClassName, "framechanged", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           x, y, 320, 220, NULL, Menu, Class->hInstance, NULL);
    CHECK(Window != NULL);
    if (!Window) return;
    SetForegroundWindow(Window);
    UpdateWindow(Window);
    Pump();
    Before = ClientBlack(Window);

    /* Change the menu bar, as Total Commander does, then redraw the frame */
    CHECK(AppendMenuA(Menu, MF_STRING, 101, "Edit"));
    CHECK(DrawMenuBar(Window));
    Pump();
    After = ClientBlack(Window);
    Edit = ItemDark(Window, Menu, 1);
    printf("AT %d,%d client black before=%u%% after=%u%% edit-item dark=%u%% paints=%u\n",
           x, y, Before, After, Edit, Paints);
    CHECK(Before >= 95);
    /* Kept, or erased and painted again: never left erased */
    CHECK(After >= 95);
    /* The frame is still painted: the new item has its text */
    CHECK(Edit >= 2 && Edit < 60);

    CHECK(SetWindowPos(Window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                       SWP_NOZORDER | SWP_NOACTIVATE));
    Pump();
    After = ClientBlack(Window);
    Edit = ItemDark(Window, Menu, 1);
    printf("AT %d,%d after SWP_FRAMECHANGED client=%u%% edit-item dark=%u%% paints=%u\n", x, y, After, Edit, Paints);
    CHECK(After >= 95);
    CHECK(Edit >= 2 && Edit < 60);
    DestroyWindow(Window);
}

int main(void)
{
    WNDCLASSA Class = {0};
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
    /* Away from the screen origin, at two places: the regions involved are
     * window-relative and screen-relative */
    Position(&Class, 60, 60);
    Position(&Class, 230, 170);
    printf("FRAMECHANGED: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
