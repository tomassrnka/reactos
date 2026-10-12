/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for GdiDrawStream
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <string.h>

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

typedef struct _MARGIN_CASE
{
    INT Left, Right, Top, Bottom;   /* sizing margins */
    INT cx, cy;                     /* destination size */
    const char *pszRow;             /* source columns of the first destination row, '.' untouched */
    const char *pszColumn;          /* source rows of the first destination column */
} MARGIN_CASE;

/* Measured on Windows Server 2008 R2 and Windows 10 (x64 and WOW64) with the 12x6 image */
static const MARGIN_CASE s_MarginCases[] =
{
    { 3, 3, 0, 0,  1, 6, ".a.", NULL },
    { 3, 3, 0, 0,  2, 6, ".1a.", NULL },
    { 3, 3, 0, 0,  3, 6, ".19b.", NULL },
    { 3, 3, 0, 0,  4, 6, ".029b.", NULL },
    { 3, 3, 0, 0,  5, 6, ".029ab.", NULL },
    { 3, 3, 0, 0,  6, 6, ".0129ab.", NULL },
    { 3, 3, 0, 0,  7, 6, ".01269ab.", NULL },
    { 3, 3, 0, 0,  8, 6, ".012479ab.", NULL },
    { 3, 3, 0, 0,  9, 6, ".0124689ab.", NULL },
    { 3, 3, 0, 0, 10, 6, ".01235689ab.", NULL },
    { 3, 3, 0, 0, 11, 6, ".012346789ab.", NULL },
    { 3, 3, 0, 0, 12, 6, ".0123456789ab.", NULL },
    { 3, 3, 0, 0, 13, 6, ".01234566789ab.", NULL },
    { 3, 3, 0, 0, 14, 6, ".012344567789ab.", NULL },
    { 2, 4, 0, 0,  1, 6, ".a.", NULL },
    { 2, 4, 0, 0,  2, 6, ".9b.", NULL },
    { 2, 4, 0, 0,  3, 6, ".19b.", NULL },
    { 2, 4, 0, 0,  4, 6, ".18ab.", NULL },
    { 2, 4, 0, 0,  5, 6, ".189ab.", NULL },
    { 2, 4, 0, 0,  6, 6, ".0189ab.", NULL },
    { 2, 4, 0, 0,  7, 6, ".01589ab.", NULL },
    { 2, 4, 0, 0,  8, 6, ".013689ab.", NULL },
    { 2, 4, 0, 0,  9, 6, ".0135789ab.", NULL },
    { 2, 4, 0, 0, 10, 6, ".01245789ab.", NULL },
    { 2, 4, 0, 0, 11, 6, ".012356789ab.", NULL },
    { 2, 4, 0, 0, 12, 6, ".0123456789ab.", NULL },
    { 2, 4, 0, 0, 13, 6, ".01234556789ab.", NULL },
    { 2, 4, 0, 0, 14, 6, ".012334566789ab.", NULL },
    { 1, 4, 0, 0,  1, 6, ".a.", NULL },
    { 1, 4, 0, 0,  2, 6, ".9b.", NULL },
    { 1, 4, 0, 0,  3, 6, ".8ab.", NULL },
    { 1, 4, 0, 0,  4, 6, ".89ab.", NULL },
    { 1, 4, 0, 0,  5, 6, ".089ab.", NULL },
    { 1, 4, 0, 0,  6, 6, ".0489ab.", NULL },
    { 1, 4, 0, 0,  7, 6, ".02689ab.", NULL },
    { 1, 4, 0, 0,  8, 6, ".024689ab.", NULL },
    { 1, 4, 0, 0,  9, 6, ".0135789ab.", NULL },
    { 1, 4, 0, 0, 10, 6, ".01345789ab.", NULL },
    { 1, 4, 0, 0, 11, 6, ".012356789ab.", NULL },
    { 1, 4, 0, 0, 12, 6, ".0123456789ab.", NULL },
    { 1, 4, 0, 0, 13, 6, ".01234456789ab.", NULL },
    { 1, 4, 0, 0, 14, 6, ".012234566789ab.", NULL },
    { 5, 0, 0, 0,  1, 6, ".2.", NULL },
    { 5, 0, 0, 0,  2, 6, ".13.", NULL },
    { 5, 0, 0, 0,  3, 6, ".024.", NULL },
    { 5, 0, 0, 0,  4, 6, ".0134.", NULL },
    { 5, 0, 0, 0,  5, 6, ".01234.", NULL },
    { 5, 0, 0, 0,  6, 6, ".012348.", NULL },
    { 5, 0, 0, 0,  7, 6, ".012346a.", NULL },
    { 5, 0, 0, 0,  8, 6, ".0123468a.", NULL },
    { 5, 0, 0, 0,  9, 6, ".01234579b.", NULL },
    { 5, 0, 0, 0, 10, 6, ".012345789b.", NULL },
    { 5, 0, 0, 0, 11, 6, ".012345679ab.", NULL },
    { 5, 0, 0, 0, 12, 6, ".0123456789ab.", NULL },
    { 5, 0, 0, 0, 13, 6, ".01234567889ab.", NULL },
    { 5, 0, 0, 0, 14, 6, ".01234566789aab.", NULL },
    { 0, 0, 2, 2, 12, 1, NULL, ".4." },
    { 0, 0, 2, 2, 12, 2, NULL, ".04." },
    { 0, 0, 2, 2, 12, 3, NULL, ".045." },
    { 0, 0, 2, 2, 12, 4, NULL, ".0145." },
    { 0, 0, 2, 2, 12, 5, NULL, ".01245." },
    { 0, 0, 2, 2, 12, 6, NULL, ".012345." },
    { 0, 0, 2, 2, 12, 7, NULL, ".0122345." },
    { 0, 0, 2, 2, 12, 8, NULL, ".01223345." },
    { 3, 3, 2, 2,  1, 1, ".a.", ".4." },
    { 3, 3, 2, 2,  2, 2, ".1a.", ".04." },
    { 3, 3, 2, 2,  3, 3, ".19b.", ".045." },
    { 3, 3, 2, 2,  4, 4, ".029b.", ".0145." },
    { 3, 3, 2, 2,  5, 5, ".029ab.", ".01245." },
    { 3, 3, 2, 2,  6, 6, ".0129ab.", ".012345." },
    { 3, 3, 2, 2,  7, 7, ".01269ab.", ".0122345." },
    { 3, 3, 2, 2,  8, 8, ".012479ab.", ".01223345." },
};

