/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for IDXGIFactory4::EnumWarpAdapter
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64) without a graphics driver. There DXGI lists two adapters,
 * both the Microsoft Basic Render Driver (1414:008c): the first has flags 0, the second is WARP, with a
 * LUID of its own and DXGI_ADAPTER_FLAG_SOFTWARE. EnumWarpAdapter returns a new object for the second
 * one on every call, never the first adapter, whatever the interface asked for (IDXGIAdapter, 1, 4).
 * What the list of EnumAdapters1 holds is not checked: ReactOS lists one adapter, Windows lists WARP last.
 * No adapter of that machine has an output (EnumOutputs fails with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE).
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#define COBJMACROS
#include <dxgi1_6.h>

/* what the adapters of the Windows machine answer: it has no display, and no output to list */
#define MY_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887A0022)

static const GUID s_Bogus = { 0x11223344, 0x5566, 0x7788, { 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01 } };
static const WCHAR s_Brd[] = L"Microsoft Basic Render Driver";

typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter *, UINT, HMODULE, UINT, const UINT *, UINT, UINT,
                                                  void **, UINT *, void **);

static BOOL SameLuid(LUID A, LUID B)
{
    return A.LowPart == B.LowPart && A.HighPart == B.HighPart;
}

static void TestWarpAdapter(IDXGIFactory4 *Factory, IDXGIFactory1 *Factory1)
{
    IDXGIAdapter1 *Warp = NULL, *Warp2 = NULL, *Adapter, *ByLuid = NULL;
    IDXGIAdapter *Plain = NULL;
    IDXGIAdapter2 *Warp2If = NULL;
    IDXGIAdapter4 *Warp4 = NULL;
    IDXGIFactory *Parent = NULL;
    IDXGIOutput *Output = NULL;
    IUnknown *Unk = NULL, *Unk2 = NULL, *Junk;
    DXGI_ADAPTER_DESC1 Desc1, OtherDesc;
    DXGI_ADAPTER_DESC2 Desc2;
    DXGI_ADAPTER_DESC3 Desc3;
    DXGI_ADAPTER_DESC Desc0;
    LARGE_INTEGER Umd;
    HRESULT hr;
    UINT i;

    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IDXGIAdapter1, (void **)&Warp);
    ok(hr == S_OK, "EnumWarpAdapter returned %08lx\n", hr);
    if (hr != S_OK || !Warp)
    {
        skip("no WARP adapter\n");
        return;
    }

    memset(&Desc1, 0xcc, sizeof(Desc1));
    hr = IDXGIAdapter1_GetDesc1(Warp, &Desc1);
    ok(hr == S_OK, "GetDesc1 returned %08lx\n", hr);
    ok(wcscmp(Desc1.Description, s_Brd) == 0, "Description is '%ls'\n", Desc1.Description);
    ok(Desc1.VendorId == 0x1414, "VendorId is %04x\n", Desc1.VendorId);
    ok(Desc1.DeviceId == 0x008c, "DeviceId is %04x\n", Desc1.DeviceId);
    ok(Desc1.SubSysId == 0, "SubSysId is %08x\n", Desc1.SubSysId);
    ok(Desc1.Revision == 0, "Revision is %u\n", Desc1.Revision);
    ok(Desc1.DedicatedVideoMemory == 0, "DedicatedVideoMemory is %Iu\n", (SIZE_T)Desc1.DedicatedVideoMemory);
    ok(Desc1.DedicatedSystemMemory == 0, "DedicatedSystemMemory is %Iu\n", (SIZE_T)Desc1.DedicatedSystemMemory);
    ok(Desc1.SharedSystemMemory != 0, "SharedSystemMemory is 0\n");
    ok(Desc1.Flags == DXGI_ADAPTER_FLAG_SOFTWARE, "Flags are %#x\n", Desc1.Flags);
    ok(Desc1.AdapterLuid.LowPart != 0 || Desc1.AdapterLuid.HighPart != 0, "the LUID is empty\n");

    hr = IDXGIAdapter1_GetDesc(Warp, &Desc0);
    ok(hr == S_OK, "GetDesc returned %08lx\n", hr);
    ok(wcscmp(Desc0.Description, s_Brd) == 0, "Description of GetDesc is '%ls'\n", Desc0.Description);
    ok(Desc0.VendorId == 0x1414 && Desc0.DeviceId == 0x008c, "GetDesc ids are %04x:%04x\n",
       Desc0.VendorId, Desc0.DeviceId);
    ok(SameLuid(Desc0.AdapterLuid, Desc1.AdapterLuid), "GetDesc LUID differs from GetDesc1\n");

    hr = IDXGIAdapter1_QueryInterface(Warp, &IID_IDXGIAdapter2, (void **)&Warp2If);
    ok(hr == S_OK, "QueryInterface(IDXGIAdapter2) returned %08lx\n", hr);
    if (Warp2If)
    {
        memset(&Desc2, 0xcc, sizeof(Desc2));
        hr = IDXGIAdapter2_GetDesc2(Warp2If, &Desc2);
        ok(hr == S_OK, "GetDesc2 returned %08lx\n", hr);
        ok(Desc2.Flags == DXGI_ADAPTER_FLAG_SOFTWARE, "GetDesc2 flags are %#x\n", Desc2.Flags);
        ok(Desc2.GraphicsPreemptionGranularity == DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY,
           "graphics preemption is %d\n", Desc2.GraphicsPreemptionGranularity);
        ok(Desc2.ComputePreemptionGranularity == DXGI_COMPUTE_PREEMPTION_INSTRUCTION_BOUNDARY,
           "compute preemption is %d\n", Desc2.ComputePreemptionGranularity);
        ok(SameLuid(Desc2.AdapterLuid, Desc1.AdapterLuid), "GetDesc2 LUID differs from GetDesc1\n");
        IDXGIAdapter2_Release(Warp2If);
    }

    hr = IDXGIAdapter1_QueryInterface(Warp, &IID_IDXGIAdapter4, (void **)&Warp4);
    ok(hr == S_OK, "QueryInterface(IDXGIAdapter4) returned %08lx\n", hr);
    if (Warp4)
    {
        memset(&Desc3, 0xcc, sizeof(Desc3));
        hr = IDXGIAdapter4_GetDesc3(Warp4, &Desc3);
        ok(hr == S_OK, "GetDesc3 returned %08lx\n", hr);
        /* Windows adds the Direct3D 12 capabilities (0x2e), which ReactOS does not claim */
        ok(Desc3.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE, "GetDesc3 flags are %#x\n", Desc3.Flags);
        ok(Desc3.GraphicsPreemptionGranularity == DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY,
           "graphics preemption is %d\n", Desc3.GraphicsPreemptionGranularity);
        ok(SameLuid(Desc3.AdapterLuid, Desc1.AdapterLuid), "GetDesc3 LUID differs from GetDesc1\n");
        IDXGIAdapter4_Release(Warp4);
    }

    /* it is not any adapter of the list, neither in object nor in LUID */
    for (i = 0; IDXGIFactory1_EnumAdapters1(Factory1, i, &Adapter) == S_OK; i++)
    {
        ok(Adapter != Warp, "adapter %u is the object of EnumWarpAdapter\n", i);
        hr = IDXGIAdapter1_GetDesc1(Adapter, &OtherDesc);
        ok(hr == S_OK, "GetDesc1 of adapter %u returned %08lx\n", i, hr);
        if (!(OtherDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            ok(!SameLuid(OtherDesc.AdapterLuid, Desc1.AdapterLuid), "adapter %u has the LUID of WARP\n", i);
        IDXGIAdapter1_Release(Adapter);
    }
    ok(i >= 1, "no adapter listed\n");

    /* a new object on every call, with the same identity */
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IDXGIAdapter1, (void **)&Warp2);
    ok(hr == S_OK, "second EnumWarpAdapter returned %08lx\n", hr);
    ok(Warp2 != Warp, "EnumWarpAdapter returned the same object twice\n");
    if (Warp2)
    {
        hr = IDXGIAdapter1_GetDesc1(Warp2, &OtherDesc);
        ok(hr == S_OK && SameLuid(OtherDesc.AdapterLuid, Desc1.AdapterLuid), "the second object has another LUID\n");
        IDXGIAdapter1_Release(Warp2);
    }

    /* other interfaces */
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IDXGIAdapter, (void **)&Plain);
    ok(hr == S_OK, "EnumWarpAdapter(IDXGIAdapter) returned %08lx\n", hr);
    if (Plain)
    {
        hr = IDXGIAdapter_GetDesc(Plain, &Desc0);
        ok(hr == S_OK && SameLuid(Desc0.AdapterLuid, Desc1.AdapterLuid), "IDXGIAdapter has another LUID\n");
        IDXGIAdapter_Release(Plain);
    }
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IUnknown, (void **)&Unk);
    ok(hr == S_OK && Unk != NULL, "EnumWarpAdapter(IUnknown) returned %08lx\n", hr);
    hr = IDXGIAdapter1_QueryInterface(Warp, &IID_IUnknown, (void **)&Unk2);
    ok(hr == S_OK && Unk2 != NULL, "QueryInterface(IUnknown) returned %08lx\n", hr);
    ok(Unk2 == (IUnknown *)Warp, "the identity differs from the interface\n");
    if (Unk)
        IUnknown_Release(Unk);
    if (Unk2)
        IUnknown_Release(Unk2);

    /* the factory is the parent */
    hr = IDXGIAdapter1_GetParent(Warp, &IID_IDXGIFactory, (void **)&Parent);
    ok(hr == S_OK, "GetParent returned %08lx\n", hr);
    ok(Parent == (IDXGIFactory *)Factory1, "the parent is not the factory\n");
    if (Parent)
        IDXGIFactory_Release(Parent);

    /* it drives no output, and it can be asked what an adapter is asked */
    hr = IDXGIAdapter1_EnumOutputs(Warp, 0, &Output);
    ok(hr == DXGI_ERROR_NOT_FOUND || hr == MY_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "EnumOutputs returned %08lx\n", hr);
    /* what a device can be asked of the first adapter is asked of WARP */
    hr = IDXGIAdapter1_CheckInterfaceSupport(Warp, &IID_IDXGIDevice, &Umd);
    if (IDXGIFactory1_EnumAdapters1(Factory1, 0, &Adapter) == S_OK)
    {
        HRESULT hrFirst = IDXGIAdapter1_CheckInterfaceSupport(Adapter, &IID_IDXGIDevice, &Umd);
        ok(hr == hrFirst, "CheckInterfaceSupport returned %08lx, %08lx for the first adapter\n", hr, hrFirst);
        IDXGIAdapter1_Release(Adapter);
    }

    /* it is found by its LUID */
    hr = IDXGIFactory4_EnumAdapterByLuid(Factory, Desc1.AdapterLuid, &IID_IDXGIAdapter1, (void **)&ByLuid);
    ok(hr == S_OK, "EnumAdapterByLuid(WARP) returned %08lx\n", hr);
    if (ByLuid)
    {
        hr = IDXGIAdapter1_GetDesc1(ByLuid, &OtherDesc);
        ok(hr == S_OK && SameLuid(OtherDesc.AdapterLuid, Desc1.AdapterLuid), "EnumAdapterByLuid found another adapter\n");
        ok(wcscmp(OtherDesc.Description, s_Brd) == 0, "found '%ls'\n", OtherDesc.Description);
        IDXGIAdapter1_Release(ByLuid);
    }

    /* failures */
    Junk = (IUnknown *)(ULONG_PTR)0x1234;
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IDXGIFactory, (void **)&Junk);
    ok(hr == E_NOINTERFACE, "EnumWarpAdapter(IDXGIFactory) returned %08lx\n", hr);
    ok(Junk == NULL, "the output is %p\n", Junk);
    Junk = (IUnknown *)(ULONG_PTR)0x1234;
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &s_Bogus, (void **)&Junk);
    ok(hr == E_NOINTERFACE, "EnumWarpAdapter(bogus) returned %08lx\n", hr);
    ok(Junk == NULL, "the output is %p\n", Junk);
    hr = IDXGIFactory4_EnumWarpAdapter(Factory, &IID_IDXGIAdapter1, NULL);
    ok(hr == DXGI_ERROR_INVALID_CALL, "EnumWarpAdapter(NULL) returned %08lx\n", hr);

    /* a device on it belongs to it */
    {
        HMODULE D3d11 = LoadLibraryW(L"d3d11.dll");
        PFN_D3D11_CREATE_DEVICE pCreateDevice = D3d11 ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(D3d11, "D3D11CreateDevice") : NULL;
        IUnknown *Device = NULL;
        IDXGIDevice *DxgiDevice = NULL;
        IDXGIAdapter *DevAdapter = NULL;

        if (!pCreateDevice)
            skip("no d3d11.dll\n");
        else
        {
            /* D3D_DRIVER_TYPE_UNKNOWN, D3D11_SDK_VERSION */
            hr = pCreateDevice((IDXGIAdapter *)Warp, 0, NULL, 0, NULL, 0, 7, (void **)&Device, NULL, NULL);
            ok(hr == S_OK, "D3D11CreateDevice on WARP returned %08lx\n", hr);
            if (Device)
            {
                hr = IUnknown_QueryInterface(Device, &IID_IDXGIDevice, (void **)&DxgiDevice);
                ok(hr == S_OK, "QueryInterface(IDXGIDevice) returned %08lx\n", hr);
                if (DxgiDevice)
                {
                    hr = IDXGIDevice_GetAdapter(DxgiDevice, &DevAdapter);
                    ok(hr == S_OK, "GetAdapter returned %08lx\n", hr);
                    if (DevAdapter)
                    {
                        hr = IDXGIAdapter_GetDesc(DevAdapter, &Desc0);
                        ok(hr == S_OK && SameLuid(Desc0.AdapterLuid, Desc1.AdapterLuid),
                           "the device is on another adapter (LUID %08lx:%08lx)\n",
                           Desc0.AdapterLuid.HighPart, Desc0.AdapterLuid.LowPart);
                        ok(DevAdapter == (IDXGIAdapter *)Warp, "the device adapter is not the adapter it was made on\n");
                        IDXGIAdapter_Release(DevAdapter);
                    }
                    IDXGIDevice_Release(DxgiDevice);
                }
                IUnknown_Release(Device);
            }
        }
    }

    IDXGIAdapter1_Release(Warp);
}

