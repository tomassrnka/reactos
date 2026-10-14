/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for versionlie shims
 * COPYRIGHT:   Copyright 2015-2018 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#ifdef __REACTOS__
#include <ntndk.h>
#else
#include <winternl.h>
#endif
#include "wine/test.h"
#include <strsafe.h>

#include "appshim_apitest.h"

static tGETHOOKAPIS pGetHookAPIs;


static DWORD g_WinVersion;

#define FLAG_BUGGY_ServicePackMajorMinor    1
#define FLAG_AlternateHookOrder             2

typedef struct VersionLieInfo
{
    DWORD FullVersion;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wFlags;
    /* ReactOS: what the registry shows for the NT 6 and 10 lies */
    PCWSTR pszCurrentVersion;
    DWORD dwRegMajorVersion;
} VersionLieInfo;

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY   0x00010000
#define RRF_SUBKEY_WOW6432KEY   0x00020000
#endif

#define NUM_HOOKS_NT6   12

typedef BOOL(WINAPI* GETVERSIONEXAPROC)(LPOSVERSIONINFOEXA);
typedef BOOL(WINAPI* GETVERSIONEXWPROC)(LPOSVERSIONINFOEXW);
typedef DWORD(WINAPI* GETVERSIONPROC)(void);

void expect_shim_imp(PHOOKAPI hook, PCSTR library, PCSTR function, PCSTR shim, int* same)
{
    int lib = lstrcmpA(library, hook->LibraryName);
    int fn = lstrcmpA(function, hook->FunctionName);
    winetest_ok(lib == 0, "Expected LibrarayName to be %s, was: %s for %s\n", library, hook->LibraryName, shim);
    winetest_ok(fn == 0, "Expected FunctionName to be %s, was: %s for %s\n", function, hook->FunctionName, shim);
    *same = (lib == 0 && fn == 0);
}