static void
Test_Margins(HDC hdc, PDWORD pdwDst)
{
    TEST_DRAW_STREAM ds;
    char szRow[DST_CX + 1], szColumn[DST_CY + 1];
    UINT i;
    INT x, y;

    for (i = 0; i < _countof(s_MarginCases); ++i)
    {
        const MARGIN_CASE *pCase = &s_MarginCases[i];

        ZeroMemory(&ds, sizeof(ds));
        ds.signature = 0x44727753;
        ds.unknown1 = 1;
        ds.unknown2 = 9;
        ds.hDC = hdc;
        ds.hImage = s_hbmSrc;
        SetRect(&ds.rcDest, 1, 1, 1 + pCase->cx, 1 + pCase->cy);
        ds.rcClip = ds.rcDest;
        SetRect(&ds.rcSrc, 0, 0, SRC_CX, SRC_CY);
        ds.leftSizingMargin = pCase->Left;
        ds.rightSizingMargin = pCase->Right;
        ds.topSizingMargin = pCase->Top;
        ds.bottomSizingMargin = pCase->Bottom;

        FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
        CallDrawStream(hdc, &ds);
        GdiFlush();

        for (x = 0; x < pCase->cx + 2; ++x)
        {
            DWORD dw = pdwDst[1 * DST_CX + x];
            szRow[x] = (dw == WHITE) ? '.' : ((dw & 0xFF0000) == 0x800000) ? "0123456789ab????"[dw & 0xF] : '?';
        }
        szRow[pCase->cx + 2] = 0;
        for (y = 0; y < pCase->cy + 2; ++y)
        {
            DWORD dw = pdwDst[y * DST_CX + 1];
            szColumn[y] = (dw == WHITE) ? '.' : ((dw & 0xFF0000) == 0x800000) ? "0123456789ab????"[(dw >> 8) & 0xF] : '?';
        }
        szColumn[pCase->cy + 2] = 0;

        if (pCase->pszRow)
        {
            ok(!strcmp(szRow, pCase->pszRow), "margins %d,%d,%d,%d size %dx%d: row %s, expected %s\n",
               pCase->Left, pCase->Right, pCase->Top, pCase->Bottom, pCase->cx, pCase->cy, szRow, pCase->pszRow);
        }
        if (pCase->pszColumn)
        {
            ok(!strcmp(szColumn, pCase->pszColumn), "margins %d,%d,%d,%d size %dx%d: column %s, expected %s\n",
               pCase->Left, pCase->Right, pCase->Top, pCase->Bottom, pCase->cx, pCase->cy, szColumn, pCase->pszColumn);
        }
    }
}

