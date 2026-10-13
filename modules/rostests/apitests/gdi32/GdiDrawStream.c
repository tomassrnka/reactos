/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for GdiDrawStream
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* The draw stream layout uxtheme passes (sdk/include/reactos/undocgdi.h) */
typedef struct _TEST_DRAW_STREAM
{
    DWORD   signature;
    DWORD   reserved;
    HDC     hDC;
    RECT    rcDest;
    DWORD   unknown1;
    HBITMAP hImage;
    DWORD   unknown2;
    RECT    rcClip;
    RECT    rcSrc;
    DWORD   drawOption;
    DWORD   leftSizingMargin;
    DWORD   rightSizingMargin;
    DWORD   topSizingMargin;
    DWORD   bottomSizingMargin;
    DWORD   crTransparent;
} TEST_DRAW_STREAM;

/* The layout Windows x64 takes: handles in 32-bit fields */
typedef struct _TEST_DRAW_STREAM32
{
    DWORD   signature;
    DWORD   reserved;
    DWORD   hDC;
    RECT    rcDest;
    DWORD   unknown1;
    DWORD   hImage;
    DWORD   unknown2;
    RECT    rcClip;
    RECT    rcSrc;
    DWORD   drawOption;
    DWORD   leftSizingMargin;
    DWORD   rightSizingMargin;
    DWORD   topSizingMargin;
    DWORD   bottomSizingMargin;
    DWORD   crTransparent;
} TEST_DRAW_STREAM32;

#define TEST_DS_TRUESIZE 0x20

typedef BOOL (WINAPI *PFN_GdiDrawStream)(HDC, ULONG, PVOID);
static PFN_GdiDrawStream pGdiDrawStream;
static BOOL s_bStream32;

static void
CallDrawStream(HDC hdc, const TEST_DRAW_STREAM *pds)
{
    TEST_DRAW_STREAM32 ds32;

    if (!s_bStream32)
    {
        pGdiDrawStream(hdc, sizeof(*pds), (PVOID)pds);
        return;
    }

    ds32.signature = pds->signature;
    ds32.reserved = pds->reserved;
    ds32.hDC = HandleToUlong(pds->hDC);
    ds32.rcDest = pds->rcDest;
    ds32.unknown1 = pds->unknown1;
    ds32.hImage = HandleToUlong(pds->hImage);
    ds32.unknown2 = pds->unknown2;
    ds32.rcClip = pds->rcClip;
    ds32.rcSrc = pds->rcSrc;
    ds32.drawOption = pds->drawOption;
    ds32.leftSizingMargin = pds->leftSizingMargin;
    ds32.rightSizingMargin = pds->rightSizingMargin;
    ds32.topSizingMargin = pds->topSizingMargin;
    ds32.bottomSizingMargin = pds->bottomSizingMargin;
    ds32.crTransparent = pds->crTransparent;
    pGdiDrawStream(hdc, sizeof(ds32), &ds32);
}

#define SRC_CX 12
#define SRC_CY 6
#define DST_CX 40
#define DST_CY 20
#define WHITE 0xFFFFFFFF

static HBITMAP s_hbmSrc;

static HBITMAP
CreateDib32(INT cx, INT cy, PDWORD *ppBits)
{
    BITMAPINFO bmi;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (PVOID *)ppBits, NULL, 0);
}

static void
DrawTrueSize(HDC hdc)
{
    TEST_DRAW_STREAM ds;

    ZeroMemory(&ds, sizeof(ds));
    ds.signature = 0x44727753;
    ds.unknown1 = 1;
    ds.unknown2 = 9;
    ds.hDC = hdc;
    ds.hImage = s_hbmSrc;
    SetRect(&ds.rcDest, 1, 1, 1 + SRC_CX, 1 + SRC_CY);
    ds.rcClip = ds.rcDest;
    SetRect(&ds.rcSrc, 0, 0, SRC_CX, SRC_CY);
    ds.drawOption = TEST_DS_TRUESIZE;
    CallDrawStream(hdc, &ds);
    GdiFlush();
}

/* Returns the number of destination pixels that differ from the source image */
static INT
CountWrongPixels(PDWORD pdwDst)
{
    INT x, y, nWrong = 0;

    for (y = 0; y < DST_CY; ++y)
    {
        for (x = 0; x < DST_CX; ++x)
        {
            DWORD dwExpected = WHITE;
            if (x >= 1 && x < 1 + SRC_CX && y >= 1 && y < 1 + SRC_CY)
                dwExpected = 0x800000 | ((y - 1) << 8) | (x - 1);
            if (pdwDst[y * DST_CX + x] != dwExpected)
                ++nWrong;
        }
    }
    return nWrong;
}

typedef struct _THREAD_DATA
{
    HANDLE hStart;
    LONG nDraws;
    LONG nWrong;
    BOOL bSetupFailed;
} THREAD_DATA;