static void verify_shima_imp(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim, int same)
{
    OSVERSIONINFOEXA v1 = { sizeof(v1), 0 }, v2 = { sizeof(v2), 0 };
    BOOL ok1, ok2;

    if (!same)
    {
        skip("Skipping implementation tests for %s\n", shim);
        return;
    }

    while (v1.dwOSVersionInfoSize)
    {
        ok1 = GetVersionExA((LPOSVERSIONINFOA)&v1);
        hook->OriginalFunction = GetVersionExA;

        ok2 = ((GETVERSIONEXAPROC)hook->ReplacementFunction)(&v2);

        winetest_ok(ok1 == ok2, "Expected ok1 to equal ok2, was: %i, %i for %s\n", ok1, ok2, shim);
        if (ok1 && ok2)
        {
            char szCSDVersion[128] = "";
            winetest_ok(v1.dwOSVersionInfoSize == v2.dwOSVersionInfoSize, "Expected dwOSVersionInfoSize to be equal, was: %u, %u for %s\n", v1.dwOSVersionInfoSize, v2.dwOSVersionInfoSize, shim);
            winetest_ok(info->dwMajorVersion == v2.dwMajorVersion, "Expected dwMajorVersion to be equal, was: %u, %u for %s\n", info->dwMajorVersion, v2.dwMajorVersion, shim);
            winetest_ok(info->dwMinorVersion == v2.dwMinorVersion, "Expected dwMinorVersion to be equal, was: %u, %u for %s\n", info->dwMinorVersion, v2.dwMinorVersion, shim);
            winetest_ok(info->dwBuildNumber == v2.dwBuildNumber, "Expected dwBuildNumber to be equal, was: %u, %u for %s\n", info->dwBuildNumber, v2.dwBuildNumber, shim);
            winetest_ok(info->dwPlatformId == v2.dwPlatformId, "Expected dwPlatformId to be equal, was: %u, %u for %s\n", info->dwPlatformId, v2.dwPlatformId, shim);

            if (info->wServicePackMajor)
                StringCchPrintfA(szCSDVersion, _countof(szCSDVersion), "Service Pack %u", info->wServicePackMajor);
            winetest_ok(lstrcmpA(szCSDVersion, v2.szCSDVersion) == 0, "Expected szCSDVersion to be equal, was: %s, %s for %s\n", szCSDVersion, v2.szCSDVersion, shim);

            if (v1.dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
            {
                if (!(info->wFlags & FLAG_BUGGY_ServicePackMajorMinor))
                {
                    winetest_ok(info->wServicePackMajor == v2.wServicePackMajor, "Expected wServicePackMajor to be equal, was: %i, %i for %s\n", info->wServicePackMajor, v2.wServicePackMajor, shim);
                    winetest_ok(info->wServicePackMinor == v2.wServicePackMinor, "Expected wServicePackMinor to be equal, was: %i, %i for %s\n", info->wServicePackMinor, v2.wServicePackMinor, shim);
                }
                else
                {
                    winetest_ok(v1.wServicePackMajor == v2.wServicePackMajor, "Expected wServicePackMajor to be equal, was: %i, %i for %s\n", v1.wServicePackMajor, v2.wServicePackMajor, shim);
                    winetest_ok(v1.wServicePackMinor == v2.wServicePackMinor, "Expected wServicePackMinor to be equal, was: %i, %i for %s\n", v1.wServicePackMinor, v2.wServicePackMinor, shim);
                }
                winetest_ok(v1.wSuiteMask == v2.wSuiteMask, "Expected wSuiteMask to be equal, was: %i, %i for %s\n", v1.wSuiteMask, v2.wSuiteMask, shim);
                winetest_ok(v1.wProductType == v2.wProductType, "Expected wProductType to be equal, was: %i, %i for %s\n", v1.wProductType, v2.wProductType, shim);
                winetest_ok(v1.wReserved == v2.wReserved, "Expected wReserved to be equal, was: %i, %i for %s\n", v1.wReserved, v2.wReserved, shim);
            }
            else
            {
                winetest_ok(v1.wServicePackMajor == 0 && v2.wServicePackMajor == 0, "Expected wServicePackMajor to be 0, was: %i, %i for %s\n", v1.wServicePackMajor, v2.wServicePackMajor, shim);
                winetest_ok(v1.wServicePackMinor == 0 && v2.wServicePackMinor == 0, "Expected wServicePackMinor to be 0, was: %i, %i for %s\n", v1.wServicePackMinor, v2.wServicePackMinor, shim);
                winetest_ok(v1.wSuiteMask == 0 && v2.wSuiteMask == 0, "Expected wSuiteMask to be 0, was: %i, %i for %s\n", v1.wSuiteMask, v2.wSuiteMask, shim);
                winetest_ok(v1.wProductType == 0 && v2.wProductType == 0, "Expected wProductType to be 0, was: %i, %i for %s\n", v1.wProductType, v2.wProductType, shim);
                winetest_ok(v1.wReserved == 0 && v2.wReserved == 0, "Expected wReserved to be 0, was: %i, %i for %s\n", v1.wReserved, v2.wReserved, shim);
            }
        }

        ZeroMemory(&v1, sizeof(v1));
        ZeroMemory(&v2, sizeof(v2));
        if (v1.dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
            v1.dwOSVersionInfoSize = v2.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
    }
}

static void verify_shimw_imp(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim, int same, int first_might_be_broken)
{
    OSVERSIONINFOEXW v1 = { sizeof(v1), 0 }, v2 = { sizeof(v2), 0 };
    BOOL ok1, ok2, first = TRUE;

    if (!same)
    {
        skip("Skipping implementation tests for %s\n", shim);
        return;
    }

    while (v1.dwOSVersionInfoSize)
    {
        ok1 = GetVersionExW((LPOSVERSIONINFOW)&v1);
        hook->OriginalFunction = GetVersionExW;

        ok2 = ((GETVERSIONEXWPROC)hook->ReplacementFunction)(&v2);

        if (first_might_be_broken && first && ok1 == TRUE && ok2 == FALSE)
        {
            skip("Skipping first check because 0x%x is (falsely) not accepted by the shim %s\n", sizeof(v1), shim);
        }
        else
        {
            winetest_ok(ok1 == ok2, "Expected ok1 to equal ok2, was: %i, %i for %s(first:%d)\n", ok1, ok2, shim, first);
        }
        if (ok1 && ok2)
        {
            WCHAR szCSDVersion[128] = { 0 };
            winetest_ok(v1.dwOSVersionInfoSize == v2.dwOSVersionInfoSize, "Expected dwOSVersionInfoSize to be equal, was: %u, %u for %s\n", v1.dwOSVersionInfoSize, v2.dwOSVersionInfoSize, shim);
            winetest_ok(info->dwMajorVersion == v2.dwMajorVersion, "Expected dwMajorVersion to be equal, was: %u, %u for %s\n", info->dwMajorVersion, v2.dwMajorVersion, shim);
            winetest_ok(info->dwMinorVersion == v2.dwMinorVersion, "Expected dwMinorVersion to be equal, was: %u, %u for %s\n", info->dwMinorVersion, v2.dwMinorVersion, shim);
            winetest_ok(info->dwBuildNumber == v2.dwBuildNumber, "Expected dwBuildNumber to be equal, was: %u, %u for %s\n", info->dwBuildNumber, v2.dwBuildNumber, shim);
            winetest_ok(info->dwPlatformId == v2.dwPlatformId, "Expected dwPlatformId to be equal, was: %u, %u for %s\n", info->dwPlatformId, v2.dwPlatformId, shim);

            if (info->wServicePackMajor)
                StringCchPrintfW(szCSDVersion, _countof(szCSDVersion), L"Service Pack %u", info->wServicePackMajor);
            winetest_ok(lstrcmpW(szCSDVersion, v2.szCSDVersion) == 0, "Expected szCSDVersion to be equal, was: %s, %s for %s\n", wine_dbgstr_w(szCSDVersion), wine_dbgstr_w(v2.szCSDVersion), shim);

            if (v1.dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
            {
                if (!(info->wFlags & FLAG_BUGGY_ServicePackMajorMinor))
                {
                    winetest_ok(info->wServicePackMajor == v2.wServicePackMajor, "Expected wServicePackMajor to be equal, was: %i, %i for %s\n", info->wServicePackMajor, v2.wServicePackMajor, shim);
                    winetest_ok(info->wServicePackMinor == v2.wServicePackMinor, "Expected wServicePackMinor to be equal, was: %i, %i for %s\n", info->wServicePackMinor, v2.wServicePackMinor, shim);
                }
                else
                {
                    winetest_ok(v1.wServicePackMajor == v2.wServicePackMajor, "Expected wServicePackMajor to be equal, was: %i, %i for %s\n", v1.wServicePackMajor, v2.wServicePackMajor, shim);
                    winetest_ok(v1.wServicePackMinor == v2.wServicePackMinor, "Expected wServicePackMinor to be equal, was: %i, %i for %s\n", v1.wServicePackMinor, v2.wServicePackMinor, shim);
                }
                winetest_ok(v1.wSuiteMask == v2.wSuiteMask, "Expected wSuiteMask to be equal, was: %i, %i for %s\n", v1.wSuiteMask, v2.wSuiteMask, shim);
                winetest_ok(v1.wProductType == v2.wProductType, "Expected wProductType to be equal, was: %i, %i for %s\n", v1.wProductType, v2.wProductType, shim);
                winetest_ok(v1.wReserved == v2.wReserved, "Expected wReserved to be equal, was: %i, %i for %s\n", v1.wReserved, v2.wReserved, shim);
            }
            else
            {
                winetest_ok(v1.wServicePackMajor == 0 && v2.wServicePackMajor == 0, "Expected wServicePackMajor to be 0, was: %i, %i for %s\n", v1.wServicePackMajor, v2.wServicePackMajor, shim);
                winetest_ok(v1.wServicePackMinor == 0 && v2.wServicePackMinor == 0, "Expected wServicePackMinor to be 0, was: %i, %i for %s\n", v1.wServicePackMinor, v2.wServicePackMinor, shim);
                winetest_ok(v1.wSuiteMask == 0 && v2.wSuiteMask == 0, "Expected wSuiteMask to be 0, was: %i, %i for %s\n", v1.wSuiteMask, v2.wSuiteMask, shim);
                winetest_ok(v1.wProductType == 0 && v2.wProductType == 0, "Expected wProductType to be 0, was: %i, %i for %s\n", v1.wProductType, v2.wProductType, shim);
                winetest_ok(v1.wReserved == 0 && v2.wReserved == 0, "Expected wReserved to be 0, was: %i, %i for %s\n", v1.wReserved, v2.wReserved, shim);
            }
        }

        ZeroMemory(&v1, sizeof(v1));
        ZeroMemory(&v2, sizeof(v2));
        if (v1.dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
            v1.dwOSVersionInfoSize = v2.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
        first = FALSE;
    }
}

/* RtlGetVersion returns an NTSTATUS, its original function is not GetVersionExW */
static void verify_rtlgetversion_imp(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim, int same)
{
    RTL_OSVERSIONINFOEXW v = { sizeof(v) };
    NTSTATUS status;

    if (!same)
    {
        skip("Skipping implementation tests for %s\n", shim);
        return;
    }

    hook->OriginalFunction = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    status = ((NTSTATUS(NTAPI*)(PRTL_OSVERSIONINFOEXW))hook->ReplacementFunction)(&v);
    winetest_ok(status == STATUS_SUCCESS, "Expected RtlGetVersion to succeed, was: 0x%x for %s\n", status, shim);
    if (status == STATUS_SUCCESS)
    {
        winetest_ok(info->dwMajorVersion == v.dwMajorVersion, "Expected dwMajorVersion to be equal, was: %u, %u for %s\n", info->dwMajorVersion, v.dwMajorVersion, shim);
        winetest_ok(info->dwMinorVersion == v.dwMinorVersion, "Expected dwMinorVersion to be equal, was: %u, %u for %s\n", info->dwMinorVersion, v.dwMinorVersion, shim);
        winetest_ok(info->dwBuildNumber == v.dwBuildNumber, "Expected dwBuildNumber to be equal, was: %u, %u for %s\n", info->dwBuildNumber, v.dwBuildNumber, shim);
        winetest_ok(info->dwPlatformId == v.dwPlatformId, "Expected dwPlatformId to be equal, was: %u, %u for %s\n", info->dwPlatformId, v.dwPlatformId, shim);
    }
}

static void verify_shim_imp(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim, int same)
{
    DWORD ver;
    if (!same)
    {
        skip("Skipping implementation tests for %s\n", shim);
        return;
    }
    ver = ((GETVERSIONPROC)hook->ReplacementFunction)();
    winetest_ok(info->FullVersion == ver, "Expected GetVersion to return 0x%x, was: 0x%x for %s\n", info->FullVersion, ver, shim);
}


#define verify_shima  (winetest_set_location(__FILE__, __LINE__), 0) ? (void)0 : verify_shima_imp
#define verify_shimw  (winetest_set_location(__FILE__, __LINE__), 0) ? (void)0 : verify_shimw_imp
#define verify_shim  (winetest_set_location(__FILE__, __LINE__), 0) ? (void)0 : verify_shim_imp
#define verify_rtlgetversion  (winetest_set_location(__FILE__, __LINE__), 0) ? (void)0 : verify_rtlgetversion_imp




typedef BOOL(WINAPI* VERIFYAPROC)(LPOSVERSIONINFOEXA, DWORD, DWORDLONG);
typedef BOOL(WINAPI* VERIFYWPROC)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);
typedef NTSTATUS(NTAPI* RTLVERIFYPROC)(PRTL_OSVERSIONINFOEXW, ULONG, ULONGLONG);
typedef VOID(NTAPI* RTLGETNTVERSIONNUMBERSPROC)(PULONG, PULONG, PULONG);
typedef LONG(WINAPI* REGQUERYEXWPROC)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* REGQUERYEXAPROC)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* REGGETVALUEWPROC)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LONG(WINAPI* REGGETVALUEAPROC)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);

