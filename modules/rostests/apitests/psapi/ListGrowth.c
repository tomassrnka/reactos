/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the lists of a 32-bit process that change while psapi reads them (a 64-bit caller)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The code of psapi is compiled into this test, with the reading of the target replaced by a script, so that a list
 * grows between the call that counts and the call that fills, which a real process does not do on demand. The
 * functions of psapi keep their own names in the DLL: here they have another one, so that the calls of the other
 * test of this program go to the DLL.
 */

#ifdef _WIN64
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

#define EmptyWorkingSet Copy_EmptyWorkingSet
#define EnumDeviceDrivers Copy_EnumDeviceDrivers
#define EnumPageFilesA Copy_EnumPageFilesA
#define EnumPageFilesW Copy_EnumPageFilesW
#define EnumProcesses Copy_EnumProcesses
#define EnumProcessModules Copy_EnumProcessModules
#define EnumProcessModulesEx Copy_EnumProcessModulesEx
#define GetDeviceDriverBaseNameA Copy_GetDeviceDriverBaseNameA
#define GetDeviceDriverBaseNameW Copy_GetDeviceDriverBaseNameW
#define GetDeviceDriverFileNameA Copy_GetDeviceDriverFileNameA
#define GetDeviceDriverFileNameW Copy_GetDeviceDriverFileNameW
#define GetMappedFileNameA Copy_GetMappedFileNameA
#define GetMappedFileNameW Copy_GetMappedFileNameW
#define GetModuleBaseNameA Copy_GetModuleBaseNameA
#define GetModuleBaseNameW Copy_GetModuleBaseNameW
#define GetModuleFileNameExA Copy_GetModuleFileNameExA
#define GetModuleFileNameExW Copy_GetModuleFileNameExW
#define GetModuleInformation Copy_GetModuleInformation
#define GetPerformanceInfo Copy_GetPerformanceInfo
#define GetProcessImageFileNameA Copy_GetProcessImageFileNameA
#define GetProcessImageFileNameW Copy_GetProcessImageFileNameW
#define GetProcessMemoryInfo Copy_GetProcessMemoryInfo
#define GetWsChanges Copy_GetWsChanges
#define InitializeProcessForWsWatch Copy_InitializeProcessForWsWatch
#define QueryWorkingSet Copy_QueryWorkingSet
#define QueryWorkingSetEx Copy_QueryWorkingSetEx

static BOOL WINAPI ScriptedEnumNative(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded);
static BOOL WINAPI ScriptedReadMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesRead);

#define PSAPI_ENUM_NATIVE ScriptedEnumNative
#define ReadProcessMemory ScriptedReadMemory

#include "../../../../dll/win32/psapi/psapi.c"

#undef ReadProcessMemory
#include <wine/test.h>

#define NATIVE_BASE(i) ((HMODULE)(ULONG_PTR)(0x7ff000000000ULL + (i) * 0x10000))
#define WOW_BASE(i)    ((ULONG)(0x00400000 + (i) * 0x10000))
#define PEB32          0x1000
#define LDR32          0x2000
#define ENTRY32(i)     (0x3000 + (i) * 0x40)

/* how the lists of the pretend process look at each call: the number of modules */
static ULONG NativeCounts[24], NativeCalls, NativeStep;
static ULONG WowCounts[24], WowPasses, WowStep;

static ULONG CurrentNative(VOID)
{
    return NativeCounts[NativeStep < RTL_NUMBER_OF(NativeCounts) ? NativeStep : RTL_NUMBER_OF(NativeCounts) - 1];
}

/* like EnumProcessModules: fills what fits, tells how much the whole list takes */
static BOOL WINAPI ScriptedEnumNative(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded)
{
    ULONG Count = CurrentNative(), i;

    NativeCalls++;
    for (i = 0; i < Count && i < cb / sizeof(HMODULE); i++)
        lphModule[i] = NATIVE_BASE(i);
    *lpcbNeeded = Count * sizeof(HMODULE);
    /* the list changes after each call that is made with a buffer, and after the call that counts */
    NativeStep++;
    return TRUE;
}

static ULONG CurrentWow(VOID)
{
    return WowCounts[WowStep < RTL_NUMBER_OF(WowCounts) ? WowStep : RTL_NUMBER_OF(WowCounts) - 1];
}

/* the 32-bit loader data: the PEB points to the data, whose load order list runs through the entries */
static BOOL WINAPI ScriptedReadMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesRead)
{
    ULONG_PTR Address = (ULONG_PTR)lpBaseAddress;
    ULONG Value = 0, Count, i;

    if (nSize != sizeof(ULONG))
        return FALSE;

    if (Address == PEB32 + 0x0C)
        Value = LDR32;
    else if (Address == LDR32 + 0x0C)
    {
        /* every pass over the list starts here: it sees the list as it is at that time */
        WowPasses++;
        WowStep = WowPasses - 1;
        Count = CurrentWow();
        Value = Count ? ENTRY32(0) : LDR32 + 0x0C;
    }
    else
    {
        Count = CurrentWow();
        for (i = 0; i < Count; i++)
        {
            if (Address == ENTRY32(i))
                Value = i + 1 < Count ? ENTRY32(i + 1) : LDR32 + 0x0C;
            else if (Address == ENTRY32(i) + 0x18)
                Value = WOW_BASE(i);
            else
                continue;
            break;
        }
        if (i == Count)
            return FALSE;
    }
    *(ULONG *)lpBuffer = Value;
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = nSize;
    return TRUE;
}

