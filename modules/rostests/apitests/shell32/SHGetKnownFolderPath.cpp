/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for SHGetKnownFolderPath and the other known-folder functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"
#include <knownfolders.h>
#include <shobjidl.h>

typedef HRESULT (WINAPI *FN_SHGetKnownFolderPath)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
typedef HRESULT (WINAPI *FN_SHGetFolderPathEx)(REFKNOWNFOLDERID, DWORD, HANDLE, LPWSTR, UINT);
typedef HRESULT (WINAPI *FN_SHGetKnownFolderIDList)(REFKNOWNFOLDERID, DWORD, HANDLE, PIDLIST_ABSOLUTE *);
typedef HRESULT (WINAPI *FN_SHGetKnownFolderItem)(REFKNOWNFOLDERID, DWORD, HANDLE, REFIID, void **);
typedef HRESULT (WINAPI *FN_SHSetKnownFolderPath)(REFKNOWNFOLDERID, DWORD, HANDLE, PCWSTR);

static FN_SHGetKnownFolderPath pSHGetKnownFolderPath;
static FN_SHGetFolderPathEx pSHGetFolderPathEx;
static FN_SHGetKnownFolderIDList pSHGetKnownFolderIDList;
static FN_SHGetKnownFolderItem pSHGetKnownFolderItem;
static FN_SHSetKnownFolderPath pSHSetKnownFolderPath;

#define KF_FLAG_NO_APPCONTAINER_REDIRECTION_ 0x00010000

static const GUID s_UnknownId =
    { 0x01234567, 0x89ab, 0xcdef, { 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 } };

static const struct
{
    const KNOWNFOLDERID *id;
    INT csidl;
    BOOL bMustExist;
    PCSTR name;
} s_Folders[] =
{
    { &FOLDERID_Documents, CSIDL_PERSONAL, TRUE, "Documents" },
    { &FOLDERID_Desktop, CSIDL_DESKTOP, TRUE, "Desktop" },
    { &FOLDERID_ProgramFiles, CSIDL_PROGRAM_FILES, TRUE, "ProgramFiles" },
    /* No x86 program folder on amd64 ReactOS (no WOW64) */
    { &FOLDERID_ProgramFilesX86, CSIDL_PROGRAM_FILESX86, FALSE, "ProgramFilesX86" },
    { &FOLDERID_LocalAppData, CSIDL_LOCAL_APPDATA, TRUE, "LocalAppData" },
    { &FOLDERID_RoamingAppData, CSIDL_APPDATA, TRUE, "RoamingAppData" },
    { &FOLDERID_Windows, CSIDL_WINDOWS, TRUE, "Windows" },
    { &FOLDERID_System, CSIDL_SYSTEM, TRUE, "System" },
    { &FOLDERID_Fonts, CSIDL_FONTS, TRUE, "Fonts" },
    { &FOLDERID_ProgramData, CSIDL_COMMON_APPDATA, TRUE, "ProgramData" },
    { &FOLDERID_Profile, CSIDL_PROFILE, TRUE, "Profile" },
    { &FOLDERID_Startup, CSIDL_STARTUP, TRUE, "Startup" },
    /* Not created by ReactOS setup */
    { &FOLDERID_CDBurning, CSIDL_CDBURN_AREA, FALSE, "CDBurning" },
};

static const struct
{
    const KNOWNFOLDERID *id;
    INT csidl;
    PCSTR name;
} s_VirtualFolders[] =
{
    { &FOLDERID_ComputerFolder, CSIDL_DRIVES, "ComputerFolder" },
    { &FOLDERID_ControlPanelFolder, CSIDL_CONTROLS, "ControlPanelFolder" },
    { &FOLDERID_RecycleBinFolder, CSIDL_BITBUCKET, "RecycleBinFolder" },
    { &FOLDERID_NetworkFolder, CSIDL_NETWORK, "NetworkFolder" },
};

