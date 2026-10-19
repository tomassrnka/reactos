/*
 *    Virtual Printers Folder
 *
 *    Copyright 1997                Marcus Meissner
 *    Copyright 1998, 1999, 2002    Juergen Schmied
 *    Copyright 2005                Huw Davies
 *    Copyright 2009                Andrew Hill
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <precomp.h>

#include <winspool.h>

WINE_DEFAULT_DEBUG_CHANNEL (shell);

static shvheader PrinterSFHeader[] = {
    {IDS_SHV_COLUMN_NAME, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15},
    {IDS_SHV_COLUMN_DOCUMENTS, SHCOLSTATE_TYPE_INT | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15},
    {IDS_SHV_COLUMN_STATUS, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15},
    {IDS_SHV_COLUMN_COMMENTS, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15},
    {IDS_SHV_COLUMN_LOCATION, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15},
    {IDS_SHV_COLUMN_MODEL, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 15}
};

#define COLUMN_NAME          0
#define COLUMN_DOCUMENTS     1
#define COLUMN_STATUS        2
#define COLUMN_COMMENTS      3
#define COLUMN_LOCATION      4
#define COLUMN_MODEL         5

#define PrinterSHELLVIEWCOLUMNS (6)

/**************************************************************************
 *  CPrintersExtractIconW_CreateInstane
 *
 *  There is no CPrintersExtractIconW. We just initialize CExtractIcon properly to do our job.
 */
HRESULT WINAPI CPrintersExtractIconW_CreateInstane(LPCITEMIDLIST pidl, REFIID riid, LPVOID *ppv)
{
    CComPtr<IDefaultExtractIconInit> initIcon;
    HRESULT hr = SHCreateDefaultExtractIcon(IID_PPV_ARG(IDefaultExtractIconInit,&initIcon));
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    /* FIXME: other icons for default, network, print to file */
    initIcon->SetNormalIcon(swShell32Name, -IDI_SHELL_PRINTER);

    return initIcon->QueryInterface(riid,ppv);
}

/***********************************************************************
 *     Printers folder implementation
 */

class CPrintersEnum: public CEnumIDListBase
{
    public:
        CPrintersEnum();
        ~CPrintersEnum();
        HRESULT WINAPI Initialize(HWND hwndOwner, DWORD dwFlags);
        BOOL CreatePrintersEnumList(DWORD dwFlags);

        BEGIN_COM_MAP(CPrintersEnum)
        COM_INTERFACE_ENTRY_IID(IID_IEnumIDList, IEnumIDList)
        END_COM_MAP()
};

CPrintersEnum::CPrintersEnum()
{
}

CPrintersEnum::~CPrintersEnum()
{
}

HRESULT WINAPI CPrintersEnum::Initialize(HWND hwndOwner, DWORD dwFlags)
{
    if (CreatePrintersEnumList(dwFlags) == FALSE)
        return E_FAIL;
    return S_OK;
}

static LPITEMIDLIST _ILCreatePrinterItem(PRINTER_INFO_4W *pi)
{
    PIDLDATA tmp;
    LPITEMIDLIST pidl;
    PIDLPrinterStruct * p;
    int size0 = (char*)&tmp.u.cprinter.szName - (char*)&tmp.u.cprinter;
    int size = size0;
    SIZE_T cchPrinterName = 0;
    SIZE_T cchServerName = 0;

    if (pi->pPrinterName)
        cchPrinterName = wcslen(pi->pPrinterName);
    if (pi->pServerName)
        cchServerName = wcslen(pi->pServerName);
    if ((cchPrinterName + cchServerName) > (MAXUSHORT - 2))
    {
        return NULL;
    }

    tmp.type = 0x00;
    tmp.u.cprinter.dummy = 0xFF;
    if (pi->pPrinterName)
        tmp.u.cprinter.offsServer = cchPrinterName + 1;
    else
        tmp.u.cprinter.offsServer = 1;

    size += tmp.u.cprinter.offsServer * sizeof(WCHAR);
    if (pi->pServerName)
        size += (cchServerName + 1) * sizeof(WCHAR);
    else
        size += sizeof(WCHAR);

    pidl = (LPITEMIDLIST)SHAlloc(size + 4);
    if (!pidl)
        return pidl;

    pidl->mkid.cb = size + 2;
    memcpy(pidl->mkid.abID, &tmp, 2 + size0);

    p = &((PIDLDATA*)pidl->mkid.abID)->u.cprinter;

    p->Attributes = pi->Attributes;
    if (pi->pPrinterName)
        wcscpy(p->szName, pi->pPrinterName);
    else
        p->szName[0] = L'\0';

    if (pi->pServerName)
        wcscpy(p->szName + p->offsServer, pi->pServerName);
    else
        p->szName[p->offsServer] = L'\0';

    *(WORD*)((char*)pidl + (size + 2)) = 0;
    return pidl;
}

