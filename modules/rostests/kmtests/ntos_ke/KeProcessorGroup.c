/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite processor group routines test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

typedef ULONG (NTAPI *PFN_COUNT_EX)(USHORT);
typedef USHORT (NTAPI *PFN_GROUP_COUNT)(VOID);
typedef ULONG (NTAPI *PFN_CURRENT_EX)(PPROCESSOR_NUMBER);
typedef NTSTATUS (NTAPI *PFN_NUMBER_FROM_INDEX)(ULONG, PPROCESSOR_NUMBER);
typedef ULONG (NTAPI *PFN_INDEX_FROM_NUMBER)(PPROCESSOR_NUMBER);

static
PVOID
GetRoutine(
    _In_ PCWSTR Name)
{
    UNICODE_STRING String;

    RtlInitUnicodeString(&String, Name);
    return MmGetSystemRoutineAddress(&String);
}

/* Windows 7 and later export the routines; ReactOS does for 0x601 and later */
static
BOOLEAN
RoutinesExpected(VOID)
{
    RTL_OSVERSIONINFOW Version;

    /* ReactOS marks the end of the shared user data page; read it through the
     * kernel mapping */
    if (*(PULONG)(KI_USER_SHARED_DATA + PAGE_SIZE - sizeof(ULONG)) == 0x8eac705)
        return DLL_EXPORT_VERSION >= 0x601;

    Version.dwOSVersionInfoSize = sizeof(Version);
    RtlGetVersion(&Version);
    return (Version.dwMajorVersion > 6) ||
           ((Version.dwMajorVersion == 6) && (Version.dwMinorVersion >= 1));
}

START_TEST(KeProcessorGroup)
{
    PFN_COUNT_EX pKeQueryActiveProcessorCountEx;
    PFN_COUNT_EX pKeQueryMaximumProcessorCountEx;
    PFN_GROUP_COUNT pKeQueryActiveGroupCount;
    PFN_GROUP_COUNT pKeQueryMaximumGroupCount;
    PFN_CURRENT_EX pKeGetCurrentProcessorNumberEx;
    PFN_NUMBER_FROM_INDEX pKeGetProcessorNumberFromIndex;
    PFN_INDEX_FROM_NUMBER pKeGetProcessorIndexFromNumber;
    PROCESSOR_NUMBER Number;
    ULONG Count, MaxCount, Index, Current;
    BOOLEAN SingleGroup;
    NTSTATUS Status;
    KIRQL OldIrql;

    pKeQueryActiveProcessorCountEx = GetRoutine(L"KeQueryActiveProcessorCountEx");
    pKeQueryMaximumProcessorCountEx = GetRoutine(L"KeQueryMaximumProcessorCountEx");
    pKeQueryActiveGroupCount = GetRoutine(L"KeQueryActiveGroupCount");
    pKeQueryMaximumGroupCount = GetRoutine(L"KeQueryMaximumGroupCount");
    pKeGetCurrentProcessorNumberEx = GetRoutine(L"KeGetCurrentProcessorNumberEx");
    pKeGetProcessorNumberFromIndex = GetRoutine(L"KeGetProcessorNumberFromIndex");
    pKeGetProcessorIndexFromNumber = GetRoutine(L"KeGetProcessorIndexFromNumber");
    if (!pKeQueryActiveProcessorCountEx || !pKeQueryMaximumProcessorCountEx ||
        !pKeQueryActiveGroupCount || !pKeQueryMaximumGroupCount ||
        !pKeGetCurrentProcessorNumberEx || !pKeGetProcessorNumberFromIndex ||
        !pKeGetProcessorIndexFromNumber)
    {
        if (RoutinesExpected())
        {
            ok(pKeQueryActiveProcessorCountEx != NULL, "KeQueryActiveProcessorCountEx is not exported\n");
            ok(pKeQueryMaximumProcessorCountEx != NULL, "KeQueryMaximumProcessorCountEx is not exported\n");
            ok(pKeQueryActiveGroupCount != NULL, "KeQueryActiveGroupCount is not exported\n");
            ok(pKeQueryMaximumGroupCount != NULL, "KeQueryMaximumGroupCount is not exported\n");
            ok(pKeGetCurrentProcessorNumberEx != NULL, "KeGetCurrentProcessorNumberEx is not exported\n");
            ok(pKeGetProcessorNumberFromIndex != NULL, "KeGetProcessorNumberFromIndex is not exported\n");
            ok(pKeGetProcessorIndexFromNumber != NULL, "KeGetProcessorIndexFromNumber is not exported\n");
        }
        else
        {
            skip(0, "Processor group routines are not available on this system\n");
        }
        return;
    }

    SingleGroup = (pKeQueryMaximumGroupCount() == 1);
    if (SingleGroup)
    {
        ok_eq_uint(pKeQueryActiveGroupCount(), 1);
    }

    Count = pKeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    trace("%lu active processors\n", Count);
    MaxCount = pKeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ok(MaxCount >= Count, "Maximum count %lu below active count %lu\n", MaxCount, Count);
    if (SingleGroup)
    {
        ok_eq_ulong(Count, (ULONG)KeNumberProcessors);
        ok_eq_ulong(pKeQueryActiveProcessorCountEx(0), Count);
        ok_eq_ulong(pKeQueryActiveProcessorCountEx(1), 0UL);
        ok_eq_ulong(pKeQueryMaximumProcessorCountEx(0), MaxCount);
        ok_eq_ulong(pKeQueryMaximumProcessorCountEx(1), 0UL);
    }
    else
    {
        skip(0, "More than one processor group\n");
    }

    for (Index = 0; Index < Count; Index++)
    {
        RtlFillMemory(&Number, sizeof(Number), 0x55);
        Status = pKeGetProcessorNumberFromIndex(Index, &Number);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (SingleGroup)
        {
            ok_eq_uint(Number.Group, 0);
            ok_eq_uint(Number.Number, Index);
        }
        ok_eq_uint(Number.Reserved, 0);
        ok_eq_ulong(pKeGetProcessorIndexFromNumber(&Number), Index);
    }

    /* Past every processor that can exist, not only the active ones */
    Status = pKeGetProcessorNumberFromIndex(MaxCount, &Number);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    if (SingleGroup)
    {
        Number.Group = 0;
        Number.Number = (UCHAR)MaxCount;
        Number.Reserved = 0;
        ok_eq_ulong(pKeGetProcessorIndexFromNumber(&Number), INVALID_PROCESSOR_INDEX);
        Number.Group = 1;
        Number.Number = 0;
        ok_eq_ulong(pKeGetProcessorIndexFromNumber(&Number), INVALID_PROCESSOR_INDEX);
    }

    /* Stay on one processor while comparing */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    RtlFillMemory(&Number, sizeof(Number), 0x55);
    Current = pKeGetCurrentProcessorNumberEx(&Number);
    ok_eq_ulong(pKeGetProcessorIndexFromNumber(&Number), Current);
    if (SingleGroup)
    {
        ok_eq_ulong(Current, KeGetCurrentProcessorNumber());
        ok_eq_uint(Number.Group, 0);
        ok_eq_uint(Number.Number, Current);
    }
    ok_eq_uint(Number.Reserved, 0);
    ok_eq_ulong(pKeGetCurrentProcessorNumberEx(NULL), Current);
    KeLowerIrql(OldIrql);
}
