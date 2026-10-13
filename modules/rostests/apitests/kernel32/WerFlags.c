/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WerGetFlags and WerSetFlags
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef HRESULT WINAPI FN_WerSetFlags(DWORD);
typedef HRESULT WINAPI FN_WerGetFlags(HANDLE, PDWORD);

START_TEST(WerFlags)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_WerSetFlags *pWerSetFlags = (FN_WerSetFlags *)GetProcAddress(Kernel32, "WerSetFlags");
    FN_WerGetFlags *pWerGetFlags = (FN_WerGetFlags *)GetProcAddress(Kernel32, "WerGetFlags");
    DWORD Flags;
    HRESULT hr;

    if (!pWerSetFlags || !pWerGetFlags)
    {
        skip("WerGetFlags/WerSetFlags are not available\n");
        return;
    }

    hr = pWerSetFlags(1);
    ok(hr == S_OK, "WerSetFlags returned 0x%lx\n", hr);
    Flags = 0xdeadbeef;
    hr = pWerGetFlags(GetCurrentProcess(), &Flags);
    ok(hr == S_OK && Flags == 1, "WerGetFlags returned 0x%lx, flags 0x%lx\n", hr, Flags);

    hr = pWerSetFlags(0);
    ok(hr == S_OK, "WerSetFlags returned 0x%lx\n", hr);
    Flags = 0xdeadbeef;
    hr = pWerGetFlags(GetCurrentProcess(), &Flags);
    ok(hr == S_OK && Flags == 0, "WerGetFlags returned 0x%lx, flags 0x%lx\n", hr, Flags);
}