/**************************************************************************
 *  CPrintersEnum::CreatePrintersEnumList()
 */
BOOL CPrintersEnum::CreatePrintersEnumList(DWORD dwFlags)
{
    BOOL ret = TRUE;

    TRACE("(%p)->(flags=0x%08lx) \n", this, dwFlags);

    /* enumerate the folders */
    if (dwFlags & SHCONTF_NONFOLDERS)
    {
        DWORD needed = 0, num = 0, i;
        PRINTER_INFO_4W *pi;

        EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 4, NULL, 0, &needed, &num);
        if (!needed)
            return ret;

        pi = (PRINTER_INFO_4W *)HeapAlloc(GetProcessHeap(), 0, needed);
        if(!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 4, (LPBYTE)pi, needed, &needed, &num)) {
            HeapFree(GetProcessHeap(), 0, pi);
            return FALSE;
        }

        for(i = 0; i < num; i++) {
            LPITEMIDLIST pidl = _ILCreatePrinterItem(&pi[i]);
            if (pidl)
            {
                if (!AddToEnumList(pidl))
                    SHFree(pidl);
            }
        }
        HeapFree(GetProcessHeap(), 0, pi);
    }
    return ret;
}

CPrinterFolder::CPrinterFolder()
{
    pidlRoot = NULL;
    dwAttributes = 0;
    pclsid = NULL;
}

CPrinterFolder::~CPrinterFolder()
{
    TRACE("-- destroying IShellFolder(%p)\n", this);
    if (pidlRoot)
        SHFree(pidlRoot);
}

/**************************************************************************
 *    CPrinterFolder::ParseDisplayName
 *
 * This is E_NOTIMPL in Windows too.
 */
HRESULT WINAPI CPrinterFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName,
        DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes)
{
    TRACE("(%p)->(HWND=%p,%p,%p=%s,%p,pidl=%p,%p)\n",
          this, hwndOwner, pbc, lpszDisplayName, debugstr_w(lpszDisplayName),
          pchEaten, ppidl, pdwAttributes);

    *ppidl = 0;
    if (pchEaten)
        *pchEaten = 0;

    return E_NOTIMPL;
}

static PIDLPrinterStruct * _ILGetPrinterStruct(LPCITEMIDLIST pidl)
{
    LPPIDLDATA pdata = _ILGetDataPointer(pidl);

    if (pdata && pdata->type == 0x00)
        return (PIDLPrinterStruct*) & (pdata->u.cfont);

    return NULL;
}

/**************************************************************************
 *        CPrinterFolder::EnumObjects
 */
HRESULT WINAPI CPrinterFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST * ppEnumIDList)
{
    return ShellObjectCreatorInit<CPrintersEnum>(hwndOwner, dwFlags, IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

/**************************************************************************
 *        CPrinterFolder::BindToObject
 */
HRESULT WINAPI CPrinterFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID * ppvOut)
{
    return E_NOTIMPL;
}

/**************************************************************************
 *    ISF_Printers_fnBindToStorage
 */
HRESULT WINAPI CPrinterFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID * ppvOut)
{
    FIXME ("(%p)->(pidl=%p,%p,%s,%p) stub\n",
           this, pidl, pbcReserved, shdebugstr_guid (&riid), ppvOut);

    *ppvOut = NULL;
    return E_NOTIMPL;
}

