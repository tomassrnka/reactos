/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for RtlWaitOnAddress, RtlWakeAddressSingle and RtlWakeAddressAll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The value-size and timeout cases follow Wine's ntdll sync tests.
 */

#include "precomp.h"
#include <pseh/pseh2.h>

typedef NTSTATUS (NTAPI *FN_RtlWaitOnAddress)(const volatile VOID *, PVOID, SIZE_T, PLARGE_INTEGER);
typedef VOID (NTAPI *FN_RtlWakeAddress)(const volatile VOID *);

static FN_RtlWaitOnAddress pWait;
static FN_RtlWakeAddress pWakeAll;
static FN_RtlWakeAddress pWakeSingle;

#define SECS(n) (-(LONGLONG)(n) * 10000000)
#define MSECS(n) (-(LONGLONG)(n) * 10000)

static
ULONG
ElapsedMs(ULONG Start)
{
    return GetTickCount() - Start;
}

/* One thread waiting on one address. */
typedef struct _WAITER
{
    HANDLE Thread;
    volatile VOID *Address;
    ULONGLONG Compare;
    SIZE_T Size;
    LONGLONG Timeout;
    volatile NTSTATUS Status;
    volatile LONG Done;
    volatile LONG Order;
} WAITER;

static volatile LONG g_DoneCounter;

static
DWORD
WINAPI
WaiterThread(PVOID Context)
{
    WAITER *Waiter = Context;
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = Waiter->Timeout;
    Waiter->Status = pWait(Waiter->Address, &Waiter->Compare, Waiter->Size, &Timeout);
    Waiter->Order = InterlockedIncrement(&g_DoneCounter);
    Waiter->Done = 1;
    return 0;
}

static
void
StartWaiter(WAITER *Waiter, volatile VOID *Address, SIZE_T Size, ULONGLONG Compare, LONG TimeoutMs)
{
    DWORD Id;

    Waiter->Address = Address;
    Waiter->Size = Size;
    Waiter->Compare = Compare;
    Waiter->Timeout = MSECS(TimeoutMs);
    Waiter->Status = 0xdeadbeef;
    Waiter->Done = 0;
    Waiter->Order = 0;
    Waiter->Thread = CreateThread(NULL, 0, WaiterThread, Waiter, 0, &Id);
    ok(Waiter->Thread != NULL, "CreateThread failed %lu\n", GetLastError());
}

static
void
FinishWaiter(WAITER *Waiter)
{
    if (Waiter->Thread != NULL)
    {
        DWORD Wait = WaitForSingleObject(Waiter->Thread, 20000);

        ok(Wait == WAIT_OBJECT_0, "waiter thread did not finish: %lu\n", Wait);
        if (Wait != WAIT_OBJECT_0)
        {
            /* It still points into the caller's stack and may hold the wait queue. */
            ExitProcess(1);
        }
        CloseHandle(Waiter->Thread);
        Waiter->Thread = NULL;
    }
}

/* Runs a wake on its own thread, so a wake that hangs fails the test instead of stopping it. */
typedef struct _WAKE_JOB
{
    const volatile VOID *Address;
    BOOLEAN All;
} WAKE_JOB;

static
DWORD
WINAPI
WakeJobThread(PVOID Context)
{
    WAKE_JOB *Job = Context;

    if (Job->All)
        pWakeAll(Job->Address);
    else
        pWakeSingle(Job->Address);
    return 0;
}

static
BOOLEAN
WakeWithWatchdog(const volatile VOID *Address, BOOLEAN All)
{
    WAKE_JOB Job = { Address, All };
    HANDLE Thread = CreateThread(NULL, 0, WakeJobThread, &Job, 0, NULL);
    DWORD Wait;

    ok(Thread != NULL, "CreateThread failed %lu\n", GetLastError());
    if (Thread == NULL)
        return FALSE;
    Wait = WaitForSingleObject(Thread, 20000);
    ok(Wait == WAIT_OBJECT_0, "wake did not return: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        /* Job is on this stack. Leave the thread; the process ends the test run. */
        ExitProcess(1);
    }
    CloseHandle(Thread);
    return TRUE;
}

