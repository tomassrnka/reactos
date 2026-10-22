/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the folder the classic Save As dialog shows
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>
#include <windows.h>
#include <commdlg.h>
#include <dlgs.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

typedef enum
{
    MODE_INITIAL,   /* report the initial folder and cancel */
    MODE_BADPATH    /* type a name in a folder that does not exist, press Save */
} TEST_MODE;

static TEST_MODE s_Mode;
typedef struct
{
    WCHAR Path[MAX_PATH];   /* empty for a folder outside the file system (a Windows 7 library) */
    WCHAR Name[MAX_PATH];   /* display name, for messages */
    BYTE Pidl[1024];
    BOOL HasPidl;
} FOLDER_ID;

static WCHAR s_TypedName[MAX_PATH];
static FOLDER_ID s_InitialFolder;
static FOLDER_ID s_FinalFolder;
static WCHAR s_FinalName[MAX_PATH];
static INT s_BoxCount;
static HWND s_hwndLastBox;
static INT s_Ticks;
static BOOL s_NameSelected;
static BOOL s_TimedOut;

static BOOL CALLBACK FindBoxProc(HWND hwnd, LPARAM lParam)
{
    HWND *pFound = (HWND *)lParam;
    WCHAR szClass[16];

    if (hwnd == pFound[0] || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != pFound[0])
        return TRUE;
    if (!GetClassNameW(hwnd, szClass, _countof(szClass)) || wcscmp(szClass, L"#32770") != 0)
        return TRUE;
    pFound[1] = hwnd;
    return FALSE;
}

static HWND FindMessageBox(HWND hwndDlg)
{
    HWND Found[2] = { hwndDlg, NULL };
    EnumThreadWindows(GetCurrentThreadId(), FindBoxProc, (LPARAM)Found);
    return Found[1];
}

static BOOL GetDialogFolder(HWND hwndDlg, FOLDER_ID *pFolder)
{
    SHFILEINFOW Info;
    LRESULT cb;

    pFolder->Path[0] = pFolder->Name[0] = UNICODE_NULL;
    if (SendMessageW(hwndDlg, CDM_GETFOLDERPATH, _countof(pFolder->Path), (LPARAM)pFolder->Path) <= 1)
        pFolder->Path[0] = UNICODE_NULL;
    /* The result is the size the ID list needs; it is copied only when it fits */
    cb = SendMessageW(hwndDlg, CDM_GETFOLDERIDLIST, sizeof(pFolder->Pidl), (LPARAM)pFolder->Pidl);
    pFolder->HasPidl = (cb > 0 && cb <= (LRESULT)sizeof(pFolder->Pidl));
    if (pFolder->HasPidl &&
        SHGetFileInfoW((PCWSTR)pFolder->Pidl, 0, &Info, sizeof(Info), SHGFI_PIDL | SHGFI_DISPLAYNAME))
    {
        StringCchCopyW(pFolder->Name, _countof(pFolder->Name), Info.szDisplayName);
    }
    return pFolder->Path[0] || pFolder->HasPidl;
}

static void FinishDialog(HWND hwndDlg)
{
    GetDialogFolder(hwndDlg, &s_FinalFolder);
    SendMessageW(hwndDlg, CDM_GETSPEC, _countof(s_FinalName), (LPARAM)s_FinalName);
    PostMessageW(hwndDlg, WM_COMMAND, IDCANCEL, 0);
}

static BOOL CALLBACK FindEditProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];
    if (GetClassNameW(hwnd, szClass, _countof(szClass)) && !_wcsicmp(szClass, L"Edit"))
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static BOOL IsNameSelected(HWND hwndDlg)
{
    HWND hwndName = GetDlgItem(hwndDlg, cmb13), hwndEdit = NULL;
    DWORD dwStart = 1, dwEnd = 0;
    if (!hwndName)
        hwndName = GetDlgItem(hwndDlg, edt1);
    if (!hwndName)
        return FALSE;
    EnumChildWindows(hwndName, FindEditProc, (LPARAM)&hwndEdit);
    if (!hwndEdit)
        hwndEdit = hwndName;
    SendMessageW(hwndEdit, EM_GETSEL, (WPARAM)&dwStart, (LPARAM)&dwEnd);
    return dwStart == 0 && dwEnd == (DWORD)GetWindowTextLengthW(hwndEdit) && dwEnd > 0;
}