/* One VerifyVersionInfo question: the fields and the comparison for each */
typedef struct VerifyCase
{
    DWORD Major, Minor, SpMajor;
    DWORD Mask;
    DWORD CondMajor, CondMinor, CondSpMajor;
} VerifyCase;

static DWORDLONG make_condition(const VerifyCase* c)
{
    DWORDLONG mask = 0;
    if (c->Mask & VER_MAJORVERSION)
        mask = VerSetConditionMask(mask, VER_MAJORVERSION, (BYTE)c->CondMajor);
    if (c->Mask & VER_MINORVERSION)
        mask = VerSetConditionMask(mask, VER_MINORVERSION, (BYTE)c->CondMinor);
    if (c->Mask & VER_SERVICEPACKMAJOR)
        mask = VerSetConditionMask(mask, VER_SERVICEPACKMAJOR, (BYTE)c->CondSpMajor);
    return mask;
}

static void verify_nt6_verifyinfo(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim)
{
    /* Wanted values are given as the lie's value plus a delta */
    static const struct
    {
        int dMajor, dMinor, dSp;
        DWORD Mask;
        DWORD CondMajor, CondMinor, CondSp;
        BOOL Expected;
    } cases[] =
    {
        { 0, 0, 0, VER_MAJORVERSION, VER_EQUAL, 0, 0, TRUE },
        { 1, 0, 0, VER_MAJORVERSION, VER_EQUAL, 0, 0, FALSE },
        { -1, 0, 0, VER_MAJORVERSION, VER_EQUAL, 0, 0, FALSE },
        { 0, 0, 0, VER_MAJORVERSION, VER_GREATER_EQUAL, 0, 0, TRUE },
        { 1, 0, 0, VER_MAJORVERSION, VER_GREATER_EQUAL, 0, 0, FALSE },
        { -1, 0, 0, VER_MAJORVERSION, VER_GREATER_EQUAL, 0, 0, TRUE },
        { 0, 0, 0, VER_MAJORVERSION, VER_GREATER, 0, 0, FALSE },
        { -1, 0, 0, VER_MAJORVERSION, VER_GREATER, 0, 0, TRUE },
        { 0, 0, 0, VER_MAJORVERSION, VER_LESS, 0, 0, FALSE },
        { 1, 0, 0, VER_MAJORVERSION, VER_LESS, 0, 0, TRUE },
        { 0, 0, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, TRUE },
        { 0, 1, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, FALSE },
        { -1, 9, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, TRUE },
        { 1, 0, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, FALSE },
        { 0, 0, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_EQUAL, VER_EQUAL, 0, TRUE },
        { 0, 1, 0, VER_MAJORVERSION | VER_MINORVERSION, VER_EQUAL, VER_EQUAL, 0, FALSE },
        { 0, 0, 0, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, TRUE },
        { 0, 0, 1, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, FALSE },
        { 0, 1, 0, VER_MINORVERSION, VER_LESS, 0, 0, TRUE },
        { 0, 0, 0, VER_MINORVERSION, VER_LESS, 0, 0, FALSE },
    };
    int n;
    DWORD err;

    hook[4].OriginalFunction = VerifyVersionInfoA;
    hook[5].OriginalFunction = VerifyVersionInfoW;

    for (n = 0; n < ARRAYSIZE(cases); ++n)
    {
        OSVERSIONINFOEXW wantw = { sizeof(wantw) };
        OSVERSIONINFOEXA wanta = { sizeof(wanta) };
        DWORDLONG cond;
        VerifyCase vc;
        BOOL res;
        /* A row that names one condition uses it for every field it tests */
        vc.Major = info->dwMajorVersion + cases[n].dMajor;
        vc.Minor = info->dwMinorVersion + cases[n].dMinor;
        vc.SpMajor = info->wServicePackMajor + cases[n].dSp;
        vc.Mask = cases[n].Mask;
        vc.CondMajor = cases[n].CondMajor;
        vc.CondMinor = cases[n].CondMinor ? cases[n].CondMinor : cases[n].CondMajor;
        vc.CondSpMajor = cases[n].CondSp ? cases[n].CondSp : cases[n].CondMajor;
        cond = make_condition(&vc);

        wantw.dwMajorVersion = wanta.dwMajorVersion = vc.Major;
        wantw.dwMinorVersion = wanta.dwMinorVersion = vc.Minor;
        wantw.wServicePackMajor = wanta.wServicePackMajor = (WORD)vc.SpMajor;

        SetLastError(0xdeadbeef);
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&wantw, vc.Mask, cond);
        err = GetLastError();
        winetest_ok(res == cases[n].Expected, "VerifyVersionInfoW case %d: expected %d, got %d for %s\n", n, cases[n].Expected, res, shim);
        if (!cases[n].Expected && !res)
            winetest_ok(err == ERROR_OLD_WIN_VERSION, "VerifyVersionInfoW case %d: expected error %u, got %u for %s\n", n, ERROR_OLD_WIN_VERSION, err, shim);

        res = ((VERIFYAPROC)hook[4].ReplacementFunction)(&wanta, vc.Mask, cond);
        winetest_ok(res == cases[n].Expected, "VerifyVersionInfoA case %d: expected %d, got %d for %s\n", n, cases[n].Expected, res, shim);
    }

    /* The cases of the kernel32 version test, with the lie as the current version */
    test_verify_version_cases(hook[4].ReplacementFunction, info->dwMajorVersion, info->dwMinorVersion, info->dwBuildNumber,
                              info->wServicePackMajor, info->wServicePackMinor, shim);

    /* Build number and platform */
    {
        OSVERSIONINFOEXW want = { sizeof(want) };
        DWORDLONG cond = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        BOOL res;

        want.dwBuildNumber = info->dwBuildNumber;
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&want, VER_BUILDNUMBER, cond);
        winetest_ok(res, "Expected build >= %u to hold for %s\n", want.dwBuildNumber, shim);
        want.dwBuildNumber = info->dwBuildNumber + 1;
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&want, VER_BUILDNUMBER, cond);
        winetest_ok(!res, "Expected build >= %u not to hold for %s\n", want.dwBuildNumber, shim);

        want.dwPlatformId = VER_PLATFORM_WIN32_NT;
        cond = VerSetConditionMask(0, VER_PLATFORMID, VER_EQUAL);
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&want, VER_PLATFORMID, cond);
        winetest_ok(res, "Expected the platform to be NT for %s\n", shim);
    }

    /* Invalid arguments */
    {
        OSVERSIONINFOEXW want = { sizeof(want) };
        BOOL res;

        SetLastError(0xdeadbeef);
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&want, 0, VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL));
        winetest_ok(!res && GetLastError() == ERROR_BAD_ARGUMENTS, "Expected failure with ERROR_BAD_ARGUMENTS, got %d, %u for %s\n", res, GetLastError(), shim);
        SetLastError(0xdeadbeef);
        res = ((VERIFYWPROC)hook[5].ReplacementFunction)(&want, VER_MAJORVERSION, 0);
        winetest_ok(!res && GetLastError() == ERROR_BAD_ARGUMENTS, "Expected failure with ERROR_BAD_ARGUMENTS, got %d, %u for %s\n", res, GetLastError(), shim);
    }
}

