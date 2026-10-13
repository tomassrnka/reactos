/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the default extension the classic Save As dialog adds
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windows.h>
#include <commdlg.h>

typedef struct
{
    PCWSTR Typed;           /* what the user types in the file name box */
    PCWSTR DefaultExtension;    /* lpstrDefExt, NULL for none */
    UINT FilterIndex;       /* 1: Text Documents (*.txt), 2: All Files (*.*) */
    PCWSTR Expected;        /* the file name part of the result */
} DEFEXT_CASE;

static const DEFEXT_CASE TestCases[] =
{
    /* A name that has an extension keeps it when no filter extension applies (CORE-20756) */
    { L"sjtest.bat", L"txt", 2, L"sjtest.bat" },
    { L"sjtest.TXT", L"txt", 2, L"sjtest.TXT" },
    { L"sjtest.bat", NULL, 1, L"sjtest.bat" },
    /* A name without an extension gets the default one */
    { L"sjtest", L"txt", 1, L"sjtest.txt" },
    { L"sjtest", L"txt", 2, L"sjtest.txt" },
    { L"sjtest", NULL, 2, L"sjtest" },
    { L"sjtest.txt", L"txt", 1, L"sjtest.txt" },
};

static
UINT_PTR
CALLBACK
HookProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (uMsg == WM_INITDIALOG)
        SetTimer(hwnd, 77, 500, NULL);
    if (uMsg == WM_TIMER && wParam == 77)
    {
        HWND hwndDialog = GetParent(hwnd);

        KillTimer(hwnd, 77);
        PostMessageW(hwndDialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                     (LPARAM)GetDlgItem(hwndDialog, IDOK));
    }
    if (uMsg == WM_NOTIFY && ((NMHDR *)lParam)->code == CDN_INITDONE)
        PostMessageW(GetParent(hwnd), WM_COMMAND, IDOK, 0);
    return 0;
}

static
BOOL
CALLBACK
CloseDialogProc(
    _In_ HWND hwnd,
    _In_ LPARAM lParam)
{
    WCHAR ClassName[16];

    if (GetWindowThreadProcessId(hwnd, NULL) == GetCurrentThreadId() &&
        GetClassNameW(hwnd, ClassName, _countof(ClassName)) &&
        !wcscmp(ClassName, L"#32770") && IsWindowVisible(hwnd))
    {
        PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
    }
    return TRUE;
}

static
VOID
CALLBACK
WatchdogProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ UINT_PTR idEvent,
    _In_ DWORD dwTime)
{
    EnumWindows(CloseDialogProc, 0);
}

START_TEST(SaveDialogDefExt)
{
    static const WCHAR Filter[] = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    WCHAR TempPath[MAX_PATH];
    UINT Index;

    GetTempPathW(_countof(TempPath), TempPath);
    for (Index = 0; Index < _countof(TestCases); Index++)
    {
        const DEFEXT_CASE *TestCase = &TestCases[Index];
        WCHAR FileName[MAX_PATH];
        OPENFILENAMEW OpenFileName = { sizeof(OpenFileName) };
        UINT_PTR Timer = SetTimer(NULL, 0, 10000, WatchdogProc);
        PWSTR NamePart;
        BOOL DidSucceed;

        wcscpy(FileName, TestCase->Typed);
        OpenFileName.lpstrFilter = Filter;
        OpenFileName.nFilterIndex = TestCase->FilterIndex;
        OpenFileName.lpstrFile = FileName;
        OpenFileName.nMaxFile = _countof(FileName);
        OpenFileName.lpstrInitialDir = TempPath;
        OpenFileName.lpstrDefExt = TestCase->DefaultExtension;
        OpenFileName.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        OpenFileName.lpfnHook = HookProc;
        DidSucceed = GetSaveFileNameW(&OpenFileName);
        KillTimer(NULL, Timer);

        ok(DidSucceed, "case %u: GetSaveFileNameW(%s) failed, error 0x%lx\n", Index,
           wine_dbgstr_w(TestCase->Typed), CommDlgExtendedError());
        if (!DidSucceed)
            continue;

        NamePart = wcsrchr(FileName, L'\\');
        ok(NamePart && !wcscmp(NamePart + 1, TestCase->Expected),
           "case %u: typed %s, default extension %s, filter %u: expected %s, got %s\n", Index,
           wine_dbgstr_w(TestCase->Typed), wine_dbgstr_w(TestCase->DefaultExtension),
           TestCase->FilterIndex, wine_dbgstr_w(TestCase->Expected), wine_dbgstr_w(FileName));
    }
}
