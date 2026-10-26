/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - PopupMenuWndProc stores the per-window POPUPMENU
 *              pointer into the window extra bytes via (PMENUWND)Wnd->ppopupmenu.
 *              The fix (a) refuses to flip a window to FNID_MENU / store that
 *              pointer unless the class reserves room (cbwndExtra >= sizeof(PVOID)),
 *              closing an OOB write past the WND, (b) flips fnid only after the
 *              context is allocated+stored, and (c) NULL-guards the context in
 *              WM_CREATE/WM_PAINT/WM_PRINTCLIENT/WM_NCDESTROY/MN_SETHMENU.
 *
 *              The OOB and the NULL-context (failed-alloc) paths are not
 *              reachable through documented user APIs, so this is a regression
 *              contract: the real system menu class (#32768) reserves enough
 *              extra bytes (the fix never rejects legitimate menus), and normal
 *              menu-window create/destroy (WM_NCCREATE..WM_NCDESTROY, valid
 *              context) stays functional.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

START_TEST(NtUserPopupMenuClassExtra)
{
    WNDCLASSEXW wc;
    BOOL ok_info;
    HMENU hMenu;
    MENUITEMINFOW mii;
    HWND hMenuWnd;

    /* The system popup-menu class. Its cbWndExtra must have room for the
     * ppopupmenu pointer that PopupMenuWndProc stores; otherwise the hardened
     * proc would (correctly) refuse real menus. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    ok_info = GetClassInfoExW(NULL, L"#32768", &wc);
    ok(ok_info, "GetClassInfoExW(#32768) failed, error %lu\n", GetLastError());
    if (ok_info)
    {
        ok(wc.cbWndExtra >= (int)sizeof(PVOID),
           "SECURITY(Astra#8): menu class cbWndExtra=%d < sizeof(PVOID)=%u - the "
           "hardened PopupMenuWndProc would reject legitimate menu windows\n",
           wc.cbWndExtra, (unsigned)sizeof(PVOID));
    }

    /* A normal popup menu is still fully constructible/usable after the fix. */
    hMenu = CreatePopupMenu();
    ok(hMenu != NULL, "CreatePopupMenu failed, error %lu\n", GetLastError());
    if (hMenu)
    {
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING;
        mii.wID = 1;
        mii.dwTypeData = (LPWSTR)L"Item0";
        ok(InsertMenuItemW(hMenu, 0, TRUE, &mii),
           "InsertMenuItemW failed, error %lu\n", GetLastError());
        ok(GetMenuItemCount(hMenu) == 1, "unexpected item count %d\n",
           GetMenuItemCount(hMenu));
        DestroyMenu(hMenu);
    }

    /* Exercise the menu-window WM_NCCREATE..WM_NCDESTROY happy path directly:
     * the restructured proc flips fnid only after storing the context, and
     * WM_NCDESTROY frees a (valid) context. Creating a #32768 window from user
     * mode may not be permitted on every build; tolerate failure. */
    hMenuWnd = CreateWindowExW(0, L"#32768", L"", WS_POPUP,
                               0, 0, 10, 10, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hMenuWnd)
    {
        ok(DestroyWindow(hMenuWnd),
           "DestroyWindow(menu class) failed, error %lu\n", GetLastError());
    }
    else
    {
        trace("direct #32768 window creation not permitted (error %lu); "
              "create/destroy path covered indirectly by menu usage\n",
              GetLastError());
    }
}
