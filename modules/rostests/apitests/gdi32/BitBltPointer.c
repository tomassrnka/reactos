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

/* The transform swaps the corners of the rectangle: left > right (and top > bottom) when it reaches the pointer code */
static void
DrawPatBltMirrored(HDC hdc)
{
    XFORM xf = {-1.0f, 0.0f, 0.0f, 1.0f, (FLOAT)WIDTH, 0.0f};
    int oldMode = SetGraphicsMode(hdc, GM_ADVANCED);

    SetWorldTransform(hdc, &xf);
    PatBlt(hdc, 20, 10, 120, 90, DSTINVERT);
    ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);
    SetGraphicsMode(hdc, oldMode);
}

static void
DrawPatBltMirroredBoth(HDC hdc)
{
    XFORM xf = {-1.0f, 0.0f, 0.0f, -1.0f, (FLOAT)WIDTH, (FLOAT)HEIGHT};
    int oldMode = SetGraphicsMode(hdc, GM_ADVANCED);

    SetWorldTransform(hdc, &xf);
    PatBlt(hdc, 30, 20, 100, 80, DSTINVERT);
    ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);
    SetGraphicsMode(hdc, oldMode);
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
DrawCaptureBlt(HDC hdc)
{
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY | CAPTUREBLT);
}

static void
DrawCaptureBltAround(HDC hdc)
{
    BitBlt(hdc, 50, 40, 60, 50, ghdcSrc, 9, 3, SRCCOPY | CAPTUREBLT);
}

static void
DrawStretchSameSize(HDC hdc)
{
    StretchBlt(hdc, 20, 10, 120, 90, ghdcSrc, 20, 10, 120, 90, SRCCOPY);
}

static void
DrawStretchSameSizeInvert(HDC hdc)
{
    StretchBlt(hdc, 30, 20, 100, 80, ghdcSrc, 4, 7, 100, 80, SRCINVERT);
}

static void
DrawStretchDouble(HDC hdc)
{
    StretchBlt(hdc, 10, 10, 140, 100, ghdcSrc, 0, 0, 70, 50, SRCCOPY);
}

static void
DrawStretchShrink(HDC hdc)
{
    StretchBlt(hdc, 30, 30, 90, 60, ghdcSrc, 0, 0, 160, 120, SRCCOPY);
}

static void
DrawAlphaBlend(HDC hdc)
{
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 128, 0};

    AlphaBlend(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, WIDTH, HEIGHT, bf);
}

static void
DrawAlphaBlendAround(HDC hdc)
{
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 90, 0};

    AlphaBlend(hdc, PTR_X - 12, PTR_Y - 9, 40, 50, ghdcSrc, 17, 5, 40, 50, bf);
}

static void
DrawAlphaBlendScaled(HDC hdc)
{
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 128, 0};

    AlphaBlend(hdc, 20, 20, 120, 80, ghdcSrc, 0, 0, 60, 40, bf);
}

static void
DrawTransparentBlt(HDC hdc)
{
    /* the colour of a source pixel, so that part of the source is skipped */
    TransparentBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, WIDTH, HEIGHT, GetPixel(ghdcSrc, WIDTH / 2, HEIGHT / 2));
}

static void
DrawTransparentBltAround(HDC hdc)
{
    TransparentBlt(hdc, PTR_X - 12, PTR_Y - 9, 40, 50, ghdcSrc, 17, 5, 40, 50, GetPixel(ghdcSrc, 20, 10));
}

static void
DrawAlphaBlendSourceOutside(HDC hdc)
{
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 128, 0};

    /* the source rectangle runs out of the source bitmap: the engine rejects the call, nothing may be drawn */
    AlphaBlend(hdc, PTR_X - 12, PTR_Y - 9, 40, 50, ghdcSrc, WIDTH - 25, HEIGHT - 20, 40, 50, bf);
}

