/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for SHGetStockIconInfo
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"

/* The SDK declares these for NTDDI_VISTA only */
typedef struct
{
    DWORD cbSize;
    HICON hIcon;
    INT iSysImageIndex;
    INT iIcon;
    WCHAR szPath[MAX_PATH];
} STOCKICONINFO_;

typedef HRESULT (WINAPI *FN_SHGetStockIconInfo)(INT, UINT, STOCKICONINFO_ *);
static FN_SHGetStockIconInfo pSHGetStockIconInfo;
typedef INT (WINAPI *FN_Shell_GetCachedImageIndexW)(PCWSTR, INT, UINT);
static FN_Shell_GetCachedImageIndexW pShell_GetCachedImageIndexW;

#define SIID_DOCNOASSOC_ 0
#define SIID_FOLDER_ 3
#define SIID_LINK_ 29
#define SIID_SHIELD_ 77
#define SIID_WARNING_ 78
/* Windows 7 accepts ids below 174, Windows 10 below 181 */
#define SIID_COMMON_COUNT 174
#define SIID_INVALID_ 181

#define CB_UNTOUCHED 0xCC

static void InitInfo(STOCKICONINFO_ *psii)
{
    memset(psii, CB_UNTOUCHED, sizeof(*psii));
    psii->cbSize = sizeof(*psii);
}

static BOOL IsUntouched(const STOCKICONINFO_ *psii)
{
    const BYTE *pb = (const BYTE *)&psii->hIcon;
    SIZE_T i;
    for (i = 0; i < sizeof(*psii) - FIELD_OFFSET(STOCKICONINFO_, hIcon); i++)
    {
        if (pb[i] != CB_UNTOUCHED)
            return FALSE;
    }
    return TRUE;
}

static void CheckCleared(const STOCKICONINFO_ *psii, PCSTR what)
{
    ok(psii->hIcon == NULL, "%s: hIcon %p\n", what, psii->hIcon);
    ok_int(psii->iSysImageIndex, -1);
    ok_int(psii->iIcon, -1);
    ok(psii->szPath[0] == UNICODE_NULL, "%s: szPath '%S'\n", what, psii->szPath);
}

static void GetIconSize(HICON hIcon, INT *pcx, INT *pcy)
{
    ICONINFO ii;
    BITMAP bm;

    *pcx = *pcy = -1;
    if (!hIcon || !GetIconInfo(hIcon, &ii))
        return;
    if (GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm))
    {
        *pcx = bm.bmWidth;
        *pcy = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
}

static void TestArguments(void)
{
    static const INT s_BadIds[] = { -1, -2, SIID_INVALID_, SIID_INVALID_ + 1, 1000 };
    static const UINT s_BadFlags[] = { 0x2, 0x8, 0x10, 0x20, 0x40, 0x80, 0x200, 0x400, 0x800, 0x1000, 0x2000,
                                       0x20000, 0x40000, 0x80000000 };
    STOCKICONINFO_ sii;
    HRESULT hr;
    INT i;

    hr = pSHGetStockIconInfo(SIID_FOLDER_, 0, NULL);
    ok_hr(hr, E_INVALIDARG);

    /* A wrong structure size is rejected before anything is written */
    for (i = -2; i <= 2; i++)
    {
        if (!i)
            continue;
        InitInfo(&sii);
        sii.cbSize = sizeof(sii) + i;
        hr = pSHGetStockIconInfo(SIID_FOLDER_, SHGSI_ICON | SHGSI_SYSICONINDEX, &sii);
        ok(hr == E_INVALIDARG, "cbSize %+d: hr 0x%lx\n", i, hr);
        ok(IsUntouched(&sii), "cbSize %+d: outputs written\n", i);
    }

    /* An unknown id or flag clears the outputs */
    for (i = 0; i < (INT)_countof(s_BadIds); i++)
    {
        InitInfo(&sii);
        hr = pSHGetStockIconInfo(s_BadIds[i], 0, &sii);
        ok(hr == E_INVALIDARG, "id %d: hr 0x%lx\n", s_BadIds[i], hr);
        CheckCleared(&sii, "bad id");
    }
    for (i = 0; i < (INT)_countof(s_BadFlags); i++)
    {
        InitInfo(&sii);
        hr = pSHGetStockIconInfo(SIID_FOLDER_, s_BadFlags[i], &sii);
        ok(hr == E_INVALIDARG, "flags 0x%x: hr 0x%lx\n", s_BadFlags[i], hr);
        CheckCleared(&sii, "bad flags");
    }
}

