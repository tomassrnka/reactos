/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for SHBindToFolderIDListParent(Ex), SHCreateItemWithParent and ILLoadFromStreamEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"
#include <shobjidl.h>
#include <shellutils.h>
#include <strsafe.h>

typedef HRESULT (WINAPI *FN_SHBindToFolderIDListParent)(IShellFolder *, PCUIDLIST_RELATIVE, REFIID, void **,
                                                         PCUITEMID_CHILD *);
typedef HRESULT (WINAPI *FN_SHBindToFolderIDListParentEx)(IShellFolder *, PCUIDLIST_RELATIVE, IBindCtx *, REFIID,
                                                           void **, PCUITEMID_CHILD *);
typedef HRESULT (WINAPI *FN_SHCreateItemWithParent)(PCIDLIST_ABSOLUTE, IShellFolder *, PCUITEMID_CHILD, REFIID,
                                                     void **);
typedef HRESULT (WINAPI *FN_ILLoadFromStreamEx)(IStream *, PIDLIST_RELATIVE *);

static FN_SHBindToFolderIDListParent pSHBindToFolderIDListParent;
static FN_SHBindToFolderIDListParentEx pSHBindToFolderIDListParentEx;
static FN_SHCreateItemWithParent pSHCreateItemWithParent;
static FN_ILLoadFromStreamEx pILLoadFromStreamEx;

#define POISON ((void *)(ULONG_PTR)1)

static WCHAR s_szSystem[MAX_PATH], s_szWindows[MAX_PATH];

static PIDLIST_ABSOLUTE ParsePath(PCWSTR pszPath)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    HRESULT hr = SHParseDisplayName(pszPath, NULL, &pidl, 0, NULL);
    ok(hr == S_OK, "SHParseDisplayName('%S'): 0x%lx\n", pszPath, hr);
    return pidl;
}

static void CheckChildPath(IShellFolder *psf, PCUITEMID_CHILD pidlChild, PCWSTR pszExpected, PCSTR what)
{
    WCHAR szName[MAX_PATH] = L"";
    STRRET sr;
    HRESULT hr = psf->GetDisplayNameOf(pidlChild, SHGDN_FORPARSING, &sr);
    ok(hr == S_OK, "%s: GetDisplayNameOf 0x%lx\n", what, hr);
    if (SUCCEEDED(hr))
        StrRetToBufW(&sr, pidlChild, szName, _countof(szName));
    ok(!lstrcmpiW(szName, pszExpected), "%s: '%S', expected '%S'\n", what, szName, pszExpected);
}

static void CheckItemPath(IShellItem *psi, PCWSTR pszExpected, PCSTR what)
{
    PWSTR pszName = NULL;
    HRESULT hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszName);
    ok(hr == S_OK, "%s: GetDisplayName 0x%lx\n", what, hr);
    ok(pszName && !lstrcmpiW(pszName, pszExpected), "%s: '%S', expected '%S'\n", what, pszName, pszExpected);
    CoTaskMemFree(pszName);
}