static void
DrawTransparentBltSourceOutside(HDC hdc)
{
    TransparentBlt(hdc, PTR_X - 12, PTR_Y - 9, 40, 50, ghdcSrc, WIDTH - 25, HEIGHT - 20, 40, 50, RGB(1, 2, 3));
}

static int gShift, gDelta, gKind;

static void
DrawStretchShift(HDC hdc)
{
    /* One scaled stretch with a flip through a clip one pixel wide (kinds 0 to 2) or high (kinds 3 and 4): the clip line lies
     * next to or inside the pointer and the destination ends at it or near it. The engine moves the destination of such a
     * stretch by a pixel, so for some of them it writes the line although the destination as given does not contain it.
     * Each one is checked alone: a later call that hides the pointer would repair what an earlier one left wrong. */
    int c = PTR_X + gShift, cy = PTR_Y + gShift;
    HRGN hrgn;

    if (gKind < 3)
        hrgn = CreateRectRgn(c, PTR_Y, c + 1, PTR_Y + 24);
    else
        hrgn = CreateRectRgn(PTR_X, cy, PTR_X + 24, cy + 1);

    SelectClipRgn(hdc, hrgn);
    switch (gKind)
    {
        case 0: StretchBlt(hdc, c + gDelta, PTR_Y + 2, -8, 20, ghdcSrc, 3, 2, 5, 10, SRCCOPY); break;
        case 1: StretchBlt(hdc, c + gDelta, PTR_Y + 4, -8, 20, ghdcSrc, 9, 2, -5, 10, SRCCOPY); break;
        case 2: StretchBlt(hdc, c + gDelta, PTR_Y + 6, 8, 20, ghdcSrc, 9, 2, -5, 10, SRCCOPY); break;
        case 3: StretchBlt(hdc, PTR_X + 2, cy + gDelta, 10, -8, ghdcSrc, 3, 14, 5, -4, SRCCOPY); break;
        case 4: StretchBlt(hdc, PTR_X + 14, cy + gDelta, -10, -8, ghdcSrc, 9, 14, -5, -4, SRCCOPY); break;
    }
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
}

static void
DrawEmptyClip(HDC hdc)
{
    HRGN hrgn = CreateRectRgn(0, 0, 0, 0);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 128, 0};

    SelectClipRgn(hdc, hrgn);
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
    AlphaBlend(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, WIDTH, HEIGHT, bf);
    TransparentBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, WIDTH, HEIGHT, RGB(1, 2, 3));
    StretchBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, WIDTH / 2, HEIGHT / 2, SRCCOPY);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
}

static void
DrawClipTwoRects(HDC hdc)
{
    /* A clip with a gap through the pointer, so the clip object has several rectangles */
    HRGN hrgn = CreateRectRgn(0, 0, PTR_X + 10, HEIGHT);
    HRGN hrgn2 = CreateRectRgn(PTR_X + 20, 0, WIDTH, HEIGHT);

    CombineRgn(hrgn, hrgn, hrgn2, RGN_OR);
    SelectClipRgn(hdc, hrgn);
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
    DeleteObject(hrgn2);
}

static void
DrawClipHole(HDC hdc)
{
    HRGN hrgn = CreateRectRgn(0, 0, WIDTH, HEIGHT);
    HRGN hrgn2 = CreateRectRgn(PTR_X - 6, PTR_Y - 6, PTR_X + 10, PTR_Y + 30);

    CombineRgn(hrgn, hrgn, hrgn2, RGN_DIFF);
    SelectClipRgn(hdc, hrgn);
    PatBlt(hdc, 0, 0, WIDTH, HEIGHT, DSTINVERT);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hrgn);
    DeleteObject(hrgn2);
}

