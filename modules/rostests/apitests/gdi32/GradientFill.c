/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GradientFill
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define CX 64
#define CY 64
#define GUARD 0x4000 /* bytes of the section in front of the bits */
#define GUARD_BYTE 0xA5

static void FillVertex(TRIVERTEX *pv, LONG x, LONG y, COLORREF cr)
{
    pv->x = x;
    pv->y = y;
    pv->Red = GetRValue(cr) << 8;
    pv->Green = GetGValue(cr) << 8;
    pv->Blue = GetBValue(cr) << 8;
    pv->Alpha = 0;
}

/* A triangle of one colour whose top lies above the target: only rows of the
 * target may be filled */
static void Test_SolidTriangleAboveTarget(void)
{
    HANDLE hSection;
    PBYTE pjView;
    PULONG pulBits = NULL;
    BITMAPINFO bmi = {{0}};
    HDC hdc;
    HBITMAP hbm, hbmOld;
    TRIVERTEX vert[3];
    GRADIENT_TRIANGLE tri = {0, 1, 2};
    ULONG i, cBad;
    BOOL ret;

    hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, GUARD + CX * CY * 4, NULL);
    ok(hSection != NULL, "CreateFileMapping failed, error %lu\n", GetLastError());
    if (!hSection)
        return;
    pjView = MapViewOfFile(hSection, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ok(pjView != NULL, "MapViewOfFile failed, error %lu\n", GetLastError());
    if (!pjView)
    {
        CloseHandle(hSection);
        return;
    }
    memset(pjView, GUARD_BYTE, GUARD);

    /* Top-down: the rows above the bitmap are the guard bytes before it */
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = CX;
    bmi.bmiHeader.biHeight = -CY;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID *)&pulBits, hSection, GUARD);
    ok(hbm != NULL, "CreateDIBSection failed, error %lu\n", GetLastError());
    if (hbm)
    {
        hbmOld = SelectObject(hdc, hbm);
        memset(pulBits, 0, CX * CY * 4);

        FillVertex(&vert[0], 20, -40, RGB(0, 0, 255));
        FillVertex(&vert[1], 60, 40, RGB(0, 0, 255));
        FillVertex(&vert[2], 4, 60, RGB(0, 0, 255));
        ret = GradientFill(hdc, vert, 3, &tri, 1, GRADIENT_FILL_TRIANGLE);
        ok(ret, "GradientFill failed\n");
        GdiFlush();

        /* The part of the triangle on the bitmap is drawn... */
        ok(GetPixel(hdc, 20, 10) == RGB(0, 0, 255), "Pixel (20,10) is 0x%06lx\n", GetPixel(hdc, 20, 10));
        ok(GetPixel(hdc, 1, 1) == 0, "Pixel (1,1) is 0x%06lx\n", GetPixel(hdc, 1, 1));

        /* A triangle reaching past the left edge: the rows of it left of the
         * bitmap must not be filled (the line functions of 24 and 32 bpp on
         * x86 took such an empty line for one of 4 billion pixels) */
        FillVertex(&vert[0], -50, 0, RGB(255, 0, 0));
        FillVertex(&vert[1], -50, 60, RGB(255, 0, 0));
        FillVertex(&vert[2], 40, 30, RGB(255, 0, 0));
        ret = GradientFill(hdc, vert, 3, &tri, 1, GRADIENT_FILL_TRIANGLE);
        ok(ret, "GradientFill failed\n");
        GdiFlush();
        ok(GetPixel(hdc, 20, 30) == RGB(255, 0, 0), "Pixel (20,30) is 0x%06lx\n", GetPixel(hdc, 20, 30));

        /* ...and nothing in front of it */
        for (i = 0, cBad = 0; i < GUARD; i++)
        {
            if (pjView[i] != GUARD_BYTE)
                cBad++;
        }
        ok(cBad == 0, "%lu bytes in front of the bitmap were overwritten\n", cBad);

        SelectObject(hdc, hbmOld);
        DeleteObject(hbm);
    }
    DeleteDC(hdc);
    UnmapViewOfFile(pjView);
    CloseHandle(hSection);
}

/* The same on the screen, where the rows above it are memory of the display
 * driver or the engine: the call must stay on the screen */
static void Test_SolidTriangleAboveScreen(void)
{
    HDC hdc;
    TRIVERTEX vert[3];
    GRADIENT_TRIANGLE tri = {0, 1, 2};
    BOOL ret;

    hdc = GetDC(NULL);
    ok(hdc != NULL, "GetDC failed\n");
    if (!hdc)
        return;

    FillVertex(&vert[0], 20, -400, RGB(0, 0, 255));
    FillVertex(&vert[1], 60, 4, RGB(0, 0, 255));
    FillVertex(&vert[2], 4, 8, RGB(0, 0, 255));
    ret = GradientFill(hdc, vert, 3, &tri, 1, GRADIENT_FILL_TRIANGLE);
    ok(ret, "GradientFill failed\n");
    FillVertex(&vert[0], -50, 0, RGB(255, 0, 0));
    FillVertex(&vert[1], -50, 60, RGB(255, 0, 0));
    FillVertex(&vert[2], 40, 30, RGB(255, 0, 0));
    ret = GradientFill(hdc, vert, 3, &tri, 1, GRADIENT_FILL_TRIANGLE);
    ok(ret, "GradientFill failed\n");
    GdiFlush();

    ReleaseDC(NULL, hdc);
    RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* A huge rectangle on a hidden window, or on a window off the screen, draws
 * nothing, and must not need memory for its size either */
static void Test_HugeRect(DWORD dwStyle, int x, int y)
{
    HWND hwnd;
    HDC hdc;
    TRIVERTEX vert[2];
    GRADIENT_RECT gr = {0, 1};
    DWORD dwStart;
    BOOL ret;

    hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | dwStyle, x, y, 200, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;
    hdc = GetDC(hwnd);

    FillVertex(&vert[0], 0, 0, RGB(255, 0, 0));
    FillVertex(&vert[1], 30000, 30000, RGB(0, 0, 255));
    dwStart = GetTickCount();
    ret = GradientFill(hdc, vert, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    ok(ret, "0x%lx: GradientFill failed, error %lu\n", dwStyle, GetLastError());
    ret = GradientFill(hdc, vert, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    ok(ret, "0x%lx: GradientFill failed, error %lu\n", dwStyle, GetLastError());
    ok(GetTickCount() - dwStart < 2000, "0x%lx: GradientFill took %lu ms\n", dwStyle, GetTickCount() - dwStart);

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
}

START_TEST(GradientFill)
{
    Test_HugeRect(0, 0, 0);
    Test_HugeRect(WS_VISIBLE, -40000, -40000);
    Test_SolidTriangleAboveTarget();
    Test_SolidTriangleAboveScreen();
}
