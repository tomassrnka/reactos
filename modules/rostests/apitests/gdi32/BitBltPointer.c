/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Drawing on a window across the visible mouse pointer gives the pixels the operation asks for
 */

#include "precomp.h"

#define WIDTH  160
#define HEIGHT 120
#define PTR_X  80
#define PTR_Y  60

static HWND ghwnd;
static HDC ghdcWnd;
static HDC ghdcSrc;
static HBITMAP ghbmSrc, ghbmMask;
static HDC ghdcRead;
static HBITMAP ghbmRead;
static ULONG *gpulRead;
static int gOrgX, gOrgY;

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;

    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void
Pump(void)
{
    MSG msg;
    DWORD end = GetTickCount() + 60;

    do
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    } while ((LONG)(GetTickCount() - end) < 0);
}

static HBITMAP
CreateBits32(HDC hdc, ULONG **ppulBits)
{
    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), WIDTH, -HEIGHT, 1, 32, BI_RGB}};

    return CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID*)ppulBits, NULL, 0);
}

static void
FillBase(HDC hdc)
{
    HBRUSH hbr = CreateSolidBrush(RGB(0x20, 0x50, 0x80));
    RECT rc = {0, 0, WIDTH, HEIGHT};

    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
}

typedef void (*DRAWPROC)(HDC hdc);

static void
DrawBitBltFull(HDC hdc)
{
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
}

static void
DrawBitBltAround(HDC hdc)
{
    BitBlt(hdc, 50, 40, 60, 50, ghdcSrc, 50, 40, SRCCOPY);
}

static void
DrawBitBltPartial(HDC hdc)
{
    /* The pointer starts at the top left of this rectangle and runs out of it on the right and bottom */
    BitBlt(hdc, PTR_X - 4, PTR_Y - 4, 22, 14, ghdcSrc, 7, 9, SRCCOPY);
}

static void
DrawBitBltInvert(HDC hdc)
{
    BitBlt(hdc, 10, 10, 140, 100, ghdcSrc, 3, 4, SRCINVERT);
}

static void
DrawPatBltHatch(HDC hdc)
{
    HBRUSH hbr = CreateHatchBrush(HS_DIAGCROSS, RGB(0xF0, 0xE0, 0x10));
    HGDIOBJ hOld = SelectObject(hdc, hbr);

    SetBrushOrgEx(hdc, 3, 5, NULL);
    PatBlt(hdc, 0, 0, WIDTH, HEIGHT, PATCOPY);
    SelectObject(hdc, hOld);
    DeleteObject(hbr);
}

static void
DrawPatBltInvert(HDC hdc)
{
    PatBlt(hdc, 30, 20, 100, 80, DSTINVERT);
}

static void
DrawMaskBlt(HDC hdc)
{
    MaskBlt(hdc, 5, 6, WIDTH - 10, HEIGHT - 12, ghdcSrc, 5, 6, ghbmMask, 3, 1,
            MAKEROP4(SRCCOPY, 0x00AA0029 /* leave the destination */));
}

static void
DrawBitBltClipLeft(HDC hdc)
{
    HRGN hrgn = CreateRectRgn(0, 0, PTR_X - 8, HEIGHT);

    SelectClipRgn(hdc, hrgn);
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
}

static void
DrawBitBltClipThrough(HDC hdc)
{
    HRGN hrgn = CreateRectRgn(0, PTR_Y + 4, WIDTH, PTR_Y + 9);

    SelectClipRgn(hdc, hrgn);
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
}

static void
Check(const char *name, DRAWPROC proc)
{
    ULONG *pulRef;
    HBITMAP hbmRef;
    HDC hdcRef;
    HGDIOBJ hOld;
    int x, y, bad = 0, firstx = -1, firsty = -1;
    ULONG got = 0, expected = 0;

    hdcRef = CreateCompatibleDC(ghdcWnd);
    hbmRef = CreateCompatibleBitmap(ghdcWnd, WIDTH, HEIGHT);
    hOld = SelectObject(hdcRef, hbmRef);

    FillBase(ghdcWnd);
    FillBase(hdcRef);

    proc(ghdcWnd);
    proc(hdcRef);

    /* Reading the screen takes the pointer out of the way, so this is what lies under it */
    BitBlt(ghdcRead, 0, 0, WIDTH, HEIGHT, ghdcWnd, 0, 0, SRCCOPY);
    {
        HDC hdcRefRead = CreateCompatibleDC(ghdcWnd);
        HBITMAP hbmRefRead = CreateBits32(ghdcWnd, &pulRef);
        HGDIOBJ hOldRead = SelectObject(hdcRefRead, hbmRefRead);

        BitBlt(hdcRefRead, 0, 0, WIDTH, HEIGHT, hdcRef, 0, 0, SRCCOPY);
        GdiFlush();
        for (y = 0; y < HEIGHT; y++)
        {
            for (x = 0; x < WIDTH; x++)
            {
                ULONG a = gpulRead[y * WIDTH + x] & 0xFFFFFF;
                ULONG b = pulRef[y * WIDTH + x] & 0xFFFFFF;

                if (a != b)
                {
                    if (!bad)
                    {
                        firstx = x;
                        firsty = y;
                        got = a;
                        expected = b;
                    }
                    bad++;
                }
            }
        }
        SelectObject(hdcRefRead, hOldRead);
        DeleteObject(hbmRefRead);
        DeleteDC(hdcRefRead);
    }

    ok(bad == 0, "%s: %d pixels differ, first at (%d,%d): got %06lx, expected %06lx\n",
       name, bad, firstx, firsty, got, expected);

    SelectObject(hdcRef, hOld);
    DeleteObject(hbmRef);
    DeleteDC(hdcRef);
}

