/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for FormatMessage inserts with 64-bit arguments
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static DWORD CDECL FormatW(PCWSTR pszFormat, PWSTR pszOut, DWORD cchOut, ...)
{
    va_list list;
    DWORD ret;

    va_start(list, cchOut);
    ret = FormatMessageW(FORMAT_MESSAGE_FROM_STRING, pszFormat, 0, 0, pszOut, cchOut, &list);
    va_end(list);
    return ret;
}

static DWORD CDECL FormatA(PCSTR pszFormat, PSTR pszOut, DWORD cchOut, ...)
{
    va_list list;
    DWORD ret;

    va_start(list, cchOut);
    ret = FormatMessageA(FORMAT_MESSAGE_FROM_STRING, pszFormat, 0, 0, pszOut, cchOut, &list);
    va_end(list);
    return ret;
}

START_TEST(FormatMessageInsert)
{
    WCHAR szW[64];
    CHAR szA[64];
    DWORD ret;

    /* A 64-bit argument that is used twice, and a pointer after it */
    ret = FormatW(L"%1!I64u!|%1!I64x!", szW, _countof(szW), 0x100000000ULL);
    ok(ret == 20 && !wcscmp(szW, L"4294967296|100000000"), "got %lu [%S]\n", ret, szW);

    ret = FormatA("%1!I64u!|%1!I64x!", szA, _countof(szA), 0x100000000ULL);
    ok(ret == 20 && !strcmp(szA, "4294967296|100000000"), "got %lu [%s]\n", ret, szA);

    /* Without the 64-bit read the upper half of the number is taken for the string */
    szW[0] = UNICODE_NULL;
    ret = 0;
    _SEH2_TRY
    {
        ret = FormatW(L"%1!I64u!|%2", szW, _countof(szW), 0x100000000ULL, L"str");
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(FALSE, "FormatMessageW raised exception %lx\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;
    ok(ret == 14 && !wcscmp(szW, L"4294967296|str"), "got %lu [%S]\n", ret, szW);

    szA[0] = ANSI_NULL;
    ret = 0;
    _SEH2_TRY
    {
        ret = FormatA("%1!I64u!|%2", szA, _countof(szA), 0x100000000ULL, "str");
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(FALSE, "FormatMessageA raised exception %lx\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;
    ok(ret == 14 && !strcmp(szA, "4294967296|str"), "got %lu [%s]\n", ret, szA);

    /* A width from an argument before a 64-bit value */
    ret = FormatW(L"%2!*I64u!", szW, _countof(szW), 0ULL, 5, 7ULL);
    ok(ret == 5 && !wcscmp(szW, L"    7"), "got %lu [%S]\n", ret, szW);

    ret = FormatA("%2!*I64u!", szA, _countof(szA), 0ULL, 5, 7ULL);
    ok(ret == 5 && !strcmp(szA, "    7"), "got %lu [%s]\n", ret, szA);
}
