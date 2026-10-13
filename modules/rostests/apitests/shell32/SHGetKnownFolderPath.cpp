/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for SHGetKnownFolderPath and the other known-folder functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"
#include <knownfolders.h>
#include <shobjidl.h>
#include <strsafe.h>

typedef HRESULT (WINAPI *FN_SHGetKnownFolderPath)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
typedef HRESULT (WINAPI *FN_SHGetFolderPathEx)(REFKNOWNFOLDERID, DWORD, HANDLE, LPWSTR, UINT);
typedef HRESULT (WINAPI *FN_SHGetKnownFolderIDList)(REFKNOWNFOLDERID, DWORD, HANDLE, PIDLIST_ABSOLUTE *);
typedef HRESULT (WINAPI *FN_SHGetKnownFolderItem)(REFKNOWNFOLDERID, DWORD, HANDLE, REFIID, void **);
typedef HRESULT (WINAPI *FN_SHSetKnownFolderPath)(REFKNOWNFOLDERID, DWORD, HANDLE, PCWSTR);
typedef HRESULT (WINAPI *FN_SHCreateItemInKnownFolder)(REFKNOWNFOLDERID, DWORD, PCWSTR, REFIID, void **);

static FN_SHGetKnownFolderPath pSHGetKnownFolderPath;
static FN_SHGetFolderPathEx pSHGetFolderPathEx;
static FN_SHGetKnownFolderIDList pSHGetKnownFolderIDList;
static FN_SHGetKnownFolderItem pSHGetKnownFolderItem;
static FN_SHSetKnownFolderPath pSHSetKnownFolderPath;
static FN_SHCreateItemInKnownFolder pSHCreateItemInKnownFolder;

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

static void CheckIDListPath(PIDLIST_ABSOLUTE pidl, PCWSTR pszExpected);

/* Known folders without a CSIDL: the expected path is the parent's path plus the relative path */
enum { BASE_PROFILE, BASE_LOCALAPPDATA, BASE_APPDATA, BASE_COMMONAPPDATA, BASE_PUBLIC, BASE_PROFILES, BASE_X64 };

static const struct
{
    const KNOWNFOLDERID *id;
    INT base;
    PCWSTR pszRelative;
    INT csidlX64;
    PCSTR name;
} s_DefFolders[] =
{
    { &FOLDERID_Downloads, BASE_PROFILE, L"Downloads", 0, "Downloads" },
    { &FOLDERID_Links, BASE_PROFILE, L"Links", 0, "Links" },
    { &FOLDERID_SavedGames, BASE_PROFILE, L"Saved Games", 0, "SavedGames" },
    { &FOLDERID_Contacts, BASE_PROFILE, L"Contacts", 0, "Contacts" },
    { &FOLDERID_SavedSearches, BASE_PROFILE, L"Searches", 0, "SavedSearches" },
    { &FOLDERID_LocalAppDataLow, BASE_PROFILE, L"AppData\\LocalLow", 0, "LocalAppDataLow" },
    { &FOLDERID_UserProgramFiles, BASE_LOCALAPPDATA, L"Programs", 0, "UserProgramFiles" },
    { &FOLDERID_UserProgramFilesCommon, BASE_LOCALAPPDATA, L"Programs\\Common", 0, "UserProgramFilesCommon" },
    { &FOLDERID_QuickLaunch, BASE_APPDATA, L"Microsoft\\Internet Explorer\\Quick Launch", 0, "QuickLaunch" },
    { &FOLDERID_Public, BASE_PUBLIC, NULL, 0, "Public" },
    { &FOLDERID_PublicDownloads, BASE_PUBLIC, L"Downloads", 0, "PublicDownloads" },
    { &FOLDERID_PublicLibraries, BASE_PUBLIC, L"Libraries", 0, "PublicLibraries" },
    { &FOLDERID_PublicGameTasks, BASE_COMMONAPPDATA, L"Microsoft\\Windows\\GameExplorer", 0, "PublicGameTasks" },
    { &FOLDERID_PublicRingtones, BASE_COMMONAPPDATA, L"Microsoft\\Windows\\Ringtones", 0, "PublicRingtones" },
    { &FOLDERID_UserProfiles, BASE_PROFILES, NULL, 0, "UserProfiles" },
    { &FOLDERID_ProgramFilesX64, BASE_X64, NULL, CSIDL_PROGRAM_FILES, "ProgramFilesX64" },
    { &FOLDERID_ProgramFilesCommonX64, BASE_X64, NULL, CSIDL_PROGRAM_FILES_COMMON, "ProgramFilesCommonX64" },
};

