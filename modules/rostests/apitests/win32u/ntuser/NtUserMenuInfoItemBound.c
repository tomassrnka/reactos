/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - the cbSize-extended NtUserThunkedMenuInfo SET
 *              path copied MENUINFO.iItem into Menu->iItem with no bound check.
 *              iItem is later used directly as rgItems[iItem], so an
 *              out-of-range value is an OOB read/write (EoP). The fix accepts
 *              the NO_SELECTED_ITEM sentinel, otherwise requires iItem < cItems
 *              (clearing the selection for anything else).
 *
 *              The stored value is not directly observable from user mode, so
 *              these are surgical/stability contract tests: an out-of-range
 *              extended SET must be accepted without corrupting the menu, and a
 *              valid extended SET must keep working. (The bound itself is
 *              enforced in-kernel by IntSetMenuInfo and is exercised, not
 *              triggered, here - following the no-weaponized-PoC convention.)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

/* Byte-for-byte mirror of win32ss/user/ntuser/menu.h tagROSMENUINFO so that
 * cbSize == sizeof(TEST_ROSMENUINFO) passes the kernel's upper-bound check and
 * drives the extended SET path the way user32 never does. */
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

#define TEST_MENUDATA ((ULONG_PTR)0x1234ABCD)

static HMENU
MakeOneItemMenu(void)
{
    HMENU hMenu;
    MENUITEMINFOW mii;

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return NULL;

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STRING;
    mii.wID = 1;
    mii.dwTypeData = (LPWSTR)L"Item0";
    InsertMenuItemW(hMenu, 0, TRUE, &mii);
    return hMenu;
}

/* Focused-item bookkeeping (menu.c IntRemoveMenuItem / IntDestroyMenu).
 * iItem is observable from user mode through GetMenuBarInfo().fFocused on a
 * window menu bar, and can be seeded through the extended SET path. */
static BOOL
IsItemFocused(HWND hwnd, int id)
{
    MENUBARINFO mbi;

    ZeroMemory(&mbi, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, id, &mbi))
        return FALSE;
    return mbi.fFocused;
}

static void
SetFocusedPos(HMENU hMenu, UINT iItem)
{
    TEST_ROSMENUINFO rmi;

    ZeroMemory(&rmi, sizeof(rmi));
    rmi.cbSize = sizeof(rmi);
    rmi.iItem = iItem;
    ok(NtUserThunkedMenuInfo(hMenu, (LPCMENUINFO)&rmi),
       "NtUserThunkedMenuInfo(iItem=%u) failed, error %lu\n", iItem, GetLastError());
}

static void
AddItems(HMENU hMenu, UINT first, UINT count)
{
    UINT i;

    for (i = 0; i < count; i++)
        ok(AppendMenuW(hMenu, MF_STRING, first + i, L"Item"),
           "AppendMenuW failed, error %lu\n", GetLastError());
}

