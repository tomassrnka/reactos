/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     VirtualAlloc2 and VirtualAlloc2FromApp
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Cygwin (sshfs-win, git for Windows' ssh, MSYS2) reserves its thread stacks with VirtualAlloc2 and an address
 * requirement and aborts at start when KernelBase.dll does not export it. There is no NtAllocateVirtualMemoryEx
 * here, so the address requirement (lowest and highest address, alignment) is met by looking for a free region
 * and reserving it at that address. The error codes and the checks of the arguments are those of Windows 10.
 * Placeholders, partitions, NUMA nodes other than 0 and the attribute flags are not supported.
 */

#include <stdarg.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

#define BASEP_GRANULARITY   0x10000
#define BASEP_PAGE_SIZE     0x1000
#define BASEP_PLACEHOLDER_FLAGS (0x40000 | 0x4000) /* MEM_RESERVE_PLACEHOLDER, MEM_REPLACE_PLACEHOLDER */

#define BASEP_ROUND_UP(x, a)    (((x) + (a) - 1) & ~((ULONG_PTR)(a) - 1))

/* Find a free range of Needed bytes in [Low, High] with the alignment Align and reserve it */
static
PVOID
BasepAllocateInRange(
    _In_ HANDLE Process,
    _In_ SIZE_T Size,
    _In_ SIZE_T Needed,
    _In_ ULONG Type,
    _In_ ULONG Protect,
    _In_ ULONG_PTR Low,
    _In_ ULONG_PTR High,
    _In_ SIZE_T Align)
{
    BOOL TopDown = (Type & MEM_TOP_DOWN) != 0;
    ULONG Attempt;

    for (Attempt = 0; Attempt < 8; Attempt++)
    {
        MEMORY_BASIC_INFORMATION Info;
        ULONG_PTR Current = Low, Best = 0;

        while (Current <= High && High - Current + 1 >= Needed)
        {
            ULONG_PTR RegionEnd, Last, Candidate;

            if (!VirtualQueryEx(Process, (PVOID)Current, &Info, sizeof(Info)))
                break;

            RegionEnd = (ULONG_PTR)Info.BaseAddress + Info.RegionSize; /* one past the region, or 0 at the top */
            Last = (RegionEnd == 0 || RegionEnd - 1 > High) ? High : RegionEnd - 1;

            if (Info.State == MEM_FREE && Last - Current + 1 >= Needed)
            {
                if (TopDown)
                {
                    /* highest candidate in this region that is not below Current */
                    Candidate = (Last + 1 - Needed) & ~((ULONG_PTR)Align - 1);
                    if (Candidate >= Current && Candidate + Needed - 1 <= Last)
                        Best = Candidate;
                }
                else
                {
                    Candidate = BASEP_ROUND_UP(Current, Align);
                    if (Candidate >= Current && Candidate <= Last && Last - Candidate + 1 >= Needed)
                    {
                        Best = Candidate;
                        break;
                    }
                }
            }

            if (RegionEnd == 0 || RegionEnd - 1 >= High)
                break;
            Current = RegionEnd;
        }

        if (!Best)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }

        /* Somebody may have taken the range since it was looked up: look again */
        {
            PVOID Base = VirtualAllocEx(Process, (PVOID)Best, Size, Type & ~MEM_TOP_DOWN, Protect);
            if (Base)
                return Base;
        }
    }

    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static
PVOID
BasepVirtualAlloc2(
    _In_opt_ HANDLE Process,
    _In_opt_ PVOID BaseAddress,
    _In_ SIZE_T Size,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _In_reads_opt_(ParameterCount) MEM_EXTENDED_PARAMETER *ExtendedParameters,
    _In_ ULONG ParameterCount)
{
    MEM_ADDRESS_REQUIREMENTS *Requirements = NULL;
    BOOL SeenNumaNode = FALSE;
    SYSTEM_INFO SystemInfo;
    ULONG_PTR Low, High;
    SIZE_T Align, Needed;
    ULONG i;
    PVOID Base;

    if (!Process)
        Process = GetCurrentProcess();

    if (ParameterCount != 0 && ExtendedParameters == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    for (i = 0; i < ParameterCount; i++)
    {
        switch (ExtendedParameters[i].Type)
        {
            case MemExtendedParameterAddressRequirements:
                if (Requirements)
                {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    return NULL;
                }
                if (!ExtendedParameters[i].Pointer)
                {
                    SetLastError(ERROR_NOACCESS);
                    return NULL;
                }
                Requirements = ExtendedParameters[i].Pointer;
                break;

            case MemExtendedParameterNumaNode:
                /* the single node of this system */
                if (SeenNumaNode || ExtendedParameters[i].ULong != 0)
                {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    return NULL;
                }
                SeenNumaNode = TRUE;
                break;

            default:
                /* partitions, physical pages, attribute flags and the image machine: Windows takes them for
                   placeholders and special memory only; invalid types are rejected the same way */
                SetLastError(ERROR_INVALID_PARAMETER);
                return NULL;
        }
    }

    if (AllocationType & BASEP_PLACEHOLDER_FLAGS)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }

    /* No requirement, or one that does not ask for anything: a plain allocation */
    if (!Requirements ||
        (!Requirements->LowestStartingAddress && !Requirements->HighestEndingAddress && !Requirements->Alignment))
    {
        return VirtualAllocEx(Process, BaseAddress, Size, AllocationType, PageProtection);
    }

    if (BaseAddress || Size == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    GetSystemInfo(&SystemInfo);
    Low = (ULONG_PTR)Requirements->LowestStartingAddress;
    High = (ULONG_PTR)Requirements->HighestEndingAddress;
    Align = Requirements->Alignment;

    if ((Low & (BASEP_GRANULARITY - 1)) ||
        (High && ((High & (BASEP_PAGE_SIZE - 1)) != (BASEP_PAGE_SIZE - 1) ||
                  High > (ULONG_PTR)SystemInfo.lpMaximumApplicationAddress)) ||
        (High && Low > High) ||
        (Align && ((Align & (Align - 1)) || Align < BASEP_GRANULARITY || (Low & (Align - 1)))))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (Size > (SIZE_T)SystemInfo.lpMaximumApplicationAddress)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    Needed = BASEP_ROUND_UP(Size, BASEP_PAGE_SIZE);
    if (High && Needed - 1 > High - Low)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    /* A reserve is meant even when the caller only commits */
    if (!(AllocationType & MEM_RESERVE))
        AllocationType |= MEM_RESERVE;

    if (!Low)
        Low = BASEP_GRANULARITY;

    if (!High)
    {
        /* Only a lowest address or an alignment: let the system choose, and look further only when that does not meet them */
        Base = VirtualAllocEx(Process, NULL, Size, AllocationType, PageProtection);
        if (Base)
        {
            if ((ULONG_PTR)Base >= Low && (!Align || ((ULONG_PTR)Base & (Align - 1)) == 0))
                return Base;
            VirtualFreeEx(Process, Base, 0, MEM_RELEASE);
        }
        High = (ULONG_PTR)SystemInfo.lpMaximumApplicationAddress;
    }
    else if (High < Low || High - Low + 1 < Needed)
    {
        /* nothing can be allocated below the first allocation granule */
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }

    return BasepAllocateInRange(Process,
                                Size,
                                Needed,
                                AllocationType,
                                PageProtection,
                                Low,
                                High,
                                Align ? Align : BASEP_GRANULARITY);
}

PVOID
WINAPI
VirtualAlloc2(
    _In_opt_ HANDLE Process,
    _In_opt_ PVOID BaseAddress,
    _In_ SIZE_T Size,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER *ExtendedParameters,
    _In_ ULONG ParameterCount)
{
    return BasepVirtualAlloc2(Process,
                              BaseAddress,
                              Size,
                              AllocationType,
                              PageProtection,
                              ExtendedParameters,
                              ParameterCount);
}

PVOID
WINAPI
VirtualAlloc2FromApp(
    _In_opt_ HANDLE Process,
    _In_opt_ PVOID BaseAddress,
    _In_ SIZE_T Size,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER *ExtendedParameters,
    _In_ ULONG ParameterCount)
{
    /* a Store app may not get memory that can run code */
    if (PageProtection == PAGE_EXECUTE || PageProtection == PAGE_EXECUTE_READ ||
        PageProtection == PAGE_EXECUTE_READWRITE || PageProtection == PAGE_EXECUTE_WRITECOPY)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    return BasepVirtualAlloc2(Process,
                              BaseAddress,
                              Size,
                              AllocationType,
                              PageProtection,
                              ExtendedParameters,
                              ParameterCount);
}
