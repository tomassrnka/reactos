/*
 * Fonts folder
 *
 * Copyright 2008       Johannes Anderwald <johannes.anderwald@reactos.org>
 * Copyright 2009       Andrew Hill
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <precomp.h>

WINE_DEFAULT_DEBUG_CHANNEL (shell);

CFontsFolder::CFontsFolder()
{
    m_pidlInner = NULL;
}

CFontsFolder::~CFontsFolder()
{
    if(m_pidlInner)
        SHFree(m_pidlInner);
}

HRESULT WINAPI CFontsFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName,
        ULONG *pchEaten, PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes)
{
    return m_pisfInner->ParseDisplayName(hwndOwner, pbc, lpszDisplayName, pchEaten, ppidl, pdwAttributes);
}

HRESULT WINAPI CFontsFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    return m_pisfInner->EnumObjects(hwndOwner, dwFlags, ppEnumIDList);
}

HRESULT WINAPI CFontsFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    return m_pisfInner->BindToObject(pidl, pbcReserved, riid, ppvOut);
}

HRESULT WINAPI CFontsFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    return m_pisfInner->BindToStorage(pidl, pbcReserved, riid, ppvOut);
}

HRESULT WINAPI CFontsFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    return m_pisfInner->CompareIDs(lParam, pidl1, pidl2);
}

HRESULT WINAPI CFontsFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    return m_pisfInner->CreateViewObject(hwndOwner, riid, ppvOut);
}

HRESULT WINAPI CFontsFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    static const DWORD dwFontsAttributes =
        SFGAO_STORAGE | SFGAO_STORAGEANCESTOR | SFGAO_FILESYSANCESTOR |
        SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_HASSUBFOLDER;

    if(cidl)
        return m_pisfInner->GetAttributesOf(cidl, apidl, rgfInOut);

    if (!rgfInOut || !apidl)
        return E_INVALIDARG;

    if (*rgfInOut == 0)
        *rgfInOut = ~0;

    *rgfInOut &= dwFontsAttributes;

    /* make sure SFGAO_VALIDATE is cleared, some apps depend on that */
    *rgfInOut &= ~SFGAO_VALIDATE;

    return S_OK;
}

HRESULT WINAPI CFontsFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
        REFIID riid, UINT * prgfInOut, LPVOID * ppvOut)
{
    return m_pisfInner->GetUIObjectOf(hwndOwner, cidl, apidl, riid, prgfInOut, ppvOut);
}

HRESULT WINAPI CFontsFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    if (!strRet || !pidl)
        return E_INVALIDARG;

    /* If we got an fs item just forward to the fs folder */
    if (!_ILIsSpecialFolder(pidl))
        return m_pisfInner->GetDisplayNameOf(pidl, dwFlags, strRet);

    /* The caller wants our path. Let fs folder handle it */
    if ((GET_SHGDN_RELATION (dwFlags) == SHGDN_NORMAL) &&
        (GET_SHGDN_FOR (dwFlags) & SHGDN_FORPARSING))
    {
        /* Give an empty pidl to the fs folder to tell us its path */
        if (pidl->mkid.cb)
            pidl = ILGetNext(pidl);

        /* The font extension cannot name itself from an empty pidl */
        if (_ILIsEmpty(pidl))
        {
            WCHAR szPath[MAX_PATH];
            HRESULT hr = SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, szPath);
            if (FAILED(hr))
                return hr;
            return SHSetStrRet(strRet, szPath);
        }

        return m_pisfInner->GetDisplayNameOf(pidl, dwFlags, strRet);
    }

    ERR("Got empty pidl without SHGDN_FORPARSING\n");
    return E_INVALIDARG;
}

HRESULT WINAPI CFontsFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl,    /* simple pidl */
        LPCOLESTR lpName, DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    return m_pisfInner->SetNameOf(hwndOwner, pidl, lpName, dwFlags, pPidlOut);
}

HRESULT WINAPI CFontsFolder::GetDefaultSearchGUID(GUID *pguid)
{
    return m_pisfInner->GetDefaultSearchGUID(pguid);
}