static BOOL
Setup(void)
{
    WNDCLASSW wc = {0};
    HDC hdcScreen;
    ULONG *pulSrc;
    BYTE abMask[HEIGHT * 20];
    int x, y;
    CURSORINFO ci = {sizeof(ci)};

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"BitBltPointerTest";
    RegisterClassW(&wc);

    hdcScreen = GetDC(NULL);
    if (GetDeviceCaps(hdcScreen, BITSPIXEL) < 24)
    {
        ReleaseDC(NULL, hdcScreen);
        skip("Needs at least 24 bits per pixel\n");
        return FALSE;
    }
    ReleaseDC(NULL, hdcScreen);

    gOrgX = 120;
    gOrgY = 120;
    ghwnd = CreateWindowExW(WS_EX_TOPMOST, L"BitBltPointerTest", L"BitBltPointer", WS_POPUP | WS_VISIBLE,
                            gOrgX, gOrgY, WIDTH, HEIGHT, NULL, NULL, wc.hInstance, NULL);
    if (!ghwnd)
    {
        skip("Could not create a window\n");
        return FALSE;
    }
    SetCursorPos(gOrgX + PTR_X, gOrgY + PTR_Y);
    Pump();

    GetCursorInfo(&ci);
    if (!(ci.flags & CURSOR_SHOWING))
    {
        skip("The mouse pointer is not showing\n");
        return FALSE;
    }

    ghdcWnd = GetDC(ghwnd);
    ghdcSrc = CreateCompatibleDC(ghdcWnd);
    ghbmSrc = CreateBits32(ghdcWnd, &pulSrc);
    SelectObject(ghdcSrc, ghbmSrc);
    for (y = 0; y < HEIGHT; y++)
        for (x = 0; x < WIDTH; x++)
            pulSrc[y * WIDTH + x] = ((x * 255 / WIDTH) << 16) | ((y * 255 / HEIGHT) << 8) | ((x ^ y) & 0xFF);
    GdiFlush();

    for (y = 0; y < HEIGHT; y++)
        for (x = 0; x < 20; x++)
            abMask[y * 20 + x] = (BYTE)(((x * 37) ^ (y * 91)) | (y & 1 ? 0x35 : 0xC8));
    ghbmMask = CreateBitmap(WIDTH, HEIGHT, 1, 1, abMask);

    ghdcRead = CreateCompatibleDC(ghdcWnd);
    ghbmRead = CreateBits32(ghdcWnd, &gpulRead);
    SelectObject(ghdcRead, ghbmRead);
    return TRUE;
}

START_TEST(BitBltPointer)
{
    if (!Setup())
        return;

    Check("BitBlt over the whole window", DrawBitBltFull);
    Check("BitBlt around the pointer", DrawBitBltAround);
    Check("BitBlt over part of the pointer", DrawBitBltPartial);
    Check("BitBlt SRCINVERT", DrawBitBltInvert);
    Check("PatBlt PATCOPY with a hatch brush and brush origin", DrawPatBltHatch);
    Check("PatBlt DSTINVERT", DrawPatBltInvert);
    Check("MaskBlt", DrawMaskBlt);
    Check("BitBlt with a clip that misses the pointer", DrawBitBltClipLeft);
    Check("BitBlt with a clip through the pointer", DrawBitBltClipThrough);

    DeleteDC(ghdcRead);
    DeleteObject(ghbmRead);
    DeleteDC(ghdcSrc);
    DeleteObject(ghbmSrc);
    DeleteObject(ghbmMask);
    ReleaseDC(ghwnd, ghdcWnd);
    DestroyWindow(ghwnd);
}