static
void
Test_Sizes(void)
{
    static const ULONGLONG Value = 0x1122334455667788ULL;
    ULONGLONG Address, Compare;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    SIZE_T Size;
    ULONG Start;

    for (Size = 1; Size <= 8; Size <<= 1)
    {
        /* The first Size bytes match, every byte after them differs. */
        Address = Value;
        Compare = ~Value;
        memcpy(&Compare, &Address, Size);

        Timeout.QuadPart = MSECS(60);
        Start = GetTickCount();
        Status = pWait(&Address, &Compare, Size, &Timeout);
        ok_hex(Status, STATUS_TIMEOUT);
        ok(ElapsedMs(Start) >= 40, "size %lu: timed out after %lu ms\n", (ULONG)Size, ElapsedMs(Start));
        ok(Address == Value, "address changed\n");

        /* The last byte inside the size differs. */
        ((PUCHAR)&Compare)[Size - 1] ^= 0x80;
        Timeout.QuadPart = SECS(10);
        Start = GetTickCount();
        Status = pWait(&Address, &Compare, Size, &Timeout);
        ok_hex(Status, STATUS_SUCCESS);
        ok(ElapsedMs(Start) < 5000, "size %lu: different value waited %lu ms\n", (ULONG)Size, ElapsedMs(Start));

        /* The first byte differs. */
        memcpy(&Compare, &Address, Size);
        ((PUCHAR)&Compare)[0] ^= 0x01;
        Status = pWait(&Address, &Compare, Size, &Timeout);
        ok_hex(Status, STATUS_SUCCESS);
    }
}

static
void
Test_InvalidParameters(void)
{
    ULONGLONG Address = 0, Compare = 0;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    SIZE_T Size;

    Timeout.QuadPart = MSECS(10);
    for (Size = 0; Size <= 17; Size++)
    {
        if (Size == 1 || Size == 2 || Size == 4 || Size == 8)
            continue;
        Status = pWait(&Address, &Compare, Size, &Timeout);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
    }

    /* No waiter: nothing happens. */
    pWakeSingle(NULL);
    pWakeAll(NULL);
}

static
void
Test_Timeouts(void)
{
    LONG Address = 0, Compare = 0;
    LARGE_INTEGER Timeout, Now;
    NTSTATUS Status;
    ULONG Start;

    /* Relative. */
    Timeout.QuadPart = MSECS(100);
    Start = GetTickCount();
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_TIMEOUT);
    ok(ElapsedMs(Start) >= 80 && ElapsedMs(Start) <= 2000, "relative: timed out after %lu ms\n", ElapsedMs(Start));

    /* Absolute. */
    NtQuerySystemTime(&Now);
    Timeout.QuadPart = Now.QuadPart + 100 * 10000;
    Start = GetTickCount();
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_TIMEOUT);
    ok(ElapsedMs(Start) >= 80 && ElapsedMs(Start) <= 2000, "absolute: timed out after %lu ms\n", ElapsedMs(Start));

    /* Zero, and an absolute time in the past, do not block. */
    Timeout.QuadPart = 0;
    Start = GetTickCount();
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_TIMEOUT);
    ok(ElapsedMs(Start) < 1000, "zero timeout took %lu ms\n", ElapsedMs(Start));

    Timeout.QuadPart = 1;
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_TIMEOUT);

    /* A different value returns at once, whatever the timeout. */
    Compare = 1;
    Timeout.QuadPart = 0;
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_SUCCESS);
    Status = pWait(&Address, &Compare, sizeof(Address), NULL);
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
Test_WakeNothing(void)
{
    LONG Address = 0, Compare = 0;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    int i;

    /* A wake that finds no waiter is not remembered. */
    pWakeSingle(&Address);
    pWakeAll(&Address);
    Timeout.QuadPart = MSECS(80);
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_TIMEOUT);
    ok_eq_long(Address, 0);

    /* Waiters that left by timeout leave nothing behind. */
    Timeout.QuadPart = 0;
    for (i = 0; i < 2000; i++)
    {
        Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
        if (Status != STATUS_TIMEOUT)
        {
            ok_hex(Status, STATUS_TIMEOUT);
            break;
        }
        pWakeSingle(&Address);
        pWakeAll(&Address);
    }
}

