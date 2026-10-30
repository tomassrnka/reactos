/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for FolderItems::_NewEnum (the IEnumVARIANT for-each walks)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"
#include <shldisp.h>
#include <strsafe.h>

static WCHAR g_Dir[MAX_PATH];

static void MakeFiles(void)
{
    WCHAR Path[MAX_PATH];
    int i;

    GetTempPathW(MAX_PATH, g_Dir);
    StringCchPrintfW(g_Dir + lstrlenW(g_Dir), MAX_PATH - lstrlenW(g_Dir), L"folderitems%lu", GetCurrentProcessId());
    CreateDirectoryW(g_Dir, NULL);
    for (i = 1; i <= 3; i++)
    {
        StringCchPrintfW(Path, MAX_PATH, L"%s\\file%d", g_Dir, i);
        HANDLE File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (File != INVALID_HANDLE_VALUE)
            CloseHandle(File);
    }
}

static void DeleteFiles(void)
{
    WCHAR Path[MAX_PATH];
    int i;

    for (i = 1; i <= 3; i++)
    {
        StringCchPrintfW(Path, MAX_PATH, L"%s\\file%d", g_Dir, i);
        DeleteFileW(Path);
    }
    RemoveDirectoryW(g_Dir);
}

/* Name of the FolderItem in a VT_DISPATCH variant, "" when it is none */
static void ItemName(const VARIANT *pVar, WCHAR *Name)
{
    FolderItem *pItem = NULL;
    BSTR bs = NULL;

    Name[0] = 0;
    if (V_VT(pVar) != VT_DISPATCH || !V_DISPATCH(pVar))
        return;
    if (SUCCEEDED(V_DISPATCH(pVar)->QueryInterface(IID_FolderItem, (void **)&pItem)))
    {
        if (SUCCEEDED(pItem->get_Name(&bs)) && bs)
            lstrcpynW(Name, bs, 32);
        SysFreeString(bs);
        pItem->Release();
    }
}

static void TestEnum(FolderItems *pItems)
{
    IUnknown *pUnk = NULL;
    IEnumVARIANT *pEnum = NULL, *pClone = NULL;
    VARIANT Var[4];
    WCHAR Name[32];
    ULONG Fetched;
    HRESULT hr;
    long Count = -1;
    int i;

    hr = pItems->get_Count(&Count);
    ok_long(hr, S_OK);
    ok_long(Count, 3);

    hr = pItems->_NewEnum(&pUnk);
    ok_long(hr, S_OK);
    if (!pUnk)
        return;
    hr = pUnk->QueryInterface(IID_IEnumVARIANT, (void **)&pEnum);
    ok_long(hr, S_OK);
    pUnk->Release();
    if (!pEnum)
        return;

    /* one at a time, in the order of the folder */
    for (i = 1; i <= 3; i++)
    {
        WCHAR Expected[16];
        StringCchPrintfW(Expected, 16, L"file%d", i);
        VariantInit(&Var[0]);
        Fetched = 99;
        hr = pEnum->Next(1, Var, &Fetched);
        ok_long(hr, S_OK);
        ok_long(Fetched, 1);
        ok_int(V_VT(&Var[0]), VT_DISPATCH);
        ItemName(&Var[0], Name);
        ok(!lstrcmpiW(Name, Expected), "item %d is \"%ls\", expected \"%ls\"\n", i, Name, Expected);
        VariantClear(&Var[0]);
    }
    VariantInit(&Var[0]);
    Fetched = 99;
    hr = pEnum->Next(1, Var, &Fetched);
    ok_long(hr, S_FALSE);
    ok_long(Fetched, 0);

    /* Reset, then all of them in one call */
    hr = pEnum->Reset();
    ok_long(hr, S_OK);
    for (i = 0; i < 4; i++)
        VariantInit(&Var[i]);
    Fetched = 99;
    hr = pEnum->Next(3, Var, &Fetched);
    ok_long(hr, S_OK);
    ok_long(Fetched, 3);
    for (i = 0; i < 3; i++)
        VariantClear(&Var[i]);

    /* asking for more than there are is a S_FALSE (what comes with it is not specified: Windows
     * returns nothing at all, the documented contract is the three) */
    pEnum->Reset();
    for (i = 0; i < 4; i++)
        VariantInit(&Var[i]);
    Fetched = 99;
    hr = pEnum->Next(4, Var, &Fetched);
    ok_long(hr, S_FALSE);
    ok(Fetched <= 3, "fetched %lu\n", Fetched);
    for (i = 0; i < 4; i++)
        VariantClear(&Var[i]);

    /* Skip, and a clone (Windows restarts the clone, the contract says it continues: only that
     * it works and yields items is checked) */
    pEnum->Reset();
    hr = pEnum->Skip(1);
    ok_long(hr, S_OK);
    hr = pEnum->Clone(&pClone);
    ok_long(hr, S_OK);
    VariantInit(&Var[0]);
    hr = pEnum->Next(1, Var, &Fetched);
    ok_long(hr, S_OK);
    ItemName(&Var[0], Name);
    ok(!lstrcmpiW(Name, L"file2"), "after Skip: \"%ls\"\n", Name);
    VariantClear(&Var[0]);
    if (pClone)
    {
        VariantInit(&Var[0]);
        hr = pClone->Next(1, Var, &Fetched);
        ok_long(hr, S_OK);
        ItemName(&Var[0], Name);
        ok(!lstrcmpiW(Name, L"file1") || !lstrcmpiW(Name, L"file2"), "clone: \"%ls\"\n", Name);
        VariantClear(&Var[0]);
        pClone->Release();
    }

    /* a NULL fetched count is for one element only */
    pEnum->Reset();
    VariantInit(&Var[0]);
    hr = pEnum->Next(1, Var, NULL);
    ok_long(hr, S_OK);
    VariantClear(&Var[0]);
    pEnum->Release();
}

