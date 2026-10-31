/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for d3d12.dll: the library loads, it has the exports of Windows 10, and without
 *              a Direct3D 12 adapter D3D12CreateDevice fails with DXGI_ERROR_UNSUPPORTED
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations are those of Windows 10 22H2. The machine that they were measured on has an adapter
 * (the Basic Render Driver), where D3D12CreateDevice succeeds, and the test then skips what depends on
 * that. A system without an adapter, and the stub of ReactOS, give DXGI_ERROR_UNSUPPORTED.
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#include <windows.h>

#define MY_DXGI_ERROR_UNSUPPORTED           ((HRESULT)0x887A0004)
#define MY_DXGI_ERROR_SDK_COMPONENT_MISSING ((HRESULT)0x887A002D)
#define FL_11_0 0xb000

typedef HRESULT (WINAPI *PFN_CREATE_DEVICE)(void *, UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_GET_DEBUG)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_ENABLE_EXP)(UINT, const GUID *, void *, UINT *);
typedef HRESULT (WINAPI *PFN_GET_IFACE)(const GUID *, REFIID, void **);
typedef HRESULT (WINAPI *PFN_SER_ROOT)(const void *, UINT, void **, void **);

/* IID_ID3D12Device */
static const GUID s_IidDevice = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
/* IID_ID3D12Debug */
static const GUID s_IidDebug = { 0x344488b7, 0x6846, 0x474b, { 0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0 } };
static const GUID s_Bogus = { 0x11223344, 0x5566, 0x7788, { 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01 } };

static const struct { const char *Name; WORD Ordinal; } s_Exports[] =
{
    { "GetBehaviorValue", 100 },
    { "D3D12CreateDevice", 101 },
    { "D3D12GetDebugInterface", 102 },
    { "SetAppCompatStringPointer", 103 },
    { "D3D12CoreCreateLayeredDevice", 104 },
    { "D3D12CoreGetLayeredDeviceSize", 105 },
    { "D3D12CoreRegisterLayers", 106 },
    { "D3D12CreateRootSignatureDeserializer", 107 },
    { "D3D12CreateVersionedRootSignatureDeserializer", 108 },
    { "D3D12EnableExperimentalFeatures", 110 },
    { "D3D12GetInterface", 111 },
    { "D3D12PIXEventsReplaceBlock", 112 },
    { "D3D12PIXGetThreadInfo", 113 },
    { "D3D12PIXNotifyWakeFromFenceSignal", 114 },
    { "D3D12PIXReportCounter", 115 },
    { "D3D12SerializeRootSignature", 116 },
    { "D3D12SerializeVersionedRootSignature", 117 },
};

static void ReleaseUnknown(void *p)
{
    if (p)
    {
        IUnknown *unk = p;
        unk->lpVtbl->Release(unk);
    }
}