static
void
Test_WakeSingle(void)
{
    WAITER Waiters[4];
    LONG Address = 0;
    int i, Finished;

    g_DoneCounter = 0;
    for (i = 0; i < 4; i++)
    {
        StartWaiter(&Waiters[i], &Address, sizeof(Address), 0, 10000);
        Sleep(100);
    }
    Sleep(100);
    for (i = 0; i < 4; i++)
        ok(!Waiters[i].Done, "waiter %d returned early, status %lx\n", i, Waiters[i].Status);

    /* The value is untouched, so each wake releases one waiter and the first in line goes first. */
    for (i = 0; i < 4; i++)
    {
        int j;

        pWakeSingle(&Address);
        Sleep(250);
        Finished = 0;
        for (j = 0; j < 4; j++)
            Finished += Waiters[j].Done;
        ok(Finished == i + 1, "after %d wakes %d waiters returned\n", i + 1, Finished);
        ok(Waiters[i].Done, "waiter %d did not go in start order\n", i);
        ok_hex(Waiters[i].Status, STATUS_SUCCESS);
    }
    for (i = 0; i < 4; i++)
        FinishWaiter(&Waiters[i]);
}

static
void
Test_WakeAll(void)
{
    WAITER Waiters[6];
    LONG Address = 0;
    int i;

    for (i = 0; i < 6; i++)
        StartWaiter(&Waiters[i], &Address, sizeof(Address), 0, 10000);
    Sleep(200);
    for (i = 0; i < 6; i++)
        ok(!Waiters[i].Done, "waiter %d returned early\n", i);

    pWakeAll(&Address);
    for (i = 0; i < 6; i++)
    {
        FinishWaiter(&Waiters[i]);
        ok(Waiters[i].Done, "waiter %d not released\n", i);
        ok_hex(Waiters[i].Status, STATUS_SUCCESS);
    }
}

/* More waiters than one wake batch, all going back to waiting at once: the wake call
   has to end, it may not chase the waiters it has just released. */
#define REQUEUE_WAITERS 80
static LONG g_RequeueAddress;
static volatile LONG g_RequeueStop;
static volatile LONG g_RequeueWakes;

static
DWORD
WINAPI
RequeueThread(PVOID Context)
{
    LARGE_INTEGER Timeout;
    LONG Compare = 0;

    Timeout.QuadPart = SECS(30);
    while (!g_RequeueStop)
    {
        if (pWait(&g_RequeueAddress, &Compare, sizeof(Compare), &Timeout) == STATUS_SUCCESS)
            InterlockedIncrement(&g_RequeueWakes);
    }
    return 0;
}

static
void
Test_WakeAllBounded(void)
{
    HANDLE Threads[REQUEUE_WAITERS];
    DWORD Wait;
    int i, j;

    g_RequeueAddress = 0;
    g_RequeueStop = 0;
    g_RequeueWakes = 0;
    for (i = 0; i < REQUEUE_WAITERS; i++)
        Threads[i] = CreateThread(NULL, 0, RequeueThread, NULL, 0, NULL);
    Sleep(500);

    WakeWithWatchdog(&g_RequeueAddress, TRUE);
    Sleep(200);
    ok(g_RequeueWakes >= REQUEUE_WAITERS, "%ld of %d waiters were woken\n", g_RequeueWakes, REQUEUE_WAITERS);

    g_RequeueStop = 1;
    for (i = 0; i < 20; i++)
    {
        WakeWithWatchdog(&g_RequeueAddress, TRUE);
        for (Wait = WAIT_OBJECT_0, j = 0; j < REQUEUE_WAITERS && Wait == WAIT_OBJECT_0; j++)
            Wait = WaitForSingleObject(Threads[j], 500);
        if (Wait == WAIT_OBJECT_0)
            break;
    }
    ok_eq_ulong(Wait, WAIT_OBJECT_0);
    for (i = 0; i < REQUEUE_WAITERS; i++)
        CloseHandle(Threads[i]);
}