static const WCHAR s_szUserShellFolders[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
static const WCHAR s_szDownloadsValue[] = L"{374DE290-123F-4565-9164-39C4925E467B}";

/* FALSE: the folder does not exist for this process (the x64 folders in 32-bit code, also under WOW64).
 * bCheck: report a failed lookup of the base folder */
static BOOL
GetDefExpectedPath(SIZE_T i, HANDLE hToken, PWSTR pszPath, BOOL bCheck = TRUE)
{
    HRESULT hr;
    WCHAR szBuf[MAX_PATH];
    INT csidl = CSIDL_PROFILE;

    pszPath[0] = UNICODE_NULL;
    switch (s_DefFolders[i].base)
    {
        case BASE_LOCALAPPDATA: csidl = CSIDL_LOCAL_APPDATA; break;
        case BASE_APPDATA: csidl = CSIDL_APPDATA; break;
        case BASE_COMMONAPPDATA: csidl = CSIDL_COMMON_APPDATA; break;
        case BASE_X64:
            if (sizeof(void *) == 4)
                return FALSE;
            csidl = s_DefFolders[i].csidlX64;
            break;
        case BASE_PUBLIC:
            /* %PUBLIC% on NT 6; ReactOS keeps the NT 5 layout, where the All Users profile is the public folder */
            if (!GetEnvironmentVariableW(L"PUBLIC", szBuf, _countof(szBuf)) &&
                !GetEnvironmentVariableW(L"ALLUSERSPROFILE", szBuf, _countof(szBuf)))
            {
                szBuf[0] = UNICODE_NULL;
            }
            if (bCheck)
                ok(szBuf[0] != UNICODE_NULL, "no public folder in the environment\n");
            StringCchCopyW(pszPath, MAX_PATH, szBuf);
            csidl = -1;
            break;
    }
    if (csidl >= 0)
    {
        hr = SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_DONT_VERIFY, hToken, SHGFP_TYPE_CURRENT, pszPath);
        if (bCheck)
            ok_hex(hr, S_OK);
    }
    if (s_DefFolders[i].base == BASE_PROFILES)
        PathRemoveFileSpecW(pszPath);
    if (s_DefFolders[i].pszRelative)
        PathAppendW(pszPath, s_DefFolders[i].pszRelative);
    return TRUE;
}

static HRESULT
GetExistence(PCWSTR pszPath)
{
    if (GetFileAttributesW(pszPath) != INVALID_FILE_ATTRIBUTES)
        return S_OK;
    return HRESULT_FROM_WIN32(GetLastError());
}

static void
ExpectPath(const KNOWNFOLDERID *id, DWORD dwFlags, HANDLE hToken, HRESULT hrExpected, PCWSTR pszExpected)
{
    PWSTR pszPath = (PWSTR)(ULONG_PTR)0xdeadbeef;
    HRESULT hr;

    hr = pSHGetKnownFolderPath(*id, dwFlags, hToken, &pszPath);
    ok(hr == hrExpected, "flags 0x%lx: hr 0x%lx, expected 0x%lx\n", dwFlags, hr, hrExpected);
    if (hr == S_OK)
    {
        ok(pszPath && !lstrcmpiW(pszPath, pszExpected), "flags 0x%lx: path %s, expected %s\n",
           dwFlags, wine_dbgstr_w(pszPath), wine_dbgstr_w(pszExpected));
        CoTaskMemFree(pszPath);
    }
    else
    {
        ok(pszPath == NULL, "flags 0x%lx: path %p, expected NULL\n", dwFlags, pszPath);
    }
}

/* Whether the User Shell Folders value of a folder of the table, in the hive the shell reads
 * for it, names another path than its default one */