static void
TestPath(const KNOWNFOLDERID *id, INT csidl, BOOL bMustExist)
{
    WCHAR szRef[MAX_PATH], szBuf[MAX_PATH];
    HRESULT hr, hrRef;
    PWSTR pszPath;
    UINT cch;

    hrRef = SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, szRef);
    if (bMustExist)
        ok_hex(hrRef, S_OK);

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, NULL, &pszPath);
    ok_hex(hr, hrRef);
    if (hr == S_OK)
    {
        ok(pszPath && !lstrcmpiW(pszPath, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszPath), wine_dbgstr_w(szRef));
        CoTaskMemFree(pszPath);
    }
    else
    {
        ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);
    }

    /* An unknown flag of later versions is ignored */
    pszPath = NULL;
    hr = pSHGetKnownFolderPath(*id, KF_FLAG_NO_APPCONTAINER_REDIRECTION_, NULL, &pszPath);
    ok_hex(hr, hrRef);
    if (hr == S_OK)
        ok(pszPath && !lstrcmpiW(pszPath, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszPath), wine_dbgstr_w(szRef));
    CoTaskMemFree(pszPath);

    hrRef = SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szRef);
    ok_hex(hrRef, S_OK);
    pszPath = NULL;
    hr = pSHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, NULL, &pszPath);
    ok_hex(hr, hrRef);
    if (hr == S_OK)
        ok(pszPath && !lstrcmpiW(pszPath, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszPath), wine_dbgstr_w(szRef));
    CoTaskMemFree(pszPath);

    hrRef = SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_DEFAULT, szRef);
    pszPath = NULL;
    hr = pSHGetKnownFolderPath(*id, KF_FLAG_DEFAULT_PATH, NULL, &pszPath);
    ok_hex(hr, hrRef);
    if (hr == S_OK)
        ok(pszPath && !lstrcmpiW(pszPath, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszPath), wine_dbgstr_w(szRef));
    CoTaskMemFree(pszPath);

    hrRef = SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, szRef);
    szBuf[0] = UNICODE_NULL;
    hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, _countof(szBuf));
    ok_hex(hr, hrRef);
    if (hr == S_OK)
    {
        ok(!lstrcmpiW(szBuf, szRef), "path %s, expected %s\n", wine_dbgstr_w(szBuf), wine_dbgstr_w(szRef));

        cch = lstrlenW(szRef);
        hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, cch);
        ok_hex(hr, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, cch + 1);
        ok_hex(hr, S_OK);
        hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, 0);
        ok_hex(hr, E_INVALIDARG);
    }
}

static void
CheckIDListPath(PIDLIST_ABSOLUTE pidl, PCWSTR pszExpected)
{
    WCHAR szBuf[MAX_PATH];

    ok(pidl && !ILIsEmpty(pidl), "empty ID list\n");
    szBuf[0] = UNICODE_NULL;
    ok(pidl && SHGetPathFromIDListW(pidl, szBuf), "no path from the ID list\n");
    ok(!lstrcmpiW(szBuf, pszExpected), "path %s, expected %s\n", wine_dbgstr_w(szBuf), wine_dbgstr_w(pszExpected));
}

