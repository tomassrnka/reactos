/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for PolyPatBlt
 * PROGRAMMERS:     Tomas Srnka
 */

#include "precomp.h"

typedef struct
{
    INT x;
    INT y;
    INT cx;
    INT cy;
    HBRUSH hBrush;
} TESTPATRECT;

static BOOL (WINAPI *pPolyPatBlt)(HDC, DWORD, PVOID, DWORD, DWORD);

#define SIDE 8
#define PIXELS (SIDE * SIDE)

/*
 * One 1x1 rect per record walks over the bitmap row by row with a white brush, and the last record
 * paints the last pixel black, which was white before the call. The DC's own brush is black, so a
 * record that is read with the wrong brush, a count that is cut short or the last record missing
 * each show in the pixels.
 */
static
void
Test_PolyPatBltCount(HDC hdc, ULONG cRects)
{
    TESTPATRECT *pRects;
    BOOL ret;
    ULONG i;
    BOOL abWhite[PIXELS] = { 0 };

    pRects = HeapAlloc(GetProcessHeap(), 0, cRects * sizeof(*pRects));
    if (!pRects)
    {
        skip("No memory for %lu rects\n", cRects);
        return;
    }

    for (i = 0; i < cRects; i++)
    {
        pRects[i].x = i % SIDE;
        pRects[i].y = (i / SIDE) % SIDE;
        pRects[i].cx = 1;
        pRects[i].cy = 1;
        pRects[i].hBrush = GetStockObject(WHITE_BRUSH);
        if (i + 1 < cRects)
            abWhite[i % PIXELS] = TRUE;
    }
    pRects[cRects - 1].x = SIDE - 1;
    pRects[cRects - 1].y = SIDE - 1;
    pRects[cRects - 1].hBrush = GetStockObject(BLACK_BRUSH);
    abWhite[PIXELS - 1] = FALSE;

    PatBlt(hdc, 0, 0, SIDE, SIDE, BLACKNESS);
    PatBlt(hdc, SIDE - 1, SIDE - 1, 1, 1, WHITENESS);
    SetLastError(0xdeadbeef);
    ret = pPolyPatBlt(hdc, PATCOPY, pRects, cRects, 0);
    ok(ret != FALSE, "PolyPatBlt(%lu) failed, error %lu\n", cRects, GetLastError());

    for (i = 0; i < PIXELS; i++)
    {
        ok(GetPixel(hdc, i % SIDE, i / SIDE) == (abWhite[i] ? RGB(255, 255, 255) : RGB(0, 0, 0)),
           "PolyPatBlt(%lu): wrong pixel at (%lu, %lu)\n", cRects, i % SIDE, i / SIDE);
    }

    HeapFree(GetProcessHeap(), 0, pRects);
}

static
BOOL
IsWow64Windows(void)
{
    BOOL (WINAPI *pIsWow64Process)(HANDLE, PBOOL);
    BOOL bWow64 = FALSE;

    pIsWow64Process = (PVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    return pIsWow64Process && pIsWow64Process(GetCurrentProcess(), &bWow64) && bWow64;
}

START_TEST(PolyPatBlt)
{
    HDC hdcScreen, hdc;
    HBITMAP hbmp, hbmpOld;
    ULONG cWrap1, cWrap2, i;
    ULONG acRects[] = { 1, 2, 3, 30, 31, 32, 33, 62, 63, 64, 65, 100, 2000, 3000, 4000, 70000 };

    pPolyPatBlt = (PVOID)GetProcAddress(GetModuleHandleA("gdi32.dll"), "PolyPatBlt");
    if (!pPolyPatBlt)
    {
        skip("PolyPatBlt is not exported\n");
        return;
    }

    /* Measured on Server 2008 R2 and Windows 10: the pixels of a 32 bit caller under WOW64 are wrong from two records up, differently on every run */
    if (!is_reactos() && IsWow64Windows())
    {
        skip("PolyPatBlt does not work for 32 bit callers under WOW64 on Windows\n");
        return;
    }

    hdcScreen = GetDC(NULL);
    hdc = CreateCompatibleDC(hdcScreen);
    hbmp = CreateCompatibleBitmap(hdcScreen, SIDE, SIDE);
    hbmpOld = hbmp ? SelectObject(hdc, hbmp) : NULL;
    if (!hdc || !hbmp || !hbmpOld)
    {
        skip("Could not create the target (%p, %p, %p)\n", hdc, hbmp, hbmpOld);
        if (hbmp)
            DeleteObject(hbmp);
        if (hdc)
            DeleteDC(hdc);
        ReleaseDC(NULL, hdcScreen);
        return;
    }
    SelectObject(hdc, GetStockObject(BLACK_BRUSH));

    for (i = 0; i < ARRAYSIZE(acRects); i++)
        Test_PolyPatBltCount(hdc, acRects[i]);

    /*
     * The batch buffer holds 1240 bytes. gdi32 formed the size of the records after the first in a
     * 16 bit variable: the first counts that wrap it (to a size the buffer holds) are 3278 for 20 byte
     * records and 2732 for 24 byte ones, and so on for each further multiple of 64 KiB.
     */
    cWrap1 = (0x10000 + sizeof(TESTPATRECT) - 1) / sizeof(TESTPATRECT) + 1;
    cWrap2 = (0x20000 + sizeof(TESTPATRECT) - 1) / sizeof(TESTPATRECT) + 1;
    for (i = 0; i < 3; i++)
    {
        Test_PolyPatBltCount(hdc, cWrap1 + i);
        Test_PolyPatBltCount(hdc, cWrap2 + i);
    }

    SelectObject(hdc, hbmpOld);
    DeleteObject(hbmp);
    DeleteDC(hdc);
    ReleaseDC(NULL, hdcScreen);
}