static BOOL
HasRedirectingValue(SIZE_T i, PCWSTR pszDefault)
{
    WCHAR szName[39], szValue[MAX_PATH + 1], szExpanded[MAX_PATH];
    DWORD cbValue, dwType, cch;
    BOOL bRedirected = FALSE;
    HKEY hRoot, hKey;

    /* The shell reads no value for FOLDERID_Public: it is neither KFD_PERUSER nor KFD_COMMON */
    if (IsEqualGUID(*s_DefFolders[i].id, FOLDERID_Public))
        return FALSE;
    switch (s_DefFolders[i].base)
    {
        case BASE_PROFILE: case BASE_LOCALAPPDATA: case BASE_APPDATA: hRoot = HKEY_CURRENT_USER; break;
        case BASE_PUBLIC: case BASE_COMMONAPPDATA: hRoot = HKEY_LOCAL_MACHINE; break;
        default: return FALSE;
    }
    StringFromGUID2(*s_DefFolders[i].id, szName, _countof(szName));
    if (RegOpenKeyExW(hRoot, s_szUserShellFolders, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    cbValue = sizeof(szValue) - sizeof(WCHAR);
    if (RegQueryValueExW(hKey, szName, NULL, &dwType, (LPBYTE)szValue, &cbValue) == ERROR_SUCCESS &&
        (dwType == REG_SZ || dwType == REG_EXPAND_SZ))
    {
        szValue[cbValue / sizeof(WCHAR)] = UNICODE_NULL;
        if (dwType == REG_SZ)
        {
            bRedirected = (lstrcmpiW(szValue, pszDefault) != 0);
        }
        else
        {
            cch = ExpandEnvironmentStringsW(szValue, szExpanded, _countof(szExpanded));
            bRedirected = (!cch || cch > _countof(szExpanded) || lstrcmpiW(szExpanded, pszDefault) != 0);
        }
    }
    RegCloseKey(hKey);
    return bRedirected;
}

/* Whether this machine redirects a folder of the table, a folder of the table it lies in, or the
 * CSIDL folder its path is built on: the baseline below expects the default layout */
static BOOL
IsDefFolderRedirected(SIZE_T i, PCWSTR pszExpected)
{
    WCHAR szOther[MAX_PATH], szCurrent[MAX_PATH], szDefault[MAX_PATH];
    SIZE_T j, cch;
    INT csidl = -1;

    if (HasRedirectingValue(i, pszExpected))
        return TRUE;
    for (j = 0; j < _countof(s_DefFolders); j++)
    {
        if (j == i || !GetDefExpectedPath(j, NULL, szOther, FALSE))
            continue;
        cch = wcslen(szOther);
        if (cch && !_wcsnicmp(pszExpected, szOther, cch) && pszExpected[cch] == L'\\' &&
            HasRedirectingValue(j, szOther))
        {
            return TRUE;
        }
    }

    switch (s_DefFolders[i].base)
    {
        case BASE_LOCALAPPDATA: csidl = CSIDL_LOCAL_APPDATA; break;
        case BASE_APPDATA: csidl = CSIDL_APPDATA; break;
        case BASE_COMMONAPPDATA: csidl = CSIDL_COMMON_APPDATA; break;
    }
    return csidl >= 0 &&
           SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szCurrent) == S_OK &&
           SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_DEFAULT, szDefault) == S_OK &&
           lstrcmpiW(szCurrent, szDefault) != 0;
}

static void
TestDefFolder(SIZE_T i)
{
    const KNOWNFOLDERID *id = s_DefFolders[i].id;
    WCHAR szExpected[MAX_PATH], szBuf[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    IShellItem *pItem;
    PWSTR pszName;
    HRESULT hr, hrExist;

    if (!GetDefExpectedPath(i, NULL, szExpected))
    {
        ExpectPath(id, KF_FLAG_DONT_VERIFY, NULL, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), NULL);
        ExpectPath(id, KF_FLAG_DEFAULT, NULL, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), NULL);
        pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
        hr = pSHGetKnownFolderIDList(*id, KF_FLAG_DEFAULT, NULL, &pidl);
        ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);
        return;
    }
    if (IsDefFolderRedirected(i, szExpected))
    {
        skip("the folder or the folder it is built on is redirected on this machine\n");
        return;
    }

    ExpectPath(id, KF_FLAG_DONT_VERIFY, NULL, S_OK, szExpected);
    ExpectPath(id, KF_FLAG_DEFAULT_PATH | KF_FLAG_DONT_VERIFY, NULL, S_OK, szExpected);
    ExpectPath(id, KF_FLAG_DEFAULT_PATH | KF_FLAG_NOT_PARENT_RELATIVE | KF_FLAG_DONT_VERIFY, NULL, S_OK, szExpected);

    /* A missing folder gives the error of the file system: path not found when its parent is missing too */
    hrExist = GetExistence(szExpected);
    ExpectPath(id, KF_FLAG_DEFAULT, NULL, hrExist, szExpected);
    ExpectPath(id, KF_FLAG_DEFAULT_PATH, NULL, hrExist, szExpected);

    szBuf[0] = L'#';
    szBuf[1] = UNICODE_NULL;
    hr = pSHGetFolderPathEx(*id, 0, NULL, szBuf, _countof(szBuf));
    ok_hex(hr, hrExist);
    if (hr == S_OK)
        ok(!lstrcmpiW(szBuf, szExpected), "path %s, expected %s\n", wine_dbgstr_w(szBuf), wine_dbgstr_w(szExpected));

    pidl = (PIDLIST_ABSOLUTE)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderIDList(*id, KF_FLAG_DEFAULT, NULL, &pidl);
    ok_hex(hr, hrExist);
    if (hr == S_OK)
        CheckIDListPath(pidl, szExpected);
    else
        ok(pidl == NULL, "pidl %p, expected NULL\n", pidl);
    if (hr == S_OK)
        ILFree(pidl);

    pidl = NULL;
    hr = pSHGetKnownFolderIDList(*id, KF_FLAG_SIMPLE_IDLIST | KF_FLAG_DONT_VERIFY, NULL, &pidl);
    ok_hex(hr, S_OK);
    if (hr == S_OK)
        CheckIDListPath(pidl, szExpected);
    ILFree(pidl);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, hrExist);
    if (hr == S_OK && pItem)
    {
        pszName = NULL;
        ok_hex(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName), S_OK);
        ok(pszName && !lstrcmpiW(pszName, szExpected), "path %s, expected %s\n", wine_dbgstr_w(pszName), wine_dbgstr_w(szExpected));
        CoTaskMemFree(pszName);
        pItem->Release();
    }
    else
    {
        ok(pItem == NULL, "item %p, expected NULL\n", pItem);
    }

    /* The default user's folder */
    if (s_DefFolders[i].base == BASE_PROFILE)
    {
        GetDefExpectedPath(i, (HANDLE)-1, szExpected);
        ExpectPath(id, KF_FLAG_DONT_VERIFY, (HANDLE)-1, S_OK, szExpected);
    }
}