static UINT_PTR CALLBACK HookProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HWND hwndDlg = GetParent(hwnd);

    if (uMsg == WM_NOTIFY && ((LPOFNOTIFYW)lParam)->hdr.code == CDN_INITDONE)
    {
        /* Windows 7 sets the folder of the classic dialog after CDN_INITDONE */
        SetTimer(hwnd, 2, 100, NULL);
    }
    else if (uMsg == WM_TIMER && wParam == 2)
    {
        if (!GetDialogFolder(hwndDlg, &s_InitialFolder) && ++s_Ticks <= 30)
            return 0;
        KillTimer(hwnd, 2);
        s_Ticks = 0;
        if (s_Mode == MODE_INITIAL)
        {
            PostMessageW(hwndDlg, WM_COMMAND, IDCANCEL, 0);
        }
        else
        {
            SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, cmb13, (LPARAM)s_TypedName);
            SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, edt1, (LPARAM)s_TypedName);
            SetTimer(hwnd, 1, 100, NULL);
            PostMessageW(hwndDlg, WM_COMMAND, IDOK, 0);
        }
    }
    else if (uMsg == WM_TIMER && wParam == 1)
    {
        HWND hwndBox = FindMessageBox(hwndDlg);
        if (hwndBox)
        {
            if (hwndBox != s_hwndLastBox)
                s_BoxCount++;
            s_hwndLastBox = hwndBox;
            s_Ticks = 0;
            PostMessageW(hwndBox, WM_COMMAND, IDOK, 0);
        }
        else if (++s_Ticks > (s_BoxCount ? 5 : 30))
        {
            /* No further box for half a second after the first one */
            KillTimer(hwnd, 1);
            s_NameSelected = IsNameSelected(hwndDlg);
            FinishDialog(hwndDlg);
        }
    }
    return 0;
}

static BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];
    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, szClass, _countof(szClass)) && !wcscmp(szClass, L"#32770"))
        PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
    return TRUE;
}

static VOID CALLBACK WatchdogProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    s_TimedOut = TRUE;
    EnumThreadWindows(GetCurrentThreadId(), FindDialogProc, 0);
}

static void RunDialog(TEST_MODE Mode, PCWSTR pszCurDir, PCWSTR pszTyped)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR szFile[MAX_PATH] = L"";
    UINT_PTR idWatchdog;

    s_Mode = Mode;
    StringCchCopyW(s_TypedName, _countof(s_TypedName), pszTyped ? pszTyped : L"");
    ZeroMemory(&s_InitialFolder, sizeof(s_InitialFolder));
    ZeroMemory(&s_FinalFolder, sizeof(s_FinalFolder));
    s_FinalName[0] = UNICODE_NULL;
    s_BoxCount = s_Ticks = 0;
    s_hwndLastBox = NULL;
    s_NameSelected = FALSE;
    s_TimedOut = FALSE;

    ofn.lpstrFilter = L"Probe files\0*.r3q\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = _countof(szFile);
    /* No default extension: comdlg32 would otherwise look for a folder recently used for it */
    ofn.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpfnHook = HookProc;

    /* The dialog changes the current folder when it browses */
    ok(SetCurrentDirectoryW(pszCurDir), "SetCurrentDirectoryW failed: %lu\n", GetLastError());
    idWatchdog = SetTimer(NULL, 0, 20000, WatchdogProc);
    ok(!GetSaveFileNameW(&ofn), "GetSaveFileNameW returned TRUE\n");
    KillTimer(NULL, idWatchdog);
    ok(!s_TimedOut, "The dialog did not finish in time\n");
}

static void GetFolderForCompare(PCWSTR pszPath, PWSTR pszOut, SIZE_T cchOut)
{
    SIZE_T cch;
    DWORD cchLong = GetLongPathNameW(pszPath, pszOut, (DWORD)cchOut);
    if (!cchLong || cchLong >= cchOut)
        StringCchCopyW(pszOut, cchOut, pszPath);
    cch = wcslen(pszOut);
    if (cch > 3 && pszOut[cch - 1] == L'\\')
        pszOut[cch - 1] = UNICODE_NULL;
}

static BOOL IsSameFolder(PCWSTR psz1, PCWSTR psz2)
{
    WCHAR sz1[MAX_PATH], sz2[MAX_PATH];
    GetFolderForCompare(psz1, sz1, _countof(sz1));
    GetFolderForCompare(psz2, sz2, _countof(sz2));
    return !lstrcmpiW(sz1, sz2);
}

static BOOL IsSamePidl(LPCITEMIDLIST pidl1, LPCITEMIDLIST pidl2)
{
    IShellFolder *psfDesktop;
    HRESULT hr;
    if (FAILED(SHGetDesktopFolder(&psfDesktop)))
        return FALSE;
    hr = IShellFolder_CompareIDs(psfDesktop, 0, pidl1, pidl2);
    IShellFolder_Release(psfDesktop);
    return SUCCEEDED(hr) && (SHORT)HRESULT_CODE(hr) == 0;
}

static BOOL IsFolder(const FOLDER_ID *pFolder, PCWSTR pszPath)
{
    WCHAR szPath[MAX_PATH];
    LPITEMIDLIST pidl;
    BOOL bSame;
    if (pFolder->Path[0])
        return IsSameFolder(pFolder->Path, pszPath);
    if (!pFolder->HasPidl)
        return FALSE;
    /* A library shows its folders as items outside the file system that still have a path */
    if (SHGetPathFromIDListW((LPCITEMIDLIST)pFolder->Pidl, szPath))
        return IsSameFolder(szPath, pszPath);
    if (FAILED(SHParseDisplayName(pszPath, NULL, &pidl, 0, NULL)))
        return FALSE;
    bSame = IsSamePidl((LPCITEMIDLIST)pFolder->Pidl, pidl);
    CoTaskMemFree(pidl);
    return bSame;
}

