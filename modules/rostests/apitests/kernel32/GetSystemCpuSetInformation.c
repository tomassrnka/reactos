/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for GetSystemCpuSetInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are those of Windows 10 22H2 (build 19045) on a machine with 4 processors in one group.
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_GetSystemCpuSetInformation)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);

static FN_GetSystemCpuSetInformation pGetSystemCpuSetInformation;

#define ENTRY_SIZE 32

static
ULONG
CountBits(
    _In_ ULONG_PTR Mask)
{
    ULONG Count = 0;

    for (; Mask; Mask &= Mask - 1)
        Count++;
    return Count;
}

static
VOID
TestFails(
    _In_ PCSTR Name,
    _In_opt_ PVOID Buffer,
    _In_ ULONG BufferLength,
    _In_opt_ HANDLE Process,
    _In_ ULONG Flags,
    _In_ DWORD ExpectedError,
    _In_ ULONG ExpectedLength)
{
    ULONG Length = 0xdeadbeef;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetSystemCpuSetInformation(Buffer, BufferLength, &Length, Process, Flags);
    ok(Ret == FALSE, "%s: Ret = %d\n", Name, Ret);
    ok(GetLastError() == ExpectedError, "%s: GetLastError() = %lu, expected %lu\n", Name, GetLastError(), ExpectedError);
    ok(Length == ExpectedLength, "%s: Length = %lu, expected %lu\n", Name, Length, ExpectedLength);
}

static
VOID
TestSucceeds(
    _In_ PCSTR Name,
    _In_opt_ HANDLE Process,
    _In_ ULONG ExpectedLength)
{
    BYTE Buffer[ENTRY_SIZE * 128];
    ULONG Length = 0xdeadbeef;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)Buffer, sizeof(Buffer), &Length, Process, 0);
    ok(Ret == TRUE, "%s: Ret = %d, error %lu\n", Name, Ret, GetLastError());
    ok(GetLastError() == 0xdeadbeef, "%s: GetLastError() = %lu\n", Name, GetLastError());
    ok(Length == ExpectedLength, "%s: Length = %lu, expected %lu\n", Name, Length, ExpectedLength);
}