/* The exact type and bytes of a User Shell Folders value, to put it back after a test */
typedef struct
{
    BOOL bKeyExisted;
    LONG lStatus;
    DWORD dwType;
    DWORD cbData;
    BYTE Data[(MAX_PATH + 1) * sizeof(WCHAR)];
} SAVED_VALUE;

static BOOL
SaveRegValue(HKEY hRoot, PCWSTR pszKey, PCWSTR pszName, SAVED_VALUE *pSaved)
{
    HKEY hKey;

    pSaved->cbData = sizeof(pSaved->Data);
    pSaved->lStatus = RegOpenKeyExW(hRoot, pszKey, 0, KEY_QUERY_VALUE, &hKey);
    pSaved->bKeyExisted = (pSaved->lStatus == ERROR_SUCCESS);
    if (pSaved->lStatus == ERROR_SUCCESS)
    {
        pSaved->lStatus = RegQueryValueExW(hKey, pszName, NULL, &pSaved->dwType, pSaved->Data, &pSaved->cbData);
        RegCloseKey(hKey);
    }
    ok(pSaved->lStatus == ERROR_SUCCESS || pSaved->lStatus == ERROR_FILE_NOT_FOUND,
       "cannot save %s: %ld\n", wine_dbgstr_w(pszName), pSaved->lStatus);
    return pSaved->lStatus == ERROR_SUCCESS || pSaved->lStatus == ERROR_FILE_NOT_FOUND;
}

static void
RestoreRegValue(HKEY hRoot, PCWSTR pszKey, PCWSTR pszName, const SAVED_VALUE *pSaved)
{
    HKEY hKey;
    LONG ret;

    ret = RegOpenKeyExW(hRoot, pszKey, 0, KEY_SET_VALUE, &hKey);
    ok_long(ret, ERROR_SUCCESS);
    if (ret != ERROR_SUCCESS)
        return;
    if (pSaved->lStatus == ERROR_SUCCESS)
        ret = RegSetValueExW(hKey, pszName, 0, pSaved->dwType, pSaved->Data, pSaved->cbData);
    else
        ret = RegDeleteValueW(hKey, pszName);
    ok(ret == ERROR_SUCCESS, "cannot restore %s: %ld\n", wine_dbgstr_w(pszName), ret);
    RegCloseKey(hKey);
    /* Writing the value created the key */
    if (!pSaved->bKeyExisted)
    {
        ret = RegDeleteKeyW(hRoot, pszKey);
        ok(ret == ERROR_SUCCESS, "cannot delete %s: %ld\n", wine_dbgstr_w(pszKey), ret);
    }
}

