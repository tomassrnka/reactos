/*
 * PROJECT:     ReactOS Windows.Data.Pdf stub
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Export surface of Windows.Data.Pdf.dll (the WinRT PDF renderer)
 *
 * The Windows 10 DLL exports exactly DllCanUnloadNow, DllGetActivationFactory,
 * DllGetClassObject and PdfCreateRenderer. Applications such as WordCraft link
 * PdfCreateRenderer statically and only call it for PDF preview, so every
 * entry point that would create a renderer fails with a clean HRESULT.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "objbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(pdf);

/* PdfCreateRenderer(IDXGIDevice *device, IPdfRendererNative **renderer) */
HRESULT WINAPI PdfCreateRenderer(void *device, void **renderer)
{
    FIXME("(%p, %p) stub\n", device, renderer);
    if (renderer) *renderer = NULL;
    return E_NOTIMPL;
}

/* DllGetActivationFactory(HSTRING classid, IActivationFactory **factory) */
HRESULT WINAPI DllGetActivationFactory(void *classid, void **factory)
{
    FIXME("(%p, %p) stub\n", classid, factory);
    if (factory) *factory = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **obj)
{
    TRACE("(%s, %s, %p)\n", debugstr_guid(clsid), debugstr_guid(iid), obj);
    if (obj) *obj = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

/* The DLL never hands out an object, so it can always be unloaded. */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
