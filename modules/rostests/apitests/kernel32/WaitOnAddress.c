/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for WaitOnAddress, WakeByAddressSingle and WakeByAddressAll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The error codes and the wake loops follow Wine's kernelbase sync tests.
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_WaitOnAddress)(volatile VOID *, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI *FN_WakeByAddress)(PVOID);

typedef struct _API
{
    const char *Name;
    FN_WaitOnAddress Wait;
    FN_WakeByAddress WakeAll;
    FN_WakeByAddress WakeSingle;
} API;

static API g_Api;

#define PENDING_ERROR 0xdeadbeef

static
void
Test_Errors(void)
{
    LONG Address = 0, Compare = 0;
    BOOL Ret;
    ULONG Start;

    SetLastError(PENDING_ERROR);
    g_Api.WakeSingle(NULL);
    ok_eq_ulong(GetLastError(), PENDING_ERROR);
    g_Api.WakeAll(NULL);
    ok_eq_ulong(GetLastError(), PENDING_ERROR);

    /* The size is checked before the pointers are used. */
    SetLastError(PENDING_ERROR);
    Ret = g_Api.Wait(NULL, NULL, 0, 0);
    ok_eq_int(Ret, FALSE);
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);

    SetLastError(PENDING_ERROR);
    Ret = g_Api.Wait(&Address, &Compare, 5, 0);
    ok_eq_int(Ret, FALSE);
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);

    /* Equal value, no time: timeout. */
    SetLastError(PENDING_ERROR);
    Start = GetTickCount();
    Ret = g_Api.Wait(&Address, &Compare, sizeof(Address), 0);
    ok_eq_int(Ret, FALSE);
    ok_eq_ulong(GetLastError(), ERROR_TIMEOUT);
    ok(GetTickCount() - Start < 1000, "zero timeout took %lu ms\n", GetTickCount() - Start);

    SetLastError(PENDING_ERROR);
    Start = GetTickCount();
    Ret = g_Api.Wait(&Address, &Compare, sizeof(Address), 100);
    ok_eq_int(Ret, FALSE);
    ok_eq_ulong(GetLastError(), ERROR_TIMEOUT);
    ok(GetTickCount() - Start >= 80 && GetTickCount() - Start < 2000, "100 ms timeout took %lu ms\n", GetTickCount() - Start);

    /* A different value returns TRUE and leaves the last error alone. */
    Compare = 1;
    SetLastError(PENDING_ERROR);
    Ret = g_Api.Wait(&Address, &Compare, sizeof(Address), 0);
    ok_eq_int(Ret, TRUE);
    ok(GetLastError() == PENDING_ERROR || broken(GetLastError() == ERROR_SUCCESS) /* Windows 8 */,
       "got error %lu\n", GetLastError());
    SetLastError(PENDING_ERROR);
    Ret = g_Api.Wait(&Address, &Compare, sizeof(Address), INFINITE);
    ok_eq_int(Ret, TRUE);
}

static
void
Test_Sizes(void)
{
    ULONGLONG Address = 0, Compare;
    SIZE_T Size;
    BOOL Ret;

    for (Size = 1; Size <= 4; Size <<= 1)
    {
        /* Bytes above the size are ignored: the low Size bytes are equal, everything above them is not. */
        Compare = ~0ULL << (Size * 8);
        SetLastError(PENDING_ERROR);
        Ret = g_Api.Wait(&Address, &Compare, Size, 20);
        ok_eq_int(Ret, FALSE);
        ok_eq_ulong(GetLastError(), ERROR_TIMEOUT);

        /* A compare of twice the size takes in those bytes. */
        SetLastError(PENDING_ERROR);
        Ret = g_Api.Wait(&Address, &Compare, Size << 1, 20);
        ok_eq_int(Ret, TRUE);
    }
}

static LONG g_Address;
static volatile LONG g_WaitFailures;

static
DWORD
WINAPI
CountdownThread(PVOID Context)
{
    LONG Compare;

    do
    {
        while (!(Compare = g_Address))
        {
            if (!g_Api.Wait(&g_Address, &Compare, sizeof(Compare), INFINITE))
                InterlockedIncrement(&g_WaitFailures);
        }
    } while (InterlockedCompareExchange(&g_Address, Compare - 1, Compare) != Compare);

    return 0;
}

