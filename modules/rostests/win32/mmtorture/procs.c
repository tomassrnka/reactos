/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Workload (b): many short-lived processes and threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

#define SHARED_DATA_NAME L"MmTortureProcsData"
#define SHARED_DATA_PAGES 1024
#define SHARED_DATA_SEED 0x5EC7105EC7105EC7ULL

static const char *ChildModes[] = { "quick", "alloc", "threads", "map", "crash", "kill", "stack", "dll" };

static ULONG
Recurse(ULONG Depth, volatile UCHAR *Previous)
{
    volatile UCHAR Frame[2048];

    Frame[0] = (UCHAR)Depth;
    Frame[sizeof(Frame) - 1] = Previous ? Previous[0] : 0;
    if (Depth == 0)
        return Frame[0];
    return Recurse(Depth - 1, Frame) + Frame[sizeof(Frame) - 1];
}

/* Set by the worker's own thread churn: its process lives on, so every allocation must be freed.
   The churn passes modes below 384 (a multiple of 8, 3 and 128), so the flag never collides. */
#define CHILD_THREAD_FREE 0x80000000

static DWORD WINAPI
ChildThread(PVOID Context)
{
    BOOL AlwaysFree = ((ULONG)(ULONG_PTR)Context & CHILD_THREAD_FREE) != 0;
    ULONG Mode = (ULONG)(ULONG_PTR)Context & ~CHILD_THREAD_FREE;
    PUCHAR P;
    SIZE_T Size = (Mode % 8 + 1) * 64 * 1024, i;

    P = VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
    if (P)
    {
        for (i = 0; i < Size; i += PAGE_SIZE)
            P[i] = (UCHAR)i;
        if ((Mode & 1) || AlwaysFree)
            VirtualFree(P, 0, MEM_RELEASE);
    }
    if (Mode % 3 == 0)
        Recurse(64 + Mode % 128, NULL);
    return 0;
}

static int
ChildMap(VOID)
{
    HANDLE Section;
    PUCHAR View, Copy;
    SIZE_T i;
    int Result = 0;

    Section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_COPY, FALSE, SHARED_DATA_NAME);
    if (!Section)
        return 2;
    View = MapViewOfFile(Section, FILE_MAP_READ, 0, 0, 0);
    Copy = MapViewOfFile(Section, FILE_MAP_COPY, 0, 0, 0);
    if (!View || !Copy)
        return 3;
    for (i = 0; i < SHARED_DATA_PAGES; i += 7)
    {
        if (!MmtCheckPage(View + i * PAGE_SIZE, SHARED_DATA_SEED, i, FALSE, "child-map"))
        {
            Result = 4;
            break;
        }
        /* Copy-on-write: our write must stay private */
        Copy[i * PAGE_SIZE + 8] ^= 0xFF;
    }
    for (i = 0; i < SHARED_DATA_PAGES && !Result; i += 7)
    {
        if (!MmtCheckPage(View + i * PAGE_SIZE, SHARED_DATA_SEED, i, FALSE, "child-map-after-cow"))
            Result = 5;
    }
    /* Exit with the views mapped half the time */
    if (GetTickCount() & 1)
    {
        UnmapViewOfFile(View);
        UnmapViewOfFile(Copy);
    }
    CloseHandle(Section);
    return Result;
}

/* child MODE */
int
MmtChildMain(int argc, char **argv)
{
    const char *Mode = argc > 2 ? argv[2] : "quick";
    HANDLE Threads[32];
    ULONG i, Count = 0;

    if (!strcmp(Mode, "quick"))
        return 0;
    if (!strcmp(Mode, "alloc"))
    {
        SIZE_T Size = ((GetTickCount() % 16) + 1) << 20, j;
        PUCHAR P = VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
        PVOID H[64];
        if (P)
        {
            for (j = 0; j < Size; j += PAGE_SIZE)
                P[j] = 1;
        }
        for (j = 0; j < 64; j++)
            H[j] = HeapAlloc(GetProcessHeap(), 0, 100 + j * 97);
        (void)H;
        return 0;
    }
    if (!strcmp(Mode, "threads"))
    {
        for (i = 0; i < 24; i++)
        {
            Threads[Count] = CreateThread(NULL, (i & 1) ? 0 : 256 * 1024, ChildThread, (PVOID)(ULONG_PTR)i, 0, NULL);
            if (Threads[Count])
                Count++;
        }
        WaitForMultipleObjects(Count, Threads, TRUE, 60000);
        return 0;
    }
    if (!strcmp(Mode, "map"))
        return ChildMap();
    if (!strcmp(Mode, "crash"))
    {
        *(volatile int *)(ULONG_PTR)0x10 = 1;
        return 9;
    }
    if (!strcmp(Mode, "kill"))
    {
        for (;;)
        {
            PUCHAR P = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
            if (P)
            {
                P[0] = 1;
                P[1 << 19] = 1;
                VirtualFree(P, 0, MEM_RELEASE);
            }
            /* One thread at a time: churn, not a thread bomb */
            HANDLE T = CreateThread(NULL, 0, ChildThread, (PVOID)(ULONG_PTR)3, 0, NULL);
            if (T)
            {
                WaitForSingleObject(T, INFINITE);
                CloseHandle(T);
            }
        }
    }
    if (!strcmp(Mode, "stack"))
    {
        /* About 700 KB of the 1 MB stack, through the guard page */
        return Recurse(330, NULL) == 0xFFFFFFFF ? 1 : 0;
    }
    if (!strcmp(Mode, "dll"))
    {
        static const WCHAR *Dlls[] = { L"user32.dll", L"gdi32.dll", L"advapi32.dll", L"shlwapi.dll", L"ws2_32.dll", L"ole32.dll" };
        HMODULE Modules[_countof(Dlls)];
        for (i = 0; i < _countof(Dlls); i++)
            Modules[i] = LoadLibraryW(Dlls[i]);
        for (i = 0; i < _countof(Dlls); i++)
        {
            if (!Modules[i])
                return 10 + i;
        }
        for (i = _countof(Dlls); i-- > 0;)
            FreeLibrary(Modules[i]);
        return 0;
    }
    return 99;
}