static void verify_nt6_rtlverify(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim)
{
    RTL_OSVERSIONINFOEXW want = { sizeof(want) };
    NTSTATUS status;
    DWORDLONG cond = VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL);

    want.dwMajorVersion = info->dwMajorVersion;
    want.dwMinorVersion = info->dwMinorVersion;
    status = ((RTLVERIFYPROC)hook[6].ReplacementFunction)(&want, VER_MAJORVERSION | VER_MINORVERSION, cond);
    winetest_ok(status == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x for %s\n", status, shim);

    want.dwMinorVersion = info->dwMinorVersion + 1;
    status = ((RTLVERIFYPROC)hook[6].ReplacementFunction)(&want, VER_MAJORVERSION | VER_MINORVERSION, cond);
    winetest_ok(status == STATUS_REVISION_MISMATCH, "Expected STATUS_REVISION_MISMATCH, got 0x%x for %s\n", status, shim);

    status = ((RTLVERIFYPROC)hook[6].ReplacementFunction)(&want, 0, cond);
    winetest_ok(status == STATUS_INVALID_PARAMETER, "Expected STATUS_INVALID_PARAMETER, got 0x%x for %s\n", status, shim);
}

static void verify_nt6_ntversionnumbers(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim)
{
    ULONG major = 0xdead, minor = 0xdead, build = 0xdead;

    ((RTLGETNTVERSIONNUMBERSPROC)hook[7].ReplacementFunction)(&major, &minor, &build);
    winetest_ok(major == info->dwMajorVersion, "Expected major %u, got %u for %s\n", info->dwMajorVersion, major, shim);
    winetest_ok(minor == info->dwMinorVersion, "Expected minor %u, got %u for %s\n", info->dwMinorVersion, minor, shim);
    winetest_ok((build & 0xffff) == info->dwBuildNumber, "Expected build %u, got 0x%x for %s\n", info->dwBuildNumber, build, shim);
}

#define CV_KEY_W        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"

