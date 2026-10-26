/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - co_UserCallNextHookEx forwarded several local
 *              structs (MSLLHOOKSTRUCT/KBDLLHOOKSTRUCT/MSG/RECTL/CBTACTIVATE-
 *              STRUCT) to the next hook; when the caller passed lParam==0 the
 *              input copy was skipped and the UNINITIALISED local was handed to
 *              the hook -> kernel-stack info-leak. The fix zero-initialises
 *              those locals.
 *
 *              The leaked bytes are not deterministically observable from user
 *              mode, so this is a functional regression contract: the hook
 *              dispatch path still works end-to-end after the zero-init change
 *              (install hook, fire CBT events, hook is called, no crash).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

static LONG g_cbtCalls = 0;

static LRESULT CALLBACK
CbtProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0)
        InterlockedIncrement(&g_cbtCalls);
    return CallNextHookEx(NULL, code, wParam, lParam);
}

START_TEST(NtUserCallNextHookZero)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    HHOOK hHook;
    HWND hWnd;

    g_cbtCalls = 0;

    hHook = SetWindowsHookExW(WH_CBT, CbtProc, NULL, GetCurrentThreadId());
    ok(hHook != NULL, "SetWindowsHookExW(WH_CBT) failed, error %lu\n", GetLastError());
    if (!hHook)
        return;

    /* Fire CBT hook events (HCBT_CREATEWND / HCBT_ACTIVATE / HCBT_MOVESIZE etc.)
     * which traverse the hardened co_UserCallNextHookEx dispatch. */
    hWnd = CreateWindowW(L"BUTTON", L"Astra8Hook", WS_OVERLAPPEDWINDOW,
                         10, 10, 100, 60, NULL, NULL, hinst, NULL);
    ok(hWnd != NULL, "CreateWindowW failed, error %lu\n", GetLastError());
    if (hWnd)
    {
        ShowWindow(hWnd, SW_SHOWNA);
        MoveWindow(hWnd, 20, 20, 120, 70, TRUE);
        DestroyWindow(hWnd);
    }

    ok(g_cbtCalls > 0,
       "WH_CBT hook was never invoked (%ld) - hook dispatch regressed\n",
       g_cbtCalls);

    ok(UnhookWindowsHookEx(hHook),
       "UnhookWindowsHookEx failed, error %lu\n", GetLastError());
}