static void
DrawClipChanges(HDC hdc)
{
    /* One clip that holds the pointer, then one that does not, then none */
    HRGN hrgn = CreateRectRgn(PTR_X - 20, PTR_Y - 20, PTR_X + 40, PTR_Y + 40);
    HRGN hrgn2 = CreateRectRgn(0, 0, PTR_X - 20, HEIGHT);

    SelectClipRgn(hdc, hrgn);
    BitBlt(hdc, 0, 0, WIDTH, HEIGHT, ghdcSrc, 0, 0, SRCCOPY);
    SelectClipRgn(hdc, hrgn2);
    PatBlt(hdc, 0, 0, WIDTH, HEIGHT, DSTINVERT);
    SelectClipRgn(hdc, NULL);
    BitBlt(hdc, PTR_X - 10, PTR_Y - 10, 30, 30, ghdcSrc, 40, 40, SRCINVERT);
    DeleteObject(hrgn);
    DeleteObject(hrgn2);
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

/* Move the pointer in steps that overlap, touch and miss the old place, then compare the window with what was drawn: a
 * wrong saved background under the pointer shows as pixels that differ after the read, which takes the pointer off */
static void
CheckMoves(const char *name)
{
    static const int steps[][2] =
    {
        {1, 0}, {2, 1}, {-3, 2}, {0, -4}, {5, 5}, {-6, 3}, {31, 0}, {-31, 1}, {0, 33}, {1, -33}, {40, 20}, {-40, -20},
        {64, 0}, {-64, 10}, {0, 50}, {100, -20}, {-100, 25}, {3, 3}, {-1, -1}, {20, -30}, {-20, 30}, {70, 0}, {-70, 0}
    };
    ULONG *pulRef;
    HBITMAP hbmRef;
    HDC hdcRef, hdcRefRead;
    HGDIOBJ hOld, hOldRead;
    int i, x = PTR_X, y = PTR_Y, bad = 0, firstx = -1, firsty = -1, px, py;
    ULONG got = 0, expected = 0;

    hdcRef = CreateCompatibleDC(ghdcWnd);
    hbmRef = CreateCompatibleBitmap(ghdcWnd, WIDTH, HEIGHT);
    hOld = SelectObject(hdcRef, hbmRef);
    FillBase(ghdcWnd);
    FillBase(hdcRef);
    DrawBitBltFull(ghdcWnd);
    DrawBitBltFull(hdcRef);

    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    {
        x += steps[i][0];
        y += steps[i][1];
        if (x < 4) x = 4;
        if (y < 4) y = 4;
        if (x > WIDTH - 36) x = WIDTH - 36;
        if (y > HEIGHT - 36) y = HEIGHT - 36;
        SetCursorPos(gOrgX + x, gOrgY + y);
        Sleep(15);
    }
    Pump();

    BitBlt(ghdcRead, 0, 0, WIDTH, HEIGHT, ghdcWnd, 0, 0, SRCCOPY);
    hdcRefRead = CreateCompatibleDC(ghdcWnd);
    {
        HBITMAP hbmRefRead = CreateBits32(ghdcWnd, &pulRef);

        hOldRead = SelectObject(hdcRefRead, hbmRefRead);
        BitBlt(hdcRefRead, 0, 0, WIDTH, HEIGHT, hdcRef, 0, 0, SRCCOPY);
        GdiFlush();
        for (py = 0; py < HEIGHT; py++)
        {
            for (px = 0; px < WIDTH; px++)
            {
                ULONG a = gpulRead[py * WIDTH + px] & 0xFFFFFF;
                ULONG b = pulRef[py * WIDTH + px] & 0xFFFFFF;

                if (a != b)
                {
                    if (!bad)
                    {
                        firstx = px;
                        firsty = py;
                        got = a;
                        expected = b;
                    }
                    bad++;
                }
            }
        }
        SelectObject(hdcRefRead, hOldRead);
        DeleteObject(hbmRefRead);
    }
    DeleteDC(hdcRefRead);
    ok(bad == 0, "%s: %d pixels differ, first at (%d,%d): got %06lx, expected %06lx\n",
       name, bad, firstx, firsty, got, expected);

    SelectObject(hdcRef, hOld);
    DeleteObject(hbmRef);
    DeleteDC(hdcRef);
    SetCursorPos(gOrgX + PTR_X, gOrgY + PTR_Y);
    Pump();
}

/* Only with BITBLTPOINTER_MODESWITCH set: a display mode of another depth, or else a smaller size, and back, with the pointer moving. The bitmaps
 * the pointer code keeps for the old depth must not be used for the new one. */
static void
CheckModeSwitch(void)
{
    DEVMODEW cur, dm;
    DWORD i;
    BOOL bFound = FALSE;

    if (!GetEnvironmentVariableA("BITBLTPOINTER_MODESWITCH", NULL, 0))
        return;

    ZeroMemory(&cur, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur))
        return;

    for (i = 0; ; i++)
    {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(NULL, i, &dm))
            break;
        if (dm.dmBitsPerPel >= 16 && dm.dmPelsWidth >= 800 && dm.dmPelsHeight >= 600 &&
            (dm.dmBitsPerPel != cur.dmBitsPerPel || (dm.dmPelsWidth < cur.dmPelsWidth && dm.dmPelsHeight < cur.dmPelsHeight)))
        {
            bFound = TRUE;
            break;
        }
    }
    if (!bFound || ChangeDisplaySettingsW(&dm, 0) != DISP_CHANGE_SUCCESSFUL)
    {
        skip("No other depth\n");
        return;
    }

    ReleaseDC(ghwnd, ghdcWnd);
    ghdcWnd = GetDC(ghwnd);
    SetCursorPos(gOrgX + PTR_X, gOrgY + PTR_Y);
    Pump();
    CheckMoves("Pointer moves after a display mode switch");

    ChangeDisplaySettingsW(&cur, 0);
    ReleaseDC(ghwnd, ghdcWnd);
    ghdcWnd = GetDC(ghwnd);
    SetCursorPos(gOrgX + PTR_X, gOrgY + PTR_Y);
    Pump();
    CheckMoves("Pointer moves after the switch back");
}

