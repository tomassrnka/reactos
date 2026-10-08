/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Stress SuspendThread, Get/SetThreadContext and ResumeThread
 *              on running threads, the way a runtime preempts its threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#if defined(_M_IX86) || defined(_M_AMD64)

/*
 * Worker threads spin in user mode, make system calls and wake each
 * other. Controller threads suspend a worker, read its context and, when
 * the worker was stopped in this module's code, redirect it to a stub
 * that only returns (pushing the old instruction pointer as the return
 * address, as an asynchronous preemption does), then resume it. A
 * watchdog reports a controller that makes no progress. The duration and
 * the thread counts can be changed with SUSPCTX_SECONDS, SUSPCTX_WORKERS,
 * SUSPCTX_CONTROLLERS and SUSPCTX_INJECT (0 disables the redirection).
 * SUSPCTX_HOLD=1 keeps the process alive after a stall, for a memory dump.
 * The default run is a short check; a lost APC took 1 to 10 minutes to
 * show with 8 processors, 24 workers and 8 controllers.
 */

#define MAX_WORKERS 128
#define MAX_CONTROLLERS 32
#define STACK_PROBE 16384

typedef struct _SC_WORKER
{
    HANDLE Thread;
    HANDLE Event;
    HANDLE PartnerEvent;
    ULONG Mode;
    volatile LONG Iterations;
} SC_WORKER, *PSC_WORKER;

typedef struct _SC_CONTROLLER
{
    HANDLE Thread;
    ULONG First;
    ULONG Count;
    volatile LONG Rounds;
    volatile LONG Phase;
    volatile LONG Worker;
    volatile LONG Injected;
    volatile LONG Errors;
    volatile LONG BadCount;
} SC_CONTROLLER, *PSC_CONTROLLER;

static SC_WORKER Workers[MAX_WORKERS];
static SC_CONTROLLER Controllers[MAX_CONTROLLERS];
static volatile LONG StopWorkers;
static volatile LONG StopControllers;
static BOOL Inject = TRUE;
static PUCHAR Stub;
static volatile LONG *StubCount;
static ULONG_PTR ImageStart, ImageEnd;

static const char *PhaseName[] =
{
    "idle", "SuspendThread", "GetThreadContext", "SetThreadContext", "ResumeThread"
};

static
ULONG
GetEnvULong(_In_ PCSTR Name, _In_ ULONG Default)
{
    char Buffer[32];
    DWORD Length;

    Length = GetEnvironmentVariableA(Name, Buffer, sizeof(Buffer));
    if (Length == 0 || Length >= sizeof(Buffer))
        return Default;
    return strtoul(Buffer, NULL, 0);
}

static
VOID
Report(_In_ PCSTR Format, ...)
{
    char Buffer[512];
    va_list Args;

    va_start(Args, Format);
    StringCbVPrintfA(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);
    OutputDebugStringA(Buffer);
    printf("%s", Buffer);
    fflush(stdout);
}

/* pushf; lock inc dword [counter]; popf; ret: preserves every register and the flags */
static
BOOL
MakeStub(VOID)
{
    PUCHAR Code;

    Code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!Code)
        return FALSE;

    StubCount = (volatile LONG *)(Code + 64);
    Code[0] = 0x9C;
    Code[1] = 0xF0;
    Code[2] = 0xFF;
    Code[3] = 0x05;
#ifdef _M_AMD64
    *(LONG UNALIGNED *)&Code[4] = (LONG)((PUCHAR)StubCount - (Code + 8));
#else
    *(ULONG UNALIGNED *)&Code[4] = (ULONG)(ULONG_PTR)StubCount;
#endif
    Code[8] = 0x9D;
    Code[9] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), Code, 16);
    Stub = Code;
    return TRUE;
}

static
ULONG
SpinWork(_In_ ULONG Seed, _In_ ULONG Count)
{
    volatile ULONG Value = Seed;
    ULONG i;

    for (i = 0; i < Count; i++)
        Value = Value * 1103515245 + 12345;
    return Value;
}