static void
RemoveTestFolder(PCWSTR pszPath)
{
    WCHAR szIni[MAX_PATH];
    DWORD dwError = ERROR_SUCCESS;

    /* Windows writes a desktop.ini when it creates Downloads */
    StringCchCopyW(szIni, _countof(szIni), pszPath);
    PathAppendW(szIni, L"desktop.ini");
    SetFileAttributesW(szIni, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(szIni);
    SetFileAttributesW(pszPath, FILE_ATTRIBUTE_DIRECTORY);
    if (!RemoveDirectoryW(pszPath))
        dwError = GetLastError();
    ok(dwError == ERROR_SUCCESS, "RemoveDirectoryW(%s) failed with %lu\n", wine_dbgstr_w(pszPath), dwError);
}

/* Windows keeps the paths a process has resolved, so every step after a change of the registry or of the
 * file system runs in a new process: SHGetKnownFolderPath.exe SHGetKnownFolderPath <step> */
static void
RunChild(PCSTR pszStep)
{
    char szExe[MAX_PATH], szCmd[MAX_PATH + 64];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    GetModuleFileNameA(NULL, szExe, _countof(szExe));
    StringCchPrintfA(szCmd, _countof(szCmd), "\"%s\" SHGetKnownFolderPath %s", szExe, pszStep);
    if (!CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessA(%s) failed with %lu\n", szCmd, GetLastError());
        return;
    }
    wait_child_process(pi.hProcess);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

static void
GetRedirectedPath(PWSTR pszPath, PWSTR pszValue, SIZE_T cchValue)
{
    WCHAR szName[64];

    ok_hex(SHGetFolderPathW(NULL, CSIDL_PROFILE | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, pszPath), S_OK);
    StringCchPrintfW(szName, _countof(szName), L"kfp-redirected-%lu", GetCurrentProcessId());
    PathAppendW(pszPath, szName);
    if (pszValue)
        StringCchPrintfW(pszValue, cchValue, L"%%USERPROFILE%%\\%s", szName);
}

static void
CheckItemPath(HRESULT hr, IShellItem *pItem, PCWSTR pszExpected)
{
    PWSTR pszName = NULL;

    ok_hex(hr, S_OK);
    if (hr != S_OK || !pItem)
        return;
    ok_hex(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName), S_OK);
    ok(pszName && !lstrcmpiW(pszName, pszExpected), "path %s, expected %s\n", wine_dbgstr_w(pszName), wine_dbgstr_w(pszExpected));
    CoTaskMemFree(pszName);
    pItem->Release();
}

/* Child steps; the redirected folder name carries the parent's process id, passed in KFP_REDIRECTED */
static void
ChildRedirect1(PCWSTR pszRedirected)
{
    WCHAR szDefault[MAX_PATH];
    IShellItem *pItem;
    DWORD dwAttributes;
    HRESULT hr;
    SIZE_T i;

    for (i = 0; !IsEqualGUID(*s_DefFolders[i].id, FOLDERID_Downloads); ++i)
        ;
    GetDefExpectedPath(i, NULL, szDefault);

    ExpectPath(&FOLDERID_Downloads, KF_FLAG_DEFAULT, NULL, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), NULL);
    ExpectPath(&FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, NULL, S_OK, pszRedirected);
    ExpectPath(&FOLDERID_Downloads, KF_FLAG_DEFAULT_PATH | KF_FLAG_DONT_VERIFY, NULL, S_OK, szDefault);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);
    ok(GetFileAttributesW(pszRedirected) == INVALID_FILE_ATTRIBUTES, "created without KF_FLAG_CREATE\n");

    ExpectPath(&FOLDERID_Downloads, KF_FLAG_CREATE, NULL, S_OK, pszRedirected);
    dwAttributes = GetFileAttributesW(pszRedirected);
    ok(dwAttributes != INVALID_FILE_ATTRIBUTES && (dwAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
       (dwAttributes & FILE_ATTRIBUTE_READONLY), "attributes 0x%lx\n", dwAttributes);
}

static void
ChildRedirect2(PCWSTR pszRedirected)
{
    WCHAR szFile[MAX_PATH];
    IShellItem *pItem;
    IUnknown *pUnk;
    HRESULT hr;

    StringCchCopyW(szFile, _countof(szFile), pszRedirected);
    PathAppendW(szFile, L"kfp-item.txt");

    /* A plain string value is used as it is */
    ExpectPath(&FOLDERID_Downloads, KF_FLAG_DEFAULT, NULL, S_OK, pszRedirected);

    pItem = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, L"kfp-item.txt", IID_IShellItem, (void **)&pItem);
    CheckItemPath(hr, pItem, szFile);

    pItem = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    CheckItemPath(hr, pItem, pszRedirected);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, L"kfp-missing.txt", IID_IShellItem, (void **)&pItem);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, L"", IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_INVALIDARG);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    pUnk = (IUnknown *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, L"kfp-item.txt", IID_IStream, (void **)&pUnk);
    ok_hex(hr, E_NOINTERFACE);
    ok(pUnk == NULL, "object %p, expected NULL\n", pUnk);

    pUnk = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Downloads, KF_FLAG_DEFAULT, L"kfp-item.txt", IID_IUnknown, (void **)&pUnk);
    ok_hex(hr, S_OK);
    if (pUnk)
        pUnk->Release();
}

static void
GetProgramsPaths(PWSTR pszPrograms, PWSTR pszCommon)
{
    ok_hex(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, pszPrograms), S_OK);
    PathAppendW(pszPrograms, L"Programs");
    StringCchCopyW(pszCommon, MAX_PATH, pszPrograms);
    PathAppendW(pszCommon, L"Common");
}

/* The per-user Programs folder is missing */
static void
ChildPrograms1(void)
{
    WCHAR szPrograms[MAX_PATH], szCommon[MAX_PATH];
    IShellItem *pItem;
    DWORD dwAttributes;
    HRESULT hr;

    GetProgramsPaths(szPrograms, szCommon);
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT, NULL, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), NULL);
    ExpectPath(&FOLDERID_UserProgramFilesCommon, KF_FLAG_DEFAULT, NULL, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), NULL);
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DONT_VERIFY, NULL, S_OK, szPrograms);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);
    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT, L"x.txt", IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);
    ok(GetFileAttributesW(szPrograms) == INVALID_FILE_ATTRIBUTES, "created without KF_FLAG_CREATE\n");

    /* KF_FLAG_CREATE also applies with KF_FLAG_DONT_VERIFY; this folder is not made read-only */
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_CREATE | KF_FLAG_DONT_VERIFY, NULL, S_OK, szPrograms);
    dwAttributes = GetFileAttributesW(szPrograms);
    ok(dwAttributes != INVALID_FILE_ATTRIBUTES && !(dwAttributes & FILE_ATTRIBUTE_READONLY), "attributes 0x%lx\n", dwAttributes);
}