static void
TestIDList(const KNOWNFOLDERID *id, INT csidl)
{
    WCHAR szRef[MAX_PATH], szUnverified[MAX_PATH];
    PIDLIST_ABSOLUTE pidl, pidlRef;
    HRESULT hr, hrRef;
    /* The desktop and My Documents are namespace items, whatever their directories */
    BOOL bAliased = (csidl == CSIDL_DESKTOP || csidl == CSIDL_PERSONAL);

    hrRef = SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, szRef);
    ok_hex(SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szUnverified), S_OK);

    pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderIDList(*id, 0, NULL, &pidl);
    ok_hex(hr, bAliased ? S_OK : hrRef);
    if (hr == S_OK)
    {
        pidlRef = NULL;
        ok_hex(SHGetFolderLocation(NULL, csidl, NULL, 0, &pidlRef), S_OK);
        ok(pidl && pidlRef && ILIsEqual(pidl, pidlRef), "ID list differs from SHGetFolderLocation\n");
        ILFree(pidlRef);
        ILFree(pidl);
    }
    else
    {
        ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);
    }

    if (hrRef == S_OK)
    {
        pidl = NULL;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_NO_ALIAS, NULL, &pidl);
        ok_hex(hr, S_OK);
        if (hr == S_OK)
            CheckIDListPath(pidl, szRef);
        ILFree(pidl);

        pidl = NULL;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_NO_ALIAS | KF_FLAG_SIMPLE_IDLIST, NULL, &pidl);
        ok_hex(hr, S_OK);
        if (hr == S_OK)
            CheckIDListPath(pidl, szRef);
        ILFree(pidl);
    }
    else if (!bAliased)
    {
        /* A missing folder: only a simple ID list can be built, and only without verification */
        pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_DONT_VERIFY, NULL, &pidl);
        ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);

        pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_SIMPLE_IDLIST, NULL, &pidl);
        ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);

        pidl = NULL;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_SIMPLE_IDLIST | KF_FLAG_DONT_VERIFY, NULL, &pidl);
        ok_hex(hr, S_OK);
        if (hr == S_OK)
            CheckIDListPath(pidl, szUnverified);
        ILFree(pidl);
    }
}

static void
TestItem(const KNOWNFOLDERID *id, INT csidl)
{
    WCHAR szRef[MAX_PATH];
    IShellItem *pItem;
    HRESULT hr, hrRef;
    PWSTR pszName;

    BOOL bAliased = (csidl == CSIDL_DESKTOP || csidl == CSIDL_PERSONAL);

    hrRef = SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, szRef);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, bAliased ? S_OK : hrRef);
    if (hr != S_OK)
    {
        ok(pItem == NULL, "item %p, expected NULL\n", pItem);
        return;
    }

    ok(pItem != NULL, "no item\n");
    if (!pItem)
        return;
    if (hrRef != S_OK)
    {
        pItem->Release();
        return;
    }
    pszName = NULL;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName);
    ok_hex(hr, S_OK);
    if (hr == S_OK)
        ok(!lstrcmpiW(pszName, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszName), wine_dbgstr_w(szRef));
    CoTaskMemFree(pszName);
    pItem->Release();
}

static void
TestVirtual(const KNOWNFOLDERID *id, INT csidl)
{
    PIDLIST_ABSOLUTE pidl, pidlRef;
    WCHAR szBuf[MAX_PATH];
    IShellItem *pItem;
    PWSTR pszPath;
    HRESULT hr;

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, NULL, &pszPath);
    ok_hex(hr, E_FAIL);
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    szBuf[0] = L'#';
    hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, _countof(szBuf));
    ok_hex(hr, E_FAIL);

    pidl = NULL;
    hr = pSHGetKnownFolderIDList(*id, 0, NULL, &pidl);
    ok_hex(hr, S_OK);
    pidlRef = NULL;
    ok_hex(SHGetFolderLocation(NULL, csidl, NULL, 0, &pidlRef), S_OK);
    ok(pidl && pidlRef && ILIsEqual(pidl, pidlRef), "ID list differs from SHGetFolderLocation\n");
    ILFree(pidlRef);
    ILFree(pidl);

    pItem = NULL;
    hr = pSHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, S_OK);
    ok(pItem != NULL, "no item\n");
    if (pItem)
        pItem->Release();
}

static void
TestUnknown(void)
{
    PIDLIST_ABSOLUTE pidl;
    WCHAR szBuf[MAX_PATH];
    IShellItem *pItem;
    PWSTR pszPath;
    HRESULT hr;

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(s_UnknownId, KF_FLAG_DEFAULT, NULL, &pszPath);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(s_UnknownId, KF_FLAG_CREATE, NULL, &pszPath);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    szBuf[0] = L'#';
    szBuf[1] = UNICODE_NULL;
    hr = pSHGetFolderPathEx(s_UnknownId, 0, NULL, szBuf, _countof(szBuf));
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));

    pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderIDList(s_UnknownId, 0, NULL, &pidl);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderItem(s_UnknownId, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    hr = pSHSetKnownFolderPath(s_UnknownId, 0, NULL, L"C:\\SHGetKnownFolderPath-not-created");
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
}