static void verify_nt6_registry(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim)
{
    HKEY key = NULL, other = NULL;
    WCHAR wbuf[64], expect[16];
    char abuf[64];
    DWORD type, size, dw;
    LONG ret, real;
    WCHAR realbuf[128];
    DWORD realtype, realsize;

    hook[8].OriginalFunction = RegQueryValueExA;
    hook[9].OriginalFunction = RegQueryValueExW;
    hook[10].OriginalFunction = GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegGetValueA");
    hook[11].OriginalFunction = GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegGetValueW");

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CV_KEY_W, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        skip("Unable to open the CurrentVersion key for %s\n", shim);
        return;
    }

    /* The version of the lie */
    size = sizeof(wbuf);
    type = 0;
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS, "RegQueryValueExW(CurrentVersion): expected success, got %d for %s\n", ret, shim);
    winetest_ok(type == REG_SZ, "RegQueryValueExW(CurrentVersion): expected REG_SZ, got %u for %s\n", type, shim);
    winetest_ok(!lstrcmpW(wbuf, info->pszCurrentVersion), "RegQueryValueExW(CurrentVersion): expected %s, got %s for %s\n",
                wine_dbgstr_w(info->pszCurrentVersion), wine_dbgstr_w(wbuf), shim);
    winetest_ok(size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR), "Unexpected size %u for %s\n", size, shim);

    /* Both build values */
    StringCchPrintfW(expect, ARRAYSIZE(expect), L"%u", info->dwBuildNumber);
    size = sizeof(wbuf);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentBuildNumber", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !lstrcmpW(wbuf, expect), "CurrentBuildNumber: expected %s, got %d, %s for %s\n", wine_dbgstr_w(expect), ret, wine_dbgstr_w(wbuf), shim);
    size = sizeof(wbuf);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"currentbuild", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !lstrcmpW(wbuf, expect), "CurrentBuild: expected %s, got %d, %s for %s\n", wine_dbgstr_w(expect), ret, wine_dbgstr_w(wbuf), shim);

    /* The product name */
    size = sizeof(wbuf);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"ProductName", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !wcsncmp(wbuf, L"Windows ", 8), "ProductName: expected 'Windows ...', got %d, %s for %s\n", ret, wine_dbgstr_w(wbuf), shim);

    /* Size query, then a buffer that is too small */
    size = 0;
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", NULL, &type, NULL, &size);
    winetest_ok(ret == ERROR_SUCCESS && size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR), "Size query: got %d, %u for %s\n", ret, size, shim);
    size = sizeof(WCHAR);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_MORE_DATA && size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR), "Small buffer: got %d, %u for %s\n", ret, size, shim);

    /* ANSI */
    size = sizeof(abuf);
    ret = ((REGQUERYEXAPROC)hook[8].ReplacementFunction)(key, "CurrentVersion", NULL, &type, (LPBYTE)abuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && type == REG_SZ && size == (DWORD)lstrlenW(info->pszCurrentVersion) + 1 &&
                abuf[0] == (char)info->pszCurrentVersion[0] && abuf[1] == '.',
                "RegQueryValueExA(CurrentVersion): got %d, %u, %u, '%s' for %s\n", ret, type, size, abuf, shim);

    /* RegGetValue, with and without a sub key */
    size = sizeof(wbuf);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(HKEY_LOCAL_MACHINE, CV_KEY_W, L"CurrentVersion", RRF_RT_REG_SZ, &type, wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !lstrcmpW(wbuf, info->pszCurrentVersion), "RegGetValueW(sub key): got %d, %s for %s\n", ret, wine_dbgstr_w(wbuf), shim);
    size = sizeof(wbuf);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_SZ, &type, wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !lstrcmpW(wbuf, info->pszCurrentVersion), "RegGetValueW(open key): got %d, %s for %s\n", ret, wine_dbgstr_w(wbuf), shim);
    size = sizeof(abuf);
    ret = ((REGGETVALUEAPROC)hook[10].ReplacementFunction)(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentVersion", RRF_RT_REG_SZ, &type, abuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && abuf[0] == (char)info->pszCurrentVersion[0], "RegGetValueA: got %d, '%s' for %s\n", ret, abuf, shim);
    size = sizeof(wbuf);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_DWORD, &type, wbuf, &size);
    winetest_ok(ret == ERROR_UNSUPPORTED_TYPE, "RegGetValueW(wrong type): expected ERROR_UNSUPPORTED_TYPE, got %d for %s\n", ret, shim);

    /* Windows 10 only: the major and minor number values; before that they do not exist */
    size = sizeof(dw);
    dw = 0xdead;
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentMajorVersionNumber", NULL, &type, (LPBYTE)&dw, &size);
    if (info->dwRegMajorVersion)
    {
        winetest_ok(ret == ERROR_SUCCESS && type == REG_DWORD && dw == info->dwRegMajorVersion,
                    "CurrentMajorVersionNumber: expected %u, got %d, %u, %u for %s\n", info->dwRegMajorVersion, ret, type, dw, shim);
        size = sizeof(dw);
        dw = 0xdead;
        ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentMinorVersionNumber", NULL, &type, (LPBYTE)&dw, &size);
        winetest_ok(ret == ERROR_SUCCESS && type == REG_DWORD && dw == 0, "CurrentMinorVersionNumber: got %d, %u, %u for %s\n", ret, type, dw, shim);
    }
    else
    {
        winetest_ok(ret == ERROR_FILE_NOT_FOUND, "CurrentMajorVersionNumber: expected ERROR_FILE_NOT_FOUND, got %d for %s\n", ret, shim);
    }

    /* Type and size are one variable: the capacity is read before the type is written */
    size = sizeof(WCHAR);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", NULL, &size, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_MORE_DATA, "Aliased type and size: expected ERROR_MORE_DATA, got %d for %s\n", ret, shim);
    size = sizeof(wbuf);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", NULL, &size, (LPBYTE)wbuf, &size);
    winetest_ok(ret == ERROR_SUCCESS && !lstrcmpW(wbuf, info->pszCurrentVersion), "Aliased type and size: got %d, %s for %s\n", ret, wine_dbgstr_w(wbuf), shim);

    /* RegGetValue: the type and size are reported when the type is not allowed, RRF_ZEROONFAILURE clears the data */
    memset(wbuf, 0x55, sizeof(wbuf));
    size = sizeof(wbuf);
    type = 0;
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_DWORD, &type, wbuf, &size);
    winetest_ok(ret == ERROR_UNSUPPORTED_TYPE && type == REG_SZ && size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR) &&
                !lstrcmpW(wbuf, info->pszCurrentVersion), "Unsupported type: got %d, %u, %u, %s for %s\n", ret, type, size, wine_dbgstr_w(wbuf), shim);
    memset(wbuf, 0x55, sizeof(wbuf));
    size = sizeof(wbuf);
    type = 0;
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_DWORD | RRF_ZEROONFAILURE, &type, wbuf, &size);
    winetest_ok(ret == ERROR_UNSUPPORTED_TYPE && type == REG_SZ && wbuf[0] == 0 && wbuf[1] == 0 && size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR),
                "RRF_ZEROONFAILURE: got %d, %u, %u, %04x for %s\n", ret, type, size, wbuf[0], shim);
    memset(wbuf, 0x55, sizeof(wbuf));
    size = sizeof(WCHAR);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, &type, wbuf, &size);
    winetest_ok(ret == ERROR_MORE_DATA && wbuf[0] == 0 && size == (lstrlenW(info->pszCurrentVersion) + 1) * sizeof(WCHAR),
                "RRF_ZEROONFAILURE with a small buffer: got %d, %04x, %u for %s\n", ret, wbuf[0], size, shim);
    size = 0;
    type = 0xdeadbeef;
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_SZ, &type, wbuf, NULL);
    winetest_ok(ret == ERROR_INVALID_PARAMETER, "Data without size: expected ERROR_INVALID_PARAMETER, got %d for %s\n", ret, shim);

    /* A value that does not fit is judged by its type first */
    size = sizeof(DWORD);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_DWORD, &type, wbuf, &size);
    winetest_ok(ret == ERROR_UNSUPPORTED_TYPE && type == REG_SZ, "Wrong type and small buffer: got %d, %u for %s\n", ret, type, shim);

    /* Expanding strings without saying so is the real function's to refuse */
    size = sizeof(realbuf);
    real = RegGetValueW(key, NULL, L"CurrentVersion", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, realbuf, &size);
    size = sizeof(wbuf);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, wbuf, &size);
    winetest_ok(ret == real, "Expanding flags: expected %d, got %d for %s\n", real, ret, shim);

    /* A reserved argument, a handle that cannot read values and a flag set the real function judges are its business */
    size = sizeof(wbuf);
    realsize = sizeof(realbuf);
    real = RegQueryValueExW(key, L"CurrentVersion", &size, &realtype, (LPBYTE)realbuf, &realsize);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentVersion", &size, &type, (LPBYTE)wbuf, &realsize);
    winetest_ok(ret == real, "Reserved argument: expected %d, got %d for %s\n", real, ret, shim);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CV_KEY_W, 0, READ_CONTROL, &other) == ERROR_SUCCESS)
    {
        size = sizeof(wbuf);
        realsize = sizeof(realbuf);
        real = RegQueryValueExW(other, L"CurrentVersion", NULL, &realtype, (LPBYTE)realbuf, &realsize);
        ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(other, L"CurrentVersion", NULL, &type, (LPBYTE)wbuf, &size);
        winetest_ok(ret == real, "Handle without KEY_QUERY_VALUE: expected %d, got %d for %s\n", real, ret, shim);
        RegCloseKey(other);
    }
    size = sizeof(wbuf);
    real = RegGetValueW(key, NULL, L"CurrentVersion", RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY | RRF_RT_REG_SZ, NULL, realbuf, &size);
    size = sizeof(wbuf);
    ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentVersion", RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY | RRF_RT_REG_SZ, NULL, wbuf, &size);
    winetest_ok(ret == real, "Both registry views: expected %d, got %d for %s\n", real, ret, shim);

    /* A value this version does not have */
    if (!info->dwRegMajorVersion)
    {
        dw = 0x55555555;
        size = sizeof(dw);
        ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentMinorVersionNumber", RRF_RT_REG_DWORD | RRF_ZEROONFAILURE, NULL, &dw, &size);
        winetest_ok(ret == ERROR_FILE_NOT_FOUND && dw == 0, "Absent value, RegGetValueW with RRF_ZEROONFAILURE: got %d, %08x for %s\n", ret, dw, shim);
        dw = 0x55555555;
        size = sizeof(dw);
        ret = ((REGGETVALUEAPROC)hook[10].ReplacementFunction)(key, NULL, "CurrentMajorVersionNumber", RRF_RT_REG_DWORD | RRF_ZEROONFAILURE, NULL, &dw, &size);
        winetest_ok(ret == ERROR_FILE_NOT_FOUND && dw == 0, "Absent value, RegGetValueA with RRF_ZEROONFAILURE: got %d, %08x for %s\n", ret, dw, shim);

        size = 0xdeadbeef;
        type = 0xdeadbeef;
        ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"CurrentMajorVersionNumber", NULL, &type, NULL, &size);
        winetest_ok(ret == ERROR_FILE_NOT_FOUND && size == 0 && type == REG_NONE, "Absent value: got %d, %u, %u for %s\n", ret, size, type, shim);
        type = 0xdeadbeef;
        size = sizeof(dw);
        ret = ((REGGETVALUEWPROC)hook[11].ReplacementFunction)(key, NULL, L"CurrentMajorVersionNumber", RRF_RT_REG_DWORD, &type, &dw, &size);
        winetest_ok(ret == ERROR_FILE_NOT_FOUND && type == REG_NONE && size == sizeof(dw), "Absent value, RegGetValueW: got %d, %u, %u for %s\n", ret, type, size, shim);
    }

    /* Other values and other keys are the operating system's business */
    realsize = sizeof(realbuf);
    real = RegQueryValueExW(key, L"SystemRoot", NULL, &realtype, (LPBYTE)realbuf, &realsize);
    size = sizeof(wbuf);
    ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(key, L"SystemRoot", NULL, &type, (LPBYTE)wbuf, &size);
    winetest_ok(ret == real, "SystemRoot: expected %d, got %d for %s\n", real, ret, shim);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT", 0, KEY_QUERY_VALUE, &other) == ERROR_SUCCESS)
    {
        size = sizeof(wbuf);
        ret = ((REGQUERYEXWPROC)hook[9].ReplacementFunction)(other, L"CurrentVersion", NULL, &type, (LPBYTE)wbuf, &size);
        real = RegQueryValueExW(other, L"CurrentVersion", NULL, &realtype, (LPBYTE)realbuf, &realsize);
        winetest_ok(ret == real, "CurrentVersion in the wrong key: expected %d, got %d for %s\n", real, ret, shim);
        RegCloseKey(other);
    }

    RegCloseKey(key);
}