/* What for-each does: Invoke(DISPID_NEWENUM) on the collection */
static void TestNewEnumDispatch(FolderItems *pItems)
{
    IDispatch *pDisp = NULL;
    IUnknown *pUnk = NULL;
    IEnumVARIANT *pEnum = NULL;
    DISPPARAMS Params = { NULL, NULL, 0, 0 };
    VARIANT Result;
    HRESULT hr;

    hr = pItems->QueryInterface(IID_IDispatch, (void **)&pDisp);
    ok_long(hr, S_OK);
    if (!pDisp)
        return;
    VariantInit(&Result);
    hr = pDisp->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                       &Params, &Result, NULL, NULL);
    ok_long(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        pUnk = V_VT(&Result) == VT_UNKNOWN ? V_UNKNOWN(&Result) : (V_VT(&Result) == VT_DISPATCH ? V_DISPATCH(&Result) : NULL);
        ok(pUnk != NULL, "DISPID_NEWENUM returned vt %d\n", V_VT(&Result));
        if (pUnk)
        {
            hr = pUnk->QueryInterface(IID_IEnumVARIANT, (void **)&pEnum);
            ok_long(hr, S_OK);
            if (pEnum)
                pEnum->Release();
        }
    }
    VariantClear(&Result);
    pDisp->Release();
}

START_TEST(CFolderItems)
{
    IShellDispatch *pSD = NULL;
    Folder *pFolder = NULL;
    FolderItems *pItems = NULL;
    VARIANT vDir;
    HRESULT hr;

    CoInitialize(NULL);
    hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_ALL, IID_IShellDispatch, (void **)&pSD);
    if (FAILED(hr))
    {
        skip("Shell.Application is not available: 0x%08lx\n", hr);
        CoUninitialize();
        return;
    }

    MakeFiles();
    V_VT(&vDir) = VT_BSTR;
    V_BSTR(&vDir) = SysAllocString(g_Dir);
    hr = pSD->NameSpace(vDir, &pFolder);
    ok_long(hr, S_OK);
    VariantClear(&vDir);
    if (pFolder)
    {
        hr = pFolder->Items(&pItems);
        ok_long(hr, S_OK);
        if (pItems)
        {
            TestEnum(pItems);
            TestNewEnumDispatch(pItems);
            pItems->Release();
        }
        pFolder->Release();
    }
    DeleteFiles();

    pSD->Release();
    CoUninitialize();
}
