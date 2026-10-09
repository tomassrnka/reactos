/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that GetThreadContext and SetThreadContext keep the
 *              x87 and SSE state of the threads they target
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#if defined(_M_IX86) && defined(__GNUC__)

/*
 * Workers load a pattern of their own into XMM0-XMM6, MXCSR and the x87
 * control word, then check it in a loop in user mode. Controllers suspend
 * a worker, read its floating point and extended registers and check them
 * against the pattern, sometimes write a context back, and resume it. A
 * noise thread prints to the debugger: with a kernel debugger attached,
 * every print freezes the other processors with an NMI, and on x86 the
 * NMI task switch sets CR0.TS under whatever code runs there, including
 * the kernel code that moves a thread's registers to and from memory for
 * the context calls. Without a kernel debugger, or with one processor,
 * the test still checks the context contract but sees no NMI.
 * NPXCTX_SECONDS, NPXCTX_WORKERS, NPXCTX_CONTROLLERS and NPXCTX_PRINT_US
 * (0: no prints) change the defaults.
 */

#define MAX_WORKERS 32
#define MAX_CONTROLLERS 8
#define XMM_CHECKED 7
#define CHECK_ITERATIONS 2000
#define STOP_TIMEOUT_MILLISECONDS 60000

#define MISMATCH_MXCSR 0x80
#define MISMATCH_FCW 0x100

typedef struct DECLSPEC_ALIGN(16) _NPX_WORKER
{
    /* The assembly below depends on the order of the first fields */
    ULONG Xmm[XMM_CHECKED][4];
    ULONG Observed[XMM_CHECKED][4];
    ULONG MxCsr;
    ULONG ObservedMxCsr;
    USHORT ControlWord;
    USHORT ObservedControlWord;
    volatile LONG Armed;

    HANDLE Thread;
    volatile LONG Loops;
    volatile LONG Failures;
    ULONG FirstMask;
    ULONG FirstObserved[XMM_CHECKED][4];
    ULONG FirstMxCsr;
    USHORT FirstControlWord;
} NPX_WORKER, *PNPX_WORKER;

typedef struct _NPX_CONTROLLER
{
    HANDLE Thread;
    ULONG First;
    ULONG Count;
    volatile LONG Rounds;
    volatile LONG Checked;
    volatile LONG Mismatches;
    volatile LONG Errors;
    ULONG FirstMismatchWorker;
    ULONG FirstMismatchMode;
    ULONG FirstMismatchMask;
} NPX_CONTROLLER, *PNPX_CONTROLLER;

static NPX_WORKER Workers[MAX_WORKERS];
static NPX_CONTROLLER Controllers[MAX_CONTROLLERS];
static volatile LONG StopWorkers;
static volatile LONG StopControllers;
static volatile LONG Prints;
static ULONG PrintMicroseconds;

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

#define CHECK_XMM(n) \
    "movdqa  %%xmm" #n ", %%xmm7\n\t" \
    "pcmpeqd " #n "*16(%[w]), %%xmm7\n\t" \
    "pmovmskb %%xmm7, %%eax\n\t" \
    "cmpl    $0xffff, %%eax\n\t" \
    "je      1" #n "f\n\t" \
    "orl     $(1 << " #n "), %[m]\n" \
    "1" #n ":\n\t"

#define SAVE_XMM(n) \
    "movdqa  %%xmm" #n ", 112+" #n "*16(%[w])\n\t"

