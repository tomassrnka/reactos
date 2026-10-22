/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for QueryIdleProcessorCycleTime, QueryProcessCycleTime and
 *              QueryThreadCycleTime
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ndk/psfuncs.h>
#include <intrin.h>

#define MAX_CPUS 64
#define UNTOUCHED 0x5555555555555555ULL

static BOOL (WINAPI *pQueryIdleProcessorCycleTime)(PULONG, PULONG64);
static BOOL (WINAPI *pQueryProcessCycleTime)(HANDLE, PULONG64);
static BOOL (WINAPI *pQueryThreadCycleTime)(HANDLE, PULONG64);
static ULONG64 Times[MAX_CPUS + 2];

static
BOOL
Query(
    _In_ ULONG Length,
    _In_ BOOL WithBuffer,
    _Out_ PULONG ReturnedLength,
    _Out_ PDWORD Error)
{
    BOOL Success;

    memset(Times, 0x55, sizeof(Times));
    *ReturnedLength = Length;
    SetLastError(0xdeadbeef);
    Success = pQueryIdleProcessorCycleTime(ReturnedLength, WithBuffer ? Times : NULL);
    *Error = GetLastError();
    return Success;
}

static
VOID
TestIdleProcessorCycleTime(VOID)
{
    SYSTEM_INFO SystemInfo;
    ULONG64 First[MAX_CPUS];
    ULONG Processors, Expected, Length, i;
    DWORD Error;
    BOOL Success;

    GetSystemInfo(&SystemInfo);
    Processors = SystemInfo.dwNumberOfProcessors;
    if (Processors > MAX_CPUS)
    {
        skip("%lu processors\n", Processors);
        return;
    }
    Expected = Processors * sizeof(ULONG64);

    /* Without a buffer: success and the size it needs, whatever the length */
    Success = Query(0, FALSE, &Length, &Error);
    ok(Success, "Success = %d, error %lu\n", Success, Error);
    ok(Error == 0xdeadbeef, "Error = %lu\n", Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);

    Success = Query(Expected, FALSE, &Length, &Error);
    ok(Success, "Success = %d, error %lu\n", Success, Error);
    ok(Error == 0xdeadbeef, "Error = %lu\n", Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);

    /* Room for no processor */
    Success = Query(0, TRUE, &Length, &Error);
    ok(!Success, "Success = %d\n", Success);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error = %lu\n", Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);
    ok(Times[0] == UNTOUCHED, "Times[0] = 0x%I64x\n", Times[0]);

    Success = Query(sizeof(ULONG64) - 1, TRUE, &Length, &Error);
    ok(!Success, "Success = %d\n", Success);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error = %lu\n", Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);
    ok(Times[0] == UNTOUCHED, "Times[0] = 0x%I64x\n", Times[0]);

    /* Room for some processors: those are written */
    if (Processors > 1)
    {
        Success = Query(Expected - 1, TRUE, &Length, &Error);
        ok(!Success, "Success = %d\n", Success);
        ok(Error == ERROR_BAD_LENGTH, "Error = %lu\n", Error);
        ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);
        for (i = 0; i < Processors - 1; i++)
            ok(Times[i] != UNTOUCHED, "Times[%lu] not written\n", i);
        ok(Times[Processors - 1] == UNTOUCHED, "Times[%lu] = 0x%I64x\n",
           Processors - 1, Times[Processors - 1]);
    }
    else
    {
        skip("One processor\n");
    }

    /* Exact size */
    Success = Query(Expected, TRUE, &Length, &Error);
    ok(Success, "Success = %d, error %lu\n", Success, Error);
    ok(Error == 0xdeadbeef, "Error = %lu\n", Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);
    for (i = 0; i < Processors; i++)
    {
        ok(Times[i] != UNTOUCHED && Times[i] != 0, "Times[%lu] = 0x%I64x\n", i, Times[i]);
        First[i] = Times[i];
    }
    ok(Times[Processors] == UNTOUCHED, "Times[%lu] = 0x%I64x\n", Processors, Times[Processors]);

    Sleep(100);

    /* A larger buffer: only the processors are written */
    Success = Query(sizeof(Times), TRUE, &Length, &Error);
    ok(Success, "Success = %d, error %lu\n", Success, Error);
    ok(Length == Expected, "Length = %lu, expected %lu\n", Length, Expected);
    ok(Times[Processors] == UNTOUCHED, "Times[%lu] = 0x%I64x\n", Processors, Times[Processors]);

    for (i = 0; i < Processors; i++)
        ok(Times[i] >= First[i], "Times[%lu] = %I64u < %I64u\n", i, Times[i], First[i]);
}

