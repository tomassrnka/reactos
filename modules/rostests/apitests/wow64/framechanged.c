/* A frame change in place (DrawMenuBar, SWP_FRAMECHANGED) must not leave the
 * client area erased without a WM_PAINT. win32k's non-client repaint erased
 * the whole visible window region and then dropped the update region it had
 * made for that, so a client painted once stayed blank: the 32-bit Total
 * Commander Lister lost its text this way after updating its menu bar.
 * The region erased is the frame, in screen coordinates: used window-relative,
 * it erased border-wide strips of the client of a window away from the origin.
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

/* Share (percent) of dark pixels in a screen rectangle, read back from the screen;
 * Light, if given, receives the number of pixels that are not dark */
static unsigned DarkCount(const RECT *Screen, LONG *Light)
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
    if (Light) *Light = Copied ? Width * Height - Dark : -1;
    return Copied && Width > 0 && Height > 0 ? (unsigned)(Dark * 100 / (Width * Height)) : 0;
}

static unsigned DarkPercent(const RECT *Screen)
{
    return DarkCount(Screen, NULL);
}

/* Pixels of the whole client area (as far as it is in the work area) that are
 * not painted. The region the frame repaint erased was window-relative but used
 * as screen coordinates, which left border-wide strips of the client erased
 * wherever the window was not at the screen origin; the corner sample above
 * cannot see them. */
static LONG ClientLight(HWND Window)
{
    RECT Client, Work;
    LONG Light;
    GetClientRect(Window, &Client);
    MapWindowPoints(Window, NULL, (POINT *)&Client, 2);
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &Work, 0))
        SetRect(&Work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    IntersectRect(&Client, &Client, &Work);
    DarkCount(&Client, &Light);
    return Light;
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

static void Position(WNDCLASSA *Class, int x, int y, int Width, int Height, BOOL Maximize)
{
    HMENU Menu = CreateMenu();
    HWND Window;
    unsigned Before, After, Edit, Painted;
    LONG Initial, Light;
    Paints = 0;
    CHECK(AppendMenuA(Menu, MF_STRING, 100, "File"));
    Window = CreateWindowA(Class->lpszClassName, "framechanged", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           x, y, Width, Height, NULL, Menu, Class->hInstance, NULL);
    CHECK(Window != NULL);
    if (!Window) return;
    if (Maximize) ShowWindow(Window, SW_MAXIMIZE);
    SetForegroundWindow(Window);
    UpdateWindow(Window);
    Pump();
    Before = ClientBlack(Window);
    /* A few stray pixels may be there before any frame change; the frame
     * changes below must not add a single one */
    Initial = ClientLight(Window);
    printf("AT %d,%d %dx%d%s client light pixels before=%ld\n", x, y, Width, Height,
           Maximize ? " maximized" : "", Initial);
    CHECK(Initial >= 0 && Initial <= 8);
    /* Kept: not erased and painted again either */
    Painted = Paints;

    /* Change the menu bar, as Total Commander does, then redraw the frame */
    CHECK(AppendMenuA(Menu, MF_STRING, 101, "Edit"));
    CHECK(DrawMenuBar(Window));
    Pump();
    After = ClientBlack(Window);
    Edit = ItemDark(Window, Menu, 1);
    Light = ClientLight(Window);
    printf("AT %d,%d client black before=%u%% after=%u%% light pixels=%ld edit-item dark=%u%% paints=%u\n",
           x, y, Before, After, Light, Edit, Paints);
    CHECK(Light >= 0 && Light <= Initial);
    CHECK(Paints == Painted);
    CHECK(Before >= 95);
    /* Kept: never left erased */
    CHECK(After >= 95);
    /* The frame is still painted: the new item has its text */
    CHECK(Edit >= 2 && Edit < 60);

    CHECK(SetWindowPos(Window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                       SWP_NOZORDER | SWP_NOACTIVATE));
    Pump();
    After = ClientBlack(Window);
    Edit = ItemDark(Window, Menu, 1);
    Light = ClientLight(Window);
    printf("AT %d,%d after SWP_FRAMECHANGED client=%u%% light pixels=%ld edit-item dark=%u%% paints=%u\n",
           x, y, After, Light, Edit, Paints);
    CHECK(Light >= 0 && Light <= Initial);
    CHECK(Paints == Painted);
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
    /* Keep the pointer out of the client areas sampled */
    SetCursorPos(0, 0);
    CHECK(RegisterClassA(&Class) != 0);
    /* Away from the screen origin, at two places: the regions involved are
     * window-relative and screen-relative */
    Position(&Class, 60, 60, 320, 220, FALSE);
    Position(&Class, 230, 170, 320, 220, FALSE);
    /* Moved by less than its size: the misplaced strips fall inside the client */
    Position(&Class, 100, 100, 400, 300, FALSE);
    /* Maximized, the window sits at minus the border width */
    Position(&Class, 100, 100, 400, 300, TRUE);
    printf("FRAMECHANGED: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
