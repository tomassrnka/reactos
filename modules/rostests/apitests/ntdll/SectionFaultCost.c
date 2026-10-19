/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Page faults on a file-backed view must not sleep while memory is plentiful
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* The thread writes once to every page of a view of a new file. The pages lie past the valid data
   length, so they come from the free lists and no fault reads the disk: the thread is busy the whole
   time and its CPU time is close to the time that passed. A memory manager that sleeps a clock tick
   on every hundredth page it hands out, whatever the free memory, leaves the thread waiting for most
   of that time (ReactOS did: about 80 % of it). The best of three runs counts, so that a stall of the
   virtual machine does not fail the test. */

#define VIEW_MB     64
#define RUNS        3

static double Seconds(LONGLONG Ticks, LONGLONG Frequency)
{
    return (double)Ticks / (double)Frequency;
}

static double ThreadCpuSeconds(VOID)
{
    FILETIME Creation, Exit, Kernel, User;
    ULARGE_INTEGER K, U;

    GetThreadTimes(GetCurrentThread(), &Creation, &Exit, &Kernel, &User);
    K.LowPart = Kernel.dwLowDateTime;
    K.HighPart = Kernel.dwHighDateTime;
    U.LowPart = User.dwLowDateTime;
    U.HighPart = User.dwHighDateTime;
    return (double)(K.QuadPart + U.QuadPart) / 1e7;
}

static BOOL TouchView(PCWSTR Path, double *Wall, double *Cpu)
{
    SIZE_T Size = (SIZE_T)VIEW_MB << 20, Offset;
    LARGE_INTEGER Frequency, Start, End;
    HANDLE File, Mapping;
    volatile UCHAR *View;
    double CpuStart;

    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Mapping = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, (DWORD)Size, NULL);
    ok(Mapping != NULL, "CreateFileMappingW failed: %lu\n", GetLastError());
    if (!Mapping)
    {
        CloseHandle(File);
        return FALSE;
    }

    View = MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, Size);
    ok(View != NULL, "MapViewOfFile failed: %lu\n", GetLastError());
    if (!View)
    {
        CloseHandle(Mapping);
        CloseHandle(File);
        return FALSE;
    }

    QueryPerformanceFrequency(&Frequency);
    CpuStart = ThreadCpuSeconds();
    QueryPerformanceCounter(&Start);
    for (Offset = 0; Offset < Size; Offset += PAGE_SIZE)
        View[Offset] = (UCHAR)(Offset / PAGE_SIZE);
    QueryPerformanceCounter(&End);
    *Cpu = ThreadCpuSeconds() - CpuStart;
    *Wall = Seconds(End.QuadPart - Start.QuadPart, Frequency.QuadPart);

    UnmapViewOfFile((PVOID)View);
    CloseHandle(Mapping);
    CloseHandle(File);
    return TRUE;
}

START_TEST(SectionFaultCost)
{
    WCHAR Directory[MAX_PATH], Path[MAX_PATH];
    double Wall, Cpu, BestWall = 0, BestCpu = 0, BestIdle = -1;
    ULONG Run;

    if (!GetTempPathW(RTL_NUMBER_OF(Directory), Directory) ||
        !GetTempFileNameW(Directory, L"sfc", 0, Path))
    {
        skip("No temporary file: %lu\n", GetLastError());
        return;
    }

    for (Run = 0; Run < RUNS; Run++)
    {
        if (!TouchView(Path, &Wall, &Cpu))
            break;
        trace("run %lu: %u pages in %.3f s, %.3f s on the processor\n",
              Run, (VIEW_MB << 20) / PAGE_SIZE, Wall, Cpu);
        if (BestIdle < 0 || Wall - Cpu < BestIdle)
        {
            BestIdle = Wall - Cpu;
            BestWall = Wall;
            BestCpu = Cpu;
        }
    }
    DeleteFileW(Path);

    if (BestIdle < 0)
        return;

    /* The clock tick that charges processor time is 10 to 16 ms: allow that much, plus half */
    ok(BestIdle <= 0.05 + BestWall / 2,
       "Faulting in a %u MB view took %.3f s, but the thread ran only %.3f s of it\n",
       VIEW_MB, BestWall, BestCpu);
}
