/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for StrChrNIW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include <apitest.h>
#include <shlwapi.h>

typedef PWSTR (WINAPI *FN_StrChrNIW)(PCWSTR, WCHAR, UINT);

START_TEST(StrChrNIW)
{
    static const WCHAR s_szText[] = L"Hello World";
    static const struct
    {
        WCHAR wch;
        UINT cchMax;
        INT iExpected; /* -1: not found */
    } s_Cases[] =
    {
        { L'l', 11, 2 },
        { L'L', 11, 2 },
        { L'w', 11, 6 },
        { L'W', 5, -1 },
        { L'd', 11, 10 },
        { L'D', 10, -1 },
        { L'h', 0, -1 },
        { L'H', 1, 0 },
        { L'o', 5, 4 },
        /* The search ends at the terminator, even within the count */
        { UNICODE_NULL, 11, -1 },
        { UNICODE_NULL, 20, -1 },
        { L'z', 20, -1 },
    };
    /* Letters outside ASCII: e acute, Cyrillic de */
    static const WCHAR s_szAccents[] = { 'a', 0x00E9, 'b', 0x0434, 0 };
    FN_StrChrNIW pStrChrNIW;
    PWSTR psz;
    INT i;

    pStrChrNIW = (FN_StrChrNIW)GetProcAddress(GetModuleHandleW(L"shlwapi.dll"), "StrChrNIW");
    if (!pStrChrNIW)
    {
        skip("StrChrNIW is not exported\n");
        return;
    }

    for (i = 0; i < _countof(s_Cases); i++)
    {
        psz = pStrChrNIW(s_szText, s_Cases[i].wch, s_Cases[i].cchMax);
        if (s_Cases[i].iExpected < 0)
            ok(psz == NULL, "'%lc' %u: found at %d\n", s_Cases[i].wch, s_Cases[i].cchMax, (INT)(psz - s_szText));
        else
            ok(psz == s_szText + s_Cases[i].iExpected, "'%lc' %u: %p, expected offset %d\n", s_Cases[i].wch,
               s_Cases[i].cchMax, psz, s_Cases[i].iExpected);
    }

    ok(pStrChrNIW(s_szAccents, 0x00C9, 4) == s_szAccents + 1, "E acute: %p\n", pStrChrNIW(s_szAccents, 0x00C9, 4));
    ok(pStrChrNIW(s_szAccents, 0x00E9, 4) == s_szAccents + 1, "e acute: %p\n", pStrChrNIW(s_szAccents, 0x00E9, 4));
    ok(pStrChrNIW(s_szAccents, 0x0414, 4) == s_szAccents + 3, "De: %p\n", pStrChrNIW(s_szAccents, 0x0414, 4));
    ok(pStrChrNIW(s_szAccents, 0x0414, 3) == NULL, "De within 3: %p\n", pStrChrNIW(s_szAccents, 0x0414, 3));

    ok(pStrChrNIW(NULL, L'a', 3) == NULL, "NULL string: not NULL\n");
}