static
void
Test_OtherAddress(void)
{
    /* Two addresses in the same cache line: only the exact address is woken. */
    static LONG Pair[2];
    WAITER Waiter;

    Pair[0] = Pair[1] = 0;
    StartWaiter(&Waiter, &Pair[0], sizeof(LONG), 0, 10000);
    Sleep(150);
    pWakeSingle(&Pair[1]);
    pWakeAll(&Pair[1]);
    Sleep(200);
    ok(!Waiter.Done, "waiter on the neighbour address was woken, status %lx\n", Waiter.Status);
    pWakeSingle(&Pair[0]);
    FinishWaiter(&Waiter);
    ok(Waiter.Done, "waiter not released\n");
    ok_hex(Waiter.Status, STATUS_SUCCESS);

    /* A waiter on a byte inside a wider value is woken only through its own address. */
    Pair[0] = 0;
    StartWaiter(&Waiter, (PUCHAR)&Pair[0] + 1, 1, 0, 10000);
    Sleep(150);
    pWakeAll(&Pair[0]);
    Sleep(200);
    ok(!Waiter.Done, "waiter on byte 1 was woken through the base address\n");
    pWakeAll((PUCHAR)&Pair[0] + 1);
    FinishWaiter(&Waiter);
    ok(Waiter.Done, "waiter not released\n");
}