static DWORD WINAPI
DrawThread(PVOID pvParam)
{
    THREAD_DATA *pData = pvParam;
    PDWORD pdwDst;
    HBITMAP hbmDst = CreateDib32(DST_CX, DST_CY, &pdwDst);
    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ hbmOld;
    DWORD dwStart;

    WaitForSingleObject(pData->hStart, INFINITE);
    if (!hbmDst || !hdc)
    {
        pData->bSetupFailed = TRUE;
        if (hdc) DeleteDC(hdc);
        if (hbmDst) DeleteObject(hbmDst);
        return 0;
    }
    hbmOld = SelectObject(hdc, hbmDst);
    dwStart = GetTickCount();
    while (GetTickCount() - dwStart < 2000)
    {
        FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
        DrawTrueSize(hdc);
        ++pData->nDraws;
        if (CountWrongPixels(pdwDst))
            ++pData->nWrong;
    }

    SelectObject(hdc, hbmOld);
    DeleteDC(hdc);
    DeleteObject(hbmDst);
    return 0;
}

static void
Test_SharedSource(void)
{
    THREAD_DATA Data[8];
    HANDLE hThreads[8];
    HANDLE hStart = CreateEventW(NULL, TRUE, FALSE, NULL);
    LONG nDraws = 0, nWrong = 0;
    DWORD dwWait;
    UINT i, nThreads = 0;

    ok(hStart != NULL, "CreateEventW failed (%lu)\n", GetLastError());
    if (!hStart)
        return;

    for (i = 0; i < _countof(hThreads); ++i)
    {
        Data[nThreads].hStart = hStart;
        Data[nThreads].nDraws = Data[nThreads].nWrong = 0;
        Data[nThreads].bSetupFailed = FALSE;
        hThreads[nThreads] = CreateThread(NULL, 0, DrawThread, &Data[nThreads], 0, NULL);
        ok(hThreads[nThreads] != NULL, "CreateThread failed (%lu)\n", GetLastError());
        if (hThreads[nThreads])
            ++nThreads;
    }
    SetEvent(hStart);

    /* Every worker is joined before its data goes away */
    for (i = 0; i < nThreads; ++i)
    {
        dwWait = WaitForSingleObject(hThreads[i], INFINITE);
        ok(dwWait == WAIT_OBJECT_0, "WaitForSingleObject returned %lu\n", dwWait);
        CloseHandle(hThreads[i]);
        ok(!Data[i].bSetupFailed, "Thread %u could not create its bitmap or DC\n", i);
        ok(Data[i].nDraws > 0, "Thread %u did not draw\n", i);
        nDraws += Data[i].nDraws;
        nWrong += Data[i].nWrong;
    }
    CloseHandle(hStart);

    trace("%u threads drawing the same image: %ld draws, %ld wrong\n", nThreads, nDraws, nWrong);
    ok(nDraws > 0, "No draws\n");
    ok(nWrong == 0, "%ld of %ld draws from %u threads are wrong\n", nWrong, nDraws, nThreads);
}

START_TEST(GdiDrawStream)
{
    PDWORD pdwSrc, pdwDst;
    HBITMAP hbmDst;
    HGDIOBJ hbmOld, hbmOther;
    HDC hdc, hdcOther;
    INT x, y;

    pGdiDrawStream = (PFN_GdiDrawStream)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "GdiDrawStream");
    if (!pGdiDrawStream)
    {
        skip("GdiDrawStream is not exported\n");
        return;
    }

    s_hbmSrc = CreateDib32(SRC_CX, SRC_CY, &pdwSrc);
    for (y = 0; s_hbmSrc && y < SRC_CY; ++y)
    {
        for (x = 0; x < SRC_CX; ++x)
            pdwSrc[y * SRC_CX + x] = 0x800000 | (y << 8) | x;
    }

    hbmDst = CreateDib32(DST_CX, DST_CY, &pdwDst);
    hdc = CreateCompatibleDC(NULL);
    ok(s_hbmSrc && hbmDst && hdc, "Cannot create the bitmaps or the DC\n");
    if (!s_hbmSrc || !hbmDst || !hdc)
    {
        if (hdc) DeleteDC(hdc);
        if (hbmDst) DeleteObject(hbmDst);
        if (s_hbmSrc) DeleteObject(s_hbmSrc);
        return;
    }
    hbmOld = SelectObject(hdc, hbmDst);

    FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
    DrawTrueSize(hdc);
    if (sizeof(PVOID) == 8 && CountWrongPixels(pdwDst) == SRC_CX * SRC_CY)
    {
        /* Windows x64 takes the handles in 32-bit fields, ReactOS x64 in pointer-sized ones */
        s_bStream32 = TRUE;
        FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
        DrawTrueSize(hdc);
        trace("Using the stream layout with 32-bit handle fields\n");
    }
    ok_int(CountWrongPixels(pdwDst), 0);

    /* The image is drawn while an application has it selected into another DC */
    hdcOther = CreateCompatibleDC(NULL);
    hbmOther = SelectObject(hdcOther, s_hbmSrc);
    ok(hbmOther != NULL, "SelectObject failed\n");
    FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
    DrawTrueSize(hdc);
    ok_int(CountWrongPixels(pdwDst), 0);
    ok(SelectObject(hdcOther, hbmOther) == s_hbmSrc, "The image is no longer selected into the other DC\n");
    DeleteDC(hdcOther);

    /* Several threads draw the same image at once */
    Test_SharedSource();

    SelectObject(hdc, hbmOld);
    DeleteDC(hdc);
    DeleteObject(hbmDst);
    DeleteObject(s_hbmSrc);
}
