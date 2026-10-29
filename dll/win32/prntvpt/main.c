/*
 * Print Ticket Services Module
 *
 * Copyright 2014 Jactry Zeng for CodeWeavers
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

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winspool.h"
#include "objbase.h"
#include "prntvpt.h"
#include "wine/debug.h"

#include "prntvpt_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(prntvpt);

HRESULT WINAPI PTReleaseMemory(PVOID mem)
{
    free(mem);
    return S_OK;
}

HRESULT WINAPI PTQuerySchemaVersionSupport(PCWSTR printer, DWORD *version)
{
    FIXME("stub:%s %p\n", debugstr_w(printer), version);
    return E_NOTIMPL;
}

HRESULT WINAPI PTCloseProvider(HPTPROVIDER provider)
{
    struct prn_provider *prov = (struct prn_provider *)provider;

    TRACE("%p\n", provider);

    if (!is_valid_provider(provider))
        return E_HANDLE;

    prov->owner = 0;
    free(prov->name);
    ClosePrinter(prov->hprn);
    free(prov);

    return S_OK;
}

HRESULT WINAPI PTOpenProvider(PCWSTR printer, DWORD version, HPTPROVIDER *provider)
{
    DWORD used_version;

    TRACE("%s, %ld, %p\n", debugstr_w(printer), version, provider);

    if (version != 1) return E_INVALIDARG;

    return PTOpenProviderEx(printer, 1, 1, provider, &used_version);
}

HRESULT WINAPI PTOpenProviderEx(const WCHAR *printer, DWORD max_version, DWORD pref_version, HPTPROVIDER *provider, DWORD *used_version)
{
    struct prn_provider *prov;

    TRACE("%s, %ld, %ld, %p, %p\n", debugstr_w(printer), max_version, pref_version, provider, used_version);

    if (!max_version || !provider || !used_version)
        return E_INVALIDARG;

    prov = malloc(sizeof(*prov));
    if (!prov) return E_OUTOFMEMORY;

    if (!OpenPrinterW((LPWSTR)printer, &prov->hprn, NULL))
    {
        free(prov);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    prov->name = wcsdup(printer);
    prov->owner = GetCurrentThreadId();
    *provider = (HPTPROVIDER)prov;
    *used_version = 1;

    return S_OK;
}

/* ReactOS additions: exports of the Windows prntvpt.dll that Wine lacks. Applications
 * that import them statically (WordCraft imports the PT* group) must load. */

HRESULT WINAPI PTGetPrintDeviceCapabilities(HPTPROVIDER provider, IStream *ticket, IStream *caps, BSTR *error)
{
    FIXME("stub: %p, %p, %p, %p\n", provider, ticket, caps, error);
    if (error) *error = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI PTGetPrintDeviceResources(HPTPROVIDER provider, LPCWSTR locale, IStream *ticket,
                                         IStream *resources, BSTR *error)
{
    FIXME("stub: %p, %s, %p, %p, %p\n", provider, debugstr_w(locale), ticket, resources, error);
    if (error) *error = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **obj)
{
    TRACE("%s, %s, %p\n", debugstr_guid(clsid), debugstr_guid(iid), obj);
    if (obj) *obj = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
