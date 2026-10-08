/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable processor related routines
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

KAFFINITY KeActiveProcessors = 0;

/* Started processors that are not active yet; they take TLB flushes */
KAFFINITY KiStartingProcessors = 0;

/* Number of processors */
CCHAR KeNumberProcessors = 0;

#ifdef CONFIG_SMP

/* Theoretical maximum number of processors that can be handled.
 * Set once at run-time. Returned by KeQueryMaximumProcessorCount(). */
ULONG KeMaximumProcessors = MAXIMUM_PROCESSORS;

/* Maximum number of logical processors that can be started
 * (including dynamically) at run-time. If 0: do not perform checks. */
ULONG KeNumprocSpecified = 0;

/* Maximum number of logical processors that can be started
 * at boot-time. If 0: do not perform checks. */
ULONG KeBootprocSpecified = 0;

#endif // CONFIG_SMP

/* FUNCTIONS *****************************************************************/

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    return KeActiveProcessors;
}

/**
 * Retrieves the number of the current processor.
 *
 * \param ProcessorNumber Pointer to a PROCESSOR_NUMBER structure that receives the processor number.
 *
 * \return NTSTATUS The status of the operation.
 */
NTSTATUS
NTAPI
NtGetCurrentProcessorNumberEx(
    _Out_ PPROCESSOR_NUMBER ProcessorNumber)
{
    _SEH2_TRY
    {
        ProbeForWrite(ProcessorNumber, sizeof(PROCESSOR_NUMBER), __alignof(PROCESSOR_NUMBER));
        ProcessorNumber->Group = 0; // TODO: Support processor groups
        ProcessorNumber->Number = (UCHAR)KeGetCurrentProcessorNumber();
        ProcessorNumber->Reserved = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        return _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

/*
 * Processor groups. The kernel runs all processors in group 0: processor
 * numbers are contiguous from 0 to KeNumberProcessors - 1, so a processor's
 * system-wide index equals its number in the group. Processors are not
 * added after startup, so the maximum count is the active count, not the
 * configured limit KeMaximumProcessors.
 */

ULONG
NTAPI
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if ((GroupNumber != 0) && (GroupNumber != ALL_PROCESSOR_GROUPS))
        return 0;

    return (ULONG)KeNumberProcessors;
}

ULONG
NTAPI
KeQueryMaximumProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    return KeQueryActiveProcessorCountEx(GroupNumber);
}

USHORT
NTAPI
KeQueryActiveGroupCount(VOID)
{
    return 1;
}

USHORT
NTAPI
KeQueryMaximumGroupCount(VOID)
{
    return 1;
}

ULONG
NTAPI
KeGetCurrentProcessorNumberEx(
    _Out_opt_ PPROCESSOR_NUMBER ProcNumber)
{
    ULONG Number = KeGetCurrentProcessorNumber();

    if (ProcNumber)
    {
        ProcNumber->Group = 0;
        ProcNumber->Number = (UCHAR)Number;
        ProcNumber->Reserved = 0;
    }

    return Number;
}

NTSTATUS
NTAPI
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcIndex >= (ULONG)KeNumberProcessors)
        return STATUS_INVALID_PARAMETER;

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)ProcIndex;
    ProcNumber->Reserved = 0;
    return STATUS_SUCCESS;
}

ULONG
NTAPI
KeGetProcessorIndexFromNumber(
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    if ((ProcNumber->Group != 0) || (ProcNumber->Number >= (ULONG)KeNumberProcessors))
        return INVALID_PROCESSOR_INDEX;

    return ProcNumber->Number;
}