/* Returns 0, or a mask of the registers that lost the pattern; Armed is 1 while the registers hold it */
static
__attribute__((__target__("sse2")))
ULONG
NpxCheck(_Inout_ PNPX_WORKER Worker, _In_ ULONG Iterations)
{
    ULONG Mask;

    __asm__ __volatile__(
        "fldcw   232(%[w])\n\t"
        "ldmxcsr 224(%[w])\n\t"
        "movdqa  0*16(%[w]), %%xmm0\n\t"
        "movdqa  1*16(%[w]), %%xmm1\n\t"
        "movdqa  2*16(%[w]), %%xmm2\n\t"
        "movdqa  3*16(%[w]), %%xmm3\n\t"
        "movdqa  4*16(%[w]), %%xmm4\n\t"
        "movdqa  5*16(%[w]), %%xmm5\n\t"
        "movdqa  6*16(%[w]), %%xmm6\n\t"
        "movl    $1, 236(%[w])\n"
        "0:\n\t"
        "xorl    %[m], %[m]\n\t"
        CHECK_XMM(0)
        CHECK_XMM(1)
        CHECK_XMM(2)
        CHECK_XMM(3)
        CHECK_XMM(4)
        CHECK_XMM(5)
        CHECK_XMM(6)
        "stmxcsr 228(%[w])\n\t"
        "movl    228(%[w]), %%eax\n\t"
        "cmpl    224(%[w]), %%eax\n\t"
        "je      17f\n\t"
        "orl     $0x80, %[m]\n"
        "17:\n\t"
        "fnstcw  234(%[w])\n\t"
        "movw    234(%[w]), %%ax\n\t"
        "cmpw    232(%[w]), %%ax\n\t"
        "je      18f\n\t"
        "orl     $0x100, %[m]\n"
        "18:\n\t"
        "testl   %[m], %[m]\n\t"
        "jnz     2f\n\t"
        "decl    %[n]\n\t"
        "jnz     0b\n\t"
        "jmp     3f\n"
        "2:\n\t"
        SAVE_XMM(0)
        SAVE_XMM(1)
        SAVE_XMM(2)
        SAVE_XMM(3)
        SAVE_XMM(4)
        SAVE_XMM(5)
        SAVE_XMM(6)
        "3:\n\t"
        "movl    $0, 236(%[w])\n\t"
        : [m] "=&r" (Mask), [n] "+r" (Iterations)
        : [w] "r" (Worker)
        : "eax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory", "cc");

    return Mask;
}

C_ASSERT(FIELD_OFFSET(NPX_WORKER, Observed) == 112);
C_ASSERT(FIELD_OFFSET(NPX_WORKER, MxCsr) == 224);
C_ASSERT(FIELD_OFFSET(NPX_WORKER, ObservedMxCsr) == 228);
C_ASSERT(FIELD_OFFSET(NPX_WORKER, ControlWord) == 232);
C_ASSERT(FIELD_OFFSET(NPX_WORKER, ObservedControlWord) == 234);
C_ASSERT(FIELD_OFFSET(NPX_WORKER, Armed) == 236);
C_ASSERT((sizeof(NPX_WORKER) % 16) == 0);

static
DWORD
WINAPI
WorkerThread(_In_ LPVOID Parameter)
{
    PNPX_WORKER Worker = Parameter;
    ULONG Mask;

    while (!StopWorkers)
    {
        Mask = NpxCheck(Worker, CHECK_ITERATIONS);
        InterlockedIncrement(&Worker->Loops);

        /* Let a worker that a controller woke up have the processor */
        Sleep(0);
        if (Mask && InterlockedIncrement(&Worker->Failures) == 1)
        {
            Worker->FirstMask = Mask;
            RtlCopyMemory(Worker->FirstObserved, Worker->Observed, sizeof(Worker->Observed));
            Worker->FirstMxCsr = Worker->ObservedMxCsr;
            Worker->FirstControlWord = Worker->ObservedControlWord;
        }
    }
    return 0;
}

/* Compare the FXSAVE image and the x87 control word of a context with the worker's pattern */
static
ULONG
CheckContext(_In_ PNPX_WORKER Worker, _In_ PCONTEXT Context)
{
    PUCHAR Fx = Context->ExtendedRegisters;
    ULONG Mask = 0, i;

    for (i = 0; i < XMM_CHECKED; i++)
    {
        if (memcmp(Fx + 160 + i * 16, Worker->Xmm[i], 16))
            Mask |= 1 << i;
    }
    if (*(ULONG UNALIGNED *)(Fx + 24) != Worker->MxCsr)
        Mask |= MISMATCH_MXCSR;
    if (*(USHORT UNALIGNED *)Fx != Worker->ControlWord ||
        (USHORT)Context->FloatSave.ControlWord != Worker->ControlWord)
        Mask |= MISMATCH_FCW;
    return Mask;
}

/*
 * Mode 0: read the floating point context only. 1: read it, then write back
 * the control registers only, as a runtime that preempts its threads does.
 * 2: read it and write all of it back. 3: control registers only, both ways.
 */