/* The per-user Programs folder exists, its Common subfolder does not */
static void
ChildPrograms2(void)
{
    WCHAR szPrograms[MAX_PATH], szCommon[MAX_PATH];

    GetProgramsPaths(szPrograms, szCommon);
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT, NULL, S_OK, szPrograms);
    ExpectPath(&FOLDERID_UserProgramFilesCommon, KF_FLAG_DEFAULT, NULL, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), NULL);
    ExpectPath(&FOLDERID_UserProgramFilesCommon, KF_FLAG_CREATE, NULL, S_OK, szCommon);
    ok(GetFileAttributesW(szCommon) != INVALID_FILE_ATTRIBUTES, "%s not created\n", wine_dbgstr_w(szCommon));
}

/* SHCreateItemInKnownFolder resolves the folder with the caller's flags: KF_FLAG_CREATE creates it */
static void
ChildPrograms3(void)
{
    WCHAR szPrograms[MAX_PATH], szCommon[MAX_PATH];
    IShellItem *pItem = NULL;
    HRESULT hr;

    GetProgramsPaths(szPrograms, szCommon);
    hr = pSHCreateItemInKnownFolder(FOLDERID_UserProgramFiles, KF_FLAG_CREATE, NULL, IID_IShellItem, (void **)&pItem);
    CheckItemPath(hr, pItem, szPrograms);
}

static void ChildParentRedirected(PCWSTR pszRedirectedParent);

static BOOL
RunChildStep(PCSTR pszStep)
{
    WCHAR szRedirected[MAX_PATH];

    if (!GetEnvironmentVariableW(L"KFP_REDIRECTED", szRedirected, _countof(szRedirected)))
        szRedirected[0] = UNICODE_NULL;

    if (!strcmp(pszStep, "redirect1"))
        ChildRedirect1(szRedirected);
    else if (!strcmp(pszStep, "redirect2"))
        ChildRedirect2(szRedirected);
    else if (!strcmp(pszStep, "redirect3"))
        ExpectPath(&FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, NULL, S_OK, szRedirected);
    else if (!strcmp(pszStep, "default1"))
        ExpectPath(&FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, (HANDLE)-1, S_OK, szRedirected);
    else if (!strcmp(pszStep, "parent"))
        ChildParentRedirected(szRedirected);
    else if (!strcmp(pszStep, "programs1"))
        ChildPrograms1();
    else if (!strcmp(pszStep, "programs2"))
        ChildPrograms2();
    else if (!strcmp(pszStep, "programs3"))
        ChildPrograms3();
    else
        return FALSE;
    return TRUE;
}

