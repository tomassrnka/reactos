/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Stress GetThreadContext on threads that create and close
 *              named objects
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#if defined(_M_IX86) || defined(_M_AMD64)

/*
 * Worker threads create and close named events. They contend on the
 * push lock of the named object directory and often wait on its gate.
 * Controller threads read the workers' context. Each read queues a
 * special kernel APC to the worker, often while it waits on that gate,
 * and the push lock owner may signal the gate at the same moment. A
 * watchdog reports a thread that makes no progress. The duration and the
 * thread counts can be changed with CTXOBJ_SECONDS, CTXOBJ_WORKERS and
 * CTXOBJ_CONTROLLERS. CTXOBJ_HOLD=1 keeps the process as it is after a
 * stall, for a memory dump. Other architectures do not implement the
 * context APC yet.
 */

#define MAX_WORKERS 64
#define MAX_CONTROLLERS 16
#define NAMES_PER_WORKER 16

typedef struct _CO_WORKER
{
    HANDLE Thread;
    ULONG Index;
    volatile ULONG Iterations;
    volatile ULONG Errors;
    volatile DWORD FirstError;
} CO_WORKER, *PCO_WORKER;

typedef struct _CO_CONTROLLER
{
    HANDLE Thread;
    ULONG First;
    ULONG Count;
    volatile ULONG Calls;
    volatile LONG Worker;
    volatile ULONG Errors;
    volatile DWORD FirstError;
} CO_CONTROLLER, *PCO_CONTROLLER;

static CO_WORKER Workers[MAX_WORKERS];
static CO_CONTROLLER Controllers[MAX_CONTROLLERS];
static volatile LONG StopWorkers;
static volatile LONG StopControllers;

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

static
DWORD
WINAPI
WorkerThread(_In_ LPVOID Parameter)
{
    PCO_WORKER Worker = Parameter;
    WCHAR Name[64];
    HANDLE Event;
    ULONG n = 0;

    while (!StopWorkers)
    {
        StringCbPrintfW(Name, sizeof(Name), L"Local\\CtxObj-%lu-%lu-%lu",
                        GetCurrentProcessId(), Worker->Index, n++ % NAMES_PER_WORKER);
        Event = CreateEventW(NULL, FALSE, FALSE, Name);
        if (!Event || !CloseHandle(Event))
        {
            if (!Worker->Errors++)
                Worker->FirstError = GetLastError();
        }
        Worker->Iterations++;
    }
    return 0;
}

static
DWORD
WINAPI
ControllerThread(_In_ LPVOID Parameter)
{
    PCO_CONTROLLER Controller = Parameter;
    CONTEXT Context;
    ULONG i;

    while (!StopControllers)
    {
        for (i = Controller->First; i < Controller->First + Controller->Count && !StopControllers; i++)
        {
            Controller->Worker = i;
            ZeroMemory(&Context, sizeof(Context));
            Context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(Workers[i].Thread, &Context))
            {
                if (!Controller->Errors++)
                    Controller->FirstError = GetLastError();
            }
            Controller->Calls++;
        }
    }
    return 0;
}

/* Controllers stop first: a worker must not exit while a controller may read its context */
static
BOOL
StopAll(_In_ ULONG WorkerCount, _In_ ULONG ControllerCount, _In_ ULONG StallSeconds)
{
    DWORD Deadline;
    DWORD Wait;
    BOOL Stopped = TRUE;
    ULONG i;

    StopControllers = 1;
    Deadline = GetTickCount() + StallSeconds * 1000;
    for (i = 0; i < ControllerCount; i++)
    {
        LONG Remaining = (LONG)(Deadline - GetTickCount());
        Wait = WaitForSingleObject(Controllers[i].Thread, max(Remaining, 0));
        if (Wait != WAIT_OBJECT_0)
        {
            Report("CTXOBJ: STALL controller %lu on worker %ld at shutdown (wait %lu, error %lu)\n",
                   i, Controllers[i].Worker, Wait, GetLastError());
            Stopped = FALSE;
        }
    }
    if (!Stopped)
        return FALSE;

    StopWorkers = 1;
    Deadline = GetTickCount() + StallSeconds * 1000;
    for (i = 0; i < WorkerCount; i++)
    {
        LONG Remaining = (LONG)(Deadline - GetTickCount());
        Wait = WaitForSingleObject(Workers[i].Thread, max(Remaining, 0));
        if (Wait != WAIT_OBJECT_0)
        {
            Report("CTXOBJ: STALL worker %lu did not stop (wait %lu, error %lu)\n", i, Wait, GetLastError());
            Stopped = FALSE;
        }
    }
    return Stopped;
}

