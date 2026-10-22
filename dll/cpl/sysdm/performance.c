/*
 * PROJECT:     ReactOS System Control Panel Applet
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * FILE:        dll/cpl/sysdm/performance.c
 * PURPOSE:     Performance settings property sheets
 * COPYRIGHT:   Copyright 2024 Whindmar Saksit <whindsaks@proton.me>
 *
 */

#include "precomp.h"
#include <shlguid_undoc.h> // CLSID_CRegTreeOptions
#include <shlobj_undoc.h> // IRegTreeOptions
#include <shlwapi_undoc.h> // SHSendMessageBroadcastW

static VOID
RegTreeOpt_OnDestroy(IRegTreeOptions *pRTO)
{
    if (pRTO)
    {
        IRegTreeOptions_WalkTree(pRTO, WALK_TREE_DESTROY);
        IRegTreeOptions_Release(pRTO);
    }
}

static BOOL
RegTreeOpt_ToggleCheckItem(IRegTreeOptions *pRTO, HTREEITEM hItem)
{
    return pRTO && SUCCEEDED(IRegTreeOptions_ToggleItem(pRTO, hItem));
}

static BOOL
RegTreeOpt_OnTreeViewClick(IRegTreeOptions *pRTO, HWND hWndTree)
{
    TV_HITTESTINFO HitTest;
    DWORD dwPos = GetMessagePos();
    HitTest.pt.x = GET_X_LPARAM(dwPos);
    HitTest.pt.y = GET_Y_LPARAM(dwPos);
    ScreenToClient(hWndTree, &HitTest.pt);
    HTREEITEM hItem = TreeView_HitTest(hWndTree, &HitTest);
    return RegTreeOpt_ToggleCheckItem(pRTO, hItem);
}

static BOOL
RegTreeOpt_OnTreeViewKeyDown(IRegTreeOptions *pRTO, HWND hWndTree, TV_KEYDOWN *pKey)
{
    if (pKey->wVKey == VK_SPACE)
        return RegTreeOpt_ToggleCheckItem(pRTO, TreeView_GetSelection(hWndTree));
    else if (pKey->wVKey == VK_F5 && pRTO)
        IRegTreeOptions_WalkTree(pRTO, WALK_TREE_REFRESH);
    return FALSE;
}

static INT_PTR CALLBACK
VisualEffectsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    IRegTreeOptions *pRTO = (IRegTreeOptions*)GetWindowLongPtrW(hDlg, DWLP_USER);
    HWND hWndTree = GetDlgItem(hDlg, IDC_TREE);

    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            IRegTreeOptions *pRTO = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_CRegTreeOptions, NULL, CLSCTX_INPROC_SERVER,
                                           &IID_IRegTreeOptions, (void**)&pRTO)))
            {
                LPCSTR pszPath = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects";
                SetWindowLongPtrW(hDlg, DWLP_USER, (LPARAM)pRTO);
                IRegTreeOptions_InitTree(pRTO, hWndTree, HKEY_LOCAL_MACHINE, pszPath, NULL);
            }
            else
            {
                // Without IRegTreeOptions, our page is pointless so we remove it.
                // MSDN says we have to post the remove message from inside WM_INITDIALOG.
                HWND hWndPS = GetParent(hDlg);
                int iPage = PropSheet_HwndToIndex(hWndPS, hDlg);
                PostMessage(hWndPS, PSM_REMOVEPAGE, 0, SendMessageW(hWndPS, PSM_INDEXTOPAGE, iPage, 0));
            }
            return TRUE;
        }

        case WM_DESTROY:
            RegTreeOpt_OnDestroy(pRTO);
            SetWindowLongPtrW(hDlg, DWLP_USER, (LPARAM)NULL);
            break;

        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code)
            {
                case NM_CLICK:
                    if (RegTreeOpt_OnTreeViewClick(pRTO, hWndTree))
                        PropSheet_Changed(GetParent(hDlg), hDlg);
                    break;

                case TVN_KEYDOWN:
                    if (RegTreeOpt_OnTreeViewKeyDown(pRTO, hWndTree, (TV_KEYDOWN*)lParam))
                        PropSheet_Changed(GetParent(hDlg), hDlg);
                    break;

                case PSN_APPLY:
                {
                    if (pRTO)
                        IRegTreeOptions_WalkTree(pRTO, WALK_TREE_SAVE);

                    SHSendMessageBroadcastW(WM_SETTINGCHANGE, 0, 0); // For the ListviewShadow setting
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

#define PRIORITY_CONTROL_KEY L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl"
#define PRIORITY_SEPARATION_VALUE L"Win32PrioritySeparation"
/* Short variable quantums with the maximum foreground boost, and long fixed quantums without a boost */
#define PRIORITY_SEPARATION_PROGRAMS 0x26
#define PRIORITY_SEPARATION_BACKGROUND 0x18

static BOOL
IsServerProductType(VOID)
{
    HKEY hKey;
    WCHAR szType[16] = L"";
    DWORD cbType = sizeof(szType) - sizeof(WCHAR), dwType = REG_NONE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"ProductType", NULL, &dwType,
                             (LPBYTE)szType, &cbType) != ERROR_SUCCESS)
        {
            dwType = REG_NONE;
        }
        RegCloseKey(hKey);
    }

    /* As in the kernel: only a type starting with "Wi" (WinNT) is a workstation */
    return !(dwType == REG_SZ && szType[0] == L'W' && szType[1] == L'i');
}