/*
 * Fixed work instead of a time: the counters are in time stamp counter
 * ticks, which can be fewer than core cycles, so expect a tenth of it
 */
#define SPIN_ITERATIONS 50000000
#define SPIN_MIN_CYCLES (SPIN_ITERATIONS / 10)

static
VOID
Spin(VOID)
{
    volatile ULONG Counter;

    for (Counter = 0; Counter < SPIN_ITERATIONS; Counter++)
        ;
}

static
DWORD
WINAPI
SpinThread(
    _In_ PVOID Parameter)
{
    Spin();
    return 0;
}

/* The information classes behind QueryProcessCycleTime and QueryThreadCycleTime */
static
VOID
TestCycleTimeClasses(VOID)
{
    struct
    {
        ULONGLONG AccumulatedCycles;
        ULONGLONG CurrentCycleCount;
        ULONGLONG Extra;
    } Info;
    ULONG64 Before, After;
    ULONG Length;
    NTSTATUS Status;
    DWORD_PTR ProcessMask, SystemMask, OldMask;
    BOOL IsWow64 = FALSE;

    /* The cycle counters of different processors need not agree */
    GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask);
    OldMask = SetThreadAffinityMask(GetCurrentThread(), ProcessMask & (~ProcessMask + 1));
    ok(OldMask != 0, "SetThreadAffinityMask failed with %lu\n", GetLastError());
    Sleep(0);
    IsWow64Process(GetCurrentProcess(), &IsWow64);

    Length = 0xdeadbeef;
    Before = __rdtsc();
    Status = NtQueryInformationProcess(GetCurrentProcess(), ProcessCycleTime, &Info, 16, &Length);
    After = __rdtsc();
    ok(Status == STATUS_SUCCESS, "Status = 0x%lx\n", Status);
    ok(Length == 16, "Length = %lu\n", Length);
    ok(Info.AccumulatedCycles != 0, "No process cycles\n");
    ok(Info.CurrentCycleCount >= Before && Info.CurrentCycleCount <= After,
       "CurrentCycleCount %I64u not in [%I64u, %I64u]\n", Info.CurrentCycleCount, Before, After);

    /* A wrong length: native Windows leaves the length alone, WOW64 writes 16 (NT 6.1) or 0 (NT 10) */
    Length = 0xdeadbeef;
    Status = NtQueryInformationProcess(GetCurrentProcess(), ProcessCycleTime, &Info, 8, &Length);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "Status = 0x%lx\n", Status);
    ok(IsWow64 ? (Length == 16 || Length == 0) : (Length == 0xdeadbeef), "Length = %lu\n", Length);
    Length = 0xdeadbeef;
    Status = NtQueryInformationProcess(GetCurrentProcess(), ProcessCycleTime, &Info, 24, &Length);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "Status = 0x%lx\n", Status);
    ok(IsWow64 ? (Length == 16 || Length == 0) : (Length == 0xdeadbeef), "Length = %lu\n", Length);

    Length = 0xdeadbeef;
    Before = __rdtsc();
    Status = NtQueryInformationThread(GetCurrentThread(), ThreadCycleTime, &Info, 16, &Length);
    After = __rdtsc();
    ok(Status == STATUS_SUCCESS, "Status = 0x%lx\n", Status);
    ok(Length == 16, "Length = %lu\n", Length);
    ok(Info.AccumulatedCycles != 0, "No thread cycles\n");
    ok(Info.CurrentCycleCount >= Before && Info.CurrentCycleCount <= After,
       "CurrentCycleCount %I64u not in [%I64u, %I64u]\n", Info.CurrentCycleCount, Before, After);

    Length = 0xdeadbeef;
    Status = NtQueryInformationThread(GetCurrentThread(), ThreadCycleTime, &Info, 8, &Length);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "Status = 0x%lx\n", Status);
    ok(IsWow64 ? (Length == 16 || Length == 0) : (Length == 0xdeadbeef), "Length = %lu\n", Length);
    Length = 0xdeadbeef;
    Status = NtQueryInformationThread(GetCurrentThread(), ThreadCycleTime, &Info, 24, &Length);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "Status = 0x%lx\n", Status);
    ok(IsWow64 ? (Length == 16 || Length == 0) : (Length == 0xdeadbeef), "Length = %lu\n", Length);

    if (OldMask)
        SetThreadAffinityMask(GetCurrentThread(), OldMask);
}

