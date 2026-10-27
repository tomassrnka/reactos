/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     optfeatures.exe - "Optional Features" applet.
 *              OS-features tier uses a Windows-native "Turn Windows features on
 *              or off" look: a checkbox list committed with OK/Apply/Cancel.
 *              Software tier uses a "Programs and Features" look (list with
 *              Install / Change / Uninstall verbs). Both are reachable from
 *              Control Panel (see dll/cpl/optionalfeatures + the appwiz hook).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "featmgr.h"
#include <commctrl.h>
#include <strsafe.h>

#define IDC_LIST       1001
#define IDC_INSTALL    1002
#define IDC_UNINSTALL  1003
#define IDC_REFRESH    1004
#define IDC_CLOSE      1005
#define IDC_DESC       1006
#define IDC_TAB        1007
#define IDC_ENABLE     1008
#define IDC_DISABLE    1009
#define IDC_OK         1010
#define IDC_CANCEL     1011
#define IDC_APPLY      1012
#define IDC_INSTR      1013

static HWND g_hTab, g_hList, g_hDesc, g_hInstr;
static HWND g_hInstall, g_hEnable, g_hDisable, g_hUninstall;
static HWND g_hOK, g_hCancel, g_hApply, g_hRefresh, g_hClose;
static HFONT g_hFont = NULL;
static FM_FEATURE *g_arr = NULL;
static int g_count = 0;
static int g_tier = FM_TIER_OS;   /* selected tab: 0 = OS features, 1 = Software */
static BOOL g_populating = FALSE;  /* ignore list state notifications while (re)filling */
static BOOL g_reverting = FALSE;   /* re-entrancy guard for check-state reverts */

/* Windows "Windows Features" instruction banner (OS tier). */
static const WCHAR szOsInstr[] =
    L"To turn a feature on, select its check box. To turn a feature off, clear "
    L"its check box. A shaded box means that only part of a feature is turned on.";
static const WCHAR szSwInstr[] =
    L"To change or uninstall an installed program, select it and then click "
    L"Change or Uninstall.";

/* Why a feature cannot be disabled/uninstalled right now, mirroring the engine
 * guards (CheckRemovable / FmGetDependents). Returns 0 if allowed; otherwise
 * ERROR_ACCESS_DENIED (base component) or ERROR_DEPENDENT_SERVICES_RUNNING,
 * with the enabled dependents (comma separated) in dep. */
static DWORD RemoveGuard(const FM_FEATURE *f, LPWSTR dep, DWORD cch)
{
    if (cch) dep[0] = L'\0';
    if (!f->Removable) return ERROR_ACCESS_DENIED;
    if (FmGetDependents(f->Id, dep, cch)) return ERROR_DEPENDENT_SERVICES_RUNNING;
    return 0;
}

static void UpdateTabLabels(void)
{
    int i, n[2] = { 0, 0 }, t;
    WCHAR lbl[64];
    TCITEMW ti;
    for (i = 0; i < g_count; i++) n[g_arr[i].Tier == FM_TIER_SOFTWARE ? 1 : 0]++;
    for (t = 0; t < 2; t++)
    {
        StringCchPrintfW(lbl, ARRAYSIZE(lbl), L"%s (%d)",
                         t ? L"Software" : L"OS features", n[t]);
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = lbl;
        TabCtrl_SetItem(g_hTab, t, &ti);
    }
}

/* Index into g_arr of the selected row (row lParam), or -1. */
static int SelectedIndex(void)
{
    LVITEMW it;
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(g_hList, &it)) return -1;
    return (int)it.lParam;
}

static void UpdateButtons(void);

/* Fill the list with the features of the selected tier (g_arr already loaded).
 * On the OS tier the list carries checkboxes whose checked state mirrors the
 * persisted Enabled state (Windows "Windows Features" dialog). */
