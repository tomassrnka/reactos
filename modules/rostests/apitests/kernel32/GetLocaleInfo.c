/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for GetLocaleInfo(Ex)
 * PROGRAMMER:      Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include "precomp.h"

#ifndef LOCALE_SPARENT
#define LOCALE_SPARENT 0x6d
#endif

typedef
int
WINAPI
FN_GetLocaleInfoEx(
    _In_opt_ LPCWSTR lpLocaleName,
    _In_ LCTYPE  LCType,
    _Out_opt_ LPWSTR lpLCData,
    _In_ int cchData);

FN_GetLocaleInfoEx* pGetLocaleInfoEx = NULL;

static void Test_GetLocaleInfoEx(void)
{
    HMODULE hmodKernel32;
    int Ret;
    ULONG CodePage;

    hmodKernel32 = GetModuleHandleW(L"kernel32.dll");
    pGetLocaleInfoEx = (FN_GetLocaleInfoEx*)GetProcAddress(hmodKernel32, "GetLocaleInfoEx");
    if (pGetLocaleInfoEx == NULL)
    {
        hmodKernel32 = LoadLibraryW(L"kernel32_vista.dll");
        pGetLocaleInfoEx = (FN_GetLocaleInfoEx*)GetProcAddress(hmodKernel32, "GetLocaleInfoEx");
        if (pGetLocaleInfoEx == NULL)
        {
            skip("GetLocaleInfoEx not found in kernel32.dll\n");
            return;
        }
    }

    // Test normal usage
    Ret = pGetLocaleInfoEx(L"en-US",
                           LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                           (WCHAR*)&CodePage,
                           sizeof(DWORD) / sizeof(WCHAR));
    ok_eq_int(Ret, 2);
    ok_eq_long(CodePage, 1252ul);

    // Test with neutral locale
    Ret = pGetLocaleInfoEx(L"en",
                           LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                           NULL,
                           0);
    ok_eq_int(Ret, 2);
    ok_eq_long(CodePage, 1252ul);

    // Test with NULL locale name
    CodePage = 0xdeadbeef;
    Ret = pGetLocaleInfoEx(NULL,
                           LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                           (WCHAR *)&CodePage,
                           sizeof(DWORD) /sizeof(WCHAR));
    ok_eq_int(Ret, 2);
    ok_eq_long(CodePage, 1252ul);

    // Test with empty locale name
    CodePage = 0xdeadbeef;
    Ret = pGetLocaleInfoEx(L"",
                           LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                           (WCHAR *)&CodePage,
                           sizeof(DWORD) /sizeof(WCHAR));
    ok_eq_int(Ret, 2);
    ok_eq_long(CodePage, 1252ul);

    // Test with invalid locale name
    CodePage = 0xdeadbeef;
    Ret = pGetLocaleInfoEx(L"invalid",
                           LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                           (WCHAR *)&CodePage,
                           sizeof(DWORD) /sizeof(WCHAR));
    ok_eq_int(Ret, 0);
    ok_eq_long(GetLastError(), (ULONG)ERROR_INVALID_PARAMETER);
    ok(CodePage == 0xdeadbeef, "CodePage should not have been modified: %lx\n", CodePage);

    // A neutral name keeps its name and its parent, with LOCALE_NOUSEROVERRIDE too (the .NET culture chain)
    {
        static const struct { PCWSTR Locale; LCTYPE Type; PCWSTR Expected; } Names[] =
        {
            { L"en-US", LOCALE_SNAME, L"en-US" },
            { L"en-US", LOCALE_SPARENT, L"en" },
            { L"en-US", LOCALE_SPARENT | LOCALE_NOUSEROVERRIDE, L"en" },
            { L"en", LOCALE_SNAME, L"en" },
            { L"en", LOCALE_SNAME | LOCALE_NOUSEROVERRIDE, L"en" },
            { L"en", LOCALE_SPARENT, L"" },
            { L"en", LOCALE_SPARENT | LOCALE_NOUSEROVERRIDE, L"" },
            { L"de", LOCALE_SPARENT | LOCALE_NOUSEROVERRIDE, L"" },
            { L"", LOCALE_SNAME, L"" },
            { L"", LOCALE_SPARENT, L"" },
            { L"", LOCALE_SPARENT | LOCALE_NOUSEROVERRIDE, L"" },
        };
        WCHAR Buffer[85];
        ULONG i;

        for (i = 0; i < _countof(Names); i++)
        {
            Buffer[0] = L'?';
            Buffer[1] = 0;
            Ret = pGetLocaleInfoEx(Names[i].Locale, Names[i].Type, Buffer, _countof(Buffer));
            ok(Ret == (INT)wcslen(Names[i].Expected) + 1, "%ls 0x%lx: returned %d\n", Names[i].Locale, Names[i].Type, Ret);
            ok(!wcscmp(Buffer, Names[i].Expected), "%ls 0x%lx: got '%ls', expected '%ls'\n",
               Names[i].Locale, Names[i].Type, Buffer, Names[i].Expected);
        }
    }
}

#undef GetLocaleInfo
START_TEST(GetLocaleInfo)
{
    Test_GetLocaleInfoEx();
}