static void TestLocations(void)
{
    WCHAR szSystem[MAX_PATH];
    STOCKICONINFO_ sii;
    HICON hIcon;
    HRESULT hr;
    INT i, cchSystem;

    cchSystem = GetSystemDirectoryW(szSystem, _countof(szSystem));
    ok(cchSystem > 0, "GetSystemDirectoryW failed\n");

    for (i = 0; i < SIID_COMMON_COUNT; i++)
    {
        InitInfo(&sii);
        hr = pSHGetStockIconInfo(i, 0 /* SHGSI_ICONLOCATION */, &sii);
        ok(hr == S_OK, "id %d: hr 0x%lx\n", i, hr);
        if (hr != S_OK)
            continue;
        ok(sii.hIcon == NULL, "id %d: hIcon %p\n", i, sii.hIcon);
        ok(sii.iSysImageIndex == -1, "id %d: iSysImageIndex %d\n", i, sii.iSysImageIndex);
        /* A negative resource id in a module of the system directory */
        ok(sii.iIcon < 0, "id %d: iIcon %d\n", i, sii.iIcon);
        ok(!_wcsnicmp(sii.szPath, szSystem, cchSystem) && sii.szPath[cchSystem] == L'\\',
           "id %d: path '%S'\n", i, sii.szPath);
        ok(GetFileAttributesW(sii.szPath) != INVALID_FILE_ATTRIBUTES, "id %d: '%S' missing\n", i, sii.szPath);
        hIcon = NULL;
        ok(ExtractIconExW(sii.szPath, sii.iIcon, &hIcon, NULL, 1) == 1 && hIcon,
           "id %d: no icon %d in '%S'\n", i, sii.iIcon, sii.szPath);
        if (hIcon)
            DestroyIcon(hIcon);
    }
}