static void Populate(LPCWSTR keepId)
{
    int i, row = 0, reselect = -1;
    BOOL os = (g_tier == FM_TIER_OS);
    LVITEMW it;

    g_populating = TRUE;
    ListView_DeleteAllItems(g_hList);
    ListView_SetExtendedListViewStyleEx(g_hList, LVS_EX_CHECKBOXES,
                                        os ? LVS_EX_CHECKBOXES : 0);

    for (i = 0; i < g_count; i++)
    {
        WCHAR buf[FM_MAX_DEPS * 2 + 64];
        if ((g_arr[i].Tier == FM_TIER_SOFTWARE ? 1 : 0) != g_tier) continue;

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = g_arr[i].Name;
        it.lParam = i;
        ListView_InsertItem(g_hList, &it);

        StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s%s", FmStateStr(g_arr[i].State),
                         g_arr[i].PayloadAvailable ? L"" : L" (deferred)");
        ListView_SetItemText(g_hList, row, 1, buf);
        ListView_SetItemText(g_hList, row, 2, g_arr[i].Version);
        ListView_SetItemText(g_hList, row, 3, g_arr[i].InstallDate);

        buf[0] = L'\0';
        if (!os && g_arr[i].Provider[0])
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"Source: %s", g_arr[i].Provider);
        else if (!g_arr[i].Removable)
            StringCchCatW(buf, ARRAYSIZE(buf), L"Base component");
        else if (g_arr[i].State != FM_STATE_NOT_INSTALLED)
        {
            WCHAR dep[FM_MAX_DEPS * 2];
            if (FmGetDependents(g_arr[i].Id, dep, ARRAYSIZE(dep)))
                StringCchPrintfW(buf, ARRAYSIZE(buf), L"Required by: %s", dep);
        }
        if (g_arr[i].PendingReboot)
            StringCchCatW(buf, ARRAYSIZE(buf), buf[0] ? L"; Reboot pending" : L"Reboot pending");
        ListView_SetItemText(g_hList, row, 4, buf);

        if (os)
            ListView_SetCheckState(g_hList, row, g_arr[i].State == FM_STATE_ENABLED);

        if (keepId && !_wcsicmp(keepId, g_arr[i].Id)) reselect = row;
        row++;
    }
    if (reselect >= 0)
        ListView_SetItemState(g_hList, reselect, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    g_populating = FALSE;
    UpdateButtons();
}

static void FillList(void)
{
    WCHAR keep[FM_MAX_ID];
    int i = (g_arr ? SelectedIndex() : -1);
    keep[0] = L'\0';
    if (i >= 0 && i < g_count) StringCchCopyW(keep, ARRAYSIZE(keep), g_arr[i].Id);
    ListView_DeleteAllItems(g_hList);   /* rows hold indexes into the old array */
    if (g_arr) { FmFree(g_arr); g_arr = NULL; }
    g_count = FmEnumFeatures(&g_arr);
    UpdateTabLabels();
    Populate(keep);
}

static void UpdateButtons(void)
{
    int i = SelectedIndex();
    BOOL os = (g_tier == FM_TIER_OS);
    BOOL canInstall = FALSE, canEnable = FALSE, canDisable = FALSE, canUninstall = FALSE;
    WCHAR text[FM_MAX_DESC + FM_MAX_DEPS * 2 + 256];

    if (i >= 0 && i < g_count)
    {
        const FM_FEATURE *f = &g_arr[i];
        WCHAR dep[FM_MAX_DEPS * 2];
        DWORD guard = RemoveGuard(f, dep, ARRAYSIZE(dep));

        /* Install/Uninstall change presence; Enable/Disable toggle an installed feature. */
        canInstall   = f->State == FM_STATE_NOT_INSTALLED && f->PayloadAvailable;
        canEnable    = f->State == FM_STATE_DISABLED && f->PayloadAvailable;
        canDisable   = f->State == FM_STATE_ENABLED && !guard;
        canUninstall = f->State != FM_STATE_NOT_INSTALLED && !guard;

        StringCchPrintfW(text, ARRAYSIZE(text), L"%s", f->Description);
        if (f->InstallDate[0])
            StringCchPrintfW(text + wcslen(text), ARRAYSIZE(text) - wcslen(text),
                             L"\r\nInstalled: %s", f->InstallDate);
        if (f->PendingReboot)
            StringCchCatW(text, ARRAYSIZE(text), L"\r\nA reboot is pending to finish removing this feature.");
        if (f->State != FM_STATE_NOT_INSTALLED && guard == ERROR_ACCESS_DENIED)
            StringCchCatW(text, ARRAYSIZE(text),
                L"\r\nBase component of the system: it cannot be turned off or uninstalled.");
        else if (f->State != FM_STATE_NOT_INSTALLED && guard == ERROR_DEPENDENT_SERVICES_RUNNING)
        {
            StringCchCatW(text, ARRAYSIZE(text), L"\r\nCannot be turned off or uninstalled while enabled features depend on it: ");
            StringCchCatW(text, ARRAYSIZE(text), dep);
        }
        if (!f->PayloadAvailable && f->State != FM_STATE_ENABLED)
            StringCchCatW(text, ARRAYSIZE(text), L"\r\nPayload not staged on this system (deferred).");
        SetWindowTextW(g_hDesc, text);
    }
    else
    {
        SetWindowTextW(g_hDesc, ListView_GetItemCount(g_hList) ? L"Select a feature to see its description." :
            (g_tier == FM_TIER_SOFTWARE ?
                L"No software-tier entries in the catalog yet." :
                L"No OS features in the catalog."));
    }

    /* OS tier commits through OK/Apply; the per-row verbs belong to Software. */
    if (!os)
    {
        EnableWindow(g_hInstall, canInstall);
        EnableWindow(g_hEnable, canEnable);
        EnableWindow(g_hDisable, canDisable);
        EnableWindow(g_hUninstall, canUninstall);
    }
}

