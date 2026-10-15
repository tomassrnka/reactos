/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for kernelbase.dll: it loads under that name and has the interrupt time functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The MSYS2 and Cygwin runtimes load KernelBase.dll and call these functions at start (clock_gettime); without
 * them they stop with a fatal error, which on ReactOS turned into an endless loop in their lazy loader.
 */

#include "precomp.h"

typedef VOID (WINAPI *FN_QueryTime)(PULONGLONG);

#define MS 10000ULL     /* 100 ns units */
#define TICK (160 * MS / 10)

static HMODULE g_Module;
static FN_QueryTime pQueryInterruptTime;
static FN_QueryTime pQueryInterruptTimePrecise;
static FN_QueryTime pQueryUnbiasedInterruptTimePrecise;
static BOOL (WINAPI *pQueryUnbiasedInterruptTime)(PULONGLONG);

static void
test_Load(void)
{
    SetLastError(0xdeadbeef);
    g_Module = LoadLibraryW(L"kernelbase.dll");
    ok(g_Module != NULL, "kernelbase.dll does not load, error %lu\n", GetLastError());
    if (g_Module == NULL)
        return;

    ok(GetModuleHandleW(L"KernelBase.dll") == g_Module, "the module is not found again by name\n");

    pQueryInterruptTime = (FN_QueryTime)GetProcAddress(g_Module, "QueryInterruptTime");
    pQueryInterruptTimePrecise = (FN_QueryTime)GetProcAddress(g_Module, "QueryInterruptTimePrecise");
    pQueryUnbiasedInterruptTimePrecise = (FN_QueryTime)GetProcAddress(g_Module, "QueryUnbiasedInterruptTimePrecise");
    pQueryUnbiasedInterruptTime = (PVOID)GetProcAddress(g_Module, "QueryUnbiasedInterruptTime");

    if (is_reactos())
    {
        ok(pQueryInterruptTime != NULL, "kernelbase.dll does not export QueryInterruptTime\n");
        ok(pQueryInterruptTimePrecise != NULL, "kernelbase.dll does not export QueryInterruptTimePrecise\n");
        ok(pQueryUnbiasedInterruptTimePrecise != NULL, "kernelbase.dll does not export QueryUnbiasedInterruptTimePrecise\n");
        ok(pQueryUnbiasedInterruptTime != NULL, "kernelbase.dll does not export QueryUnbiasedInterruptTime\n");
    }
}

static void
test_Forwarder(void)
{
    HMODULE Ros;
    PVOID Wait, WaitRos;

    /* ReactOS keeps the code in kernelbase_ros.dll; kernelbase.dll forwards to it, so that one copy is loaded */
    if (!is_reactos() || g_Module == NULL)
        return;

    Ros = LoadLibraryW(L"kernelbase_ros.dll");
    ok(Ros != NULL, "kernelbase_ros.dll does not load, error %lu\n", GetLastError());
    if (Ros == NULL)
        return;

    Wait = GetProcAddress(g_Module, "WaitOnAddress");
    WaitRos = GetProcAddress(Ros, "WaitOnAddress");
    ok(Wait != NULL, "kernelbase.dll does not export WaitOnAddress\n");
    ok(Wait == WaitRos, "kernelbase.dll WaitOnAddress %p is not the one of kernelbase_ros.dll %p\n", Wait, WaitRos);
    ok(GetProcAddress(g_Module, "QueryInterruptTime") == GetProcAddress(Ros, "QueryInterruptTime"),
       "QueryInterruptTime of kernelbase.dll is not the one of kernelbase_ros.dll\n");

    /* Every export of kernelbase_ros.dll is forwarded by name, to the same code */
    {
        PBYTE Base = (PBYTE)Ros;
        PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
        PIMAGE_DATA_DIRECTORY Dir = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        PIMAGE_EXPORT_DIRECTORY Exp = (PIMAGE_EXPORT_DIRECTORY)(Base + Dir->VirtualAddress);
        PDWORD Names = (PDWORD)(Base + Exp->AddressOfNames);
        DWORD i, Missing = 0, Different = 0;

        for (i = 0; i < Exp->NumberOfNames; i++)
        {
            const char *Name = (const char *)(Base + Names[i]);
            PVOID Forwarded = GetProcAddress(g_Module, Name);

            if (Forwarded == NULL)
            {
                Missing++;
                trace("kernelbase.dll does not export %s\n", Name);
            }
            else if (Forwarded != GetProcAddress(Ros, Name))
            {
                Different++;
                trace("%s differs between kernelbase.dll and kernelbase_ros.dll\n", Name);
            }
        }
        ok(Exp->NumberOfNames > 100, "kernelbase_ros.dll has only %lu named exports\n", Exp->NumberOfNames);
        ok(Missing == 0, "%lu of %lu exports of kernelbase_ros.dll are missing in kernelbase.dll\n", Missing, Exp->NumberOfNames);
        ok(Different == 0, "%lu exports of kernelbase.dll are not the ones of kernelbase_ros.dll\n", Different);
    }
    FreeLibrary(Ros);
}

