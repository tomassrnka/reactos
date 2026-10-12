/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test the program that the drive Tools page starts
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"
#include <commctrl.h>
#include <strsafe.h>
#include "shell32_apitest_sub.h"

#define DEFRAG_KEY   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\DefragPath"
#define APPPATHS_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"
#define IFEO_KEY     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\"

static WCHAR s_szDir[MAX_PATH];
static WCHAR s_szMarker[MAX_PATH];
static WCHAR s_szDrive[4];

static const LPCWSTR s_Programs[] = { L"Fx.exe", L"Fx Tool.exe", L"Only Tool.exe" };
static WCHAR s_szSysProgram[MAX_PATH];

/* Keys that the test creates and deletes; it does not run when one of them exists already */
static const LPCWSTR s_FixtureKeys[] =
{
    APPPATHS_KEY L"FxGood.exe", APPPATHS_KEY L"FxGood Tool.exe", APPPATHS_KEY L"FxMissing.exe",
    APPPATHS_KEY L"Only.exe", APPPATHS_KEY L"FxSys.exe", IFEO_KEY L"Only Tool.exe", IFEO_KEY L"Fx.exe",
    L"SOFTWARE\\Classes\\.fxtdoc", L"SOFTWARE\\Classes\\fxtdocfile",
};

/* |D| is the test directory, |C| the drive letter */
static void
ExpandValue(LPCWSTR pszIn, LPWSTR pszOut, SIZE_T cchOut)
{
    *pszOut = UNICODE_NULL;
    while (*pszIn)
    {
        if (pszIn[0] == L'|' && (pszIn[1] == L'D' || pszIn[1] == L'C') && pszIn[2] == L'|')
        {
            if (pszIn[1] == L'D')
                StringCchCatW(pszOut, cchOut, s_szDir);
            else
                StringCchCatNW(pszOut, cchOut, s_szDrive, 1);
            pszIn += 3;
        }
        else
        {
            StringCchCatNW(pszOut, cchOut, pszIn++, 1);
        }
    }
}

static BOOL CALLBACK
FindSheetProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid;
    WCHAR szClass[32];
    HWND *phwnd = (HWND *)lParam;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || hwnd == phwnd[1])
        return TRUE;
    GetClassNameW(hwnd, szClass, _countof(szClass));
    if (lstrcmpW(szClass, L"#32770") != 0)
        return TRUE;
    if (!phwnd[1] && !FindWindowExW(hwnd, NULL, WC_TABCONTROLW, NULL))
        return TRUE;
    phwnd[0] = hwnd;
    return FALSE;
}

/* Our visible dialog: the property sheet, or with hSheet given any other one (a message) */
static HWND
FindOurDialog(HWND hSheet)
{
    HWND ahwnd[2] = { NULL, hSheet };
    EnumWindows(FindSheetProc, (LPARAM)ahwnd);
    return ahwnd[0];
}

/* The Defragment button: ID 14001 on ReactOS, 14420 on Windows Server 2008 R2 and Windows 10 */
static BOOL CALLBACK
FindButtonProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[32];
    INT id = GetDlgCtrlID(hwnd);

    GetClassNameW(hwnd, szClass, _countof(szClass));
    if (lstrcmpiW(szClass, L"Button") != 0 ||
        (GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK) > BS_DEFPUSHBUTTON ||
        (id != 14001 && id != 14420))
    {
        return TRUE;
    }
    *(HWND *)lParam = hwnd;
    return FALSE;
}

static BOOL CALLBACK
CollectTextProc(HWND hwnd, LPARAM lParam)
{
    LPWSTR pszText = (LPWSTR)lParam;
    WCHAR szClass[32], szText[512];

    GetClassNameW(hwnd, szClass, _countof(szClass));
    if (lstrcmpiW(szClass, L"Static") == 0 && GetWindowTextW(hwnd, szText, _countof(szText)))
    {
        if (*pszText)
            StringCchCatW(pszText, 1024, L" / ");
        StringCchCatW(pszText, 1024, szText);
    }
    return TRUE;
}

