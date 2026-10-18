/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Plain text extents are the sum of the advances: pair kerning is not applied, nor drawn
 */

#include "precomp.h"

static const PCWSTR g_faces[] = { L"Arial", L"Times New Roman", L"Tahoma", L"Verdana" };
static const INT g_heights[] = { -12, -16, -48, -140 };
/* Every pair has a kerning entry in the Windows versions of the faces above */
static const PCWSTR g_pairs[] = { L"AV", L"AT", L"To", L"Ty", L"Yo", L"VA", L"WA", L"LT" };

#define BMP_CX 700
#define BMP_CY 260

static INT Extent(HDC hdc, PCWSTR psz, INT cch)
{
    SIZE size = { -1, -1 };
    BOOL ret = GetTextExtentPoint32W(hdc, psz, cch, &size);
    ok(ret, "GetTextExtentPoint32W failed\n");
    return size.cx;
}

/* The pair drawn as one string must put the second glyph where the measured advance of the first says */
static void TestDraw(HDC hdc, PCWSTR pszFace, INT nHeight, PCWSTR pszPair, DWORD *pvBits, DWORD *pdwRef)
{
    UINT i, cDiff = 0;

    memset(pvBits, 0xFF, BMP_CX * BMP_CY * sizeof(DWORD));
    ExtTextOutW(hdc, 10, 10, 0, NULL, pszPair, 2, NULL);
    GdiFlush();
    memcpy(pdwRef, pvBits, BMP_CX * BMP_CY * sizeof(DWORD));

    memset(pvBits, 0xFF, BMP_CX * BMP_CY * sizeof(DWORD));
    ExtTextOutW(hdc, 10, 10, 0, NULL, pszPair, 1, NULL);
    ExtTextOutW(hdc, 10 + Extent(hdc, pszPair, 1), 10, 0, NULL, pszPair + 1, 1, NULL);
    GdiFlush();

    for (i = 0; i < BMP_CX * BMP_CY; i++)
        cDiff += (pvBits[i] != pdwRef[i]);
    ok(cDiff == 0, "%ls %d \"%ls\": the pair drawn as one string differs from the split drawing in %u pixels\n",
       pszFace, nHeight, pszPair, cDiff);
}

/* GetCharacterPlacementW kerns, unlike the plain extent: the kerning of a pair goes to the width of its first character,
 * the caret positions add the widths up, and the returned width is their sum. Without results it is the plain extent. */
static void TestPlacement(HDC hdc, PCWSTR pszFace, INT nHeight, PCWSTR psz, BOOL bKerned)
{
    /* With GCP_REORDER only the widths are checked: ReactOS has the caret positions from Uniscribe there. Windows kerns
     * Latin text with GCP_GLYPHSHAPE too. */
    static const DWORD s_flags[] = { 0, GCP_USEKERNING, GCP_REORDER, GCP_REORDER | GCP_GLYPHSHAPE };
    INT cch = lstrlenW(psz), cxPlain = Extent(hdc, psz, cch), cxLast = Extent(hdc, psz + cch - 1, 1);
    INT aDx[16], aCaret[16], i, cxSum;
    WCHAR szOut[16];
    GCP_RESULTSW results;
    DWORD ret, iflag;

    ret = GetCharacterPlacementW(hdc, psz, cch, 0, NULL, 0);
    ok(LOWORD(ret) == cxPlain, "%ls %d \"%ls\": GCP without results %u, extent %d\n",
       pszFace, nHeight, psz, LOWORD(ret), cxPlain);

    for (iflag = 0; iflag < ARRAYSIZE(s_flags); iflag++)
    {
        memset(&results, 0, sizeof(results));
        results.lStructSize = sizeof(results);
        results.lpOutString = szOut;
        results.lpDx = aDx;
        results.lpCaretPos = aCaret;
        results.nGlyphs = cch;
        for (i = 0; i < ARRAYSIZE(aDx); i++)
            aDx[i] = aCaret[i] = -7;
        ret = GetCharacterPlacementW(hdc, psz, cch, 0, &results, s_flags[iflag]);
        ok(results.nGlyphs == cch, "%ls %d \"%ls\" %lu: nGlyphs %u\n", pszFace, nHeight, psz, s_flags[iflag], results.nGlyphs);

        for (i = 0, cxSum = 0; i < cch; cxSum += aDx[i], i++)
        {
            ok(aCaret[i] == cxSum || (s_flags[iflag] & GCP_REORDER),
               "%ls %d \"%ls\" %lu: caret %d is %d, the widths before it add up to %d\n",
               pszFace, nHeight, psz, s_flags[iflag], i, aCaret[i], cxSum);
        }
        ok(LOWORD(ret) == cxSum, "%ls %d \"%ls\" %lu: GCP width %u, its widths add up to %d\n",
           pszFace, nHeight, psz, s_flags[iflag], LOWORD(ret), cxSum);
        ok(aDx[cch - 1] == cxLast, "%ls %d \"%ls\" %lu: last width %d, its advance %d\n",
           pszFace, nHeight, psz, s_flags[iflag], aDx[cch - 1], cxLast);
        if (bKerned)
        {
            ok(LOWORD(ret) < cxPlain, "%ls %d \"%ls\" %lu: GCP width %u is not kerned, extent %d\n",
               pszFace, nHeight, psz, s_flags[iflag], LOWORD(ret), cxPlain);
        }
        else
        {
            ok(LOWORD(ret) <= cxPlain, "%ls %d \"%ls\" %lu: GCP width %u, extent %d\n",
               pszFace, nHeight, psz, s_flags[iflag], LOWORD(ret), cxPlain);
        }

        /* Only the arrays asked for are written; the width does not depend on them */
        memset(&results, 0, sizeof(results));
        results.lStructSize = sizeof(results);
        results.nGlyphs = cch;
        ok(GetCharacterPlacementW(hdc, psz, cch, 0, &results, s_flags[iflag]) == ret,
           "%ls %d \"%ls\" %lu: GCP without arrays differs\n", pszFace, nHeight, psz, s_flags[iflag]);
    }
}

