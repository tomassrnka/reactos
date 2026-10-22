/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetMaximumProcessorGroupCount and GetProcessGroupAffinity
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static WORD (WINAPI *pGetActiveProcessorGroupCount)(VOID);
static WORD (WINAPI *pGetMaximumProcessorGroupCount)(VOID);
static BOOL (WINAPI *pGetProcessGroupAffinity)(HANDLE, PUSHORT, PUSHORT);

START_TEST(ProcessorGroups)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    /* Windows fails the call with ERROR_NOACCESS when the array is not aligned to 4 bytes */
    ULONG ArrayStorage[16];
    USHORT *Array = (USHORT *)ArrayStorage;
    USHORT Count, Required;
    WORD Active, Maximum = 0;
    HANDLE hProcess;
    BOOL Ret;

    pGetActiveProcessorGroupCount = (void *)GetProcAddress(hKernel32, "GetActiveProcessorGroupCount");
    pGetMaximumProcessorGroupCount = (void *)GetProcAddress(hKernel32, "GetMaximumProcessorGroupCount");
    pGetProcessGroupAffinity = (void *)GetProcAddress(hKernel32, "GetProcessGroupAffinity");

    /* GetActiveProcessorGroupCount marks the Windows 7 level of the exports */
    if (!pGetActiveProcessorGroupCount)
    {
        skip("The Windows 7 exports are not enabled\n");
        return;
    }

    ok(pGetMaximumProcessorGroupCount != NULL, "GetMaximumProcessorGroupCount is missing\n");
    ok(pGetProcessGroupAffinity != NULL, "GetProcessGroupAffinity is missing\n");

    if (pGetActiveProcessorGroupCount && pGetMaximumProcessorGroupCount)
    {
        Active = pGetActiveProcessorGroupCount();
        Maximum = pGetMaximumProcessorGroupCount();
        ok(Active >= 1, "Active = %u\n", Active);
        ok(Maximum >= Active, "Maximum = %u, Active = %u\n", Maximum, Active);
    }

    if (!pGetProcessGroupAffinity)
        return;

    /* The buffer is too small: the required size is reported */
    Count = 0;
    SetLastError(0xdeadbeef);
    Ret = pGetProcessGroupAffinity(GetCurrentProcess(), &Count, Array);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetLastError() = %lu\n", GetLastError());
    ok(Count >= 1 && Count <= 8, "Count = %u\n", Count);
    if (Count < 1 || Count > 8)
        return;
    Required = Count;
    ok(Maximum == 0 || Required <= Maximum, "Required = %u, Maximum = %u\n", Required, Maximum);

    /* An exact buffer */
    Count = Required;
    Array[0] = 0xdead;
    SetLastError(0xdeadbeef);
    Ret = pGetProcessGroupAffinity(GetCurrentProcess(), &Count, Array);
    ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
    ok(Count == Required, "Count = %u, Required = %u\n", Count, Required);
    ok(Maximum == 0 || Array[0] < Maximum, "Array[0] = %u, Maximum = %u\n", Array[0], Maximum);

    /* A larger buffer: the count shrinks to what is used, the rest is untouched */
    Count = 8;
    Array[Required] = 0xdead;
    Ret = pGetProcessGroupAffinity(GetCurrentProcess(), &Count, Array);
    ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
    ok(Count == Required, "Count = %u, Required = %u\n", Count, Required);
    ok(Array[Required] == 0xdead, "Array[%u] = %u\n", Required, Array[Required]);

    /* An array that cannot be used (Windows crashes on a NULL count, which is not tested) */
    Count = Required;
    SetLastError(0xdeadbeef);
    Ret = pGetProcessGroupAffinity(GetCurrentProcess(), &Count, NULL);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_NOACCESS, "GetLastError() = %lu\n", GetLastError());
    Count = Required;
    SetLastError(0xdeadbeef);
    Ret = pGetProcessGroupAffinity(GetCurrentProcess(), &Count, (USHORT *)((char *)Array + 2));
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_NOACCESS, "GetLastError() = %lu\n", GetLastError());

    /* The handle needs query access */
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(hProcess != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (hProcess)
    {
        Count = Required;
        Array[0] = 0xdead;
        Ret = pGetProcessGroupAffinity(hProcess, &Count, Array);
        ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
        ok(Count == Required, "Count = %u, Required = %u\n", Count, Required);
        CloseHandle(hProcess);
    }

    hProcess = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    ok(hProcess != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (hProcess)
    {
        Count = 1;
        SetLastError(0xdeadbeef);
        Ret = pGetProcessGroupAffinity(hProcess, &Count, Array);
        ok(Ret == FALSE, "Ret = %d\n", Ret);
        ok(GetLastError() == ERROR_ACCESS_DENIED, "GetLastError() = %lu\n", GetLastError());
        CloseHandle(hProcess);
    }

    /* A bad handle */
    Count = 1;
    SetLastError(0xdeadbeef);
    Ret = pGetProcessGroupAffinity((HANDLE)(ULONG_PTR)0x1234, &Count, Array);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "GetLastError() = %lu\n", GetLastError());
}
