/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test installing a package that cannot be opened
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>

#include <wtypes.h>
#include <objbase.h>
#include <msi.h>
#include <strsafe.h>

static WCHAR g_Dir[MAX_PATH];
static UINT g_UiMessages;
static BOOL g_ApiHung;

static INT CALLBACK ExternalUi(LPVOID pvContext, UINT uType, LPCWSTR pszMessage)
{
    trace("external UI message 0x%08x '%S'\n", uType, pszMessage ? pszMessage : L"(null)");
    g_UiMessages++;
    return IDOK;
}

typedef struct _WINDOW_SEARCH
{
    DWORD dwPid;
    UINT Count;
    HWND Seen[8];
    WCHAR szTitle[128];
    WCHAR szStatic[512];
} WINDOW_SEARCH;

static BOOL CALLBACK FindStaticText(HWND hWnd, LPARAM lParam)
{
    WINDOW_SEARCH *pSearch = (WINDOW_SEARCH *)lParam;
    WCHAR szText[512], szClass[64];

    szText[0] = szClass[0] = 0;
    GetClassNameW(hWnd, szClass, _countof(szClass));
    if (_wcsicmp(szClass, L"Static"))
        return TRUE;
    SendMessageTimeoutW(hWnd, WM_GETTEXT, _countof(szText), (LPARAM)szText, SMTO_ABORTIFHUNG, 2000, NULL);
    if (wcslen(szText) > wcslen(pSearch->szStatic))
        StringCchCopyW(pSearch->szStatic, _countof(pSearch->szStatic), szText);
    return TRUE;
}

/* Records and dismisses the visible top-level windows of a process */
static BOOL CALLBACK CloseProcessWindow(HWND hWnd, LPARAM lParam)
{
    WINDOW_SEARCH *pSearch = (WINDOW_SEARCH *)lParam;
    DWORD dwPid;
    UINT i;

    GetWindowThreadProcessId(hWnd, &dwPid);
    if (dwPid != pSearch->dwPid || !IsWindowVisible(hWnd))
        return TRUE;
    for (i = 0; i < min(pSearch->Count, _countof(pSearch->Seen)); i++)
    {
        if (pSearch->Seen[i] == hWnd)
            break;
    }
    if (i == min(pSearch->Count, _countof(pSearch->Seen)))
    {
        SendMessageTimeoutW(hWnd, WM_GETTEXT, _countof(pSearch->szTitle), (LPARAM)pSearch->szTitle, SMTO_ABORTIFHUNG, 2000, NULL);
        EnumChildWindows(hWnd, FindStaticText, lParam);
        if (pSearch->Count < _countof(pSearch->Seen))
            pSearch->Seen[pSearch->Count] = hWnd;
        pSearch->Count++;
    }
    PostMessageW(hWnd, WM_COMMAND, IDOK, 0);
    return TRUE;
}

typedef struct _API_CALL
{
    WCHAR szPath[MAX_PATH];
    BOOL bOpenPackage;
    UINT Result;
} API_CALL;

static DWORD WINAPI ApiThread(LPVOID pvParam)
{
    API_CALL *pCall = pvParam;
    MSIHANDLE hPackage = 0;

    if (pCall->bOpenPackage)
    {
        pCall->Result = MsiOpenPackageW(pCall->szPath, &hPackage);
        if (hPackage)
            MsiCloseHandle(hPackage);
    }
    else
    {
        pCall->Result = MsiInstallProductW(pCall->szPath, NULL);
    }
    return 0;
}

/* Calls the API in a thread; any window it shows is counted and dismissed. After a hang the
   thread keeps its call data and no further API call is made. */
static UINT CallApi(LPCWSTR pszPath, BOOL bOpenPackage, UINT *pWindows)
{
    WINDOW_SEARCH Search;
    API_CALL *pCall;
    HANDLE hThread;
    UINT Result;
    int i;

    *pWindows = 0;
    if (g_ApiHung)
        return 0xdeadbeef;
    pCall = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pCall));
    if (!pCall)
        return 0xdeadbeef;
    StringCchCopyW(pCall->szPath, _countof(pCall->szPath), pszPath);
    pCall->bOpenPackage = bOpenPackage;
    pCall->Result = 0xdeadbeef;
    ZeroMemory(&Search, sizeof(Search));
    Search.dwPid = GetCurrentProcessId();
    hThread = CreateThread(NULL, 0, ApiThread, pCall, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hThread)
    {
        HeapFree(GetProcessHeap(), 0, pCall);
        return 0xdeadbeef;
    }
    for (i = 0; i < 60 && WaitForSingleObject(hThread, 500) == WAIT_TIMEOUT; i++)
    {
        if (i >= 4)
            EnumWindows(CloseProcessWindow, (LPARAM)&Search);
    }
    *pWindows = Search.Count;
    if (WaitForSingleObject(hThread, 0) != WAIT_OBJECT_0)
    {
        ok(FALSE, "%s did not return within 30 s\n", bOpenPackage ? "MsiOpenPackageW" : "MsiInstallProductW");
        g_ApiHung = TRUE;
        CloseHandle(hThread);
        return 0xdeadbeef;
    }
    CloseHandle(hThread);
    Result = pCall->Result;
    HeapFree(GetProcessHeap(), 0, pCall);
    return Result;
}

