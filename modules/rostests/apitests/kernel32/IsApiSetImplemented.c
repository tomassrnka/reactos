/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for IsApiSetImplemented (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64). Only names that the system knows by the exact
 * name are used: Windows 10 also accepts newer minor versions of a family.
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_IsApiSetImplemented)(LPCSTR);

#define PENDING_ERROR 0xdeadbeef

START_TEST(IsApiSetImplemented)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_IsApiSetImplemented pIsApiSetImplemented;
    static const char *const Known[] =
    {
        "api-ms-win-core-synch-l1-2-0",
        "API-MS-WIN-CORE-SYNCH-L1-2-0",
        "api-ms-win-core-synch-l1-1-0",
        "api-ms-win-security-base-l1-1-0",
        "api-ms-win-eventing-provider-l1-1-0",
    };
    static const char *const Unknown[] =
    {
        "api-ms-win-core-synch-l1-2-0.dll",
        "api-ms-win-core-synch-l1-3-0",
        "api-ms-win-nonexistent-l1-1-0",
        "api-ms-win-core-eventing-provider-l1-1-0",
        "api-ms-win",
        "kernel32",
        "kernel32.dll",
        "",
    };
    ULONG i;

    pIsApiSetImplemented = (PFN_IsApiSetImplemented)GetProcAddress(Kernel32, "IsApiSetImplemented");
    if (!pIsApiSetImplemented)
    {
        pIsApiSetImplemented = (PFN_IsApiSetImplemented)GetProcAddress(LoadLibraryW(L"kernelbase.dll"), "IsApiSetImplemented");
        /* ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
        ok(pIsApiSetImplemented || !(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")), "IsApiSetImplemented is not exported\n");
        if (!pIsApiSetImplemented)
        {
            skip("IsApiSetImplemented is not exported\n");
            return;
        }
    }

    /* the question goes to ntdll, which has the answer from export level 0x601 */
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "ApiSetQueryApiSetPresence"))
    {
        ok(!(is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules")), "ntdll has no ApiSetQueryApiSetPresence\n");
        skip("ntdll has no ApiSetQueryApiSetPresence\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Known); i++)
    {
        SetLastError(PENDING_ERROR);
        ok(pIsApiSetImplemented(Known[i]) == TRUE, "%s is not implemented\n", Known[i]);
        ok_hex(GetLastError(), PENDING_ERROR);
    }
    for (i = 0; i < RTL_NUMBER_OF(Unknown); i++)
        ok(pIsApiSetImplemented(Unknown[i]) == FALSE, "%s is implemented\n", Unknown[i]);
    ok(pIsApiSetImplemented(NULL) == FALSE, "a null name is implemented\n");
}