/* Windows 7 shows the documents library for the personal folder */
static BOOL IsDocumentsLibrary(const FOLDER_ID *pFolder)
{
    typedef HRESULT (WINAPI *PFN_GETKNOWNFOLDERIDLIST)(REFGUID, DWORD, HANDLE, LPITEMIDLIST *);
    static const GUID FolderIdDocumentsLibrary =
        { 0x7b0db17d, 0x9cd2, 0x4a93, { 0x97, 0x33, 0x46, 0xcc, 0x89, 0x02, 0x2e, 0x7c } };
    PFN_GETKNOWNFOLDERIDLIST pfn;
    LPITEMIDLIST pidl;
    BOOL bSame;

    pfn = (PFN_GETKNOWNFOLDERIDLIST)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetKnownFolderIDList");
    if (pFolder->Path[0] || !pFolder->HasPidl || !pfn || FAILED(pfn(&FolderIdDocumentsLibrary, 0, NULL, &pidl)))
        return FALSE;
    bSame = IsSamePidl((LPCITEMIDLIST)pFolder->Pidl, pidl);
    CoTaskMemFree(pidl);
    return bSame;
}

static BOOL IsSameFolderId(const FOLDER_ID *p1, const FOLDER_ID *p2)
{
    if (p1->HasPidl && p2->HasPidl)
        return IsSamePidl((LPCITEMIDLIST)p1->Pidl, (LPCITEMIDLIST)p2->Pidl);
    return p1->Path[0] && IsSameFolder(p1->Path, p2->Path);
}

START_TEST(SaveDialogFolder)
{
    WCHAR szTemp[MAX_PATH], szDir[MAX_PATH], szFile[MAX_PATH], szPersonal[MAX_PATH], szTyped[MAX_PATH];
    WCHAR szOldDir[MAX_PATH];
    HANDLE hFile;
    BOOL bCreated;

    GetCurrentDirectoryW(_countof(szOldDir), szOldDir);
    GetTempPathW(_countof(szTemp), szTemp);
    StringCchPrintfW(szDir, _countof(szDir), L"%sr3q-%lu", szTemp, GetTickCount());
    bCreated = CreateDirectoryW(szDir, NULL);
    ok(bCreated, "CreateDirectoryW(%S) failed: %lu\n", szDir, GetLastError());
    if (!bCreated)
        return;
    ok(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, szPersonal) == S_OK, "SHGetFolderPathW failed\n");

    /* Every dialog here is cancelled, so the test leaves no recent folder behind for itself */

    /* No recent folder, no matching file in the current folder: the personal folder */
    RunDialog(MODE_INITIAL, szDir, NULL);
    ok(IsFolder(&s_InitialFolder, szPersonal) || IsDocumentsLibrary(&s_InitialFolder),
       "Initial folder '%S' ('%S'), expected '%S'\n", s_InitialFolder.Path, s_InitialFolder.Name, szPersonal);

    /* A matching file in the current folder: the current folder (Windows 2000 and XP rule;
     * Windows 7 reuses the folder of the application's previous dialog instead) */
    StringCchPrintfW(szFile, _countof(szFile), L"%s\\a.r3q", szDir);
    hFile = CreateFileW(szFile, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    if (hFile != INVALID_HANDLE_VALUE && LOBYTE(LOWORD(GetVersion())) < 6)
    {
        RunDialog(MODE_INITIAL, szDir, NULL);
        ok(IsFolder(&s_InitialFolder, szDir), "Initial folder '%S' ('%S'), expected '%S'\n", s_InitialFolder.Path, s_InitialFolder.Name, szDir);
    }

    /* A name in a folder that does not exist: the message, then the same folder and the same name.
     * The deepest existing folder of the name (the temporary folder) is not the dialog's folder. */
    StringCchPrintfW(szTyped, _countof(szTyped), L"%sr3q-missing-%lu\\x.r3q", szTemp, GetTickCount());
    RunDialog(MODE_BADPATH, szDir, szTyped);
    ok(s_BoxCount == 1, "Message boxes: %d\n", s_BoxCount);
    ok(IsSameFolderId(&s_FinalFolder, &s_InitialFolder), "Folder after the message '%S' ('%S'), expected '%S' ('%S')\n",
       s_FinalFolder.Path, s_FinalFolder.Name, s_InitialFolder.Path, s_InitialFolder.Name);
    ok(!lstrcmpiW(s_FinalName, szTyped), "Name after the message '%S', expected '%S'\n", s_FinalName, szTyped);
    ok(s_NameSelected, "The name is not selected after the message\n");

    SetCurrentDirectoryW(szOldDir);
    if (hFile != INVALID_HANDLE_VALUE)
        DeleteFileW(szFile);
    RemoveDirectoryW(szDir);
}
