/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the list-view LVN_ITEMACTIVATE notification sent for the Enter key
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "wine/test.h"

#include <windows.h>
#include <commctrl.h>

#define CANARY 0xA5

static NMITEMACTIVATE g_Activate;
static UINT g_ActivateCount;
static UINT g_ReturnCount;

static LRESULT CALLBACK ParentProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_NOTIFY)
    {
        NMHDR *pnmh = (NMHDR *)lParam;
        if (pnmh->code == LVN_ITEMACTIVATE)
        {
            g_Activate = *(NMITEMACTIVATE *)lParam;
            g_ActivateCount++;
        }
        else if (pnmh->code == NM_RETURN)
        {
            g_ReturnCount++;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* Fills the stack below the caller with a pattern, so that a header-only notification shows garbage */
static DECLSPEC_NOINLINE void DirtyStack(void)
{
    volatile BYTE Buffer[4096];
    UINT i;
    for (i = 0; i < sizeof(Buffer); i++)
        Buffer[i] = CANARY;
}

/* Sends Enter to the list-view with at most one modifier (VK_SHIFT, VK_CONTROL, VK_MENU) held */
static void PressEnter(HWND hwndLV, BYTE Modifier)
{
    BYTE State[256], Saved[256];

    GetKeyboardState(Saved);
    memcpy(State, Saved, sizeof(State));
    State[VK_SHIFT] = State[VK_CONTROL] = State[VK_MENU] = 0;
    if (Modifier)
        State[Modifier] = 0x80;
    SetKeyboardState(State);

    memset(&g_Activate, 0, sizeof(g_Activate));
    g_ActivateCount = 0;
    g_ReturnCount = 0;
    DirtyStack();
    SendMessageW(hwndLV, WM_KEYDOWN, VK_RETURN, 0x001C0001);

    SetKeyboardState(Saved);
}

START_TEST(listviewv6)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    WNDCLASSW wc = { 0 };
    LVITEMW item = { 0 };
    WCHAR szText[] = L"Item";
    HWND hwndParent, hwndLV;
    int i;

    InitCommonControlsEx(&icc);

    wc.lpfnWndProc = ParentProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ListViewActivateParent";
    RegisterClassW(&wc);

    hwndParent = CreateWindowExW(0, wc.lpszClassName, L"listview", WS_OVERLAPPEDWINDOW,
                                 0, 0, 300, 300, NULL, NULL, wc.hInstance, NULL);
    ok(hwndParent != NULL, "parent window not created\n");
    if (!hwndParent)
        return;

    hwndLV = CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT,
                             0, 0, 280, 280, hwndParent, (HMENU)1, wc.hInstance, NULL);
    ok(hwndLV != NULL, "list-view not created\n");
    if (!hwndLV)
    {
        DestroyWindow(hwndParent);
        return;
    }

    {
        LVCOLUMNW col = { LVCF_WIDTH, 0, 200 };
        SendMessageW(hwndLV, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    }
    for (i = 0; i < 3; i++)
    {
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.pszText = szText;
        item.lParam = 0x1000 + i;
        SendMessageW(hwndLV, LVM_INSERTITEMW, 0, (LPARAM)&item);
    }
    item.stateMask = LVIS_FOCUSED | LVIS_SELECTED;
    item.state = LVIS_FOCUSED | LVIS_SELECTED;
    SendMessageW(hwndLV, LVM_SETITEMSTATE, 1, (LPARAM)&item);

    PressEnter(hwndLV, 0);
    ok(g_ReturnCount == 1, "NM_RETURN count %u\n", g_ReturnCount);
    ok(g_ActivateCount == 1, "LVN_ITEMACTIVATE count %u\n", g_ActivateCount);
    ok(g_Activate.uKeyFlags == 0, "Enter: uKeyFlags 0x%x\n", g_Activate.uKeyFlags);
    ok(g_Activate.iItem == 1, "Enter: iItem %d\n", g_Activate.iItem);
    ok(g_Activate.iSubItem == 0, "Enter: iSubItem %d\n", g_Activate.iSubItem);
    ok(g_Activate.ptAction.x == -1 && g_Activate.ptAction.y == -1, "Enter: ptAction (%ld,%ld)\n",
       g_Activate.ptAction.x, g_Activate.ptAction.y);
    ok(g_Activate.uChanged == 0, "Enter: uChanged 0x%x\n", g_Activate.uChanged);

    PressEnter(hwndLV, VK_SHIFT);
    ok(g_ActivateCount == 1, "Shift+Enter: LVN_ITEMACTIVATE count %u\n", g_ActivateCount);
    ok(g_Activate.uKeyFlags == LVKF_SHIFT, "Shift+Enter: uKeyFlags 0x%x\n", g_Activate.uKeyFlags);

    PressEnter(hwndLV, VK_CONTROL);
    ok(g_ActivateCount == 1, "Ctrl+Enter: LVN_ITEMACTIVATE count %u\n", g_ActivateCount);
    ok(g_Activate.uKeyFlags == LVKF_CONTROL, "Ctrl+Enter: uKeyFlags 0x%x\n", g_Activate.uKeyFlags);

    PressEnter(hwndLV, VK_MENU);
    ok(g_ActivateCount == 1, "Alt+Enter: LVN_ITEMACTIVATE count %u\n", g_ActivateCount);
    ok(g_Activate.uKeyFlags == LVKF_ALT, "Alt+Enter: uKeyFlags 0x%x\n", g_Activate.uKeyFlags);

    DestroyWindow(hwndParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
