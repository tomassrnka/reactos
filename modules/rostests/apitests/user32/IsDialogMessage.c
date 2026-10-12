/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for IsDialogMessageW keyboard mnemonic search
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define IDC_LABEL   100
#define IDC_EDIT    101
#define IDC_BUTTON  102
#define IDC_PAGE    103
#define IDC_HIDDEN  104

/* Child exit code: CHILD_DONE plus the result bits and the WM_CHAR count */
#define CHILD_DONE          0x1000
#define CHILD_RESULT        0x0001
#define CHILD_FOCUS_KEPT    0x0002
#define CHILD_FOCUS_EDIT    0x0004
#define CHILD_SUBCLASSED    0x0008
#define CHILD_CHARS_SHIFT   4
#define CHILD_CHARS_MASK    0x00F0

static WNDPROC g_OldButtonProc;
static LONG g_CharCount;

static INT_PTR CALLBACK
EmptyDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return (uMsg == WM_INITDIALOG);
}

static LRESULT CALLBACK
HiddenButtonProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_CHAR)
        g_CharCount++;
    return CallWindowProcW(g_OldButtonProc, hwnd, uMsg, wParam, lParam);
}

static DWORD
RunChild(WCHAR Char)
{
    struct
    {
        DLGTEMPLATE tmpl;
        WORD menu, wndclass, title;
    } dlg;
    HWND hDlg, hPage, hEdit, hHidden, hFocusBefore, hFocus;
    MSG msg;
    DWORD dwCode = CHILD_DONE;

    ZeroMemory(&dlg, sizeof(dlg));
    dlg.tmpl.style = WS_POPUP | WS_CAPTION | WS_VISIBLE;
    dlg.tmpl.cx = 200;
    dlg.tmpl.cy = 100;
    hDlg = CreateDialogIndirectParamW(GetModuleHandleW(NULL), &dlg.tmpl, NULL, EmptyDlgProc, 0);
    if (!hDlg)
        return 1;

    CreateWindowExW(0, L"STATIC", L"&Name:", WS_CHILD | WS_VISIBLE,
                    5, 5, 50, 20, hDlg, (HMENU)IDC_LABEL, NULL, NULL);
    hEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER,
                            60, 5, 100, 20, hDlg, (HMENU)IDC_EDIT, NULL, NULL);
    CreateWindowExW(0, L"BUTTON", L"&Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    5, 30, 60, 20, hDlg, (HMENU)IDC_BUTTON, NULL, NULL);

    /* A hidden page, like the inactive page of a tab control */
    hPage = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                            5, 55, 150, 30, hDlg, (HMENU)IDC_PAGE, NULL, NULL);
    hHidden = CreateWindowExW(0, L"BUTTON", L"Hidden", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, 60, 20, hPage, (HMENU)IDC_HIDDEN, NULL, NULL);
    if (!hEdit || !hHidden)
        return 2;
    g_OldButtonProc = (WNDPROC)SetWindowLongPtrW(hHidden, GWLP_WNDPROC, (LONG_PTR)HiddenButtonProc);

    /* The message comes from a control whose parent is hidden */
    ZeroMemory(&msg, sizeof(msg));
    msg.hwnd = hHidden;
    msg.message = WM_CHAR;
    msg.wParam = Char;
    msg.lParam = 1;
    hFocusBefore = GetFocus();
    if (IsDialogMessageW(hDlg, &msg))
        dwCode |= CHILD_RESULT;
    hFocus = GetFocus();

    if (hFocus == hFocusBefore)
        dwCode |= CHILD_FOCUS_KEPT;
    if (hFocus == hEdit)
        dwCode |= CHILD_FOCUS_EDIT;
    dwCode |= (min(g_CharCount, 15) << CHILD_CHARS_SHIFT);

    /* The counter sees a character sent directly */
    g_CharCount = 0;
    SendMessageW(hHidden, WM_CHAR, Char, 1);
    if (g_CharCount == 1)
        dwCode |= CHILD_SUBCLASSED;

    DestroyWindow(hDlg);
    return dwCode;
}

static BOOL
RunAccelTest(char Char, PDWORD pdwCode)
{
    char szPath[MAX_PATH], szCmd[MAX_PATH + 64];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD dwWait;
    BOOL bOk;

    GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath));
    sprintf(szCmd, "\"%s\" IsDialogMessage child %c", szPath, Char);
    bOk = CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    ok(bOk, "CreateProcessA failed: %lu\n", GetLastError());
    if (!bOk)
        return FALSE;
    CloseHandle(pi.hThread);

    dwWait = WaitForSingleObject(pi.hProcess, 10000);
    ok(dwWait == WAIT_OBJECT_0, "IsDialogMessageW did not return for '%c' (wait %lu)\n", Char, dwWait);
    if (dwWait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 0xDEAD);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        return FALSE;
    }
    GetExitCodeProcess(pi.hProcess, pdwCode);
    CloseHandle(pi.hProcess);
    ok((*pdwCode & ~(CHILD_RESULT | CHILD_FOCUS_KEPT | CHILD_FOCUS_EDIT | CHILD_SUBCLASSED | CHILD_CHARS_MASK)) == CHILD_DONE,
       "Child for '%c' failed: 0x%lx\n", Char, *pdwCode);
    return ((*pdwCode & ~(CHILD_RESULT | CHILD_FOCUS_KEPT | CHILD_FOCUS_EDIT | CHILD_SUBCLASSED | CHILD_CHARS_MASK)) == CHILD_DONE);
}

START_TEST(IsDialogMessage)
{
    char **argv;
    int argc;
    DWORD dwCode;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "child"))
        ExitProcess(RunChild((WCHAR)(UCHAR)argv[3][0]));

    /* No control has this mnemonic: the focus stays, and Windows drops the character */
    if (RunAccelTest('z', &dwCode))
    {
        ok(dwCode & CHILD_RESULT, "Expected TRUE, got 0x%lx\n", dwCode);
        ok(dwCode & CHILD_FOCUS_KEPT, "Expected the focus to stay, got 0x%lx\n", dwCode);
        ok(dwCode & CHILD_SUBCLASSED, "Expected the counter to work, got 0x%lx\n", dwCode);
        todo_ros ok(((dwCode & CHILD_CHARS_MASK) >> CHILD_CHARS_SHIFT) == 0, "Expected no WM_CHAR, got 0x%lx\n", dwCode);
    }

    /* A visible label has this mnemonic: the focus moves to the control after it */
    if (RunAccelTest('n', &dwCode))
    {
        ok(dwCode & CHILD_RESULT, "Expected TRUE, got 0x%lx\n", dwCode);
        ok(dwCode & CHILD_FOCUS_EDIT, "Expected the focus on the edit, got 0x%lx\n", dwCode);
        ok(dwCode & CHILD_SUBCLASSED, "Expected the counter to work, got 0x%lx\n", dwCode);
        ok(((dwCode & CHILD_CHARS_MASK) >> CHILD_CHARS_SHIFT) == 0, "Expected no WM_CHAR, got 0x%lx\n", dwCode);
    }
}