static
void
Test_Wake(void)
{
    HANDLE Threads[8];
    DWORD Index, Count;
    int i;

    /* Wake all */
    g_Address = 0;
    g_WaitFailures = 0;
    for (i = 0; i < ARRAY_SIZE(Threads); i++)
        Threads[i] = CreateThread(NULL, 0, CountdownThread, NULL, 0, NULL);
    Sleep(100);
    g_Address = ARRAY_SIZE(Threads);
    g_Api.WakeAll(&g_Address);
    Index = WaitForMultipleObjects(ARRAY_SIZE(Threads), Threads, TRUE, 5000);
    ok_eq_ulong(Index, WAIT_OBJECT_0);
    for (i = 0; i < ARRAY_SIZE(Threads); i++)
        CloseHandle(Threads[i]);
    ok_eq_long(g_Address, 0);
    ok_eq_long(g_WaitFailures, 0);

    /* Wake single */
    g_Address = 0;
    for (i = 0; i < ARRAY_SIZE(Threads); i++)
        Threads[i] = CreateThread(NULL, 0, CountdownThread, NULL, 0, NULL);
    Sleep(100);
    Count = ARRAY_SIZE(Threads);
    g_Address = ARRAY_SIZE(Threads);
    while (Count)
    {
        g_Api.WakeSingle(&g_Address);
        Index = WaitForMultipleObjects(Count, Threads, FALSE, 2000);
        ok(Index < WAIT_OBJECT_0 + Count, "got %lu\n", Index);
        if (Index >= WAIT_OBJECT_0 + Count)
            break;
        CloseHandle(Threads[Index]);
        memmove(&Threads[Index], &Threads[Index + 1], (Count - Index - 1) * sizeof(Threads[0]));
        Count--;
    }
    ok_eq_long(g_Address, 0);
    ok_eq_long(g_WaitFailures, 0);
}

/* The same lost-wakeup stress as in the ntdll test, through the Win32 calls. A consumer sleeps on
   its own address only; in the first phase its timeout is far longer than the test, so a lost
   wake leaves tokens that the drain below never sees taken. */
#define SEM_COUNT      12
#define SEM_PER_ADDR   2
#define SEM_PRODUCERS  4
#define SEM_TOKENS     6000

static LONG g_Sem[SEM_COUNT * 4];       /* one value per 16 bytes, so addresses neighbour each other */
static volatile LONG g_Taken;
static volatile LONG g_Stop;
static BOOLEAN g_StressMixed;

static
volatile LONG *
SemAt(ULONG Index)
{
    return &g_Sem[(Index % SEM_COUNT) * 4];
}

static
DWORD
WINAPI
SemProducer(PVOID Context)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Context * 2654435761u + 1;
    LONG i;

    for (i = 0; i < SEM_TOKENS && !g_Stop; i++)
    {
        volatile LONG *Sem;

        Seed = Seed * 1103515245 + 12345;
        Sem = SemAt(Seed >> 8);
        while (*Sem >= 50 && !g_Stop)
            Sleep(1);
        InterlockedIncrement(Sem);
        if (i % 9 == 8)
            g_Api.WakeAll((PVOID)Sem);
        else
            g_Api.WakeSingle((PVOID)Sem);
        if ((i & 7) == 7)
            Sleep(1);
    }
    return 0;
}

static
DWORD
WINAPI
SemConsumer(PVOID Context)
{
    ULONG Id = (ULONG)(ULONG_PTR)Context;
    volatile LONG *Sem = SemAt(Id);
    DWORD Timeout = g_StressMixed && (Id & 1) ? 1 : 300000;

    while (!g_Stop)
    {
        LONG Count = *Sem, Zero = 0;

        if (Count > 0)
        {
            if (InterlockedCompareExchange(Sem, Count - 1, Count) == Count)
                InterlockedIncrement(&g_Taken);
            continue;
        }
        g_Api.Wait(Sem, &Zero, sizeof(Zero), Timeout);
    }
    return 0;
}