static void ResetScript(const ULONG *Native, ULONG NativeN, const ULONG *Wow, ULONG WowN)
{
    ULONG i;

    memset(NativeCounts, 0, sizeof(NativeCounts));
    memset(WowCounts, 0, sizeof(WowCounts));
    for (i = 0; i < RTL_NUMBER_OF(NativeCounts); i++)
        NativeCounts[i] = i < NativeN ? Native[i] : Native[NativeN - 1];
    for (i = 0; i < RTL_NUMBER_OF(WowCounts); i++)
        WowCounts[i] = i < WowN ? Wow[i] : Wow[WowN - 1];
    NativeCalls = NativeStep = 0;
    WowPasses = WowStep = 0;
}

START_TEST(ListGrowth)
{
    /* the native list has 2 modules when it is counted and 30 when it is read, the 32-bit list 3 and then 40: more than
       the spare room of the first buffer */
    static const ULONG Native[] = { 2, 30 }, Wow[] = { 3, 40 };
    HMODULE Modules[128];
    DWORD Needed, i;
    BOOL Result;

    /* both lists, the 32-bit one first, then the native one without its executable */
    ResetScript(Native, 2, Wow, 2);
    memset(Modules, 0, sizeof(Modules));
    Needed = 0;
    SetLastError(0xdeadbeef);
    Result = EnumWow64Modules(GetCurrentProcess(), PEB32, Modules, sizeof(Modules), &Needed, LIST_MODULES_ALL);
    ok(Result, "failed with %lu\n", GetLastError());
    ok(Needed == (40 + 30 - 1) * sizeof(HMODULE), "%lu bytes, expected %lu\n", Needed, (40 + 30 - 1) * (DWORD)sizeof(HMODULE));
    if (Needed == (40 + 30 - 1) * sizeof(HMODULE))
    {
        for (i = 0; i < 40; i++)
            ok(Modules[i] == (HMODULE)(ULONG_PTR)WOW_BASE(i), "32-bit module %lu is %p\n", i, Modules[i]);
        for (i = 0; i < 29; i++)
            ok(Modules[40 + i] == NATIVE_BASE(i + 1), "native module %lu is %p\n", i + 1, Modules[40 + i]);
    }

    /* the native list alone, with the executable and without it */
    ResetScript(Native, 2, Wow, 1);
    memset(Modules, 0, sizeof(Modules));
    Result = EnumWow64Modules(GetCurrentProcess(), PEB32, Modules, sizeof(Modules), &Needed, LIST_MODULES_DEFAULT);
    ok(Result, "default: failed with %lu\n", GetLastError());
    ok(Needed == 30 * sizeof(HMODULE), "default: %lu bytes\n", Needed);
    for (i = 0; Result && i < 30 && Needed == 30 * sizeof(HMODULE); i++)
        ok(Modules[i] == NATIVE_BASE(i), "default: module %lu is %p\n", i, Modules[i]);

    ResetScript(Native, 2, Wow, 1);
    memset(Modules, 0, sizeof(Modules));
    Result = EnumWow64Modules(GetCurrentProcess(), PEB32, Modules, sizeof(Modules), &Needed, LIST_MODULES_64BIT);
    ok(Result, "64-bit: failed with %lu\n", GetLastError());
    ok(Needed == 29 * sizeof(HMODULE), "64-bit: %lu bytes\n", Needed);
    for (i = 0; Result && i < 29 && Needed == 29 * sizeof(HMODULE); i++)
        ok(Modules[i] == NATIVE_BASE(i + 1), "64-bit: module %lu is %p\n", i, Modules[i]);

    /* a list that keeps growing is an error, not a read past the end */
    {
        static const ULONG Always[] = { 1, 101, 201, 301, 401, 501, 601, 701, 801, 901, 1001, 1101 };

        ResetScript(Always, 12, Wow, 1);
        SetLastError(0xdeadbeef);
        Result = EnumWow64Modules(GetCurrentProcess(), PEB32, Modules, sizeof(Modules), &Needed, LIST_MODULES_DEFAULT);
        ok(!Result, "a list that grows with every read was given\n");
        ok(GetLastError() == ERROR_PARTIAL_COPY, "last error %lu\n", GetLastError());
    }

    /* the 32-bit list grows with every pass: the same */
    {
        static const ULONG Grow[] = { 1, 101, 201, 301, 401, 501, 601, 701, 801, 901, 1001, 1101, 1201, 1301, 1401, 1501, 1601, 1701, 1801 };

        ResetScript(Native, 1, Grow, 19);
        SetLastError(0xdeadbeef);
        Result = EnumWow64Modules(GetCurrentProcess(), PEB32, Modules, sizeof(Modules), &Needed, LIST_MODULES_32BIT);
        ok(!Result, "a 32-bit list that grows with every read was given\n");
        ok(GetLastError() == ERROR_PARTIAL_COPY, "last error %lu\n", GetLastError());
    }
}
#else
#include <wine/test.h>

START_TEST(ListGrowth)
{
    skip("the list of a 32-bit process is read by 64-bit callers only\n");
}
#endif