static HWND
OpenToolsPage(HWND *phSheet)
{
    HWND hSheet = NULL, hTab, hPage;
    INT i, nCount;

    SHObjectProperties(NULL, SHOP_FILEPATH, s_szDrive, NULL);
    for (i = 0; i < 100 && !hSheet; ++i)
    {
        Sleep(100);
        hSheet = FindOurDialog(NULL);
    }
    *phSheet = hSheet;
    if (!hSheet)
        return NULL;

    /* Find the page by its button, so that the tab caption's language does not matter */
    hTab = FindWindowExW(hSheet, NULL, WC_TABCONTROLW, NULL);
    nCount = (INT)SendMessageW(hTab, TCM_GETITEMCOUNT, 0, 0);
    for (i = 0; i < nCount; ++i)
    {
        HWND hButton = NULL;
        SendMessageW(hSheet, PSM_SETCURSEL, i, 0);
        hPage = (HWND)SendMessageW(hSheet, PSM_GETCURRENTPAGEHWND, 0, 0);
        if (hPage)
            EnumChildWindows(hPage, FindButtonProc, (LPARAM)&hButton);
        if (hButton)
            return hButton;
    }
    return NULL;
}

static void
CloseDialog(HWND hwnd, BOOL bSheet)
{
    static const UINT ids[] = { IDOK, IDCANCEL, IDNO };
    for (UINT i = 0; i < 30 && IsWindow(hwnd); ++i)
    {
        if (i % 10 == 0)
        {
            if (bSheet)
                PostMessageW(hwnd, PSM_PRESSBUTTON, PSBTN_CANCEL, 0);
            else
                PostMessageW(hwnd, WM_COMMAND, ids[i / 10], 0);
        }
        Sleep(100);
    }
}

static void
ReadMarker(LPWSTR pszRan, SIZE_T cchRan)
{
    WCHAR szData[1024] = L"";
    DWORD cb = 0;
    HANDLE hFile = CreateFileW(s_szMarker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        ReadFile(hFile, szData, sizeof(szData) - sizeof(WCHAR), &cb, NULL);
        CloseHandle(hFile);
    }
    szData[cb / sizeof(WCHAR)] = UNICODE_NULL;
    StrTrimW(szData, L"\r\n ");
    StringCchCopyW(pszRan, cchRan, szData);
}

/* pszExpected: "program|arguments" for each program that must start, or "" when a message must say it failed */
static void
TestTool(INT line, LPCWSTR pszValue, LPCWSTR pszExpected)
{
    WCHAR szValue[MAX_PATH * 2], szExpected[MAX_PATH * 2], szRan[1024] = L"", szMessage[1024] = L"";
    HWND hSheet, hButton, hMessage = NULL;
    UINT i;

    ExpandValue(pszValue, szValue, _countof(szValue));
    ExpandValue(pszExpected, szExpected, _countof(szExpected));
    StrTrimW(szExpected, L" ");
    if (RegSetString(HKEY_LOCAL_MACHINE, DEFRAG_KEY, NULL, szValue) != ERROR_SUCCESS)
    {
        skip("Line %d: cannot set DefragPath\n", line);
        return;
    }
    DeleteFileW(s_szMarker);

    hButton = OpenToolsPage(&hSheet);
    ok(hButton != NULL, "Line %d: no Defragment button\n", line);
    if (!hButton)
    {
        if (hSheet)
            CloseDialog(hSheet, TRUE);
        return;
    }
    ok(IsWindowEnabled(hButton), "Line %d: the Defragment button is disabled\n", line);
    PostMessageW(GetParent(hButton), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hButton), BN_CLICKED), (LPARAM)hButton);

    for (i = 0; i < 100; ++i)
    {
        Sleep(100);
        if ((hMessage = FindOurDialog(hSheet)) != NULL)
            break;
        if (GetFileAttributesW(s_szMarker) != INVALID_FILE_ATTRIBUTES)
        {
            /* A message after a started program would be a second attempt */
            Sleep(1500);
            hMessage = FindOurDialog(hSheet);
            break;
        }
    }
    if (hMessage)
    {
        Sleep(300);
        EnumChildWindows(hMessage, CollectTextProc, (LPARAM)szMessage);
        CloseDialog(hMessage, FALSE);
        Sleep(1000);
    }
    ReadMarker(szRan, _countof(szRan));
    CloseDialog(hSheet, TRUE);

    trace("Line %d: '%S': started '%S', message '%S'\n", line, szValue, szRan, szMessage);
    ok(lstrcmpiW(szRan, szExpected) == 0, "Line %d: '%S' started '%S', expected '%S'\n",
       line, szValue, szRan, szExpected);
    ok((hMessage != NULL) == (szExpected[0] == UNICODE_NULL), "Line %d: '%S' message '%S'\n",
       line, szValue, szMessage);
}