static void TestFlags(void)
{
    static const INT s_Ids[] = { SIID_DOCNOASSOC_, SIID_FOLDER_, SIID_LINK_, SIID_SHIELD_, SIID_WARNING_ };
    static const struct
    {
        UINT uFlags;
        INT iSize; /* 0: no icon, 1: large, 2: small, 3: shell size */
    } s_Flags[] =
    {
        { SHGSI_ICON, 1 },
        { SHGSI_ICON | SHGSI_SMALLICON, 2 },
        { SHGSI_ICON | SHGSI_SHELLICONSIZE, 3 },
        { SHGSI_ICON | SHGSI_SHELLICONSIZE | SHGSI_SMALLICON, 3 },
        { SHGSI_ICON | SHGSI_LINKOVERLAY, 1 },
        { SHGSI_ICON | SHGSI_SELECTED, 1 },
        { SHGSI_ICON | SHGSI_SMALLICON | SHGSI_LINKOVERLAY, 2 },
        { SHGSI_ICON | SHGSI_SMALLICON | SHGSI_SELECTED, 2 },
        { SHGSI_SYSICONINDEX, 0 },
        { SHGSI_SYSICONINDEX | SHGSI_SMALLICON, 0 },
        { SHGSI_SYSICONINDEX | SHGSI_LINKOVERLAY, 0 },
        { SHGSI_SYSICONINDEX | SHGSI_SELECTED, 0 },
        { SHGSI_LINKOVERLAY, 0 },
        { SHGSI_SELECTED, 0 },
        { SHGSI_ICON | SHGSI_SYSICONINDEX, 1 },
        { SHGSI_ICON | SHGSI_SYSICONINDEX | SHGSI_SMALLICON, 2 },
    };
    STOCKICONINFO_ sii, siiLoc;
    HRESULT hr;
    INT i, j, cx, cy, iCached;

    for (j = 0; j < (INT)_countof(s_Ids); j++)
    {
        InitInfo(&siiLoc);
        hr = pSHGetStockIconInfo(s_Ids[j], 0, &siiLoc);
        ok_hr(hr, S_OK);
        iCached = pShell_GetCachedImageIndexW(siiLoc.szPath, siiLoc.iIcon, 0);
        ok(iCached >= 0, "id %d: cached index %d\n", s_Ids[j], iCached);

        for (i = 0; i < (INT)_countof(s_Flags); i++)
        {
            UINT uFlags = s_Flags[i].uFlags;

            InitInfo(&sii);
            hr = pSHGetStockIconInfo(s_Ids[j], uFlags, &sii);
            ok(hr == S_OK, "id %d flags 0x%x: hr 0x%lx\n", s_Ids[j], uFlags, hr);
            if (hr != S_OK)
                continue;

            ok(sii.iIcon == siiLoc.iIcon && !lstrcmpiW(sii.szPath, siiLoc.szPath),
               "id %d flags 0x%x: location %d '%S'\n", s_Ids[j], uFlags, sii.iIcon, sii.szPath);

            /* The system image index of the icon location; the overlay does not change it */
            if (uFlags & SHGSI_SYSICONINDEX)
                ok(sii.iSysImageIndex == iCached, "id %d flags 0x%x: index %d, cached %d\n", s_Ids[j], uFlags,
                   sii.iSysImageIndex, iCached);
            else
                ok(sii.iSysImageIndex == -1, "id %d flags 0x%x: index %d\n", s_Ids[j], uFlags, sii.iSysImageIndex);

            if (!s_Flags[i].iSize)
            {
                ok(sii.hIcon == NULL, "id %d flags 0x%x: hIcon %p\n", s_Ids[j], uFlags, sii.hIcon);
                continue;
            }
            ok(sii.hIcon != NULL, "id %d flags 0x%x: no icon\n", s_Ids[j], uFlags);
            GetIconSize(sii.hIcon, &cx, &cy);
            if (s_Flags[i].iSize == 1)
                ok(cx == GetSystemMetrics(SM_CXICON) && cy == GetSystemMetrics(SM_CYICON),
                   "id %d flags 0x%x: %dx%d\n", s_Ids[j], uFlags, cx, cy);
            else if (s_Flags[i].iSize == 2)
                ok(cx == GetSystemMetrics(SM_CXSMICON) && cy == GetSystemMetrics(SM_CYSMICON),
                   "id %d flags 0x%x: %dx%d\n", s_Ids[j], uFlags, cx, cy);
            else
                ok(cx > 0 && cy > 0, "id %d flags 0x%x: %dx%d\n", s_Ids[j], uFlags, cx, cy);
            if (sii.hIcon)
                DestroyIcon(sii.hIcon);
        }
    }
}

#define SHELLSIZE_OK 0
#define SHELLSIZE_NOT_USED 0x10

/* Child: the registry asks for a shell icon size that differs from the system metrics */
static int ShellSizeChild(INT cxShell)
{
    STOCKICONINFO_ sii;
    INT cx, cy, iResult = SHELLSIZE_OK;

    InitInfo(&sii);
    if (FAILED(pSHGetStockIconInfo(SIID_FOLDER_, SHGSI_ICON | SHGSI_SHELLICONSIZE, &sii)) || !sii.hIcon)
        return 1;
    GetIconSize(sii.hIcon, &cx, &cy);
    DestroyIcon(sii.hIcon);
    if (cx == GetSystemMetrics(SM_CXICON))
        return SHELLSIZE_NOT_USED;
    if (cx != cxShell || cy != cxShell)
        iResult |= 2;

    /* Without SHGSI_SHELLICONSIZE the system metrics apply, with or without an overlay */
    for (UINT uFlags = SHGSI_ICON; uFlags; uFlags = (uFlags == SHGSI_ICON) ? (SHGSI_ICON | SHGSI_LINKOVERLAY) : 0)
    {
        InitInfo(&sii);
        if (FAILED(pSHGetStockIconInfo(SIID_FOLDER_, uFlags, &sii)) || !sii.hIcon)
            return 1;
        GetIconSize(sii.hIcon, &cx, &cy);
        DestroyIcon(sii.hIcon);
        if (cx != GetSystemMetrics(SM_CXICON) || cy != GetSystemMetrics(SM_CYICON))
            iResult |= 4;
    }
    return iResult;
}

