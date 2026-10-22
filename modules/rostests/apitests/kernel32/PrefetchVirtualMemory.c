/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for PrefetchVirtualMemory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef struct _TEST_RANGE
{
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
} TEST_RANGE;

typedef BOOL (WINAPI *FN_PrefetchVirtualMemory)(HANDLE, ULONG_PTR, TEST_RANGE *, ULONG);

static FN_PrefetchVirtualMemory pPrefetchVirtualMemory;

static void
Expect(const char *Description, HANDLE Process, ULONG_PTR Count, TEST_RANGE *Ranges, ULONG Flags, BOOL Result, DWORD Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pPrefetchVirtualMemory(Process, Count, Ranges, Flags);
    ok(Ret == Result, "%s: returned %d, expected %d\n", Description, Ret, Result);
    if (!Result)
        ok(GetLastError() == Error, "%s: error %lu, expected %lu\n", Description, GetLastError(), Error);
    else
        ok(GetLastError() == 0xdeadbeef, "%s: error changed to %lu\n", Description, GetLastError());
}

static BOOL
IsWow64(VOID)
{
    typedef BOOL (WINAPI *FN_IsWow64Process)(HANDLE, PBOOL);
    FN_IsWow64Process pIsWow64Process = (FN_IsWow64Process)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    BOOL Wow64 = FALSE;

    return pIsWow64Process && pIsWow64Process(GetCurrentProcess(), &Wow64) && Wow64;
}

