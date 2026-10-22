/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A DLL that the loader test copies next to an executable under the name of a contract
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    return TRUE;
}

/* The loader test recognises this module by this export */
DWORD
WINAPI
ApisetLoaderTestMarker(VOID)
{
    return 0x41504c44;
}