/**************************************************************************
 *     CPrinterFolder::CompareIDs
 */
HRESULT WINAPI CPrinterFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    return SHELL32_CompareDetails(this, lParam, pidl1, pidl2);
}

/**************************************************************************
 *    CPrinterFolder::CreateViewObject
 */
HRESULT WINAPI CPrinterFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID * ppvOut)
{
    CComPtr<IShellView> pShellView;
    HRESULT hr = E_INVALIDARG;

    TRACE("(%p)->(hwnd=%p,%s,%p)\n", this,
          hwndOwner, shdebugstr_guid (&riid), ppvOut);

    if (!ppvOut)
        return hr;

    *ppvOut = NULL;

    if (IsEqualIID(riid, IID_IDropTarget))
    {
        WARN("IDropTarget not implemented\n");
        hr = E_NOTIMPL;
    }
    else if(IsEqualIID(riid, IID_IContextMenu))
    {
        WARN("IContextMenu not implemented\n");
        hr = E_NOTIMPL;
    }
    else if(IsEqualIID(riid, IID_IShellView))
    {
        SFV_CREATE sfvparams = {sizeof(SFV_CREATE), this};
        hr = SHCreateShellFolderView(&sfvparams, (IShellView**)ppvOut);
    }
    TRACE ("-- (%p)->(interface=%p)\n", this, ppvOut);
    return hr;
}

/**************************************************************************
 *  CPrinterFolder::GetAttributesOf
 */
HRESULT WINAPI CPrinterFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    static const DWORD dwPrintersAttributes =
        SFGAO_HASPROPSHEET | SFGAO_STORAGEANCESTOR | SFGAO_FILESYSANCESTOR | SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_CANDELETE;
    static const DWORD dwPrinterItemAttributes = SFGAO_HASPROPSHEET | SFGAO_CANDELETE;
    HRESULT hr = S_OK;

    TRACE("(%p)->(cidl=%d apidl=%p mask=0x%08lx)\n",
          this, cidl, apidl, *rgfInOut);

    *rgfInOut &= (cidl ? dwPrinterItemAttributes : dwPrintersAttributes);

    *rgfInOut &= ~SFGAO_VALIDATE;

    TRACE ("-- result=0x%08x\n", *rgfInOut);
    return hr;
}

HRESULT CPrinterFolder::_AssocCreate(REFIID riid, PVOID *ppvObj)
{
    IQueryAssociations *pAssoc;
    HRESULT hr = ::AssocCreate(CLSID_QueryAssociations, IID_PPV_ARG(IQueryAssociations, &pAssoc));
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    hr = pAssoc->Init(ASSOCF_NONE, L"Printers", NULL, NULL);
    if (SUCCEEDED(hr))
        hr = pAssoc->QueryInterface(riid, ppvObj);

    pAssoc->Release();
    return hr;
}

/***********************************************************************
 *     Context menu of a printer item
 */

enum
{
    PRINTER_CMD_OPEN = 0,
    PRINTER_CMD_SETDEFAULT,
    PRINTER_CMD_DELETE,
    PRINTER_CMD_PROPERTIES,
    PRINTER_CMD_COUNT
};

static const struct
{
    LPCSTR pszVerbA;
    LPCWSTR pszVerbW;
} PrinterVerbs[PRINTER_CMD_COUNT] =
{
    { "open", L"open" },
    { "setdefault", L"setdefault" },
    { "delete", L"delete" },
    { "properties", L"properties" },
};

class CPrinterItemMenu :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IContextMenu
{
    private:
        CComHeapPtr<ITEMIDLIST> m_pidlItem;  /* absolute pidl of the printer */
        WCHAR m_szPrinter[MAX_PATH];

        BOOL IsDefaultPrinter();
        void ShowError(HWND hwnd, DWORD dwError);
        HRESULT RunPrintUI(HWND hwnd, LPCWSTR pszCommand);
        HRESULT SetDefault(HWND hwnd);
        HRESULT DeletePrinterItem(HWND hwnd);

    public:
        HRESULT WINAPI Initialize(PCIDLIST_ABSOLUTE pidlFolder, PCUITEMID_CHILD pidlChild, LPCWSTR pszPrinter);