static void
test_Coarse(void)
{
    ULONGLONG First, Second, Biased;
    BOOL Ret;

    if (pQueryInterruptTime == NULL)
    {
        skip("QueryInterruptTime is not exported\n");
        return;
    }

    pQueryInterruptTime(&First);
    ok(First != 0, "interrupt time is 0\n");
    Sleep(100);
    pQueryInterruptTime(&Second);
    ok(Second >= First + 80 * MS, "interrupt time advanced by only %I64u ms in 100 ms\n", (Second - First) / MS);
    ok(Second <= First + 5000 * MS, "interrupt time advanced by %I64u ms in 100 ms\n", (Second - First) / MS);

    if (pQueryUnbiasedInterruptTime != NULL)
    {
        First = 0;
        SetLastError(0xdeadbeef);
        Ret = pQueryUnbiasedInterruptTime(&First);
        ok(Ret == TRUE, "QueryUnbiasedInterruptTime returned %d, error %lu\n", Ret, GetLastError());
        pQueryInterruptTime(&Biased);
        ok(First != 0, "unbiased interrupt time is 0\n");
        /* The bias is the time spent asleep: it can only make the biased time larger */
        ok(First <= Biased + 2 * TICK, "unbiased %I64u is above the interrupt time %I64u\n", First, Biased);
    }
}

static void
test_Precise(FN_QueryTime Precise, const char *Name)
{
    ULONGLONG Before, After, Value, Last = 0;
    int Below = 0, Above = 0, Back = 0, Same = 0, i;
    LONGLONG Low = MAXLONGLONG, High = -MAXLONGLONG;

    if (Precise == NULL || pQueryInterruptTime == NULL)
    {
        skip("%s or QueryInterruptTime is not exported\n", Name);
        return;
    }

    for (i = 0; i < 20000; i++)
    {
        pQueryInterruptTime(&Before);
        Precise(&Value);
        pQueryInterruptTime(&After);

        /* Never before the interrupt time read first, at most about one tick ahead of the one read last */
        if (Value < Before)
            Below++;
        if (Value > After + 2 * TICK)
            Above++;
        if ((LONGLONG)(Value - Before) < Low)
            Low = Value - Before;
        if ((LONGLONG)(Value - After) > High)
            High = Value - After;
        if (Value < Last)
            Back++;
        if (Value == Last)
            Same++;
        Last = Value;
    }

    ok(Below == 0, "%s: %d of 20000 values were before the interrupt time (lowest difference %I64d)\n", Name, Below, Low);
    ok(Above == 0, "%s: %d of 20000 values were more than 2 ticks ahead of the interrupt time (highest difference %I64d)\n",
       Name, Above, High);
    ok(Back == 0, "%s: the value went back %d times\n", Name, Back);
    trace("%s: lowest difference to the interrupt time before %I64d, highest to the one after %I64d, %d equal steps\n",
          Name, Low, High, Same);

    /* Between two ticks the precise time moves on */
    {
        ULONGLONG Seen[2];
        int Differ = 0;

        for (i = 0; i < 2000; i++)
        {
            Precise(&Seen[0]);
            Precise(&Seen[1]);
            if (Seen[0] != Seen[1])
                Differ++;
        }
        ok(Differ > 20, "%s: only %d of 2000 pairs of reads differ, the value looks like the tick time\n", Name, Differ);
    }
}

