/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ntdll!ApiSetQueryApiSetPresence and ApiSetQueryApiSetPresenceEx (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64), and the first table is the one of Wine's test for the
 * two functions. The first function answers for the family of a contract, whatever the extension and the minor
 * version are; the second one wants a name without an extension and knows the contract by its name or by a newer
 * revision of its family. Windows 8 answers FALSE to the first one for a revision it does not list and for a name
 * with an unknown extension, as Wine's test allows.
 */

#include "precomp.h"

typedef NTSTATUS (WINAPI *PFN_Query)(const UNICODE_STRING *, BOOLEAN *);
typedef NTSTATUS (WINAPI *PFN_QueryEx)(const UNICODE_STRING *, BOOLEAN *, BOOLEAN *);

START_TEST(ApiSetQueryApiSetPresence)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_Query pQuery = (PFN_Query)GetProcAddress(Ntdll, "ApiSetQueryApiSetPresence");
    PFN_QueryEx pQueryEx = (PFN_QueryEx)GetProcAddress(Ntdll, "ApiSetQueryApiSetPresenceEx");
    static const struct
    {
        PCWSTR Name;
        BOOLEAN Present;
        NTSTATUS ExStatus;
        BOOLEAN ExPresent;
        BOOLEAN FalseOnWindows8;
    } Cases[] =
    {
        { L"api-ms-win-core-console-l1-1-0",      TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-console-l1-1-0.dll",  TRUE,  STATUS_INVALID_PARAMETER, FALSE },
        { L"api-ms-win-core-console-l1-1-9",      TRUE,  STATUS_SUCCESS,           FALSE, TRUE },
        { L"api-ms-win-core-console-l1-1-9.dll",  TRUE,  STATUS_INVALID_PARAMETER, FALSE, TRUE },
        { L"api-ms-win-core-console-l1-1",        FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-core-console-l1-1-0.fake", TRUE,  STATUS_INVALID_PARAMETER, FALSE, TRUE },
        { L"api-ms-win-foo-bar-l1-1-0",           FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-foo-bar-l1-1-0.dll",       FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"ext-ms-win-gdi-draw-l1-1-3",          TRUE,  STATUS_SUCCESS,           TRUE },
        { L"ext-ms-win-gdi-draw-l1-1-3.dll",      TRUE,  STATUS_INVALID_PARAMETER, FALSE },
        { L"foo",                                 FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"foo.dll",                             FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"",                                    FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"api-ms-win-core-synch-l1-2-0",        TRUE,  STATUS_SUCCESS,           TRUE },
        { L"API-MS-WIN-CORE-SYNCH-L1-2-0",        TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-synch-l1-3-0",        FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-security-base-l1-1-0",     TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-eventing-provider-l1-1-0", TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-eventing-provider-l1-1-0", FALSE, STATUS_SUCCESS,      FALSE },
        { L"kernel32",                            FALSE, STATUS_INVALID_PARAMETER, FALSE },
    };
    ULONG i;

    /* Windows 7 and older have none: they came with Windows 8; ReactOS has it from export level 0x601 */
    if (!pQuery)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "ApiSetQueryApiSetPresence is not exported\n");
        skip("ApiSetQueryApiSetPresence is not exported\n");
        return;
    }

    ok(pQueryEx != NULL || !(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
       "ApiSetQueryApiSetPresenceEx is not exported\n");

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        UNICODE_STRING Name;
        BOOLEAN Present = 0xff, InSchema = 0xff;
        NTSTATUS Status;

        RtlInitUnicodeString(&Name, Cases[i].Name);
        Status = pQuery(&Name, &Present);
        ok(Status == STATUS_SUCCESS, "%ls: status %#lx\n", Cases[i].Name, Status);
        ok(Present == Cases[i].Present || (!is_reactos() && Cases[i].FalseOnWindows8 && !Present),
           "%ls: present %u, expected %u\n", Cases[i].Name, Present, Cases[i].Present);

        if (!pQueryEx)
            continue;
        Present = InSchema = 0xff;
        Status = pQueryEx(&Name, &InSchema, &Present);
        ok(Status == Cases[i].ExStatus, "%ls: Ex status %#lx, expected %#lx\n", Cases[i].Name, Status, Cases[i].ExStatus);
        if (Status == STATUS_SUCCESS)
        {
            ok(InSchema == Cases[i].ExPresent, "%ls: in schema %u, expected %u\n", Cases[i].Name, InSchema, Cases[i].ExPresent);
            ok(Present == Cases[i].ExPresent, "%ls: Ex present %u, expected %u\n", Cases[i].Name, Present, Cases[i].ExPresent);
        }
        else
        {
            /* a refused name leaves the answers alone */
            ok(InSchema == 0xff && Present == 0xff, "%ls: the answers became %u and %u\n", Cases[i].Name, InSchema, Present);
        }
    }
}