static
DWORD
WINAPI
ControllerThread(_In_ LPVOID Parameter)
{
    PNPX_CONTROLLER Controller = Parameter;
    PNPX_WORKER Worker;
    CONTEXT Context;
    ULONG Round, Mode, Mask, Index;

    for (Round = 0; !StopControllers; Round++)
    {
        Index = Controller->First + Round % Controller->Count;
        Worker = &Workers[Index];
        Mode = (Round / Controller->Count) & 3;

        if (SuspendThread(Worker->Thread) == (DWORD)-1)
        {
            InterlockedIncrement(&Controller->Errors);
            continue;
        }

        RtlZeroMemory(&Context, sizeof(Context));
        Context.ContextFlags = (Mode == 3) ? CONTEXT_CONTROL :
            (CONTEXT_FULL | CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS);
        if (!GetThreadContext(Worker->Thread, &Context))
        {
            InterlockedIncrement(&Controller->Errors);
        }
        else
        {
            /* The worker is suspended, so Armed cannot change any more */
            if (Mode != 3 && Worker->Armed)
            {
                InterlockedIncrement(&Controller->Checked);
                Mask = CheckContext(Worker, &Context);
                if (Mask && InterlockedIncrement(&Controller->Mismatches) == 1)
                {
                    Controller->FirstMismatchWorker = Index;
                    Controller->FirstMismatchMode = Mode;
                    Controller->FirstMismatchMask = Mask;
                }
            }

            if (Mode == 1)
                Context.ContextFlags = CONTEXT_CONTROL;
            if (Mode != 0 && !SetThreadContext(Worker->Thread, &Context))
                InterlockedIncrement(&Controller->Errors);
        }

        if (ResumeThread(Worker->Thread) == (DWORD)-1)
            InterlockedIncrement(&Controller->Errors);
        InterlockedIncrement(&Controller->Rounds);
    }
    return 0;
}

static
DWORD
WINAPI
NoiseThread(_In_ LPVOID Parameter)
{
    LARGE_INTEGER Frequency, Now, Next;

    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Next);
    while (!StopControllers)
    {
        DbgPrint(".");
        InterlockedIncrement(&Prints);
        Next.QuadPart += Frequency.QuadPart * PrintMicroseconds / 1000000;
        do
        {
            QueryPerformanceCounter(&Now);
        } while (Now.QuadPart < Next.QuadPart && !StopControllers);
        if (Now.QuadPart > Next.QuadPart + Frequency.QuadPart / 10)
            Next = Now;
    }
    return 0;
}

/* A context call that never returns must fail the test, not hang it */
static
VOID
JoinThread(_In_ HANDLE Thread, _In_ PCSTR Name, _In_ ULONG Index)
{
    DWORD Wait;

    Wait = WaitForSingleObject(Thread, STOP_TIMEOUT_MILLISECONDS);
    ok(Wait == WAIT_OBJECT_0, "%s %lu did not stop: wait returned %lu\n", Name, Index, Wait);
    CloseHandle(Thread);
}