typedef struct _SPAWNER
{
    LONG Slot;
    ULONG Index;
} SPAWNER;

static DWORD WINAPI
SpawnerThread(PVOID Context)
{
    SPAWNER *S = Context;
    MMT_RNG Rng;
    WCHAR Args[64];

    MmtRngInit(&Rng, GetTickCount() ^ (S->Index << 20) ^ GetCurrentProcessId());
    while (!MmtShouldStop())
    {
        ULONG Mode = MmtRandRange(&Rng, 0, _countof(ChildModes) - 1);
        DWORD Code = 0, Expected = 0;
        HANDLE Process;

        _snwprintf(Args, _countof(Args), L"child %S", ChildModes[Mode]);
        if (!MmtSpawn(Args, FALSE, 0, NULL, &Process))
        {
            MmtLog("procs: CreateProcess failed %lu", GetLastError());
            Sleep(500);
            continue;
        }
        InterlockedIncrement(&MmtShared->Children);
        if (!strcmp(ChildModes[Mode], "kill"))
        {
            Sleep(MmtRandRange(&Rng, 0, 300));
            TerminateProcess(Process, 0xDEAD);
            Expected = 0xDEAD;
        }
        else if (!strcmp(ChildModes[Mode], "crash"))
        {
            Expected = STATUS_ACCESS_VIOLATION;
        }
        if (WaitForSingleObject(Process, 180000) != WAIT_OBJECT_0)
        {
            MmtFail("procs: child '%s' did not end within 180 s", ChildModes[Mode]);
            InterlockedIncrement(&MmtShared->ChildFailures);
            TerminateProcess(Process, 0xDEAD);
            WaitForSingleObject(Process, 60000);
        }
        else
        {
            GetExitCodeProcess(Process, &Code);
            if (Code != Expected)
            {
                MmtFail("procs: child '%s' exited %08lx, expected %08lx", ChildModes[Mode], Code, Expected);
                InterlockedIncrement(&MmtShared->ChildFailures);
            }
        }
        CloseHandle(Process);
        MmtProgress(S->Slot);
    }
    return 0;
}

static DWORD WINAPI
ThreadChurn(PVOID Context)
{
    LONG Slot = (LONG)(ULONG_PTR)Context;
    ULONG n = 0;

    while (!MmtShouldStop())
    {
        HANDLE Threads[16];
        ULONG i, Count = 0;
        DWORD Wait;
        for (i = 0; i < 16; i++)
        {
            Threads[Count] = CreateThread(NULL, (i & 3) ? 0 : 128 * 1024, ChildThread,
                                         (PVOID)(ULONG_PTR)(((n + i) % 384) | CHILD_THREAD_FREE), 0, NULL);
            if (Threads[Count])
                Count++;
        }
        if (!Count)
        {
            MmtFail("procs: CreateThread failed %lu", GetLastError());
            Sleep(500);
            continue;
        }
        Wait = WaitForMultipleObjects(Count, Threads, TRUE, 120000);
        if (Wait != WAIT_OBJECT_0)
        {
            MmtFail("procs: threads did not end within 120 s (%lx)", Wait == WAIT_FAILED ? GetLastError() : Wait);
            /* Closing the handles would not end them: wait for this round, and start no other unless it ends */
            while (Wait == WAIT_TIMEOUT && !MmtShouldStop())
                Wait = WaitForMultipleObjects(Count, Threads, TRUE, 10000);
        }
        for (i = 0; i < Count; i++)
            CloseHandle(Threads[i]);
        if (Wait != WAIT_OBJECT_0)
            break;
        n += 16;
        MmtProgress(Slot);
    }
    return 0;
}

/* procs SPAWNERS */
int
MmtProcsMain(int argc, char **argv)
{
    ULONG Spawners = min(MmtArgUlong(argc, argv, 2, 4), 30);
    SPAWNER S[30];
    HANDLE Handles[32], Section;
    PUCHAR View;
    ULONG i, Count = 0;
    LONG Slot;

    MmtOpenShared(FALSE);
    Slot = MmtAllocSlot("procs");

    Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 SHARED_DATA_PAGES * PAGE_SIZE, SHARED_DATA_NAME);
    View = Section ? MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!View)
    {
        MmtFail("procs: shared data section failed %lu", GetLastError());
        return 1;
    }
    for (i = 0; i < SHARED_DATA_PAGES; i++)
        MmtFillPage(View + i * PAGE_SIZE, SHARED_DATA_SEED, i);

    for (i = 0; i < Spawners; i++)
    {
        S[i].Slot = Slot;
        S[i].Index = i;
        Handles[Count] = CreateThread(NULL, 0, SpawnerThread, &S[i], 0, NULL);
        if (Handles[Count])
            Count++;
    }
    Handles[Count] = CreateThread(NULL, 0, ThreadChurn, (PVOID)(ULONG_PTR)Slot, 0, NULL);
    if (Handles[Count])
        Count++;
    WaitForMultipleObjects(Count, Handles, TRUE, INFINITE);

    for (i = 0; i < SHARED_DATA_PAGES; i++)
    {
        if (!MmtCheckPage(View + i * PAGE_SIZE, SHARED_DATA_SEED, i, FALSE, "procs-shared-final"))
            break;
    }
    UnmapViewOfFile(View);
    CloseHandle(Section);
    MmtLog("procs done");
    return 0;
}
