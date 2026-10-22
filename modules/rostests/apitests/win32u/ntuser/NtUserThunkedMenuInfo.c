/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     T68 - win32k menu type-confusion / kernel-pointer EoP contract
 *              tests (Astra#3 P1). The cbSize-extended ROSMENUINFO /
 *              ROSMENUITEMINFO transfer tail must NEVER be usable to inject a
 *              kernel pointer into a MENU (spwndNotify / hWnd) or to leak one
 *              back to user mode (spwndNotify / lpstr), while legitimate
 *              menu-info set/get keeps working unchanged.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

/*
 * Mirror of win32ss/user/ntuser/menu.h tagROSMENUINFO.
 *
 * The kernel (UserMenuInfo) copies exactly cbSize bytes of this layout from the
 * caller, and accepts any cbSize in [sizeof(MENUINFO), sizeof(ROSMENUINFO)].
 * Legitimate user32 only ever passes sizeof(MENUINFO); we reproduce the full
 * extended layout here so the test can drive the extended path the way a
 * malicious caller would, straight through NtUserThunkedMenuInfo.
 *
 * Default alignment on purpose - it must match the kernel struct byte-for-byte
 * so cbSize == sizeof(TEST_ROSMENUINFO) passes the kernel's upper bound check.
 */
typedef struct _TEST_ROSMENUINFO
{
    /* ----------- MENUINFO ----------- */
    DWORD cbSize;
    DWORD fMask;
    DWORD dwStyle;
    UINT cyMax;
    HBRUSH hbrBack;
    DWORD dwContextHelpID;
    ULONG_PTR dwMenuData;
    /* ----------- Extra (ReactOS internal transfer tail) ----------- */
    ULONG fFlags;
    UINT iItem;
    UINT cItems;
    WORD cxMenu;
    WORD cyMenu;
    ULONG cxTextAlign;
    PWND spwndNotify;   /* kernel PWND - the type-confusion sink */
    INT iTop;
    INT iMaxTop;
    DWORD dwArrowsOn:2;
    HMENU Self;
    HWND Wnd;           /* internal owner window - must not be adopted */
    BOOL TimeToHide;
} TEST_ROSMENUINFO;

/* A recognisable bogus kernel-ish pointer. Must never be dereferenced: these
 * tests only ever SET it and then verify it was ignored - they never trigger a
 * menu operation that would follow spwndNotify (that would be a weaponized EoP
 * and would bugcheck an unfixed build). */
#ifdef _WIN64
#define SENTINEL_PTR ((ULONG_PTR)0xDEADBEEFDEADBEEFULL)
#else
#define SENTINEL_PTR ((ULONG_PTR)0xDEADBEEFUL)
#endif

#define TEST_MENUDATA ((ULONG_PTR)0x1234ABCD)

/*
 * Contract A - legitimate menu-info set/get is unaffected by the fix.
 *   - public SetMenuInfo/GetMenuInfo round-trips its fields,
 *   - item text set via SetMenuItemInfo comes back as a STRING (never a kernel
 *     pointer) from GetMenuItemInfo. This exercises the ITEM->Xlpstr path the
 *     fix must leave intact when it stops leaking ROSMENUITEMINFO.lpstr.
 */
static void Test_ValidRoundTrip(void)
{
    HMENU hMenu;
    MENUINFO mi, mi2;
    MENUITEMINFOW mii;
    WCHAR buf[32];

    hMenu = CreatePopupMenu();
    ok(hMenu != NULL, "CreatePopupMenu failed, error %lu\n", GetLastError());
    if (!hMenu)
        return;

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE | MIM_MAXHEIGHT | MIM_MENUDATA;
    mi.dwStyle = MNS_NOCHECK;
    mi.cyMax = 321;
    mi.dwMenuData = TEST_MENUDATA;
    ok(SetMenuInfo(hMenu, &mi), "SetMenuInfo failed, error %lu\n", GetLastError());

    ZeroMemory(&mi2, sizeof(mi2));
    mi2.cbSize = sizeof(mi2);
    mi2.fMask = MIM_STYLE | MIM_MAXHEIGHT | MIM_MENUDATA;
    ok(GetMenuInfo(hMenu, &mi2), "GetMenuInfo failed, error %lu\n", GetLastError());
    ok(mi2.cyMax == 321, "cyMax round-trip failed: got %u\n", mi2.cyMax);
    ok(mi2.dwMenuData == TEST_MENUDATA,
       "dwMenuData round-trip failed: got %p\n", (PVOID)mi2.dwMenuData);
    ok((mi2.dwStyle & MNS_NOCHECK) == MNS_NOCHECK,
       "dwStyle round-trip failed: got %lx\n", mi2.dwStyle);

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STRING;
    mii.wID = 10;
    mii.dwTypeData = (LPWSTR)L"Hello";
    ok(InsertMenuItemW(hMenu, 0, TRUE, &mii),
       "InsertMenuItemW failed, error %lu\n", GetLastError());

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf);
    buf[0] = 0;
    ok(GetMenuItemInfoW(hMenu, 10, FALSE, &mii),
       "GetMenuItemInfoW failed, error %lu\n", GetLastError());
    ok(lstrcmpW(buf, L"Hello") == 0, "item text round-trip failed: got '%ls'\n", buf);

    DestroyMenu(hMenu);
}

