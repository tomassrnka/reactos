/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for psapi!EnumProcessModulesEx and kernel32!K32EnumProcessModulesEx (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows Server 2008 R2 and Windows 10 22H2 (a 64-bit process, and a 32-bit one
 * under WOW64).
 */

#include <wine/test.h>
#include <windef.h>
#include <winbase.h>
#include <psapi.h>

#define PENDING_ERROR 0xdeadbeef
#define MAX_TEST_MODULES 256

typedef BOOL (WINAPI *PFN_EnumProcessModulesEx)(HANDLE, HMODULE *, DWORD, LPDWORD, DWORD);

static PFN_EnumProcessModulesEx pEnumProcessModulesEx;
static HMODULE Baseline[MAX_TEST_MODULES];
static DWORD BaselineCount;

/* The list of a call is the baseline of EnumProcessModules entry by entry; ExpectedError is what the call leaves */
static void CheckList(const char *Name, PFN_EnumProcessModulesEx Function, DWORD Filter, DWORD ExpectedError, int Line)
{
    HMODULE Modules[MAX_TEST_MODULES];
    DWORD Needed = 0xdead, i;
    BOOL Result;

    memset(Modules, 0x55, sizeof(Modules));
    SetLastError(PENDING_ERROR);
    Result = Function(GetCurrentProcess(), Modules, sizeof(Modules), &Needed, Filter);
    ok(Result, "%s filter %#lx (line %d): failed with %lu\n", Name, Filter, Line, GetLastError());
    ok(GetLastError() == ExpectedError, "%s filter %#lx (line %d): last error %lu, expected %lu\n", Name, Filter, Line,
       GetLastError(), ExpectedError);
    ok(Needed == BaselineCount * sizeof(HMODULE), "%s filter %#lx (line %d): %lu bytes, expected %lu\n", Name, Filter,
       Line, Needed, BaselineCount * (DWORD)sizeof(HMODULE));
    if (Needed == BaselineCount * sizeof(HMODULE))
    {
        for (i = 0; i < BaselineCount; i++)
            ok(Modules[i] == Baseline[i], "%s filter %#lx (line %d): module %lu is %p, expected %p\n", Name, Filter,
               Line, i, Modules[i], Baseline[i]);
    }
}

#ifdef _WIN64
#define WITH_32BIT_ERROR ERROR_PARTIAL_COPY
#else
#define WITH_32BIT_ERROR PENDING_ERROR
#endif

static void TestFunction(const char *Name, PFN_EnumProcessModulesEx Function)
{
    HMODULE Modules[MAX_TEST_MODULES];
    DWORD Needed, i;
    BOOL Result;

    /* the list of the process is the one of EnumProcessModules */
    CheckList(Name, Function, LIST_MODULES_DEFAULT, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, LIST_MODULES_64BIT, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, LIST_MODULES_ALL, WITH_32BIT_ERROR, __LINE__);
#ifdef _WIN64
    /* only the low bits choose; the other bits of an accepted filter do not */
    CheckList(Name, Function, 0x6, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, 0x7, WITH_32BIT_ERROR, __LINE__);
    CheckList(Name, Function, 0xffffffff, WITH_32BIT_ERROR, __LINE__);

    /* a native 64-bit process has no 32-bit modules: asking for only them gives an empty list and a last error */
    for (i = 0; i < 3; i++)
    {
        static const DWORD Only32[] = { LIST_MODULES_32BIT, 0x5, 0xfffffffd };

        Needed = 0xdead;
        SetLastError(PENDING_ERROR);
        Result = Function(GetCurrentProcess(), Modules, sizeof(Modules), &Needed, Only32[i]);
        ok(Result, "filter %#lx: failed with %lu\n", Only32[i], GetLastError());
        ok_dec(Needed, 0);
        ok_dec(GetLastError(), ERROR_PARTIAL_COPY);
    }

    /* the filter needs one of the two low bits, or none at all */
    {
        static const DWORD Bad[] = { 4, 0x10, 0x8, 0x80000000 };

        for (i = 0; i < RTL_NUMBER_OF(Bad); i++)
        {
            Needed = 0xdead;
            SetLastError(PENDING_ERROR);
            Result = Function(GetCurrentProcess(), Modules, sizeof(Modules), &Needed, Bad[i]);
            ok(!Result, "a filter of %#lx is accepted\n", Bad[i]);
            ok_dec(GetLastError(), ERROR_INVALID_PARAMETER);
            ok_dec(Needed, 0xdead);
        }
    }
#else
    /* a 32-bit process, measured under WOW64, lists its modules for every filter and refuses none */
    CheckList(Name, Function, LIST_MODULES_32BIT, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, 4, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, 0x10, PENDING_ERROR, __LINE__);
    CheckList(Name, Function, 0xffffffff, PENDING_ERROR, __LINE__);
#endif

    /* the size of the list is returned when the buffer is missing or small */
    Needed = 0;
    SetLastError(PENDING_ERROR);
    Result = Function(GetCurrentProcess(), NULL, 0, &Needed, LIST_MODULES_DEFAULT);
    ok(Result, "failed with %lu\n", GetLastError());
    ok_dec(Needed, BaselineCount * sizeof(HMODULE));
    Needed = 0;
    Result = Function(GetCurrentProcess(), Modules, sizeof(HMODULE), &Needed, LIST_MODULES_DEFAULT);
    ok(Result, "failed with %lu\n", GetLastError());
    ok_dec(Needed, BaselineCount * sizeof(HMODULE));

    /* the handle of the current process can also be the pseudo-handle */
    Needed = 0;
    Result = Function((HANDLE)(LONG_PTR)-1, Modules, sizeof(Modules), &Needed, LIST_MODULES_DEFAULT);
    ok(Result, "failed with %lu\n", GetLastError());
    ok_dec(Needed, BaselineCount * sizeof(HMODULE));

    /* bad arguments */
    SetLastError(PENDING_ERROR);
    Result = Function(GetCurrentProcess(), Modules, sizeof(Modules), NULL, LIST_MODULES_DEFAULT);
    ok(!Result, "a missing size is accepted\n");
    ok_dec(GetLastError(), ERROR_NOACCESS);
    SetLastError(PENDING_ERROR);
    Result = Function(NULL, Modules, sizeof(Modules), &Needed, LIST_MODULES_DEFAULT);
    ok(!Result, "a null handle is accepted\n");
    ok_dec(GetLastError(), ERROR_INVALID_HANDLE);
}

#ifdef _WIN64
typedef struct
{
    HMODULE Default[MAX_TEST_MODULES], Only32[MAX_TEST_MODULES], Only64[MAX_TEST_MODULES], Both[MAX_TEST_MODULES];
    DWORD NeededDefault, Needed32, Needed64, NeededBoth;
    BOOL Ok;
} WOW64_LISTS;

/* All four lists, one after the other; a list that is not complete or does not fit makes the snapshot a failure */
static void TakeSnapshot(PFN_EnumProcessModulesEx Function, HANDLE Process, WOW64_LISTS *Lists)
{
    Lists->Ok = Function(Process, Lists->Default, sizeof(Lists->Default), &Lists->NeededDefault, LIST_MODULES_DEFAULT) &&
                Function(Process, Lists->Only32, sizeof(Lists->Only32), &Lists->Needed32, LIST_MODULES_32BIT) &&
                Function(Process, Lists->Only64, sizeof(Lists->Only64), &Lists->Needed64, LIST_MODULES_64BIT) &&
                Function(Process, Lists->Both, sizeof(Lists->Both), &Lists->NeededBoth, LIST_MODULES_ALL);
    if (Lists->Ok)
    {
        Lists->Ok = Lists->NeededDefault <= sizeof(Lists->Default) && Lists->Needed32 <= sizeof(Lists->Only32) &&
                    Lists->Needed64 <= sizeof(Lists->Only64) && Lists->NeededBoth <= sizeof(Lists->Both) &&
                    !(Lists->NeededDefault % sizeof(HMODULE)) && !(Lists->Needed32 % sizeof(HMODULE)) &&
                    !(Lists->Needed64 % sizeof(HMODULE)) && !(Lists->NeededBoth % sizeof(HMODULE));
    }
}

/* Two snapshots are the same when no library came or went between them */
static BOOL SameSnapshot(const WOW64_LISTS *First, const WOW64_LISTS *Second)
{
    return First->NeededDefault == Second->NeededDefault && First->Needed32 == Second->Needed32 &&
           First->Needed64 == Second->Needed64 && First->NeededBoth == Second->NeededBoth &&
           !memcmp(First->Only32, Second->Only32, First->Needed32) && !memcmp(First->Only64, Second->Only64, First->Needed64);
}

/* The lists of a 32-bit process, from a 64-bit caller: the 32-bit filter, the 64-bit one without the executable, both */
static void TestWow64Target(const char *Name, PFN_EnumProcessModulesEx Function)
{
    char Directory[MAX_PATH], Command[MAX_PATH + 96];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    static WOW64_LISTS First, Second;
    HMODULE Native[MAX_TEST_MODULES];
    DWORD NativeNeeded = 0, Tries, i;
    BOOL Wow64 = FALSE, Stable = FALSE;

    if (!GetSystemWow64DirectoryA(Directory, sizeof(Directory)))
    {
        skip("no 32-bit programs here\n");
        return;
    }
    lstrcpyA(Command, "\"");
    lstrcatA(Command, Directory);
    lstrcatA(Command, "\\cmd.exe\" /c ping -n 12 127.0.0.1 > nul");
    if (!CreateProcessA(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("the 32-bit process did not start (%lu)\n", GetLastError());
        return;
    }

    /* it is ready when the native list, which EnumProcessModules gives, has the loader of WOW64 in it */
    for (Tries = 0; Tries < 50; Tries++)
    {
        NativeNeeded = 0;
        if (EnumProcessModules(ProcessInfo.hProcess, Native, sizeof(Native), &NativeNeeded) && NativeNeeded / sizeof(HMODULE) >= 5)
            break;
        Sleep(100);
    }

    IsWow64Process(ProcessInfo.hProcess, &Wow64);
    if (!Wow64 || NativeNeeded / sizeof(HMODULE) < 5)
    {
        skip("the process is not a 32-bit one or did not start in time\n");
    }
    else
    {
        /* the libraries of the process load for a while: the lists are taken again until two of them agree */
        Sleep(500);
        TakeSnapshot(Function, ProcessInfo.hProcess, &First);
        for (Tries = 0; Tries < 20 && First.Ok; Tries++)
        {
            Sleep(100);
            TakeSnapshot(Function, ProcessInfo.hProcess, &Second);
            if (!Second.Ok)
            {
                First.Ok = FALSE;
                break;
            }
            if (SameSnapshot(&First, &Second))
            {
                Stable = TRUE;
                break;
            }
            memcpy(&First, &Second, sizeof(First));
        }

        ok(First.Ok, "%s: a list of the 32-bit process was not given (last error %lu)\n", Name, GetLastError());
        ok(!First.Ok || Stable, "%s: the lists of the process did not settle\n", Name);
        if (First.Ok && Stable)
        {
            /* the 32-bit list starts with the executable, which is not in the 64-bit list; the other 32-bit modules are below 4 GB */
            ok(First.Needed32 / sizeof(HMODULE) > 3, "%s: %lu 32-bit modules\n", Name, First.Needed32 / (DWORD)sizeof(HMODULE));
            ok(First.Needed64 > 0, "%s: no 64-bit modules\n", Name);
            for (i = 0; i < First.Needed32 / sizeof(HMODULE); i++)
                ok((ULONG_PTR)First.Only32[i] < 0x100000000ULL, "%s: 32-bit module %lu is %p\n", Name, i, First.Only32[i]);
            ok(First.NeededDefault == First.Needed64 + sizeof(HMODULE), "%s: the default list has %lu bytes, the 64-bit one %lu\n",
               Name, First.NeededDefault, First.Needed64);
            if (First.NeededDefault == First.Needed64 + sizeof(HMODULE))
            {
                ok(First.Default[0] == First.Only32[0], "%s: the default list starts with %p, not the executable %p\n", Name,
                   First.Default[0], First.Only32[0]);
                ok(!memcmp(First.Default + 1, First.Only64, First.Needed64), "%s: the 64-bit list is not the default one without the executable\n", Name);
            }
            ok(First.NeededBoth == First.Needed32 + First.Needed64, "%s: both lists have %lu bytes, expected %lu\n", Name,
               First.NeededBoth, First.Needed32 + First.Needed64);
            if (First.NeededBoth == First.Needed32 + First.Needed64)
            {
                ok(!memcmp(First.Both, First.Only32, First.Needed32), "%s: both lists do not start with the 32-bit one\n", Name);
                ok(!memcmp(First.Both + First.Needed32 / sizeof(HMODULE), First.Only64, First.Needed64),
                   "%s: both lists do not end with the 64-bit one\n", Name);
            }
        }
    }

    TerminateProcess(ProcessInfo.hProcess, 0);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}
#endif

START_TEST(EnumProcessModulesEx)
{
    HMODULE Psapi = LoadLibraryW(L"psapi.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_EnumProcessModulesEx pK32EnumProcessModulesEx;
    /* ReactOS has them from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    BOOL Expected = is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules");
    DWORD Needed = 0;

    pEnumProcessModulesEx = (PFN_EnumProcessModulesEx)GetProcAddress(Psapi, "EnumProcessModulesEx");
    pK32EnumProcessModulesEx = (PFN_EnumProcessModulesEx)GetProcAddress(Kernel32, "K32EnumProcessModulesEx");
    /* Windows XP and Windows Server 2003 have no such function; Windows Vista has the psapi one only */
    if (!pEnumProcessModulesEx)
    {
        ok(!Expected, "EnumProcessModulesEx is not exported\n");
        skip("EnumProcessModulesEx is not exported\n");
        return;
    }

    ok(EnumProcessModules(GetCurrentProcess(), Baseline, sizeof(Baseline), &Needed), "EnumProcessModules failed\n");
    BaselineCount = Needed / sizeof(HMODULE);
    ok(BaselineCount > 2 && BaselineCount <= MAX_TEST_MODULES, "%lu modules\n", BaselineCount);
    if (BaselineCount <= 2 || BaselineCount > MAX_TEST_MODULES)
        return;

    TestFunction("psapi", pEnumProcessModulesEx);

    /* kernel32 has the same function under another name, from Windows 7 on */
    if (!pK32EnumProcessModulesEx)
    {
        ok(!Expected, "K32EnumProcessModulesEx is not exported\n");
        skip("K32EnumProcessModulesEx is not exported\n");
#ifdef _WIN64
        TestWow64Target("psapi", pEnumProcessModulesEx);
#endif
        return;
    }
    TestFunction("kernel32", pK32EnumProcessModulesEx);
#ifdef _WIN64
    /* last: starting a process loads libraries here, which the lists of this process were taken before */
    TestWow64Target("psapi", pEnumProcessModulesEx);
    TestWow64Target("kernel32", pK32EnumProcessModulesEx);
#endif
}