/* Runs msiexec; any window it shows is recorded and dismissed */
static DWORD RunMsiexec(LPCWSTR pszPath, LPCWSTR pszUi, WINDOW_SEARCH *pSearch)
{
    WCHAR szCmd[MAX_PATH * 2];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD dwExit = 0xdeadbeef;
    int i;

    ZeroMemory(pSearch, sizeof(*pSearch));
    StringCchPrintfW(szCmd, _countof(szCmd), L"msiexec.exe /i \"%s\" %s", pszPath, pszUi);
    if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(FALSE, "CreateProcessW(%S) failed: %lu\n", szCmd, GetLastError());
        return dwExit;
    }
    pSearch->dwPid = pi.dwProcessId;
    for (i = 0; i < 120 && WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT; i++)
    {
        if (i >= 6)
            EnumWindows(CloseProcessWindow, (LPARAM)pSearch);
    }
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pi.hProcess, &dwExit);
    }
    else
    {
        ok(FALSE, "%S did not exit within 60 s\n", szCmd);
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return dwExit;
}

static void WriteFileBytes(LPCWSTR pszPath, const void *pvData, DWORD cbData)
{
    HANDLE hFile;
    DWORD dwWritten;

    hFile = CreateFileW(pszPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", pszPath, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    if (cbData)
        ok(WriteFile(hFile, pvData, cbData, &dwWritten, NULL), "WriteFile failed: %lu\n", GetLastError());
    CloseHandle(hFile);
}

static void CreateStorageFile(LPCWSTR pszPath)
{
    IStorage *pStorage;
    HRESULT hr;

    hr = StgCreateDocfile(pszPath, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &pStorage);
    ok(hr == S_OK, "StgCreateDocfile(%S) failed: 0x%08lx\n", pszPath, hr);
    if (SUCCEEDED(hr))
        IStorage_Release(pStorage);
}

static void GetSystemMessage(UINT uError, PWSTR pszText, DWORD cchText)
{
    DWORD cch;

    cch = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, uError, 0, pszText, cchText, NULL);
    while (cch && (pszText[cch - 1] == L'\r' || pszText[cch - 1] == L'\n' || pszText[cch - 1] == L' '))
        pszText[--cch] = 0;
    if (!cch)
        pszText[0] = 0;
}

typedef struct _CASE
{
    LPCWSTR pszFile;
    UINT ApiResult;
    DWORD ExitCode;
} CASE;

static void TestCase(const CASE *pCase)
{
    static const INSTALLUILEVEL Levels[] = { INSTALLUILEVEL_NONE, INSTALLUILEVEL_BASIC };
    WCHAR szPath[MAX_PATH], szMessage[512];
    WINDOW_SEARCH Search;
    UINT r, uWindows, i;
    DWORD dwExit;

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", g_Dir, pCase->pszFile);

    for (i = 0; i < _countof(Levels) && !g_ApiHung; i++)
    {
        MsiSetInternalUI(Levels[i], NULL);
        g_UiMessages = 0;
        MsiSetExternalUIW(ExternalUi, INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_WARNING | INSTALLLOGMODE_USER, NULL);
        r = CallApi(szPath, FALSE, &uWindows);
        if (!g_ApiHung)
            MsiSetExternalUIW(NULL, 0, NULL);
        ok(r == pCase->ApiResult, "%S, UI level %d: MsiInstallProductW returned %u, expected %u\n", pCase->pszFile, Levels[i], r, pCase->ApiResult);
        ok(uWindows == 0, "%S, UI level %d: MsiInstallProductW showed %u windows\n", pCase->pszFile, Levels[i], uWindows);
        ok(g_UiMessages == 0, "%S, UI level %d: %u external UI messages\n", pCase->pszFile, Levels[i], g_UiMessages);
    }

    if (!g_ApiHung)
    {
        MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
        r = CallApi(szPath, TRUE, &uWindows);
        ok(r == pCase->ApiResult, "%S: MsiOpenPackageW returned %u, expected %u\n", pCase->pszFile, r, pCase->ApiResult);
        ok(uWindows == 0, "%S: MsiOpenPackageW showed %u windows\n", pCase->pszFile, uWindows);
    }

    dwExit = RunMsiexec(szPath, L"/qn", &Search);
    ok(dwExit == pCase->ExitCode, "%S /qn: msiexec exit code %lu, expected %lu\n", pCase->pszFile, dwExit, pCase->ExitCode);
    ok(Search.Count == 0, "%S /qn: msiexec showed %u windows\n", pCase->pszFile, Search.Count);

    /* Windows shows a task dialog without readable text, ReactOS a message box */
    GetSystemMessage(pCase->ExitCode, szMessage, _countof(szMessage));
    dwExit = RunMsiexec(szPath, L"", &Search);
    ok(dwExit == pCase->ExitCode, "%S: msiexec exit code %lu, expected %lu\n", pCase->pszFile, dwExit, pCase->ExitCode);
    ok(Search.Count == 1, "%S: msiexec showed %u windows, expected 1\n", pCase->pszFile, Search.Count);
    if (Search.Count)
    {
        ok(!wcscmp(Search.szTitle, L"Windows Installer"), "%S: window title '%S'\n", pCase->pszFile, Search.szTitle);
        ok(!Search.szStatic[0] || !wcscmp(Search.szStatic, szMessage), "%S: message '%S', expected '%S'\n",
           pCase->pszFile, Search.szStatic, szMessage);
    }
}