static
VOID
TestProcessThreadCycleTime(VOID)
{
    ULONG64 Cycles, Cycles2, ProcessBefore, ProcessAfter, SpinGrowth, SleepGrowth;
    HANDLE hThread;
    BOOL Success;

    /* Invalid handles */
    Cycles = 0x5555;
    SetLastError(0xdeadbeef);
    Success = pQueryProcessCycleTime(NULL, &Cycles);
    ok(!Success, "Success = %d\n", Success);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "Error = %lu\n", GetLastError());
    ok(Cycles == 0x5555, "Cycles = 0x%I64x\n", Cycles);

    Cycles = 0x5555;
    SetLastError(0xdeadbeef);
    Success = pQueryThreadCycleTime(NULL, &Cycles);
    ok(!Success, "Success = %d\n", Success);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "Error = %lu\n", GetLastError());
    ok(Cycles == 0x5555, "Cycles = 0x%I64x\n", Cycles);

    /* The current process and thread */
    Cycles = 0;
    SetLastError(0xdeadbeef);
    Success = pQueryProcessCycleTime(GetCurrentProcess(), &Cycles);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    ok(GetLastError() == 0xdeadbeef, "Error = %lu\n", GetLastError());
    ok(Cycles != 0, "No process cycles\n");

    Cycles = 0;
    SetLastError(0xdeadbeef);
    Success = pQueryThreadCycleTime(GetCurrentThread(), &Cycles);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    ok(GetLastError() == 0xdeadbeef, "Error = %lu\n", GetLastError());
    ok(Cycles != 0, "No thread cycles\n");

    /* A thread counts while it runs, hardly while it waits */
    Success = pQueryThreadCycleTime(GetCurrentThread(), &Cycles);
    Spin();
    Success = pQueryThreadCycleTime(GetCurrentThread(), &Cycles2) && Success;
    ok(Success && Cycles2 >= Cycles, "Cycles went from %I64u to %I64u\n", Cycles, Cycles2);
    SpinGrowth = (Cycles2 >= Cycles) ? Cycles2 - Cycles : 0;
    ok(SpinGrowth >= SPIN_MIN_CYCLES, "Spinning added %I64u cycles\n", SpinGrowth);

    Success = pQueryThreadCycleTime(GetCurrentThread(), &Cycles);
    Sleep(200);
    Success = pQueryThreadCycleTime(GetCurrentThread(), &Cycles2) && Success;
    ok(Success && Cycles2 >= Cycles, "Cycles went from %I64u to %I64u\n", Cycles, Cycles2);
    SleepGrowth = (Cycles2 >= Cycles) ? Cycles2 - Cycles : ~0ULL;
    ok(SpinGrowth > SleepGrowth, "Spinning added %I64u cycles, sleeping %I64u\n",
       SpinGrowth, SleepGrowth);

    /* The process keeps the cycles of a thread that has exited */
    Success = pQueryProcessCycleTime(GetCurrentProcess(), &ProcessBefore);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    hThread = CreateThread(NULL, 0, SpinThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!hThread)
        return;
    WaitForSingleObject(hThread, INFINITE);

    Cycles = 0;
    Success = pQueryThreadCycleTime(hThread, &Cycles);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    ok(Cycles >= SPIN_MIN_CYCLES, "The exited thread used %I64u cycles\n", Cycles);
    CloseHandle(hThread);

    Success = pQueryProcessCycleTime(GetCurrentProcess(), &ProcessAfter);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    /* Windows Server 2008 R2 sometimes counts a little less for the process than
     * for the thread (0.1 %); a process that dropped the thread would lose it all */
    ok(ProcessAfter >= ProcessBefore && ProcessAfter - ProcessBefore >= Cycles - Cycles / 8,
       "The process went from %I64u to %I64u, the thread used %I64u\n",
       ProcessBefore, ProcessAfter, Cycles);
}

START_TEST(QueryCycleTime)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pQueryIdleProcessorCycleTime = (PVOID)GetProcAddress(hKernel32, "QueryIdleProcessorCycleTime");
    pQueryProcessCycleTime = (PVOID)GetProcAddress(hKernel32, "QueryProcessCycleTime");
    pQueryThreadCycleTime = (PVOID)GetProcAddress(hKernel32, "QueryThreadCycleTime");
    if (!pQueryIdleProcessorCycleTime || !pQueryProcessCycleTime || !pQueryThreadCycleTime)
    {
        skip("The cycle time functions are not available\n");
        return;
    }

    TestIdleProcessorCycleTime();
    TestCycleTimeClasses();
    TestProcessThreadCycleTime();
}
