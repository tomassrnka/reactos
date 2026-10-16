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
    }

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

    SelectObject(hdc, hbmOld);
done:
    HeapFree(GetProcessHeap(), 0, pdwRef);
    if (hbm)
        DeleteObject(hbm);
    DeleteDC(hdc);
}
