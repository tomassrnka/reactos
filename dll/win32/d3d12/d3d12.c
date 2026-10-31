/*
 * PROJECT:     ReactOS Direct3D 12 stub
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     d3d12.dll for the export levels that have no Direct3D 12 (below Windows 10): the
 *              library loads, so that a module that imports D3D12CreateDevice can be loaded, and the
 *              device is not there, as on Windows 10 without an adapter that supports Direct3D 12.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * What Windows 10 22H2 does, as measured there, with the arguments that these functions check:
 *  - D3D12CreateDevice: a minimum feature level below 11_0 is E_INVALIDARG and *ppDevice is NULL. With a
 *    level of 11_0 or more it creates the device, or fails with DXGI_ERROR_UNSUPPORTED when no adapter
 *    can (the stub never can). A NULL ppDevice with a valid level is a check that gives S_FALSE there.
 *  - D3D12GetDebugInterface without the graphics tools: DXGI_ERROR_SDK_COMPONENT_MISSING.
 *  - D3D12EnableExperimentalFeatures: S_OK for none, E_NOINTERFACE for a feature that is not known.
 *  - D3D12GetInterface of a class that is not known: E_NOINTERFACE.
 *  - D3D12SerializeRootSignature with a NULL description: E_INVALIDARG.
 * The root signature functions work on Windows without an adapter. Here they are not implemented.
 */

#include <windows.h>
#include <winerror.h>

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED _HRESULT_TYPEDEF_(0x887A0004)
#endif
#ifndef DXGI_ERROR_SDK_COMPONENT_MISSING
#define DXGI_ERROR_SDK_COMPONENT_MISSING _HRESULT_TYPEDEF_(0x887A002D)
#endif

#define D3D_FEATURE_LEVEL_11_0_VALUE 0xb000

HRESULT WINAPI
D3D12CreateDevice(
    _In_opt_ void *pAdapter,
    _In_ UINT MinimumFeatureLevel,
    _In_ REFIID riid,
    _Out_opt_ void **ppDevice)
{
    if (ppDevice)
        *ppDevice = NULL;

    if (MinimumFeatureLevel < D3D_FEATURE_LEVEL_11_0_VALUE)
        return E_INVALIDARG;

    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI
D3D12GetDebugInterface(
    _In_ REFIID riid,
    _Out_opt_ void **ppvDebug)
{
    if (ppvDebug)
        *ppvDebug = NULL;
    return DXGI_ERROR_SDK_COMPONENT_MISSING;
}

HRESULT WINAPI
D3D12EnableExperimentalFeatures(
    _In_ UINT NumFeatures,
    _In_reads_opt_(NumFeatures) const IID *pIIDs,
    _In_opt_ void *pConfigurationStructs,
    _In_opt_ UINT *pConfigurationStructSizes)
{
    return NumFeatures ? E_NOINTERFACE : S_OK;
}

HRESULT WINAPI
D3D12GetInterface(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Out_opt_ void **ppvDebug)
{
    if (ppvDebug)
        *ppvDebug = NULL;
    return E_NOINTERFACE;
}

HRESULT WINAPI
D3D12SerializeRootSignature(
    _In_ const void *pRootSignature,
    _In_ UINT Version,
    _Out_ void **ppBlob,
    _Out_opt_ void **ppErrorBlob)
{
    if (ppBlob)
        *ppBlob = NULL;
    if (ppErrorBlob)
        *ppErrorBlob = NULL;
    if (!pRootSignature || !ppBlob)
        return E_INVALIDARG;
    return E_NOTIMPL;
}

HRESULT WINAPI
D3D12SerializeVersionedRootSignature(
    _In_ const void *pRootSignature,
    _Out_ void **ppBlob,
    _Out_opt_ void **ppErrorBlob)
{
    if (ppBlob)
        *ppBlob = NULL;
    if (ppErrorBlob)
        *ppErrorBlob = NULL;
    if (!pRootSignature || !ppBlob)
        return E_INVALIDARG;
    return E_NOTIMPL;
}

HRESULT WINAPI
D3D12CreateRootSignatureDeserializer(
    _In_ const void *pSrcData,
    _In_ SIZE_T SrcDataSizeInBytes,
    _In_ REFIID pRootSignatureDeserializerInterface,
    _Out_ void **ppRootSignatureDeserializer)
{
    if (ppRootSignatureDeserializer)
        *ppRootSignatureDeserializer = NULL;
    if (!pSrcData || !ppRootSignatureDeserializer)
        return E_INVALIDARG;
    return E_NOTIMPL;
}

HRESULT WINAPI
D3D12CreateVersionedRootSignatureDeserializer(
    _In_ const void *pSrcData,
    _In_ SIZE_T SrcDataSizeInBytes,
    _In_ REFIID pRootSignatureDeserializerInterface,
    _Out_ void **ppRootSignatureDeserializer)
{
    if (ppRootSignatureDeserializer)
        *ppRootSignatureDeserializer = NULL;
    if (!pSrcData || !ppRootSignatureDeserializer)
        return E_INVALIDARG;
    return E_NOTIMPL;
}

/* The PIX event runtime hooks: there is no PIX capture, no thread info and no block. */

UINT64 * WINAPI
D3D12PIXEventsReplaceBlock(
    _In_ void *pThreadInfo,
    _In_ BOOL GetEarliestTime)
{
    return NULL;
}

void * WINAPI
D3D12PIXGetThreadInfo(VOID)
{
    return NULL;
}

VOID WINAPI
D3D12PIXNotifyWakeFromFenceSignal(
    _In_ HANDLE hEvent)
{
}

VOID WINAPI
D3D12PIXReportCounter(
    _In_ PCWSTR Name,
    _In_ DWORD Value)
{
}

/*
 * The exports whose signature is not known (they are not documented, and Wine has them as stubs).
 * They are cdecl, so that a caller that has pushed arguments never has the stack pointer moved by the
 * wrong count. They return 0 and do nothing.
 */

INT __cdecl GetBehaviorValue(VOID) { return 0; }
INT __cdecl SetAppCompatStringPointer(VOID) { return 0; }
INT __cdecl D3D12CoreCreateLayeredDevice(VOID) { return (INT)E_NOTIMPL; }
INT __cdecl D3D12CoreGetLayeredDeviceSize(VOID) { return 0; }
INT __cdecl D3D12CoreRegisterLayers(VOID) { return (INT)E_NOTIMPL; }