typedef enum { ACT_INSTALL, ACT_ENABLE, ACT_DISABLE, ACT_UNINSTALL } ACTION;

static void DoAction(HWND hwnd, ACTION act)
{
    int i = SelectedIndex();
    HRESULT hr;
    WCHAR msg[768];
    FM_FEATURE f;
    UINT icon = MB_ICONWARNING;
    if (i < 0 || i >= g_count) return;
    f = g_arr[i];   /* g_arr is reloaded below */

    if (act == ACT_UNINSTALL)
    {
        StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"Uninstall '%s'?\n\nIt will be turned off and its record removed; "
            L"it returns to Not installed.", f.Name);
        if (MessageBoxW(hwnd, msg, L"Optional Features", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    switch (act)
    {
        case ACT_INSTALL:
        case ACT_ENABLE:    hr = FmEnable(f.Id, NULL); break;
        case ACT_DISABLE:   hr = FmDisable(f.Id, NULL); break;
        default:            hr = FmUninstall(f.Id, NULL); break;
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED))
    {
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"'%s' was turned off, but some components "
                         L"are removed at the next reboot. It stays listed until then.", f.Name);
        icon = MB_ICONINFORMATION;
    }
    else if (SUCCEEDED(hr))
    {
        static const LPCWSTR done[] = { L"installed and enabled", L"enabled",
                                        L"disabled and reverted", L"uninstalled" };
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"'%s' %s.", f.Name, done[act]);
        icon = MB_ICONINFORMATION;
    }
    else if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
             (act == ACT_DISABLE || act == ACT_UNINSTALL))
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"'%s' is a base component of the system "
                         L"and cannot be disabled or removed.", f.Name);
    else if (hr == HRESULT_FROM_WIN32(ERROR_DEPENDENT_SERVICES_RUNNING))
    {
        WCHAR dep[FM_MAX_DEPS * 2];
        FmGetDependents(f.Id, dep, ARRAYSIZE(dep));
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"'%s' cannot be turned off while other enabled "
                         L"features depend on it (%s). Turn those off first.", f.Name, dep);
    }
    else
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Operation failed (0x%08lx).\n\n"
                         L"If the payload is deferred, the feature cannot be enabled "
                         L"on this system yet.", hr);
    MessageBoxW(hwnd, msg, L"Optional Features", MB_OK | icon);
    FillList();
}

/* --- OS tier: commit the checkbox selection, Windows "Windows Features" style --- */

/* TRUE if any checkbox differs from the persisted state. */
static BOOL OsHasPendingChanges(void)
{
    int row, n = ListView_GetItemCount(g_hList);
    LVITEMW it;
    if (g_tier != FM_TIER_OS) return FALSE;
    for (row = 0; row < n; row++)
    {
        BOOL checked = ListView_GetCheckState(g_hList, row);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_PARAM; it.iItem = row;
        if (!ListView_GetItem(g_hList, &it)) continue;
        if ((int)it.lParam < g_count &&
            checked != (g_arr[it.lParam].State == FM_STATE_ENABLED))
            return TRUE;
    }
    return FALSE;
}