        // IContextMenu
        STDMETHOD(QueryContextMenu)(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) override;
        STDMETHOD(InvokeCommand)(LPCMINVOKECOMMANDINFO lpcmi) override;
        STDMETHOD(GetCommandString)(UINT_PTR idCommand, UINT uFlags, UINT *lpReserved, LPSTR lpszName, UINT uMaxNameLen) override;

        BEGIN_COM_MAP(CPrinterItemMenu)
        COM_INTERFACE_ENTRY_IID(IID_IContextMenu, IContextMenu)
        END_COM_MAP()
};

HRESULT WINAPI CPrinterItemMenu::Initialize(PCIDLIST_ABSOLUTE pidlFolder, PCUITEMID_CHILD pidlChild, LPCWSTR pszPrinter)
{
    if (!pszPrinter || !*pszPrinter)
        return E_INVALIDARG;
    m_pidlItem.Attach(ILCombine(pidlFolder, pidlChild));
    if (!m_pidlItem)
        return E_OUTOFMEMORY;
    return StringCchCopyW(m_szPrinter, _countof(m_szPrinter), pszPrinter);
}

BOOL CPrinterItemMenu::IsDefaultPrinter()
{
    WCHAR szDefault[MAX_PATH];
    DWORD cch = _countof(szDefault);

    return GetDefaultPrinterW(szDefault, &cch) && !_wcsicmp(szDefault, m_szPrinter);
}

void CPrinterItemMenu::ShowError(HWND hwnd, DWORD dwError)
{
    WCHAR szCaption[64], szFormat[128], szSystem[256], szText[512];

    if (!LoadStringW(shell32_hInstance, IDS_PRINTERS, szCaption, _countof(szCaption)))
        szCaption[0] = UNICODE_NULL;
    if (!LoadStringW(shell32_hInstance, IDS_PRINTER_OP_FAILED, szFormat, _countof(szFormat)))
        StringCchCopyW(szFormat, _countof(szFormat), L"%s");
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dwError, 0,
                        szSystem, _countof(szSystem), NULL))
    {
        StringCchPrintfW(szSystem, _countof(szSystem), L"0x%08lx", dwError);
    }
    StringCchPrintfW(szText, _countof(szText), szFormat, szSystem);
    MessageBoxW(hwnd, szText, szCaption, MB_OK | MB_ICONERROR);
}

HRESULT CPrinterItemMenu::RunPrintUI(HWND hwnd, LPCWSTR pszCommand)
{
    WCHAR szRundll[MAX_PATH], szName[2 * MAX_PATH], szParams[2 * MAX_PATH + 64];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    PCWSTR pszSrc;
    PWSTR pszDst = szName;

    // Quote the printer name for CommandLineToArgvW: a quote becomes \" (printer names contain no backslash).
    for (pszSrc = m_szPrinter; *pszSrc; pszSrc++)
    {
        if (*pszSrc == L'"')
            *pszDst++ = L'\\';
        *pszDst++ = *pszSrc;
    }
    *pszDst = UNICODE_NULL;

    if (!GetSystemDirectoryW(szRundll, _countof(szRundll)) ||
        FAILED(StringCchCatW(szRundll, _countof(szRundll), L"\\rundll32.exe")) ||
        FAILED(StringCchPrintfW(szParams, _countof(szParams), L"printui.dll,PrintUIEntry %s /n \"%s\"",
                                pszCommand, szName)))
    {
        return E_FAIL;
    }

    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = hwnd;
    sei.lpFile = szRundll;
    sei.lpParameters = szParams;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        DWORD dwError = GetLastError();
        ShowError(hwnd, dwError);
        return HRESULT_FROM_WIN32(dwError);
    }
    return S_OK;
}

HRESULT CPrinterItemMenu::SetDefault(HWND hwnd)
{
    if (!SetDefaultPrinterW(m_szPrinter))
    {
        DWORD dwError = GetLastError();
        ShowError(hwnd, dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"windows", SMTO_ABORTIFHUNG, 1000, NULL);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, m_pidlItem, NULL);
    return S_OK;
}

