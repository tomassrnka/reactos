/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for PathCanonicalizeW and PathCombineW with dot segments
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <shlwapi.h>

typedef struct
{
    PCWSTR Directory;   /* NULL for a PathCanonicalizeW test of File alone */
    PCWSTR File;
    PCWSTR Expected;
} PATH_CASE;

static const PATH_CASE TestCases[] =
{
    /* A name that merely starts with dots is an ordinary name (CORE-20129) */
    { NULL, L"C:\\...xx", L"C:\\...xx" },
    { NULL, L"C:\\foo\\...xx", L"C:\\foo\\...xx" },
    { NULL, L"C:\\foo\\..bar", L"C:\\foo\\..bar" },
    { NULL, L"C:\\foo\\..bar\\baz", L"C:\\foo\\..bar\\baz" },
    { NULL, L"C:\\foo\\...\\x", L"C:\\foo\\...\\x" },
    { NULL, L"C:\\a\\b\\.....\\c", L"C:\\a\\b\\.....\\c" },
    { NULL, L"\\...x", L"\\...x" },
    { NULL, L"...xx", L"...xx" },
    { NULL, L"a\\...xx", L"a\\...xx" },
    { NULL, L"a\\..xx", L"a\\..xx" },
    { NULL, L"C:...x", L"C:...x" },
    { NULL, L"\\\\srv\\share\\...x", L"\\\\srv\\share\\...x" },
    { NULL, L"\\\\srv\\share\\..x", L"\\\\srv\\share\\..x" },
    { L"C:\\", L"...xx", L"C:\\...xx" },
    { L"C:\\foo", L"...xx", L"C:\\foo\\...xx" },
    { L"C:\\foo\\bar", L"..x", L"C:\\foo\\bar\\..x" },
    { L"C:\\foo\\bar", L"...\\x", L"C:\\foo\\bar\\...\\x" },
    { L"C:\\foo", L"....x", L"C:\\foo\\....x" },
    { L"C:\\", L"..xx", L"C:\\..xx" },
    { L"\\\\srv\\share\\", L"...x", L"\\\\srv\\share\\...x" },
    { L"\\\\srv\\share\\", L"..x", L"\\\\srv\\share\\..x" },
    /* A real parent reference still works */
    { NULL, L"C:\\foo\\..", L"C:\\" },
    { NULL, L"C:\\foo\\..\\", L"C:\\" },
    { NULL, L"C:\\foo\\..\\x", L"C:\\x" },
    { NULL, L"C:\\foo\\.x", L"C:\\foo\\.x" },
    { NULL, L"C:\\foo\\.\\x", L"C:\\foo\\x" },
    { NULL, L"C:\\foo\\..\\..\\..", L"C:\\" },
    { L"C:\\foo\\bar", L"..\\x", L"C:\\foo\\x" },
    { L"C:\\foo\\", L"..", L"C:\\" },
    { L"C:\\foo\\", L"..\\", L"C:\\" },
};

START_TEST(PathCanonicalize)
{
    UINT Index;

    for (Index = 0; Index < _countof(TestCases); Index++)
    {
        const PATH_CASE *TestCase = &TestCases[Index];
        WCHAR Buffer[MAX_PATH] = L"";
        PWSTR Result;
        BOOL IsCanonicalized;

        if (TestCase->Directory)
        {
            Result = PathCombineW(Buffer, TestCase->Directory, TestCase->File);
            ok(Result == Buffer, "PathCombineW(%s, %s) returned %p\n",
               wine_dbgstr_w(TestCase->Directory), wine_dbgstr_w(TestCase->File), Result);
            ok(!wcscmp(Buffer, TestCase->Expected), "PathCombineW(%s, %s): expected %s, got %s\n",
               wine_dbgstr_w(TestCase->Directory), wine_dbgstr_w(TestCase->File),
               wine_dbgstr_w(TestCase->Expected), wine_dbgstr_w(Buffer));
        }
        else
        {
            IsCanonicalized = PathCanonicalizeW(Buffer, TestCase->File);
            ok(IsCanonicalized, "PathCanonicalizeW(%s) failed\n", wine_dbgstr_w(TestCase->File));
            ok(!wcscmp(Buffer, TestCase->Expected), "PathCanonicalizeW(%s): expected %s, got %s\n",
               wine_dbgstr_w(TestCase->File), wine_dbgstr_w(TestCase->Expected),
               wine_dbgstr_w(Buffer));
        }
    }
}