/* Downloads redirected through its User Shell Folders value to a missing folder */
static void
TestDownloadsRedirected(void)
{
    WCHAR szRedirected[MAX_PATH], szValue[MAX_PATH], szFile[MAX_PATH], szUser[MAX_PATH];
    SAVED_VALUE Saved;
    BOOL bFolder;
    HANDLE hFile;

    GetRedirectedPath(szRedirected, szValue, _countof(szValue));
    if (GetFileAttributesW(szRedirected) != INVALID_FILE_ATTRIBUTES)
    {
        skip("%s exists\n", wine_dbgstr_w(szRedirected));
        return;
    }
    SetEnvironmentVariableW(L"KFP_REDIRECTED", szRedirected);

    if (!SaveRegValue(HKEY_CURRENT_USER, s_szUserShellFolders, s_szDownloadsValue, &Saved))
        return;
    if (RegSetStringEx(HKEY_CURRENT_USER, s_szUserShellFolders, s_szDownloadsValue, szValue, REG_EXPAND_SZ) != ERROR_SUCCESS)
    {
        skip("cannot write the User Shell Folders value\n");
        return;
    }
    RunChild("redirect1");
    bFolder = (GetFileAttributesW(szRedirected) != INVALID_FILE_ATTRIBUTES);
    ok(bFolder, "%s not created\n", wine_dbgstr_w(szRedirected));

    if (bFolder)
    {
        ok_int(RegSetString(HKEY_CURRENT_USER, s_szUserShellFolders, s_szDownloadsValue, szRedirected), ERROR_SUCCESS);
        StringCchCopyW(szFile, _countof(szFile), szRedirected);
        PathAppendW(szFile, L"kfp-item.txt");
        hFile = CreateFileW(szFile, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
        if (hFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(hFile);
            RunChild("redirect2");
            DeleteFileW(szFile);
        }
        RemoveTestFolder(szRedirected);
    }

    /* Every variable of the value is expanded, also after the profile variable */
    if (GetEnvironmentVariableW(L"USERNAME", szUser, _countof(szUser)))
    {
        StringCchPrintfW(szValue, _countof(szValue), L"%%USERPROFILE%%\\kfp-%%USERNAME%%-%lu", GetCurrentProcessId());
        ok_hex(SHGetFolderPathW(NULL, CSIDL_PROFILE | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szRedirected), S_OK);
        StringCchPrintfW(szFile, _countof(szFile), L"kfp-%s-%lu", szUser, GetCurrentProcessId());
        PathAppendW(szRedirected, szFile);
        SetEnvironmentVariableW(L"KFP_REDIRECTED", szRedirected);
        ok_int(RegSetStringEx(HKEY_CURRENT_USER, s_szUserShellFolders, s_szDownloadsValue, szValue, REG_EXPAND_SZ), ERROR_SUCCESS);
        RunChild("redirect3");
    }

    RestoreRegValue(HKEY_CURRENT_USER, s_szUserShellFolders, s_szDownloadsValue, &Saved);
}

/* The default user's Downloads redirected through a value that starts with a variable other than the
 * profile variables: it is expanded without the default user's pseudo-token */
static void
TestDefaultUserRedirected(void)
{
    static const WCHAR szDefaultKey[] = L".Default\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
    WCHAR szValue[MAX_PATH], szExpected[MAX_PATH];
    SAVED_VALUE Saved;
    DWORD cch;

    StringCchPrintfW(szValue, _countof(szValue), L"%%SystemRoot%%\\kfp-default-%lu", GetCurrentProcessId());
    cch = ExpandEnvironmentStringsW(szValue, szExpected, _countof(szExpected));
    ok(cch && cch <= _countof(szExpected), "ExpandEnvironmentStringsW failed with %lu\n", GetLastError());
    if (!cch || cch > _countof(szExpected))
        return;
    if (!SaveRegValue(HKEY_USERS, szDefaultKey, s_szDownloadsValue, &Saved))
        return;
    if (RegSetStringEx(HKEY_USERS, szDefaultKey, s_szDownloadsValue, szValue, REG_EXPAND_SZ) != ERROR_SUCCESS)
    {
        skip("cannot write the default user's User Shell Folders value\n");
        return;
    }
    SetEnvironmentVariableW(L"KFP_REDIRECTED", szExpected);
    RunChild("default1");
    RestoreRegValue(HKEY_USERS, szDefaultKey, s_szDownloadsValue, &Saved);
}

/* With the parent redirected, the default path follows the parent's current path,
 * and the parent's default path with KF_FLAG_NOT_PARENT_RELATIVE */
static void
ChildParentRedirected(PCWSTR pszRedirectedParent)
{
    WCHAR szCurrent[MAX_PATH], szDefault[MAX_PATH];

    ok_hex(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szCurrent), S_OK);
    ok(!lstrcmpiW(szCurrent, pszRedirectedParent), "local application data %s, expected %s\n",
       wine_dbgstr_w(szCurrent), wine_dbgstr_w(pszRedirectedParent));
    PathAppendW(szCurrent, L"Programs");
    ok_hex(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_DEFAULT, szDefault), S_OK);
    PathAppendW(szDefault, L"Programs");

    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DONT_VERIFY, NULL, S_OK, szCurrent);
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT_PATH | KF_FLAG_DONT_VERIFY, NULL, S_OK, szCurrent);
    ExpectPath(&FOLDERID_UserProgramFiles, KF_FLAG_DEFAULT_PATH | KF_FLAG_NOT_PARENT_RELATIVE | KF_FLAG_DONT_VERIFY, NULL,
               S_OK, szDefault);
}

static void
TestParentRedirected(void)
{
    static const WCHAR szLocalAppData[] = L"Local AppData";
    WCHAR szRedirected[MAX_PATH], szValue[MAX_PATH], szName[64];
    SAVED_VALUE Saved;

    ok_hex(SHGetFolderPathW(NULL, CSIDL_PROFILE | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szRedirected), S_OK);
    StringCchPrintfW(szName, _countof(szName), L"kfp-local-%lu", GetCurrentProcessId());
    PathAppendW(szRedirected, szName);
    StringCchPrintfW(szValue, _countof(szValue), L"%%USERPROFILE%%\\%s", szName);
    SetEnvironmentVariableW(L"KFP_REDIRECTED", szRedirected);

    if (!SaveRegValue(HKEY_CURRENT_USER, s_szUserShellFolders, szLocalAppData, &Saved))
        return;
    ok_int(RegSetStringEx(HKEY_CURRENT_USER, s_szUserShellFolders, szLocalAppData, szValue, REG_EXPAND_SZ), ERROR_SUCCESS);
    RunChild("parent");
    RestoreRegValue(HKEY_CURRENT_USER, s_szUserShellFolders, szLocalAppData, &Saved);
    /* ReactOS copies the value to Shell Folders when it reads it; read it back */
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szValue);
}