static
BOOLEAN
Test_Stress(BOOLEAN Mixed)
{
    const int Consumers = SEM_COUNT * SEM_PER_ADDR;
    HANDLE Threads[SEM_PRODUCERS + SEM_COUNT * SEM_PER_ADDR];
    const LONG Total = SEM_PRODUCERS * SEM_TOKENS;
    BOOLEAN Success;
    DWORD Wait;
    int i;

    memset(g_Sem, 0, sizeof(g_Sem));
    g_Taken = g_Stop = 0;
    g_StressMixed = Mixed;

    for (i = 0; i < Consumers; i++)
        Threads[SEM_PRODUCERS + i] = CreateThread(NULL, 0, SemConsumer, (PVOID)(ULONG_PTR)i, 0, NULL);
    for (i = 0; i < SEM_PRODUCERS; i++)
        Threads[i] = CreateThread(NULL, 0, SemProducer, (PVOID)(ULONG_PTR)i, 0, NULL);

    Wait = WaitForMultipleObjects(SEM_PRODUCERS, Threads, TRUE, 120000);
    ok_eq_ulong(Wait, WAIT_OBJECT_0);
    for (i = 0; i < 300 && g_Taken != Total; i++)
        Sleep(100);
    ok(g_Taken == Total, "%ld of %ld tokens taken\n", g_Taken, Total);
    Success = (g_Taken == Total);

    /* A consumer may have checked the flag just before it went to sleep, so the wake is repeated. */
    g_Stop = 1;
    for (Wait = WAIT_OBJECT_0, i = 0; i < Consumers && Wait == WAIT_OBJECT_0; i++)
    {
        ULONG Join = GetTickCount();

        do
        {
            int j;

            for (j = 0; j < SEM_COUNT; j++)
                g_Api.WakeAll((PVOID)SemAt(j));
            Wait = WaitForSingleObject(Threads[SEM_PRODUCERS + i], 50);
        } while (Wait == WAIT_TIMEOUT && GetTickCount() - Join < 30000);
    }
    ok_eq_ulong(Wait, WAIT_OBJECT_0);
    Success = Success && (Wait == WAIT_OBJECT_0);
    for (i = 0; i < SEM_PRODUCERS + Consumers; i++)
        CloseHandle(Threads[i]);
    return Success;
}

static
BOOL
LoadApi(HMODULE Module, const char *Name)
{
    g_Api.Name = Name;
    g_Api.Wait = (FN_WaitOnAddress)GetProcAddress(Module, "WaitOnAddress");
    g_Api.WakeAll = (FN_WakeByAddress)GetProcAddress(Module, "WakeByAddressAll");
    g_Api.WakeSingle = (FN_WakeByAddress)GetProcAddress(Module, "WakeByAddressSingle");
    return g_Api.Wait && g_Api.WakeAll && g_Api.WakeSingle;
}

static
void
RunAll(BOOL Stress)
{
    trace("Testing the functions of %s\n", g_Api.Name);
    Test_Errors();
    Test_Sizes();
    Test_Wake();
    if (Stress)
    {
        if (Test_Stress(FALSE))
            Test_Stress(TRUE);
    }
}

START_TEST(WaitOnAddress)
{
    HMODULE Module;

    /* Programs import these through the API set, which Windows 8 introduced. */
    Module = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    if (Module == NULL || !LoadApi(Module, "api-ms-win-core-synch-l1-2-0"))
    {
        ok(!is_reactos(), "api-ms-win-core-synch-l1-2-0 does not provide WaitOnAddress (error %lu)\n", GetLastError());
        skip("WaitOnAddress is not available\n");
        return;
    }
    RunAll(TRUE);

    /* ReactOS has no kernelbase.dll under that name; its kernel32 and kernelbase_ros hold the functions. */
    if (is_reactos())
    {
        ok(LoadApi(GetModuleHandleW(L"kernel32.dll"), "kernel32"), "kernel32 does not export WaitOnAddress\n");
        if (g_Api.Wait)
            RunAll(FALSE);
        Module = LoadLibraryW(L"kernelbase_ros.dll");
        if (Module != NULL)
        {
            ok(LoadApi(Module, "kernelbase_ros"), "kernelbase_ros does not export WaitOnAddress\n");
            if (g_Api.Wait)
                Test_Wake();
        }
    }
}
