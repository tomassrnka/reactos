/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that CShellLink persists IPropertyStore values in the PropertyStoreDataBlock
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"

#include <stdio.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellutils.h>

static const PROPERTYKEY KeyAppId = { { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 5 };
static const PROPERTYKEY KeyToast = { { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 26 };
static const PROPERTYKEY KeyOther = { { 0x1ec4a8f0, 0x5d0e, 0x4b9c, { 0x90, 0x3f, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } }, 7 };
static const PROPERTYKEY KeyNum = { { 0x1ec4a8f0, 0x5d0e, 0x4b9c, { 0x90, 0x3f, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } }, 9 };
static const CLSID ToastClsid = { 0x3a2b1c0d, 0x4e5f, 0x6071, { 0x82, 0x93, 0xa4, 0xb5, 0xc6, 0xd7, 0xe8, 0xf9 } };

static HRESULT CreateLink(IShellLinkW **ppsl)
{
    return CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, ppsl));
}

static void SaveLink(IShellLinkW *psl, PCWSTR path)
{
    CComPtr<IPersistFile> ppf;
    HRESULT hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    ok_hex(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        hr = ppf->Save(path, TRUE);
        ok_hex(hr, S_OK);
    }
}

static void LoadLink(IShellLinkW *psl, PCWSTR path)
{
    CComPtr<IPersistFile> ppf;
    HRESULT hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    ok_hex(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        hr = ppf->Load(path, STGM_READ);
        ok_hex(hr, S_OK);
    }
}

/* The raw file must carry a data block with EXP_PROPERTYSTORAGE_SIG whose storages begin with "1SPS" */
static BOOL FileHasPropertyBlock(PCWSTR path)
{
    BYTE buf[4096];
    DWORD cb = 0, i;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(h, buf, sizeof(buf), &cb, NULL);
    CloseHandle(h);
    for (i = 0; i + 12 <= cb; i++)
    {
        if (*(DWORD *)(buf + i + 4) == 0xA0000009 && *(DWORD *)(buf + i + 12) == 0x53505331)
            return TRUE;
    }
    return FALSE;
}

static void TestPropertyRoundTrip(PCWSTR path)
{
    CComPtr<IShellLinkW> psl, psl2, psl3;
    CComPtr<IPropertyStore> pps, pps2, pps3;
    PROPVARIANT pv;
    DWORD count;
    HRESULT hr;

    hr = CreateLink(&psl);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    hr = psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;

    psl->SetPath(L"C:\\Windows\\System32\\cmd.exe");

    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)L"Nitro.Test.App";
    ok_hex(pps->SetValue(KeyAppId, pv), S_OK);
    pv.pwszVal = (LPWSTR)L"x";
    ok_hex(pps->SetValue(KeyOther, pv), S_OK);
    pv.vt = VT_CLSID;
    pv.puuid = (CLSID *)&ToastClsid;
    ok_hex(pps->SetValue(KeyToast, pv), S_OK);
    pv.vt = VT_UI4;
    pv.ulVal = 0xDEADBEEF;
    ok_hex(pps->SetValue(KeyNum, pv), S_OK);
    ok_hex(pps->Commit(), S_OK);

    SaveLink(psl, path);
    ok(FileHasPropertyBlock(path), "no property storage data block in the saved link\n");

    /* A new object reads the values back */
    hr = CreateLink(&psl2);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    LoadLink(psl2, path);
    hr = psl2->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps2));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;

    count = 0;
    ok_hex(pps2->GetCount(&count), S_OK);
    ok_dec(count, 4);

    PropVariantInit(&pv);
    ok_hex(pps2->GetValue(KeyAppId, &pv), S_OK);
    ok_dec(pv.vt, VT_LPWSTR);
    if (pv.vt == VT_LPWSTR)
        ok(!lstrcmpW(pv.pwszVal, L"Nitro.Test.App"), "AppUserModel.ID is %ls\n", pv.pwszVal);
    PropVariantClear(&pv);

    ok_hex(pps2->GetValue(KeyOther, &pv), S_OK);
    ok_dec(pv.vt, VT_LPWSTR);
    if (pv.vt == VT_LPWSTR)
        ok(!lstrcmpW(pv.pwszVal, L"x"), "other string is %ls\n", pv.pwszVal);
    PropVariantClear(&pv);

    ok_hex(pps2->GetValue(KeyToast, &pv), S_OK);
    ok_dec(pv.vt, VT_CLSID);
    if (pv.vt == VT_CLSID && pv.puuid)
        ok(IsEqualGUID(*pv.puuid, ToastClsid), "toast CLSID differs\n");
    PropVariantClear(&pv);

    ok_hex(pps2->GetValue(KeyNum, &pv), S_OK);
    ok_dec(pv.vt, VT_UI4);
    ok_hex(pv.ulVal, 0xDEADBEEF);
    PropVariantClear(&pv);

    /* Loading does not make the link dirty */
    CComPtr<IPersistFile> ppf2;
    hr = psl2->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf2));
    if (SUCCEEDED(hr))
        ok_hex(ppf2->IsDirty(), S_FALSE);

    /* Saving a loaded link keeps the values, changed ones included */
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)L"Nitro.Test.App2";
    ok_hex(pps2->SetValue(KeyAppId, pv), S_OK);
    SaveLink(psl2, path);

    hr = CreateLink(&psl3);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    LoadLink(psl3, path);
    hr = psl3->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps3));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    PropVariantInit(&pv);
    ok_hex(pps3->GetValue(KeyAppId, &pv), S_OK);
    ok_dec(pv.vt, VT_LPWSTR);
    if (pv.vt == VT_LPWSTR)
        ok(!lstrcmpW(pv.pwszVal, L"Nitro.Test.App2"), "AppUserModel.ID is %ls\n", pv.pwszVal);
    PropVariantClear(&pv);
    count = 0;
    ok_hex(pps3->GetCount(&count), S_OK);
    ok_dec(count, 4);
}

