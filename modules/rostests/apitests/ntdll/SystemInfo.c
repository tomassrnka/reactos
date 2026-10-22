/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for NtQuery/SetSystemInformation
 * PROGRAMMERS:     Timo Kreuzer
 *                  Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"
#include <versionhelpers.h>
#include <intrin.h>

#define ntv6(x) (LOBYTE(LOWORD(GetVersion())) >= 6 ? (x) : 0)

static
void
Test_Flags(void)
{
    NTSTATUS Status;
    ULONG ReturnLength;
    ULONG Flags;
    ULONG Buffer[2];
    ULONG Buffer2[2];
    PSYSTEM_FLAGS_INFORMATION FlagsInfo = (PVOID)Buffer;
    BOOLEAN PrivilegeEnabled;

    /* Make sure we don't have debug privileges initially, otherwise WHS testbot fails */
    Status = RtlAdjustPrivilege(SE_SYSTEMTIME_PRIVILEGE, FALSE, FALSE, &PrivilegeEnabled);
    ok(Status == STATUS_SUCCESS, "RtlAdjustPrivilege returned %lx\n", Status);

    /* Query */
    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(SystemFlagsInformation, NULL, 0, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(SystemFlagsInformation, NULL, sizeof(SYSTEM_FLAGS_INFORMATION) - 1, &ReturnLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0x55555555, "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(SystemFlagsInformation, (PVOID)2, 0, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(SystemFlagsInformation, (PVOID)4, 0, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, (PUCHAR)FlagsInfo + 2, sizeof(SYSTEM_FLAGS_INFORMATION), &ReturnLength);
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0x55555555, "ReturnLength = %lu\n", ReturnLength);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, (PUCHAR)FlagsInfo + 2, sizeof(SYSTEM_FLAGS_INFORMATION) - 1, &ReturnLength);
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0x55555555, "ReturnLength = %lu\n", ReturnLength);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION) - 1, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), &ReturnLength);
    ok(Status == STATUS_SUCCESS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION), "ReturnLength = %lu\n", ReturnLength);
    ok(FlagsInfo->Flags != 0x55555555, "Flags = %lx\n", FlagsInfo->Flags);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);
    Flags = FlagsInfo->Flags;

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION) + 1, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_FLAGS_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), NULL);
    ok(Status == STATUS_SUCCESS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(FlagsInfo->Flags == Flags, "Flags = %lx, expected %lx\n", FlagsInfo->Flags, Flags);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), (PVOID)4);
    ok(Status == STATUS_ACCESS_VIOLATION, "NtQuerySystemInformation returned %lx\n", Status);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), (PVOID)2);
    ok(Status == STATUS_ACCESS_VIOLATION, "NtQuerySystemInformation returned %lx\n", Status);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), (PVOID)1);
    ok(Status == STATUS_ACCESS_VIOLATION, "NtQuerySystemInformation returned %lx\n", Status);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    RtlFillMemory(Buffer2, sizeof(Buffer2), 0x55);
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION), (PULONG)((PUCHAR)Buffer2 + 1));
    ok(Status == STATUS_SUCCESS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(FlagsInfo->Flags == Flags, "Flags = %lx, expected %lx\n", FlagsInfo->Flags, Flags);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);
    ok(Buffer2[0] == 0x00000455, "Buffer2[0] = %lx\n", Buffer2[0]);
    ok(Buffer2[1] == 0x55555500, "Buffer2[1] = %lx\n", Buffer2[1]);

    /* Set */
    Status = NtSetSystemInformation(SystemFlagsInformation, NULL, 0);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, NULL, sizeof(SYSTEM_FLAGS_INFORMATION) - 1);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, NULL, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == (IsWindows7OrGreater() ? STATUS_ACCESS_DENIED : STATUS_ACCESS_VIOLATION), "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, (PVOID)2, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtSetSystemInformation returned %lx\n", Status);

    Status = RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE, TRUE, FALSE, &PrivilegeEnabled);
    if (!NT_SUCCESS(Status))
    {
        skip("Cannot acquire SeDebugPrivilege\n");
        return;
    }

    Status = NtSetSystemInformation(SystemFlagsInformation, NULL, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_ACCESS_VIOLATION, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, (PVOID)2, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, (PVOID)4, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_ACCESS_VIOLATION, "NtSetSystemInformation returned %lx\n", Status);

    FlagsInfo->Flags = Flags;
    Status = NtSetSystemInformation(SystemFlagsInformation, (PUCHAR)FlagsInfo + 2, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, (PUCHAR)FlagsInfo + 2, sizeof(SYSTEM_FLAGS_INFORMATION) - 1);
    ok(Status == STATUS_DATATYPE_MISALIGNMENT, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION) - 1);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION) + 1);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtSetSystemInformation returned %lx\n", Status);

    Status = NtSetSystemInformation(SystemFlagsInformation, FlagsInfo, sizeof(SYSTEM_FLAGS_INFORMATION));
    ok(Status == STATUS_SUCCESS, "NtSetSystemInformation returned %lx\n", Status);

    ok(FlagsInfo->Flags == Flags, "Flags = %lx, expected %lu\n", FlagsInfo->Flags, Flags);

    Status = RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE, PrivilegeEnabled, FALSE, &PrivilegeEnabled);
    ok(Status == STATUS_SUCCESS, "RtlAdjustPrivilege returned %lx\n", Status);
}

