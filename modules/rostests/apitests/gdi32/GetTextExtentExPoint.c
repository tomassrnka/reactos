/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for GetTextExtentExPoint
 * PROGRAMMERS:     Timo Kreuzer
 *                  Katayama Hirofumi MZ
 */

#include "precomp.h"

void Test_GetTextExtentExPoint()
{
    INT nFit;
    SIZE size;
    BOOL result;

    SetLastError(0);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, 1000, &nFit, NULL, &size);
    ok_int(result, 1);
    ok_int(nFit, 4);
    ok_err(0);
    printf("nFit = %d\n", nFit);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, 1, &nFit, NULL, &size);
    ok_int(result, 1);
    ok_int(nFit, 0);
    ok_err(0);
    printf("nFit = %d\n", nFit);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, 0, &nFit, NULL, &size);
    ok_int(result, 1);
    ok_int(nFit, 0);
    ok_err(0);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, -1, &nFit, NULL, &size);
    ok_int(result, 1);
    ok_int(nFit, 4);
    ok_err(0);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, -2, &nFit, NULL, &size);
    ok_int(result, 0);
    ok_err(87);

    result = GetTextExtentExPointW(GetDC(0), L"test", 4, -10, &nFit, NULL, &size);
    ok_int(result, 1);

    result = GetTextExtentExPointA(GetDC(0), "test", 4, -10, &nFit, NULL, &size);
    ok_int(result, 0);
}

typedef struct
{
    BOOL ret, ret2;
    INT fit;
    INT extents[3], extents2[3];
    SIZE size, size2;
} ONE_EXTENT;

/* A glyph bitmap of 2000 pixels is megabytes, so a few dozen of them drain the 360 MB paged pool of a 2 GB i386
   system; smaller ones then take what the big ones left */
#define BIG_HEIGHT 2000

static HFONT CreateArial(INT nHeight)
{
    LOGFONTW lf = { 0 };

    lf.lfHeight = nHeight;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Arial");
    return CreateFontIndirectW(&lf);
}

/* The two calls of the Wine test: all extents, then the extents that fit up to the second one */
static void MeasureOne(HDC hdc, ONE_EXTENT *pOne)
{
    HFONT hfont, hOld;

    memset(pOne, 0, sizeof(*pOne));
    hfont = CreateArial(BIG_HEIGHT);
    hOld = SelectObject(hdc, hfont);
    pOne->ret = GetTextExtentExPointW(hdc, L"One", 3, 0, NULL, pOne->extents, &pOne->size);
    pOne->ret2 = GetTextExtentExPointW(hdc, L"One", 3, pOne->extents[1], &pOne->fit, pOne->extents2, &pOne->size2);
    SelectObject(hdc, hOld);
    DeleteObject(hfont);
}

static void CheckOne(PCSTR pszWhen, const ONE_EXTENT *pOne, const ONE_EXTENT *pRef)
{
    trace("%s: ret %d,%d fit %d extents %d, %d, %d size %ldx%ld\n", pszWhen, pOne->ret, pOne->ret2, pOne->fit,
          pOne->extents[0], pOne->extents[1], pOne->extents[2], pOne->size.cx, pOne->size.cy);

    ok(pOne->ret && pOne->ret2, "%s: GetTextExtentExPointW failed\n", pszWhen);
    ok(pOne->extents[0] > 0, "%s: first extent is %d\n", pszWhen, pOne->extents[0]);
    ok(pOne->extents[1] > pOne->extents[0] && pOne->extents[2] > pOne->extents[1],
       "%s: extents %d, %d, %d do not grow\n", pszWhen, pOne->extents[0], pOne->extents[1], pOne->extents[2]);
    ok(pOne->size.cx == pOne->extents[2], "%s: cx %ld, last extent %d\n", pszWhen, pOne->size.cx, pOne->extents[2]);
    ok(pOne->fit == 2, "%s: fit %d, expected 2\n", pszWhen, pOne->fit);
    ok(pOne->size2.cx == pOne->size.cx && pOne->extents2[0] == pOne->extents[0] &&
       pOne->extents2[1] == pOne->extents[1], "%s: second call gives cx %ld and extents %d, %d, first gave %ld, %d, %d\n",
       pszWhen, pOne->size2.cx, pOne->extents2[0], pOne->extents2[1], pOne->size.cx, pOne->extents[0], pOne->extents[1]);
    if (pRef)
    {
        ok(memcmp(pOne->extents, pRef->extents, sizeof(pOne->extents)) == 0 && pOne->size.cx == pRef->size.cx &&
           pOne->size.cy == pRef->size.cy,
           "%s: extents %d, %d, %d size %ldx%ld, cold values %d, %d, %d size %ldx%ld\n", pszWhen,
           pOne->extents[0], pOne->extents[1], pOne->extents[2], pOne->size.cx, pOne->size.cy,
           pRef->extents[0], pRef->extents[1], pRef->extents[2], pRef->size.cx, pRef->size.cy);
    }
}

