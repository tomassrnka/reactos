/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - NtUserGetMenuItemRect must ProbeForWrite the
 *              caller RECT before copying. Previously the RtlCopyMemory was
 *              guarded only by SEH, so a caller passing a valid *kernel*
 *              address faulted nothing and got an arbitrary kernel write.
 *              These contract tests assert the write path rejects a
 *              non-user-writable destination cleanly (FALSE, no bugcheck) and
 *              that a legitimate RECT destination still succeeds.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

START_TEST(NtUserGetMenuItemRectProbe)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    HWND hWnd;
    HMENU hMenu;
    MENUITEMINFOW mii;
    RECT rc;
    PVOID faultPage;
    BOOL ret;

    hWnd = CreateWindowW(L"BUTTON", L"Astra8", WS_OVERLAPPEDWINDOW,
                         0, 0, 80, 40, NULL, NULL, hinst, NULL);
    ok(hWnd != NULL, "CreateWindowW failed, error %lu\n", GetLastError());

    hMenu = CreateMenu();
    ok(hMenu != NULL, "CreateMenu failed, error %lu\n", GetLastError());
    if (!hMenu || !hWnd)
    {
        if (hMenu) DestroyMenu(hMenu);
        if (hWnd) DestroyWindow(hWnd);
        return;
    }

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STRING;
    mii.wID = 1;
    mii.dwTypeData = (LPWSTR)L"Item0";
    ok(InsertMenuItemW(hMenu, 0, TRUE, &mii),
       "InsertMenuItemW failed, error %lu\n", GetLastError());
    ok(SetMenu(hWnd, hMenu), "SetMenu failed, error %lu\n", GetLastError());

    /* --- Positive: a valid user RECT destination works. --- */
    ZeroMemory(&rc, sizeof(rc));
    ret = NtUserGetMenuItemRect(hWnd, hMenu, 0, &rc);
    ok(ret == TRUE, "NtUserGetMenuItemRect(valid) failed, error %lu\n", GetLastError());

    /* --- Negative: a misaligned/unmapped destination is rejected cleanly. --- */
    ret = NtUserGetMenuItemRect(hWnd, hMenu, 0, (LPRECT)(ULONG_PTR)0x3);
    ok(ret == FALSE,
       "SECURITY(Astra#8): NtUserGetMenuItemRect accepted a bogus lprcItem "
       "(ProbeForWrite missing?)\n");

    /* --- Negative: a PAGE_NOACCESS destination is rejected cleanly (no write,
     *     no bugcheck). ProbeForWrite turns this into a clean failure. --- */
    faultPage = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    ok(faultPage != NULL, "VirtualAlloc failed, error %lu\n", GetLastError());
    if (faultPage)
    {
        ret = NtUserGetMenuItemRect(hWnd, hMenu, 0, (LPRECT)faultPage);
        ok(ret == FALSE,
           "SECURITY(Astra#8): NtUserGetMenuItemRect accepted a no-access "
           "lprcItem destination\n");
        VirtualFree(faultPage, 0, MEM_RELEASE);
    }

    /* The menu/window survived the rejected calls and remain valid. */
    ZeroMemory(&rc, sizeof(rc));
    ok(NtUserGetMenuItemRect(hWnd, hMenu, 0, &rc) == TRUE,
       "NtUserGetMenuItemRect(valid, post-reject) failed, error %lu\n", GetLastError());

    DestroyMenu(hMenu);
    DestroyWindow(hWnd);
}