/* Quantum fields of 0 or 3 mean the product default: long fixed on a server, short variable otherwise */
static BOOL
IsProgramsPrioritySeparation(DWORD dwValue)
{
    DWORD dwType = (dwValue >> 2) & 3, dwLength = (dwValue >> 4) & 3;
    BOOL bServer = IsServerProductType();
    BOOL bVariable = (dwType == 1) || ((dwType == 0 || dwType == 3) && !bServer);
    BOOL bShort = (dwLength == 2) || ((dwLength == 0 || dwLength == 3) && !bServer);

    return bVariable && bShort;
}

static VOID
AdvancedPerf_OnInitDialog(HWND hDlg)
{
    HKEY hKey;
    DWORD dwValue = 0, cbValue = sizeof(dwValue), dwType;
    BOOL bCanWrite = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PRIORITY_CONTROL_KEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        bCanWrite = TRUE;
    }
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PRIORITY_CONTROL_KEY, 0,
                           KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
    {
        hKey = NULL;
    }

    if (hKey)
    {
        if (RegQueryValueExW(hKey, PRIORITY_SEPARATION_VALUE, NULL, &dwType,
                             (LPBYTE)&dwValue, &cbValue) != ERROR_SUCCESS ||
            dwType != REG_DWORD || cbValue != sizeof(dwValue))
        {
            dwValue = 0;
        }
        RegCloseKey(hKey);
    }

    CheckRadioButton(hDlg, IDC_CPUCLIENT, IDC_CPUSERVER,
                     IsProgramsPrioritySeparation(dwValue) ? IDC_CPUCLIENT : IDC_CPUSERVER);

    if (!bCanWrite)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_CPUCLIENT), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_CPUSERVER), FALSE);
    }
}

static BOOL
AdvancedPerf_OnApply(HWND hDlg)
{
    HKEY hKey;
    LONG lError;
    DWORD dwValue = (IsDlgButtonChecked(hDlg, IDC_CPUCLIENT) == BST_CHECKED) ?
                    PRIORITY_SEPARATION_PROGRAMS : PRIORITY_SEPARATION_BACKGROUND;

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PRIORITY_CONTROL_KEY, 0, KEY_SET_VALUE, &hKey);
    if (lError == ERROR_SUCCESS)
    {
        lError = RegSetValueExW(hKey, PRIORITY_SEPARATION_VALUE, 0, REG_DWORD,
                                (const BYTE *)&dwValue, sizeof(dwValue));
        RegCloseKey(hKey);
    }
    if (lError != ERROR_SUCCESS)
    {
        SetLastError(lError);
        ShowLastWin32Error(hDlg);
        return FALSE;
    }
    return TRUE;
}

static INT_PTR CALLBACK
AdvancedDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
            AdvancedPerf_OnInitDialog(hDlg);
            SetWindowLongPtrW(hDlg, DWLP_USER, FALSE);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_CPUCLIENT:
                case IDC_CPUSERVER:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        SetWindowLongPtrW(hDlg, DWLP_USER, TRUE);
                        PropSheet_Changed(GetParent(hDlg), hDlg);
                    }
                    break;

                case IDC_CHANGESWAP:
                    DialogBoxW(hApplet, MAKEINTRESOURCEW(IDD_VIRTMEM), hDlg, VirtMemDlgProc);
                    break;
            }
            break;

        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->code == PSN_APPLY)
            {
                if (GetWindowLongPtrW(hDlg, DWLP_USER))
                {
                    if (!AdvancedPerf_OnApply(hDlg))
                    {
                        SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                        return TRUE;
                    }
                    SetWindowLongPtrW(hDlg, DWLP_USER, FALSE);
                }
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

VOID
ShowPerformanceOptions(HWND hDlg)
{
    HRESULT hrInit = CoInitialize(NULL); // For IRegTreeOptions
    PROPSHEETHEADERW psh = { sizeof(psh), PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP };
    PROPSHEETPAGEW pages[2] = { 0 };

    pages[0].dwSize = sizeof(*pages);
    pages[0].hInstance = hApplet;
    pages[0].pszTemplate = MAKEINTRESOURCEW(IDD_VISUALEFFECTS);
    pages[0].pfnDlgProc = VisualEffectsDlgProc;

    pages[1].dwSize = sizeof(*pages);
    pages[1].hInstance = hApplet;
    pages[1].pszTemplate = MAKEINTRESOURCEW(IDD_ADVANCEDPERF);
    pages[1].pfnDlgProc = AdvancedDlgProc;

    psh.hwndParent = hDlg;
    psh.hInstance = hApplet;
    psh.pszCaption = MAKEINTRESOURCEW(IDS_PERFORMANCEOPTIONS);
    psh.nPages = _countof(pages);
    psh.ppsp = pages;

    PropertySheetW(&psh);

    if (SUCCEEDED(hrInit))
        CoUninitialize();
}
