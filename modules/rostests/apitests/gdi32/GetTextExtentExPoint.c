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

#define DX_SENTINEL 0x5A5A5A5A

typedef BOOL (WINAPI *PFN_EXTENT)(HDC, LPCWSTR, INT, INT, LPINT, LPINT, LPSIZE);

/* Called through the exported entry point and, where gdi32 has it, through GetTextExtentExPointWPri, which goes to the
   kernel without the language-pack layer that otherwise measures a surrogate per code unit.

   Every code unit of the string gets an extent that includes the units before it; none is left as it was handed in,
   which for the kernel's copy of the buffer would be whatever the pool held. Windows gives a surrogate its own
   width per code unit and the width of the glyph a ReactOS pair has differs, so the values are only compared with
   each other. */
static void CheckDx(HDC hdc, PFN_EXTENT pfn, PCSTR pszName, PCWSTR psz, INT cch)
{
    INT dx[8], dx2[8], fit = -1, i;
    SIZE size = { -1, -1 }, size2 = { -1, -1 };
    BOOL ret, ret2;
    HFONT hfontBig, hOldFont;
    static const WCHAR wide[8] = { 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W' };

    /* The kernel's buffer for this many extents is likely the one the call before has just freed: leave values in it
       that no extent of the test font reaches */
    hfontBig = CreateArial(-100);
    hOldFont = SelectObject(hdc, hfontBig);
    pfn(hdc, wide, cch, 100000, &fit, dx, &size);
    SelectObject(hdc, hOldFont);
    DeleteObject(hfontBig);
    for (i = 0; i < cch; i++)
        dx[i] = dx2[i] = DX_SENTINEL;
    ret = pfn(hdc, psz, cch, 1000, &fit, dx, &size);
    ret2 = pfn(hdc, psz, cch, 1000, NULL, dx2, &size2);

    trace("DX %s: %d %d %d %d | %d %d %d %d cx %ld fit %d\n", pszName, dx[0], dx[1], cch>2?dx[2]:0, cch>3?dx[3]:0, dx2[0], dx2[1], cch>2?dx2[2]:0, cch>3?dx2[3]:0, size.cx, fit);
    ok(ret && ret2, "%s: GetTextExtentExPointW failed\n", pszName);
    ok(fit == cch, "%s: fit %d, expected %d\n", pszName, fit, cch);
    ok(size.cx > 0 && size.cx == size2.cx, "%s: cx %ld and %ld\n", pszName, size.cx, size2.cx);
    for (i = 0; i < cch; i++)
    {
        ok(dx[i] != DX_SENTINEL && dx[i] > 0 && dx[i] <= size.cx, "%s: dx[%d] is %d, cx %ld\n", pszName, i, dx[i], size.cx);
        ok(i == 0 || dx[i] >= dx[i - 1], "%s: dx[%d] %d is below dx[%d] %d\n", pszName, i, dx[i], i - 1, dx[i - 1]);
        ok(dx[i] == dx2[i], "%s: dx[%d] is %d, then %d\n", pszName, i, dx[i], dx2[i]);
    }
    ok(dx[cch - 1] == size.cx, "%s: last extent %d, cx %ld\n", pszName, dx[cch - 1], size.cx);
}

static void Test_SurrogateExtents(void)
{
    static const WCHAR pair[] = { 'A', 0xD83D, 0xDE00, 'B' };
    static const WCHAR loneHigh[] = { 'A', 0xD83D };
    static const WCHAR highThenChar[] = { 'A', 0xD83D, 'B' };
    static const WCHAR loneLow[] = { 'A', 0xDE00, 'B' };
    static const WCHAR plain[] = { 'A' };
    HDC hdc = CreateCompatibleDC(NULL);
    HFONT hfont = CreateArial(-20), hOld = SelectObject(hdc, hfont);
    SIZE sizeHigh, sizeHighB, sizeA;
    PFN_EXTENT pfnPri = (PFN_EXTENT)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "GetTextExtentExPointWPri");
    UINT i;
    PFN_EXTENT pfns[2];
    PCSTR names[2] = { "W", "WPri" };

    pfns[0] = GetTextExtentExPointW;
    pfns[1] = pfnPri;
    for (i = 0; i < ARRAYSIZE(pfns); i++)
    {
        char name[64];

        if (!pfns[i])
        {
            skip("%s is not exported\n", names[i]);
            continue;
        }
        StringCchPrintfA(name, ARRAYSIZE(name), "%s pair", names[i]);
        CheckDx(hdc, pfns[i], name, pair, ARRAYSIZE(pair));
        StringCchPrintfA(name, ARRAYSIZE(name), "%s lone high surrogate last", names[i]);
        CheckDx(hdc, pfns[i], name, loneHigh, ARRAYSIZE(loneHigh));
        StringCchPrintfA(name, ARRAYSIZE(name), "%s high surrogate then character", names[i]);
        CheckDx(hdc, pfns[i], name, highThenChar, ARRAYSIZE(highThenChar));
        StringCchPrintfA(name, ARRAYSIZE(name), "%s lone low surrogate", names[i]);
        CheckDx(hdc, pfns[i], name, loneLow, ARRAYSIZE(loneLow));
    }

    /* A high surrogate that no low one follows is measured on its own and swallows nothing after it */
    GetTextExtentPoint32W(hdc, plain, 1, &sizeA);
    GetTextExtentPoint32W(hdc, loneHigh, 2, &sizeHigh);
    GetTextExtentPoint32W(hdc, highThenChar, 3, &sizeHighB);
    ok(sizeHigh.cx > sizeA.cx, "lone high surrogate adds no width: %ld, A is %ld\n", sizeHigh.cx, sizeA.cx);
    ok(sizeHighB.cx > sizeHigh.cx, "character after a high surrogate adds no width: cx %ld, without it %ld\n",
       sizeHighB.cx, sizeHigh.cx);

    SelectObject(hdc, hOld);
    DeleteObject(hfont);
    DeleteDC(hdc);
}