HRESULT WINAPI CFontsFolder::EnumSearches(IEnumExtraSearch ** ppenum)
{
    return m_pisfInner->EnumSearches(ppenum);
}

HRESULT WINAPI CFontsFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    return m_pisfInner->GetDefaultColumn(dwRes, pSort, pDisplay);
}

HRESULT WINAPI CFontsFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    return m_pisfInner->GetDefaultColumnState(iColumn, pcsFlags);
}

HRESULT WINAPI CFontsFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    return m_pisfInner->GetDetailsEx(pidl, pscid, pv);
}

HRESULT WINAPI CFontsFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    return m_pisfInner->GetDetailsOf(pidl, iColumn, psd);
}

HRESULT WINAPI CFontsFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    return m_pisfInner->MapColumnToSCID(column, pscid);
}

HRESULT WINAPI CFontsFolder::GetClassID(CLSID *lpClassId)
{
    if (!lpClassId)
        return E_POINTER;

    *lpClassId = CLSID_FontsFolderShortcut;

    return S_OK;
}

/* Bind the class named by the fonts directory's desktop.ini (the font shell
 * extension) on the directory's own pidl, as CFSFolder::BindToObject does when
 * the directory is reached by path, so that both routes show the same view. */
HRESULT CFontsFolder::CreateDesktopIniFolder()
{
    WCHAR szPath[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, szPath);
    if (FAILED(hr))
        return hr;

    DWORD dwAttributes = GetFileAttributesW(szPath);
    if (dwAttributes == INVALID_FILE_ATTRIBUTES ||
        !(dwAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !(dwAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)))
    {
        return E_FAIL;
    }

    CLSID clsidFolder;
    hr = SHELL32_GetCLSIDForDirectory(szPath, L"CLSID", &clsidFolder);
    if (FAILED(hr))
        return hr;
    if (IsEqualCLSID(clsidFolder, CLSID_FontsFolderShortcut))
        return E_FAIL;

    CComHeapPtr<ITEMIDLIST> pidlFonts;
    hr = SHGetFolderLocation(NULL, CSIDL_FONTS, NULL, 0, &pidlFonts);
    if (FAILED(hr))
        return hr;

    CComPtr<IShellFolder2> psf;
    hr = SHCoCreateInstance(NULL, &clsidFolder, NULL, IID_PPV_ARG(IShellFolder2, &psf));
    if (FAILED(hr))
        return hr;

    CComPtr<IPersistFolder3> ppf3;
    CComPtr<IPersistFolder> ppf;
    if (SUCCEEDED(psf->QueryInterface(IID_PPV_ARG(IPersistFolder3, &ppf3))))
    {
        PERSIST_FOLDER_TARGET_INFO pfti = { 0 };
        pfti.dwAttributes = -1;
        pfti.csidl = CSIDL_FONTS;
        StringCchCopyW(pfti.szTargetParsingName, _countof(pfti.szTargetParsingName), szPath);
        hr = ppf3->InitializeEx(NULL, pidlFonts, &pfti);
    }
    else if (SUCCEEDED(psf->QueryInterface(IID_PPV_ARG(IPersistFolder, &ppf))))
    {
        hr = ppf->Initialize(pidlFonts);
    }
    else
    {
        hr = E_NOINTERFACE;
    }
    if (FAILED(hr))
        return hr;

    m_pisfInner = psf;
    return S_OK;
}

HRESULT WINAPI CFontsFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    m_pidlInner = ILClone(pidl);
    if (!m_pidlInner)
        return E_OUTOFMEMORY;

    if (SUCCEEDED(CreateDesktopIniFolder()))
        return S_OK;

    return SHELL32_CoCreateInitSF(m_pidlInner,
                                  &CLSID_ShellFSFolder,
                                  CSIDL_FONTS,
                                  IID_PPV_ARG(IShellFolder2, &m_pisfInner));
}

HRESULT WINAPI CFontsFolder::GetCurFolder(PIDLIST_ABSOLUTE *pidl)
{
    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(m_pidlInner);
    return S_OK;
}
