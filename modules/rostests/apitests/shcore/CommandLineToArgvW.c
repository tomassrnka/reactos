/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CommandLineToArgvW of shcore, called directly and through the obsolete contract
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *              The cases are those of the shell32 test by Katayama Hirofumi MZ (Copyright 2023)
 */

#include <apitest.h>
#include <windows.h>

typedef WCHAR **(WINAPI *FN_CommandLineToArgvW)(const WCHAR *, int *);

typedef struct _CASE
{
    int Line;
    const WCHAR *CommandLine;
    int Argc;
    const WCHAR *Arg1;
    const WCHAR *Arg2;
    const WCHAR *Arg3;
} CASE;

#define C(cmd, argc) { __LINE__, cmd, argc, NULL, NULL, NULL }
#define C1(cmd, argc, a1) { __LINE__, cmd, argc, a1, NULL, NULL }
#define C2(cmd, argc, a1, a2) { __LINE__, cmd, argc, a1, a2, NULL }
#define C3(cmd, argc, a1, a2, a3) { __LINE__, cmd, argc, a1, a2, a3 }

static const CASE g_Cases[] =
{
    C(L"", 1),
    C(L"test.exe", 1),
    C(L"test.exe ", 1),
    C(L"test.exe\t", 1),
    C(L"test.exe\r", 1),
    C(L"test.exe\n", 1),
    C(L"test.exe\v", 1),
    C(L"test.exe\f", 1),
    C(L"test.exe\u3000", 1),
    C(L"\"This is a test.exe\"", 1),
    C(L"\"This is a test.exe\" ", 1),
    C(L"\"This is a test.exe\"\t", 1),
    C1(L"\"This is a test.exe\"\r", 2, L"\r"),
    C1(L"\"This is a test.exe\"\n", 2, L"\n"),
    C1(L"\"This is a test.exe\"\v", 2, L"\v"),
    C1(L"\"This is a test.exe\"\f", 2, L"\f"),
    C1(L"\"This is a test.exe\"\u3000", 2, L"\u3000"),
    C1(L"test.exe a", 2, L"a"),
    C1(L"test.exe\ta", 2, L"a"),
    C1(L"test.exe\ra", 2, L"a"),
    C1(L"test.exe\na", 2, L"a"),
    C1(L"test.exe\va", 2, L"a"),
    C1(L"test.exe\fa", 2, L"a"),
    C(L"test.exe\u3000" L"a", 1),
    C1(L"test.exe  a", 2, L"a"),
    C1(L"test.exe \ta", 2, L"a"),
    C1(L"test.exe \ra", 2, L"\ra"),
    C1(L"test.exe \na", 2, L"\na"),
    C1(L"test.exe \va", 2, L"\va"),
    C1(L"test.exe \fa", 2, L"\fa"),
    C1(L"test.exe a ", 2, L"a"),
    C1(L"test.exe a\t", 2, L"a"),
    C1(L"test.exe a\r", 2, L"a\r"),
    C1(L"test.exe a\n", 2, L"a\n"),
    C1(L"test.exe \"a\" ", 2, L"a"),
    C1(L"test.exe \"a\"\t", 2, L"a"),
    C1(L"test.exe \"a\"\r", 2, L"a\r"),
    C1(L"test.exe \u3000" L"a", 2, L"\u3000" L"a"),
    C1(L"test.exe \"a b\"", 2, L"a b"),
    C1(L"test.exe \"a\rb\"", 2, L"a\rb"),
    C3(L"test.exe a b c", 4, L"a", L"b", L"c"),
    C3(L"test.exe a b \"c", 4, L"a", L"b", L"c"),
    C2(L"test.exe \"a b\" \"c d\"", 3, L"a b", L"c d"),
    C2(L"test.exe \"a \" d\"", 3, L"a ", L"d"),
    C2(L"test.exe \"0 1\"\" 2", 3, L"0 1\"", L"2"),
    C1(L"test.exe \"0 1\"\"\" 2", 2, L"0 1\" 2"),
};

static void test_module(const char *Label, FN_CommandLineToArgvW pCommandLineToArgvW)
{
    size_t n;

    for (n = 0; n < ARRAYSIZE(g_Cases); ++n)
    {
        const CASE *Case = &g_Cases[n];
        const WCHAR *Expected[3] = { Case->Arg1, Case->Arg2, Case->Arg3 };
        int Argc = -1, i;
        WCHAR **Argv = pCommandLineToArgvW(Case->CommandLine, &Argc);

        ok(Argv != NULL, "%s, line %d: no result\n", Label, Case->Line);
        if (!Argv)
            continue;
        ok(Argc == Case->Argc, "%s, line %d: %d arguments, expected %d\n", Label, Case->Line, Argc, Case->Argc);
        for (i = 1; i < Argc && i < Case->Argc && i <= 3; ++i)
            ok(Argv[i] && !wcscmp(Argv[i], Expected[i - 1]), "%s, line %d: argument %d is %ls\n", Label, Case->Line, i, Argv[i] ? Argv[i] : L"(null)");
        LocalFree(Argv);
    }
}

START_TEST(CommandLineToArgvW)
{
    HMODULE Shcore = LoadLibraryA("shcore.dll");
    HMODULE Contract = LoadLibraryA("api-ms-win-shcore-obsolete-l1-1-0.dll");
    FN_CommandLineToArgvW pFn;

    if (!Shcore)
    {
        skip("shcore.dll is not present\n");
        return;
    }

    pFn = (FN_CommandLineToArgvW)GetProcAddress(Shcore, "CommandLineToArgvW");
    ok(pFn != NULL, "shcore does not export CommandLineToArgvW\n");
    if (pFn)
        test_module("shcore", pFn);

    if (Contract)
    {
        pFn = (FN_CommandLineToArgvW)GetProcAddress(Contract, "CommandLineToArgvW");
        ok(pFn != NULL, "the obsolete contract does not export CommandLineToArgvW\n");
        if (pFn)
            test_module("contract", pFn);
        FreeLibrary(Contract);
    }
    FreeLibrary(Shcore);
}
