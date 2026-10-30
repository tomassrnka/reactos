/*
 * PROJECT:     ReactOS Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The WinRT class that reports the diagnostic data collection level of the system
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * PowerShell 7 asks this class, through RoGetActivationFactory, whether it may collect
 * diagnostics before it starts its telemetry. This system has no telemetry consent: the class
 * reports the lowest level (Security) and refuses every collection level above it. Without the
 * class PowerShell's query throws, and the exception's stack trace loads three assemblies.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "objbase.h"
#include "initguid.h"
#include "inspectable.h"
#include "activation.h"
#include "winstring.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(profile);

static const WCHAR class_name[] = L"Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings";

/* IPlatformDiagnosticsAndUsageDataSettingsStatics {B6E24C1B-7B1C-4B32-8C62-A66597CE723A} */
static const GUID IID_IPlatformDiagnosticsAndUsageDataSettingsStatics =
    {0xb6e24c1b, 0x7b1c, 0x4b32, {0x8c, 0x62, 0xa6, 0x65, 0x97, 0xce, 0x72, 0x3a}};

/* Windows.System.Profile.PlatformDataCollectionLevel */
#define PlatformDataCollectionLevel_Security 0

typedef struct IPlatformDiagnosticsAndUsageDataSettingsStatics IPlatformDiagnosticsAndUsageDataSettingsStatics;

typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This);
    ULONG (STDMETHODCALLTYPE *Release)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This);
    HRESULT (STDMETHODCALLTYPE *GetIids)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, ULONG *count, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, TrustLevel *level);
    HRESULT (STDMETHODCALLTYPE *get_CollectionLevel)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT32 *value);
    HRESULT (STDMETHODCALLTYPE *add_CollectionLevelChanged)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, void *handler, INT64 *token);
    HRESULT (STDMETHODCALLTYPE *remove_CollectionLevelChanged)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT64 token);
    HRESULT (STDMETHODCALLTYPE *CanCollectDiagnostics)(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT32 level, BOOLEAN *result);
} StaticsVtbl;

struct IPlatformDiagnosticsAndUsageDataSettingsStatics
{
    const StaticsVtbl *lpVtbl;
};

/* One object serves as the activation factory and as the statics: it lives as long as the DLL */
static struct
{
    IActivationFactory IActivationFactory_iface;
    IPlatformDiagnosticsAndUsageDataSettingsStatics statics;
    LONG ref;
} object;

static HRESULT object_query_interface(REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IActivationFactory))
        *out = &object.IActivationFactory_iface;
    else if (IsEqualGUID(iid, &IID_IPlatformDiagnosticsAndUsageDataSettingsStatics))
        *out = &object.statics;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }

    InterlockedIncrement(&object.ref);
    return S_OK;
}

static HRESULT object_get_iids(ULONG *count, IID **iids)
{
    IID *list = CoTaskMemAlloc(2 * sizeof(IID));

    if (!list)
        return E_OUTOFMEMORY;
    list[0] = IID_IActivationFactory;
    list[1] = IID_IPlatformDiagnosticsAndUsageDataSettingsStatics;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT object_get_class_name(HSTRING *name)
{
    return WindowsCreateString(class_name, ARRAYSIZE(class_name) - 1, name);
}

/* IActivationFactory */
static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IActivationFactory *This, REFIID iid, void **out)
{
    return object_query_interface(iid, out);
}

static ULONG STDMETHODCALLTYPE factory_AddRef(IActivationFactory *This)
{
    return InterlockedIncrement(&object.ref);
}

static ULONG STDMETHODCALLTYPE factory_Release(IActivationFactory *This)
{
    return InterlockedDecrement(&object.ref);
}

static HRESULT STDMETHODCALLTYPE factory_GetIids(IActivationFactory *This, ULONG *count, IID **iids)
{
    return object_get_iids(count, iids);
}

static HRESULT STDMETHODCALLTYPE factory_GetRuntimeClassName(IActivationFactory *This, HSTRING *name)
{
    return object_get_class_name(name);
}

static HRESULT STDMETHODCALLTYPE factory_GetTrustLevel(IActivationFactory *This, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

/* The class is static: it has no instances */
static HRESULT STDMETHODCALLTYPE factory_ActivateInstance(IActivationFactory *This, IInspectable **instance)
{
    TRACE("(%p)\n", instance);
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

/* IPlatformDiagnosticsAndUsageDataSettingsStatics */
static HRESULT STDMETHODCALLTYPE statics_QueryInterface(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, REFIID iid, void **out)
{
    return object_query_interface(iid, out);
}

static ULONG STDMETHODCALLTYPE statics_AddRef(IPlatformDiagnosticsAndUsageDataSettingsStatics *This)
{
    return InterlockedIncrement(&object.ref);
}

static ULONG STDMETHODCALLTYPE statics_Release(IPlatformDiagnosticsAndUsageDataSettingsStatics *This)
{
    return InterlockedDecrement(&object.ref);
}

static HRESULT STDMETHODCALLTYPE statics_GetIids(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, ULONG *count, IID **iids)
{
    return object_get_iids(count, iids);
}

static HRESULT STDMETHODCALLTYPE statics_GetRuntimeClassName(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, HSTRING *name)
{
    return object_get_class_name(name);
}

static HRESULT STDMETHODCALLTYPE statics_GetTrustLevel(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE statics_get_CollectionLevel(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT32 *value)
{
    TRACE("(%p)\n", value);
    *value = PlatformDataCollectionLevel_Security;
    return S_OK;
}

/* The level never changes, so the handler is accepted and never called */
static HRESULT STDMETHODCALLTYPE statics_add_CollectionLevelChanged(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, void *handler, INT64 *token)
{
    TRACE("(%p, %p)\n", handler, token);
    if (!handler)
        return E_INVALIDARG;
    if (!token)
        return E_POINTER;
    *token = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE statics_remove_CollectionLevelChanged(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT64 token)
{
    TRACE("(%s)\n", wine_dbgstr_longlong(token));
    return S_OK;
}

/* A level can be collected when it does not exceed the current one (Security) */
static HRESULT STDMETHODCALLTYPE statics_CanCollectDiagnostics(IPlatformDiagnosticsAndUsageDataSettingsStatics *This, INT32 level, BOOLEAN *result)
{
    TRACE("(%d, %p)\n", level, result);
    if (!result)
        return E_POINTER;
    *result = level <= PlatformDataCollectionLevel_Security;
    return S_OK;
}

static const StaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    statics_get_CollectionLevel,
    statics_add_CollectionLevelChanged,
    statics_remove_CollectionLevelChanged,
    statics_CanCollectDiagnostics,
};

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    LPCWSTR name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("(%p, %p)\n", classid, factory);

    if (!factory)
        return E_POINTER;
    *factory = NULL;

    if (!name || _wcsicmp(name, class_name) != 0)
        return CLASS_E_CLASSNOTAVAILABLE;

    *factory = &object.IActivationFactory_iface;
    InterlockedIncrement(&object.ref);
    return S_OK;
}

/* The object is static and may be in use by anyone holding a reference: stay loaded */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            object.IActivationFactory_iface.lpVtbl = (IActivationFactoryVtbl *)&factory_vtbl;
            object.statics.lpVtbl = &statics_vtbl;
            object.ref = 1;
            break;
    }
    return TRUE;
}