static
void
Test_TimeAdjustment(void)
{
    SYSTEM_QUERY_TIME_ADJUST_INFORMATION TimeInfoOrg, GetTimeInfo;
    SYSTEM_SET_TIME_ADJUST_INFORMATION SetTimeInfo;
    NTSTATUS Status;
    ULONG ReturnLength;
    BOOLEAN PrivilegeEnabled;

    /* Make sure we don't have debug privileges initially, otherwise WHS testbot fails */
    Status = RtlAdjustPrivilege(SE_SYSTEMTIME_PRIVILEGE, FALSE, FALSE, &PrivilegeEnabled);
    ok(Status == STATUS_SUCCESS, "RtlAdjustPrivilege returned %lx\n", Status);

    SetTimeInfo.TimeAdjustment = 0;
    SetTimeInfo.Enable = 0;

    /* Query original values */
    Status = NtQuerySystemInformation(SystemTimeAdjustmentInformation,
                                      &TimeInfoOrg,
                                      sizeof(TimeInfoOrg),
                                      &ReturnLength);

    /* Test without privilege */
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo));
    ok_ntstatus(Status, STATUS_PRIVILEGE_NOT_HELD);

    /* Get the required privilege */
    Status = RtlAdjustPrivilege(SE_SYSTEMTIME_PRIVILEGE, TRUE, FALSE, &PrivilegeEnabled);
    if (!NT_SUCCESS(Status))
    {
        skip("Cannot acquire SeSystemTimePrivilege\n");
        return;
    }

    /* Test wrong length */
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo) + 1);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Test both members 0 */
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo));
    ok_ntstatus(Status, IsWindows7OrGreater() ? STATUS_INTEGER_DIVIDE_BY_ZERO : STATUS_INVALID_PARAMETER_2);

    /* Set huge value */
    SetTimeInfo.TimeAdjustment = -1;
    SetTimeInfo.Enable = 0;
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo));
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Query the result */
    Status = NtQuerySystemInformation(SystemTimeAdjustmentInformation,
                                      &GetTimeInfo,
                                      sizeof(GetTimeInfo),
                                      &ReturnLength);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_long(GetTimeInfo.TimeAdjustment, IsWindows7OrGreater() ? 0x7076cc : -1);
    ok_long(GetTimeInfo.Enable, 0);

    /* set Enable to 1 */
    SetTimeInfo.TimeAdjustment = -1;
    SetTimeInfo.Enable = 1;
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo));
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Query the result */
    Status = NtQuerySystemInformation(SystemTimeAdjustmentInformation,
                                      &GetTimeInfo,
                                      sizeof(GetTimeInfo),
                                      &ReturnLength);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_long(GetTimeInfo.TimeAdjustment, GetTimeInfo.TimeIncrement);
    ok_long(GetTimeInfo.Enable, 1);

    /* Restore original values */
    SetTimeInfo.TimeAdjustment = TimeInfoOrg.TimeAdjustment;
    SetTimeInfo.Enable = TimeInfoOrg.Enable;
    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &SetTimeInfo,
                                    sizeof(SetTimeInfo));
    ok_ntstatus(Status, STATUS_SUCCESS);

    Status = RtlAdjustPrivilege(SE_SYSTEMTIME_PRIVILEGE, PrivilegeEnabled, FALSE, &PrivilegeEnabled);
    ok(Status == STATUS_SUCCESS, "RtlAdjustPrivilege returned %lx\n", Status);
}

