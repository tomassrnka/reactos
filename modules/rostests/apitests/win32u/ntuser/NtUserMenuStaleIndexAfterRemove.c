/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 (completeness) - bounding MENUINFO.iItem only at
 *              SET time is bypassable: IntRemoveMenuItem shrinks cItems (and
 *              frees rgItems when it hits 0) without touching Menu->iItem, so a
 *              previously-valid focused index becomes stale and >= cItems. It is
 *              then used as rgItems[iItem] by the select/exec/hide paths -> OOB.
 *              The fix clears Menu->iItem in IntRemoveMenuItem when it is no
 *              longer a valid position.
 *
 *              This test ARMS that exact sequence (focus an item, then remove
 *              items down to zero) and asserts the menu stays a valid, reusable
 *              object. Per the no-weaponized-PoC convention it does not drive an
 *              interactive menu-selection (which is what would dereference the
 *              stale index and bugcheck an unfixed build); the in-kernel clamp
 *              is what makes that later selection safe.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

/* Byte-for-byte mirror of win32ss/user/ntuser/menu.h tagROSMENUINFO. */
typedef struct _TEST_ROSMENUINFO
{
    DWORD cbSize;
    DWORD fMask;
    DWORD dwStyle;
    UINT cyMax;
    HBRUSH hbrBack;
    DWORD dwContextHelpID;
    ULONG_PTR dwMenuData;
    ULONG fFlags;
    UINT iItem;
    UINT cItems;
    WORD cxMenu;
    WORD cyMenu;
    ULONG cxTextAlign;
    PVOID spwndNotify;
    INT iTop;
    INT iMaxTop;
    DWORD dwArrowsOn:2;
    HMENU Self;
    HWND Wnd;
    BOOL TimeToHide;
} TEST_ROSMENUINFO;

static void AddItem(HMENU hMenu, UINT id, UINT pos)
{
    MENUITEMINFOW mii;
    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STRING;
    mii.wID = id;
    mii.dwTypeData = (LPWSTR)L"Item";
    InsertMenuItemW(hMenu, pos, TRUE, &mii);
}

START_TEST(NtUserMenuStaleIndexAfterRemove)
{
    HMENU hMenu;
    TEST_ROSMENUINFO rmi;
    MENUINFO mi;
    BOOL ret;

    hMenu = CreatePopupMenu();
    ok(hMenu != NULL, "CreatePopupMenu failed, error %lu\n", GetLastError());
    if (!hMenu)
        return;

    AddItem(hMenu, 1, 0);
    AddItem(hMenu, 2, 1);
    AddItem(hMenu, 3, 2);
    ok(GetMenuItemCount(hMenu) == 3, "unexpected count %d\n", GetMenuItemCount(hMenu));

    /* Focus the LAST item (valid position 2) via the extended SET path. */
    ZeroMemory(&rmi, sizeof(rmi));
    rmi.cbSize = sizeof(rmi);
    rmi.fMask = MIM_MENUDATA;
    rmi.dwMenuData = (ULONG_PTR)0x1234ABCD;
    rmi.iItem = 2;
    ret = NtUserThunkedMenuInfo(hMenu, (LPCMENUINFO)&rmi);
    ok(ret, "NtUserThunkedMenuInfo(iItem=2) failed, error %lu\n", GetLastError());

    /* Now remove items so the focused index (2) is left dangling past cItems,
     * and finally past an empty (freed) item array. A stale Menu->iItem here is
     * the OOB primitive the IntRemoveMenuItem clamp closes. */
    ok(RemoveMenu(hMenu, 2, MF_BYPOSITION), "RemoveMenu(2) failed, error %lu\n", GetLastError());
    ok(GetMenuItemCount(hMenu) == 2, "count after remove(2): %d\n", GetMenuItemCount(hMenu));

    ok(RemoveMenu(hMenu, 0, MF_BYPOSITION), "RemoveMenu(0) failed, error %lu\n", GetLastError());
    ok(RemoveMenu(hMenu, 0, MF_BYPOSITION), "RemoveMenu(0) #2 failed, error %lu\n", GetLastError());
    ok(GetMenuItemCount(hMenu) == 0, "menu not empty: %d\n", GetMenuItemCount(hMenu));

    /* The menu survived the arming sequence and is still a valid, usable object:
     * re-populate and round-trip a benign field. */
    AddItem(hMenu, 10, 0);
    ok(GetMenuItemCount(hMenu) == 1, "menu unusable after arming: %d\n",
       GetMenuItemCount(hMenu));
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_MENUDATA;
    ok(GetMenuInfo(hMenu, &mi), "GetMenuInfo failed after arming, error %lu\n", GetLastError());

    DestroyMenu(hMenu);
}