static void TestBindToParent(void)
{
    PIDLIST_ABSOLUTE pidl = ParsePath(s_szSystem);
    CComPtr<IShellFolder> psfDesktop;
    IShellFolder *psf;
    IUnknown *punk;
    PCUITEMID_CHILD pidlLast;
    HRESULT hr;

    if (!pidl)
        return;
    SHGetDesktopFolder(&psfDesktop);

    /* NULL root: relative to the desktop */
    psf = (IShellFolder *)POISON;
    pidlLast = (PCUITEMID_CHILD)POISON;
    hr = pSHBindToFolderIDListParent(NULL, pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
    ok_hr(hr, S_OK);
    ok(pidlLast == ILFindLastID(pidl), "pidlLast %p, expected %p\n", pidlLast, ILFindLastID(pidl));
    if (SUCCEEDED(hr))
    {
        CheckChildPath(psf, pidlLast, s_szSystem, "NULL root");
        psf->Release();
    }

    /* An explicit root, and no last item wanted */
    psf = (IShellFolder *)POISON;
    hr = pSHBindToFolderIDListParent(psfDesktop, pidl, IID_PPV_ARG(IShellFolder, &psf), NULL);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        CheckChildPath(psf, ILFindLastID(pidl), s_szSystem, "desktop root");
        psf->Release();
    }

    /* Another interface of the parent */
    punk = (IUnknown *)POISON;
    hr = pSHBindToFolderIDListParent(NULL, pidl, IID_IPersist, (void **)&punk, &pidlLast);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
        punk->Release();

    /* An interface folders do not have */
    punk = (IUnknown *)POISON;
    hr = pSHBindToFolderIDListParent(NULL, pidl, IID_IShellLinkW, (void **)&punk, &pidlLast);
    ok(FAILED(hr), "IShellLinkW: hr 0x%lx\n", hr);
    ok(punk == NULL, "IShellLinkW: %p\n", punk);

    psf = (IShellFolder *)POISON;
    pidlLast = (PCUITEMID_CHILD)POISON;
    hr = pSHBindToFolderIDListParent(NULL, NULL, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
    ok_hr(hr, E_INVALIDARG);
    ok(psf == NULL, "NULL list: %p\n", psf);
    ok(pidlLast == NULL, "NULL list: pidlLast %p\n", pidlLast);

    if (pSHBindToFolderIDListParentEx)
    {
        CComPtr<IBindCtx> pbc;

        psf = (IShellFolder *)POISON;
        pidlLast = (PCUITEMID_CHILD)POISON;
        hr = pSHBindToFolderIDListParentEx(NULL, pidl, NULL, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
        ok_hr(hr, S_OK);
        ok(pidlLast == ILFindLastID(pidl), "Ex: pidlLast %p, expected %p\n", pidlLast, ILFindLastID(pidl));
        if (SUCCEEDED(hr))
        {
            CheckChildPath(psf, pidlLast, s_szSystem, "Ex");
            psf->Release();
        }

        ok_hr(CreateBindCtx(0, &pbc), S_OK);
        psf = (IShellFolder *)POISON;
        hr = pSHBindToFolderIDListParentEx(psfDesktop, pidl, pbc, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr))
        {
            CheckChildPath(psf, pidlLast, s_szSystem, "Ex with bind context");
            psf->Release();
        }

        psf = (IShellFolder *)POISON;
        pidlLast = (PCUITEMID_CHILD)POISON;
        hr = pSHBindToFolderIDListParentEx(NULL, NULL, NULL, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
        ok_hr(hr, E_INVALIDARG);
        ok(psf == NULL, "Ex NULL list: %p\n", psf);
        ok(pidlLast == NULL, "Ex NULL list: pidlLast %p\n", pidlLast);
    }
    else
    {
        skip("SHBindToFolderIDListParentEx is not exported\n");
    }

    ILFree(pidl);
}

static void TestCreateItemWithParent(void)
{
    PIDLIST_ABSOLUTE pidl = ParsePath(s_szSystem), pidlWindows = ParsePath(s_szWindows);
    CComPtr<IShellFolder> psfDesktop, psfWindows;
    IShellItem *psi;
    IUnknown *punk;
    HRESULT hr;

    if (!pidl || !pidlWindows)
        goto Cleanup;
    SHGetDesktopFolder(&psfDesktop);
    hr = psfDesktop->BindToObject(pidlWindows, NULL, IID_PPV_ARG(IShellFolder, &psfWindows));
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        goto Cleanup;

    /* The parent as a folder */
    psi = (IShellItem *)POISON;
    hr = pSHCreateItemWithParent(NULL, psfWindows, ILFindLastID(pidl), IID_PPV_ARG(IShellItem, &psi));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        CheckItemPath(psi, s_szSystem, "folder parent");
        psi->Release();
    }

    /* The parent as an ID list and a folder */
    psi = (IShellItem *)POISON;
    hr = pSHCreateItemWithParent(pidlWindows, psfWindows, ILFindLastID(pidl), IID_PPV_ARG(IShellItem, &psi));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        CheckItemPath(psi, s_szSystem, "ID list and folder parent");
        psi->Release();
    }

    /* No parent at all */
    psi = (IShellItem *)POISON;
    hr = pSHCreateItemWithParent(NULL, NULL, pidl, IID_PPV_ARG(IShellItem, &psi));
    ok_hr(hr, E_INVALIDARG);
    ok(psi == NULL, "no parent: %p\n", psi);

    psi = (IShellItem *)POISON;
    hr = pSHCreateItemWithParent(NULL, psfWindows, NULL, IID_PPV_ARG(IShellItem, &psi));
    ok_hr(hr, E_INVALIDARG);
    ok(psi == NULL, "NULL child: %p\n", psi);

    /* Any interface of the item */
    punk = (IUnknown *)POISON;
    hr = pSHCreateItemWithParent(NULL, psfWindows, ILFindLastID(pidl), IID_IShellFolder, (void **)&punk);
    ok_hr(hr, E_NOINTERFACE);
    ok(punk == NULL, "IShellFolder: %p\n", punk);

    punk = (IUnknown *)POISON;
    hr = pSHCreateItemWithParent(NULL, psfWindows, ILFindLastID(pidl), IID_IPersistIDList, (void **)&punk);
    ok_hr(hr, S_OK);
    ok(punk && punk != POISON, "IPersistIDList: %p\n", punk);
    if (SUCCEEDED(hr) && punk && punk != POISON)
        punk->Release();

Cleanup:
    ILFree(pidl);
    ILFree(pidlWindows);
}