/* Commit stack below the worker loop's frames: a redirection writes below the stack pointer */
static
DECLSPEC_NOINLINE
VOID
ProbeStack(VOID)
{
    volatile UCHAR Probe[STACK_PROBE];
    ULONG i;

    /* Top down, one guard page at a time */
    for (i = STACK_PROBE; i > 0; i -= 256)
        Probe[i - 1] = 0;
}

static
DWORD
WINAPI
WorkerThread(_In_ LPVOID Parameter)
{
    PSC_WORKER Worker = Parameter;
    ULONG Seed = (ULONG)(ULONG_PTR)Worker;
    ULONG Round = 0;

    ProbeStack();
    while (!StopWorkers)
    {
        switch (Worker->Mode)
        {
            case 0:
                /* User-mode computation only */
                Seed = SpinWork(Seed, 20000);
                break;

            case 1:
                /* Short computation and system calls that may switch */
                Seed = SpinWork(Seed, 2000);
                if (Round & 1)
                    SwitchToThread();
                else
                    Sleep(0);
                break;

            case 2:
                /* Ping-pong with another mode 2 worker: waits satisfied from another processor */
                Seed = SpinWork(Seed, 500);
                SetEvent(Worker->PartnerEvent);
                WaitForSingleObject(Worker->Event, 10);
                break;

            default:
                /* Timed waits, so the worker is often waiting when suspended */
                Seed = SpinWork(Seed, 5000);
                if ((Round & 15) == 0)
                    Sleep(1);
                else
                    WaitForSingleObject(Worker->Event, 0);
                break;
        }
        Round++;
        InterlockedIncrement(&Worker->Iterations);
    }
    return Seed;
}

static
DWORD
WINAPI
ControllerThread(_In_ LPVOID Parameter)
{
    PSC_CONTROLLER Controller = Parameter;
    ULONG Next = 0;
    CONTEXT Context;
    ULONG_PTR Pc, Sp;
    DWORD Count;

    while (!StopControllers)
    {
        PSC_WORKER Worker = &Workers[Controller->First + Next];
        Next = (Next + 1) % Controller->Count;
        Controller->Worker = (LONG)(Worker - Workers);

        Controller->Phase = 1;
        Count = SuspendThread(Worker->Thread);
        if (Count == (DWORD)-1)
        {
            InterlockedIncrement(&Controller->Errors);
            Controller->Phase = 0;
            continue;
        }
        if (Count != 0)
            InterlockedIncrement(&Controller->BadCount);

        Controller->Phase = 2;
        RtlZeroMemory(&Context, sizeof(Context));
        Context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(Worker->Thread, &Context))
        {
            InterlockedIncrement(&Controller->Errors);
        }
        else
        {
#ifdef _M_AMD64
            Pc = Context.Rip;
            Sp = Context.Rsp;
#else
            Pc = Context.Eip;
            Sp = Context.Esp;
#endif
            /* Redirect only a thread that was stopped in this module's code */
            if (Inject && Pc >= ImageStart && Pc < ImageEnd)
            {
                Sp -= sizeof(ULONG_PTR);
                *(ULONG_PTR *)Sp = Pc;
#ifdef _M_AMD64
                Context.Rip = (ULONG_PTR)Stub;
                Context.Rsp = Sp;
#else
                Context.Eip = (ULONG_PTR)Stub;
                Context.Esp = Sp;
#endif
                Controller->Phase = 3;
                if (SetThreadContext(Worker->Thread, &Context))
                    InterlockedIncrement(&Controller->Injected);
                else
                    InterlockedIncrement(&Controller->Errors);
            }
        }

        Controller->Phase = 4;
        if (ResumeThread(Worker->Thread) == (DWORD)-1)
            InterlockedIncrement(&Controller->Errors);

        Controller->Phase = 0;
        InterlockedIncrement(&Controller->Rounds);
    }
    return 0;
}