static void
TestFlags(void)
{
    PIDLIST_ABSOLUTE pidl;
    PWSTR pszPath;
    HRESULT hr;

    /* KF_FLAG_NOT_PARENT_RELATIVE is only valid together with KF_FLAG_DEFAULT_PATH */
    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(FOLDERID_Windows, KF_FLAG_NOT_PARENT_RELATIVE, NULL, &pszPath);
    ok_hex(hr, E_INVALIDARG);
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(s_UnknownId, KF_FLAG_NOT_PARENT_RELATIVE, NULL, &pszPath);
    ok_hex(hr, E_INVALIDARG);
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderPath(FOLDERID_Windows, KF_FLAG_ALIAS_ONLY, NULL, &pszPath);
    ok_hex(hr, E_INVALIDARG);
    ok(pszPath == NULL, "path %p, expected NULL\n", pszPath);

    pszPath = NULL;
    hr = pSHGetKnownFolderPath(FOLDERID_Windows, KF_FLAG_DEFAULT_PATH | KF_FLAG_NOT_PARENT_RELATIVE, NULL, &pszPath);
    ok_hex(hr, S_OK);
    ok(pszPath != NULL, "no path\n");
    CoTaskMemFree(pszPath);

    hr = pSHGetKnownFolderIDList(FOLDERID_Desktop, 0, NULL, NULL);
    ok_hex(hr, E_INVALIDARG);

    /* The desktop is the namespace root unless the file system folder is asked for */
    pidl = NULL;
    hr = pSHGetKnownFolderIDList(FOLDERID_Desktop, 0, NULL, &pidl);
    ok_hex(hr, S_OK);
    ok(pidl && ILIsEmpty(pidl), "desktop ID list is not empty\n");
    ILFree(pidl);
}

START_TEST(SHGetKnownFolderPath)
{
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    SIZE_T i;

    pSHGetKnownFolderPath = (FN_SHGetKnownFolderPath)GetProcAddress(hShell32, "SHGetKnownFolderPath");
    pSHGetFolderPathEx = (FN_SHGetFolderPathEx)GetProcAddress(hShell32, "SHGetFolderPathEx");
    pSHGetKnownFolderIDList = (FN_SHGetKnownFolderIDList)GetProcAddress(hShell32, "SHGetKnownFolderIDList");
    pSHGetKnownFolderItem = (FN_SHGetKnownFolderItem)GetProcAddress(hShell32, "SHGetKnownFolderItem");
    pSHSetKnownFolderPath = (FN_SHSetKnownFolderPath)GetProcAddress(hShell32, "SHSetKnownFolderPath");
    if (!pSHGetKnownFolderPath || !pSHGetFolderPathEx || !pSHGetKnownFolderIDList ||
        !pSHGetKnownFolderItem || !pSHSetKnownFolderPath)
    {
        skip("Known-folder functions not exported (NT6.1 exports needed)\n");
        return;
    }

    CoInitialize(NULL);

    for (i = 0; i < _countof(s_Folders); ++i)
    {
        winetest_push_context("%s", s_Folders[i].name);
        TestPath(s_Folders[i].id, s_Folders[i].csidl, s_Folders[i].bMustExist);
        TestIDList(s_Folders[i].id, s_Folders[i].csidl);
        TestItem(s_Folders[i].id, s_Folders[i].csidl);
        winetest_pop_context();
    }

    for (i = 0; i < _countof(s_VirtualFolders); ++i)
    {
        winetest_push_context("%s", s_VirtualFolders[i].name);
        TestVirtual(s_VirtualFolders[i].id, s_VirtualFolders[i].csidl);
        winetest_pop_context();
    }

    TestUnknown();
    TestFlags();

    CoUninitialize();
}