static void
SetKeyValue(LPCWSTR pszRoot, LPCWSTR pszName, LPCWSTR pszValueName, LPCWSTR pszData)
{
    WCHAR szKey[MAX_PATH], szData[MAX_PATH * 2];
    StringCchPrintfW(szKey, _countof(szKey), L"%s%s", pszRoot, pszName);
    ExpandValue(pszData, szData, _countof(szData));
    ok_long(RegSetString(HKEY_LOCAL_MACHINE, szKey, pszValueName, szData), ERROR_SUCCESS);
}

static void
DeleteKey(LPCWSTR pszRoot, LPCWSTR pszName)
{
    WCHAR szKey[MAX_PATH];
    StringCchPrintfW(szKey, _countof(szKey), L"%s%s", pszRoot, pszName);
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, szKey);
}

static void
RunTests(void)
{
    WCHAR szDoc[MAX_PATH];

    TestTool(__LINE__, L"|D|\\Missing.exe", L"");
    TestTool(__LINE__, L"|D|\\Missing.exe %c:", L"");
    TestTool(__LINE__, L"\"|D|\\Missing Tool.exe\" %c:", L"");
    TestTool(__LINE__, L"|D|\\Fx.exe /x %c:", L"Fx.exe|/x |C|:");
    TestTool(__LINE__, L"|D|\\Fx", L"Fx.exe|");
    TestTool(__LINE__, L"\"|D|\\Fx Tool.exe\" %c:", L"Fx Tool.exe||C|:");

    /* As in CreateProcess, the shortest part that names an existing file is the program */
    TestTool(__LINE__, L"|D|\\Fx Tool.exe", L"Fx.exe|Tool.exe");
    TestTool(__LINE__, L"|D|\\Fx Tool.exe %c:", L"Fx.exe|Tool.exe |C|:");
    TestTool(__LINE__, L"|D|\\Only Tool.exe %c:", L"Only Tool.exe||C|:");

    /* A document starts through its association */
    SetKeyValue(L"SOFTWARE\\Classes\\", L".fxtdoc", NULL, L"fxtdocfile");
    SetKeyValue(L"SOFTWARE\\Classes\\", L"fxtdocfile\\shell\\open\\command", NULL, L"\"|D|\\Fx.exe\" \"%1\" %*");
    StringCchPrintfW(szDoc, _countof(szDoc), L"%s\\doc.fxtdoc", s_szDir);
    CloseHandle(CreateFileW(szDoc, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    TestTool(__LINE__, L"|D|\\doc.fxtdoc %c:", L"Fx.exe|\"|D|\\doc.fxtdoc\" |C|:");
    DeleteKey(L"SOFTWARE\\Classes\\", L".fxtdoc");
    DeleteKey(L"SOFTWARE\\Classes\\", L"fxtdocfile");
    DeleteFileW(szDoc);

    /* An App Paths name without spaces starts its target; with spaces it is not a program name */
    SetKeyValue(APPPATHS_KEY, L"FxGood.exe", NULL, L"|D|\\Fx.exe");
    TestTool(__LINE__, L"FxGood.exe", L"Fx.exe|");
    TestTool(__LINE__, L"FxGood", L"Fx.exe|");
    DeleteKey(APPPATHS_KEY, L"FxGood.exe");
    SetKeyValue(APPPATHS_KEY, L"FxGood Tool.exe", NULL, L"|D|\\Fx.exe");
    TestTool(__LINE__, L"FxGood Tool.exe", L"");
    DeleteKey(APPPATHS_KEY, L"FxGood Tool.exe");

    /* A program found in the Windows directory, quoted or not */
    if (s_szSysProgram[0])
    {
        TestTool(__LINE__, L"FxSys.exe /z %c:", L"FxSys.exe|/z |C|:");
        TestTool(__LINE__, L"\"FxSys.exe\" %c:", L"FxSys.exe||C|:");
    }

    /* A program that fails to start is reported; nothing else starts in its place */
    SetKeyValue(APPPATHS_KEY, L"FxMissing.exe", NULL, L"|D|\\NotThere.exe");
    TestTool(__LINE__, L"FxMissing.exe", L"");
    DeleteKey(APPPATHS_KEY, L"FxMissing.exe");
    SetKeyValue(IFEO_KEY, L"Only Tool.exe", L"Debugger", L"|D|\\NoDebugger.exe");
    SetKeyValue(APPPATHS_KEY, L"Only.exe", NULL, L"|D|\\Fx.exe");
    TestTool(__LINE__, L"|D|\\Only Tool.exe", L"");
    DeleteKey(APPPATHS_KEY, L"Only.exe");
    DeleteKey(IFEO_KEY, L"Only Tool.exe");
    if (s_szSysProgram[0])
    {
        /* For a bare name, App Paths comes before the search path */
        SetKeyValue(APPPATHS_KEY, L"FxSys.exe", NULL, L"|D|\\Fx.exe");
        TestTool(__LINE__, L"FxSys.exe %c:", L"Fx.exe||C|:");
        TestTool(__LINE__, L"\"FxSys.exe\" %c:", L"Fx.exe||C|:");
        SetKeyValue(IFEO_KEY, L"Fx.exe", L"Debugger", L"|D|\\NoDebugger.exe");
        TestTool(__LINE__, L"FxSys.exe %c:", L"");
        DeleteKey(IFEO_KEY, L"Fx.exe");
        SetKeyValue(APPPATHS_KEY, L"FxSys.exe", NULL, L"|D|\\NotThere.exe");
        TestTool(__LINE__, L"FxSys.exe %c:", L"");
        TestTool(__LINE__, L"\"FxSys.exe\" %c:", L"");
        DeleteKey(APPPATHS_KEY, L"FxSys.exe");
    }
}

START_TEST(DriveTools)
{
    WCHAR szSub[MAX_PATH], szPath[MAX_PATH];
    LPWSTR pszOldValue = NULL;
    DWORD cbOld = 0, dwType = REG_NONE;
    BOOL bKeyExisted = FALSE;
    LSTATUS lError;
    HKEY hKey;
    UINT i;

    if (!FindSubProgram(szSub, _countof(szSub)))
    {
        skip("shell32_apitest_sub.exe not found\n");
        return;
    }
    for (i = 0; i < _countof(s_FixtureKeys); ++i)
    {
        lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, s_FixtureKeys[i], 0, KEY_READ, &hKey);
        if (lError == ERROR_SUCCESS)
            RegCloseKey(hKey);
        if (lError != ERROR_FILE_NOT_FOUND)
        {
            skip("%S exists or cannot be checked (%ld)\n", s_FixtureKeys[i], lError);
            return;
        }
    }

    /* Keep the whole DefragPath state (no key, no default value, or the value); stop on any doubt */
    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, DEFRAG_KEY, 0, KEY_READ, &hKey);
    if (lError == ERROR_SUCCESS)
    {
        bKeyExisted = TRUE;
        lError = RegQueryValueExW(hKey, NULL, NULL, &dwType, NULL, &cbOld);
        if (lError == ERROR_SUCCESS)
        {
            pszOldValue = (LPWSTR)calloc(1, cbOld + sizeof(WCHAR));
            lError = pszOldValue ? RegQueryValueExW(hKey, NULL, NULL, &dwType, (LPBYTE)pszOldValue, &cbOld)
                                 : ERROR_OUTOFMEMORY;
        }
        else if (lError == ERROR_FILE_NOT_FOUND)
        {
            lError = ERROR_SUCCESS;
        }
        RegCloseKey(hKey);
    }
    else if (lError == ERROR_FILE_NOT_FOUND)
    {
        lError = ERROR_SUCCESS;
    }
    if (lError != ERROR_SUCCESS)
    {
        skip("Cannot save DefragPath (%ld)\n", lError);
        free(pszOldValue);
        return;
    }

    /* A fresh directory, so that every file in it belongs to the test */
    GetWindowsDirectoryW(szPath, _countof(szPath));
    StringCchPrintfW(s_szDrive, _countof(s_szDrive), L"%c:\\", szPath[0]);
    StringCchPrintfW(s_szDir, _countof(s_szDir), L"%c:\\shell32_apitest_drivetools_%lu", szPath[0],
                     GetCurrentProcessId());
    StringCchPrintfW(s_szMarker, _countof(s_szMarker), L"%s\\marker.txt", s_szDir);
    if (!CreateDirectoryW(s_szDir, NULL))
    {
        skip("Cannot create %S (%lu)\n", s_szDir, GetLastError());
        free(pszOldValue);
        return;
    }
    for (i = 0; i < _countof(s_Programs); ++i)
    {
        StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", s_szDir, s_Programs[i]);
        ok(CopyFileW(szSub, szPath, TRUE), "Cannot copy %S (%lu)\n", szPath, GetLastError());
    }

    /* Windows' Tools page finds a bare name in the Windows directory, not on this process's PATH */
    GetWindowsDirectoryW(s_szSysProgram, _countof(s_szSysProgram));
    StringCchCatW(s_szSysProgram, _countof(s_szSysProgram), L"\\FxSys.exe");
    if (!CopyFileW(szSub, s_szSysProgram, TRUE))
    {
        skip("Cannot create %S (%lu)\n", s_szSysProgram, GetLastError());
        s_szSysProgram[0] = UNICODE_NULL;
    }
    SetEnvironmentVariableW(DRIVETOOLS_MARKER_VARIABLE, s_szMarker);

    RunTests();

    SetEnvironmentVariableW(DRIVETOOLS_MARKER_VARIABLE, NULL);
    if (s_szSysProgram[0])
        ok(DeleteFileW(s_szSysProgram), "Cannot delete %S (%lu)\n", s_szSysProgram, GetLastError());

    if (pszOldValue)
        lError = SHSetValueW(HKEY_LOCAL_MACHINE, DEFRAG_KEY, NULL, dwType, pszOldValue, cbOld);
    else if (bKeyExisted)
        lError = SHDeleteValueW(HKEY_LOCAL_MACHINE, DEFRAG_KEY, NULL);
    else
        lError = SHDeleteKeyW(HKEY_LOCAL_MACHINE, DEFRAG_KEY);
    ok(lError == ERROR_SUCCESS, "Cannot restore DefragPath (%ld)\n", lError);
    free(pszOldValue);

    for (i = 0; i < _countof(s_Programs); ++i)
    {
        StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", s_szDir, s_Programs[i]);
        DeleteFileW(szPath);
    }
    DeleteFileW(s_szMarker);
    RemoveDirectoryW(s_szDir);
}