static
VOID
RunTest(VOID)
{
    SYSTEM_INFO SystemInfo;
    HANDLE Noise = NULL;
    ULONG Seconds, WorkerCount, ControllerCount, i, r, k;
    ULONG Failures = 0, Mismatches = 0, Errors = 0, Checked = 0, Rounds = 0, Loops = 0;
    PNPX_WORKER Worker;
    PNPX_CONTROLLER Controller;

    if (!IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE))
    {
        skip("SSE2 is not available\n");
        return;
    }

    GetSystemInfo(&SystemInfo);
    Seconds = GetEnvULong("NPXCTX_SECONDS", 10);
    /* With more threads than processors, a worker woken up for a context call waits for a processor */
    WorkerCount = GetEnvULong("NPXCTX_WORKERS", max(1, SystemInfo.dwNumberOfProcessors - 1));
    WorkerCount = max(1, min(WorkerCount, MAX_WORKERS));
    ControllerCount = GetEnvULong("NPXCTX_CONTROLLERS", WorkerCount);
    ControllerCount = max(1, min(min(ControllerCount, MAX_CONTROLLERS), WorkerCount));
    PrintMicroseconds = GetEnvULong("NPXCTX_PRINT_US", 1000);

    for (i = 0; i < WorkerCount; i++)
    {
        Worker = &Workers[i];
        for (r = 0; r < XMM_CHECKED; r++)
        {
            for (k = 0; k < 4; k++)
                Worker->Xmm[r][k] = ((i + 1) << 24) | ((r + 1) << 16) | ((k + 1) << 8) | 0xA5;
        }
        /* Neither is the value FNINIT or a new thread gets */
        Worker->ControlWord = 0x027F | ((i & 3) << 10);
        Worker->MxCsr = 0x9F80 | ((i & 3) << 13);
        Worker->Thread = CreateThread(NULL, 0, WorkerThread, Worker, 0, NULL);
        ok(Worker->Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (!Worker->Thread)
        {
            WorkerCount = i;
            break;
        }
    }
    if (!WorkerCount)
        return;
    ControllerCount = min(ControllerCount, WorkerCount);

    for (i = 0; i < ControllerCount; i++)
    {
        Controller = &Controllers[i];
        Controller->First = i * WorkerCount / ControllerCount;
        Controller->Count = (i + 1) * WorkerCount / ControllerCount - Controller->First;
        Controller->Thread = CreateThread(NULL, 0, ControllerThread, Controller, CREATE_SUSPENDED, NULL);
        ok(Controller->Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (Controller->Thread)
        {
            SetThreadPriority(Controller->Thread, THREAD_PRIORITY_HIGHEST);
            ResumeThread(Controller->Thread);
        }
    }

    /* A print freezes the other processors only: on one there is nothing to gain */
    if (PrintMicroseconds && SystemInfo.dwNumberOfProcessors > 1)
    {
        Noise = CreateThread(NULL, 0, NoiseThread, NULL, 0, NULL);
        ok(Noise != NULL, "CreateThread failed with %lu\n", GetLastError());
    }

    Sleep(Seconds * 1000);

    InterlockedExchange(&StopControllers, 1);
    for (i = 0; i < ControllerCount; i++)
    {
        Controller = &Controllers[i];
        if (!Controller->Thread)
            continue;
        JoinThread(Controller->Thread, "controller", i);
        Rounds += Controller->Rounds;
        Checked += Controller->Checked;
        Errors += Controller->Errors;
        Mismatches += Controller->Mismatches;
        if (Controller->Mismatches)
        {
            trace("controller %lu: first context mismatch on worker %lu, mode %lu, mask 0x%lx\n",
                  i, Controller->FirstMismatchWorker, Controller->FirstMismatchMode,
                  Controller->FirstMismatchMask);
        }
    }
    if (Noise)
    {
        JoinThread(Noise, "noise thread", 0);
    }

    InterlockedExchange(&StopWorkers, 1);
    for (i = 0; i < WorkerCount; i++)
    {
        Worker = &Workers[i];
        JoinThread(Worker->Thread, "worker", i);
        Loops += Worker->Loops;
        Failures += Worker->Failures;
        if (Worker->Failures)
        {
            trace("worker %lu: %ld failures, first mask 0x%lx, MXCSR 0x%lx (0x%lx), FCW 0x%x (0x%x), XMM0 %08lx %08lx %08lx %08lx\n",
                  i, Worker->Failures, Worker->FirstMask, Worker->FirstMxCsr, Worker->MxCsr,
                  Worker->FirstControlWord, Worker->ControlWord,
                  Worker->FirstObserved[0][0], Worker->FirstObserved[0][1],
                  Worker->FirstObserved[0][2], Worker->FirstObserved[0][3]);
        }
    }

    ok(Failures == 0, "%lu of %lu worker loops saw registers other than their own\n", Failures, Loops);
    ok(Mismatches == 0, "%lu of %lu contexts did not hold the worker's registers\n", Mismatches, Checked);
    ok(Errors == 0, "%lu context or suspension calls failed\n", Errors);
    if (!Checked)
        skip("No context was taken while a worker held its pattern\n");
    trace("%lu s, %lu workers, %lu controllers: %lu rounds, %lu contexts checked, %lu worker loops, %ld prints\n",
          Seconds, WorkerCount, ControllerCount, Rounds, Checked, Loops, Prints);
}

#endif

START_TEST(ThreadContextNpx)
{
#if defined(_M_IX86) && defined(__GNUC__)
    RunTest();
#else
    skip("Test only for x86 with GCC\n");
#endif
}