#define DRAW_WIDTH 400
#define DRAW_HEIGHT 180
#define DRAW_FILL 0x00FF0000
#define DRAW_X 10
/* Windows pads a text background by a pixel or two on each side; the faults tested for are many pixels off */
#define DRAW_SLACK 4

typedef struct
{
    LONG ink, inkRight, fillLeft, fillRight;
} DRAWN;

/* Draws on white with a red text background: red is the background box the kernel sizes from the string, anything
   else that is not white is the glyphs */
static void DrawString(HDC hdc, const DWORD *pixels, PCSTR pszName, UINT fuOptions, UINT uAlign, INT x,
                       PCWSTR psz, INT cch, const INT *pDx, DRAWN *pDrawn)
{
    INT ix, iy;
    BOOL ret;

    PatBlt(hdc, 0, 0, DRAW_WIDTH, DRAW_HEIGHT, WHITENESS);
    SetTextAlign(hdc, uAlign);
    ret = ExtTextOutW(hdc, x, 5, fuOptions, NULL, psz, cch, pDx);
    ok(ret, "%s: ExtTextOutW failed\n", pszName);
    GdiFlush();

    pDrawn->ink = 0;
    pDrawn->inkRight = -1;
    pDrawn->fillLeft = DRAW_WIDTH;
    pDrawn->fillRight = 0;
    for (iy = 0; iy < DRAW_HEIGHT; iy++)
    {
        for (ix = 0; ix < DRAW_WIDTH; ix++)
        {
            DWORD pixel = pixels[iy * DRAW_WIDTH + ix] & 0x00FFFFFF;

            if (pixel == 0x00FFFFFF)
                continue;
            if (pixel == DRAW_FILL)
            {
                if (ix < pDrawn->fillLeft)
                    pDrawn->fillLeft = ix;
                if (ix + 1 > pDrawn->fillRight)
                    pDrawn->fillRight = ix + 1;
            }
            else
            {
                pDrawn->ink++;
                if (ix > pDrawn->inkRight)
                    pDrawn->inkRight = ix;
            }
        }
    }
}

/* The draw loop walks the string as the measuring loop does: a high surrogate that no low surrogate follows is a
   code unit of its own and the character after it is drawn, sized and aligned like any other */