static void
TestFocusedItemTracking(void)
{
    HMENU hMenu = CreateMenu();
    HWND hwnd;

    ok(hMenu != NULL, "CreateMenu failed, error %lu\n", GetLastError());
    if (!hMenu)
        return;
    AddItems(hMenu, 1, 3); /* ids 1,2,3 at positions 0,1,2 */

    hwnd = CreateWindowExW(0, L"STATIC", L"menutest", WS_POPUP | WS_CAPTION,
                           0, 0, 100, 100, NULL, hMenu, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
    {
        DestroyMenu(hMenu);
        return;
    }

    /* Remove an item BEFORE the focused one: focus must keep naming id 3 */
    SetFocusedPos(hMenu, 2);
    ok(IsItemFocused(hwnd, 3), "id 3 should be focused before remove\n");
    ok(RemoveMenu(hMenu, 0, MF_BYPOSITION), "RemoveMenu failed\n");
    ok(IsItemFocused(hwnd, 2), "focus did not follow id 3 after remove-before-focus\n");
    ok(!IsItemFocused(hwnd, 1), "pos 0 wrongly focused after remove-before-focus\n");

    /* Remove an item AFTER the focused one: focus unchanged */
    SetFocusedPos(hMenu, 0);
    ok(RemoveMenu(hMenu, 1, MF_BYPOSITION), "RemoveMenu(2) failed\n");
    ok(IsItemFocused(hwnd, 1), "focus moved by remove-after-focus\n");

    /* Empty the menu, rebuild: no stale focus may survive */
    SetFocusedPos(hMenu, 0);
    ok(RemoveMenu(hMenu, 0, MF_BYPOSITION), "RemoveMenu(3) failed\n");
    ok(GetMenuItemCount(hMenu) == 0, "menu not empty\n");
    AddItems(hMenu, 10, 3);
    ok(!IsItemFocused(hwnd, 1) && !IsItemFocused(hwnd, 2) && !IsItemFocused(hwnd, 3),
       "stale focus survived empty-then-rebuild\n");

    DestroyWindow(hwnd);
    DestroyMenu(hMenu);
}

START_TEST(NtUserMenuInfoItemBound)
{
    HMENU hMenu;
    TEST_ROSMENUINFO rmi;
    MENUINFO mi;
    BOOL ret;

    /* --- Positive: a valid extended SET (in-range iItem) still works and the
     *     surgical, benign public field is honoured. --- */
    hMenu = MakeOneItemMenu();
    ok(hMenu != NULL, "CreatePopupMenu failed, error %lu\n", GetLastError());
    if (!hMenu)
        return;

    ZeroMemory(&rmi, sizeof(rmi));
    rmi.cbSize = sizeof(rmi);          /* > sizeof(MENUINFO): extended path */
    rmi.fMask = MIM_MENUDATA;
    rmi.dwMenuData = TEST_MENUDATA;
    rmi.iItem = 0;                     /* valid position */
    ret = NtUserThunkedMenuInfo(hMenu, (LPCMENUINFO)&rmi);
    ok(ret, "NtUserThunkedMenuInfo(valid iItem) failed, error %lu\n", GetLastError());

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_MENUDATA;
    ok(GetMenuInfo(hMenu, &mi), "GetMenuInfo failed, error %lu\n", GetLastError());
    ok(mi.dwMenuData == TEST_MENUDATA,
       "extended SET did not honour benign public field: got %p\n",
       (PVOID)mi.dwMenuData);
    ok(GetMenuItemCount(hMenu) == 1, "menu item count wrong: %d\n",
       GetMenuItemCount(hMenu));
    DestroyMenu(hMenu);

    /* --- Contract: an out-of-range iItem via the extended SET must be accepted
     *     (clamped in-kernel to NO_SELECTED_ITEM) without corrupting the menu.
     *     An unfixed build stores the raw value, arming a later OOB on
     *     rgItems[iItem]; the fixed build clamps it. We assert the SET succeeds
     *     and the menu remains a valid, usable object. --- */
    hMenu = MakeOneItemMenu();
    ok(hMenu != NULL, "CreatePopupMenu(2) failed, error %lu\n", GetLastError());
    if (!hMenu)
        return;

    ZeroMemory(&rmi, sizeof(rmi));
    rmi.cbSize = sizeof(rmi);
    rmi.fMask = MIM_MENUDATA;
    rmi.dwMenuData = TEST_MENUDATA;
    rmi.iItem = 0x7FFFFFFF;            /* wildly out of range for a 1-item menu */
    ret = NtUserThunkedMenuInfo(hMenu, (LPCMENUINFO)&rmi);
    ok(ret, "NtUserThunkedMenuInfo(oob iItem) failed, error %lu\n", GetLastError());

    /* Menu survived and is still valid: the out-of-range focus index did not
     * corrupt the object (and, in the fixed kernel, was not stored). */
    ok(GetMenuItemCount(hMenu) == 1,
       "menu corrupted by out-of-range iItem SET: count %d\n",
       GetMenuItemCount(hMenu));
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_MENUDATA;
    ok(GetMenuInfo(hMenu, &mi) && mi.dwMenuData == TEST_MENUDATA,
       "menu unusable after out-of-range iItem SET\n");

    DestroyMenu(hMenu);

    TestFocusedItemTracking();
}