static
void
Test_KernelDebugger(void)
{
    NTSTATUS Status;
    ULONG ReturnLength;
    ULONG Buffer[2];
    PSYSTEM_KERNEL_DEBUGGER_INFORMATION DebuggerInfo = (PVOID)Buffer;

    /* Query */
    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(SystemKernelDebuggerInformation, NULL, 0, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemKernelDebuggerInformation, DebuggerInfo, sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION) - 1, &ReturnLength);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION)), "ReturnLength = %lu\n", ReturnLength);
    ok(Buffer[0] == 0x55555555, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemKernelDebuggerInformation, (PUCHAR)DebuggerInfo + 1, sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION), &ReturnLength);
    ok(Status == STATUS_SUCCESS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION), "ReturnLength = %lu\n", ReturnLength);
    ok((Buffer[0] & 0x55fefe55) == 0x55000055, "Buffer[0] = %lx\n", Buffer[0]);
    ok(Buffer[1] == 0x55555555, "Buffer[1] = %lx\n", Buffer[1]);

    ReturnLength = 0x55555555;
    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Status = NtQuerySystemInformation(SystemKernelDebuggerInformation, DebuggerInfo, sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION), &ReturnLength);
    ok(Status == STATUS_SUCCESS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION), "ReturnLength = %lu\n", ReturnLength);
    ok(DebuggerInfo->KernelDebuggerEnabled == FALSE ||
       DebuggerInfo->KernelDebuggerEnabled == TRUE, "KernelDebuggerEnabled = %u\n", DebuggerInfo->KernelDebuggerEnabled);
    ok(DebuggerInfo->KernelDebuggerNotPresent == FALSE ||
       DebuggerInfo->KernelDebuggerNotPresent == TRUE, "KernelDebuggerNotPresent = %u\n", DebuggerInfo->KernelDebuggerNotPresent);

    /* Set - not supported */
    DebuggerInfo->KernelDebuggerEnabled = FALSE;
    DebuggerInfo->KernelDebuggerNotPresent = TRUE;
    Status = NtSetSystemInformation(SystemKernelDebuggerInformation, DebuggerInfo, sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION));
    ok(Status == STATUS_INVALID_INFO_CLASS, "NtSetSystemInformation returned %lx\n", Status);
}

#define MAX_PROCESSORS (sizeof(DWORD_PTR) * 8)

/* The process list, in a heap buffer that grows as needed */
static
PSYSTEM_PROCESS_INFORMATION
QueryProcessList(VOID)
{
    PVOID Buffer;
    ULONG Size = 0x40000, Length, Try;
    NTSTATUS Status;

    for (Try = 0; Try < 8; Try++)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Buffer)
            break;
        Length = 0;
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, &Length);
        if (NT_SUCCESS(Status))
            return Buffer;
        HeapFree(GetProcessHeap(), 0, Buffer);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
        {
            ok_hex(Status, STATUS_SUCCESS);
            return NULL;
        }
        Size = max(Size * 2, Length + 0x10000);
    }
    ok(FALSE, "Cannot query the process list\n");
    return NULL;
}

/* CycleTime and user plus kernel time of a process from SystemProcessInformation */
static
BOOLEAN
QueryProcessCycles(
    _In_ HANDLE ProcessId,
    _Out_ PULONG64 CycleTime,
    _Out_ PLONGLONG Time)
{
    PSYSTEM_PROCESS_INFORMATION List, ProcessInfo;

    List = QueryProcessList();
    if (!List)
        return FALSE;

    for (ProcessInfo = List; ; ProcessInfo = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)ProcessInfo + ProcessInfo->NextEntryOffset))
    {
        if (ProcessInfo->UniqueProcessId == ProcessId)
        {
            *CycleTime = ProcessInfo->CycleTime;
            *Time = ProcessInfo->UserTime.QuadPart + ProcessInfo->KernelTime.QuadPart;
            HeapFree(GetProcessHeap(), 0, List);
            return TRUE;
        }
        if (ProcessInfo->NextEntryOffset == 0)
            break;
    }
    HeapFree(GetProcessHeap(), 0, List);
    ok(FALSE, "Process %p is not in the process list\n", ProcessId);
    return FALSE;
}

/* The idle time of each processor, in 100 ns units */
static
BOOLEAN
QueryIdleTimes(
    _In_ ULONG Processors,
    _Out_writes_(Processors) PLONGLONG IdleTimes)
{
    PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION Info;
    NTSTATUS Status;
    ULONG i;

    Info = HeapAlloc(GetProcessHeap(), 0, Processors * sizeof(*Info));
    if (!Info)
        return FALSE;
    Status = NtQuerySystemInformation(SystemProcessorPerformanceInformation, Info, Processors * sizeof(*Info), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Processors; i++)
            IdleTimes[i] = Info[i].IdleTime.QuadPart;
    }
    HeapFree(GetProcessHeap(), 0, Info);
    return NT_SUCCESS(Status);
}