#define TEST_DS_TILE 0x2

typedef struct _TILE_CASE
{
    INT Left, Right, Top, Bottom;
    INT cx, cy;
    const char *pszGrid;
} TILE_CASE;

/* DS_TILE, measured on Windows 10 (x64 and WOW64): source column and row of every destination pixel */
static const TILE_CASE s_TileCases[] =
{
    { 3, 3, 2, 2, 2, 15,
      "10a0"
      "11a1"
      "12a2"
      "13a3"
      "12a2"
      "13a3"
      "12a2"
      "13a3"
      "12a2"
      "13a3"
      "12a2"
      "13a3"
      "12a2"
      "14a4"
      "15a5" },
    { 3, 3, 2, 2, 3, 15,
      "1090b0"
      "1191b1"
      "1292b2"
      "1393b3"
      "1292b2"
      "1393b3"
      "1292b2"
      "1393b3"
      "1292b2"
      "1393b3"
      "1292b2"
      "1393b3"
      "1292b2"
      "1494b4"
      "1595b5" },
    { 3, 3, 2, 2, 5, 15,
      "002090a0b0"
      "012191a1b1"
      "022292a2b2"
      "032393a3b3"
      "022292a2b2"
      "032393a3b3"
      "022292a2b2"
      "032393a3b3"
      "022292a2b2"
      "032393a3b3"
      "022292a2b2"
      "032393a3b3"
      "022292a2b2"
      "042494a4b4"
      "052595a5b5" },
    { 3, 3, 2, 2, 20, 3,
      "001020304050607080304050607080304090a0b0"
      "041424344454647484344454647484344494a4b4"
      "051525354555657585354555657585354595a5b5" },
    { 3, 3, 2, 2, 20, 2,
      "001020304050607080304050607080304090a0b0"
      "041424344454647484344454647484344494a4b4" },
    { 3, 3, 2, 2, 9, 9,
      "00102030405090a0b0"
      "01112131415191a1b1"
      "02122232425292a2b2"
      "03132333435393a3b3"
      "02122232425292a2b2"
      "03132333435393a3b3"
      "02122232425292a2b2"
      "04142434445494a4b4"
      "05152535455595a5b5" },
    { 3, 3, 2, 2, 20, 15,
      "001020304050607080304050607080304090a0b0"
      "011121314151617181314151617181314191a1b1"
      "021222324252627282324252627282324292a2b2"
      "031323334353637383334353637383334393a3b3"
      "021222324252627282324252627282324292a2b2"
      "031323334353637383334353637383334393a3b3"
      "021222324252627282324252627282324292a2b2"
      "031323334353637383334353637383334393a3b3"
      "021222324252627282324252627282324292a2b2"
      "031323334353637383334353637383334393a3b3"
      "021222324252627282324252627282324292a2b2"
      "031323334353637383334353637383334393a3b3"
      "021222324252627282324252627282324292a2b2"
      "041424344454647484344454647484344494a4b4"
      "051525354555657585354555657585354595a5b5" },
    { 0, 0, 0, 0, 15, 10,
      "00102030405060708090a0b0001020"
      "01112131415161718191a1b1011121"
      "02122232425262728292a2b2021222"
      "03132333435363738393a3b3031323"
      "04142434445464748494a4b4041424"
      "05152535455565758595a5b5051525"
      "00102030405060708090a0b0001020"
      "01112131415161718191a1b1011121"
      "02122232425262728292a2b2021222"
      "03132333435363738393a3b3031323" },
};

