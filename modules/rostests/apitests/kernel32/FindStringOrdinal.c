/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for FindStringOrdinal (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The vectors were measured on Windows Server 2008 R2 and Windows 10 22H2.
 */

#include "precomp.h"

typedef INT (WINAPI *PFN_FindStringOrdinal)(DWORD, LPCWSTR, INT, LPCWSTR, INT, BOOL);

#define PENDING_ERROR 0xdeadbeef

static PFN_FindStringOrdinal pFindStringOrdinal;

static void Check(DWORD Flags, LPCWSTR Source, INT SourceLength, LPCWSTR Value, INT ValueLength,
                  BOOL IgnoreCase, INT Expected, DWORD ExpectedError, int Line)
{
    INT Result;
    DWORD Error;

    SetLastError(PENDING_ERROR);
    Result = pFindStringOrdinal(Flags, Source, SourceLength, Value, ValueLength, IgnoreCase);
    Error = GetLastError();
    ok(Result == Expected, "line %d: got %d, expected %d\n", Line, Result, Expected);
    ok(Error == ExpectedError, "line %d: error %lu, expected %lu\n", Line, Error, ExpectedError);
}

#define CHECK(f, s, sl, v, vl, ic, e, err) Check(f, s, sl, v, vl, ic, e, err, __LINE__)

START_TEST(FindStringOrdinal)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    static const WCHAR Umlauts[] = { 0xe4, 0xc4, 0 };
    static const WCHAR UpperUmlaut[] = { 0xc4, 0 };
    static const WCHAR Nul[] = { 'a', 0, 'b' };
    static const WCHAR B[] = { 'b' };
    static const DWORD Bad[] = { 0x300000, 0x500000, 0x1000000, 0x900000, 0xf00000 };
    ULONG i;

    /* Windows XP and Windows Server 2003 do not have it */
    if (!GetProcAddress(Kernel32, "GetTickCount64"))
    {
        skip("this system is older than Windows Vista\n");
        return;
    }

    pFindStringOrdinal = (PFN_FindStringOrdinal)GetProcAddress(Kernel32, "FindStringOrdinal");
    if (!pFindStringOrdinal)
    {
        ok(!is_reactos(), "FindStringOrdinal is not exported\n");
        skip("FindStringOrdinal is not exported\n");
        return;
    }

    /* the four searches: from the start (0x400000), from the end (0x800000), starts with, ends with */
    CHECK(FIND_FROMSTART, L"abcABCabc", -1, L"abc", -1, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abcABCabc", -1, L"ABC", -1, TRUE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abcABCabc", -1, L"ABC", -1, FALSE, 3, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, L"abcABCabc", -1, L"abc", -1, FALSE, 6, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, L"abcABCabc", -1, L"ABC", -1, TRUE, 6, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, L"abcABCabc", -1, L"ABC", -1, FALSE, 3, ERROR_SUCCESS);
    CHECK(FIND_STARTSWITH, L"abcABCabc", -1, L"abc", -1, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_STARTSWITH, L"abcABCabc", -1, L"ABC", -1, TRUE, 0, ERROR_SUCCESS);
    CHECK(FIND_STARTSWITH, L"abcABCabc", -1, L"ABC", -1, FALSE, -1, ERROR_SUCCESS);
    CHECK(FIND_ENDSWITH, L"abcABCabc", -1, L"abc", -1, FALSE, 6, ERROR_SUCCESS);
    CHECK(FIND_ENDSWITH, L"abcABCabc", -1, L"ABC", -1, TRUE, 6, ERROR_SUCCESS);
    CHECK(FIND_ENDSWITH, L"abcABCabc", -1, L"ABC", -1, FALSE, -1, ERROR_SUCCESS);

    /* the flag is one of the four, or 0, which searches from the start */
    CHECK(0, L"abcABCabc", -1, L"ABC", -1, FALSE, 3, ERROR_SUCCESS);
    CHECK(0, L"abcABCabc", -1, L"ABC", -1, TRUE, 0, ERROR_SUCCESS);
    for (i = 0; i < RTL_NUMBER_OF(Bad); i++)
        CHECK(Bad[i], L"abcABCabc", -1, L"abc", -1, FALSE, -1, ERROR_INVALID_FLAGS);

    /* null strings, lengths below -1 and a case flag that is neither 0 nor 1 */
    CHECK(FIND_FROMSTART, NULL, -1, L"a", -1, FALSE, -1, ERROR_INVALID_PARAMETER);
    CHECK(FIND_FROMSTART, L"a", -1, NULL, -1, FALSE, -1, ERROR_INVALID_PARAMETER);
    CHECK(FIND_FROMSTART, L"abc", -2, L"a", 1, FALSE, -1, ERROR_INVALID_PARAMETER);
    CHECK(FIND_FROMSTART, L"abc", -1, L"a", -2, FALSE, -1, ERROR_INVALID_PARAMETER);
    CHECK(FIND_FROMSTART, L"abc", 3, L"a", 1, 2, -1, ERROR_INVALID_PARAMETER);

    /* the empty string is found where the search starts */
    CHECK(FIND_FROMSTART, L"", -1, L"", -1, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abc", 0, L"", 0, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abc", -1, L"", -1, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, L"abc", -1, L"", -1, FALSE, 3, ERROR_SUCCESS);
    CHECK(FIND_STARTSWITH, L"abc", -1, L"", -1, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_ENDSWITH, L"abc", -1, L"", -1, FALSE, 3, ERROR_SUCCESS);

    /* the lengths are the ones given, not the ones of the strings */
    CHECK(FIND_FROMSTART, L"abc", 2, L"bc", -1, FALSE, -1, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abc", 3, L"bc", 2, FALSE, 1, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abc", 3, L"bcd", 3, FALSE, -1, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"abc", 2, L"abcd", 4, FALSE, -1, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, Nul, 3, B, 1, FALSE, 2, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, L"aaa", 3, L"aa", 2, FALSE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, L"aaa", 3, L"aa", 2, FALSE, 1, ERROR_SUCCESS);

    /* the case is folded for the whole Unicode range, not for ASCII only */
    CHECK(FIND_FROMSTART, Umlauts, -1, UpperUmlaut, -1, TRUE, 0, ERROR_SUCCESS);
    CHECK(FIND_FROMEND, Umlauts, -1, UpperUmlaut, -1, TRUE, 1, ERROR_SUCCESS);
    CHECK(FIND_FROMSTART, Umlauts, -1, UpperUmlaut, -1, FALSE, 1, ERROR_SUCCESS);
}