static void Test_SurrogateDraw(void)
{
    static const WCHAR loneHigh[] = { 0xD83D };
    static const WCHAR highThenChar[] = { 0xD83D, 'B' };
    static const WCHAR charThenHigh[] = { 'A', 0xD83D };
    static const WCHAR pairThenChar[] = { 0xD83D, 0xDE00, 'B' };
    static const WCHAR pair[] = { 0xD83D, 0xDE00 };
    static const WCHAR lowThenChar[] = { 0xDE00, 'B' };
    static const WCHAR loneLow[] = { 0xDE00 };
    static const WCHAR ab[] = { 'A', 'B' };
    static const INT dx[2] = { 40, 40 };
    static const INT dxPdy[4] = { 40, 0, 40, 0 };
    static const UINT options[2] = { 0, ETO_IGNORELANGUAGE };
    static const PCSTR names[2] = { "default", "ignore language" };
    HDC hdcScreen = GetDC(NULL), hdc = CreateCompatibleDC(hdcScreen);
    HFONT hfont = CreateArial(-20), hOldFont;
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), DRAW_WIDTH, -DRAW_HEIGHT, 1, 32, BI_RGB } };
    DWORD *pixels = NULL;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID*)&pixels, NULL, 0), hbmOld;
    SIZE sizeHigh, sizeHighB, sizeAHigh, sizePair, sizePairB, sizeLow, sizeLowB, sizeAB, sizeA;
    WORD glyphs[2];
    BOOL bHaveGlyphs;
    DRAWN lone, drawn;
    UINT i;

    ReleaseDC(NULL, hdcScreen);
    if (!hbm || !pixels)
    {
        skip("no DIB section\n");
        goto Cleanup;
    }

    hbmOld = SelectObject(hdc, hbm);
    hOldFont = SelectObject(hdc, hfont);
    SetTextColor(hdc, RGB(0, 0, 0));
    SetBkColor(hdc, RGB(255, 0, 0));
    SetBkMode(hdc, OPAQUE);
    GetTextExtentPoint32W(hdc, loneHigh, 1, &sizeHigh);
    GetTextExtentPoint32W(hdc, highThenChar, 2, &sizeHighB);
    GetTextExtentPoint32W(hdc, charThenHigh, 2, &sizeAHigh);
    GetTextExtentPoint32W(hdc, pair, 2, &sizePair);
    GetTextExtentPoint32W(hdc, pairThenChar, 3, &sizePairB);
    GetTextExtentPoint32W(hdc, loneLow, 1, &sizeLow);
    GetTextExtentPoint32W(hdc, lowThenChar, 2, &sizeLowB);
    GetTextExtentPoint32W(hdc, ab, 2, &sizeAB);
    GetTextExtentPoint32W(hdc, ab, 1, &sizeA);
    bHaveGlyphs = (GetGlyphIndicesW(hdc, ab, 2, glyphs, GGI_MARK_NONEXISTING_GLYPHS) == 2 &&
                   glyphs[0] != 0xFFFF && glyphs[1] != 0xFFFF);

    for (i = 0; i < ARRAYSIZE(options); i++)
    {
        PCSTR pszName = names[i];

        /* A high surrogate followed by a character: the character is drawn after it, and the background box is
           as wide as the string measures */
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, loneHigh, 1, NULL, &lone);
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, highThenChar, 2, NULL, &drawn);
        trace("%s: lone ink %ld right %ld fill %ld..%ld | then char ink %ld right %ld fill %ld..%ld | cx %ld %ld\n",
              pszName, lone.ink, lone.inkRight, lone.fillLeft, lone.fillRight,
              drawn.ink, drawn.inkRight, drawn.fillLeft, drawn.fillRight, sizeHigh.cx, sizeHighB.cx);
        ok(drawn.inkRight >= DRAW_X + sizeHigh.cx + 2,
           "%s: the character after a lone high surrogate is not drawn: ink ends at %ld, the surrogate ends at %ld\n",
           pszName, drawn.inkRight, DRAW_X + sizeHigh.cx);
        ok(abs((drawn.fillRight - drawn.fillLeft) - sizeHighB.cx) <= DRAW_SLACK,
           "%s: background is %ld wide, the string measures %ld\n",
           pszName, drawn.fillRight - drawn.fillLeft, sizeHighB.cx);
        ok(abs((lone.fillRight - lone.fillLeft) - sizeHigh.cx) <= DRAW_SLACK,
           "%s: background of the lone surrogate is %ld wide, it measures %ld\n",
           pszName, lone.fillRight - lone.fillLeft, sizeHigh.cx);

        /* A high surrogate at the end is a unit of its own: it counts in the width */
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, charThenHigh, 2, NULL, &drawn);
        ok(abs((drawn.fillRight - drawn.fillLeft) - sizeAHigh.cx) <= DRAW_SLACK,
           "%s: background of a trailing high surrogate is %ld wide, the string measures %ld\n",
           pszName, drawn.fillRight - drawn.fillLeft, sizeAHigh.cx);

        /* Right alignment places the box by the whole width */
        DrawString(hdc, pixels, pszName, options[i], TA_RIGHT | TA_TOP, 150, highThenChar, 2, NULL, &drawn);
        ok(abs(drawn.fillLeft - (150 - sizeHighB.cx)) <= DRAW_SLACK && abs(drawn.fillRight - 150) <= DRAW_SLACK,
           "%s: right-aligned box is %ld..%ld, expected %ld..150\n",
           pszName, drawn.fillLeft, drawn.fillRight, 150 - sizeHighB.cx);

        /* With an explicit advance for every code unit */
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, highThenChar, 2, dx, &drawn);
        ok(abs((drawn.fillRight - drawn.fillLeft) - (dx[0] + dx[1])) <= DRAW_SLACK,
           "%s: background is %ld wide, the advances add up to %d\n",
           pszName, drawn.fillRight - drawn.fillLeft, dx[0] + dx[1]);
        ok(drawn.inkRight >= DRAW_X + dx[0],
           "%s: the character after the high surrogate is not drawn at its advance: ink ends at %ld, expected past %d\n",
           pszName, drawn.inkRight, DRAW_X + dx[0]);

        /* The same with an advance and a vertical offset for every code unit */
        DrawString(hdc, pixels, pszName, options[i] | ETO_PDY, TA_LEFT | TA_TOP, DRAW_X, highThenChar, 2, dxPdy, &drawn);
        ok(abs((drawn.fillRight - drawn.fillLeft) - (dxPdy[0] + dxPdy[2])) <= DRAW_SLACK,
           "%s: ETO_PDY: background is %ld wide, the advances add up to %d\n",
           pszName, drawn.fillRight - drawn.fillLeft, dxPdy[0] + dxPdy[2]);
        ok(drawn.inkRight >= DRAW_X + dxPdy[0],
           "%s: ETO_PDY: the character after the high surrogate is not drawn at its advance: ink ends at %ld, expected past %d\n",
           pszName, drawn.inkRight, DRAW_X + dxPdy[0]);

        /* A valid pair is one glyph and the character after it is drawn after it */
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, pairThenChar, 3, NULL, &drawn);
        ok(drawn.inkRight >= DRAW_X + sizePair.cx + 2,
           "%s: the character after a surrogate pair is not drawn: ink ends at %ld, the pair ends at %ld\n",
           pszName, drawn.inkRight, DRAW_X + sizePair.cx);
        /* Windows draws a pair as two glyphs when the language layer is bypassed, and measures it as one */
        if (!(options[i] & ETO_IGNORELANGUAGE))
        {
            ok(abs((drawn.fillRight - drawn.fillLeft) - sizePairB.cx) <= DRAW_SLACK,
               "%s: background of a pair and a character is %ld wide, the string measures %ld\n",
               pszName, drawn.fillRight - drawn.fillLeft, sizePairB.cx);
        }

        /* A lone low surrogate is a code unit of its own too */
        DrawString(hdc, pixels, pszName, options[i], TA_LEFT | TA_TOP, DRAW_X, lowThenChar, 2, NULL, &drawn);
        ok(drawn.inkRight >= DRAW_X + sizeLow.cx + 2,
           "%s: the character after a lone low surrogate is not drawn: ink ends at %ld, the surrogate ends at %ld\n",
           pszName, drawn.inkRight, DRAW_X + sizeLow.cx);
        ok(abs((drawn.fillRight - drawn.fillLeft) - sizeLowB.cx) <= DRAW_SLACK,
           "%s: background of a low surrogate and a character is %ld wide, the string measures %ld\n",
           pszName, drawn.fillRight - drawn.fillLeft, sizeLowB.cx);

        /* Glyph indices are never surrogates */
        if (bHaveGlyphs)
        {
            DrawString(hdc, pixels, pszName, options[i] | ETO_GLYPH_INDEX, TA_LEFT | TA_TOP, DRAW_X,
                       (PCWSTR)glyphs, 2, NULL, &drawn);
            ok(drawn.inkRight >= DRAW_X + sizeA.cx + 2,
               "%s: glyph indices: the second glyph is not drawn: ink ends at %ld, the first ends at %ld\n",
               pszName, drawn.inkRight, DRAW_X + sizeA.cx);
            ok(abs((drawn.fillRight - drawn.fillLeft) - sizeAB.cx) <= DRAW_SLACK,
               "%s: glyph indices: background is %ld wide, the string measures %ld\n",
               pszName, drawn.fillRight - drawn.fillLeft, sizeAB.cx);
        }
    }

    SelectObject(hdc, hOldFont);
    SelectObject(hdc, hbmOld);
    DeleteObject(hbm);
