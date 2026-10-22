/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the Control Panel Fonts folder shortcut
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"
#include <shellutils.h>
#include <stdlib.h>

typedef WCHAR NAME[MAX_PATH];

static int __cdecl CompareNames(const void *p1, const void *p2)
{
    return lstrcmpiW((LPCWSTR)p1, (LPCWSTR)p2);
}

static HRESULT ParseName(LPCWSTR pszName, IShellFolder *psfDesktop, LPITEMIDLIST *ppidl)
{
    return psfDesktop->ParseDisplayName(NULL, NULL, const_cast<LPWSTR>(pszName), NULL, ppidl, NULL);
}

/* Returns the sorted names (free with free()), or NULL on failure */
static NAME *GetSortedNames(IShellFolder *psf, UINT *pcNames)
{
    *pcNames = 0;
    CComPtr<IEnumIDList> penum;
    HRESULT hr = psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum);
    ok(hr == S_OK, "EnumObjects: hr = 0x%lx\n", hr);
    if (hr != S_OK)
        return NULL;

    UINT cNames = 0, cMax = 64;
    NAME *Names = (NAME *)malloc(cMax * sizeof(NAME));
    if (!Names)
        return NULL;

    LPITEMIDLIST pidl;
    while ((hr = penum->Next(1, &pidl, NULL)) == S_OK)
    {
        if (cNames == cMax)
        {
            NAME *NewNames = (NAME *)realloc(Names, 2 * cMax * sizeof(NAME));
            if (!NewNames)
            {
                ILFree(pidl);
                free(Names);
                return NULL;
            }
            Names = NewNames;
            cMax *= 2;
        }

        STRRET sr;
        Names[cNames][0] = UNICODE_NULL;
        hr = psf->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr);
        ok(hr == S_OK, "GetDisplayNameOf: hr = 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = StrRetToBufW(&sr, pidl, Names[cNames], MAX_PATH);
            ok(hr == S_OK, "StrRetToBufW: hr = 0x%lx\n", hr);
        }
        ILFree(pidl);
        ++cNames;
    }
    ok(hr == S_FALSE, "IEnumIDList::Next: hr = 0x%lx\n", hr);

    qsort(Names, cNames, sizeof(NAME), CompareNames);
    *pcNames = cNames;
    return Names;
}

START_TEST(CFontsFolder)
{
    HRESULT hr = CoInitialize(NULL);
    ok(hr == S_OK, "CoInitialize: hr = 0x%lx\n", hr);

    WCHAR szFonts[MAX_PATH];
    hr = SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, szFonts);
    ok(hr == S_OK, "SHGetFolderPathW: hr = 0x%lx\n", hr);

    CComPtr<IShellFolder> psfDesktop;
    hr = SHGetDesktopFolder(&psfDesktop);
    ok(hr == S_OK, "SHGetDesktopFolder: hr = 0x%lx\n", hr);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }

    /* Windows 7 and later have no Fonts item in the Control Panel namespace */
    CComHeapPtr<ITEMIDLIST> pidlShortcut;
    hr = ParseName(L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}\\::{D20EA4E1-3957-11d2-A40B-0C5020524152}",
                   psfDesktop, &pidlShortcut);
    if (FAILED(hr))
    {
        skip("The Control Panel has no Fonts folder shortcut (hr = 0x%lx)\n", hr);
        psfDesktop.Release();
        CoUninitialize();
        return;
    }

    CComHeapPtr<ITEMIDLIST> pidlPath;
    hr = ParseName(szFonts, psfDesktop, &pidlPath);
    ok(hr == S_OK, "Parsing '%S': hr = 0x%lx\n", szFonts, hr);

    CComPtr<IShellFolder> psfShortcut, psfPath;
    hr = psfDesktop->BindToObject(pidlShortcut, NULL, IID_PPV_ARG(IShellFolder, &psfShortcut));
    ok(hr == S_OK, "Binding the Fonts folder shortcut: hr = 0x%lx\n", hr);
    if (pidlPath)
    {
        hr = psfDesktop->BindToObject(pidlPath, NULL, IID_PPV_ARG(IShellFolder, &psfPath));
        ok(hr == S_OK, "Binding '%S': hr = 0x%lx\n", szFonts, hr);
    }

    if (psfShortcut && psfPath)
    {
        UINT cPath, cShortcut;
        NAME *PathNames = GetSortedNames(psfPath, &cPath);
        NAME *ShortcutNames = GetSortedNames(psfShortcut, &cShortcut);
        ok(PathNames && ShortcutNames, "Enumeration failed\n");
        ok(cPath > 0, "No items in '%S'\n", szFonts);

        /* Opened by path, the fonts directory is the font shell extension (desktop.ini);
         * the Control Panel shortcut must show the same items */
        ok(cShortcut == cPath, "Item count: shortcut %u, path %u\n", cShortcut, cPath);
        if (PathNames && ShortcutNames)
        {
            UINT cDiffer = 0, iFirst = 0;
            for (UINT i = 0; i < min(cPath, cShortcut); ++i)
            {
                if (lstrcmpiW(PathNames[i], ShortcutNames[i]) != 0 && cDiffer++ == 0)
                    iFirst = i;
            }
            ok(cDiffer == 0, "%u of %u names differ, first: shortcut '%S', path '%S'\n", cDiffer,
               min(cPath, cShortcut), cDiffer ? ShortcutNames[iFirst] : L"", cDiffer ? PathNames[iFirst] : L"");
        }
        free(PathNames);
        free(ShortcutNames);
    }

    psfPath.Release();
    psfShortcut.Release();
    psfDesktop.Release();
    CoUninitialize();
}
