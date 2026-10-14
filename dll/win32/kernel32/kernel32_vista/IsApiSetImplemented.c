/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IsApiSetImplemented, the api-ms-win-core-apiquery contract (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * After IsApiSetImplemented of Wine's kernelbase (dlls/kernelbase/loader.c). The question goes to
 * ApiSetQueryApiSetPresenceEx of ntdll, which is looked up when it is asked: a system without it knows no API set.
 * The contract has to be known by its exact name, and a name with the extension of a file is not an API set
 * name (measured on Windows 10).
 */

#include "k32_vista.h"
#include <ndk/ldrfuncs.h>

#define NDEBUG
#include <debug.h>

typedef NTSTATUS (NTAPI *PFN_ApiSetQueryApiSetPresenceEx)(PCUNICODE_STRING, PBOOLEAN, PBOOLEAN);

BOOL
WINAPI
IsApiSetImplemented(
    _In_z_ LPCSTR Contract)
{
    static PFN_ApiSetQueryApiSetPresenceEx Query;
    ANSI_STRING Ansi;
    UNICODE_STRING Name;
    BOOLEAN Present = FALSE, InSchema = FALSE;
    NTSTATUS Status;

    if (!Contract)
        return FALSE;

    if (!Query)
    {
        ANSI_STRING Function = RTL_CONSTANT_STRING("ApiSetQueryApiSetPresenceEx");
        PVOID Address = NULL;

        if (!NT_SUCCESS(LdrGetProcedureAddress(GetModuleHandleW(L"ntdll.dll"), &Function, 0, &Address)))
            return FALSE;
        Query = Address;
    }

    RtlInitAnsiString(&Ansi, Contract);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&Name, &Ansi, TRUE)))
        return FALSE;

    Status = Query(&Name, &InSchema, &Present);
    if (!NT_SUCCESS(Status))
        Present = FALSE;

    RtlFreeUnicodeString(&Name);
    return Present;
}