/* Windows shapes Thai sara am into two glyphs: with room for them GCP_GLYPHSHAPE gives three glyphs for two characters
 * and widths that add up to the returned width, and the call fails without the room (Windows 10 and Server 2008 R2) */
static void TestShapedRoom(HDC hdc)
{
    static const WCHAR s_szThai[] = { 0x0E01, 0x0E33, 0 };
    LOGFONTW lf = { 0 };
    WCHAR szFace[LF_FACESIZE] = L"", szOut[16], aGlyphs[16];
    INT aDx[16], i, cxSum;
    GCP_RESULTSW results;
    HFONT hfont, hOld;
    DWORD ret;

    lf.lfHeight = -24;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Tahoma");
    hfont = CreateFontIndirectW(&lf);
    hOld = SelectObject(hdc, hfont);
    GetTextFaceW(hdc, ARRAYSIZE(szFace), szFace);
    if (lstrcmpiW(szFace, L"Tahoma") != 0)
    {
        skip("Tahoma is not installed\n");
        goto done;
    }

    memset(&results, 0, sizeof(results));
    results.lStructSize = sizeof(results);
    results.lpOutString = szOut;
    results.lpDx = aDx;
    results.lpGlyphs = aGlyphs;
    results.nGlyphs = ARRAYSIZE(aGlyphs);
    for (i = 0; i < ARRAYSIZE(aDx); i++)
        aDx[i] = -7;
    ret = GetCharacterPlacementW(hdc, s_szThai, 2, 0, &results, GCP_REORDER | GCP_GLYPHSHAPE);
    ok(ret != 0, "Thai: GCP failed\n");
    ok(results.nGlyphs == 2 || results.nGlyphs == 3, "Thai: nGlyphs %u\n", results.nGlyphs);
    for (i = 0, cxSum = 0; i < (INT)results.nGlyphs && i < ARRAYSIZE(aDx); i++)
        cxSum += aDx[i];
    ok(LOWORD(ret) == cxSum, "Thai: GCP width %u, its widths add up to %d\n", LOWORD(ret), cxSum);

    /* Windows has 3; a shaper that keeps sara am whole has 2, and then the room cannot be checked */
    if (results.nGlyphs <= 2)
    {
        skip("Thai: sara am is not shaped into two glyphs\n");
    }
    else
    {
        DWORD iflag, s_flags[] = { GCP_REORDER | GCP_GLYPHSHAPE, GCP_REORDER };

        for (iflag = 0; iflag < ARRAYSIZE(s_flags); iflag++)
        {
            memset(&results, 0, sizeof(results));
            results.lStructSize = sizeof(results);
            results.lpOutString = szOut;
            results.lpDx = aDx;
            results.lpGlyphs = aGlyphs;
            results.nGlyphs = 2;
            ret = GetCharacterPlacementW(hdc, s_szThai, 2, 0, &results, s_flags[iflag]);
            ok(ret == 0, "Thai %lu: GCP with room for 2 glyphs returned %lu\n", s_flags[iflag], ret);
        }
    }

done:
    SelectObject(hdc, hOld);
    DeleteObject(hfont);
}