static void verify_nt6_hooks(PHOOKAPI hook, const VersionLieInfo* info, PCSTR shim)
{
    int same = 0;

    expect_shim(hook + 4, "KERNEL32.DLL", "VerifyVersionInfoA", shim, &same);
    expect_shim(hook + 5, "KERNEL32.DLL", "VerifyVersionInfoW", shim, &same);
    expect_shim(hook + 6, "NTDLL.DLL", "RtlVerifyVersionInfo", shim, &same);
    expect_shim(hook + 7, "NTDLL.DLL", "RtlGetNtVersionNumbers", shim, &same);
    expect_shim(hook + 8, "ADVAPI32.DLL", "RegQueryValueExA", shim, &same);
    expect_shim(hook + 9, "ADVAPI32.DLL", "RegQueryValueExW", shim, &same);
    expect_shim(hook + 10, "ADVAPI32.DLL", "RegGetValueA", shim, &same);
    expect_shim(hook + 11, "ADVAPI32.DLL", "RegGetValueW", shim, &same);
    if (!same)
    {
        skip("Skipping implementation tests for the NT 6 hooks of %s\n", shim);
        return;
    }

    verify_nt6_verifyinfo(hook, info, shim);
    verify_nt6_rtlverify(hook, info, shim);
    verify_nt6_ntversionnumbers(hook, info, shim);
    verify_nt6_registry(hook, info, shim);
}