/* Apply the checkbox selection. Enables happen first; disables run in repeated
 * passes so that a dependent unchecked in the same batch is turned off before
 * the feature it depends on (the engine refuses an in-use disable). Returns the
 * number of still-failed operations and fills summary. */
static int ApplyOsChanges(HWND hwnd, LPWSTR summary, DWORD cch)
{
    int row, n = ListView_GetItemCount(g_hList);
    int done = 0, failed = 0, pass;
    LVITEMW it;
    WCHAR line[256];

    if (summary && cch) summary[0] = L'\0';

    /* Pass 1: enable everything newly checked (install-on-check for NotInstalled). */
    for (row = 0; row < n; row++)
    {
        FM_FEATURE *f;
        if (!ListView_GetCheckState(g_hList, row)) continue;
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = row;
        if (!ListView_GetItem(g_hList, &it) || (int)it.lParam >= g_count) continue;
        f = &g_arr[it.lParam];
        if (f->State == FM_STATE_ENABLED) continue;
        if (!f->PayloadAvailable) { failed++; continue; }
        if (SUCCEEDED(FmEnable(f->Id, NULL))) done++;
        else failed++;
    }

    /* Passes 2..N: disable everything newly unchecked, retrying blocked ones so
     * dependents (also unchecked) clear first. Stop when a pass makes no change. */
    for (pass = 0; pass < n + 1; pass++)
    {
        int progress = 0;
        for (row = 0; row < n; row++)
        {
            FM_FEATURE *f;
            HRESULT hr;
            if (ListView_GetCheckState(g_hList, row)) continue;
            ZeroMemory(&it, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = row;
            if (!ListView_GetItem(g_hList, &it) || (int)it.lParam >= g_count) continue;
            f = &g_arr[it.lParam];
            if (FmGetState(f->Id) != FM_STATE_ENABLED) continue;  /* already off */
            hr = FmDisable(f->Id, NULL);
            if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED))
            {
                done++; progress++;
            }
        }
        if (!progress) break;
    }

    /* Anything still enabled but unchecked, or unchecked+enabled that refused. */
    for (row = 0; row < n; row++)
    {
        FM_FEATURE *f;
        if (ListView_GetCheckState(g_hList, row)) continue;
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = row;
        if (!ListView_GetItem(g_hList, &it) || (int)it.lParam >= g_count) continue;
        f = &g_arr[it.lParam];
        if (FmGetState(f->Id) == FM_STATE_ENABLED)
        {
            failed++;
            if (summary)
            {
                StringCchPrintfW(line, ARRAYSIZE(line), L"\r\n  %s could not be turned off.", f->Name);
                StringCchCatW(summary, cch, line);
            }
        }
    }

    UNREFERENCED_PARAMETER(hwnd);
    return failed;
}

static void CommitOs(HWND hwnd, BOOL closeAfter)
{
    WCHAR summary[1024];
    int failed;

    if (!OsHasPendingChanges())
    {
        if (closeAfter) DestroyWindow(hwnd);
        return;
    }

    failed = ApplyOsChanges(hwnd, summary, ARRAYSIZE(summary));
    FillList();

    if (failed)
    {
        WCHAR msg[1280];
        StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"Some features could not be changed. Base components cannot be turned "
            L"off, deferred payloads cannot be turned on, and a feature in use by "
            L"another enabled feature stays on.%s", summary);
        MessageBoxW(hwnd, msg, L"Windows Features", MB_OK | MB_ICONWARNING);
        return;   /* keep the dialog open so the user sees the corrected state */
    }

    MessageBoxW(hwnd, L"ReactOS has applied the requested changes.",
                L"Windows Features", MB_OK | MB_ICONINFORMATION);
    if (closeAfter) DestroyWindow(hwnd);
}

/* Reject check-state changes the engine would refuse outright, so the dialog
 * never shows an illegal pending selection (Windows greys / snaps these back). */
static void OnOsCheck(int row, BOOL nowChecked)
{
    LVITEMW it;
    const FM_FEATURE *f;
    LPCWSTR why = NULL;

    ZeroMemory(&it, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = row;
    if (!ListView_GetItem(g_hList, &it) || (int)it.lParam >= g_count) return;
    f = &g_arr[it.lParam];

    if (!nowChecked && f->State == FM_STATE_ENABLED && !f->Removable)
        why = L"is a base component of the system and cannot be turned off.";
    else if (nowChecked && f->State != FM_STATE_ENABLED && !f->PayloadAvailable)
        why = L"has a deferred payload that is not staged on this system yet.";

    if (why)
    {
        WCHAR msg[256];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"'%s' %s", f->Name, why);
        g_reverting = TRUE;
        ListView_SetCheckState(g_hList, row, !nowChecked);  /* snap back */
        g_reverting = FALSE;
        SetWindowTextW(g_hDesc, msg);
    }
}

