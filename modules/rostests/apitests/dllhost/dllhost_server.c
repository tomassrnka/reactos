/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     In-process COM server that the Surrogate test runs inside dllhost.exe
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#define WIN32_NO_STATUS
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

/* The server object is an IPersist; its CLSID has the process id of the server as Data1. */

typedef struct
{
    IPersist iface;
    LONG ref;
} Obj;

static HRESULT WINAPI Obj_QueryInterface(IPersist *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPersist))
    {
        *ppv = iface;
        IPersist_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Obj_AddRef(IPersist *iface)
{
    return InterlockedIncrement(&((Obj *)iface)->ref);
}

static ULONG WINAPI Obj_Release(IPersist *iface)
{
    LONG ref = InterlockedDecrement(&((Obj *)iface)->ref);
    if (!ref)
        HeapFree(GetProcessHeap(), 0, iface);
    return ref;
}

static HRESULT WINAPI Obj_GetClassID(IPersist *iface, CLSID *pClassID)
{
    static const GUID guid = { 0, 0xd11c, 0x4057, { 0x9a, 0x5e, 0x53, 0x75, 0x72, 0x72, 0x6f, 0x31 } };

    *pClassID = guid;
    pClassID->Data1 = GetCurrentProcessId();
    return S_OK;
}

static const IPersistVtbl ObjVtbl =
{
    Obj_QueryInterface,
    Obj_AddRef,
    Obj_Release,
    Obj_GetClassID
};

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Factory_AddRef(IClassFactory *iface)  { return 2; }
static ULONG WINAPI Factory_Release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI Factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    Obj *obj;
    HRESULT hr;

    *ppv = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;

    obj = HeapAlloc(GetProcessHeap(), 0, sizeof(*obj));
    if (!obj)
        return E_OUTOFMEMORY;
    obj->iface.lpVtbl = (IPersistVtbl *)&ObjVtbl;
    obj->ref = 1;
    hr = IPersist_QueryInterface(&obj->iface, riid, ppv);
    IPersist_Release(&obj->iface);
    return hr;
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *iface, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_CreateInstance,
    Factory_LockServer
};

static IClassFactory g_Factory = { (IClassFactoryVtbl *)&FactoryVtbl };

/* The class is {5d0e1f10-d11c-4057-9a5e-5375727260xx}: the last byte is the test's own, so that
 * the 64-bit and the 32-bit run of the test use different classes. */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    static const GUID base = { 0x5d0e1f10, 0xd11c, 0x4057, { 0x9a, 0x5e, 0x53, 0x75, 0x72, 0x72, 0x6f, 0x00 } };

    if (memcmp(rclsid, &base, 15))
    {
        *ppv = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return IClassFactory_QueryInterface(&g_Factory, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}