static void run_test(LPCSTR shim, const VersionLieInfo* info)
{
    DWORD num_shims = 0;
    WCHAR wide_shim[50] = { 0 };
    PHOOKAPI hook;
    DWORD ver;
    MultiByteToWideChar(CP_ACP, 0, shim, -1, wide_shim, 50);
    hook = pGetHookAPIs("", wide_shim, &num_shims);
    ver = (info->dwMajorVersion << 8) | info->dwMinorVersion;
    /* ReactOS has all the layers: a missing one is a bug */
    if (hook == NULL && !is_reactos())
    {
        skip("Skipping tests for layers (%s) not present in this os (0x%x)\n", shim, g_WinVersion);
        return;
    }
    ok(hook != NULL, "Expected hook to be a valid pointer for %s\n", shim);
    if (hook == NULL)
        return;
    if (info->wFlags & FLAG_AlternateHookOrder)
    {
        ok(num_shims == 3, "Expected num_shims to be 3, was: %u for %s\n", num_shims, shim);
        if (hook && num_shims == 3)
        {
            int same = 0;
            expect_shim(hook + 0, "KERNEL32.DLL", "GetVersion", shim, &same);
            verify_shim(hook + 0, info, shim, same);
            expect_shim(hook + 1, "KERNEL32.DLL", "GetVersionExA", shim, &same);
            verify_shima(hook + 1, info, shim, same);
            expect_shim(hook + 2, "KERNEL32.DLL", "GetVersionExW", shim, &same);
            verify_shimw(hook + 2, info, shim, same, 0);
        }
    }
    else
    {
        /* ReactOS answers VerifyVersionInfo and the version registry values for the NT 6 and later lies too */
        int nt6 = is_reactos() && ver >= _WIN32_WINNT_VISTA;
        int shimnum_ok = nt6 ? (num_shims == NUM_HOOKS_NT6) : (num_shims == 4 || ((ver < _WIN32_WINNT_WINXP) && (num_shims == 3)));
        ok(shimnum_ok, "Expected num_shims to be %d%s, was: %u for %s\n", nt6 ? NUM_HOOKS_NT6 : 4, ((ver < _WIN32_WINNT_WINXP) ? " or 3":""), num_shims, shim);
        if (hook && shimnum_ok)
        {
            int same = 0;
            expect_shim(hook + 0, "KERNEL32.DLL", "GetVersionExA", shim, &same);
            verify_shima(hook + 0, info, shim, same);
            expect_shim(hook + 1, "KERNEL32.DLL", "GetVersionExW", shim, &same);
            verify_shimw(hook + 1, info, shim, same, 0);
            expect_shim(hook + 2, "KERNEL32.DLL", "GetVersion", shim, &same);
            verify_shim(hook + 2, info, shim, same);
            if (num_shims >= 4)
            {
                expect_shim(hook + 3, "NTDLL.DLL", "RtlGetVersion", shim, &same);
                verify_rtlgetversion(hook + 3, info, shim, same);
            }
            if (nt6)
                verify_nt6_hooks(hook, info, shim);
        }
    }
}


VersionLieInfo g_Win95 = { 0xC3B60004, 4, 0, 950, VER_PLATFORM_WIN32_WINDOWS, 0, 0, FLAG_BUGGY_ServicePackMajorMinor | FLAG_AlternateHookOrder };
VersionLieInfo g_WinNT4SP5 = { 0x05650004, 4, 0, 1381, VER_PLATFORM_WIN32_NT, 5, 0, FLAG_BUGGY_ServicePackMajorMinor };
VersionLieInfo g_Win98 = { 0xC0000A04, 4, 10, 0x040A08AE, VER_PLATFORM_WIN32_WINDOWS, 0, 0, FLAG_BUGGY_ServicePackMajorMinor };

VersionLieInfo g_Win2000 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_Win2000SP1 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 1, 0 };
VersionLieInfo g_Win2000SP2 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 2, 0 };
VersionLieInfo g_Win2000SP3 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 3, 0 };

VersionLieInfo g_WinXP = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_WinXPSP1 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 1, 0 };
VersionLieInfo g_WinXPSP2 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 2, 0 };
VersionLieInfo g_WinXPSP3 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 3, 0 };

