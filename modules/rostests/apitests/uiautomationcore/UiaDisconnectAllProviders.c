/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for uiautomationcore!UiaDisconnectAllProviders (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64): the call succeeds, also when it is repeated and when no provider
 * was ever created. The last error is not tested: the first call sets it to 0 and the later ones leave it alone.
 */

#include <apitest.h>
#include <winbase.h>

typedef HRESULT (WINAPI *PFN_UiaDisconnectAllProviders)(void);

START_TEST(UiaDisconnectAllProviders)
{
    HMODULE Module = LoadLibraryW(L"uiautomationcore.dll");
    PFN_UiaDisconnectAllProviders pUiaDisconnectAllProviders;
    /* ReactOS has the function from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    BOOL Required = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    HRESULT hr;
    int i;

    if (!Module)
    {
        ok(!Required, "uiautomationcore.dll did not load: %lu\n", GetLastError());
        skip("uiautomationcore.dll is not available\n");
        return;
    }

    pUiaDisconnectAllProviders = (PFN_UiaDisconnectAllProviders)GetProcAddress(Module, "UiaDisconnectAllProviders");
    /* Windows 7 has no UI Automation core of this kind */
    if (!pUiaDisconnectAllProviders)
    {
        ok(!Required, "UiaDisconnectAllProviders is not exported\n");
        skip("UiaDisconnectAllProviders is not exported\n");
        return;
    }

    for (i = 0; i < 2; i++)
    {
        hr = pUiaDisconnectAllProviders();
        ok_hex(hr, S_OK);
    }
}