static void
Test_Tile(HDC hdc, PDWORD pdwDst)
{
    TEST_DRAW_STREAM ds;
    char szGrid[2 * DST_CX * DST_CY + 1];
    UINT i;
    INT x, y, n;

    for (i = 0; i < _countof(s_TileCases); ++i)
    {
        const TILE_CASE *pCase = &s_TileCases[i];

        ZeroMemory(&ds, sizeof(ds));
        ds.signature = 0x44727753;
        ds.unknown1 = 1;
        ds.unknown2 = 9;
        ds.hDC = hdc;
        ds.hImage = s_hbmSrc;
        SetRect(&ds.rcDest, 1, 1, 1 + pCase->cx, 1 + pCase->cy);
        ds.rcClip = ds.rcDest;
        SetRect(&ds.rcSrc, 0, 0, SRC_CX, SRC_CY);
        ds.drawOption = TEST_DS_TILE;
        ds.leftSizingMargin = pCase->Left;
        ds.rightSizingMargin = pCase->Right;
        ds.topSizingMargin = pCase->Top;
        ds.bottomSizingMargin = pCase->Bottom;

        FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
        CallDrawStream(hdc, &ds);
        GdiFlush();

        n = 0;
        for (y = 0; y < pCase->cy; ++y)
        {
            for (x = 0; x < pCase->cx; ++x)
            {
                DWORD dw = pdwDst[(1 + y) * DST_CX + 1 + x];
                BOOL bSrc = (dw & 0xFF0000) == 0x800000;
                szGrid[n++] = (dw == WHITE) ? '.' : bSrc ? "0123456789ab????"[dw & 0xF] : '?';
                szGrid[n++] = (dw == WHITE) ? '.' : bSrc ? "0123456789ab????"[(dw >> 8) & 0xF] : '?';
            }
        }
        szGrid[n] = 0;
        ok(!strcmp(szGrid, pCase->pszGrid), "tile margins %d,%d,%d,%d size %dx%d: %s, expected %s\n",
           pCase->Left, pCase->Right, pCase->Top, pCase->Bottom, pCase->cx, pCase->cy, szGrid, pCase->pszGrid);
    }
}

/* A destination in a DC scaled four times is resized to device pixels (measured on Windows 10, x64 and WOW64) */
static const char s_szScaled[] =
    "00102030405060708090a0b0"
    "01112131415161718191a1b1"
    "01112131415161718191a1b1"
    "02122232425262728292a2b2"
    "03132333435363738393a3b3"
    "04142434445464748494a4b4"
    "04142434445464748494a4b4"
    "05152535455565758595a5b5";

static void
ScaledGrid(PDWORD pdwDst, INT cx, INT cy, char *pszGrid)
{
    INT x, y, n = 0;

    for (y = 0; y < cy; ++y)
    {
        for (x = 0; x < cx; ++x)
        {
            DWORD dw = pdwDst[y * DST_CX + x];
            BOOL bSrc = (dw & 0xFF0000) == 0x800000;
            pszGrid[n++] = (dw == WHITE) ? '.' : bSrc ? "0123456789ab????"[dw & 0xF] : '?';
            pszGrid[n++] = (dw == WHITE) ? '.' : bSrc ? "0123456789ab????"[(dw >> 8) & 0xF] : '?';
        }
    }
    pszGrid[n] = 0;
}

static void
Test_Scaled(HDC hdc, PDWORD pdwDst)
{
    TEST_DRAW_STREAM ds;
    char szGrid[2 * DST_CX * DST_CY + 1];
    HRGN hrgn;

    ZeroMemory(&ds, sizeof(ds));
    ds.signature = 0x44727753;
    ds.unknown1 = 1;
    ds.unknown2 = 9;
    ds.hDC = hdc;
    ds.hImage = s_hbmSrc;
    SetRect(&ds.rcDest, 0, 0, 3, 2);
    ds.rcClip = ds.rcDest;
    SetRect(&ds.rcSrc, 0, 0, SRC_CX, SRC_CY);

    SetMapMode(hdc, MM_ANISOTROPIC);
    SetWindowExtEx(hdc, 1, 1, NULL);
    SetViewportExtEx(hdc, 4, 4, NULL);

    FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
    CallDrawStream(hdc, &ds);
    GdiFlush();
    ScaledGrid(pdwDst, 12, 8, szGrid);
    ok(!strcmp(szGrid, s_szScaled), "scaled: %s, expected %s\n", szGrid, s_szScaled);

    /* A device clip smaller than one logical unit */
    hrgn = CreateRectRgn(0, 0, 2, 2);
    ExtSelectClipRgn(hdc, hrgn, RGN_COPY);
    DeleteObject(hrgn);
    FillMemory(pdwDst, DST_CX * DST_CY * sizeof(DWORD), 0xFF);
    CallDrawStream(hdc, &ds);
    GdiFlush();
    ScaledGrid(pdwDst, 3, 3, szGrid);
    ok(!strcmp(szGrid, "0010..0111........"), "scaled and clipped: %s\n", szGrid);

    SelectClipRgn(hdc, NULL);
    SetMapMode(hdc, MM_TEXT);
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

    /* Sizing margins, also in a destination smaller than both of them */
    Test_Margins(hdc, pdwDst);

    /* Tiling, also across compressed margins */
    Test_Tile(hdc, pdwDst);

    Test_Scaled(hdc, pdwDst);

    SelectObject(hdc, hbmOld);
    DeleteDC(hdc);
    DeleteObject(hbmDst);
    DeleteObject(s_hbmSrc);
}