HRESULT CPrinterItemMenu::DeletePrinterItem(HWND hwnd)
{
    WCHAR szCaption[64], szFormat[128], szText[MAX_PATH + 128];
    PRINTER_DEFAULTSW Defaults = { NULL, NULL, PRINTER_ALL_ACCESS };
    HANDLE hPrinter;
    DWORD dwError = ERROR_SUCCESS;

    if (!LoadStringW(shell32_hInstance, IDS_PRINTERS, szCaption, _countof(szCaption)) ||
        !LoadStringW(shell32_hInstance, IDS_PRINTER_DELETE_TEXT, szFormat, _countof(szFormat)))
    {
        return E_FAIL;
    }
    StringCchPrintfW(szText, _countof(szText), szFormat, m_szPrinter);
    if (MessageBoxW(hwnd, szText, szCaption, MB_YESNO | MB_ICONWARNING) != IDYES)
        return S_FALSE;

    if (!OpenPrinterW(m_szPrinter, &hPrinter, &Defaults))
    {
        dwError = GetLastError();
    }
    else
    {
        if (!DeletePrinter(hPrinter))
            dwError = GetLastError();
        ClosePrinter(hPrinter);
    }

    if (dwError != ERROR_SUCCESS)
    {
        ShowError(hwnd, dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

    SHChangeNotify(SHCNE_DELETE, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, m_pidlItem, NULL);
    return S_OK;
}

HRESULT WINAPI CPrinterItemMenu::QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    UINT idx = indexMenu;

    if (idCmdFirst + PRINTER_CMD_COUNT - 1 > idCmdLast)
        return E_INVALIDARG;

    _InsertMenuItemW(hMenu, idx++, TRUE, idCmdFirst + PRINTER_CMD_OPEN, MFT_STRING,
                     MAKEINTRESOURCEW(IDS_OPEN_VERB), (uFlags & CMF_NODEFAULT) ? MFS_ENABLED : MFS_DEFAULT);
    if (uFlags & CMF_DEFAULTONLY)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, PRINTER_CMD_OPEN + 1);

    if (!IsDefaultPrinter())
    {
        _InsertMenuItemW(hMenu, idx++, TRUE, 0, MFT_SEPARATOR, NULL, 0);
        _InsertMenuItemW(hMenu, idx++, TRUE, idCmdFirst + PRINTER_CMD_SETDEFAULT, MFT_STRING,
                         MAKEINTRESOURCEW(IDS_PRINTER_SETDEFAULT), MFS_ENABLED);
    }
    _InsertMenuItemW(hMenu, idx++, TRUE, 0, MFT_SEPARATOR, NULL, 0);
    _InsertMenuItemW(hMenu, idx++, TRUE, idCmdFirst + PRINTER_CMD_DELETE, MFT_STRING,
                     MAKEINTRESOURCEW(IDS_DELETE), MFS_ENABLED);
    _InsertMenuItemW(hMenu, idx++, TRUE, 0, MFT_SEPARATOR, NULL, 0);
    _InsertMenuItemW(hMenu, idx++, TRUE, idCmdFirst + PRINTER_CMD_PROPERTIES, MFT_STRING,
                     MAKEINTRESOURCEW(IDS_PROPERTIES), MFS_ENABLED);

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, PRINTER_CMD_COUNT);
}