/* Run once on each processor. An idle thread that was running there switches out,
   which adds its current run to the idle process on systems that count it at a context switch. */
static
DWORD_PTR
VisitProcessors(
    _In_ DWORD_PTR ProcessMask)
{
    DWORD_PTR Mask, Visited = 0;

    for (Mask = 1; Mask != 0; Mask <<= 1)
    {
        if (!(ProcessMask & Mask))
            continue;
        if (SetThreadAffinityMask(GetCurrentThread(), Mask))
        {
            Sleep(1);
            Visited |= Mask;
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), ProcessMask);
    return Visited;
}

static
void
Test_ProcessCycleTime(void)
{
    HANDLE Self = NtCurrentTeb()->ClientId.UniqueProcess;
    ULONG64 Cycles1, Cycles2, Tsc1, Tsc2, TscPerSecond, Expected;
    LONGLONG Time1, Time2, Idle1[MAX_PROCESSORS], Idle2[MAX_PROCESSORS];
    LARGE_INTEGER Qpc1, Qpc2, Frequency;
    SYSTEM_INFO SystemInfo;
    DWORD_PTR ProcessMask, SystemMask, Visited;
    DWORD Start;
    ULONG Try, i;

    /* The field was added in Windows 7 */
    if (!is_reactos() && GetNTVersion() < _WIN32_WINNT_WIN7)
    {
        skip("SystemProcessInformation has no CycleTime before Windows 7\n");
        return;
    }

    GetSystemInfo(&SystemInfo);
    QueryPerformanceFrequency(&Frequency);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask) || !ProcessMask ||
        SystemInfo.dwNumberOfProcessors > MAX_PROCESSORS)
    {
        skip("Cannot get the processors of the process\n");
        return;
    }

    /* A spinning process is charged about the time stamp counter ticks of the time it ran.
       The spin stays on one processor, so that both counter reads come from the same counter.
       The waits let the thread's current run reach the process total on systems that add it
       at a context switch. */
    SetThreadAffinityMask(GetCurrentThread(), ProcessMask & (~ProcessMask + 1));
    Sleep(10);
    if (!QueryProcessCycles(Self, &Cycles1, &Time1))
        return;
    QueryPerformanceCounter(&Qpc1);
    Tsc1 = __rdtsc();
    Start = GetTickCount();
    while (GetTickCount() - Start < 300)
        ;
    Tsc2 = __rdtsc();
    QueryPerformanceCounter(&Qpc2);
    Sleep(10);
    SetThreadAffinityMask(GetCurrentThread(), ProcessMask);
    if (!QueryProcessCycles(Self, &Cycles2, &Time2))
        return;

    if (Time2 - Time1 < 100 * 10000 || Qpc2.QuadPart <= Qpc1.QuadPart || Tsc2 <= Tsc1)
    {
        skip("The process ran %I64d ms in a 300 ms spin\n", (Time2 - Time1) / 10000);
    }
    else
    {
        TscPerSecond = (Tsc2 - Tsc1) * Frequency.QuadPart / (Qpc2.QuadPart - Qpc1.QuadPart);
        Expected = (ULONG64)(Time2 - Time1) * (TscPerSecond / 10000) / 1000;
        ok(Cycles2 >= Cycles1 && Cycles2 - Cycles1 >= Expected / 2,
           "CycleTime grew by %I64d while the process ran %I64d ms (%I64u ticks expected)\n",
           (LONG64)(Cycles2 - Cycles1), (Time2 - Time1) / 10000, Expected);
    }

    /* Idle processors are charged to the idle process. The idle time is sampled inside the
       cycle sample, and every processor is visited before each cycle sample. Try again while
       no processor was idle. */
    for (Try = 0; Try < 10; Try++)
    {
        Visited = VisitProcessors(ProcessMask);
        if (!QueryProcessCycles(NULL, &Cycles1, &Time1) ||
            !QueryIdleTimes(SystemInfo.dwNumberOfProcessors, Idle1))
            return;
        Sleep(500);
        if (!QueryIdleTimes(SystemInfo.dwNumberOfProcessors, Idle2))
            return;
        Visited &= VisitProcessors(ProcessMask);
        if (!QueryProcessCycles(NULL, &Cycles2, &Time2))
            return;

        /* Only a processor visited both times has published its idle cycles */
        for (i = 0; i < SystemInfo.dwNumberOfProcessors; i++)
        {
            if ((Visited & ((DWORD_PTR)1 << i)) && Idle2[i] - Idle1[i] >= 100 * 10000)
                break;
        }
        if (i < SystemInfo.dwNumberOfProcessors)
            break;
    }
    if (Try == 10)
        skip("No processor was idle for 100 ms in 500 ms, ten times\n");
    else
        ok(Cycles2 > Cycles1, "The idle process' CycleTime did not grow while processor %lu was idle %I64d ms (%I64u)\n",
           i, (Idle2[i] - Idle1[i]) / 10000, Cycles1);
}