START_TEST(OpenFailure)
{
    static const CASE Cases[] =
    {
        { L"nosuch.msi", ERROR_FILE_NOT_FOUND, ERROR_INSTALL_PACKAGE_OPEN_FAILED },
        { L"nodir\\nosuch.msi", ERROR_PATH_NOT_FOUND, ERROR_INSTALL_PACKAGE_OPEN_FAILED },
        { L"subdir", ERROR_INSTALL_PACKAGE_OPEN_FAILED, ERROR_INSTALL_PACKAGE_OPEN_FAILED },
        { L"locked.msi", ERROR_INSTALL_PACKAGE_OPEN_FAILED, ERROR_INSTALL_PACKAGE_OPEN_FAILED },
        { L"writing.msi", ERROR_INSTALL_PACKAGE_OPEN_FAILED, ERROR_INSTALL_PACKAGE_OPEN_FAILED },
        { L"text.msi", ERROR_INSTALL_PACKAGE_INVALID, ERROR_INSTALL_PACKAGE_INVALID },
        { L"empty.msi", ERROR_INSTALL_PACKAGE_INVALID, ERROR_INSTALL_PACKAGE_INVALID },
        { L"storage.msi", ERROR_INSTALL_PACKAGE_INVALID, ERROR_INSTALL_PACKAGE_INVALID },
    };
    static const struct
    {
        LPCWSTR pszUi;
        UINT Windows;
    } UiLevels[] =
    {
        { L"/qn+", 0 },
        { L"/qb-", 0 },
        { L"/passive", 0 },
        { L"/qb", 1 },
        { L"/qb!", 1 },
        { L"/qb+", 1 },
        { L"/qr", 1 },
        { L"/qf", 1 },
    };
    static const char Text[] = "This is not an installer package.\r\n";
    static const LPCWSTR Files[] = { L"text.msi", L"empty.msi", L"storage.msi", L"locked.msi", L"writing.msi" };
    WCHAR szTemp[MAX_PATH], szPath[MAX_PATH];
    WINDOW_SEARCH Search;
    HANDLE hLocked, hWriting;
    DWORD dwExit;
    UINT i;

    GetTempPathW(_countof(szTemp), szTemp);
    StringCchPrintfW(g_Dir, _countof(g_Dir), L"%smsiexec_openfail_%lu", szTemp, GetCurrentProcessId());
    if (!CreateDirectoryW(g_Dir, NULL))
    {
        skip("CreateDirectoryW(%S) failed: %lu\n", g_Dir, GetLastError());
        return;
    }
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\subdir", g_Dir);
    ok(CreateDirectoryW(szPath, NULL), "CreateDirectoryW(%S) failed: %lu\n", szPath, GetLastError());
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\text.msi", g_Dir);
    WriteFileBytes(szPath, Text, sizeof(Text) - 1);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\empty.msi", g_Dir);
    WriteFileBytes(szPath, NULL, 0);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\storage.msi", g_Dir);
    CreateStorageFile(szPath);

    /* No sharing at all, and a writer that shares reading only */
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\locked.msi", g_Dir);
    WriteFileBytes(szPath, Text, sizeof(Text) - 1);
    hLocked = CreateFileW(szPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(hLocked != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", szPath, GetLastError());
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\writing.msi", g_Dir);
    CreateStorageFile(szPath);
    hWriting = CreateFileW(szPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hWriting != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", szPath, GetLastError());

    for (i = 0; i < _countof(Cases); i++)
        TestCase(&Cases[i]);

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\nosuch.msi", g_Dir);
    for (i = 0; i < _countof(UiLevels); i++)
    {
        dwExit = RunMsiexec(szPath, UiLevels[i].pszUi, &Search);
        ok(dwExit == ERROR_INSTALL_PACKAGE_OPEN_FAILED, "%S: msiexec exit code %lu\n", UiLevels[i].pszUi, dwExit);
        ok(Search.Count == UiLevels[i].Windows, "%S: msiexec showed %u windows, expected %u\n",
           UiLevels[i].pszUi, Search.Count, UiLevels[i].Windows);
    }

    if (hLocked != INVALID_HANDLE_VALUE)
        CloseHandle(hLocked);
    if (hWriting != INVALID_HANDLE_VALUE)
        CloseHandle(hWriting);
    for (i = 0; i < _countof(Files); i++)
    {
        StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", g_Dir, Files[i]);
        DeleteFileW(szPath);
    }
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\subdir", g_Dir);
    RemoveDirectoryW(szPath);
    RemoveDirectoryW(g_Dir);
}