static void TestShellIconSize(void)
{
    static const WCHAR s_szKey[] = L"Control Panel\\Desktop\\WindowMetrics";
    WCHAR szNew[16], szExe[MAX_PATH], szCmd[MAX_PATH + 64];
    DWORD cbOld = 0, dwType = 0, dwExit = 0xDEADBEEF;
    INT cxShell = GetSystemMetrics(SM_CXICON) + 16;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BYTE *pOld = NULL;
    HKEY hKey;
    LONG lError;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, s_szKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
    {
        skip("Cannot open the window metrics key\n");
        return;
    }

    /* Back up the raw value to restore it exactly; only an absent value is deleted afterwards */
    lError = RegQueryValueExW(hKey, L"Shell Icon Size", NULL, &dwType, NULL, &cbOld);
    if (lError == ERROR_SUCCESS)
    {
        pOld = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cbOld ? cbOld : 1);
        lError = pOld ? RegQueryValueExW(hKey, L"Shell Icon Size", NULL, &dwType, pOld, &cbOld) : ERROR_OUTOFMEMORY;
    }
    /* RegSetValueExW can store one more character than given for an unterminated string */
    if (lError == ERROR_SUCCESS && (dwType == REG_SZ || dwType == REG_EXPAND_SZ || dwType == REG_MULTI_SZ) &&
        (cbOld % sizeof(WCHAR) || (cbOld && ((PWCHAR)pOld)[cbOld / sizeof(WCHAR) - 1] != UNICODE_NULL)))
    {
        lError = ERROR_INVALID_DATA;
    }
    if (lError != ERROR_SUCCESS && lError != ERROR_FILE_NOT_FOUND)
    {
        skip("Cannot back up the shell icon size, error %ld\n", lError);
        HeapFree(GetProcessHeap(), 0, pOld);
        RegCloseKey(hKey);
        return;
    }

    wsprintfW(szNew, L"%d", cxShell);
    if (RegSetValueExW(hKey, L"Shell Icon Size", 0, REG_SZ, (const BYTE *)szNew,
                       (lstrlenW(szNew) + 1) * sizeof(WCHAR)) != ERROR_SUCCESS)
    {
        skip("Cannot set the shell icon size\n");
        HeapFree(GetProcessHeap(), 0, pOld);
        RegCloseKey(hKey);
        return;
    }

    GetModuleFileNameW(NULL, szExe, _countof(szExe));
    wsprintfW(szCmd, L"\"%s\" SHGetStockIconInfo shellsize %d", szExe, cxShell);
    if (CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &dwExit);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (lError == ERROR_SUCCESS)
        RegSetValueExW(hKey, L"Shell Icon Size", 0, dwType, pOld, cbOld);
    else
        RegDeleteValueW(hKey, L"Shell Icon Size");
    HeapFree(GetProcessHeap(), 0, pOld);
    RegCloseKey(hKey);

    if (dwExit == SHELLSIZE_NOT_USED)
        skip("The shell does not use the registry icon size\n");
    else
        ok(dwExit == SHELLSIZE_OK, "shell icon size %d: child result 0x%lx\n", cxShell, dwExit);
}

START_TEST(SHGetStockIconInfo)
{
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    pSHGetStockIconInfo = (FN_SHGetStockIconInfo)GetProcAddress(hShell32, "SHGetStockIconInfo");
    if (!pSHGetStockIconInfo)
    {
        skip("SHGetStockIconInfo is not exported\n");
        return;
    }
    pShell_GetCachedImageIndexW = (FN_Shell_GetCachedImageIndexW)GetProcAddress(hShell32, "Shell_GetCachedImageIndexW");
    ok(pShell_GetCachedImageIndexW != NULL, "Shell_GetCachedImageIndexW is not exported\n");
    if (!pShell_GetCachedImageIndexW)
        return;

    if (argc >= 4 && !strcmp(argv[2], "shellsize"))
        ExitProcess(ShellSizeChild(atoi(argv[3])));

    TestArguments();
    TestLocations();
    TestFlags();
    TestShellIconSize();
}
