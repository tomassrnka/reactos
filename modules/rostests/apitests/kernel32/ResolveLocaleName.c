/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for ResolveLocaleName
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef INT (WINAPI *FN_ResolveLocaleName)(LPCWSTR, LPWSTR, INT);
typedef INT (WINAPI *FN_GetLocaleName)(LPWSTR, INT);

static FN_ResolveLocaleName pResolveLocaleName;

static void TestResolve(LPCWSTR Name, LPCWSTR Expected, int Line)
{
    WCHAR Buffer[LOCALE_NAME_MAX_LENGTH];
    INT Ret, Size;

    SetLastError(0xdeadbeef);
    Size = pResolveLocaleName(Name, NULL, 0);
    ok_(__FILE__, Line)(Size == (INT)wcslen(Expected) + 1, "%S: size %d, expected %u\n", Name, Size, (UINT)wcslen(Expected) + 1);
    ok_(__FILE__, Line)(GetLastError() == 0xdeadbeef, "%S: GetLastError() = %lu\n", Name, GetLastError());

    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    Ret = pResolveLocaleName(Name, Buffer, ARRAYSIZE(Buffer));
    ok_(__FILE__, Line)(Ret == (INT)wcslen(Expected) + 1, "%S: ret %d, expected %u\n", Name, Ret, (UINT)wcslen(Expected) + 1);
    if (Ret)
        ok_(__FILE__, Line)(!wcscmp(Buffer, Expected), "%S: got '%S', expected '%S'\n", Name, Buffer, Expected);
}

static void TestInvalid(LPCWSTR Name, int Line)
{
    WCHAR Buffer[LOCALE_NAME_MAX_LENGTH];
    INT Ret;

    SetLastError(0xdeadbeef);
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    Ret = pResolveLocaleName(Name, Buffer, ARRAYSIZE(Buffer));
    if (Ret)
        ok_(__FILE__, Line)(Ret == 1 && !Buffer[0] && !is_reactos() && LOBYTE(LOWORD(GetVersion())) == 6 && HIBYTE(LOWORD(GetVersion())) == 1,
                            "%S: ret %d\n", Name, Ret);
    else
        ok_(__FILE__, Line)(GetLastError() == ERROR_INVALID_PARAMETER, "%S: GetLastError() = %lu\n", Name, GetLastError());
}

/* Without NT 6.1 exports (DLL_EXPORT_VERSION below 0x601) ReactOS does not export the function */
static BOOL HasWin7Exports(HMODULE hKernel32)
{
    return GetProcAddress(hKernel32, "K32EnumProcesses") != NULL;
}

START_TEST(ResolveLocaleName)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_GetLocaleName pGetUserDefaultLocaleName;
    WCHAR Buffer[LOCALE_NAME_MAX_LENGTH], Default[LOCALE_NAME_MAX_LENGTH];
    INT Ret;

    pResolveLocaleName = (FN_ResolveLocaleName)GetProcAddress(hKernel32, "ResolveLocaleName");
    if (!pResolveLocaleName && is_reactos() && !HasWin7Exports(hKernel32))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pResolveLocaleName != NULL, "ResolveLocaleName is not exported\n");
    if (!pResolveLocaleName)
        return;

    /* Specific names resolve to themselves, in their canonical case */
    TestResolve(L"en-US", L"en-US", __LINE__);
    TestResolve(L"en-us", L"en-US", __LINE__);
    TestResolve(L"de-AT", L"de-AT", __LINE__);
    TestResolve(L"fr-CA", L"fr-CA", __LINE__);

    /* Alternate sorts resolve to their locale */
    TestResolve(L"de-DE_phoneb", L"de-DE", __LINE__);
    TestResolve(L"DE-de-phoneb", L"de-DE", __LINE__);
    TestResolve(L"ja-jp_radstr", L"ja-JP", __LINE__);

    /* Neutral names resolve to the specific locale they stand for */
    TestResolve(L"en", L"en-US", __LINE__);
    TestResolve(L"de", L"de-DE", __LINE__);
    TestResolve(L"fr", L"fr-FR", __LINE__);
    TestResolve(L"es", L"es-ES", __LINE__);
    TestResolve(L"cs", L"cs-CZ", __LINE__);
    TestResolve(L"ga", L"ga-IE", __LINE__);
    TestResolve(L"zh", L"zh-CN", __LINE__);
    TestResolve(L"zh-hans", L"zh-CN", __LINE__);
    TestResolve(L"zh-Hant", L"zh-HK", __LINE__);
    TestResolve(L"az", L"az-Latn-AZ", __LINE__);
    TestResolve(L"uz", L"uz-Latn-UZ", __LINE__);
    TestResolve(L"uz-cyrl", L"uz-Cyrl-UZ", __LINE__);

    /* Unknown regions and tags fall back to the language */
    TestResolve(L"en-RR", L"en-US", __LINE__);
    TestResolve(L"EN-zz", L"en-US", __LINE__);
    TestResolve(L"de-XY", L"de-DE", __LINE__);
    TestResolve(L"fr-CHXX", L"fr-FR", __LINE__);

    /* Nothing matches: the invariant locale */
    TestResolve(L"zz", L"", __LINE__);
    TestResolve(L"zzz-ZZZ", L"", __LINE__);
    TestResolve(L"zzzz", L"", __LINE__);
    TestResolve(L"", L"", __LINE__);

    /*
     * Characters a locale name cannot have, LOCALE_NAME_SYSTEM_DEFAULT ("!sys-default-locale") included:
     * Windows 10 fails, Windows 7 returns the invariant locale
     */
    TestInvalid(L"zz+XX", __LINE__);
    TestInvalid(L"!sys-default-locale", __LINE__);

    /* NULL is the user default locale */
    pGetUserDefaultLocaleName = (FN_GetLocaleName)GetProcAddress(hKernel32, "GetUserDefaultLocaleName");
    if (pGetUserDefaultLocaleName && pGetUserDefaultLocaleName(Default, ARRAYSIZE(Default)))
        TestResolve(NULL, Default, __LINE__);

    /* Buffer too small: as much of the name as fits */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    SetLastError(0xdeadbeef);
    Ret = pResolveLocaleName(L"en-US", Buffer, 4);
    ok(Ret == 0, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetLastError() = %lu\n", GetLastError());
    ok(!wcscmp(Buffer, L"en-"), "Buffer = '%.*S'\n", 4, Buffer);
    ok(Buffer[4] == 0xCCCC, "Buffer[4] = 0x%x\n", Buffer[4]);

    /* Exact size */
    Ret = pResolveLocaleName(L"en-US", Buffer, 6);
    ok(Ret == 6, "Ret = %d\n", Ret);
    ok(!wcscmp(Buffer, L"en-US"), "Buffer = '%S'\n", Buffer);
}