/* Draw distinct huge glyphs: each leaves its bitmap in the glyph cache (256 entries at most), and a draw that
   finds no pool fails quietly. The sizes step down so the smaller bitmaps fill the gaps the bigger ones left. */
static void FillGlyphCache(HDC hdc)
{
    static const struct { INT nHeight; UINT cGlyphs; } tiers[] =
    {
        { 2400, 40 }, { 1200, 24 }, { 600, 24 }, { 300, 24 }
    };
    UINT iTier, i;

    for (iTier = 0; iTier < ARRAYSIZE(tiers); iTier++)
    {
        for (i = 0; i < tiers[iTier].cGlyphs; i++)
        {
            HFONT hfont = CreateArial(-(tiers[iTier].nHeight + (INT)i));
            HFONT hOld = SelectObject(hdc, hfont);

            ExtTextOutW(hdc, 0, 0, 0, NULL, L"W", 1, NULL);
            SelectObject(hdc, hOld);
            DeleteObject(hfont);
        }
    }
    GdiFlush();
}

/* The cache holds 256 glyphs and drops the least recently drawn: enough small ones push every big one out */
static void FlushGlyphCache(HDC hdc)
{
    static const INT heights[] = { -8, -9, -10 };
    UINT i;
    WCHAR ch;

    for (i = 0; i < ARRAYSIZE(heights); i++)
    {
        HFONT hfont = CreateArial(heights[i]);
        HFONT hOld = SelectObject(hdc, hfont);

        for (ch = L'!'; ch <= L'~'; ch++)
            ExtTextOutW(hdc, 0, 0, 0, NULL, &ch, 1, NULL);

        SelectObject(hdc, hOld);
        DeleteObject(hfont);
    }
    GdiFlush();
}

/* Windows measures without building a glyph bitmap, so a measurement cannot fail for lack of memory, nor come back
   cut short. Here the glyph cache is filled with huge glyphs first, which on a small system leaves no pool for the
   bitmaps of one more string. */
static void Test_LargeHimetricHeight(void)
{
    HDC hdcScreen, hdc, hdcFill;
    HBITMAP hbm, hbmOld, hbmFill, hbmFillOld;
    ONE_EXTENT cold, full, flushed;

    hdcScreen = GetDC(NULL);
    hdc = CreateCompatibleDC(hdcScreen);
    hdcFill = CreateCompatibleDC(hdcScreen);
    hbm = CreateCompatibleBitmap(hdcScreen, 64, 64);
    hbmFill = CreateCompatibleBitmap(hdcScreen, 64, 64);
    hbmOld = SelectObject(hdc, hbm);
    hbmFillOld = SelectObject(hdcFill, hbmFill);
    SetMapMode(hdc, MM_HIMETRIC);

    MeasureOne(hdc, &cold);
    CheckOne("cold", &cold, NULL);

    /* A measurement that built bitmaps left them in the cache, where the next one finds them */
    FlushGlyphCache(hdcFill);
    FillGlyphCache(hdcFill);

    MeasureOne(hdc, &full);
    CheckOne("cache full of huge glyphs", &full, &cold);

    FlushGlyphCache(hdcFill);
    MeasureOne(hdc, &flushed);
    CheckOne("cache flushed", &flushed, &cold);

    SelectObject(hdcFill, hbmFillOld);
    SelectObject(hdc, hbmOld);
    DeleteObject(hbmFill);
    DeleteObject(hbm);
    DeleteDC(hdcFill);
    DeleteDC(hdc);
    ReleaseDC(NULL, hdcScreen);
}

START_TEST(GetTextExtentExPoint)
{
    Test_GetTextExtentExPoint();
    Test_LargeHimetricHeight();
}