START_TEST(EnumWarpAdapter)
{
    HMODULE Dxgi = LoadLibraryW(L"dxgi.dll");
    HRESULT (WINAPI *pCreateFactory1)(REFIID, void **);
    HRESULT (WINAPI *pCreateFactory)(REFIID, void **);
    IDXGIFactory1 *Factory1 = NULL;
    IDXGIFactory *Factory = NULL;
    IDXGIFactory4 *Factory4 = NULL;
    HRESULT hr;

    ok(Dxgi != NULL, "dxgi.dll does not load, error %lu\n", GetLastError());
    if (!Dxgi)
        return;

    pCreateFactory1 = (void *)GetProcAddress(Dxgi, "CreateDXGIFactory1");
    pCreateFactory = (void *)GetProcAddress(Dxgi, "CreateDXGIFactory");
    ok(pCreateFactory1 != NULL, "CreateDXGIFactory1 is not exported\n");
    if (!pCreateFactory1)
        return;

    hr = pCreateFactory1(&IID_IDXGIFactory1, (void **)&Factory1);
    if (hr == DXGI_ERROR_UNSUPPORTED)
    {
        /* ReactOS: no 3D adapter (a 32-bit process on 64-bit ReactOS has none yet) */
        skip("no DXGI factory, CreateDXGIFactory1 returned %08lx\n", hr);
        return;
    }
    ok(hr == S_OK, "CreateDXGIFactory1 returned %08lx\n", hr);
    if (!Factory1)
        return;

    hr = IDXGIFactory1_QueryInterface(Factory1, &IID_IDXGIFactory4, (void **)&Factory4);
    ok(hr == S_OK, "QueryInterface(IDXGIFactory4) returned %08lx\n", hr);
    if (Factory4)
    {
        TestWarpAdapter(Factory4, Factory1);
        IDXGIFactory4_Release(Factory4);
    }
    IDXGIFactory1_Release(Factory1);

    /* the factory of CreateDXGIFactory is the same */
    if (pCreateFactory)
    {
        IDXGIAdapter1 *Warp = NULL;

        hr = pCreateFactory(&IID_IDXGIFactory, (void **)&Factory);
        ok(hr == S_OK, "CreateDXGIFactory returned %08lx\n", hr);
        if (Factory)
        {
            hr = IDXGIFactory_QueryInterface(Factory, &IID_IDXGIFactory4, (void **)&Factory4);
            ok(hr == S_OK, "QueryInterface(IDXGIFactory4) returned %08lx\n", hr);
            if (Factory4)
            {
                hr = IDXGIFactory4_EnumWarpAdapter(Factory4, &IID_IDXGIAdapter1, (void **)&Warp);
                ok(hr == S_OK, "EnumWarpAdapter returned %08lx\n", hr);
                if (Warp)
                    IDXGIAdapter1_Release(Warp);
                IDXGIFactory4_Release(Factory4);
            }
            IDXGIFactory_Release(Factory);
        }
    }
}
