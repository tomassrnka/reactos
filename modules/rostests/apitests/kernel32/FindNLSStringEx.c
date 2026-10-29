/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for FindNLSStringEx (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The .NET runtime uses it for culture-aware IndexOf, StartsWith and EndsWith when it has no ICU. The vectors
 * stay with matches as long as the value, which is what a sliding comparison and Windows agree on.
 */

#include "precomp.h"

#ifndef LINGUISTIC_IGNORECASE
#define LINGUISTIC_IGNORECASE 0x00000010
#endif

typedef INT (WINAPI *PFN_FindNLSStringEx)(LPCWSTR, DWORD, LPCWSTR, INT, LPCWSTR, INT, LPINT, LPNLSVERSIONINFO,
                                          LPVOID, LPARAM);

#define PENDING_FOUND 0x5a5a

static PFN_FindNLSStringEx pFindNLSStringEx;

static void Check(LPCWSTR Locale, DWORD Flags, LPCWSTR Source, INT SourceLength, LPCWSTR Value, INT ValueLength,
                  INT Expected, INT ExpectedFound, int Line)
{
    INT Result, Found = PENDING_FOUND;

    Result = pFindNLSStringEx(Locale, Flags, Source, SourceLength, Value, ValueLength, &Found, NULL, NULL, 0);
    ok(Result == Expected, "line %d: got %d, expected %d\n", Line, Result, Expected);
    ok(Found == ExpectedFound, "line %d: found %d, expected %d\n", Line, Found, ExpectedFound);
}

static void CheckError(LPCWSTR Source, INT SourceLength, LPCWSTR Value, INT ValueLength, LPNLSVERSIONINFO Version,
                       LPVOID Reserved, LPARAM Handle, DWORD ExpectedError, int Line)
{
    INT Result, Found = PENDING_FOUND;

    SetLastError(0xdeadbeef);
    Result = pFindNLSStringEx(L"en-US", FIND_FROMSTART, Source, SourceLength, Value, ValueLength, &Found,
                              Version, Reserved, Handle);
    ok(Result == -1, "line %d: got %d\n", Line, Result);
    ok(GetLastError() == ExpectedError, "line %d: error %lu, expected %lu\n", Line, GetLastError(), ExpectedError);
    ok(Found == PENDING_FOUND, "line %d: found was written (%d)\n", Line, Found);
}

START_TEST(FindNLSStringEx)
{
    pFindNLSStringEx = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FindNLSStringEx");
    if (!pFindNLSStringEx)
    {
        skip("FindNLSStringEx is not exported\n");
        return;
    }

    /* The four directions */
    Check(L"en-US", FIND_FROMSTART, L"abcabc", -1, L"abc", -1, 0, 3, __LINE__);
    Check(L"en-US", FIND_FROMSTART, L"xxabcabc", -1, L"abc", -1, 2, 3, __LINE__);
    Check(L"en-US", FIND_FROMEND, L"abcabcxx", -1, L"abc", -1, 3, 3, __LINE__);
    Check(L"en-US", FIND_STARTSWITH, L"abcdef", -1, L"abc", -1, 0, 3, __LINE__);
    Check(L"en-US", FIND_STARTSWITH, L"xabcdef", -1, L"abc", -1, -1, PENDING_FOUND, __LINE__);
    Check(L"en-US", FIND_ENDSWITH, L"abcdef", -1, L"def", -1, 3, 3, __LINE__);
    Check(L"en-US", FIND_ENDSWITH, L"abcdefx", -1, L"def", -1, -1, PENDING_FOUND, __LINE__);

    /* Case is significant unless asked otherwise */
    Check(L"en-US", FIND_FROMSTART, L"Hello World", -1, L"WORLD", -1, -1, PENDING_FOUND, __LINE__);
    Check(L"en-US", FIND_FROMSTART | NORM_IGNORECASE, L"Hello World", -1, L"WORLD", -1, 6, 5, __LINE__);
    Check(L"en-US", FIND_FROMSTART | LINGUISTIC_IGNORECASE, L"Hello World", -1, L"wOrLd", -1, 6, 5, __LINE__);

    /* Explicit lengths, the user default locale and the invariant one */
    Check(L"en-US", FIND_FROMSTART, L"abcabc", 4, L"bcab", 3, 1, 3, __LINE__);
    Check(L"en-US", FIND_FROMSTART, L"abcabc", 4, L"cab", -1, -1, PENDING_FOUND, __LINE__);
    Check(L"en-US", FIND_FROMSTART, L"abcabc", 5, L"cab", -1, 2, 3, __LINE__);
    Check(NULL, FIND_FROMSTART, L"path\\to\\file", -1, L"\\", -1, 4, 1, __LINE__);
    Check(L"", FIND_FROMEND, L"path\\to\\file", -1, L"\\", -1, 7, 1, __LINE__);

    /* A value longer than the source is not found, and that is no error */
    SetLastError(0xdeadbeef);
    Check(L"en-US", FIND_FROMSTART, L"ab", -1, L"abc", -1, -1, PENDING_FOUND, __LINE__);

    /* Bad arguments */
    CheckError(NULL, -1, L"a", -1, NULL, NULL, 0, ERROR_INVALID_PARAMETER, __LINE__);
    CheckError(L"abc", -1, NULL, -1, NULL, NULL, 0, ERROR_INVALID_PARAMETER, __LINE__);
    CheckError(L"abc", -2, L"a", -1, NULL, NULL, 0, ERROR_INVALID_PARAMETER, __LINE__);
    CheckError(L"abc", -1, L"a", -2, NULL, NULL, 0, ERROR_INVALID_PARAMETER, __LINE__);

    /* More than one direction is a flag error; no direction means from the start (measured on Windows 10 22H2) */
    {
        static const DWORD Bad[] = { FIND_FROMSTART | FIND_FROMEND, FIND_STARTSWITH | FIND_ENDSWITH,
                                     FIND_FROMSTART | FIND_STARTSWITH, FIND_FROMEND | FIND_ENDSWITH,
                                     FIND_FROMSTART | FIND_FROMEND | FIND_STARTSWITH | FIND_ENDSWITH };
        INT Found, i;

        for (i = 0; i < (INT)_countof(Bad); i++)
        {
            Found = PENDING_FOUND;
            SetLastError(0xdeadbeef);
            ok(pFindNLSStringEx(L"en-US", Bad[i], L"abcabc", -1, L"abc", -1, &Found, NULL, NULL, 0) == -1,
               "flags 0x%lx\n", Bad[i]);
            ok(GetLastError() == ERROR_INVALID_FLAGS, "flags 0x%lx: error %lu\n", Bad[i], GetLastError());
            ok(Found == PENDING_FOUND, "flags 0x%lx: found was written (%d)\n", Bad[i], Found);
        }
    }
    Check(L"en-US", 0, L"xxabcabc", -1, L"abc", -1, 2, 3, __LINE__);
    Check(L"en-US", 0, L"xabc", -1, L"abc", -1, 1, 3, __LINE__);

    /* The reserved pointer is not looked at (measured on Windows 10 22H2) */
    {
        INT Found = PENDING_FOUND;
        ok(pFindNLSStringEx(L"en-US", FIND_FROMSTART, L"abc", -1, L"c", -1, &Found, NULL, (LPVOID)1, 0) == 2,
           "with a reserved pointer\n");
        ok(Found == 1, "found %d\n", Found);
    }
}
