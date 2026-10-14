/* ReactOS WoW64 process environment. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

static NTSTATUS SetVariable(PVOID *Environment, PCWSTR Name, PCWSTR Value)
{
    UNICODE_STRING Key, Data;
    RtlInitUnicodeString(&Key, Name);
    RtlInitUnicodeString(&Data, Value);
    return RtlSetEnvironmentVariable(Environment, &Key, &Data);
}

static BOOLEAN GetVariable(PVOID Environment, PCWSTR Name, PWSTR Value, USHORT Capacity)
{
    UNICODE_STRING Key, Data;
    NTSTATUS Status;
    RtlInitUnicodeString(&Key, Name);
    RtlInitEmptyUnicodeString(&Data, Value, Capacity - sizeof(WCHAR));
    Status = RtlQueryEnvironmentVariable_U(Environment, &Key, &Data);
    if (!NT_SUCCESS(Status)) return FALSE;
    Value[Data.Length / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

NTSTATUS Wow64CreateEnvironment(PVOID *Environment)
{
    WCHAR Program[MAX_PATH], Common[MAX_PATH], Program32[MAX_PATH], Common32[MAX_PATH];
    NTSTATUS Status;
    ULONG Length;

    /* Work on a private native-addressed copy, then serialize it below 4 GB.
     * This also preserves custom inherited variables and drive directories. */
    Status = RtlCreateEnvironment(TRUE, Environment);
    if (!NT_SUCCESS(Status)) return Status;
    Status = SetVariable(Environment, L"PROCESSOR_ARCHITECTURE", L"x86");
    if (!NT_SUCCESS(Status)) return Status;
    Status = SetVariable(Environment, L"PROCESSOR_ARCHITEW6432", L"AMD64");
    if (!NT_SUCCESS(Status)) return Status;

    /* A native parent may already provide the architecture-specific paths.
     * Keep those values, including non-default installation directories. */
    if (GetVariable(*Environment, L"ProgramW6432", Program, sizeof(Program)) ||
        GetVariable(*Environment, L"ProgramFiles", Program, sizeof(Program)))
    {
        if (!GetVariable(*Environment, L"ProgramFiles(x86)", Program32, sizeof(Program32)))
        {
            if (wcslen(Program) + 6 >= RTL_NUMBER_OF(Program32)) return STATUS_NAME_TOO_LONG;
            wcscpy(Program32, Program);
            wcscat(Program32, L" (x86)");
        }
        Status = SetVariable(Environment, L"ProgramW6432", Program);
        if (!NT_SUCCESS(Status)) return Status;
        Status = SetVariable(Environment, L"ProgramFiles(x86)", Program32);
        if (!NT_SUCCESS(Status)) return Status;
        Status = SetVariable(Environment, L"ProgramFiles", Program32);
        if (!NT_SUCCESS(Status)) return Status;
        if (GetVariable(*Environment, L"CommonProgramW6432", Common, sizeof(Common)) ||
            GetVariable(*Environment, L"CommonProgramFiles", Common, sizeof(Common)))
        {
            if (!GetVariable(*Environment, L"CommonProgramFiles(x86)", Common32, sizeof(Common32)))
            {
                Length = wcslen(Program);
                /* Derive only a component beneath the native Program Files.
                 * A separately configured common path is preserved. */
                if (!_wcsnicmp(Common, Program, Length) && Common[Length] == L'\\')
                {
                    if (wcslen(Program32) + wcslen(Common + Length) >= RTL_NUMBER_OF(Common32))
                        return STATUS_NAME_TOO_LONG;
                    wcscpy(Common32, Program32);
                    wcscat(Common32, Common + Length);
                }
                else wcscpy(Common32, Common);
            }
            Status = SetVariable(Environment, L"CommonProgramW6432", Common);
            if (!NT_SUCCESS(Status)) return Status;
            Status = SetVariable(Environment, L"CommonProgramFiles(x86)", Common32);
            if (!NT_SUCCESS(Status)) return Status;
            Status = SetVariable(Environment, L"CommonProgramFiles", Common32);
        }
    }
    return Status;
}