static void TestNoProperties(PCWSTR path)
{
    CComPtr<IShellLinkW> psl, psl2;
    CComPtr<IPropertyStore> pps2;
    DWORD count = 123;
    HRESULT hr = CreateLink(&psl);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    psl->SetPath(L"C:\\Windows\\System32\\cmd.exe");
    SaveLink(psl, path);
    ok(!FileHasPropertyBlock(path), "a link without properties got a property storage data block\n");

    hr = CreateLink(&psl2);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    LoadLink(psl2, path);
    hr = psl2->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps2));
    ok_hex(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        ok_hex(pps2->GetCount(&count), S_OK);
        ok_dec(count, 0);
    }
}

/* A block as another writer lays it out: values and storages are not DWORD aligned, a storage of a named
 * property comes in between, unknown types are skipped */
static void TestForeignBlock(PCWSTR path)
{
    static const GUID fmtA = { 0x11111111, 0x2222, 0x3333, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    static const GUID fmtB = { 0x44444444, 0x5555, 0x6666, { 9, 10, 11, 12, 13, 14, 15, 16 } };
    static const GUID fmtNamed = { 0xD5CDD505, 0x2E9C, 0x101B, { 0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE } };
    BYTE blk[256] = { 0 };
    BYTE *p = blk + 8;
    BYTE *storage;
    CComPtr<IShellLinkW> psl, psl2;
    CComPtr<IShellLinkDataList> pdl;
    CComPtr<IPropertyStore> pps;
    PROPVARIANT pv;
    DWORD v, count = 0;
    HRESULT hr;

    /* storage A: id 5 = VT_LPWSTR "AB"; id 6 = VT_BSTR (type this store does not keep) */
    storage = p;
    p += 4; v = 0x53505331; memcpy(p, &v, 4); p += 4; memcpy(p, &fmtA, 16); p += 16;
    v = 13 + 4 + 8; memcpy(p, &v, 4); v = 5; memcpy(p + 4, &v, 4); p[8] = 0;
    *(WORD *)(p + 9) = VT_LPWSTR; v = 3; memcpy(p + 13, &v, 4); memcpy(p + 17, L"AB", 6); p += 13 + 4 + 8;
    v = 13 + 4; memcpy(p, &v, 4); v = 6; memcpy(p + 4, &v, 4); *(WORD *)(p + 9) = VT_BSTR; p += 13 + 4;
    p += 4; /* the zero terminator */
    v = (DWORD)(p - storage); memcpy(storage, &v, 4);
    /* a storage of named properties, left alone */
    storage = p;
    p += 4; v = 0x53505331; memcpy(p, &v, 4); p += 4; memcpy(p, &fmtNamed, 16); p += 16;
    v = 9 + 4 + 13; memcpy(p, &v, 4); v = 4; memcpy(p + 4, &v, 4); p[8] = 0; memcpy(p + 9, L"N", 2); p += 9 + 4 + 13;
    p += 4;
    v = (DWORD)(p - storage); memcpy(storage, &v, 4);
    /* storage B: id 9 = VT_UI4 */
    storage = p;
    p += 4; v = 0x53505331; memcpy(p, &v, 4); p += 4; memcpy(p, &fmtB, 16); p += 16;
    v = 13 + 4; memcpy(p, &v, 4); v = 9; memcpy(p + 4, &v, 4); p[8] = 0;
    *(WORD *)(p + 9) = VT_UI4; v = 77; memcpy(p + 13, &v, 4); p += 13 + 4;
    p += 4;
    v = (DWORD)(p - storage); memcpy(storage, &v, 4);
    p += 4; /* the list terminator */
    while ((p - blk) & 3)
        p++;
    v = (DWORD)(p - blk); memcpy(blk, &v, 4); v = 0xA0000009; memcpy(blk + 4, &v, 4);

    hr = CreateLink(&psl);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    psl->SetPath(L"C:\\Windows\\System32\\cmd.exe");
    hr = psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    ok_hex(pdl->AddDataBlock(blk), S_OK);
    SaveLink(psl, path);

    hr = CreateLink(&psl2);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    LoadLink(psl2, path);
    hr = psl2->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return;
    PropVariantInit(&pv);
    PROPERTYKEY ka = { fmtA, 5 }, kb = { fmtB, 9 };
    ok_hex(pps->GetValue(ka, &pv), S_OK);
    ok_dec(pv.vt, VT_LPWSTR);
    if (pv.vt == VT_LPWSTR)
        ok(!lstrcmpW(pv.pwszVal, L"AB"), "string is %ls\n", pv.pwszVal);
    PropVariantClear(&pv);
    ok_hex(pps->GetValue(kb, &pv), S_OK);
    ok_dec(pv.vt, VT_UI4);
    ok_dec(pv.ulVal, 77);
    PropVariantClear(&pv);
    ok_hex(pps->GetCount(&count), S_OK);
    ok_dec(count, 2);
}

/* ---- malformed property blocks: the parser must not crash and must not take values it was not given ---- */

struct RAWBLK
{
    BYTE b[512];
    DWORD n;
};

static void RawPut(RAWBLK *r, const void *data, DWORD cb)
{
    memcpy(r->b + r->n, data, cb);
    r->n += cb;
}

static void RawDword(RAWBLK *r, DWORD v)
{
    RawPut(r, &v, 4);
}

static void RawBegin(RAWBLK *r)
{
    r->n = 0;
    memset(r->b, 0, sizeof(r->b));
    RawDword(r, 0); /* the size, in RawEnd */
    RawDword(r, 0xA0000009);
}

/* Opens a storage; returns its offset for RawStorageEnd. cbStorage is filled in later. */
static DWORD RawStorage(RAWBLK *r, const GUID *fmtid)
{
    DWORD ib = r->n;
    RawDword(r, 0);
    RawDword(r, 0x53505331);
    RawPut(r, fmtid, sizeof(GUID));
    return ib;
}

/* cbValue, pid, a zero byte, the type and a zero word: the 13 bytes in front of the data */
static void RawValueHead(RAWBLK *r, DWORD cbValue, DWORD pid, WORD vt)
{
    const BYTE zero[1] = { 0 };
    WORD w = 0;
    RawDword(r, cbValue);
    RawDword(r, pid);
    RawPut(r, zero, 1);
    RawPut(r, &vt, 2);
    RawPut(r, &w, 2);
}

static void RawUI4(RAWBLK *r, DWORD pid, DWORD val)
{
    RawValueHead(r, 13 + 4, pid, VT_UI4);
    RawDword(r, val);
}

static void RawStorageEnd(RAWBLK *r, DWORD ib, BOOL bTerminator, int cbOverride)
{
    DWORD cb;
    if (bTerminator)
        RawDword(r, 0);
    cb = cbOverride >= 0 ? (DWORD)cbOverride : r->n - ib;
    memcpy(r->b + ib, &cb, 4);
}

static void RawEnd(RAWBLK *r, int cbBlockOverride)
{
    DWORD cb;
    RawDword(r, 0);
    while (r->n & 3)
        r->b[r->n++] = 0;
    cb = cbBlockOverride >= 0 ? (DWORD)cbBlockOverride : r->n;
    memcpy(r->b, &cb, 4);
}

/* Feeds the block to a link and counts the properties it ends up with, directly and after a save/load. */
static void CheckRaw(PCWSTR path, const char *what, RAWBLK *r, DWORD expectCount, const PROPERTYKEY *pkMissing)
{
    CComPtr<IShellLinkW> psl, psl2;
    CComPtr<IShellLinkDataList> pdl;
    CComPtr<IPropertyStore> pps, pps2;
    DWORD count = 0xdead;
    PROPVARIANT pv;

    if (FAILED(CreateLink(&psl)))
        return;
    psl->SetPath(L"C:\\Windows\\System32\\cmd.exe");
    ok_hex(psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl)), S_OK);
    ok_hex(psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps)), S_OK);
    if (!pdl || !pps)
        return;
    pdl->AddDataBlock(r->b);
    ok_hex(pps->GetCount(&count), S_OK);
    ok(count == expectCount, "%s: count is %lu, expected %lu\n", what, count, expectCount);
    if (pkMissing)
    {
        PropVariantInit(&pv);
        ok_hex(pps->GetValue(*pkMissing, &pv), S_OK);
        ok(pv.vt == VT_EMPTY, "%s: value that must not be read has type %u\n", what, pv.vt);
        PropVariantClear(&pv);
    }

    DeleteFileW(path);
    SaveLink(psl, path);
    if (FAILED(CreateLink(&psl2)))
        return;
    LoadLink(psl2, path);
    ok_hex(psl2->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps2)), S_OK);
    count = 0xdead;
    if (pps2)
        ok_hex(pps2->GetCount(&count), S_OK);
    ok(count == expectCount, "%s: after save/load count is %lu, expected %lu\n", what, count, expectCount);
}

