/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for in-context WinEvent hooks of a DLL above 4 GB
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef LONG (WINAPI *FN_WevGetCount)(VOID);

static VOID
TestInContextHook(VOID)
{
    HMODULE hDll1, hDll2;
    WINEVENTPROC pProc1, pProc2;
    FN_WevGetCount pGetCount1, pGetCount2;
    HWINEVENTHOOK hHook;
    HWND hWnd;
    LONG Count1, Count2;
    BOOL Ret;

    hDll1 = LoadLibraryW(L"user32_wevhook1.dll");
    hDll2 = LoadLibraryW(L"user32_wevhook2.dll");
    ok(hDll1 != NULL && hDll2 != NULL, "LoadLibrary: %p, %p, error %lu\n", hDll1, hDll2, GetLastError());
    if (!hDll1 || !hDll2)
    {
        skip("The hook DLLs are missing\n");
        return;
    }
    pProc1 = (WINEVENTPROC)GetProcAddress(hDll1, "WevHookProc");
    pProc2 = (WINEVENTPROC)GetProcAddress(hDll2, "WevHookProc");
    pGetCount1 = (FN_WevGetCount)GetProcAddress(hDll1, "WevGetCount");
    pGetCount2 = (FN_WevGetCount)GetProcAddress(hDll2, "WevGetCount");
    ok(pProc1 && pProc2 && pGetCount1 && pGetCount2, "Exports missing\n");
    if (!pProc1 || !pProc2 || !pGetCount1 || !pGetCount2)
        return;
    trace("DLL 1 at %p, DLL 2 at %p\n", hDll1, hDll2);

    /* Both copies must have the procedure at the same offset, or the aliasing below proves nothing */
    ok((ULONG_PTR)pProc1 - (ULONG_PTR)hDll1 == (ULONG_PTR)pProc2 - (ULONG_PTR)hDll2,
       "The procedure is at 0x%lx in DLL 1 and at 0x%lx in DLL 2\n",
       (ULONG_PTR)pProc1 - (ULONG_PTR)hDll1, (ULONG_PTR)pProc2 - (ULONG_PTR)hDll2);

    hWnd = CreateWindowExW(0, L"STATIC", L"WinEventHook", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ok(hWnd != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    if (!hWnd)
        return;

    hHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, hDll1, pProc1,
                            GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
    ok(hHook != NULL, "SetWinEventHook failed: %lu\n", GetLastError());
    if (hHook)
    {
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hWnd, OBJID_WINDOW, CHILDID_SELF);
        Count1 = pGetCount1();
        Count2 = pGetCount2();
        ok(Count1 == 1, "The hook procedure of DLL 1 ran %ld times\n", Count1);
        ok(Count2 == 0, "The hook procedure of DLL 2 ran %ld times\n", Count2);
        Ret = UnhookWinEvent(hHook);
        ok(Ret, "UnhookWinEvent failed: %lu\n", GetLastError());
    }
    DestroyWindow(hWnd);
    FreeLibrary(hDll2);
    FreeLibrary(hDll1);
}

START_TEST(WinEventHook)
{
    TestInContextHook();
}