Cleanup:
    DeleteObject(hfont);
    DeleteDC(hdc);
}

/* With an explicit advance for every character the text is placed by those advances alone, so the background and the
   alignment, which are worked out before the glyphs are drawn, must not add the pair kerning of the font either */
static void Test_DrawKerning(void)
{
    static const WCHAR av[] = { 'A', 'V' };
    static const INT dx[2] = { 100, 100 };
    HDC hdcScreen = GetDC(NULL), hdc = CreateCompatibleDC(hdcScreen);
    HFONT hfont = CreateArial(-140), hOldFont;
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), DRAW_WIDTH, -DRAW_HEIGHT, 1, 32, BI_RGB } };
    DWORD *pixels = NULL;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID*)&pixels, NULL, 0), hbmOld;
    SIZE sizeA, sizeV;
    GCP_RESULTSW gcp;
    INT cxKerned;
    DRAWN drawn;
    UINT i;
    static const UINT options[2] = { 0, ETO_IGNORELANGUAGE };
    static const PCSTR names[2] = { "default", "ignore language" };

    ReleaseDC(NULL, hdcScreen);
    if (!hbm || !pixels)
    {
        skip("no DIB section\n");
        goto Cleanup;
    }

    hbmOld = SelectObject(hdc, hbm);
    hOldFont = SelectObject(hdc, hfont);
    SetTextColor(hdc, RGB(0, 0, 0));
    SetBkColor(hdc, RGB(255, 0, 0));
    SetBkMode(hdc, OPAQUE);
    /* A plain extent has no kerning; GetCharacterPlacementW has the kerning of the pair */
    memset(&gcp, 0, sizeof(gcp));
    gcp.lStructSize = sizeof(gcp);
    gcp.nGlyphs = 2;
    cxKerned = LOWORD(GetCharacterPlacementW(hdc, av, 2, 0, &gcp, 0));
    GetTextExtentPoint32W(hdc, av, 1, &sizeA);
    GetTextExtentPoint32W(hdc, av + 1, 1, &sizeV);
    trace("kerning: A %ld, V %ld, AV placed %d\n", sizeA.cx, sizeV.cx, cxKerned);
    ok(cxKerned > 0, "GetCharacterPlacementW failed\n");
    if (cxKerned <= 0 || cxKerned >= sizeA.cx + sizeV.cx - 4)
        skip("the font does not kern A and V\n");
    else
    {
        for (i = 0; i < ARRAYSIZE(options); i++)
        {
            DrawString(hdc, pixels, names[i], options[i], TA_LEFT | TA_TOP, DRAW_X, av, 2, dx, &drawn);
            ok(abs((drawn.fillRight - drawn.fillLeft) - (dx[0] + dx[1])) <= DRAW_SLACK,
               "%s: background is %ld wide, the advances add up to %d\n",
               names[i], drawn.fillRight - drawn.fillLeft, dx[0] + dx[1]);
            DrawString(hdc, pixels, names[i], options[i], TA_RIGHT | TA_TOP, DRAW_WIDTH - 10, av, 2, dx, &drawn);
            ok(abs(drawn.fillLeft - (DRAW_WIDTH - 10 - dx[0] - dx[1])) <= DRAW_SLACK &&
               abs(drawn.fillRight - (DRAW_WIDTH - 10)) <= DRAW_SLACK,
               "%s: right-aligned box is %ld..%ld, expected %d..%d\n",
               names[i], drawn.fillLeft, drawn.fillRight, DRAW_WIDTH - 10 - dx[0] - dx[1], DRAW_WIDTH - 10);
        }
    }

    SelectObject(hdc, hOldFont);
    SelectObject(hdc, hbmOld);
    DeleteObject(hbm);
Cleanup:
    DeleteObject(hfont);
    DeleteDC(hdc);
}

START_TEST(GetTextExtentExPoint)
{
    Test_GetTextExtentExPoint();
    Test_SurrogateExtents();
    Test_SurrogateDraw();
    Test_DrawKerning();
    Test_LargeHimetricHeight();
}
