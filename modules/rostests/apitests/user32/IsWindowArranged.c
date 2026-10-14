/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for IsWindowArranged (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64): a window that is not snapped to a side is not
 * arranged, whether it is normal, minimized or maximized, and a handle that is no window is an invalid parameter.
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_IsWindowArranged)(HWND);

#define PENDING_ERROR 0xdeadbeef

START_TEST(IsWindowArranged)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    PFN_IsWindowArranged pIsWindowArranged;
    HWND Window;
    HMENU Menu;
    BOOL Result;
    DWORD Error;

    pIsWindowArranged = (PFN_IsWindowArranged)GetProcAddress(User32, "IsWindowArranged");
    /* Windows 8.1 and older have none; ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pIsWindowArranged)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "IsWindowArranged is not exported\n");
        skip("IsWindowArranged is not exported\n");
        return;
    }

    Window = CreateWindowExW(0, L"STATIC", L"arranged", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowEx failed\n");
    if (!Window)
        return;

    SetLastError(PENDING_ERROR);
    Result = pIsWindowArranged(Window);
    Error = GetLastError();
    ok(Result == FALSE, "a normal window is arranged\n");
    ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);

    ShowWindow(Window, SW_MINIMIZE);
    ok(pIsWindowArranged(Window) == FALSE, "a minimized window is arranged\n");
    ShowWindow(Window, SW_MAXIMIZE);
    ok(pIsWindowArranged(Window) == FALSE, "a maximized window is arranged\n");
    ShowWindow(Window, SW_RESTORE);
    ok(pIsWindowArranged(Window) == FALSE, "a restored window is arranged\n");

    SetLastError(PENDING_ERROR);
    Result = pIsWindowArranged(GetDesktopWindow());
    Error = GetLastError();
    ok(Result == FALSE, "the desktop is arranged\n");
    ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);

    SetLastError(PENDING_ERROR);
    Result = pIsWindowArranged(NULL);
    Error = GetLastError();
    ok(Result == FALSE, "no window is arranged\n");
    ok(Error == ERROR_INVALID_PARAMETER, "no window: last error %lu\n", Error);

    /* a menu is a live handle of USER that is no window */
    Menu = CreateMenu();
    ok(Menu != NULL, "CreateMenu failed\n");
    SetLastError(PENDING_ERROR);
    Result = pIsWindowArranged((HWND)Menu);
    Error = GetLastError();
    ok(Result == FALSE, "a bad handle is arranged\n");
    ok(Error == ERROR_INVALID_PARAMETER, "bad handle: last error %lu\n", Error);

    DestroyMenu(Menu);
    DestroyWindow(Window);
}
