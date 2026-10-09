/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Test that a ready thread runs on an idle processor when its
 *              ideal processor is busy
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define ITERATIONS 10
#define MIN_IDLE_RUNS 3
#define SPIN_MS 2000
#define MAX_START_MS 200

typedef enum _RUN_RESULT
{
    RunError,
    RunInvalid,
    RunIdle,
    RunBusy
} RUN_RESULT;

static ULONG (NTAPI *pRtlGetCurrentProcessorNumber)(VOID);
static volatile LONG s_SpinnerRunning;
static volatile LONG s_SpinnerExpired;
static volatile LONG s_StopSpinner;
static LARGE_INTEGER s_Frequency;
static volatile DWORD s_ProbeProcessor;
static volatile DWORD s_ProbeTick;
static volatile LONG s_ProbeSawExpired;
static HANDLE s_ProbeReady, s_ProbeGo, s_ProbeRan;

static DWORD WINAPI
SpinnerThread(PVOID Parameter)
{
    ULONG BusyProcessor = PtrToUlong(Parameter);
    LARGE_INTEGER Now, Deadline;
    ULONG i;

    /* Pin itself while it runs, rather than being moved before it starts */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << BusyProcessor);
    for (i = 0; i < 100 && pRtlGetCurrentProcessorNumber() != BusyProcessor; i++)
        Sleep(1);
    if (pRtlGetCurrentProcessorNumber() != BusyProcessor)
        return 1;

    QueryPerformanceCounter(&Deadline);
    Deadline.QuadPart += s_Frequency.QuadPart * SPIN_MS / 1000;

    InterlockedExchange(&s_SpinnerRunning, 1);
    do
    {
        QueryPerformanceCounter(&Now);
    } while (!s_StopSpinner && Now.QuadPart < Deadline.QuadPart);

    if (!s_StopSpinner)
        InterlockedExchange(&s_SpinnerExpired, 1);
    return 0;
}

static DWORD WINAPI
ProbeThread(PVOID Parameter)
{
    SetEvent(s_ProbeReady);
    WaitForSingleObject(s_ProbeGo, INFINITE);
    s_ProbeTick = GetTickCount();
    s_ProbeSawExpired = s_SpinnerExpired;
    s_ProbeProcessor = pRtlGetCurrentProcessorNumber();
    SetEvent(s_ProbeRan);
    return 0;
}

