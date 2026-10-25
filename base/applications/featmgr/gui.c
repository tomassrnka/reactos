/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     optfeatures.exe - "Optional Features" applet (Turn features on/off).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "featmgr.h"
#include <commctrl.h>
#include <strsafe.h>

#define IDC_LIST     1001
#define IDC_INSTALL  1002
#define IDC_REMOVE   1003
#define IDC_REFRESH  1004
#define IDC_CLOSE    1005
#define IDC_DESC     1006

static HWND g_hList, g_hDesc, g_hInstall, g_hRemove;
static FM_FEATURE *g_arr = NULL;
static int g_count = 0;

static void FillList(void)
{
    int i;
    LVITEMW it;
    if (g_arr) { FmFree(g_arr); g_arr = NULL; }
    ListView_DeleteAllItems(g_hList);
    g_count = FmEnumFeatures(&g_arr);

    for (i = 0; i < g_count; i++)
    {
        WCHAR state[48];
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = g_arr[i].Name;
        it.lParam = i;
        ListView_InsertItem(g_hList, &it);
        ListView_SetItemText(g_hList, i, 1,
            (LPWSTR)(g_arr[i].Tier == FM_TIER_SOFTWARE ? L"Software" : L"OS feature"));
        StringCchPrintfW(state, 48, L"%s%s", FmStateStr(g_arr[i].State),
                         g_arr[i].PayloadAvailable ? L"" : L" (deferred)");
        ListView_SetItemText(g_hList, i, 2, state);
        ListView_SetItemText(g_hList, i, 3, g_arr[i].Version);
    }
}

static int SelectedIndex(void)
{
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    return sel; /* lParam == row index == g_arr index (1:1 insert order) */
}

static void UpdateButtons(void)
{
    int i = SelectedIndex();
    BOOL canInstall = FALSE, canRemove = FALSE;
    if (i >= 0 && i < g_count)
    {
        if (g_arr[i].State == FM_STATE_ENABLED)
            canRemove = TRUE;
        else if (g_arr[i].PayloadAvailable)
            canInstall = TRUE;
        SetWindowTextW(g_hDesc, g_arr[i].Description);
    }
    else
    {
        SetWindowTextW(g_hDesc, L"Select a feature to see its description.");
    }
    EnableWindow(g_hInstall, canInstall);
    EnableWindow(g_hRemove, canRemove);
}

static void DoAction(HWND hwnd, BOOL enable)
{
    int i = SelectedIndex();
    HRESULT hr;
    WCHAR msg[512];
    if (i < 0 || i >= g_count) return;

    if (enable)
        hr = FmEnable(g_arr[i].Id, NULL);
    else
        hr = FmDisable(g_arr[i].Id, NULL);

    if (SUCCEEDED(hr))
        StringCchPrintfW(msg, 512, L"'%s' %s.", g_arr[i].Name,
                         enable ? L"installed and enabled" : L"disabled and reverted");
    else
        StringCchPrintfW(msg, 512, L"Operation failed (0x%08lx).\n\n"
                         L"If the payload is deferred, the feature cannot be enabled "
                         L"on this system yet.", hr);
    MessageBoxW(hwnd, msg, L"Optional Features", MB_OK |
                (SUCCEEDED(hr) ? MB_ICONINFORMATION : MB_ICONWARNING));
    FillList();
    UpdateButtons();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            LVCOLUMNW col;
            HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HWND hHdr = CreateWindowExW(0, L"STATIC", L"Turn ReactOS optional features on or off",
                WS_CHILD | WS_VISIBLE, 12, 10, 560, 18, hwnd, NULL, NULL, NULL);

            g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                12, 34, 560, 230, hwnd, (HMENU)(INT_PTR)IDC_LIST, NULL, NULL);
            ListView_SetExtendedListViewStyle(g_hList,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)L"Feature";   col.cx = 240; ListView_InsertColumn(g_hList, 0, &col);
            col.pszText = (LPWSTR)L"Tier";      col.cx = 90;  ListView_InsertColumn(g_hList, 1, &col);
            col.pszText = (LPWSTR)L"State";     col.cx = 140; ListView_InsertColumn(g_hList, 2, &col);
            col.pszText = (LPWSTR)L"Version";   col.cx = 80;  ListView_InsertColumn(g_hList, 3, &col);

            g_hDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC",
                L"Select a feature to see its description.",
                WS_CHILD | WS_VISIBLE, 12, 272, 560, 48, hwnd, (HMENU)(INT_PTR)IDC_DESC, NULL, NULL);

            g_hInstall = CreateWindowExW(0, L"BUTTON", L"&Install",
                WS_CHILD | WS_VISIBLE | WS_DISABLED, 12, 332, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_INSTALL, NULL, NULL);
            g_hRemove = CreateWindowExW(0, L"BUTTON", L"&Remove",
                WS_CHILD | WS_VISIBLE | WS_DISABLED, 110, 332, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_REMOVE, NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"Re&fresh", WS_CHILD | WS_VISIBLE,
                208, 332, 90, 26, hwnd, (HMENU)(INT_PTR)IDC_REFRESH, NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"&Close", WS_CHILD | WS_VISIBLE,
                482, 332, 90, 26, hwnd, (HMENU)(INT_PTR)IDC_CLOSE, NULL, NULL);

            SendMessageW(hHdr, WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(g_hDesc, WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(g_hInstall, WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(g_hRemove, WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDC_REFRESH), WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDC_CLOSE), WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(g_hList, WM_SETFONT, (WPARAM)hf, TRUE);

            FillList();
            UpdateButtons();
            return 0;
        }
        case WM_NOTIFY:
        {
            LPNMHDR nh = (LPNMHDR)lp;
            if (nh->idFrom == IDC_LIST &&
                (nh->code == LVN_ITEMCHANGED || nh->code == NM_CLICK))
                UpdateButtons();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
                case IDC_INSTALL: DoAction(hwnd, TRUE);  break;
                case IDC_REMOVE:  DoAction(hwnd, FALSE); break;
                case IDC_REFRESH: FillList(); UpdateButtons(); break;
                case IDC_CLOSE:   DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_DESTROY:
            if (g_arr) { FmFree(g_arr); g_arr = NULL; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG m;
    INITCOMMONCONTROLSEX icc;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(lpCmd);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NitroOptionalFeatures";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Optional Features",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 404, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