static void TestFont(HDC hdc, DWORD *pvBits, DWORD *pdwRef, PCWSTR pszFace, INT nHeight)
{
    LOGFONTW lf = { 0 };
    WCHAR szFace[LF_FACESIZE] = L"";
    HFONT hfont, hOld;
    UINT ipair;

    lf.lfHeight = nHeight;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), pszFace);
    hfont = CreateFontIndirectW(&lf);
    hOld = SelectObject(hdc, hfont);
    GetTextFaceW(hdc, ARRAYSIZE(szFace), szFace);
    if (lstrcmpiW(szFace, pszFace) != 0)
    {
        skip("%ls is not installed\n", pszFace);
        goto done;
    }

    for (ipair = 0; ipair < ARRAYSIZE(g_pairs); ipair++)
    {
        PCWSTR pszPair = g_pairs[ipair];
        INT cxPair, cxSum, cxChars = 0, aDx[2] = { -1, -1 }, nFit = -1, i;
        SIZE sizeEx = { -1, -1 };
        BOOL ret;

        cxPair = Extent(hdc, pszPair, 2);
        cxSum = Extent(hdc, pszPair, 1) + Extent(hdc, pszPair + 1, 1);
        ok(cxPair == cxSum, "%ls %d \"%ls\": extent %d, the advances add up to %d\n",
           pszFace, nHeight, pszPair, cxPair, cxSum);

        for (i = 0; i < 2; i++)
        {
            INT cxChar = 0;
            ret = GetCharWidth32W(hdc, pszPair[i], pszPair[i], &cxChar);
            ok(ret, "GetCharWidth32W failed\n");
            cxChars += cxChar;
        }
        ok(cxPair == cxChars, "%ls %d \"%ls\": extent %d, GetCharWidth32W adds up to %d\n",
           pszFace, nHeight, pszPair, cxPair, cxChars);

        ret = GetTextExtentExPointW(hdc, pszPair, 2, 100000, &nFit, aDx, &sizeEx);
        ok(ret, "GetTextExtentExPointW failed\n");
        ok(nFit == 2, "%ls %d \"%ls\": nFit is %d\n", pszFace, nHeight, pszPair, nFit);
        ok(sizeEx.cx == cxSum && aDx[1] == cxSum, "%ls %d \"%ls\": Ex extent %ld, dx %d, the advances add up to %d\n",
           pszFace, nHeight, pszPair, sizeEx.cx, aDx[1], cxSum);

        TestDraw(hdc, pszFace, nHeight, pszPair, pvBits, pdwRef);

        /* Windows kerns these pairs of Arial and Times New Roman from 48 pixels on; the other faces are not checked */
        TestPlacement(hdc, pszFace, nHeight, pszPair, nHeight <= -48 && (!lstrcmpiW(pszFace, L"Arial") ||
                                                                       !lstrcmpiW(pszFace, L"Times New Roman")));
    }
    TestPlacement(hdc, pszFace, nHeight, L"AVATAR", nHeight <= -48 && !lstrcmpiW(pszFace, L"Arial"));

done:
    SelectObject(hdc, hOld);
    DeleteObject(hfont);
}

START_TEST(GetTextExtentKerning)
{
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), BMP_CX, -BMP_CY, 1, 32, BI_RGB } };
    HDC hdc = CreateCompatibleDC(NULL);
    HBITMAP hbm, hbmOld;
    DWORD *pvBits = NULL, *pdwRef;
    UINT iFace, iHeight;

    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID *)&pvBits, NULL, 0);
    pdwRef = HeapAlloc(GetProcessHeap(), 0, BMP_CX * BMP_CY * sizeof(DWORD));
    if (!hbm || !pdwRef)
    {
        skip("no bitmap\n");
        goto done;
    }
    hbmOld = SelectObject(hdc, hbm);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    for (iFace = 0; iFace < ARRAYSIZE(g_faces); iFace++)
        for (iHeight = 0; iHeight < ARRAYSIZE(g_heights); iHeight++)
            TestFont(hdc, pvBits, pdwRef, g_faces[iFace], g_heights[iHeight]);
    TestShapedRoom(hdc);

    SelectObject(hdc, hbmOld);
done:
    HeapFree(GetProcessHeap(), 0, pdwRef);
    if (hbm)
        DeleteObject(hbm);
    DeleteDC(hdc);
}