static BOOL
EnableTimePrivilege(void)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;
    BOOL Ret;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return FALSE;
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Ret = LookupPrivilegeValueW(NULL, L"SeSystemtimePrivilege", &Privileges.Privileges[0].Luid);
    if (Ret)
    {
        SetLastError(ERROR_SUCCESS);
        Ret = AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    }
    if (!Ret)
        trace("cannot enable the system time privilege, error %lu\n", GetLastError());
    CloseHandle(Token);
    return Ret;
}

static BOOL
ShiftSystemTime(LONGLONG Delta100ns)
{
    FILETIME FileTime;
    SYSTEMTIME SystemTime;
    ULARGE_INTEGER Value;

    GetSystemTimeAsFileTime(&FileTime);
    Value.LowPart = FileTime.dwLowDateTime;
    Value.HighPart = FileTime.dwHighDateTime;
    Value.QuadPart += Delta100ns;
    FileTime.dwLowDateTime = Value.LowPart;
    FileTime.dwHighDateTime = Value.HighPart;
    return FileTimeToSystemTime(&FileTime, &SystemTime) && SetSystemTime(&SystemTime);
}

/* The wall clock is changed between the calls: the interrupt time must neither go back nor jump */
static void
test_ClockAdjust(FN_QueryTime Precise, const char *Name)
{
    static const LONGLONG Deltas[] = { -50 * 1000 * MS, 50 * 1000 * MS, -500 * MS, 500 * MS };
    ULONGLONG A, B, C, D;
    int i;

    if (Precise == NULL)
    {
        skip("%s is not exported\n", Name);
        return;
    }
    if (!EnableTimePrivilege())
    {
        skip("no privilege to set the system time\n");
        return;
    }

    for (i = 0; i < (int)_countof(Deltas); i++)
    {
        BOOL Set, Restored;

        Precise(&A);
        Set = ShiftSystemTime(Deltas[i]);
        Precise(&B);
        Sleep(30);
        Precise(&C);
        /* The clock kept running: undoing the shift gives the right time again */
        Restored = ShiftSystemTime(-Deltas[i]);
        Precise(&D);

        if (!Set)
        {
            skip("cannot set the system time (error %lu)\n", GetLastError());
            return;
        }
        ok(Restored, "cannot restore the system time\n");
        ok(B >= A, "%s: went back by %I64d ms when the system time moved by %I64d ms\n", Name,
           (LONGLONG)(A - B) / (LONGLONG)MS, Deltas[i] / (LONGLONG)MS);
        ok(C >= B, "%s: went back by %I64d ms after the system time moved\n", Name, (LONGLONG)(B - C) / (LONGLONG)MS);
        ok(D >= C, "%s: went back by %I64d ms when the system time was restored\n", Name, (LONGLONG)(C - D) / (LONGLONG)MS);
        ok(D - A < 2000 * MS, "%s: moved by %I64u ms across a system time change of %I64d ms\n", Name,
           (D - A) / MS, Deltas[i] / (LONGLONG)MS);
    }
}

START_TEST(KernelBaseTime)
{
    test_Load();
    if (g_Module == NULL)
        return;

    test_Forwarder();
    test_Coarse();
    test_Precise(pQueryInterruptTimePrecise, "QueryInterruptTimePrecise");
    test_Precise(pQueryUnbiasedInterruptTimePrecise, "QueryUnbiasedInterruptTimePrecise");
    test_ClockAdjust(pQueryInterruptTimePrecise, "QueryInterruptTimePrecise");
    test_ClockAdjust(pQueryUnbiasedInterruptTimePrecise, "QueryUnbiasedInterruptTimePrecise");
    test_ClockAdjust(pQueryInterruptTime, "QueryInterruptTime");
}