VersionLieInfo g_Win2k3RTM = { 0x0ece0205, 5, 2, 3790, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_Win2k3SP1 = { 0x0ece0205, 5, 2, 3790, VER_PLATFORM_WIN32_NT, 1, 0 };

VersionLieInfo g_WinVistaRTM = { 0x17700006, 6, 0, 6000, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.0" };
VersionLieInfo g_WinVistaSP1 = { 0x17710006, 6, 0, 6001, VER_PLATFORM_WIN32_NT, 1, 0, .pszCurrentVersion = L"6.0" };
VersionLieInfo g_WinVistaSP2 = { 0x17720006, 6, 0, 6002, VER_PLATFORM_WIN32_NT, 2, 0, .pszCurrentVersion = L"6.0" };

VersionLieInfo g_Win7RTM = { 0x1db00106, 6, 1, 7600, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.1" };
VersionLieInfo g_Win7SP1 = { 0x1db10106, 6, 1, 7601, VER_PLATFORM_WIN32_NT, 1, 0, .pszCurrentVersion = L"6.1" }; /* ReactOS specific. Windows does not have  this version lie */

VersionLieInfo g_Win8RTM = { 0x23f00206, 6, 2, 9200, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.2" };
VersionLieInfo g_Win81RTM = { 0x25800306, 6, 3, 9600, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.3" };

VersionLieInfo g_Win10RTM = { 0x47ba000a, 10, 0, 18362, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.3", .dwRegMajorVersion = 10 };

VersionLieInfo g_Win2k16RTM = { 0x3fab000a, 10, 0, 16299, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.3", .dwRegMajorVersion = 10 };
VersionLieInfo g_Win2k19RTM = { 0x4563000a, 10, 0, 17763, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.3", .dwRegMajorVersion = 10 };

DWORD get_host_winver(void)
{
    RTL_OSVERSIONINFOEXW rtlinfo = {0};
    void (__stdcall* pRtlGetVersion)(RTL_OSVERSIONINFOEXW*);
    pRtlGetVersion = (void (__stdcall*)(RTL_OSVERSIONINFOEXW*))GetProcAddress(GetModuleHandleA("ntdll"), "RtlGetVersion");

    rtlinfo.dwOSVersionInfoSize = sizeof(rtlinfo);
    pRtlGetVersion(&rtlinfo);
    return (rtlinfo.dwMajorVersion << 8) | rtlinfo.dwMinorVersion;
}

BOOL LoadShimDLL(PCWSTR ShimDll, HMODULE* module, tGETHOOKAPIS* ppGetHookAPIs)
{
    static tSDBGETAPPPATCHDIR pSdbGetAppPatchDir = NULL;
    HMODULE dll;
    WCHAR buf[MAX_PATH] = {0};
    if (!pSdbGetAppPatchDir)
    {
        dll = LoadLibraryA("apphelp.dll");
        pSdbGetAppPatchDir = (tSDBGETAPPPATCHDIR)GetProcAddress(dll, "SdbGetAppPatchDir");

        if (!pSdbGetAppPatchDir)
        {
            skip("Unable to retrieve SdbGetAppPatchDir (%p, %p)\n", dll, pSdbGetAppPatchDir);
        }
    }

    if (!pSdbGetAppPatchDir || !SUCCEEDED(pSdbGetAppPatchDir(NULL, buf, MAX_PATH)))
    {
        skip("Unable to retrieve AppPatch dir, building manually\n");
        if (!GetSystemWindowsDirectoryW(buf, MAX_PATH))
        {
            skip("Unable to build AppPatch name(1)\n");
            return FALSE;
        }
        if (!SUCCEEDED(StringCchCatW(buf, _countof(buf), L"\\AppPatch")))
        {
            skip("Unable to build AppPatch name(2)\n");
            return FALSE;
        }
    }
    if (!SUCCEEDED(StringCchCatW(buf, _countof(buf), L"\\")) ||
        !SUCCEEDED(StringCchCatW(buf, _countof(buf), ShimDll)))
    {
        skip("Unable to append dll name\n");
        return FALSE;
    }

    dll = LoadLibraryW(buf);
    if (!dll)
    {
        skip("Unable to load shim dll from AppPatch\n");
        GetSystemWindowsDirectoryW(buf, _countof(buf));

        if (SUCCEEDED(StringCchCatW(buf, _countof(buf), L"\\System32\\")) &&
            SUCCEEDED(StringCchCatW(buf, _countof(buf), ShimDll)))
        {
            dll = LoadLibraryW(buf);
        }

        if (!dll)
        {
            skip("Unable to load shim dll from System32 (Recent Win10)\n");
            return FALSE;
        }
    }
    *module = dll;
    *ppGetHookAPIs = (tGETHOOKAPIS)GetProcAddress(dll, "GetHookAPIs");

    return *ppGetHookAPIs != NULL;
}


tGETHOOKAPIS LoadShimDLL2(PCWSTR ShimDll)
{
    HMODULE module;
    tGETHOOKAPIS pGetHookAPIs;

    if (LoadShimDLL(ShimDll, &module, &pGetHookAPIs))
    {
        if (!pGetHookAPIs)
            skip("No GetHookAPIs found\n");
        return pGetHookAPIs;
    }
    return NULL;
}


START_TEST(versionlie)
{
    pGetHookAPIs = LoadShimDLL2(L"aclayers.dll");

    if (!pGetHookAPIs)
        return;

    g_WinVersion = get_host_winver();

    run_test("Win95VersionLie", &g_Win95);
    run_test("WinNT4SP5VersionLie", &g_WinNT4SP5);
    run_test("Win98VersionLie", &g_Win98);
    run_test("Win2000VersionLie", &g_Win2000);
    run_test("Win2000SP1VersionLie", &g_Win2000SP1);
    run_test("Win2000SP2VersionLie", &g_Win2000SP2);
    run_test("Win2000SP3VersionLie", &g_Win2000SP3);
    run_test("WinXPVersionLie", &g_WinXP);
    run_test("WinXPSP1VersionLie", &g_WinXPSP1);
    run_test("WinXPSP2VersionLie", &g_WinXPSP2);
    run_test("WinXPSP3VersionLie", &g_WinXPSP3);
    run_test("Win2k3RTMVersionLie", &g_Win2k3RTM);
    run_test("Win2k3SP1VersionLie", &g_Win2k3SP1);
    run_test("VistaRTMVersionLie", &g_WinVistaRTM);
    run_test("VistaSP1VersionLie", &g_WinVistaSP1);
    run_test("VistaSP2VersionLie", &g_WinVistaSP2);
    run_test("Win7RTMVersionLie", &g_Win7RTM);
    run_test("Win7SP1VersionLie", &g_Win7SP1);    /* ReactOS specific. Windows does not have this version lie */
    run_test("Win8RTMVersionLie", &g_Win8RTM);
    run_test("Win81RTMVersionLie", &g_Win81RTM);
    run_test("Win10RTMVersionLie", &g_Win10RTM);    /* ReactOS specific. Windows does not have this version lie */
    run_test("Win2k16RTMVersionLie", &g_Win2k16RTM);    /* ReactOS specific. Windows does not have this version lie */
    run_test("Win2k19RTMVersionLie", &g_Win2k19RTM);    /* ReactOS specific. Windows does not have this version lie */
}