START_TEST(d3d12)
{
    HMODULE hMod;
    PFN_CREATE_DEVICE pCreateDevice;
    PFN_GET_DEBUG pGetDebug;
    PFN_ENABLE_EXP pEnableExp;
    PFN_GET_IFACE pGetIface;
    PFN_SER_ROOT pSerRoot;
    void *pv;
    HRESULT hr;
    UINT i;
    BOOL HaveAdapter;

    hMod = LoadLibraryExW(L"d3d12.dll", NULL, 0);
    ok(hMod != NULL, "LoadLibrary(d3d12.dll) failed, %lu\n", GetLastError());
    if (!hMod)
        return;

    for (i = 0; i < _countof(s_Exports); i++)
    {
        FARPROC byName = GetProcAddress(hMod, s_Exports[i].Name);
        FARPROC byOrdinal = GetProcAddress(hMod, (LPCSTR)(ULONG_PTR)s_Exports[i].Ordinal);
        ok(byName != NULL, "%s is not exported\n", s_Exports[i].Name);
        ok(byName == byOrdinal, "%s: ordinal %u gives %p, the name %p\n",
           s_Exports[i].Name, s_Exports[i].Ordinal, byOrdinal, byName);
    }

    pCreateDevice = (PFN_CREATE_DEVICE)GetProcAddress(hMod, "D3D12CreateDevice");
    pGetDebug = (PFN_GET_DEBUG)GetProcAddress(hMod, "D3D12GetDebugInterface");
    pEnableExp = (PFN_ENABLE_EXP)GetProcAddress(hMod, "D3D12EnableExperimentalFeatures");
    pGetIface = (PFN_GET_IFACE)GetProcAddress(hMod, "D3D12GetInterface");
    pSerRoot = (PFN_SER_ROOT)GetProcAddress(hMod, "D3D12SerializeRootSignature");
    if (!pCreateDevice || !pGetDebug || !pEnableExp || !pGetIface || !pSerRoot)
        return;

    /* A level below 11_0 is not valid for Direct3D 12 */
    pv = (void *)1;
    hr = pCreateDevice(NULL, 0, &s_IidDevice, &pv);
    ok_hex(hr, E_INVALIDARG);
    ok(pv == NULL, "ppDevice is %p after a failure\n", pv);
    pv = (void *)1;
    hr = pCreateDevice(NULL, 0x9300, &s_IidDevice, &pv);
    ok_hex(hr, E_INVALIDARG);
    ok(pv == NULL, "ppDevice is %p after a failure\n", pv);

    /* The device: an adapter makes it, no adapter is DXGI_ERROR_UNSUPPORTED */
    pv = (void *)1;
    hr = pCreateDevice(NULL, FL_11_0, &s_IidDevice, &pv);
    HaveAdapter = SUCCEEDED(hr);
    if (HaveAdapter)
    {
        skip("This system has a Direct3D 12 adapter (D3D12CreateDevice gave 0x%08lx)\n", hr);
        ReleaseUnknown(pv);
    }
    else
    {
        ok_hex(hr, MY_DXGI_ERROR_UNSUPPORTED);
        ok(pv == NULL, "ppDevice is %p after a failure\n", pv);
    }

    /* The capability check without an output: S_FALSE with an adapter */
    hr = pCreateDevice(NULL, FL_11_0, &s_IidDevice, NULL);
    if (HaveAdapter)
        ok_hex(hr, S_FALSE);
    else
        ok_hex(hr, MY_DXGI_ERROR_UNSUPPORTED);

    /* No graphics tools */
    pv = (void *)1;
    hr = pGetDebug(&s_IidDebug, &pv);
    if (hr == S_OK)
        ReleaseUnknown(pv);
    else
    {
        ok_hex(hr, MY_DXGI_ERROR_SDK_COMPONENT_MISSING);
        ok(pv == NULL, "ppvDebug is %p after a failure\n", pv);
    }

    ok_hex(pEnableExp(0, NULL, NULL, NULL), S_OK);
    ok_hex(pEnableExp(1, &s_Bogus, NULL, NULL), E_NOINTERFACE);

    /* Windows leaves *ppv alone here */
    pv = NULL;
    ok_hex(pGetIface(&s_Bogus, &s_IidDevice, &pv), E_NOINTERFACE);

    /* The root signature functions work on Windows without an adapter, and are not implemented here */
    {
        struct { UINT NumParameters; void *pParameters; UINT NumStaticSamplers; void *pStaticSamplers; UINT Flags; }
            Empty = { 0 };
        void *pBlob = (void *)1, *pErr = (void *)1;

        hr = pSerRoot(&Empty, 1, &pBlob, &pErr);
        ok(hr == S_OK || hr == E_NOTIMPL, "D3D12SerializeRootSignature gave 0x%08lx\n", hr);
        if (hr == S_OK)
            ReleaseUnknown(pBlob);
        else
            ok(pBlob == NULL, "ppBlob is %p after a failure\n", pBlob);

        pBlob = (void *)1;
        hr = pSerRoot(NULL, 1, &pBlob, &pErr);
        ok_hex(hr, E_INVALIDARG);
    }

    FreeLibrary(hMod);
}
