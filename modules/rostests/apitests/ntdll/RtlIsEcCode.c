/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ntdll!RtlIsEcCode and its kernel32 forwarder
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The Visual C++ 14.4x runtime (vcruntime140_1.dll) imports RtlIsEcCode from kernel32.dll.
 * Windows 10 22H2 before the servicing update that added it exports it from neither DLL, and a
 * process that is not ARM64EC has no EC code at all.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

typedef BOOLEAN (NTAPI *PFN_RtlIsEcCode)(ULONG_PTR);

START_TEST(RtlIsEcCode)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_RtlIsEcCode pNtdll = (PFN_RtlIsEcCode)GetProcAddress(Ntdll, "RtlIsEcCode");
    PFN_RtlIsEcCode pKernel32 = (PFN_RtlIsEcCode)GetProcAddress(Kernel32, "RtlIsEcCode");

    if (is_reactos())
    {
        ok(pNtdll != NULL, "ntdll.dll does not export RtlIsEcCode\n");
        ok(pKernel32 != NULL, "kernel32.dll does not export RtlIsEcCode\n");
    }
    if (!pNtdll && !pKernel32)
    {
        skip("RtlIsEcCode is not exported\n");
        return;
    }

    if (pNtdll)
    {
        ok(!pNtdll((ULONG_PTR)GetModuleHandleW(NULL)), "ntdll: the image base is EC code\n");
        ok(!pNtdll((ULONG_PTR)pNtdll), "ntdll: RtlIsEcCode itself is EC code\n");
        ok(!pNtdll(0), "ntdll: NULL is EC code\n");
    }
    if (pKernel32)
    {
        ok(!pKernel32((ULONG_PTR)pKernel32), "kernel32: RtlIsEcCode itself is EC code\n");
        ok(!pKernel32(0), "kernel32: NULL is EC code\n");
    }
}
