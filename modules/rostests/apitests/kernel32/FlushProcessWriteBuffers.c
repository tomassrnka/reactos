/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for FlushProcessWriteBuffers (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The .NET runtime calls it each time it suspends its threads for a garbage collection. A store that a thread
 * made before the call returns must be visible to the caller after it; the test runs the store buffering
 * pattern (two threads, each stores, flushes or fences, then loads) where a missing barrier lets both loads
 * see the old values, and checks that this never happens.
 */

#include "precomp.h"

typedef VOID (WINAPI *PFN_FlushProcessWriteBuffers)(VOID);
typedef NTSTATUS (NTAPI *PFN_NtFlushProcessWriteBuffers)(VOID);

static PFN_FlushProcessWriteBuffers pFlushProcessWriteBuffers;

#define ROUNDS 20000

static volatile LONG g_X, g_Y, g_R1, g_R2;
static volatile LONG g_Round, g_Done;
static HANDLE g_Start1, g_Start2, g_End1, g_End2;

/* The light side: a plain store and load, ordered by the heavy side's flush */
static DWORD WINAPI LightThread(PVOID Parameter)
{
    LONG i;
    UNREFERENCED_PARAMETER(Parameter);
    for (i = 0; i < ROUNDS; i++)
    {
        WaitForSingleObject(g_Start1, INFINITE);
        g_X = 1;
        _ReadWriteBarrier();
        g_R1 = g_Y;
        SetEvent(g_End1);
    }
    return 0;
}

static DWORD WINAPI HeavyThread(PVOID Parameter)
{
    LONG i;
    UNREFERENCED_PARAMETER(Parameter);
    for (i = 0; i < ROUNDS; i++)
    {
        WaitForSingleObject(g_Start2, INFINITE);
        g_Y = 1;
        pFlushProcessWriteBuffers();
        g_R2 = g_X;
        SetEvent(g_End2);
    }
    return 0;
}

START_TEST(FlushProcessWriteBuffers)
{
    PFN_NtFlushProcessWriteBuffers pNtFlushProcessWriteBuffers;
    HANDLE Threads[2];
    LONG i, Both = 0;
    SYSTEM_INFO Info;

    pFlushProcessWriteBuffers = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FlushProcessWriteBuffers");
    pNtFlushProcessWriteBuffers = (PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtFlushProcessWriteBuffers");
    if (!pFlushProcessWriteBuffers || !pNtFlushProcessWriteBuffers)
    {
        skip("FlushProcessWriteBuffers is not exported\n");
        return;
    }

    ok(pNtFlushProcessWriteBuffers() == STATUS_SUCCESS, "NtFlushProcessWriteBuffers failed\n");
    for (i = 0; i < 1000; i++)
        pFlushProcessWriteBuffers();

    GetSystemInfo(&Info);
    if (Info.dwNumberOfProcessors < 2)
    {
        skip("one processor, the ordering is not observable\n");
        return;
    }

    g_Start1 = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_Start2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_End1 = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_End2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    Threads[0] = CreateThread(NULL, 0, LightThread, NULL, 0, NULL);
    Threads[1] = CreateThread(NULL, 0, HeavyThread, NULL, 0, NULL);
    SetThreadAffinityMask(Threads[0], 1);
    SetThreadAffinityMask(Threads[1], 2);

    for (i = 0; i < ROUNDS; i++)
    {
        g_X = g_Y = 0;
        SetEvent(g_Start1);
        SetEvent(g_Start2);
        WaitForSingleObject(g_End1, INFINITE);
        WaitForSingleObject(g_End2, INFINITE);
        if (!g_R1 && !g_R2)
            Both++;
    }
    ok(Both == 0, "both loads saw the old values %ld times in %d rounds\n", Both, ROUNDS);

    WaitForMultipleObjects(2, Threads, TRUE, INFINITE);
    CloseHandle(Threads[0]);
    CloseHandle(Threads[1]);
    CloseHandle(g_Start1);
    CloseHandle(g_Start2);
    CloseHandle(g_End1);
    CloseHandle(g_End2);
}