START_TEST(GetSystemCpuSetInformation)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    SYSTEM_INFO SystemInfo;
    BYTE Buffer[ENTRY_SIZE * 128];
    PSYSTEM_CPU_SET_INFORMATION Entry;
    ULONG Count, Length, Index, Offset;
    ULONG_PTR Seen;
    HANDLE Handle;
    BOOL Ret;

    pGetSystemCpuSetInformation = (FN_GetSystemCpuSetInformation)GetProcAddress(hKernel32, "GetSystemCpuSetInformation");
    if (!pGetSystemCpuSetInformation && is_reactos() && !GetProcAddress(hKernel32, "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pGetSystemCpuSetInformation != NULL, "GetSystemCpuSetInformation is not exported\n");
    if (!pGetSystemCpuSetInformation)
        return;

    ok(sizeof(SYSTEM_CPU_SET_INFORMATION) == ENTRY_SIZE, "sizeof = %lu\n", (ULONG)sizeof(SYSTEM_CPU_SET_INFORMATION));

    GetSystemInfo(&SystemInfo);
    Count = CountBits(SystemInfo.dwActiveProcessorMask);
    if (Count > ARRAYSIZE(Buffer) / ENTRY_SIZE)
    {
        skip("%lu processors\n", Count);
        return;
    }
    Length = Count * ENTRY_SIZE;

    /* The size comes back for a buffer that is too small, whatever the buffer is; it is left alone */
    TestFails("NULL, 0", NULL, 0, NULL, 0, ERROR_INSUFFICIENT_BUFFER, Length);
    memset(Buffer, 0xcc, 64);
    TestFails("8 bytes", Buffer, 8, NULL, 0, ERROR_INSUFFICIENT_BUFFER, Length);
    TestFails("one byte short", Buffer, Length - 1, NULL, 0, ERROR_INSUFFICIENT_BUFFER, Length);
    ok(Buffer[0] == 0xcc && Buffer[1] == 0xcc && Buffer[ENTRY_SIZE - 1] == 0xcc, "the buffer was written\n");

    /* A length that fits and no buffer */
    TestFails("NULL, fits", NULL, Length, NULL, 0, ERROR_NOACCESS, 0);

    /* Flags are reserved, the length is cleared first */
    TestFails("flags 1", Buffer, sizeof(Buffer), NULL, 1, ERROR_INVALID_PARAMETER, 0);
    TestFails("flags 0x80000000", Buffer, sizeof(Buffer), NULL, 0x80000000, ERROR_INVALID_PARAMETER, 0);

    /* The process: none, the current one, or a process handle that may be queried */
    TestSucceeds("no process", NULL, Length);
    TestSucceeds("current process", GetCurrentProcess(), Length);
    Handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        TestSucceeds("limited information", Handle, Length);
        CloseHandle(Handle);
    }
    Handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    if (Handle)
    {
        TestSucceeds("query information", Handle, Length);
        CloseHandle(Handle);
    }
    Handle = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        TestFails("no query right", Buffer, sizeof(Buffer), Handle, 0, ERROR_ACCESS_DENIED, 0);
        CloseHandle(Handle);
    }
    TestFails("bad handle", Buffer, sizeof(Buffer), (HANDLE)(ULONG_PTR)0x12345, 0, ERROR_INVALID_HANDLE, 0);
    TestFails("current thread", Buffer, sizeof(Buffer), GetCurrentThread(), 0, ERROR_INVALID_HANDLE, 0);
    Handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    TestFails("event", Buffer, sizeof(Buffer), Handle, 0, ERROR_INVALID_HANDLE, 0);
    CloseHandle(Handle);

    /* The entries: one per processor, nothing after them */
    memset(Buffer, 0xcc, sizeof(Buffer));
    Ret = pGetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)Buffer, sizeof(Buffer), &Length, NULL, 0);
    ok(Ret == TRUE, "Ret = %d\n", Ret);
    ok(Length == Count * ENTRY_SIZE, "Length = %lu\n", Length);
    ok(Buffer[Length] == 0xcc, "wrote past the entries\n");
    Seen = 0;
    for (Index = 0, Offset = 0; Offset + ENTRY_SIZE <= Length; Index++, Offset += ENTRY_SIZE)
    {
        Entry = (PSYSTEM_CPU_SET_INFORMATION)(Buffer + Offset);
        ok(Entry->Size == ENTRY_SIZE, "[%lu] Size = %lu\n", Index, Entry->Size);
        ok(Entry->Type == CpuSetInformation, "[%lu] Type = %d\n", Index, Entry->Type);
        ok(Entry->CpuSet.Id == 0x100 + Index, "[%lu] Id = 0x%lx\n", Index, Entry->CpuSet.Id);
        ok(Entry->CpuSet.Group == 0, "[%lu] Group = %u\n", Index, Entry->CpuSet.Group);
        ok(Entry->CpuSet.LogicalProcessorIndex == Index, "[%lu] LogicalProcessorIndex = %u\n",
           Index, Entry->CpuSet.LogicalProcessorIndex);
        ok(Entry->CpuSet.CoreIndex < Count, "[%lu] CoreIndex = %u\n", Index, Entry->CpuSet.CoreIndex);
        ok(Entry->CpuSet.NumaNodeIndex == 0, "[%lu] NumaNodeIndex = %u\n", Index, Entry->CpuSet.NumaNodeIndex);
        ok(Entry->CpuSet.LastLevelCacheIndex < Count, "[%lu] LastLevelCacheIndex = %u\n",
           Index, Entry->CpuSet.LastLevelCacheIndex);
        ok(Entry->CpuSet.EfficiencyClass == 0, "[%lu] EfficiencyClass = %u\n", Index, Entry->CpuSet.EfficiencyClass);
        ok((Entry->CpuSet.AllFlags & 0xf0) == 0, "[%lu] AllFlags = 0x%x\n", Index, Entry->CpuSet.AllFlags);
        ok(Entry->CpuSet.AllocationTag == 0, "[%lu] AllocationTag = %I64u\n", Index, Entry->CpuSet.AllocationTag);
        if (Entry->CpuSet.LogicalProcessorIndex < 64)
            Seen |= (ULONG_PTR)1 << Entry->CpuSet.LogicalProcessorIndex;
    }
    ok(Seen == SystemInfo.dwActiveProcessorMask, "processors seen 0x%Ix, active 0x%Ix\n",
       Seen, SystemInfo.dwActiveProcessorMask);

    /* A larger buffer gets the same, and the length that was used */
    Length = 0xdeadbeef;
    Ret = pGetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)Buffer, sizeof(Buffer), &Length, NULL, 0);
    ok(Ret == TRUE, "Ret = %d\n", Ret);
    ok(Length == Count * ENTRY_SIZE, "Length = %lu\n", Length);
}