static RUN_RESULT
RunOnce(ULONG BusyProcessor)
{
    HANDLE Spinner, Probe;
    DWORD Wait, Start, i;
    LONG Ms;
    RUN_RESULT Result;

    s_SpinnerRunning = 0;
    s_SpinnerExpired = 0;
    s_StopSpinner = 0;
    s_ProbeProcessor = MAXDWORD;
    s_ProbeSawExpired = 0;

    /* The probe may run anywhere but prefers the busy processor */
    ResetEvent(s_ProbeGo);
    ResetEvent(s_ProbeRan);
    Probe = CreateThread(NULL, 0, ProbeThread, NULL, CREATE_SUSPENDED, NULL);
    ok(Probe != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Probe)
        return RunError;
    ok(SetThreadIdealProcessor(Probe, BusyProcessor) != (DWORD)-1,
       "SetThreadIdealProcessor failed: %lu\n", GetLastError());
    ResumeThread(Probe);
    ok(WaitForSingleObject(s_ProbeReady, 5000) == WAIT_OBJECT_0,
       "Probe did not start\n");

    /* Keep the busy processor occupied with a higher-priority thread */
    Spinner = CreateThread(NULL, 0, SpinnerThread, UlongToPtr(BusyProcessor), 0, NULL);
    ok(Spinner != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Spinner)
    {
        SetEvent(s_ProbeGo);
        WaitForSingleObject(Probe, INFINITE);
        CloseHandle(Probe);
        return RunError;
    }
    for (i = 0; i < 1000 && !s_SpinnerRunning; i++)
        Sleep(1);
    ok(s_SpinnerRunning, "Spinner did not start on processor %lu\n", BusyProcessor);
    Result = s_SpinnerRunning ? RunBusy : RunError;

    /* Let the probe block and other woken threads go idle again */
    Sleep(200);

    /* The tick count is shared by all processors, unlike the performance counter */
    Start = GetTickCount();
    if (Result == RunBusy && s_SpinnerExpired)
        Result = RunInvalid;
    SetEvent(s_ProbeGo);
    Wait = WaitForSingleObject(s_ProbeRan, SPIN_MS + 3000);
    ok(Wait == WAIT_OBJECT_0, "Probe wait returned %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        Result = RunError;

    InterlockedExchange(&s_StopSpinner, 1);
    WaitForSingleObject(Spinner, INFINITE);
    WaitForSingleObject(Probe, INFINITE);
    CloseHandle(Spinner);
    CloseHandle(Probe);

    if (Result == RunBusy)
    {
        Ms = (LONG)(s_ProbeTick - Start);
        trace("Probe ran on processor %lu after %ld ms\n", s_ProbeProcessor, Ms);
        if (s_ProbeProcessor != BusyProcessor)
        {
            /* Elsewhere, it only counts while the busy processor was busy */
            if (s_ProbeSawExpired)
                Result = RunInvalid;
            else if (Ms < MAX_START_MS)
                Result = RunIdle;
        }
    }
    return Result;
}

START_TEST(IdleProcessor)
{
    DWORD_PTR ProcessMask, SystemMask, OldMask;
    ULONG Count = 0, BusyProcessor = MAXULONG, MainProcessor = 0, i;
    ULONG Valid = 0, Idle = 0;
    RUN_RESULT Result = RunInvalid;

    pRtlGetCurrentProcessorNumber = (PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                          "RtlGetCurrentProcessorNumber");
    if (!pRtlGetCurrentProcessorNumber)
    {
        skip("RtlGetCurrentProcessorNumber is not available\n");
        return;
    }

    if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask))
    {
        ok(0, "GetProcessAffinityMask failed: %lu\n", GetLastError());
        return;
    }
    for (i = 0; i < sizeof(ProcessMask) * 8; i++)
    {
        if (!(ProcessMask & ((DWORD_PTR)1 << i)))
            continue;
        if (BusyProcessor == MAXULONG)
            BusyProcessor = i;
        MainProcessor = i;
        Count++;
    }

    /* One processor runs the spinner, one this thread, one must stay idle */
    if (Count < 3)
    {
        skip("Need at least 3 processors, have %lu\n", Count);
        return;
    }
    QueryPerformanceFrequency(&s_Frequency);
    s_ProbeReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_ProbeGo = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_ProbeRan = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(s_ProbeReady && s_ProbeGo && s_ProbeRan,
       "CreateEventW failed: %lu\n", GetLastError());

    OldMask = SetThreadAffinityMask(GetCurrentThread(),
                                    (DWORD_PTR)1 << MainProcessor);
    ok(OldMask != 0, "SetThreadAffinityMask failed: %lu\n", GetLastError());
    Sleep(0);
    ok(pRtlGetCurrentProcessorNumber() == MainProcessor,
       "Running on processor %lu, expected %lu\n",
       pRtlGetCurrentProcessorNumber(), MainProcessor);

    for (i = 0; i < ITERATIONS; i++)
    {
        Result = RunOnce(BusyProcessor);
        if (Result == RunError)
            break;
        if (Result == RunInvalid)
            continue;
        Valid++;
        if (Result == RunIdle)
            Idle++;
    }

    /* A wake-up can still land on the busy processor when every other
       processor is busy at that moment, so not every run must go idle */
    if (Result == RunError)
        ok(0, "Run %lu could not be set up\n", i);
    else if (Valid < MIN_IDLE_RUNS)
        skip("Only %lu of %d runs were valid\n", Valid, ITERATIONS);
    else
        ok(Idle >= MIN_IDLE_RUNS,
           "Only %lu of %lu ready threads ran on another processor within "
           "%d ms\n", Idle, Valid, MAX_START_MS);

    if (OldMask)
        SetThreadAffinityMask(GetCurrentThread(), OldMask);
    CloseHandle(s_ProbeRan);
    CloseHandle(s_ProbeGo);
    CloseHandle(s_ProbeReady);
}
