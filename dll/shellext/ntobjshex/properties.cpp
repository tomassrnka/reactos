/*
 * PROJECT:     NT Object Namespace shell extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Properties of an object, from the folder's details columns
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <commctrl.h>

#define MAX_PROPERTY_ROWS 8

struct OBJECT_PROPERTIES
{
    IUnknown *punkProcess;
    UINT cRows;
    WCHAR szName[MAX_PATH];
    WCHAR szLabel[MAX_PROPERTY_ROWS][64];
    WCHAR szValue[MAX_PROPERTY_ROWS][MAX_PATH];
};

static INT_PTR CALLBACK
ObjectPropertiesDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg != WM_INITDIALOG)
        return FALSE;

    OBJECT_PROPERTIES *pProps = (OBJECT_PROPERTIES *)((LPPROPSHEETPAGEW)lParam)->lParam;
    HWND hList = GetDlgItem(hwndDlg, IDC_OBJECTPROPERTIES);
    RECT rc;
    LVCOLUMNW lvc = { LVCF_WIDTH };

    GetClientRect(hList, &rc);
    lvc.cx = rc.right / 3;
    ListView_InsertColumn(hList, 0, &lvc);
    lvc.cx = rc.right - lvc.cx - GetSystemMetrics(SM_CXVSCROLL);
    ListView_InsertColumn(hList, 1, &lvc);
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);

    for (UINT i = 0; i < pProps->cRows; i++)
    {
        LVITEMW item = { LVIF_TEXT };
        item.iItem = i;
        item.pszText = pProps->szLabel[i];
        INT iItem = ListView_InsertItem(hList, &item);
        if (iItem >= 0)
            ListView_SetItemText(hList, iItem, 1, pProps->szValue[i]);
    }

    /* a long name or link target widens its column and scrolls instead of being cut */
    INT cxValue = lvc.cx;
    ListView_SetColumnWidth(hList, 1, LVSCW_AUTOSIZE);
    if (ListView_GetColumnWidth(hList, 1) < cxValue)
        ListView_SetColumnWidth(hList, 1, cxValue);
    return TRUE;
}

static DWORD WINAPI
ObjectPropertiesThread(LPVOID lpParameter)
{
    OBJECT_PROPERTIES *pProps = (OBJECT_PROPERTIES *)lpParameter;

    PROPSHEETPAGEW psp = { sizeof(psp) };
    psp.hInstance = g_hInstance;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_OBJECTPROPERTIES);
    psp.pfnDlgProc = ObjectPropertiesDlgProc;
    psp.lParam = (LPARAM)pProps;

    PROPSHEETHEADERW psh = { sizeof(psh) };
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_PROPTITLE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    psh.hInstance = g_hInstance;
    psh.pszCaption = pProps->szName;
    psh.nPages = 1;
    psh.ppsp = &psp;

    PropertySheetW(&psh);
    if (pProps->punkProcess)
        pProps->punkProcess->Release();
    LocalFree(pProps);
    FreeLibraryAndExitThread(g_hInstance, 0);
    return 0;
}

/* S_FALSE: not an item of this extension, the shell shows its own property sheet */
HRESULT
ShowObjectProperties(IShellFolder *psf, HWND /*hwndOwner*/, IDataObject *pdtobj)
{
    CComPtr<IPersist> pPersist;
    CComPtr<IShellFolder2> psf2;
    CLSID clsid;
    HRESULT hr;

    if (!psf || !pdtobj ||
        FAILED(psf->QueryInterface(IID_PPV_ARG(IPersist, &pPersist))) ||
        FAILED(pPersist->GetClassID(&clsid)) ||
        !IsEqualCLSID(clsid, CLSID_NtObjectFolder))
    {
        return S_FALSE;
    }

    hr = psf->QueryInterface(IID_PPV_ARG(IShellFolder2, &psf2));
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    CDataObjectHIDA cida(pdtobj);
    if (FAILED_UNEXPECTEDLY(cida.hr()))
        return cida.hr();
    if (cida->cidl != 1)
        return E_INVALIDARG;

    PCUITEMID_CHILD pidl = (PCUITEMID_CHILD)HIDA_GetPIDLItem(cida, 0);
    OBJECT_PROPERTIES *pProps = (OBJECT_PROPERTIES *)LocalAlloc(LPTR, sizeof(*pProps));
    STRRET str;
    if (!pProps)
        return E_OUTOFMEMORY;

    for (UINT iColumn = 0; pProps->cRows < MAX_PROPERTY_ROWS; iColumn++)
    {
        SHELLDETAILS header = { 0 }, value = { 0 };
        UINT iRow = pProps->cRows;

        if (FAILED(psf2->GetDetailsOf(NULL, iColumn, &header)))
            break;
        if (FAILED(StrRetToBufW(&header.str, NULL, pProps->szLabel[iRow], _countof(pProps->szLabel[0]))) ||
            FAILED(psf2->GetDetailsOf(pidl, iColumn, &value)) ||
            FAILED(StrRetToBufW(&value.str, pidl, pProps->szValue[iRow], _countof(pProps->szValue[0]))))
        {
            pProps->szValue[iRow][0] = UNICODE_NULL;
            continue;
        }
        if (pProps->szValue[iRow][0] != UNICODE_NULL)
            pProps->cRows++;
    }

    hr = psf->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &str);
    if (FAILED(hr) || FAILED(StrRetToBufW(&str, pidl, pProps->szName, _countof(pProps->szName))))
        pProps->szName[0] = UNICODE_NULL;

    /* Like the shell's own property sheets, the sheet runs in its own thread, keeps the shell process
       (SHGetInstanceExplorer, as CTF_PROCESS_REF does) and this module loaded */
    SHGetInstanceExplorer(&pProps->punkProcess);
    WCHAR szModule[MAX_PATH];
    HMODULE hModule = NULL;
    HANDLE hThread = NULL;
    if (GetModuleFileNameW(g_hInstance, szModule, _countof(szModule)))
        hModule = LoadLibraryW(szModule);
    if (hModule)
        hThread = CreateThread(NULL, 0, ObjectPropertiesThread, pProps, 0, NULL);
    if (!hThread)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (hModule)
            FreeLibrary(hModule);
        if (pProps->punkProcess)
            pProps->punkProcess->Release();
        LocalFree(pProps);
        return FAILED(hr) ? hr : E_FAIL;
    }
    CloseHandle(hThread);
    return S_OK;
}
