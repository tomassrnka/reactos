/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for SHGetPathFromIDListEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"

typedef BOOL (WINAPI *FN_SHGetPathFromIDListEx)(PCIDLIST_ABSOLUTE, PWSTR, DWORD, GPFIDL_FLAGS);
static FN_SHGetPathFromIDListEx pSHGetPathFromIDListEx;

#define CH_UNTOUCHED 0xCCCC

static void TestBufferSizes(void)
{
    WCHAR szExpected[MAX_PATH], szPath[MAX_PATH * 2];
    PIDLIST_ABSOLUTE pidl = NULL;
    DWORD cchExpected, cch;
    HRESULT hr;
    BOOL ret;
    INT i;

    ok(GetSystemDirectoryW(szExpected, _countof(szExpected)) > 0, "GetSystemDirectoryW failed\n");
    hr = SHParseDisplayName(szExpected, NULL, &pidl, 0, NULL);
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    /* The shell spells the folder as the file system does */
    ok(SHGetPathFromIDListW(pidl, szExpected), "SHGetPathFromIDListW failed\n");
    cchExpected = lstrlenW(szExpected);

    for (i = 0; i < 2; i++)
    {
        cch = i ? _countof(szPath) : cchExpected + 1;
        memset(szPath, 0xCC, sizeof(szPath));
        ret = pSHGetPathFromIDListEx(pidl, szPath, cch, GPFIDL_DEFAULT);
        ok(ret == TRUE, "cch %lu: ret %d\n", cch, ret);
        ok(!lstrcmpW(szPath, szExpected), "cch %lu: '%S', expected '%S'\n", cch, szPath, szExpected);
    }

    /* Too small: failure, and at most a truncated prefix (Windows 7 leaves it empty) */
    for (cch = 1; cch <= cchExpected; cch += (cch < 4 || cch >= cchExpected - 2) ? 1 : cchExpected / 4)
    {
        memset(szPath, 0xCC, sizeof(szPath));
        ret = pSHGetPathFromIDListEx(pidl, szPath, cch, GPFIDL_DEFAULT);
        ok(ret == FALSE, "cch %lu: ret %d\n", cch, ret);
        ok((DWORD)lstrlenW(szPath) < cch && !wcsncmp(szPath, szExpected, lstrlenW(szPath)),
           "cch %lu: '%.20S'\n", cch, szPath);
    }

    /* No buffer to write to */
    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(pidl, szPath, 0, GPFIDL_DEFAULT);
    ok(ret == FALSE, "cch 0: ret %d\n", ret);
    ok(szPath[0] == CH_UNTOUCHED, "cch 0: buffer written\n");

    /* Unknown flags are ignored, and printers have no other name here */
    for (i = 0; i < 4; i++)
    {
        static const GPFIDL_FLAGS s_Flags[] = { GPFIDL_UNCPRINTER, 0x4, 0x8, 0x80 };
        memset(szPath, 0xCC, sizeof(szPath));
        ret = pSHGetPathFromIDListEx(pidl, szPath, _countof(szPath), s_Flags[i]);
        ok(ret == TRUE, "flags 0x%x: ret %d\n", s_Flags[i], ret);
        ok(!lstrcmpW(szPath, szExpected), "flags 0x%x: '%S'\n", s_Flags[i], szPath);
    }

    ILFree(pidl);
}

static void TestAltName(void)
{
    WCHAR szLong[MAX_PATH], szShort[MAX_PATH], szPath[MAX_PATH];
    PIDLIST_ABSOLUTE pidl = NULL;
    HRESULT hr;
    BOOL ret;

    /* A folder whose name is not an 8.3 name */
    hr = SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, szLong);
    ok_hr(hr, S_OK);
    hr = SHParseDisplayName(szLong, NULL, &pidl, 0, NULL);
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    ok(SHGetPathFromIDListW(pidl, szLong), "SHGetPathFromIDListW failed\n");
    if (!GetShortPathNameW(szLong, szShort, _countof(szShort)))
    {
        skip("No short name for '%S'\n", szLong);
        ILFree(pidl);
        return;
    }

    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_ALTNAME);
    ok(ret == TRUE, "ret %d\n", ret);
    ok(!lstrcmpiW(szPath, szShort), "'%S', expected '%S'\n", szPath, szShort);

    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_ALTNAME | GPFIDL_UNCPRINTER);
    ok(ret == TRUE, "ret %d\n", ret);
    ok(!lstrcmpiW(szPath, szShort), "'%S', expected '%S'\n", szPath, szShort);

    ILFree(pidl);
}

static void TestSpecialLists(void)
{
    WCHAR szPath[MAX_PATH], szExpected[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;
    BOOL ret;

    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(NULL, szPath, _countof(szPath), GPFIDL_DEFAULT);
    ok(ret == FALSE, "NULL: ret %d\n", ret);
    ok(szPath[0] == UNICODE_NULL, "NULL: buffer 0x%04x\n", szPath[0]);

    /* A virtual folder has no path */
    pidl = NULL;
    hr = SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pidl);
    ok_hr(hr, S_OK);
    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_DEFAULT);
    ok(ret == FALSE, "My Computer: ret %d\n", ret);
    ok(szPath[0] == UNICODE_NULL, "My Computer: buffer 0x%04x\n", szPath[0]);
    ILFree(pidl);

    /* The desktop is the user's desktop folder */
    pidl = NULL;
    hr = SHGetSpecialFolderLocation(NULL, CSIDL_DESKTOP, &pidl);
    ok_hr(hr, S_OK);
    hr = SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, szExpected);
    ok_hr(hr, S_OK);
    memset(szPath, 0xCC, sizeof(szPath));
    ret = pSHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_DEFAULT);
    ok(ret == TRUE, "Desktop: ret %d\n", ret);
    ok(!lstrcmpiW(szPath, szExpected), "Desktop: '%S', expected '%S'\n", szPath, szExpected);
    ILFree(pidl);
}

START_TEST(SHGetPathFromIDListEx)
{
    pSHGetPathFromIDListEx = (FN_SHGetPathFromIDListEx)GetProcAddress(GetModuleHandleW(L"shell32.dll"),
                                                                       "SHGetPathFromIDListEx");
    if (!pSHGetPathFromIDListEx)
    {
        skip("SHGetPathFromIDListEx is not exported\n");
        return;
    }

    TestBufferSizes();
    TestAltName();
    TestSpecialLists();
}
