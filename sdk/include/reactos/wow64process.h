/* Private guest/native process-creation ABI. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

typedef struct
{
    ULONG Token, Application, CommandLine, ProcessAttributes, ThreadAttributes;
    ULONG InheritHandles, CreationFlags, Environment, CurrentDirectory;
    ULONG StartupInfo, ProcessInformation, NewToken, ParentParameters;
    ULONG Error;
    ULONG AttributeList; /* EXTENDED_STARTUPINFO_PRESENT: the guest's validated attribute list, or 0 */
} WOW64_CREATE_PROCESS;

NTSTATUS NTAPI RtlWow64CreateProcess(WOW64_CREATE_PROCESS *Request);