HRESULT WINAPI CPrinterItemMenu::InvokeCommand(LPCMINVOKECOMMANDINFO lpcmi)
{
    LPCMINVOKECOMMANDINFOEX lpcmiex = (LPCMINVOKECOMMANDINFOEX)lpcmi;
    UINT idCmd = PRINTER_CMD_COUNT;
    UINT i;

    if (!lpcmi)
        return E_INVALIDARG;

    if (lpcmi->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (lpcmi->fMask & CMIC_MASK_UNICODE) && lpcmiex->lpVerbW)
    {
        if (IS_INTRESOURCE(lpcmiex->lpVerbW))
        {
            idCmd = LOWORD(lpcmiex->lpVerbW);
        }
        else
        {
            for (i = 0; i < PRINTER_CMD_COUNT; i++)
            {
                if (!_wcsicmp(lpcmiex->lpVerbW, PrinterVerbs[i].pszVerbW))
                    idCmd = i;
            }
        }
    }
    else if (IS_INTRESOURCE(lpcmi->lpVerb))
    {
        idCmd = LOWORD(lpcmi->lpVerb);
    }
    else
    {
        for (i = 0; i < PRINTER_CMD_COUNT; i++)
        {
            if (!lstrcmpiA(lpcmi->lpVerb, PrinterVerbs[i].pszVerbA))
                idCmd = i;
        }
    }

    switch (idCmd)
    {
        case PRINTER_CMD_OPEN:
            return RunPrintUI(lpcmi->hwnd, L"/o");
        case PRINTER_CMD_SETDEFAULT:
            return SetDefault(lpcmi->hwnd);
        case PRINTER_CMD_DELETE:
            return DeletePrinterItem(lpcmi->hwnd);
        case PRINTER_CMD_PROPERTIES:
            return RunPrintUI(lpcmi->hwnd, L"/p");
    }
    return E_INVALIDARG;
}

HRESULT WINAPI CPrinterItemMenu::GetCommandString(UINT_PTR idCommand, UINT uFlags, UINT *lpReserved, LPSTR lpszName, UINT uMaxNameLen)
{
    if (idCommand >= PRINTER_CMD_COUNT)
        return E_INVALIDARG;

    switch (uFlags)
    {
        case GCS_VERBA:
            return StringCchCopyA(lpszName, uMaxNameLen, PrinterVerbs[idCommand].pszVerbA);
        case GCS_VERBW:
            return StringCchCopyW((LPWSTR)lpszName, uMaxNameLen, PrinterVerbs[idCommand].pszVerbW);
        case GCS_VALIDATEA:
        case GCS_VALIDATEW:
            return S_OK;
    }
    return E_NOTIMPL;
}

/**************************************************************************
 *    CPrinterFolder::GetUIObjectOf
 *
 * PARAMETERS
 *  HWND           hwndOwner, //[in ] Parent window for any output
 *  UINT           cidl,      //[in ] array size
 *  LPCITEMIDLIST* apidl,     //[in ] simple pidl array
 *  REFIID         riid,      //[in ] Requested Interface
 *  UINT*          prgfInOut, //[   ] reserved
 *  LPVOID*        ppvObject) //[out] Resulting Interface
 *
 */
HRESULT WINAPI CPrinterFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
        REFIID riid, UINT * prgfInOut, LPVOID * ppvOut)
{
    LPVOID pObj = NULL;
    HRESULT hr = E_INVALIDARG;

    TRACE ("(%p)->(%p,%u,apidl=%p,%s,%p,%p)\n",
           this, hwndOwner, cidl, apidl, shdebugstr_guid (&riid), prgfInOut, ppvOut);

    if (!ppvOut)
        return hr;

    *ppvOut = NULL;

    if ((IsEqualIID (riid, IID_IExtractIconA) || IsEqualIID(riid, IID_IExtractIconW)) && cidl == 1)
        hr = CPrintersExtractIconW_CreateInstane(apidl[0], riid, &pObj);
    else if (IsEqualIID(riid, IID_IContextMenu) && cidl == 1)
    {
        PIDLPrinterStruct *pPrinter = _ILGetPrinterStruct(apidl[0]);
        if (pPrinter)
            hr = ShellObjectCreatorInit<CPrinterItemMenu>(pidlRoot, apidl[0], pPrinter->szName, riid, &pObj);
        else
            hr = E_INVALIDARG;
    }
    else if (IsEqualIID(riid, IID_IQueryAssociations))
        hr = _AssocCreate(riid, &pObj);
    else
        hr = E_NOINTERFACE;

    if (SUCCEEDED(hr) && !pObj)
        hr = E_OUTOFMEMORY;

    *ppvOut = pObj;
    TRACE ("(%p)->hr=0x%08lx\n", this, hr);
    return hr;
}

/**************************************************************************
 *    CPrinterFolder::GetDisplayNameOf
 *
 */