/* Position the per-tab button rows. The OS tier reads like "Windows Features"
 * (OK / Cancel / Apply, with an Uninstall for the forget-record verb); the
 * Software tier reads like "Programs and Features" (Install / Change / verbs). */
static void LayoutButtons(HWND hwnd)
{
    BOOL os = (g_tier == FM_TIER_OS);
    RECT rc;
    int y, right, bw = 92, bh = 26, gap = 6;
    GetClientRect(hwnd, &rc);
    y = rc.bottom - bh - 12;
    right = rc.right - 12;

    ShowWindow(g_hOK,       os ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hCancel,   os ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hApply,    os ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hInstall,  os ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hEnable,   os ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hDisable,  os ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hClose,    os ? SW_HIDE : SW_SHOW);

    if (os)
    {
        MoveWindow(g_hApply,  right - bw,               y, bw, bh, TRUE);
        MoveWindow(g_hCancel, right - 2 * bw - gap,     y, bw, bh, TRUE);
        MoveWindow(g_hOK,     right - 3 * bw - 2 * gap, y, bw, bh, TRUE);
        MoveWindow(g_hUninstall, 12,                    y, bw, bh, TRUE);
        MoveWindow(g_hRefresh,   12 + bw + gap,         y, bw, bh, TRUE);
    }
    else
    {
        MoveWindow(g_hInstall,   12,                    y, bw, bh, TRUE);
        MoveWindow(g_hEnable,    12 + (bw + gap),       y, bw, bh, TRUE);
        MoveWindow(g_hDisable,   12 + 2 * (bw + gap),   y, bw, bh, TRUE);
        MoveWindow(g_hUninstall, 12 + 3 * (bw + gap),   y, bw, bh, TRUE);
        MoveWindow(g_hClose,  right - bw,               y, bw, bh, TRUE);
        MoveWindow(g_hRefresh, right - 2 * bw - gap,    y, bw, bh, TRUE);
    }
    SetWindowTextW(g_hInstr, os ? szOsInstr : szSwInstr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            LVCOLUMNW col;
            TCITEMW ti;
            NONCLIENTMETRICSW ncm;
            HWND hHdr;

            /* Authentic dialog typeface (Tahoma/Segoe per theme), not the old
             * bitmap DEFAULT_GUI_FONT - this is what makes it read as native. */
            ZeroMemory(&ncm, sizeof(ncm));
            ncm.cbSize = sizeof(ncm);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
                g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (!g_hFont)
                g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            hHdr = CreateWindowExW(0, L"STATIC", L"Turn ReactOS features on or off",
                WS_CHILD | WS_VISIBLE, 12, 10, 620, 18, hwnd, NULL, NULL, NULL);
            g_hInstr = CreateWindowExW(0, L"STATIC", szOsInstr,
                WS_CHILD | WS_VISIBLE, 12, 30, 620, 30, hwnd,
                (HMENU)(INT_PTR)IDC_INSTR, NULL, NULL);

            /* two tiers = two tabs; one list shows the selected tier's features */
            g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                12, 64, 628, 300, hwnd, (HMENU)(INT_PTR)IDC_TAB, NULL, NULL);
            ZeroMemory(&ti, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)L"OS features";
            TabCtrl_InsertItem(g_hTab, 0, &ti);
            ti.pszText = (LPWSTR)L"Software";
            TabCtrl_InsertItem(g_hTab, 1, &ti);

            g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                18, 94, 616, 262, hwnd, (HMENU)(INT_PTR)IDC_LIST, NULL, NULL);
            ListView_SetExtendedListViewStyle(g_hList,
                LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);

            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)L"Feature";   col.cx = 180; ListView_InsertColumn(g_hList, 0, &col);
            col.pszText = (LPWSTR)L"State";     col.cx = 110; ListView_InsertColumn(g_hList, 1, &col);
            col.pszText = (LPWSTR)L"Version";   col.cx = 60;  ListView_InsertColumn(g_hList, 2, &col);
            col.pszText = (LPWSTR)L"Installed (UTC)"; col.cx = 130; ListView_InsertColumn(g_hList, 3, &col);
            col.pszText = (LPWSTR)L"Notes";     col.cx = 130; ListView_InsertColumn(g_hList, 4, &col);

            g_hDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC",
                L"Select a feature to see its description.",
                WS_CHILD | WS_VISIBLE, 12, 372, 628, 62, hwnd, (HMENU)(INT_PTR)IDC_DESC, NULL, NULL);

            /* OS-tier commit row (Windows "Windows Features" style). */
            g_hOK = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_OK, NULL, NULL);
            g_hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_CANCEL, NULL, NULL);
            g_hApply = CreateWindowExW(0, L"BUTTON", L"&Apply",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_APPLY, NULL, NULL);

            /* Software-tier verb row ("Programs and Features" style). */
            g_hInstall = CreateWindowExW(0, L"BUTTON", L"&Install",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_INSTALL, NULL, NULL);
            g_hEnable = CreateWindowExW(0, L"BUTTON", L"&Enable",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_ENABLE, NULL, NULL);
            g_hDisable = CreateWindowExW(0, L"BUTTON", L"&Disable",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_DISABLE, NULL, NULL);
            g_hUninstall = CreateWindowExW(0, L"BUTTON", L"&Uninstall",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_UNINSTALL, NULL, NULL);
            g_hRefresh = CreateWindowExW(0, L"BUTTON", L"Re&fresh",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_REFRESH, NULL, NULL);
            g_hClose = CreateWindowExW(0, L"BUTTON", L"&Close",
                WS_CHILD | WS_TABSTOP, 0, 0, 90, 26, hwnd,
                (HMENU)(INT_PTR)IDC_CLOSE, NULL, NULL);

            {
                HWND c;
                for (c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                    SendMessageW(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
                UNREFERENCED_PARAMETER(hHdr);
            }

            FillList();
            LayoutButtons(hwnd);
            return 0;
        }
        case WM_NOTIFY:
        {
            LPNMHDR nh = (LPNMHDR)lp;
            if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE)
            {
                g_tier = TabCtrl_GetCurSel(g_hTab) == 1 ? FM_TIER_SOFTWARE : FM_TIER_OS;
                Populate(NULL);
                LayoutButtons(hwnd);
            }
            else if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED)
            {
                LPNMLISTVIEW nlv = (LPNMLISTVIEW)lp;
                /* checkbox toggle on the OS tier: validate, then refresh verbs */
                if (!g_populating && !g_reverting && g_tier == FM_TIER_OS &&
                    (nlv->uChanged & LVIF_STATE) &&
                    (nlv->uNewState & LVIS_STATEIMAGEMASK) !=
                    (nlv->uOldState & LVIS_STATEIMAGEMASK) &&
                    (nlv->uOldState & LVIS_STATEIMAGEMASK) != 0)
                {
                    BOOL nowChecked =
                        ((nlv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                    OnOsCheck(nlv->iItem, nowChecked);
                }
                UpdateButtons();
            }
            else if (nh->idFrom == IDC_LIST && nh->code == NM_CLICK)
                UpdateButtons();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
                case IDC_INSTALL:   DoAction(hwnd, ACT_INSTALL);   break;
                case IDC_ENABLE:    DoAction(hwnd, ACT_ENABLE);    break;
                case IDC_DISABLE:   DoAction(hwnd, ACT_DISABLE);   break;
                case IDC_UNINSTALL: DoAction(hwnd, ACT_UNINSTALL); break;
                case IDC_REFRESH:   FillList(); break;
                case IDC_OK:        CommitOs(hwnd, TRUE); break;
                case IDC_APPLY:     CommitOs(hwnd, FALSE); break;
                case IDC_CANCEL:
                case IDC_CLOSE:     DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_DESTROY:
            if (g_arr) { FmFree(g_arr); g_arr = NULL; }
            if (g_hFont && g_hFont != GetStockObject(DEFAULT_GUI_FONT))
                DeleteObject(g_hFont);
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
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
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

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Windows Features",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 672, 520, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (IsDialogMessageW(hwnd, &m)) continue;  /* Tab/Esc/default-button nav */
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
