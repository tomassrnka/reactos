/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Test that a ready thread runs on an idle processor when its
 *              ideal processor is busy
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define ITERATIONS 5
#define SPIN_MS 2000
#define MAX_START_MS 200

static volatile LONG s_SpinnerRunning;
static volatile LONG s_StopSpinner;
static LARGE_INTEGER s_Frequency;
static DWORD s_ProbeProcessor;
static HANDLE s_ProbeReady, s_ProbeGo;

static DWORD WINAPI
SpinnerThread(PVOID Parameter)
{
    ULONG BusyProcessor = PtrToUlong(Parameter);
    LARGE_INTEGER Now, Deadline;
    ULONG i;

    /* Pin itself while it runs, rather than being moved before it starts */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << BusyProcessor);
    for (i = 0; i < 100 && RtlGetCurrentProcessorNumber() != BusyProcessor; i++)
        Sleep(1);
    if (RtlGetCurrentProcessorNumber() != BusyProcessor)
        return 1;

    QueryPerformanceCounter(&Deadline);
    Deadline.QuadPart += s_Frequency.QuadPart * SPIN_MS / 1000;

    InterlockedExchange(&s_SpinnerRunning, 1);
    do
    {
        QueryPerformanceCounter(&Now);
    } while (!s_StopSpinner && Now.QuadPart < Deadline.QuadPart);

    return 0;
}

static DWORD WINAPI
ProbeThread(PVOID Parameter)
{
    SetEvent(s_ProbeReady);
    WaitForSingleObject(s_ProbeGo, INFINITE);
    s_ProbeProcessor = RtlGetCurrentProcessorNumber();
    return 0;
}

static LONGLONG
RunOnce(ULONG BusyProcessor)
{
    HANDLE Spinner, Probe;
    LARGE_INTEGER Start, Done;
    DWORD Wait, i;
    LONGLONG Ms = -1;

    s_SpinnerRunning = 0;
    s_StopSpinner = 0;
    s_ProbeProcessor = MAXDWORD;

    /* The probe may run anywhere but prefers the busy processor */
    ResetEvent(s_ProbeGo);
    Probe = CreateThread(NULL, 0, ProbeThread, NULL, CREATE_SUSPENDED, NULL);
    ok(Probe != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Probe)
        return -1;
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
        return -1;
    }
    for (i = 0; i < 1000 && !s_SpinnerRunning; i++)
        Sleep(1);
    ok(s_SpinnerRunning, "Spinner did not start on processor %lu\n", BusyProcessor);

    /* Let the probe block and other woken threads go idle again */
    Sleep(200);

    /* Time it on this thread only: counters may differ between processors */
    QueryPerformanceCounter(&Start);
    SetEvent(s_ProbeGo);
    Wait = WaitForSingleObject(Probe, SPIN_MS + 3000);
    QueryPerformanceCounter(&Done);
    ok(Wait == WAIT_OBJECT_0, "Probe wait returned %lu\n", Wait);

    InterlockedExchange(&s_StopSpinner, 1);
    WaitForSingleObject(Spinner, INFINITE);
    CloseHandle(Spinner);
    CloseHandle(Probe);

    if (Wait == WAIT_OBJECT_0)
    {
        Ms = (Done.QuadPart - Start.QuadPart) * 1000 / s_Frequency.QuadPart;
        trace("Probe ran on processor %lu and finished after %I64d ms\n",
              s_ProbeProcessor, Ms);
    }
    return Ms;
}

START_TEST(IdleProcessor)
{
    DWORD_PTR ProcessMask, SystemMask, OldMask;
    ULONG Count = 0, BusyProcessor = MAXULONG, MainProcessor = 0, i;
    LONGLONG Ms, Worst = 0;

    ok(GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask),
       "GetProcessAffinityMask failed: %lu\n", GetLastError());
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
    ok(s_ProbeReady && s_ProbeGo, "CreateEventW failed: %lu\n", GetLastError());

    OldMask = SetThreadAffinityMask(GetCurrentThread(),
                                    (DWORD_PTR)1 << MainProcessor);
    ok(OldMask != 0, "SetThreadAffinityMask failed: %lu\n", GetLastError());
    Sleep(0);
    ok(RtlGetCurrentProcessorNumber() == MainProcessor,
       "Running on processor %lu, expected %lu\n",
       RtlGetCurrentProcessorNumber(), MainProcessor);

    for (i = 0; i < ITERATIONS; i++)
    {
        Ms = RunOnce(BusyProcessor);
        if (Ms < 0)
            break;
        if (Ms > Worst)
            Worst = Ms;
    }

    ok(Ms >= 0 && Worst < MAX_START_MS,
       "A ready thread waited %I64d ms for its busy ideal processor "
       "while other processors were idle\n", Worst);

    if (OldMask)
        SetThreadAffinityMask(GetCurrentThread(), OldMask);
    CloseHandle(s_ProbeGo);
    CloseHandle(s_ProbeReady);
}