/* The processor cores must cover every active processor once */
static
void
Test_LogicalProcessorInformation(void)
{
    NTSTATUS Status;
    SYSTEM_BASIC_INFORMATION BasicInfo;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Info = NULL;
    ULONG Length = 0, Count, i;
    KAFFINITY Cores = 0;
    BOOL IsWow64 = FALSE;

    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
    {
        skip("WOW64 does not pass SystemLogicalProcessorInformation on\n");
        return;
    }

    Status = NtQuerySystemInformation(SystemBasicInformation, &BasicInfo, sizeof(BasicInfo), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length != 0 && (Length % sizeof(*Info)) == 0, "Length %lu\n", Length);
    if (Length == 0)
        return;

    Info = HeapAlloc(GetProcessHeap(), 0, Length);
    if (!Info)
    {
        skip("Out of memory\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation, Info, Length, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Count = Length / sizeof(*Info);
        for (i = 0; i < Count; i++)
        {
            if (Info[i].Relationship != RelationProcessorCore)
                continue;
            ok((Cores & Info[i].ProcessorMask) == 0, "Core %lu (mask %Ix) overlaps an earlier core (%Ix)\n",
               i, Info[i].ProcessorMask, Cores);
            Cores |= Info[i].ProcessorMask;
        }
        ok(Cores == BasicInfo.ActiveProcessorsAffinityMask,
           "The processor cores cover %Ix, the active processors are %Ix (%u processors)\n",
           Cores, BasicInfo.ActiveProcessorsAffinityMask, BasicInfo.NumberOfProcessors);
    }

    HeapFree(GetProcessHeap(), 0, Info);
}

START_TEST(NtSystemInformation)
{
    NTSTATUS Status;
    ULONG ReturnLength;

    Status = NtQuerySystemInformation(9999, NULL, 0, NULL);
    ok(Status == STATUS_INVALID_INFO_CLASS, "NtQuerySystemInformation returned %lx\n", Status);

    Status = NtQuerySystemInformation(9999, NULL, 0, (PVOID)1);
    ok(Status == STATUS_ACCESS_VIOLATION ||
       ntv6(Status == STATUS_INVALID_INFO_CLASS), "NtQuerySystemInformation returned %lx\n", Status);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(9999, NULL, 0, &ReturnLength);
    ok(Status == STATUS_INVALID_INFO_CLASS, "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0 ||
       ntv6(ReturnLength == 0x55555555), "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(9999, NULL, 1, &ReturnLength);
    ok(Status == STATUS_ACCESS_VIOLATION ||
       ntv6(Status == STATUS_INVALID_INFO_CLASS), "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0x55555555, "ReturnLength = %lu\n", ReturnLength);

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(9999, (PVOID)1, 1, &ReturnLength);
    ok(Status == STATUS_DATATYPE_MISALIGNMENT ||
       ntv6(Status == STATUS_INVALID_INFO_CLASS), "NtQuerySystemInformation returned %lx\n", Status);
    ok(ReturnLength == 0x55555555, "ReturnLength = %lu\n", ReturnLength);

    Status = NtQuerySystemInformation(9999, NULL, 1, (PVOID)1);
    ok(Status == STATUS_ACCESS_VIOLATION ||
       ntv6(Status == STATUS_INVALID_INFO_CLASS), "NtQuerySystemInformation returned %lx\n", Status);

    Status = NtQuerySystemInformation(9999, (PVOID)1, 1, (PVOID)1);
    ok(Status == STATUS_DATATYPE_MISALIGNMENT ||
       ntv6(Status == STATUS_INVALID_INFO_CLASS), "NtQuerySystemInformation returned %lx\n", Status);

    Test_Flags();
    Test_TimeAdjustment();
    Test_KernelDebugger();
    Test_ProcessCycleTime();
    Test_LogicalProcessorInformation();
}