/* A missing per-user folder and its subfolder, created on request */
static void
TestUserProgramFilesCreate(void)
{
    WCHAR szPrograms[MAX_PATH], szCommon[MAX_PATH];

    GetProgramsPaths(szPrograms, szCommon);
    if (GetFileAttributesW(szPrograms) != INVALID_FILE_ATTRIBUTES)
    {
        skip("%s exists\n", wine_dbgstr_w(szPrograms));
        return;
    }

    RunChild("programs1");
    RunChild("programs2");
    RemoveTestFolder(szCommon);
    RemoveTestFolder(szPrograms);
    RunChild("programs3");
    RemoveTestFolder(szPrograms);
}

static void
TestCreateItem(void)
{
    WCHAR szRef[MAX_PATH], szDrive[4];
    IShellItem *pItem;
    PWSTR pszName;
    HRESULT hr;

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(s_UnknownId, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(s_UnknownId, KF_FLAG_DEFAULT, L"x.txt", IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    /* Invalid flags fail like a folder that cannot be resolved */
    pItem = (IShellItem *)(ULONG_PTR)0xdeadbeef;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Documents, KF_FLAG_NOT_PARENT_RELATIVE, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, E_FAIL);
    ok(pItem == NULL, "item %p, expected NULL\n", pItem);

    ok_hex(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, szRef), S_OK);
    pItem = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_Documents, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, S_OK);
    if (hr == S_OK && pItem)
    {
        pszName = NULL;
        ok_hex(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName), S_OK);
        ok(pszName && !lstrcmpiW(pszName, szRef), "path %s, expected %s\n", wine_dbgstr_w(pszName), wine_dbgstr_w(szRef));
        CoTaskMemFree(pszName);
        pItem->Release();
    }

    pItem = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, S_OK);
    if (pItem)
        pItem->Release();

    GetWindowsDirectoryW(szRef, _countof(szRef));
    StringCchCopyNW(szDrive, _countof(szDrive), szRef, 3);
    pItem = NULL;
    hr = pSHCreateItemInKnownFolder(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, szDrive, IID_IShellItem, (void **)&pItem);
    ok_hex(hr, S_OK);
    if (hr == S_OK && pItem)
    {
        pszName = NULL;
        ok_hex(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName), S_OK);
        ok(pszName && !lstrcmpiW(pszName, szDrive), "path %s, expected %s\n", wine_dbgstr_w(pszName), wine_dbgstr_w(szDrive));
        CoTaskMemFree(pszName);
        pItem->Release();
    }
}

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
    char **argv;
    SIZE_T i;

    pSHGetKnownFolderPath = (FN_SHGetKnownFolderPath)GetProcAddress(hShell32, "SHGetKnownFolderPath");
    pSHGetFolderPathEx = (FN_SHGetFolderPathEx)GetProcAddress(hShell32, "SHGetFolderPathEx");
    pSHGetKnownFolderIDList = (FN_SHGetKnownFolderIDList)GetProcAddress(hShell32, "SHGetKnownFolderIDList");
    pSHGetKnownFolderItem = (FN_SHGetKnownFolderItem)GetProcAddress(hShell32, "SHGetKnownFolderItem");
    pSHSetKnownFolderPath = (FN_SHSetKnownFolderPath)GetProcAddress(hShell32, "SHSetKnownFolderPath");
    pSHCreateItemInKnownFolder = (FN_SHCreateItemInKnownFolder)GetProcAddress(hShell32, "SHCreateItemInKnownFolder");
    if (!pSHGetKnownFolderPath || !pSHGetFolderPathEx || !pSHGetKnownFolderIDList ||
        !pSHGetKnownFolderItem || !pSHSetKnownFolderPath || !pSHCreateItemInKnownFolder)
    {
        skip("Known-folder functions not exported (NT6.1 exports needed)\n");
        return;
    }

    CoInitialize(NULL);

    if (winetest_get_mainargs(&argv) >= 3)
    {
        ok(RunChildStep(argv[2]), "unknown step %s\n", argv[2]);
        CoUninitialize();
        return;
    }

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

    for (i = 0; i < _countof(s_DefFolders); ++i)
    {
        winetest_push_context("%s", s_DefFolders[i].name);
        TestDefFolder(i);
        winetest_pop_context();
    }

    TestUnknown();
    TestFlags();
    TestCreateItem();
    TestUserProgramFilesCreate();
    TestDownloadsRedirected();
    TestDefaultUserRedirected();
    TestParentRedirected();

    CoUninitialize();
}