HRESULT WINAPI CPrinterFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    PIDLPrinterStruct * p;

    TRACE ("(%p)->(pidl=%p,0x%08lx,%p)\n", this, pidl, dwFlags, strRet);
    pdump (pidl);

    if (!strRet)
    {
        WARN("no strRet\n");
        return E_INVALIDARG;
    }

    p = _ILGetPrinterStruct(pidl);
    if (!p)
    {
        ERR("no printer struct\n");
        return E_INVALIDARG;
    }

    return SHSetStrRet(strRet, p->szName);
}

/**************************************************************************
 *  CPrinterFolder::SetNameOf
 *  Changes the name of a file object or subfolder, possibly changing its item
 *  identifier in the process.
 *
 * PARAMETERS
 *  HWND          hwndOwner,  //[in ] Owner window for output
 *  LPCITEMIDLIST pidl,       //[in ] simple pidl of item to change
 *  LPCOLESTR     lpszName,   //[in ] the items new display name
 *  DWORD         dwFlags,    //[in ] SHGNO formatting flags
 *  LPITEMIDLIST* ppidlOut)   //[out] simple pidl returned
 */
HRESULT WINAPI CPrinterFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl,    /* simple pidl */
        LPCOLESTR lpName, DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    FIXME("(%p)->(%p,pidl=%p,%s,%lu,%p)\n", this, hwndOwner, pidl,
          debugstr_w (lpName), dwFlags, pPidlOut);

    return E_FAIL;
}

HRESULT WINAPI CPrinterFolder::GetDefaultSearchGUID(GUID *pguid)
{
    FIXME("(%p)\n", this);
    return E_NOTIMPL;
}

HRESULT WINAPI CPrinterFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    FIXME("(%p)\n", this);
    return E_NOTIMPL;
}

HRESULT WINAPI CPrinterFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    return E_NOTIMPL; // Not required when column 0 is our default.
}

HRESULT WINAPI CPrinterFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    if (!pcsFlags || iColumn >= PrinterSHELLVIEWCOLUMNS)
        return E_INVALIDARG;
    *pcsFlags = PrinterSFHeader[iColumn].colstate;
    return S_OK;

}

HRESULT WINAPI CPrinterFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    FIXME("(%p): stub\n", this);

    return E_NOTIMPL;
}

HRESULT WINAPI CPrinterFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    TRACE("(%p)->(%p %i %p): stub\n", this, pidl, iColumn, psd);

    if (iColumn >= PrinterSHELLVIEWCOLUMNS)
        return E_NOTIMPL;

    psd->fmt = PrinterSFHeader[iColumn].fmt;
    psd->cxChar = PrinterSFHeader[iColumn].cxChar;
    if (pidl == NULL)
        return SHSetStrRet(&psd->str, PrinterSFHeader[iColumn].colnameid);

    if (iColumn == COLUMN_NAME)
        return GetDisplayNameOf(pidl, SHGDN_NORMAL, &psd->str);

    psd->str.uType = STRRET_CSTR;
    psd->str.cStr[0] = '\0';

    return E_NOTIMPL;
}

HRESULT WINAPI CPrinterFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    FIXME ("(%p): stub\n", this);
    return E_NOTIMPL;
}

/************************************************************************
 *    CPrinterFolder::GetClassID
 */
HRESULT WINAPI CPrinterFolder::GetClassID(CLSID *lpClassId)
{
    TRACE ("(%p)\n", this);

    *lpClassId = CLSID_Printers;

    return S_OK;
}

/************************************************************************
 *    CPrinterFolder::Initialize
 */
HRESULT WINAPI CPrinterFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (pidlRoot)
        SHFree((LPVOID)pidlRoot);

    pidlRoot = ILClone(pidl);
    return S_OK;
}

/**************************************************************************
 *    CPrinterFolder::GetCurFolder
 */
HRESULT WINAPI CPrinterFolder::GetCurFolder(PIDLIST_ABSOLUTE * pidl)
{
    TRACE ("(%p)->(%p)\n", this, pidl);

    if (pidlRoot)
        return SHILClone(pidlRoot, pidl);
    *pidl = NULL;
    return S_FALSE;
}
