/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DiscardVirtualMemory (Windows 8.1 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * After DiscardVirtualMemory of Wine's kernelbase (dlls/kernelbase/memory.c). Windows 10 checks the range first
 * (measured): whole pages only, and every page committed and writable.
 */

#include "k32_vista.h"
#include <ndk/mmfuncs.h>

#define NDEBUG
#include <debug.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif

#define PAGE_WRITABLE_MASK (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

DWORD
WINAPI
DiscardVirtualMemory(
    _Inout_updates_bytes_(Size) PVOID VirtualAddress,
    _In_ SIZE_T Size)
{
    MEMORY_BASIC_INFORMATION Info;
    PUCHAR Address = VirtualAddress, End;
    NTSTATUS Status;
    PVOID Base;
    SIZE_T RegionSize;

    if (!Size || ((ULONG_PTR)Address & (PAGE_SIZE - 1)) || (Size & (PAGE_SIZE - 1)) ||
        (ULONG_PTR)Address + Size < (ULONG_PTR)Address)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* The Nt call leaves the last error of the caller alone, which Windows 10 does too (measured) */
    for (End = Address + Size; Address < End; Address += Info.RegionSize)
    {
        if (!NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(), Address, MemoryBasicInformation, &Info, sizeof(Info), NULL)) ||
            Info.State != MEM_COMMIT ||
            !(Info.Protect & PAGE_WRITABLE_MASK) ||
            (Info.Protect & PAGE_GUARD))
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    Base = VirtualAddress;
    RegionSize = Size;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &RegionSize, MEM_RESET, PAGE_NOACCESS);
    return RtlNtStatusToDosError(Status);
}