START_TEST(PrefetchVirtualMemory)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    TEST_RANGE Ranges[3];
    PVOID Committed, Reserved;
    HANDLE Handle;
    SYSTEM_INFO SystemInfo;
    DWORD OldProtect;

    pPrefetchVirtualMemory = (FN_PrefetchVirtualMemory)GetProcAddress(hKernel32, "PrefetchVirtualMemory");
    if (!pPrefetchVirtualMemory && !is_reactos())
    {
        skip("PrefetchVirtualMemory is not exported\n");
        return;
    }
    ok(pPrefetchVirtualMemory != NULL, "PrefetchVirtualMemory is not exported\n");
    if (!pPrefetchVirtualMemory)
        return;

    GetSystemInfo(&SystemInfo);
    Committed = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Reserved = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ok(Committed != NULL && Reserved != NULL, "VirtualAlloc failed with %lu\n", GetLastError());
    if (!Committed || !Reserved)
        return;

    /* The flags must be zero */
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0x10000;
    Expect("committed, flags 0", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Expect("flags 1", GetCurrentProcess(), 1, Ranges, 1, FALSE, ERROR_INVALID_PARAMETER);
    Expect("flags 2", GetCurrentProcess(), 1, Ranges, 2, FALSE, ERROR_INVALID_PARAMETER);
    Expect("flags 0x80000000", GetCurrentProcess(), 1, Ranges, 0x80000000, FALSE, ERROR_INVALID_PARAMETER);

    /* The number of entries */
    Expect("no entries", GetCurrentProcess(), 0, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
    Expect("no entries, no array", GetCurrentProcess(), 0, NULL, 0, FALSE, ERROR_INVALID_PARAMETER);
    Expect("no array", GetCurrentProcess(), 1, NULL, 0, FALSE, ERROR_NOACCESS);

    /* The process handle */
    Expect("NULL process", NULL, 1, Ranges, 0, FALSE, ERROR_INVALID_HANDLE);
    Expect("invalid process", (HANDLE)(ULONG_PTR)0xdeadbee0, 1, Ranges, 0, FALSE, ERROR_INVALID_HANDLE);
    Expect("thread handle", GetCurrentThread(), 1, Ranges, 0, FALSE, ERROR_INVALID_HANDLE);

    Handle = OpenProcess(PROCESS_VM_OPERATION, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        Expect("handle with PROCESS_VM_OPERATION", Handle, 1, Ranges, 0, TRUE, 0);
        CloseHandle(Handle);
    }

    Handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        Expect("handle without PROCESS_VM_OPERATION", Handle, 1, Ranges, 0, FALSE, ERROR_ACCESS_DENIED);
        CloseHandle(Handle);
    }

    /* The order of the checks: no entries, the handle (its type, then its rights), the array, the flags, the ranges */
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0x1000;
    Ranges[1].VirtualAddress = Committed;
    Ranges[1].NumberOfBytes = 0;
    Expect("invalid handle, flags 1", (HANDLE)(ULONG_PTR)0xdeadbee0, 1, Ranges, 1, FALSE, ERROR_INVALID_HANDLE);
    Expect("invalid handle, no entries", (HANDLE)(ULONG_PTR)0xdeadbee0, 0, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
#ifdef _WIN64
    /* 32-bit Windows programs on 64-bit Windows read the array before they look at the handle */
    Expect("invalid handle, no array", (HANDLE)(ULONG_PTR)0xdeadbee0, 1, NULL, 0, FALSE, ERROR_INVALID_HANDLE);
#endif
    Expect("invalid handle, an empty range", (HANDLE)(ULONG_PTR)0xdeadbee0, 2, Ranges, 0, FALSE, ERROR_INVALID_HANDLE);
    Handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        Expect("handle without rights, flags 1", Handle, 1, Ranges, 1, FALSE, ERROR_ACCESS_DENIED);
        Expect("handle without rights, no entries", Handle, 0, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
#ifdef _WIN64
        Expect("handle without rights, no array", Handle, 1, NULL, 0, FALSE, ERROR_ACCESS_DENIED);
#endif
        Expect("handle without rights, an empty range", Handle, 2, Ranges, 0, FALSE, ERROR_ACCESS_DENIED);
        CloseHandle(Handle);
    }
    Expect("flags 1, an empty range", GetCurrentProcess(), 2, Ranges, 1, FALSE, ERROR_INVALID_PARAMETER);
    Expect("flags 1, no array", GetCurrentProcess(), 1, NULL, 1, FALSE, ERROR_NOACCESS);
    Expect("no entries, flags 1", GetCurrentProcess(), 0, Ranges, 1, FALSE, ERROR_INVALID_PARAMETER);
    Ranges[0].NumberOfBytes = 0;
    Ranges[1].NumberOfBytes = 0x1000;
    Expect("the first range empty", GetCurrentProcess(), 2, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
    {
        /* An array that ends before the page that cannot be read: every entry is read before one is judged */
        PBYTE Page = VirtualAlloc(NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        TEST_RANGE *Array;

        ok(Page != NULL, "VirtualAlloc failed with %lu\n", GetLastError());
        if (Page)
        {
            VirtualProtect(Page + 0x1000, 0x1000, PAGE_NOACCESS, &OldProtect);
            Array = (TEST_RANGE *)(Page + 0x1000 - 2 * sizeof(TEST_RANGE));
            Array[0].VirtualAddress = Committed;
            Array[0].NumberOfBytes = 0x1000;
            Array[1] = Array[0];
            Expect("an array at the end of a page", GetCurrentProcess(), 2, Array, 0, TRUE, 0);
            Expect("a third entry on the page that cannot be read", GetCurrentProcess(), 3, Array, 0, FALSE, ERROR_NOACCESS);
            Array[0].NumberOfBytes = 0;
            Expect("an empty range and an unreadable entry", GetCurrentProcess(), 3, Array, 0, FALSE, ERROR_NOACCESS);
            VirtualFree(Page, 0, MEM_RELEASE);
        }
    }
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0x10000;

    /* The ranges are a hint: they need not be committed, mapped or even allocated */
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0;
    Expect("size 0", GetCurrentProcess(), 1, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
    Ranges[0].NumberOfBytes = 1;
    Expect("size 1", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = (PBYTE)Committed + 1;
    Ranges[0].NumberOfBytes = 100;
    Expect("unaligned", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = Reserved;
    Ranges[0].NumberOfBytes = 0x10000;
    Expect("reserved only", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = (PVOID)(ULONG_PTR)0x10000;
    Ranges[0].NumberOfBytes = 0x1000;
    Expect("not allocated", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = NULL;
    Ranges[0].NumberOfBytes = 0x1000;
    Expect("NULL address", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = (PBYTE)Committed + 0xF000;
    Ranges[0].NumberOfBytes = 0x20000;
    Expect("past the end of the allocation", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    VirtualProtect(Committed, 0x1000, PAGE_NOACCESS, &OldProtect);
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0x1000;
    Expect("no access", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    VirtualProtect(Committed, 0x1000, PAGE_READWRITE, &OldProtect);

    /* Several entries: all of them are checked */
    Ranges[0].VirtualAddress = Committed;
    Ranges[0].NumberOfBytes = 0x1000;
    Ranges[1].VirtualAddress = Reserved;
    Ranges[1].NumberOfBytes = 0x1000;
    Ranges[2].VirtualAddress = (PBYTE)Committed + 0x2000;
    Ranges[2].NumberOfBytes = 0x1000;
    Expect("three entries", GetCurrentProcess(), 3, Ranges, 0, TRUE, 0);
    Ranges[1].NumberOfBytes = 0;
    Expect("three entries, the second empty", GetCurrentProcess(), 3, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);

    /* An image */
    Ranges[0].VirtualAddress = GetModuleHandleW(NULL);
    Ranges[0].NumberOfBytes = 0x1000;
    Expect("own image", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
    Ranges[0].VirtualAddress = hKernel32;
    Ranges[0].NumberOfBytes = 0x10000;
    Expect("kernel32 image", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);

    /* A range that wraps around or ends beyond the address space; a 32-bit program on 64-bit Windows has more space */
    if (!IsWow64())
    {
        Ranges[0].VirtualAddress = Committed;
        Ranges[0].NumberOfBytes = (SIZE_T)-1;
        Expect("size -1", GetCurrentProcess(), 1, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
        Ranges[0].VirtualAddress = (PVOID)(ULONG_PTR)-0x1000;
        Ranges[0].NumberOfBytes = 0x2000;
        Expect("wraps around", GetCurrentProcess(), 1, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
        Ranges[0].VirtualAddress = (PVOID)((ULONG_PTR)SystemInfo.lpMaximumApplicationAddress + 1 - 0x1000);
        Ranges[0].NumberOfBytes = 0x1000;
        Expect("last page of user space", GetCurrentProcess(), 1, Ranges, 0, TRUE, 0);
        Ranges[0].NumberOfBytes = 0x1001;
        Expect("one byte too many", GetCurrentProcess(), 1, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
        Ranges[0].VirtualAddress = (PVOID)((ULONG_PTR)SystemInfo.lpMaximumApplicationAddress + 1);
        Ranges[0].NumberOfBytes = 1;
        Expect("above user space", GetCurrentProcess(), 1, Ranges, 0, FALSE, ERROR_INVALID_PARAMETER);
    }

    VirtualFree(Committed, 0, MEM_RELEASE);
    VirtualFree(Reserved, 0, MEM_RELEASE);
}
