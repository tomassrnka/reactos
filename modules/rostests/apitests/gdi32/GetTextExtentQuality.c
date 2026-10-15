/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Text extents must not depend on the font quality, nor on what was drawn before
 */

#include "precomp.h"

static const BYTE g_qualities[] =
{
    NONANTIALIASED_QUALITY, DEFAULT_QUALITY, DRAFT_QUALITY, PROOF_QUALITY, ANTIALIASED_QUALITY, CLEARTYPE_QUALITY
};

static const char g_text[] = "Hamburgefonstiv AVATAR To Wa. The quick brown fox 0123456789 {}[]()";

static BOOL FontInstalled(HDC hdc, PCSTR pszFace)
{
    LOGFONTA lf = { 0 };
    char szFace[LF_FACESIZE] = "";
    HFONT hfont, hOld;

    lf.lfHeight = -12;
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyA(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), pszFace);
    hfont = CreateFontIndirectA(&lf);
    hOld = SelectObject(hdc, hfont);
    GetTextFaceA(hdc, ARRAYSIZE(szFace), szFace);
    SelectObject(hdc, hOld);
    DeleteObject(hfont);
    return lstrcmpiA(szFace, pszFace) == 0;
}

static void TestFont(HDC hdc, PCSTR pszFace, INT nHeight, INT nWeight, BOOL bItalic)
{
    SIZE sizeRef = { 0 }, size;
    INT widthRef[26] = { 0 }, width[26];
    UINT iQuality, ich;
    BOOL bHaveRef = FALSE;

    for (iQuality = 0; iQuality < ARRAYSIZE(g_qualities); iQuality++)
    {
        LOGFONTA lf = { 0 };
        HFONT hfont, hOld;
        BOOL ret;

        lf.lfHeight = nHeight;
        lf.lfWeight = nWeight;
        lf.lfItalic = bItalic;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = g_qualities[iQuality];
        StringCchCopyA(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), pszFace);
        hfont = CreateFontIndirectA(&lf);
        hOld = SelectObject(hdc, hfont);

        /* Draw first: the glyph cache then holds this quality's glyphs when the width is asked for again */
        ret = ExtTextOutA(hdc, 2, 2, 0, NULL, g_text, lstrlenA(g_text), NULL);
        ok(ret, "%s %d w%d i%d quality %u: ExtTextOutA failed\n", pszFace, nHeight, nWeight, bItalic, g_qualities[iQuality]);

        ret = GetTextExtentPoint32A(hdc, g_text, lstrlenA(g_text), &size);
        ok(ret, "%s %d: GetTextExtentPoint32A failed\n", pszFace, nHeight);

        for (ich = 0; ich < 26; ich++)
            GetCharWidth32A(hdc, 'A' + ich, 'A' + ich, &width[ich]);

        if (!bHaveRef)
        {
            sizeRef = size;
            memcpy(widthRef, width, sizeof(widthRef));
            bHaveRef = TRUE;
        }
        else
        {
            ok(size.cx == sizeRef.cx && size.cy == sizeRef.cy,
               "%s %d w%d i%d quality %u: extent %ldx%ld, bilevel gives %ldx%ld\n",
               pszFace, nHeight, nWeight, bItalic, g_qualities[iQuality], size.cx, size.cy, sizeRef.cx, sizeRef.cy);
            for (ich = 0; ich < 26; ich++)
                ok(width[ich] == widthRef[ich], "%s %d w%d i%d quality %u: width of '%c' is %d, bilevel gives %d\n",
                   pszFace, nHeight, nWeight, bItalic, g_qualities[iQuality], 'A' + ich, width[ich], widthRef[ich]);
        }

        SelectObject(hdc, hOld);
        DeleteObject(hfont);
    }
}

START_TEST(GetTextExtentQuality)
{
    /* Tahoma and Microsoft Sans Serif both carry embedded bitmap strikes at some sizes; the outlines have other
       advances there, and the extent must stay the one of the font as installed. */
    static const PCSTR s_faces[] = { "Tahoma", "Microsoft Sans Serif", "Arial" };
    static const INT s_heights[] = { -8, -9, -10, -11, -12, -13, -14, -16, -21 };
    HDC hdc, hdcMem;
    HBITMAP hbm, hbmOld;
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), 600, -40, 1, 32, BI_RGB } };
    PVOID pvBits;
    UINT iFace, iHeight;

    hdc = GetDC(NULL);
    hdcMem = CreateCompatibleDC(hdc);
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    hbmOld = SelectObject(hdcMem, hbm);
    SetBkMode(hdcMem, TRANSPARENT);

    for (iFace = 0; iFace < ARRAYSIZE(s_faces); iFace++)
    {
        if (!FontInstalled(hdcMem, s_faces[iFace]))
        {
            skip("%s is not installed\n", s_faces[iFace]);
            continue;
        }
        for (iHeight = 0; iHeight < ARRAYSIZE(s_heights); iHeight++)
        {
            /* Microsoft Sans Serif has no bold or italic face, so those two are simulated: FreeType widens a strike
               by whole pixels and an outline by a fraction, and the advance must not depend on which is drawn. */
            TestFont(hdcMem, s_faces[iFace], s_heights[iHeight], FW_NORMAL, FALSE);
            TestFont(hdcMem, s_faces[iFace], s_heights[iHeight], FW_BOLD, FALSE);
            TestFont(hdcMem, s_faces[iFace], s_heights[iHeight], FW_NORMAL, TRUE);
            TestFont(hdcMem, s_faces[iFace], s_heights[iHeight], FW_BOLD, TRUE);
        }
    }

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdc);
}
