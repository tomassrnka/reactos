/*
 * PROJECT:     ReactOS shell32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SHGetPropertyStoreForWindow
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/*
 * @unimplemented
 *
 * The property store of a window carries what the taskbar reads from it (the AppUserModelID and its
 * relaunch properties). The shell here keeps no per-window properties and its taskbar reads none, so
 * the request fails instead of handing out a store that nothing would honour.
 */
EXTERN_C HRESULT
WINAPI
SHGetPropertyStoreForWindow(
    _In_ HWND hwnd,
    _In_ REFIID riid,
    _Out_ void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;

    if (!IsWindow(hwnd))
        return E_INVALIDARG;

    FIXME("(%p, %s): window property stores are not supported\n", hwnd, debugstr_guid(&riid));
    return E_NOTIMPL;
}