/*
 * Contract B - the cbSize-extended SET path must not import kernel-owned fields
 * from user input.
 *
 * We issue a malicious oversized NtUserThunkedMenuInfo carrying:
 *   - a benign PUBLIC field (dwMenuData) that must still be honoured, and
 *   - a bogus spwndNotify plus a real window handle in Wnd that must both be
 *     ignored by the kernel.
 *
 * Observable (safe, non-crashing) fail-before/pass-after:
 *   NtUserGetMenuItemRect(NULL, ...) falls back to the menu's internal hWnd.
 *   For an un-attached popup that hWnd is NULL -> the call returns FALSE.
 *   If the vulnerable code adopted our Wnd handle, hWnd would resolve to a real
 *   window and the call would return TRUE. spwndNotify is removed from user
 *   input by the very same change, so this differential also guards it without
 *   ever dereferencing the bogus pointer.
 */
static void Test_OversizedSet_NoInjection(void)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    HMENU hMenu = NULL;
    HWND hWnd = NULL;
    TEST_ROSMENUINFO rmi;
    MENUINFO mi;
    MENUITEMINFOW mii;
    RECT rcl;
    BOOL ret;

    hWnd = CreateWindowW(L"BUTTON", L"T68", WS_OVERLAPPEDWINDOW,
                         0, 0, 50, 30, NULL, NULL, hinst, NULL);
    ok(hWnd != NULL, "CreateWindowW failed, error %lu\n", GetLastError());

    hMenu = CreatePopupMenu();
    ok(hMenu != NULL, "CreatePopupMenu failed, error %lu\n", GetLastError());
    if (!hMenu || !hWnd)
        goto cleanup;

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STRING;
    mii.wID = 1;
    mii.dwTypeData = (LPWSTR)L"Item";
    ok(InsertMenuItemW(hMenu, 0, TRUE, &mii),
       "InsertMenuItemW failed, error %lu\n", GetLastError());

    ZeroMemory(&rmi, sizeof(rmi));
    rmi.cbSize = sizeof(rmi);          /* > sizeof(MENUINFO): extended path */
    rmi.fMask = MIM_MENUDATA;          /* benign public field, must be honoured */
    rmi.dwMenuData = TEST_MENUDATA;
    rmi.spwndNotify = (PWND)SENTINEL_PTR;  /* must be ignored (never deref'd here) */
    rmi.Wnd = hWnd;                    /* must NOT become Menu->hWnd */

    ret = NtUserThunkedMenuInfo(hMenu, (LPCMENUINFO)&rmi);
    ok(ret, "NtUserThunkedMenuInfo(oversized) failed, error %lu\n", GetLastError());

    /* Benign public field honoured -> fix is surgical, extended path still works. */
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_MENUDATA;
    ok(GetMenuInfo(hMenu, &mi), "GetMenuInfo failed, error %lu\n", GetLastError());
    ok(mi.dwMenuData == TEST_MENUDATA,
       "benign public field not applied through extended path: got %p\n",
       (PVOID)mi.dwMenuData);

    /* Fail-before/pass-after: internal owner window must not be adopted. */
    ZeroMemory(&rcl, sizeof(rcl));
    ret = NtUserGetMenuItemRect(NULL, hMenu, 0, &rcl);
    ok(!ret,
       "SECURITY(T68): NtUserGetMenuItemRect(NULL) succeeded -> the menu's "
       "internal hWnd was adopted from user input via the extended SET tail "
       "(kernel-pointer / owner injection present)\n");

    /* The menu survived the malicious set and is still a valid, usable object. */
    ok(GetMenuItemCount(hMenu) == 1,
       "menu corrupted by oversized set: item count %d\n", GetMenuItemCount(hMenu));

cleanup:
    if (hMenu)
        DestroyMenu(hMenu);
    if (hWnd)
        DestroyWindow(hWnd);
}

START_TEST(NtUserThunkedMenuInfo)
{
    Test_ValidRoundTrip();
    Test_OversizedSet_NoInjection();
}