static void TestLoadFromStreamEx(void)
{
    PIDLIST_ABSOLUTE pidl = ParsePath(s_szSystem);
    PIDLIST_RELATIVE pidlOut;
    CComPtr<IStream> pstm;
    LARGE_INTEGER liZero = {};
    ULARGE_INTEGER uliPos;
    WORD cbZero = 0;
    HRESULT hr;

    if (!pidl)
        return;

    /* What ILSaveToStream writes */
    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pstm), S_OK);
    ok_hr(ILSaveToStream(pstm, pidl), S_OK);
    pstm->Seek(liZero, STREAM_SEEK_SET, NULL);
    pidlOut = (PIDLIST_RELATIVE)POISON;
    hr = pILLoadFromStreamEx(pstm, &pidlOut);
    ok_hr(hr, S_OK);
    ok(pidlOut && pidlOut != POISON && ILIsEqual(pidlOut, pidl), "loaded list differs\n");
    if (SUCCEEDED(hr))
        ILFree(pidlOut);
    pstm->Seek(liZero, STREAM_SEEK_CUR, &uliPos);
    ok(uliPos.QuadPart == sizeof(WORD) + ILGetSize(pidl), "position %lu, expected %u\n",
       (ULONG)uliPos.QuadPart, (UINT)(sizeof(WORD) + ILGetSize(pidl)));

    /* At the end of the stream */
    pidlOut = (PIDLIST_RELATIVE)POISON;
    hr = pILLoadFromStreamEx(pstm, &pidlOut);
    ok_hr(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    ok(pidlOut == NULL, "at the end: %p\n", pidlOut);
    pstm.Release();

    /* A zero size */
    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pstm), S_OK);
    pstm->Write(&cbZero, sizeof(cbZero), NULL);
    pstm->Seek(liZero, STREAM_SEEK_SET, NULL);
    pidlOut = (PIDLIST_RELATIVE)POISON;
    hr = pILLoadFromStreamEx(pstm, &pidlOut);
    ok_hr(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    ok(pidlOut == NULL, "zero size: %p\n", pidlOut);

    ILFree(pidl);
}

START_TEST(SHBindToFolderIDListParent)
{
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");

    pSHBindToFolderIDListParent = (FN_SHBindToFolderIDListParent)GetProcAddress(hShell32, "SHBindToFolderIDListParent");
    pSHBindToFolderIDListParentEx = (FN_SHBindToFolderIDListParentEx)GetProcAddress(hShell32, "SHBindToFolderIDListParentEx");
    pSHCreateItemWithParent = (FN_SHCreateItemWithParent)GetProcAddress(hShell32, "SHCreateItemWithParent");
    pILLoadFromStreamEx = (FN_ILLoadFromStreamEx)GetProcAddress(hShell32, "ILLoadFromStreamEx");

    CoInitialize(NULL);
    GetSystemDirectoryW(s_szSystem, _countof(s_szSystem));
    GetWindowsDirectoryW(s_szWindows, _countof(s_szWindows));
    {
        /* Compare with the spelling the shell uses */
        PIDLIST_ABSOLUTE pidl = ParsePath(s_szSystem);
        if (pidl)
            SHGetPathFromIDListW(pidl, s_szSystem);
        ILFree(pidl);
    }

    if (pSHBindToFolderIDListParent)
        TestBindToParent();
    else
        skip("SHBindToFolderIDListParent is not exported\n");
    if (pSHCreateItemWithParent)
        TestCreateItemWithParent();
    else
        skip("SHCreateItemWithParent is not exported\n");
    if (pILLoadFromStreamEx)
        TestLoadFromStreamEx();
    else
        skip("ILLoadFromStreamEx is not exported\n");

    CoUninitialize();
}