/* A scaled stretch whose destination runs from x = 0 to the end of the coordinate range, with the pointer at the top left of the
 * screen and a clip that is the column x = 0: the engine moves the destination of a flipped stretch by a pixel, which at the end
 * of the range wraps, and the hazard of the call must not wrap with it. The screen DC is used (its origin is 0) and compared
 * with a copy of the same screen part that gets the same calls in memory. */
static void
CheckRangeEnd(const char *name)
{
    HDC hdcScr = GetDC(NULL), hdcCopy = CreateCompatibleDC(hdcScr);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScr, 64, 64);
    HDC hdcRead = CreateCompatibleDC(hdcScr), hdcRefRead = CreateCompatibleDC(hdcScr);
    HGDIOBJ hOld = SelectObject(hdcCopy, hbm), hOldRead, hOldRef;
    HRGN hrgn = CreateRectRgn(0, 0, 1, 40);
    ULONG *pulA, *pulB;
    HBITMAP hbmA = CreateBits32(hdcScr, &pulA), hbmB = CreateBits32(hdcScr, &pulB);
    int i, x, y, bad = 0;
    static const struct { int x, w, sx, sw; } calls[] =
    {
        {0x7fffffff, -0x7fffffff, 9, -5},
        {0x7fffffff, -0x7fffffff, 3, 5},
        {0, 0x7fffffff, 3, 5},
        {-0x7fffffff, 0x7fffffff, 3, 5},
    };

    SetCursorPos(0, 0);
    Pump();
    hOldRead = SelectObject(hdcRead, hbmA);
    hOldRef = SelectObject(hdcRefRead, hbmB);

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
    {
        /* what the screen part shows now, taken with the pointer off it */
        BitBlt(hdcCopy, 0, 0, 64, 64, hdcScr, 0, 0, SRCCOPY);
        SelectClipRgn(hdcScr, hrgn);
        SelectClipRgn(hdcCopy, hrgn);
        StretchBlt(hdcScr, calls[i].x, 4, calls[i].w, 20, ghdcSrc, calls[i].sx, 2, calls[i].sw, 10, SRCCOPY);
        StretchBlt(hdcCopy, calls[i].x, 4, calls[i].w, 20, ghdcSrc, calls[i].sx, 2, calls[i].sw, 10, SRCCOPY);
        SelectClipRgn(hdcScr, NULL);
        SelectClipRgn(hdcCopy, NULL);

        BitBlt(hdcRead, 0, 0, 64, 64, hdcScr, 0, 0, SRCCOPY);
        BitBlt(hdcRefRead, 0, 0, 64, 64, hdcCopy, 0, 0, SRCCOPY);
        GdiFlush();
        bad = 0;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                if ((pulA[y * WIDTH + x] & 0xFFFFFF) != (pulB[y * WIDTH + x] & 0xFFFFFF))
                    bad++;
        ok(bad == 0, "%s %d: %d pixels differ\n", name, i, bad);
    }

    SelectObject(hdcRead, hOldRead);
    SelectObject(hdcRefRead, hOldRef);
    SelectObject(hdcCopy, hOld);
    DeleteObject(hbm);
    DeleteObject(hbmA);
    DeleteObject(hbmB);
    DeleteObject(hrgn);
    DeleteDC(hdcRead);
    DeleteDC(hdcRefRead);
    DeleteDC(hdcCopy);
    ReleaseDC(NULL, hdcScr);
    SetCursorPos(gOrgX + PTR_X, gOrgY + PTR_Y);
    Pump();
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
    Check("PatBlt DSTINVERT under a mirroring transform", DrawPatBltMirrored);
    Check("PatBlt DSTINVERT under a transform that mirrors both axes", DrawPatBltMirroredBoth);
    Check("MaskBlt", DrawMaskBlt);
    Check("BitBlt with a clip that misses the pointer", DrawBitBltClipLeft);
    Check("BitBlt with a clip through the pointer", DrawBitBltClipThrough);
    Check("BitBlt with CAPTUREBLT", DrawCaptureBlt);
    Check("BitBlt with CAPTUREBLT around the pointer", DrawCaptureBltAround);
    Check("StretchBlt without scaling", DrawStretchSameSize);
    Check("StretchBlt without scaling, SRCINVERT", DrawStretchSameSizeInvert);
    Check("StretchBlt to twice the size", DrawStretchDouble);
    Check("StretchBlt to a smaller size", DrawStretchShrink);
    Check("AlphaBlend without scaling", DrawAlphaBlend);
    Check("AlphaBlend around the pointer", DrawAlphaBlendAround);
    Check("AlphaBlend with scaling", DrawAlphaBlendScaled);
    Check("TransparentBlt without scaling", DrawTransparentBlt);
    Check("TransparentBlt around the pointer", DrawTransparentBltAround);
    Check("AlphaBlend with a source outside the source bitmap", DrawAlphaBlendSourceOutside);
    Check("TransparentBlt with a source outside the source bitmap", DrawTransparentBltSourceOutside);
    for (gShift = -1; gShift <= 2; gShift++)
        for (gDelta = -2; gDelta <= 2; gDelta++)
            for (gKind = 0; gKind < 5; gKind++)
                Check("StretchBlt with a negative width and a thin clip at the pointer", DrawStretchShift);
    Check("Every call with an empty clip", DrawEmptyClip);
    Check("BitBlt with a clip of two rectangles through the pointer", DrawClipTwoRects);
    Check("PatBlt with a clip that has a hole over the pointer", DrawClipHole);
    Check("Clip that changes between operations", DrawClipChanges);

    CheckMoves("Pointer moves leave the window as drawn");
    CheckRangeEnd("Stretch to the end of the coordinate range, pointer at the top left");
    CheckModeSwitch();

    DeleteDC(ghdcRead);
    DeleteObject(ghbmRead);
    DeleteDC(ghdcSrc);
    DeleteObject(ghbmSrc);
    DeleteObject(ghbmMask);
    ReleaseDC(ghwnd, ghdcWnd);
    DestroyWindow(ghwnd);
}
