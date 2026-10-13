/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests that menu item data keeps every bit of a pointer-sized value
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _WIN64
#define DATA1 ((ULONG_PTR)0x123456789ABCDEF0ULL)
#define DATA2 ((ULONG_PTR)0xFEDCBA9876543210ULL)
#define DATA3 ((ULONG_PTR)0x00007FF612345678ULL)
#else
#define DATA1 ((ULONG_PTR)0x9ABCDEF0UL)
#define DATA2 ((ULONG_PTR)0x76543210UL)
#define DATA3 ((ULONG_PTR)0x82345678UL)
#endif

#define ID_ITEM 100
#define ID_ITEM_A 101
#define ID_OWNERDRAW 102

static ULONG_PTR g_MeasureData;
static ULONG_PTR g_DrawData;
static UINT g_MeasureCount;
static UINT g_DrawCount;

static ULONG_PTR GetItemData(HMENU hMenu, UINT id)
{
    MENUITEMINFOW mii = { sizeof(mii) };

    mii.fMask = MIIM_DATA;
    mii.dwItemData = 0xCC;
    ok(GetMenuItemInfoW(hMenu, id, FALSE, &mii), "GetMenuItemInfoW failed, error %lu\n", GetLastError());
    return mii.dwItemData;
}

static void TestInsertSetGet(void)
{
    HMENU hMenu = CreatePopupMenu();
    MENUITEMINFOW mii = { sizeof(mii) };
    MENUITEMINFOA miiA = { sizeof(miiA) };
    WCHAR szText[] = L"Item";
    CHAR szTextA[] = "ItemA";

    ok(hMenu != NULL, "CreatePopupMenu failed\n");
    if (!hMenu)
        return;

    mii.fMask = MIIM_ID | MIIM_DATA | MIIM_STRING;
    mii.wID = ID_ITEM;
    mii.dwItemData = DATA1;
    mii.dwTypeData = szText;
    ok(InsertMenuItemW(hMenu, 0, TRUE, &mii), "InsertMenuItemW failed, error %lu\n", GetLastError());
    ok(GetItemData(hMenu, ID_ITEM) == DATA1, "InsertMenuItemW: got %p\n", (PVOID)GetItemData(hMenu, ID_ITEM));

    mii.fMask = MIIM_DATA;
    mii.dwItemData = DATA2;
    ok(SetMenuItemInfoW(hMenu, ID_ITEM, FALSE, &mii), "SetMenuItemInfoW failed, error %lu\n", GetLastError());
    ok(GetItemData(hMenu, ID_ITEM) == DATA2, "SetMenuItemInfoW: got %p\n", (PVOID)GetItemData(hMenu, ID_ITEM));

    miiA.fMask = MIIM_ID | MIIM_DATA | MIIM_STRING;
    miiA.wID = ID_ITEM_A;
    miiA.dwItemData = DATA3;
    miiA.dwTypeData = szTextA;
    ok(InsertMenuItemA(hMenu, 1, TRUE, &miiA), "InsertMenuItemA failed, error %lu\n", GetLastError());
    miiA.fMask = MIIM_DATA;
    miiA.dwItemData = 0xCC;
    ok(GetMenuItemInfoA(hMenu, ID_ITEM_A, FALSE, &miiA), "GetMenuItemInfoA failed, error %lu\n", GetLastError());
    ok(miiA.dwItemData == DATA3, "InsertMenuItemA: got %p\n", (PVOID)miiA.dwItemData);

    ok(AppendMenuW(hMenu, MF_OWNERDRAW, ID_OWNERDRAW, (LPCWSTR)DATA1), "AppendMenuW failed, error %lu\n", GetLastError());
    ok(GetItemData(hMenu, ID_OWNERDRAW) == DATA1, "AppendMenuW(MF_OWNERDRAW): got %p\n", (PVOID)GetItemData(hMenu, ID_OWNERDRAW));

    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_MEASUREITEM:
        {
            MEASUREITEMSTRUCT *pmis = (MEASUREITEMSTRUCT *)lParam;
            if (pmis->CtlType == ODT_MENU && pmis->itemID == ID_OWNERDRAW)
            {
                g_MeasureData = pmis->itemData;
                g_MeasureCount++;
                pmis->itemWidth = 40;
                pmis->itemHeight = 16;
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT *pdis = (DRAWITEMSTRUCT *)lParam;
            if (pdis->CtlType == ODT_MENU && pdis->itemID == ID_OWNERDRAW)
            {
                g_DrawData = pdis->itemData;
                g_DrawCount++;
                return TRUE;
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void TestOwnerDrawMessages(void)
{
    WNDCLASSW wc = { 0 };
    HMENU hMenu;
    HWND hwnd;
    MSG msg;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MenuItemDataTest";
    if (!RegisterClassW(&wc))
    {
        skip("RegisterClassW failed, error %lu\n", GetLastError());
        return;
    }

    hMenu = CreateMenu();
    ok(AppendMenuW(hMenu, MF_STRING, ID_ITEM, L"&File"), "AppendMenuW failed\n");
    ok(AppendMenuW(hMenu, MF_OWNERDRAW, ID_OWNERDRAW, (LPCWSTR)DATA2), "AppendMenuW(MF_OWNERDRAW) failed\n");

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"MenuItemData", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           50, 50, 300, 200, NULL, hMenu, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
    {
        DestroyMenu(hMenu);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    DrawMenuBar(hwnd);
    RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);

    ok(g_MeasureCount > 0, "no WM_MEASUREITEM for the owner-drawn item\n");
    ok(g_DrawCount > 0, "no WM_DRAWITEM for the owner-drawn item\n");
    if (g_MeasureCount)
        ok(g_MeasureData == DATA2, "WM_MEASUREITEM itemData %p\n", (PVOID)g_MeasureData);
    if (g_DrawCount)
        ok(g_DrawData == DATA2, "WM_DRAWITEM itemData %p\n", (PVOID)g_DrawData);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

START_TEST(MenuItemData)
{
    TestInsertSetGet();
    TestOwnerDrawMessages();
}