static void TestMalformedBlocks(PCWSTR path)
{
    static const GUID fmtA = { 0x11111111, 0x2222, 0x3333, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    static const PROPERTYKEY kA6 = { fmtA, 6 };
    RAWBLK r;
    DWORD ib, v;

    /* sanity: the builder yields a block the parser accepts (two values) */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawUI4(&r, 6, 2);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, -1);
    CheckRaw(path, "well-formed", &r, 2, NULL);

    /* truncated block: the size of the block ends inside the first storage */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, 8 + 24 + 8);
    CheckRaw(path, "truncated block", &r, 0, NULL);

    /* truncated second storage: the first one is kept */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, TRUE, -1);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 7, 3);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, r.n - 12); /* cut into the second storage's value */
    CheckRaw(path, "truncated second storage", &r, 1, NULL);

    /* a storage that claims more bytes than the block has */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, TRUE, 0x1000);
    RawEnd(&r, -1);
    CheckRaw(path, "oversized cbStorage", &r, 0, NULL);

    /* oversized cch: a VT_LPWSTR whose character count is far beyond its value; the value after it survives */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawValueHead(&r, 13 + 4 + 4, 5, VT_LPWSTR);
    RawDword(&r, 0x7FFFFFFF); /* cch */
    RawDword(&r, 0);          /* one WCHAR and padding of data */
    RawUI4(&r, 6, 2);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, -1);
    CheckRaw(path, "oversized cch", &r, 1, NULL);

    /* cch one more than the data holds */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawValueHead(&r, 13 + 4 + 4, 5, VT_LPWSTR); /* 4 bytes of cch + 4 bytes of data = 2 WCHAR */
    RawDword(&r, 3); /* cch 3 needs 6 bytes */
    RawDword(&r, 0x00410041);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, -1);
    CheckRaw(path, "cch off by one", &r, 0, NULL);

    /* cbValue below the 13 byte value header: the rest of that storage is not read */
    for (v = 0; v < 13; v += 12)
    {
        RawBegin(&r);
        ib = RawStorage(&r, &fmtA);
        RawDword(&r, v ? v : 1); /* cbValue 1, then 12 */
        RawDword(&r, 5);
        RawDword(&r, 0);
        RawDword(&r, 0);
        RawUI4(&r, 6, 2);
        RawStorageEnd(&r, ib, TRUE, -1);
        RawEnd(&r, -1);
        CheckRaw(path, v ? "cbValue 12" : "cbValue 1", &r, 0, &kA6);
    }

    /* zero cbStorage: the list ends there, a storage behind it is not read */
    RawBegin(&r);
    RawDword(&r, 0);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, -1);
    CheckRaw(path, "zero cbStorage", &r, 0, NULL);

    /* cbStorage covers only the first value: the second value is outside the storage and is not read */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, FALSE, -1); /* ends before the second value */
    RawUI4(&r, 6, 2);
    RawDword(&r, 0);
    RawEnd(&r, -1);
    CheckRaw(path, "cbStorage shorter than the values", &r, 1, &kA6);

    /* the same, followed by a zero (the end of the list for a reader that honours cbStorage) and a storage that
     * only a reader that walks to the value terminator would find */
    RawBegin(&r);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 5, 1);
    RawStorageEnd(&r, ib, FALSE, -1);
    RawDword(&r, 0);
    ib = RawStorage(&r, &fmtA);
    RawUI4(&r, 6, 2);
    RawStorageEnd(&r, ib, TRUE, -1);
    RawEnd(&r, -1);
    CheckRaw(path, "hidden storage behind a short cbStorage", &r, 1, &kA6);
}

START_TEST(CShellLinkPropertyStore)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    HRESULT hr = CoInitialize(NULL);
    ok_hex(hr, S_OK);

    GetTempPathW(_countof(dir), dir);
    PathCombineW(path, dir, L"nitro-propstore.lnk");
    DeleteFileW(path);
    TestPropertyRoundTrip(path);
    DeleteFileW(path);
    TestNoProperties(path);
    DeleteFileW(path);
    TestForeignBlock(path);
    DeleteFileW(path);
    TestMalformedBlocks(path);
    DeleteFileW(path);

    CoUninitialize();
}