START_TEST(SuspendThreadContext)
{
    SYSTEM_INFO SystemInfo;
    PIMAGE_NT_HEADERS NtHeaders;
    ULONG Seconds, StallSeconds, WorkerCount, ControllerCount, i, Elapsed;
    DWORD Deadline;
    LONG LastRounds[MAX_CONTROLLERS];
    ULONG Still[MAX_CONTROLLERS];
    LONG TotalRounds, TotalInjected, TotalErrors, TotalBad;
    BOOL Stalled = FALSE;
    HANDLE Event;

    GetSystemInfo(&SystemInfo);
    Seconds = GetEnvULong("SUSPCTX_SECONDS", 30);
    StallSeconds = (Seconds >= 60) ? 20 : 10;
    WorkerCount = GetEnvULong("SUSPCTX_WORKERS", max(4, 2 * SystemInfo.dwNumberOfProcessors));
    ControllerCount = GetEnvULong("SUSPCTX_CONTROLLERS", max(1, SystemInfo.dwNumberOfProcessors / 2));
    Inject = GetEnvULong("SUSPCTX_INJECT", 1) != 0;
    WorkerCount = min(max(WorkerCount, 2), MAX_WORKERS);
    ControllerCount = min(max(ControllerCount, 1), min(MAX_CONTROLLERS, WorkerCount));

    NtHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)GetModuleHandleW(NULL) +
                                    ((PIMAGE_DOS_HEADER)GetModuleHandleW(NULL))->e_lfanew);
    ImageStart = (ULONG_PTR)GetModuleHandleW(NULL);
    ImageEnd = ImageStart + NtHeaders->OptionalHeader.SizeOfImage;

    if (!MakeStub())
    {
        skip("VirtualAlloc failed: %lu\n", GetLastError());
        return;
    }

    Report("SUSPCTX: start %lu processors, %lu workers, %lu controllers, %lu s, inject %d\n",
           SystemInfo.dwNumberOfProcessors, WorkerCount, ControllerCount, Seconds, Inject);

    for (i = 0; i < WorkerCount; i++)
    {
        Event = CreateEventW(NULL, FALSE, FALSE, NULL);
        ok(Event != NULL, "CreateEventW failed: %lu\n", GetLastError());
        if (!Event)
            return;
        Workers[i].Event = Event;
        Workers[i].Mode = i % 4;
    }
    for (i = 0; i < WorkerCount; i++)
        Workers[i].PartnerEvent = Workers[(i ^ 4) < WorkerCount ? (i ^ 4) : i].Event;
    for (i = 0; i < WorkerCount; i++)
    {
        Workers[i].Thread = CreateThread(NULL, 0, WorkerThread, &Workers[i], 0, NULL);
        ok(Workers[i].Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!Workers[i].Thread)
        {
            StopWorkers = 1;
            return;
        }
    }

    /* Each controller owns a disjoint set of workers */
    for (i = 0; i < ControllerCount; i++)
    {
        Controllers[i].First = i * WorkerCount / ControllerCount;
        Controllers[i].Count = (i + 1) * WorkerCount / ControllerCount - Controllers[i].First;
        LastRounds[i] = 0;
        Still[i] = 0;
        Controllers[i].Thread = CreateThread(NULL, 0, ControllerThread, &Controllers[i], 0, NULL);
        ok(Controllers[i].Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!Controllers[i].Thread)
        {
            StopControllers = 1;
            StopWorkers = 1;
            return;
        }
    }

    for (Elapsed = 0; Elapsed < Seconds && !Stalled; Elapsed++)
    {
        Sleep(1000);
        TotalRounds = 0;
        for (i = 0; i < ControllerCount; i++)
        {
            LONG Rounds = Controllers[i].Rounds;
            TotalRounds += Rounds;
            if (Rounds == LastRounds[i])
                Still[i]++;
            else
                Still[i] = 0;
            LastRounds[i] = Rounds;
            if (Still[i] >= StallSeconds)
            {
                Report("SUSPCTX: STALL controller %lu in %s on worker %ld (tid %lu) for %lu s at %lu s\n",
                       i, PhaseName[Controllers[i].Phase],
                       Controllers[i].Worker, GetThreadId(Workers[Controllers[i].Worker].Thread),
                       Still[i], Elapsed + 1);
                Stalled = TRUE;
            }
        }
        if ((Elapsed + 1) % 60 == 0)
            Report("SUSPCTX: %lu s, %ld rounds, %ld injected calls ran\n", Elapsed + 1, TotalRounds, *StubCount);
    }

    /* Stop with one deadline; a controller that does not stop is stalled too */
    StopControllers = 1;
    Deadline = GetTickCount() + StallSeconds * 1000;
    for (i = 0; i < ControllerCount && !Stalled; i++)
    {
        LONG Remaining = (LONG)(Deadline - GetTickCount());
        if (WaitForSingleObject(Controllers[i].Thread, max(Remaining, 0)) != WAIT_OBJECT_0)
        {
            Report("SUSPCTX: STALL controller %lu in %s on worker %ld (tid %lu) at shutdown\n",
                   i, PhaseName[Controllers[i].Phase], Controllers[i].Worker,
                   GetThreadId(Workers[Controllers[i].Worker].Thread));
            Stalled = TRUE;
        }
    }
    if (Stalled && GetEnvULong("SUSPCTX_HOLD", 0))
    {
        Report("SUSPCTX: HOLD\n");
        Sleep(INFINITE);
    }
    StopWorkers = 1;
    if (!Stalled)
    {
        for (i = 0; i < WorkerCount; i++)
            SetEvent(Workers[i].Event);
        Deadline = GetTickCount() + StallSeconds * 1000;
        for (i = 0; i < WorkerCount && !Stalled; i++)
        {
            LONG Remaining = (LONG)(Deadline - GetTickCount());
            if (WaitForSingleObject(Workers[i].Thread, max(Remaining, 0)) != WAIT_OBJECT_0)
            {
                Report("SUSPCTX: STALL worker %lu did not stop\n", i);
                Stalled = TRUE;
            }
        }
    }

    TotalRounds = TotalInjected = TotalErrors = TotalBad = 0;
    for (i = 0; i < ControllerCount; i++)
    {
        TotalRounds += Controllers[i].Rounds;
        TotalInjected += Controllers[i].Injected;
        TotalErrors += Controllers[i].Errors;
        TotalBad += Controllers[i].BadCount;
    }

    ok(!Stalled, "A controller stopped making progress\n");
    ok(TotalErrors == 0, "%ld calls failed\n", TotalErrors);
    ok(TotalBad == 0, "SuspendThread returned a nonzero count %ld times\n", TotalBad);
    ok(TotalRounds > 0, "No suspend rounds completed\n");
    if (Inject && !Stalled)
    {
        ok(TotalInjected > 0, "No context was redirected\n");
        ok(*StubCount == TotalInjected, "%ld redirections, but the stub ran %ld times\n",
           TotalInjected, *StubCount);
    }

    Report("SUSPCTX: %s after %lu s: %ld rounds, %ld redirected, %ld stub calls, %ld errors\n",
           Stalled ? "STALLED" : "DONE", Elapsed, TotalRounds, TotalInjected, *StubCount, TotalErrors);

    if (!Stalled)
    {
        for (i = 0; i < ControllerCount; i++)
            CloseHandle(Controllers[i].Thread);
        for (i = 0; i < WorkerCount; i++)
        {
            CloseHandle(Workers[i].Thread);
            CloseHandle(Workers[i].Event);
        }
        VirtualFree(Stub, 0, MEM_RELEASE);
    }
}

#else

START_TEST(SuspendThreadContext)
{
    skip("Not implemented for this architecture\n");
}

#endif