START_TEST(ThreadContextObjects)
{
    SYSTEM_INFO SystemInfo;
    ULONG WorkerCount, ControllerCount, Seconds, StallSeconds;
    ULONG StartedWorkers = 0, StartedControllers = 0;
    ULONG i, Elapsed;
    ULONG LastWorker[MAX_WORKERS], LastController[MAX_CONTROLLERS];
    ULONG StillWorker[MAX_WORKERS], StillController[MAX_CONTROLLERS];
    ULONGLONG TotalCalls, TotalIterations, TotalErrors;
    DWORD FirstError = 0;
    BOOL Stalled = FALSE;

    GetSystemInfo(&SystemInfo);
    if (SystemInfo.dwNumberOfProcessors < 2)
    {
        skip("Needs at least 2 processors\n");
        return;
    }

    WorkerCount = GetEnvULong("CTXOBJ_WORKERS", 2 * SystemInfo.dwNumberOfProcessors);
    WorkerCount = max(min(WorkerCount, MAX_WORKERS), 2);
    ControllerCount = GetEnvULong("CTXOBJ_CONTROLLERS", max(SystemInfo.dwNumberOfProcessors / 2, 1));
    ControllerCount = max(min(min(ControllerCount, MAX_CONTROLLERS), WorkerCount), 1);
    Seconds = max(min(GetEnvULong("CTXOBJ_SECONDS", 30), 7 * 24 * 3600), 1);
    StallSeconds = 10;

    Report("CTXOBJ: start %lu processors, %lu workers, %lu controllers, %lu s\n",
           SystemInfo.dwNumberOfProcessors, WorkerCount, ControllerCount, Seconds);

    for (i = 0; i < WorkerCount; i++)
    {
        Workers[i].Index = i;
        Workers[i].Thread = CreateThread(NULL, 0, WorkerThread, &Workers[i], 0, NULL);
        ok(Workers[i].Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!Workers[i].Thread)
            break;
        StartedWorkers++;
    }

    /* Each controller owns a disjoint set of workers */
    for (i = 0; i < ControllerCount && StartedWorkers == WorkerCount; i++)
    {
        Controllers[i].First = i * WorkerCount / ControllerCount;
        Controllers[i].Count = (i + 1) * WorkerCount / ControllerCount - Controllers[i].First;
        Controllers[i].Thread = CreateThread(NULL, 0, ControllerThread, &Controllers[i], 0, NULL);
        ok(Controllers[i].Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!Controllers[i].Thread)
            break;
        StartedControllers++;
    }

    if (StartedWorkers != WorkerCount || StartedControllers != ControllerCount)
    {
        if (StopAll(StartedWorkers, StartedControllers, StallSeconds))
        {
            for (i = 0; i < StartedControllers; i++)
                CloseHandle(Controllers[i].Thread);
            for (i = 0; i < StartedWorkers; i++)
                CloseHandle(Workers[i].Thread);
        }
        return;
    }

    for (i = 0; i < WorkerCount; i++)
    {
        LastWorker[i] = 0;
        StillWorker[i] = 0;
    }
    for (i = 0; i < ControllerCount; i++)
    {
        LastController[i] = 0;
        StillController[i] = 0;
    }

    for (Elapsed = 0; Elapsed < Seconds && !Stalled; Elapsed++)
    {
        Sleep(1000);
        for (i = 0; i < ControllerCount; i++)
        {
            ULONG Calls = Controllers[i].Calls;
            StillController[i] = (Calls == LastController[i]) ? StillController[i] + 1 : 0;
            LastController[i] = Calls;
            if (StillController[i] >= StallSeconds)
            {
                Report("CTXOBJ: STALL controller %lu on worker %ld (tid %lu) for %lu s at %lu s\n",
                       i, Controllers[i].Worker, GetThreadId(Workers[Controllers[i].Worker].Thread),
                       StillController[i], Elapsed + 1);
                Stalled = TRUE;
            }
        }
        for (i = 0; i < WorkerCount; i++)
        {
            ULONG Iterations = Workers[i].Iterations;
            StillWorker[i] = (Iterations == LastWorker[i]) ? StillWorker[i] + 1 : 0;
            LastWorker[i] = Iterations;
            if (StillWorker[i] >= StallSeconds)
            {
                Report("CTXOBJ: STALL worker %lu (tid %lu) for %lu s at %lu s\n",
                       i, GetThreadId(Workers[i].Thread), StillWorker[i], Elapsed + 1);
                Stalled = TRUE;
            }
        }
        if ((Elapsed + 1) % 60 == 0)
            Report("CTXOBJ: %lu s\n", Elapsed + 1);
    }

    /* Keep the stalled state intact for a memory dump */
    if (Stalled && GetEnvULong("CTXOBJ_HOLD", 0))
    {
        Report("CTXOBJ: HOLD\n");
        Sleep(INFINITE);
    }

    if (!StopAll(WorkerCount, ControllerCount, StallSeconds))
        Stalled = TRUE;

    TotalCalls = TotalIterations = TotalErrors = 0;
    for (i = 0; i < ControllerCount; i++)
    {
        TotalCalls += Controllers[i].Calls;
        TotalErrors += Controllers[i].Errors;
        if (!FirstError)
            FirstError = Controllers[i].FirstError;
    }
    for (i = 0; i < WorkerCount; i++)
    {
        TotalIterations += Workers[i].Iterations;
        TotalErrors += Workers[i].Errors;
        if (!FirstError)
            FirstError = Workers[i].FirstError;
    }

    ok(!Stalled, "A thread stopped making progress\n");
    ok(TotalErrors == 0, "%I64u calls failed, first error %lu\n", TotalErrors, FirstError);
    ok(TotalCalls > 0, "No context was read\n");
    ok(TotalIterations > 0, "No object was created\n");

    Report("CTXOBJ: %s after %lu s: %I64u context reads, %I64u objects, %I64u errors\n",
           Stalled ? "STALLED" : "DONE", Elapsed, TotalCalls, TotalIterations, TotalErrors);

    /* A thread that did not stop may still use its handle */
    if (!Stalled)
    {
        for (i = 0; i < ControllerCount; i++)
            CloseHandle(Controllers[i].Thread);
        for (i = 0; i < WorkerCount; i++)
            CloseHandle(Workers[i].Thread);
    }
}

#else

START_TEST(ThreadContextObjects)
{
    skip("Not implemented for this architecture\n");
}

#endif