static
void
Test_ChangeBeforeWait(void)
{
    /* The waiter sees a value change made before its wait began. */
    LONG Address = 0, Compare = 0;
    NTSTATUS Status;
    LARGE_INTEGER Timeout;

    Address = 1;
    pWakeSingle(&Address);
    Timeout.QuadPart = SECS(10);
    Status = pWait(&Address, &Compare, sizeof(Address), &Timeout);
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
Test_BadPointer(void)
{
    PUCHAR Region;
    ULONG Old;
    LONG Compare = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    LARGE_INTEGER Timeout;
    WAITER Waiter;

    Region = VirtualAlloc(NULL, 2 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(Region != NULL, "VirtualAlloc failed\n");
    if (Region == NULL)
        return;
    ok(VirtualProtect(Region, 4096, PAGE_NOACCESS, &Old), "VirtualProtect failed\n");

    /* An address the caller cannot read raises an exception in the caller. */
    Timeout.QuadPart = MSECS(10);
    _SEH2_TRY
    {
        Status = pWait(Region, &Compare, sizeof(Compare), &Timeout);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Windows leaves the faulting waiter's record in its queue, and a later wake of a colliding
       address crashes on it (measured on Windows 10), so the rest is for ReactOS only. */
    if (!is_reactos())
    {
        VirtualFree(Region, 0, MEM_RELEASE);
        return;
    }

    /* The failed call must not leave anything locked: an address one page up has the same
       hash bucket in some implementations. */
    *(volatile LONG *)(Region + 4096) = 0;
    StartWaiter(&Waiter, Region + 4096, sizeof(LONG), 0, 10000);
    Sleep(100);
    ok(!Waiter.Done, "waiter returned early\n");
    WakeWithWatchdog(Region + 4096, TRUE);
    FinishWaiter(&Waiter);
    ok(Waiter.Done, "waiter not released after the faulting call\n");
    ok_hex(Waiter.Status, STATUS_SUCCESS);

    /* A bad compare pointer faults as well. */
    _SEH2_TRY
    {
        Status = pWait(Region + 4096, Region, sizeof(Compare), &Timeout);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    VirtualFree(Region, 0, MEM_RELEASE);
}

/* Two threads hand a turn back and forth. A wake that gets lost leaves one of them
   waiting out its long timeout. */
static volatile LONG g_Turn;
static volatile LONG g_PingPongLost;
#define PINGPONG_ROUNDS 20000

static
DWORD
WINAPI
PingPongThread(PVOID Context)
{
    LONG Me = (LONG)(LONG_PTR)Context;
    LARGE_INTEGER Timeout;
    LONG Other = 1 - Me;
    int i;

    Timeout.QuadPart = SECS(20);
    for (i = 0; i < PINGPONG_ROUNDS; i++)
    {
        while (g_Turn != Me)
        {
            if (pWait(&g_Turn, &Other, sizeof(g_Turn), &Timeout) == STATUS_TIMEOUT)
            {
                InterlockedIncrement(&g_PingPongLost);
                return 1;
            }
        }
        InterlockedExchange(&g_Turn, Other);
        if (i & 1)
            pWakeSingle(&g_Turn);
        else
            pWakeAll(&g_Turn);
    }
    return 0;
}

static
void
Test_PingPong(void)
{
    HANDLE Threads[2];
    DWORD Wait;
    ULONG Start = GetTickCount();

    g_Turn = 0;
    g_PingPongLost = 0;
    Threads[0] = CreateThread(NULL, 0, PingPongThread, (PVOID)0, 0, NULL);
    Threads[1] = CreateThread(NULL, 0, PingPongThread, (PVOID)1, 0, NULL);
    Wait = WaitForMultipleObjects(2, Threads, TRUE, 120000);
    ok(Wait == WAIT_OBJECT_0, "ping-pong did not finish: %lu\n", Wait);
    ok(g_PingPongLost == 0, "%ld lost wakeups in ping-pong\n", g_PingPongLost);
    trace("ping-pong of %d rounds took %lu ms\n", PINGPONG_ROUNDS, ElapsedMs(Start));
    CloseHandle(Threads[0]);
    CloseHandle(Threads[1]);
}

/* Value helpers for the stress tests: slots of 1, 2, 4 and 8 bytes. */
static
ULONGLONG
LoadValue(volatile VOID *Address, SIZE_T Size)
{
    switch (Size)
    {
        case 1: return *(volatile UCHAR *)Address;
        case 2: return *(volatile USHORT *)Address;
        case 4: return *(volatile ULONG *)Address;
        default: return *(volatile ULONGLONG *)Address;
    }
}

static
BOOLEAN
CasValue(volatile VOID *Address, SIZE_T Size, ULONGLONG Old, ULONGLONG New)
{
    switch (Size)
    {
        case 1: return (UCHAR)_InterlockedCompareExchange8(Address, (CHAR)New, (CHAR)Old) == (UCHAR)Old;
        case 2: return (USHORT)_InterlockedCompareExchange16(Address, (SHORT)New, (SHORT)Old) == (USHORT)Old;
        case 4: return (ULONG)InterlockedCompareExchange(Address, (LONG)New, (LONG)Old) == (ULONG)Old;
        default: return (ULONGLONG)InterlockedCompareExchange64(Address, (LONGLONG)New, (LONGLONG)Old) == Old;
    }
}

#define STRESS_ADDRESSES 24
#define STRESS_PER_ADDRESS 2        /* consumers per address */
#define STRESS_PRODUCERS 6
#define STRESS_TOKENS    6000       /* per producer */
#define STRESS_LIMIT     50         /* tokens an address holds at most, so that a byte never wraps */

typedef struct _SLOT
{
    DECLSPEC_ALIGN(16) ULONGLONG Value;
    SIZE_T Size;
} SLOT;

static SLOT g_Slots[STRESS_ADDRESSES];
static volatile LONG g_Consumed;
static volatile LONG g_StressDone;
static BOOLEAN g_StressMixed;

static
ULONG
NextRandom(ULONG *Seed)
{
    *Seed = *Seed * 1103515245 + 12345;
    return (*Seed >> 8) & 0xFFFFFF;
}

static
DWORD
WINAPI
ProducerThread(PVOID Context)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Context * 7919 + 1;
    LONG i;

    for (i = 0; i < STRESS_TOKENS && !g_StressDone; i++)
    {
        SLOT *Slot = &g_Slots[NextRandom(&Seed) % STRESS_ADDRESSES];

        for (;;)
        {
            ULONGLONG Count = LoadValue(&Slot->Value, Slot->Size);

            if (g_StressDone)
                return 1;
            if (Count >= STRESS_LIMIT)
            {
                Sleep(1);
                continue;
            }
            if (CasValue(&Slot->Value, Slot->Size, Count, Count + 1))
                break;
        }

        if ((i & 15) == 15)
            pWakeAll(&Slot->Value);
        else
            pWakeSingle(&Slot->Value);

        /* A pause now and then lets the consumers run dry and go to sleep. */
        if ((i & 7) == 7)
            Sleep(1);
    }
    return 0;
}

/* A consumer sleeps on its own address only, so a token can wake nobody else. In the first
   phase its timeout is far longer than the test: a wake that gets lost leaves the token
   lying there and the drain below fails. In the mixed phase odd consumers use a tiny timeout,
   which makes timeouts race with wakes. */
static
DWORD
WINAPI
ConsumerThread(PVOID Context)
{
    ULONG Id = (ULONG)(ULONG_PTR)Context;
    SLOT *Slot = &g_Slots[Id % STRESS_ADDRESSES];
    BOOLEAN Short = g_StressMixed && (Id & 1);
    LARGE_INTEGER Timeout;
    ULONGLONG Zero = 0;

    Timeout.QuadPart = Short ? MSECS(1) : SECS(300);
    while (!g_StressDone)
    {
        ULONGLONG Count = LoadValue(&Slot->Value, Slot->Size);

        if (Count != 0)
        {
            if (CasValue(&Slot->Value, Slot->Size, Count, Count - 1))
                InterlockedIncrement(&g_Consumed);
            continue;
        }

        pWait(&Slot->Value, &Zero, Slot->Size, &Timeout);
    }
    return 0;
}

static
BOOLEAN
Test_Stress(BOOLEAN Mixed)
{
    const int Consumers = STRESS_ADDRESSES * STRESS_PER_ADDRESS;
    HANDLE Threads[STRESS_PRODUCERS + STRESS_ADDRESSES * STRESS_PER_ADDRESS];
    const LONG Total = STRESS_PRODUCERS * STRESS_TOKENS;
    ULONG Start = GetTickCount();
    BOOLEAN Success;
    DWORD Wait;
    int i;

    for (i = 0; i < STRESS_ADDRESSES; i++)
    {
        g_Slots[i].Value = 0;
        g_Slots[i].Size = 1 << (i & 3);
    }
    g_Consumed = 0;
    g_StressDone = 0;
    g_StressMixed = Mixed;

    for (i = 0; i < Consumers; i++)
        Threads[STRESS_PRODUCERS + i] = CreateThread(NULL, 0, ConsumerThread, (PVOID)(ULONG_PTR)i, 0, NULL);
    for (i = 0; i < STRESS_PRODUCERS; i++)
        Threads[i] = CreateThread(NULL, 0, ProducerThread, (PVOID)(ULONG_PTR)i, 0, NULL);

    Wait = WaitForMultipleObjects(STRESS_PRODUCERS, Threads, TRUE, 120000);
    ok(Wait == WAIT_OBJECT_0, "producers did not finish: %lu\n", Wait);

    /* Every token must be taken, by wakes alone. */
    for (i = 0; i < 300 && g_Consumed != Total; i++)
        Sleep(100);
    ok(g_Consumed == Total, "%ld of %ld tokens consumed\n", g_Consumed, Total);
    Success = (g_Consumed == Total);

    /* A consumer may have checked the flag just before it went to sleep, so the wake is repeated. */
    g_StressDone = 1;
    for (Wait = WAIT_OBJECT_0, i = 0; i < Consumers && Wait == WAIT_OBJECT_0; i++)
    {
        ULONG Join = GetTickCount();

        do
        {
            int j;

            for (j = 0; j < STRESS_ADDRESSES; j++)
                pWakeAll(&g_Slots[j].Value);
            Wait = WaitForSingleObject(Threads[STRESS_PRODUCERS + i], 50);
        } while (Wait == WAIT_TIMEOUT && ElapsedMs(Join) < 30000);
    }
    ok(Wait == WAIT_OBJECT_0, "consumers did not stop: %lu\n", Wait);
    Success = Success && (Wait == WAIT_OBJECT_0);

    for (i = 0; i < STRESS_PRODUCERS + Consumers; i++)
        CloseHandle(Threads[i]);
    trace("%s stress of %ld tokens, %d addresses, %d consumers took %lu ms\n",
          Mixed ? "mixed" : "blocking", Total, STRESS_ADDRESSES, Consumers, ElapsedMs(Start));
    return Success;
}

/* One thread bumps a generation and wakes everybody; every waiter must get through each
   generation it can see without sitting out its timeout. */
#define BROADCAST_WAITERS 24
#define BROADCAST_ROUNDS  4000
static volatile LONG64 g_Generation;
static volatile LONG g_BroadcastLost;

static
DWORD
WINAPI
BroadcastWaiter(PVOID Context)
{
    LONG64 Seen = 0, Current;
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = SECS(20);
    while ((Current = g_Generation) != BROADCAST_ROUNDS)
    {
        if (Current != Seen)
        {
            Seen = Current;
            continue;
        }
        if (pWait(&g_Generation, &Seen, sizeof(g_Generation), &Timeout) == STATUS_TIMEOUT)
        {
            InterlockedIncrement(&g_BroadcastLost);
            break;
        }
    }
    return 0;
}

static
void
Test_Broadcast(void)
{
    HANDLE Threads[BROADCAST_WAITERS];
    ULONG Start = GetTickCount();
    DWORD Wait;
    int i;

    g_Generation = 0;
    g_BroadcastLost = 0;
    for (i = 0; i < BROADCAST_WAITERS; i++)
        Threads[i] = CreateThread(NULL, 0, BroadcastWaiter, NULL, 0, NULL);

    for (i = 1; i <= BROADCAST_ROUNDS; i++)
    {
        InterlockedExchange64(&g_Generation, i);
        pWakeAll(&g_Generation);
        if ((i & 7) == 0)
            Sleep(1);
    }

    Wait = WaitForMultipleObjects(BROADCAST_WAITERS, Threads, TRUE, 60000);
    ok(Wait == WAIT_OBJECT_0, "broadcast waiters did not finish: %lu\n", Wait);
    ok(g_BroadcastLost == 0, "%ld lost wakeups in broadcast\n", g_BroadcastLost);
    for (i = 0; i < BROADCAST_WAITERS; i++)
        CloseHandle(Threads[i]);
    trace("broadcast of %d rounds took %lu ms\n", BROADCAST_ROUNDS, ElapsedMs(Start));
}

START_TEST(RtlWaitOnAddress)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    pWait = (FN_RtlWaitOnAddress)GetProcAddress(Ntdll, "RtlWaitOnAddress");
    pWakeAll = (FN_RtlWakeAddress)GetProcAddress(Ntdll, "RtlWakeAddressAll");
    pWakeSingle = (FN_RtlWakeAddress)GetProcAddress(Ntdll, "RtlWakeAddressSingle");
    if (pWait == NULL || pWakeAll == NULL || pWakeSingle == NULL)
    {
        /* Windows has them from version 8 on; ReactOS exports them at every export level. */
        ok(!is_reactos(), "RtlWaitOnAddress and RtlWakeAddress* are not all exported\n");
        skip("RtlWaitOnAddress is not exported\n");
        return;
    }

    Test_Sizes();
    Test_InvalidParameters();
    Test_Timeouts();
    Test_WakeNothing();
    Test_ChangeBeforeWait();
    Test_WakeSingle();
    Test_WakeAll();
    Test_WakeAllBounded();
    Test_OtherAddress();
    Test_PingPong();
    Test_Broadcast();
    /* After a failure its threads may still be running, so nothing more is started. */
    if (Test_Stress(FALSE))
        Test_Stress(TRUE);

    /* Last: on Windows the faulting call damages the wait queue. */
    Test_BadPointer();
}
